/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>

#include "mdw_cmn.h"
#include "mdw_queue.h"
#include "mdw_rsc.h"
#include "mdw_cmd.h"
#include "mdw_sched.h"
#include "mdw_pack.h"
#include "midware_trace.h"
#include "mnoc_api.h"
#include "reviser_export.h"
#include "mdw_tag.h"
#define CREATE_TRACE_POINTS
#include "mdw_events.h"

struct mdw_sched_mgr {
	struct task_struct *task;
	struct completion cmplt;

	struct list_head ds_list; //done sc list
	struct mutex mtx;

	bool pause;
	bool stop;
};

static struct mdw_cmd_parser *cmd_parser;
static struct mdw_sched_mgr ms_mgr;

#define MDW_EXEC_PRINT " pid(%d/%d) cmd(0x%llx/0x%llx-#%d/%u)"\
	" dev(%d/%s-#%d) mp(0x%x/%u/%u/0x%llx) sched(%d/%u/%u/%u/%u/%d)"\
	" mem(%lu/%d/0x%x/0x%x) boost(%u) time(%u/%u)"

static void mdw_sched_met_start(struct mdw_apu_sc *sc, struct mdw_dev_info *d)
{
	mdw_trace_begin("apusys_scheduler|dev: %d_%d, cmd_id: 0x%08llx",
		d->type,
		d->idx,
		sc->parent->kid);
}

static void mdw_sched_met_end(struct mdw_apu_sc *sc, struct mdw_dev_info *d,
	int ret)
{
	mdw_trace_end("apusys_scheduler|dev: %d_%d, cmd_id: 0x%08llx, ret:%d",
		d->type,
		d->idx,
		sc->parent->kid, ret);
}

static void mdw_sched_trace(struct mdw_apu_sc *sc,
	struct mdw_dev_info *d, struct apusys_cmd_hnd *h, int ret, int done)
{
	char state[16];

	/* prefix */
	memset(state, 0, sizeof(state));
	if (!done) {
		mdw_sched_met_start(sc, d);
		snprintf(state, sizeof(state)-1, "start :");
	} else {
		mdw_sched_met_end(sc, d, ret);
		if (ret)
			snprintf(state, sizeof(state)-1, "fail :");
		else
			snprintf(state, sizeof(state)-1, "done :");
	}

	/* if err, use mdw_drv_err */
	if (ret) {
		mdw_drv_err("%s"MDW_EXEC_PRINT" ret(%d)\n",
			state,
			sc->parent->pid,
			sc->parent->tgid,
			sc->parent->hdr->uid,
			sc->parent->kid,
			sc->idx,
			sc->parent->hdr->num_sc,
			d->type,
			d->name,
			d->idx,
			sc->hdr->pack_id,
			h->multicore_idx,
			sc->multi_total,
			sc->multi_bmp,
			sc->parent->hdr->priority,
			sc->parent->hdr->soft_limit,
			sc->parent->hdr->hard_limit,
			sc->hdr->ip_time,
			sc->hdr->suggest_time,
			0,//sc->par_cmd->power_save,
			sc->ctx,
			sc->hdr->tcm_force,
			sc->hdr->tcm_usage,
			sc->real_tcm_usage,
			h->boost_val,
			h->ip_time,
			sc->driver_time,
			ret);
	} else {
		mdw_drv_debug("%s"MDW_EXEC_PRINT" ret(%d)\n",
			state,
			sc->parent->pid,
			sc->parent->tgid,
			sc->parent->hdr->uid,
			sc->parent->kid,
			sc->idx,
			sc->parent->hdr->num_sc,
			d->type,
			d->name,
			d->idx,
			sc->hdr->pack_id,
			h->multicore_idx,
			sc->multi_total,
			sc->multi_bmp,
			sc->parent->hdr->priority,
			sc->parent->hdr->soft_limit,
			sc->parent->hdr->hard_limit,
			sc->hdr->ip_time,
			sc->hdr->suggest_time,
			0,//sc->par_cmd->power_save,
			sc->ctx,
			sc->hdr->tcm_force,
			sc->hdr->tcm_usage,
			sc->real_tcm_usage,
			h->boost_val,
			h->ip_time,
			sc->driver_time,
			ret);
	}

	/* trace cmd end */
	trace_mdw_cmd(done,
		sc->parent->pid,
		sc->parent->tgid,
		sc->parent->hdr->uid,
		sc->parent->kid,
		sc->idx,
		sc->parent->hdr->num_sc,
		d->type,
		d->name,
		d->idx,
		sc->hdr->pack_id,
		h->multicore_idx,
		sc->multi_total,
		sc->multi_bmp,
		sc->parent->hdr->priority,
		sc->parent->hdr->soft_limit,
		sc->parent->hdr->hard_limit,
		sc->hdr->ip_time,
		sc->hdr->suggest_time,
		0,//sc->par_cmd->power_save,
		sc->ctx,
		sc->hdr->tcm_force,
		sc->hdr->tcm_usage,
		sc->real_tcm_usage,
		h->boost_val,
		h->ip_time,
		ret);
}
#undef MDW_EXEC_PRINT

static int mdw_sched_sc_done(void)
{
	struct mdw_apu_cmd *c = NULL;
	struct mdw_apu_sc *sc = NULL, *s = NULL;
	int ret = 0;

	mdw_flw_debug("\n");

	/* get done sc from done sc list */
	mutex_lock(&ms_mgr.mtx);
	s = list_first_entry_or_null(&ms_mgr.ds_list,
		struct mdw_apu_sc, ds_item);
	if (s)
		list_del(&s->ds_item);
	mutex_unlock(&ms_mgr.mtx);
	if (!s)
		return -ENODATA;

	/* recv finished subcmd */
	while (1) {
		c = s->parent;
		ret = cmd_parser->end_sc(s, &sc);
		mdw_flw_debug("\n");
		/* check return value */
		if (ret) {
			mdw_drv_err("parse done sc fail(%d)\n", ret);
			complete(&c->cmplt);
			break;
		}
		/* check parsed sc */
		if (sc) {
		/*
		 * finished sc parse ok, should be call
		 * again because residual sc
		 */
			mdw_flw_debug("sc(0x%llx-#%d)\n",
				sc->parent->kid, sc->idx);
			mdw_sched(sc);
		} else {
		/* finished sc parse done, break loop */
			mdw_sched(NULL);
			break;
		}
	};

	return ret;
}

static void mdw_sched_enque_done_sc(struct kref *ref)
{
	struct mdw_apu_sc *sc =
		container_of(ref, struct mdw_apu_sc, multi_ref);

	mdw_flw_debug("sc(0x%llx-#%d)\n", sc->parent->kid, sc->idx);
	cmd_parser->put_ctx(sc);

	mutex_lock(&ms_mgr.mtx);
	list_add_tail(&sc->ds_item, &ms_mgr.ds_list);
	mdw_sched(NULL);
	mutex_unlock(&ms_mgr.mtx);
}

int mdw_sched_dev_routine(void *arg)
{
	int ret = 0;
	struct mdw_dev_info *d = (struct mdw_dev_info *)arg;
	struct apusys_cmd_hnd h;
	struct mdw_apu_sc *sc = NULL;

	/* execute */
	while (!kthread_should_stop() && d->stop == false) {
		mdw_flw_debug("\n");
		ret = wait_for_completion_interruptible(&d->cmplt);
		if (ret)
			goto next;

		sc = (struct mdw_apu_sc *)d->sc;
		if (!sc) {
			mdw_drv_err("no sc to exec\n");
			goto next;
		}

		/* get mem ctx */
		if (cmd_parser->get_ctx(sc)) {
			mdw_drv_err("cmd(0x%llx-#%d) get ctx fail\n",
				sc->parent->kid, sc->idx);
			goto next;
		}

		/* contruct cmd hnd */
		mdw_queue_deadline_boost(sc);
		cmd_parser->set_hnd(sc, &h);

		/*
		 * Execute reviser to switch VLM:
		 * Skip set context on preemptive command,
		 * context should be set by engine driver itself.
		 * Give engine a callback to set context id.
		 */
		if (d->type != APUSYS_DEVICE_MDLA &&
			d->type != APUSYS_DEVICE_MDLA_RT) {
			reviser_set_context(d->type,
					d->idx, sc->ctx);
		}

		/* count qos start */
		apu_cmd_qos_start(sc->parent->kid, sc->idx,
			sc->type, d->idx, h.boost_val);

		/* execute */
		mdw_sched_trace(sc, d, &h, ret, 0);
		getnstimeofday(&sc->ts_start);
		ret = d->dev->send_cmd(APUSYS_CMD_EXECUTE, &h, d->dev);
		getnstimeofday(&sc->ts_end);
		sc->driver_time = mdw_cmn_get_time_diff(&sc->ts_start,
			&sc->ts_end);
		mdw_sched_trace(sc, d, &h, ret, 1);

		/* count qos end */
		mutex_lock(&sc->mtx);
		sc->bw += apu_cmd_qos_end(sc->parent->kid, sc->idx,
			sc->type, d->idx);
		sc->ip_time = sc->ip_time > h.ip_time ? sc->ip_time : h.ip_time;
		sc->boost = h.boost_val;
		mdw_flw_debug("multi bmp(0x%llx)\n", sc->multi_bmp);
		mutex_unlock(&sc->mtx);

		/* put device */
		if (mdw_rsc_put_dev(d))
			mdw_drv_err("put dev(%d-#%d) fail\n",
				d->type, d->dev->idx);

		mdw_flw_debug("sc(0x%llx-#%d) ref(%d)\n", sc->parent->kid,
			sc->idx, kref_read(&sc->multi_ref));
		kref_put(&sc->multi_ref, mdw_sched_enque_done_sc);
next:
		mdw_flw_debug("done\n");
		continue;
	}

	return 0;
}

static int mdw_sched_get_type(uint64_t bmp)
{
	return find_last_bit((unsigned long *)&bmp, APUSYS_DEVICE_MAX);
}

int mdw_sched_dispatch_pack(struct mdw_apu_sc *sc)
{
	return mdw_pack_dispatch(sc);
}

static int mdw_sched_dispatch_norm(struct mdw_apu_sc *sc)
{
	struct mdw_rsc_req r;
	struct mdw_dev_info *d = NULL;
	struct list_head *tmp = NULL, *list_ptr = NULL;
	int ret = 0, dev_num = 0, exec_num = 0;

	dev_num =  mdw_rsc_get_dev_num(sc->type);

	memset(&r, 0, sizeof(r));
	exec_num = cmd_parser->exec_core_num(sc);
	exec_num = exec_num < dev_num ? exec_num : dev_num;
	r.num[sc->type] = exec_num;
	r.total_num = exec_num;
	r.acq_bmp |= (1ULL << sc->type);
	r.mode = MDW_DEV_INFO_GET_MODE_TRY;
	if (cmd_parser->is_deadline(sc))
		r.policy = MDW_DEV_INFO_GET_POLICY_RR;
	else
		r.policy = MDW_DEV_INFO_GET_POLICY_SEQ;

	ret = mdw_rsc_get_dev(&r);
	if (ret)
		goto out;

	mutex_lock(&sc->mtx);
	sc->multi_total = r.get_num[sc->type];
	refcount_set(&sc->multi_ref.refcount, r.get_num[sc->type]);

	/* power on each device if multicore */
	if (r.get_num[sc->type] > 1) {
		list_for_each_safe(list_ptr, tmp, &r.d_list) {
			d = list_entry(list_ptr, struct mdw_dev_info, r_item);
			d->pwr_on(d, sc->boost, MDW_RSC_SET_PWR_TIMEOUT);
			sc->multi_idx = d->idx;
		}
	}
	mutex_unlock(&sc->mtx);
	mdw_flw_debug("sc(0x%llx-#%d) #dev(%u/%u) ref(%d)\n",
		sc->parent->kid, sc->idx, r.get_num[sc->type],
		r.num[sc->type], kref_read(&sc->multi_ref));

	/* dispatch cmd */
	list_for_each_safe(list_ptr, tmp, &r.d_list) {
		d = list_entry(list_ptr, struct mdw_dev_info, r_item);
		ret = d->exec(d, sc);
		if (ret)
			goto fail_exec_sc;
		list_del(&d->r_item);
	}

	goto out;

fail_exec_sc:
	list_for_each_safe(list_ptr, tmp, &r.d_list) {
		d = list_entry(list_ptr, struct mdw_dev_info, r_item);
		list_del(&d->r_item);
		mdw_rsc_put_dev(d);
	}
out:
	return ret;
}

static struct mdw_apu_sc *mdw_sched_pop_sc(int type)
{
	struct mdw_queue *mq = NULL;
	struct mdw_apu_sc *sc = NULL;

	/* get queue */
	mq = mdw_rsc_get_queue(type);
	if (!mq)
		return NULL;

	/* get sc */
	if (mq->deadline.ops.len(&mq->deadline))
		sc = mq->deadline.ops.pop(&mq->deadline);
	else
		sc = mq->norm.ops.pop(&mq->norm);

	if (sc) {
		getnstimeofday(&sc->ts_deque);
		mdw_flw_debug("pop sc(0x%llx-#%d/%d/%llu)\n",
			sc->parent->kid, sc->idx, sc->type, sc->period);
	}
	return sc;
}

static int mdw_sched_insert_sc(struct mdw_apu_sc *sc, int type)
{
	struct mdw_queue *mq = NULL;
	int ret = 0;

	/* get queue */
	mq = mdw_rsc_get_queue(sc->type);
	if (!mq) {
		mdw_drv_err("invalid sc(%d) type\n", sc->type);
		ret = -ENODEV;
		goto out;
	}

	mdw_flw_debug("insert sc(0x%llx-#%d/%d/%llu)\n",
		sc->parent->kid, sc->idx, sc->type, sc->period);

	if (cmd_parser->is_deadline(sc))
		ret = mq->deadline.ops.insert(sc, &mq->deadline,
			type);
	else
		ret = mq->norm.ops.insert(sc, &mq->norm, type);

	getnstimeofday(&sc->ts_enque);

out:
	return ret;
}

static int mdw_sched_routine(void *arg)
{
	int ret = 0;
	struct mdw_apu_sc *sc = NULL;
	uint64_t bmp = 0;
	int t = 0;

	mdw_flw_debug("\n");

	while (!kthread_should_stop() && !ms_mgr.stop) {
		ret = wait_for_completion_interruptible(&ms_mgr.cmplt);
		if (ret)
			mdw_drv_warn("sched ret(%d)\n", ret);

		if (ms_mgr.pause == true)
			continue;

		if (!mdw_sched_sc_done()) {
			mdw_sched(NULL);
			goto next;
		}

		mdw_pack_check();

		bmp = mdw_rsc_get_avl_bmp();
		t = mdw_sched_get_type(bmp);
		if (t < 0 || t >= APUSYS_DEVICE_MAX) {
			mdw_flw_debug("nothing to sched(%d)\n", t);
			goto next;
		}

		/* get queue */
		sc = mdw_sched_pop_sc(t);
		if (!sc) {
			mdw_drv_err("pop sc(%d) fail\n", t);
			goto fail_pop_sc;
		}

		/* dispatch cmd */
		if (sc->hdr->pack_id)
			ret = mdw_sched_dispatch_pack(sc);
		else
			ret = mdw_sched_dispatch_norm(sc);

		if (ret) {
			mdw_flw_debug("sc(0x%llx-#%d) dispatch fail",
				sc->parent->kid, sc->idx);
			goto fail_exec_sc;
		}

		goto next;

fail_exec_sc:
	if (mdw_sched_insert_sc(sc, MDW_QUEUE_INSERT_FRONT)) {
		mdw_drv_err("sc(0x%llx-#%d) insert fail\n",
			sc->parent->kid, sc->idx);
	}
fail_pop_sc:
next:
		mdw_flw_debug("\n");
		if (mdw_rsc_get_avl_bmp())
			mdw_sched(NULL);
	}

	mdw_drv_warn("schedule thread end\n");

	return 0;
}

int mdw_sched(struct mdw_apu_sc *sc)
{
	int ret = 0;

	/* no input sc, trigger sched thread only */
	if (!sc) {
		complete(&ms_mgr.cmplt);
		return 0;
	}

	/* insert sc to queue */
	ret = mdw_sched_insert_sc(sc, MDW_QUEUE_INSERT_NORM);
	if (ret) {
		mdw_drv_err("sc(0x%llx-#%d) enque fail\n",
			sc->parent->kid, sc->idx);
		return ret;
	}

	complete(&ms_mgr.cmplt);

	mdw_flw_debug("\n");

	return 0;
}

void mdw_sched_pause(void)
{
	struct mdw_dev_info *d = NULL;
	int type = 0, idx = 0, ret = 0, i = 0;

	if (ms_mgr.pause == true) {
		mdw_drv_warn("pause ready\n");
		return;
	}

	ms_mgr.pause = true;

	for (type = 0; type < APUSYS_DEVICE_MAX; type++) {
		for (idx = 0; idx < mdw_rsc_get_dev_num(type); idx++) {
			d = mdw_rsc_get_dinfo(type, idx);
			if (!d)
				continue;
			ret = d->suspend(d);
			if (ret) {
				mdw_drv_err("dev(%s%d) suspend fail(%d)\n",
					d->name, d->idx, ret);
				goto fail_sched_pause;
			}
		}
	}

	mdw_drv_info("pause\n");
	return;

fail_sched_pause:
	for (i = 0; i <= type; i++) {
		for (idx = 0; idx < mdw_rsc_get_dev_num(type); idx++) {
			d = mdw_rsc_get_dinfo(type, idx);
			if (!d)
				continue;
			ret = d->resume(d);
			if (ret) {
				mdw_drv_err("dev(%s%d) resume fail(%d)\n",
					d->name, d->idx, ret);
				goto fail_sched_pause;
			}
		}
	}
	ms_mgr.pause = false;
	mdw_drv_warn("resume\n");
}

void mdw_sched_restart(void)
{
	struct mdw_dev_info *d = NULL;
	int type = 0, idx = 0, ret = 0;

	if (ms_mgr.pause == false)
		mdw_drv_warn("resume ready\n");
	else
		mdw_drv_info("resume\n");

	for (type = 0; type < APUSYS_DEVICE_MAX; type++) {
		for (idx = 0; idx < mdw_rsc_get_dev_num(type); idx++) {
			d = mdw_rsc_get_dinfo(type, idx);
			if (!d)
				continue;
			ret = d->resume(d);
			if (ret)
				mdw_drv_err("dev(%s%d) resume fail(%d)\n",
				d->name, d->idx, ret);
		}
	}

	ms_mgr.pause = false;
	mdw_sched(NULL);
}

void mdw_sched_set_thd_group(void)
{
	struct file *fd;
	char buf[8];
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());

	fd = filp_open(APUSYS_THD_TASK_FILE_PATH, O_WRONLY, 0);
	if (IS_ERR(fd)) {
		mdw_drv_debug("don't support low latency group\n");
		goto out;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf)-1, "%d", ms_mgr.task->pid);
	vfs_write(fd, (__force const char __user *)buf,
		sizeof(buf), &fd->f_pos);
	mdw_drv_debug("setup worker(%d/%s) to group\n",
		ms_mgr.task->pid, buf);

	filp_close(fd, NULL);
out:
	set_fs(oldfs);
}

int mdw_sched_init(void)
{
	memset(&ms_mgr, 0, sizeof(ms_mgr));
	ms_mgr.pause = false;
	ms_mgr.stop = false;
	init_completion(&ms_mgr.cmplt);
	INIT_LIST_HEAD(&ms_mgr.ds_list);
	mutex_init(&ms_mgr.mtx);

	cmd_parser = mdw_cmd_get_parser();
	if (!cmd_parser)
		return -ENODEV;

	ms_mgr.task = kthread_run(mdw_sched_routine,
		NULL, "apusys_sched");
	if (!ms_mgr.task) {
		mdw_drv_err("create kthread(sched) fail\n");
		return -ENOMEM;
	}

	mdw_pack_init();

	return 0;
}

void mdw_sched_exit(void)
{
	ms_mgr.stop = true;
	mdw_sched(NULL);
	mdw_pack_exit();
}