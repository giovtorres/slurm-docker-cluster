/*****************************************************************************\
 *  node_scheduler.c - select and allocated nodes to jobs
 *	Note: there is a global node table (node_record_table_ptr)
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Copyright (C) SchedMD LLC.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/group_cache.h"
#include "src/common/hostlist.h"
#include "src/common/id_util.h"
#include "src/common/job_features.h"
#include "src/common/list.h"
#include "src/common/node_features.h"
#include "src/common/port_mgr.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/accounting_storage.h"
#include "src/interfaces/burst_buffer.h"
#include "src/interfaces/gres.h"
#include "src/interfaces/mcs.h"
#include "src/interfaces/node_features.h"
#include "src/interfaces/preempt.h"
#include "src/interfaces/priority.h"
#include "src/interfaces/select.h"
#include "src/interfaces/switch.h"
#include "src/interfaces/topology.h"

#include "src/slurmctld/acct_policy.h"
#include "src/slurmctld/agent.h"
#include "src/slurmctld/front_end.h"
#include "src/slurmctld/gang.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/power_save.h"
#include "src/slurmctld/proc_req.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"

#include "src/stepmgr/gres_stepmgr.h"
#include "src/stepmgr/stepmgr.h"

#define _DEBUG	0
#define MAX_FEATURES  64	/* max exclusive features "[fs1|fs2]"=2 */

struct node_set {		/* set of nodes with same configuration */
	uint16_t cpus_per_node;	/* NOTE: This is the minimum count */
	char     *features;		/* Node features */
	bitstr_t *feature_bits;		/* MORed feature's position */
	uint32_t flags;			/* See NODE_SET_* below */
	bitstr_t *my_bitmap;		/* Node bitmap */
	uint32_t node_cnt;		/* Node count */
	uint32_t node_weight;		/* Node weight */
	uint64_t real_memory;		/* Real memory on node */
	uint64_t sched_weight;		/* Scheduling weight, based upon
					 * node_weight and flags */
};

#define NODE_SET_NOFLAG		0x00
#define NODE_SET_REBOOT		0x01
#define NODE_SET_OUTSIDE_FLEX	0x02
#define NODE_SET_POWER_DN	0x04

enum {
	IN_FL,		/* Inside flex reservation */
	OUT_FL,		/* Outside flex reservation */
	IN_FL_RE,	/* Inside flex reservation + need reboot */
	OUT_FL_NO_RE,	/* Outside flex reservation + NO to need reboot */
	OUT_FL_RE,	/* Outside flex reservation + need reboot */
	REBOOT,		/* Needs reboot */
	NM_TYPES	/* Number of node types */
};

static int  _build_node_list(job_record_t *job_ptr,
			     struct node_set **node_set_pptr,
			     int *node_set_size, char **err_msg,
			     bool test_only, bool can_reboot);
static bitstr_t *_find_grp_node_bitmap(job_record_t *job_ptr);
static bool _first_array_task(job_record_t *job_ptr);
static void _log_node_set(job_record_t *job_ptr,
			  struct node_set *node_set_ptr,
			  int node_set_size);
static int _match_feature(list_t *feature_list, bitstr_t **inactive_bitmap);
static int _nodes_in_sets(bitstr_t *req_bitmap,
			  struct node_set * node_set_ptr,
			  int node_set_size);
static int _pick_best_nodes(struct node_set *node_set_ptr,
			    int node_set_size, bitstr_t ** select_bitmap,
			    job_record_t *job_ptr, part_record_t *part_ptr,
			    uint32_t min_nodes, uint32_t max_nodes,
			    uint32_t req_nodes, bool test_only,
			    list_t *preemptee_candidates,
			    list_t **preemptee_job_list, bool has_xand,
			    resv_exc_t *resv_exc_ptr, bool resv_overlap);
static void _set_err_msg(bool cpus_ok, bool mem_ok, bool disk_ok,
			 bool job_mc_ok, char **err_msg);
static void _set_sched_weight(struct node_set *node_set_ptr);
static int _sort_node_set(const void *x, const void *y);
static bitstr_t *_valid_features(job_record_t *job_ptr,
				 config_record_t *config_ptr,
				 bool can_reboot, bitstr_t *reboot_bitmap);

/*
 * _get_ntasks_per_core - Retrieve the value of ntasks_per_core from
 *	the given job_details record.  If it wasn't set, return INFINITE16.
 *	Intended for use with the adjust_cpus_nppcu function.
 */
static uint16_t _get_ntasks_per_core(job_details_t *details)
{
	if (details->mc_ptr)
		return details->mc_ptr->ntasks_per_core;
	else
		return INFINITE16;
}

/*
 * allocate_nodes - change state of specified nodes to NODE_STATE_ALLOCATED
 *	also claim required licenses and resources reserved by accounting
 *	policy association
 * IN job_ptr - job being allocated resources
 */
extern void allocate_nodes(job_record_t *job_ptr)
{
	node_record_t *node_ptr;

	for (int i = 0; (node_ptr = next_node_bitmap(job_ptr->node_bitmap, &i));
	     i++) {
		make_node_alloc(node_ptr, job_ptr);
	}

	node_mgr_make_node_blocked(job_ptr, true);

	last_node_update = time(NULL);
	license_job_get(job_ptr, false);
	set_initial_job_alias_list(job_ptr);
}

extern void set_initial_job_alias_list(job_record_t *job_ptr)
{
	node_record_t *node_ptr;
	bool has_cloud = false, has_cloud_power_save = false;
	bool has_dynamic_norm = false;

	for (int i = 0; (node_ptr = next_node_bitmap(job_ptr->node_bitmap, &i));
	     i++) {
		if (IS_NODE_DYNAMIC_FUTURE(node_ptr))
			has_cloud = true;

		if (IS_NODE_DYNAMIC_NORM(node_ptr)) {
			/* Must set alias list as nodes won't exist in conf */
			has_cloud = true;
			has_dynamic_norm = true;
		}

		if (IS_NODE_CLOUD(node_ptr)) {
			has_cloud = true;
			if (IS_NODE_POWERED_DOWN(node_ptr) ||
			    IS_NODE_POWERING_UP(node_ptr))
				has_cloud_power_save = true;
		}
	}

	if (has_cloud) {
		if (has_cloud_power_save &&
		    job_ptr->origin_cluster &&
		    xstrcmp(slurm_conf.cluster_name, job_ptr->origin_cluster)) {
			/* Set TBD so remote srun will updated node_addrs */
			job_ptr->alias_list = xstrdup("TBD");
			job_ptr->wait_all_nodes = 1;
		} else if (cloud_dns && !has_dynamic_norm) {
			job_ptr->wait_all_nodes = 1;
		} else if (has_cloud_power_save) {
			job_ptr->alias_list = xstrdup("TBD");
			job_ptr->wait_all_nodes = 1;
		} else
			set_job_alias_list(job_ptr);
	} else {
		/* set addrs if the job is coming from a different cluster */
		set_job_node_addrs(job_ptr, job_ptr->origin_cluster);
	}
}

/*
 * Set addrs if:
 * 1. There is an alias_list (cloud/dynamic nodes) and it isn't TBD (nodes are
 *    powering up).
 * 2. No alias_list but job/request is from a different cluster.
 */
extern void set_job_node_addrs(job_record_t *job_ptr,
			       const char *origin_cluster)
{
	if (!job_ptr->node_addrs &&
	    job_ptr->node_bitmap &&
	    bit_set_count(job_ptr->node_bitmap) &&
	    ((!job_ptr->alias_list && /* remote job */
	      origin_cluster &&
	      xstrcmp(origin_cluster, slurm_conf.cluster_name)) ||
	     (job_ptr->alias_list && xstrcmp(job_ptr->alias_list, "TBD")))) {
		node_record_t *node_ptr;

		job_ptr->node_addrs =
			xcalloc(bit_set_count(job_ptr->node_bitmap),
				sizeof(slurm_addr_t));
		for (int i = 0, addr_index = 0;
		     (node_ptr = next_node_bitmap(job_ptr->node_bitmap,
						  &i));
		     i++) {
			slurm_conf_get_addr(node_ptr->name,
					    &job_ptr->node_addrs[addr_index++],
					    0);
		}
	}
}

/* Set a job's alias_list string */
extern void set_job_alias_list(job_record_t *job_ptr)
{
	node_record_t *node_ptr;

	xfree(job_ptr->alias_list);

	if (cloud_dns && bit_super_set(job_ptr->node_bitmap, cloud_node_bitmap))
		return;

	for (int i = 0; (node_ptr = next_node_bitmap(job_ptr->node_bitmap, &i));
	     i++) {
		if (IS_NODE_DYNAMIC_FUTURE(node_ptr) ||
		    IS_NODE_DYNAMIC_NORM(node_ptr) ||
		    (!cloud_dns && IS_NODE_CLOUD(node_ptr))) {
			if (job_ptr->alias_list)
				xstrcat(job_ptr->alias_list, ",");

			xstrfmtcat(job_ptr->alias_list, "%s:[%s]:%s",
				   node_ptr->name, node_ptr->comm_name,
				   node_ptr->node_hostname);
		}
	}

	set_job_node_addrs(job_ptr, job_ptr->origin_cluster);
}

extern void set_job_features_use(job_details_t *details_ptr)
{
	if (!details_ptr)
		return;

	if (details_ptr->prefer) {
		details_ptr->features_use = details_ptr->prefer;
		details_ptr->feature_list_use = details_ptr->prefer_list;
	} else {
		details_ptr->features_use = details_ptr->features;
		details_ptr->feature_list_use = details_ptr->feature_list;
	}
}

/*
 * deallocate_nodes - for a given job, deallocate its nodes and make
 *	their state NODE_STATE_COMPLETING also release the job's licenses
 *	and resources reserved by accounting policy association
 * IN job_ptr - pointer to terminating job (already in some COMPLETING state)
 * IN timeout - true if job exhausted time limit, send REQUEST_KILL_TIMELIMIT
 *	RPC instead of REQUEST_TERMINATE_JOB
 * IN suspended - true if job was already suspended (node's run_job_cnt
 *	already decremented);
 * IN preempted - true if job is being preempted
 */
extern void deallocate_nodes(job_record_t *job_ptr, bool timeout,
			     bool suspended, bool preempted)
{
	kill_job_msg_t *kill_job = NULL;
	agent_arg_t *agent_args = NULL;
	node_record_t *node_ptr;
	hostlist_t *hostlist = NULL;
	uint16_t use_protocol_version = 0;
	uint16_t msg_flags = 0;
#ifdef HAVE_FRONT_END
	front_end_record_t *front_end_ptr;
#endif

	xassert(job_ptr);
	xassert(job_ptr->details);

	log_flag(TRACE_JOBS, "%s: %pJ", __func__, job_ptr);

	acct_policy_job_fini(job_ptr, false);

	node_mgr_make_node_blocked(job_ptr, false);

	if (select_g_job_fini(job_ptr) != SLURM_SUCCESS)
		error("select_g_job_fini(%pJ): %m", job_ptr);

	/* Release any job-related switch data */
	switch_g_job_complete(job_ptr);

	epilog_slurmctld(job_ptr);

	if (!job_ptr->details->prolog_running)
		hostlist = hostlist_create(NULL);

#ifdef HAVE_FRONT_END
	if (job_ptr->batch_host &&
	    (front_end_ptr = job_ptr->front_end_ptr)) {
		use_protocol_version = front_end_ptr->protocol_version;
		if (IS_NODE_DOWN(front_end_ptr)) {
			/* Issue the KILL RPC, but don't verify response */
			front_end_ptr->job_cnt_comp = 0;
			front_end_ptr->job_cnt_run  = 0;
			if (job_ptr->node_bitmap_cg) {
				bit_clear_all(job_ptr->node_bitmap_cg);
			} else {
				error("deallocate_nodes: node_bitmap_cg is "
				      "not set");
				/* Create empty node_bitmap_cg */
				job_ptr->node_bitmap_cg =
					bit_alloc(node_record_count);
			}
			job_ptr->cpu_cnt  = 0;
			job_ptr->node_cnt = 0;
		} else {
			bool set_fe_comp = false;
			if (front_end_ptr->job_cnt_run) {
				front_end_ptr->job_cnt_run--;
			} else {
				error("%s: front_end %s job_cnt_run underflow",
				      __func__, front_end_ptr->name);
			}
			if (front_end_ptr->job_cnt_run == 0) {
				uint32_t state_flags;
				state_flags = front_end_ptr->node_state &
					      NODE_STATE_FLAGS;
				front_end_ptr->node_state = NODE_STATE_IDLE |
							    state_flags;
			}
			for (int i = 0; (node_ptr = next_node_bitmap(
					     job_ptr->node_bitmap, &i));
			     i++) {
				make_node_comp(node_ptr, job_ptr, suspended);
				set_fe_comp = true;
			}
			if (set_fe_comp) {
				front_end_ptr->job_cnt_comp++;
				front_end_ptr->node_state |=
					NODE_STATE_COMPLETING;
			}
		}

		if (hostlist)
			hostlist_push_host(hostlist, job_ptr->batch_host);
	}
#else
	if (!job_ptr->node_bitmap_cg)
		build_cg_bitmap(job_ptr);
	use_protocol_version = SLURM_PROTOCOL_VERSION;

	for (int i = 0;
	     (node_ptr = next_node_bitmap(job_ptr->node_bitmap_cg, &i)); i++) {
		/* Sync up conditionals with make_node_comp() */
		if (IS_NODE_DOWN(node_ptr) ||
		    IS_NODE_POWERED_DOWN(node_ptr) ||
		    IS_NODE_POWERING_UP(node_ptr)) {
			/* Issue the KILL RPC, but don't verify response */
			bit_clear(job_ptr->node_bitmap_cg, i);
			job_update_tres_cnt(job_ptr, i);
			/*
			 * node_cnt indicates how many nodes we are waiting
			 * to get epilog complete messages from, so do not
			 * count down nodes. NOTE: The job's node_cnt will not
			 * match the number of entries in the node string
			 * during its completion.
			 */
			job_ptr->node_cnt--;
		}
		make_node_comp(node_ptr, job_ptr, suspended);

		if (hostlist &&
		    !IS_NODE_POWERED_DOWN(node_ptr) &&
		    !IS_NODE_POWERING_UP(node_ptr)) {
			hostlist_push_host(hostlist, node_ptr->name);
			if (use_protocol_version > node_ptr->protocol_version) {
				use_protocol_version =
					node_ptr->protocol_version;
				debug3("%s: protocol version downgraded to %u from node %s",
				       __func__, use_protocol_version,
				       node_ptr->name);
			}
			if (PACK_FANOUT_ADDRS(node_ptr))
				msg_flags |= SLURM_PACK_ADDRS;
		}
	}
#endif
	if (job_ptr->details->prolog_running) {
		/*
		 * Job was configuring when it was cancelled and epilog wasn't
		 * run on the nodes, so cleanup the nodes now. Final cleanup
		 * will happen after EpilogSlurmctld is done.
		 */
		if (job_ptr->node_bitmap_cg) {
			/*
			 * Call cleanup_completing before job_epilog_complete or
			 * we will end up requeuing there before this is called.
			 */
			if ((job_ptr->node_cnt == 0) &&
			    !job_ptr->epilog_running)
				cleanup_completing(job_ptr);

			/*
			 * job_epilog_complete() can free
			 * job_ptr->node_bitmap_cg
			 */
			for (int i = 0;
			     job_ptr->node_bitmap_cg &&
				     (node_ptr = next_node_bitmap(
					     job_ptr->node_bitmap_cg, &i));
			     i++) {
				job_epilog_complete(job_ptr->job_id,
						    node_ptr->name, 0);
			}
		}

		return;
	}

	if (!job_ptr->node_cnt) {
		/* Can not wait for epilog complete to release licenses and
		 * update gang scheduling table */
		cleanup_completing(job_ptr);
	}

	resv_replace_update(job_ptr);

	if (!hostlist || !hostlist_count(hostlist)) {
		hostlist_destroy(hostlist);
		return;
	}

	agent_args = xmalloc(sizeof(agent_arg_t));
	if (timeout)
		agent_args->msg_type = REQUEST_KILL_TIMELIMIT;
	else if (preempted)
		agent_args->msg_type = REQUEST_KILL_PREEMPTED;
	else
		agent_args->msg_type = REQUEST_TERMINATE_JOB;
	agent_args->retry = 0;	/* re_kill_job() resends as needed */
	agent_args->protocol_version = use_protocol_version;
	agent_args->hostlist = hostlist;
	agent_args->node_count = hostlist_count(hostlist);
	agent_args->msg_flags = msg_flags;

	last_node_update = time(NULL);
	kill_job = create_kill_job_msg(job_ptr, use_protocol_version);
	kill_job->nodes = xstrdup(job_ptr->nodes);

	agent_args->msg_args = kill_job;
	set_agent_arg_r_uid(agent_args, SLURM_AUTH_UID_ANY);
	agent_queue_request(agent_args);
}

static void _log_feature_nodes(job_feature_t  *job_feat_ptr)
{
	char *tmp1, *tmp2, *tmp3, *tmp4 = NULL;

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_NODE_FEATURES))
		return;

	if (job_feat_ptr->op_code == FEATURE_OP_OR)
		tmp3 = "OR";
	else if (job_feat_ptr->op_code == FEATURE_OP_AND)
		tmp3 = "AND";
	else if (job_feat_ptr->op_code == FEATURE_OP_MOR)
		tmp3 = "MOR";
	else if (job_feat_ptr->op_code == FEATURE_OP_XAND)
		tmp3 = "XAND";
	else if (job_feat_ptr->op_code == FEATURE_OP_END)
		tmp3 = "END";
	else {
		xstrfmtcat(tmp4, "UNKNOWN:%u", job_feat_ptr->op_code);
		tmp3 = tmp4;
	}
	tmp1 = bitmap2node_name(job_feat_ptr->node_bitmap_active);
	tmp2 = bitmap2node_name(job_feat_ptr->node_bitmap_avail);
	log_flag(NODE_FEATURES, "%s: FEAT:%s COUNT:%u BRACKET:%u PAREN:%d OP:%s ACTIVE:%s AVAIL:%s",
	     __func__, job_feat_ptr->name, job_feat_ptr->count,
	     job_feat_ptr->bracket, job_feat_ptr->paren, tmp3, tmp1, tmp2);
	xfree(tmp1);
	xfree(tmp2);
	xfree(tmp4);
}

/*
 * For every element in the feature_list, identify the nodes with that feature
 * either active or available and set the feature_list's node_bitmap_active and
 * node_bitmap_avail fields accordingly.
 */
extern void find_feature_nodes(list_t *feature_list, bool can_reboot)
{
	list_itr_t *feat_iter;
	job_feature_t  *job_feat_ptr;
	node_feature_t *node_feat_ptr;

	if (!feature_list)
		return;
	feat_iter = list_iterator_create(feature_list);
	while ((job_feat_ptr = list_next(feat_iter))) {
		FREE_NULL_BITMAP(job_feat_ptr->node_bitmap_active);
		FREE_NULL_BITMAP(job_feat_ptr->node_bitmap_avail);
		node_feat_ptr = list_find_first(active_feature_list,
						list_find_feature,
						job_feat_ptr->name);
		if (node_feat_ptr && node_feat_ptr->node_bitmap) {
			job_feat_ptr->node_bitmap_active =
				bit_copy(node_feat_ptr->node_bitmap);
		} else {	/* This feature not active */
			job_feat_ptr->node_bitmap_active =
				bit_alloc(node_record_count);
		}
		if (can_reboot && job_feat_ptr->changeable) {
			node_feat_ptr = list_find_first(avail_feature_list,
							list_find_feature,
							job_feat_ptr->name);
			if (node_feat_ptr && node_feat_ptr->node_bitmap) {
				job_feat_ptr->node_bitmap_avail =
					bit_copy(node_feat_ptr->node_bitmap);
			} else {   /* This feature not available */
				job_feat_ptr->node_bitmap_avail =
					bit_alloc(node_record_count);
			}
		} else if (job_feat_ptr->node_bitmap_active) {
			job_feat_ptr->node_bitmap_avail =
				bit_copy(job_feat_ptr->node_bitmap_active);
		}

		_log_feature_nodes(job_feat_ptr);
	}
	list_iterator_destroy(feat_iter);
}

/*
 * _match_feature - determine which of the job features are now inactive
 * IN feature_list - Job's feature request list
 * OUT inactive_bitmap - Nodes with this as inactive feature
 * RET 1 if some nodes with this inactive feature, 0 no inactive feature
 * NOTE: Currently fully supports only AND/OR of features, not XAND/MOR
 */
static int _match_feature(list_t *feature_list, bitstr_t **inactive_bitmap)
{
	list_itr_t *job_feat_iter;
	job_feature_t *job_feat_ptr;
	int last_op = FEATURE_OP_AND, last_paren_op = FEATURE_OP_AND;
	int i, last_paren_cnt = 0;
	bitstr_t *feature_bitmap, *paren_bitmap = NULL, *work_bitmap;

	xassert(inactive_bitmap);

	if (!feature_list ||			/* nothing to look for */
	    (node_features_g_count() == 0))	/* No inactive features */
		return 0;

	feature_bitmap = node_conf_get_active_bitmap();
	work_bitmap = feature_bitmap;
	job_feat_iter = list_iterator_create(feature_list);
	while ((job_feat_ptr = list_next(job_feat_iter))) {
		if (last_paren_cnt < job_feat_ptr->paren) {
			/* Start of expression in parenthesis */
			last_paren_op = last_op;
			last_op = FEATURE_OP_AND;
			FREE_NULL_BITMAP(paren_bitmap);
			paren_bitmap = node_conf_get_active_bitmap();
			work_bitmap = paren_bitmap;
		}

		if (job_feat_ptr->node_bitmap_avail) {
			if (last_op == FEATURE_OP_AND) {
				bit_and(work_bitmap,
					job_feat_ptr->node_bitmap_active);
			} else if (last_op == FEATURE_OP_OR) {
				bit_or(work_bitmap,
				       job_feat_ptr->node_bitmap_active);
			} else {	/* FEATURE_OP_MOR or FEATURE_OP_XAND */
				bit_and(work_bitmap,
				        job_feat_ptr->node_bitmap_active);
			}
		} else {	/* feature not found */
			if (last_op == FEATURE_OP_AND) {
				bit_clear_all(work_bitmap);
			}
		}

		if (last_paren_cnt > job_feat_ptr->paren) {
			/* End of expression in parenthesis */
			if (last_paren_op == FEATURE_OP_AND) {
				bit_and(feature_bitmap, work_bitmap);
			} else if (last_paren_op == FEATURE_OP_OR) {
				bit_or(feature_bitmap, work_bitmap);
			} else {	/* FEATURE_OP_MOR or FEATURE_OP_XAND */
				bit_and(feature_bitmap, work_bitmap);
			}
			work_bitmap = feature_bitmap;
		}

		last_op = job_feat_ptr->op_code;
		last_paren_cnt = job_feat_ptr->paren;
	}
	list_iterator_destroy(job_feat_iter);
#if 0
{
	char tmp[32];
	bit_fmt(tmp, sizeof(tmp), work_bitmap);
	info("%s: NODE_BITMAP:%s", __func__, tmp);
}
#endif
	FREE_NULL_BITMAP(paren_bitmap);
	i = bit_ffc(feature_bitmap);
	if (i == -1) {	/* No required node features inactive */
		FREE_NULL_BITMAP(feature_bitmap);
		return 0;
	}
	bit_not(feature_bitmap);
	*inactive_bitmap = feature_bitmap;
	return 1;
}

/*
 * For a given job, if the available nodes differ from those with currently
 *	active features, return a bitmap of nodes with the job's required
 *	features currently active
 * IN job_ptr - job requesting resource allocation
 * IN avail_bitmap - nodes currently available for this job
 * OUT active_bitmap - nodes with job's features currently active, NULL if
 *	identical to avail_bitmap
 * NOTE: Currently fully supports only AND/OR of features, not XAND/MOR
 */
extern void build_active_feature_bitmap(job_record_t *job_ptr,
					bitstr_t *avail_bitmap,
					bitstr_t **active_bitmap)
{
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *tmp_bitmap = NULL;
	bool can_reboot;

	*active_bitmap = NULL;
	if (!details_ptr->feature_list_use ||	/* nothing to look for */
	    (node_features_g_count() == 0))	/* No inactive features */
		return;

	can_reboot = node_features_g_user_update(job_ptr->user_id);
	find_feature_nodes(details_ptr->feature_list_use, can_reboot);
	if (_match_feature(details_ptr->feature_list_use, &tmp_bitmap) == 0)
		return;		/* No inactive features */

	bit_not(tmp_bitmap);
	if (bit_super_set(avail_bitmap, tmp_bitmap)) {
		FREE_NULL_BITMAP(tmp_bitmap);
		return;
	}
	bit_and(tmp_bitmap, avail_bitmap);
	*active_bitmap = tmp_bitmap;
}

/* Return bitmap of nodes with all specified features currently active */
extern bitstr_t *build_active_feature_bitmap2(char *reboot_features)
{
	const char *delim = ",";
	char *tmp, *tok, *save_ptr = NULL;
	bitstr_t *active_node_bitmap = NULL;
	node_feature_t *node_feat_ptr;

	if (!reboot_features || (reboot_features[0] == '\0')) {
		active_node_bitmap = node_conf_get_active_bitmap();
		return active_node_bitmap;
	}

	tmp = xstrdup(reboot_features);
	tok = strtok_r(tmp, delim, &save_ptr);

	while (tok) {
		node_feat_ptr = list_find_first(active_feature_list,
						list_find_feature, tok);
		if (node_feat_ptr && node_feat_ptr->node_bitmap) {
			/*
			 * Found feature, add nodes with this feature and
			 * remove nodes without this feature (bit_and)
			 */
			if (!active_node_bitmap)
				active_node_bitmap =
					bit_copy(node_feat_ptr->node_bitmap);
			else
				bit_and(active_node_bitmap,
					node_feat_ptr->node_bitmap);
		} else {
			/*
			 * Feature not found in any nodes, so we definitely
			 * need to reboot all of the nodes
			 */
			if (!active_node_bitmap)
				active_node_bitmap =
					bit_alloc(node_record_count);
			else
				bit_clear_all(active_node_bitmap);
			break;
		}

		tok = strtok_r(NULL, delim, &save_ptr);
	}

	xfree(tmp);

	return active_node_bitmap;
}

/*
 * Decide if a job can share nodes with other jobs based on the
 * following three input parameters:
 *
 * IN user_flag - may be 0 (do not share nodes), 1 (node sharing allowed),
 *                or any other number means "don't care"
 * IN part_max_share - current partition's node sharing policy
 *
 *
 * The followed table details the node SHARED state for the various scenarios
 *
 *					part=	part=	part=	part=
 *	cons_tres	user_request	EXCLUS	NO	YES	FORCE
 *	--------	------------	------	-----	-----	-----
 *	no		default		whole	whole	whole	whole/O
 *	no		exclusive	whole	whole	whole	whole/O
 *	no		share=yes	whole	whole	whole/O	whole/O
 *	yes		default		whole	share	share	share/O
 *	yes		exclusive	whole	whole	whole	whole/O
 *	yes		share=yes	whole	share	share/O	share/O
 *
 * whole  = entire node is allocated to the job
 * share  = less than entire node may be allocated to the job
 * -/O    = resources can be over-committed (e.g. gang scheduled)
 *
 * part->max_share:
 *	&SHARED_FORCE 	= FORCE
 *	0		= EXCLUSIVE
 *	1		= NO
 *	> 1		= YES
 *
 * job_ptr->details->share_res:
 *	0		= default or share=no
 *	1		= share=yes
 *
 * job_ptr->details->whole_node:
 *				  0	= default
 *	WHOLE_NODE_REQUIRED	= 1	= exclusive
 *	WHOLE_NODE_USER		= 2	= user
 *	WHOLE_NODE_MCS		= 3	= mcs
 *
 * Return values:
 *	0 = requires idle nodes
 *	1 = can use non-idle nodes
 */
static int _resolve_shared_status(job_record_t *job_ptr,
				  uint16_t part_max_share)
{
	if (job_ptr->reboot)
		return 0;

	/* no sharing if partition OverSubscribe=EXCLUSIVE */
	if (part_max_share == 0) {
		job_ptr->details->whole_node |= WHOLE_NODE_REQUIRED;
		job_ptr->details->share_res = 0;
		return 0;
	}

	/* sharing if partition OverSubscribe=FORCE with count > 1 */
	if ((part_max_share & SHARED_FORCE) &&
	    ((part_max_share & (~SHARED_FORCE)) > 1)) {
		job_ptr->details->share_res = 1;
		return 1;
	}

	if (slurm_select_cr_type()) {
		if ((job_ptr->details->share_res  == 0) ||
		    (job_ptr->details->whole_node & WHOLE_NODE_REQUIRED)) {
			job_ptr->details->share_res = 0;
			return 0;
		}
		return 1;
	} else {
		job_ptr->details->whole_node |= WHOLE_NODE_REQUIRED;
		if (part_max_share == 1) { /* partition is OverSubscribe=NO */
			job_ptr->details->share_res = 0;
			return 0;
		}
		/* share if the user requested it */
		if (job_ptr->details->share_res == 1)
			return 1;
		job_ptr->details->share_res = 0;
		return 0;
	}
}

typedef struct {
	job_record_t *job_ptr;
	bitstr_t *usable_node_mask;
} foreach_filter_by_node_t;

static int _foreach_filter_by_node_owner(void *x, void *arg)
{
	job_record_t *job_ptr2 = x;
	foreach_filter_by_node_t *argstruct = arg;
	job_record_t *job_ptr = argstruct->job_ptr;
	bitstr_t *usable_node_mask = argstruct->usable_node_mask;

	if (IS_JOB_PENDING(job_ptr2) || IS_JOB_COMPLETED(job_ptr2) ||
	    (job_ptr->user_id == job_ptr2->user_id) || !job_ptr2->node_bitmap)
		return 0;

	bit_and_not(usable_node_mask, job_ptr2->node_bitmap);

	return 0;
}

/*
 * Remove nodes from consideration for allocation based upon "ownership" by
 * other users
 * job_ptr IN - Job to be scheduled
 * usable_node_mask IN/OUT - Nodes available for use by this job's user
 */
extern void filter_by_node_owner(job_record_t *job_ptr,
				 bitstr_t *usable_node_mask)
{
	node_record_t *node_ptr;
	int i;
	foreach_filter_by_node_t argstruct = { .job_ptr = job_ptr,
					      .usable_node_mask =
					      usable_node_mask };

	if ((job_ptr->details->whole_node & WHOLE_NODE_USER) ||
	    (job_ptr->part_ptr->flags & PART_FLAG_EXCLUSIVE_USER)) {
		/* Need to remove all nodes allocated to any active job from
		 * any other user */
		list_for_each(job_list, _foreach_filter_by_node_owner,
			      &argstruct);
		return;
	}

	/* Need to filter out any nodes exclusively allocated to other users */
	for (i = 0; (node_ptr = next_node(&i)); i++) {
		if ((node_ptr->owner != NO_VAL) &&
		    (node_ptr->owner != job_ptr->user_id))
			bit_clear(usable_node_mask, node_ptr->index);
	}
}

/*
 * Remove nodes from consideration for allocation based upon "mcs" by
 * other users
 * job_ptr IN - Job to be scheduled
 * usable_node_mask IN/OUT - Nodes available for use by this job's mcs
 */
extern void filter_by_node_mcs(job_record_t *job_ptr, int mcs_select,
			       bitstr_t *usable_node_mask)
{
	node_record_t *node_ptr;
	int i;

	/* Need to filter out any nodes allocated with other mcs */
	if (job_ptr->mcs_label && (mcs_select == 1)) {
		for (i = 0; (node_ptr = next_node(&i)); i++) {
			/* if there is a mcs_label -> OK if it's the same */
			if ((node_ptr->mcs_label != NULL) &&
			     xstrcmp(node_ptr->mcs_label,job_ptr->mcs_label)) {
				bit_clear(usable_node_mask, node_ptr->index);
			}
			/* if no mcs_label -> OK if no jobs running */
			if ((node_ptr->mcs_label == NULL) &&
			    (node_ptr->run_job_cnt != 0)) {
				bit_clear(usable_node_mask, node_ptr->index);
			}
		}
	} else {
		for (i = 0; (node_ptr = next_node(&i)); i++) {
			 if (node_ptr->mcs_label != NULL) {
				bit_clear(usable_node_mask, node_ptr->index);
			}
		}
	}
}

/*
 * Remove nodes from the "avail_node_bitmap" which need to be rebooted in order
 * to be used if the job's "delay_boot" time has not yet been reached.
 */
static void _filter_by_node_feature(job_record_t *job_ptr,
				    struct node_set *node_set_ptr,
				    int node_set_size)
{
	int i;

	if ((job_ptr->details == NULL) ||
	    ((job_ptr->details->begin_time != 0) &&
 	     ((job_ptr->details->begin_time + job_ptr->delay_boot) <=
	      time(NULL))))
		return;

	for (i = 0; i < node_set_size; i++) {
		if (node_set_ptr[i].flags & NODE_SET_REBOOT) {
			bit_and_not(avail_node_bitmap,
				    node_set_ptr[i].my_bitmap);
		}
	}
}

static void _find_qos_grp_node_bitmap(job_record_t *job_ptr,
				      slurmdb_qos_rec_t *qos_ptr,
				      bitstr_t **grp_node_bitmap,
				      bool *per_grp_limit,
				      bool *per_user_limit,
				      bool *per_acct_limit)
{
	slurmdb_used_limits_t *used_limits = NULL;

	if (!qos_ptr || !qos_ptr->usage)
		return;

	if (!*per_grp_limit &&
	    qos_ptr->usage->grp_node_bitmap &&
	    (qos_ptr->grp_tres_ctld[TRES_ARRAY_NODE] != INFINITE64)) {
		*per_grp_limit = true;
		*grp_node_bitmap = bit_copy(qos_ptr->usage->grp_node_bitmap);
	}

	if (!*per_user_limit &&
	    (qos_ptr->max_tres_pu_ctld[TRES_ARRAY_NODE] != INFINITE64)) {
		*per_user_limit = true;
		used_limits = acct_policy_get_user_used_limits(
			&qos_ptr->usage->user_limit_list,
			job_ptr->user_id);
		if (used_limits && used_limits->node_bitmap) {
			if (*grp_node_bitmap)
				bit_or(*grp_node_bitmap,
				       used_limits->node_bitmap);
			else
				*grp_node_bitmap =
					bit_copy(used_limits->node_bitmap);
		}
	}

	if (!*per_acct_limit &&
	    job_ptr->assoc_ptr &&
	    (qos_ptr->max_tres_pa_ctld[TRES_ARRAY_NODE] != INFINITE64)) {
		*per_acct_limit = true;
		used_limits = acct_policy_get_acct_used_limits(
			&qos_ptr->usage->acct_limit_list,
			job_ptr->assoc_ptr->acct);
		if (used_limits && used_limits->node_bitmap) {
			if (*grp_node_bitmap)
				bit_or(*grp_node_bitmap,
				       used_limits->node_bitmap);
			else
				*grp_node_bitmap =
					bit_copy(used_limits->node_bitmap);
		}
	}
}

/*
 * For a given job, return a bitmap of nodes to be preferred in it's allocation
 */
static bitstr_t *_find_grp_node_bitmap(job_record_t *job_ptr)
{
	bitstr_t *grp_node_bitmap = NULL;
	slurmdb_qos_rec_t *qos_ptr1 = NULL, *qos_ptr2 = NULL;
	bool per_acct_limit = false, per_user_limit = false,
		per_grp_limit = false;
	assoc_mgr_lock_t qos_read_locks =
		{ .assoc = READ_LOCK, .qos = READ_LOCK };
	slurmdb_assoc_rec_t *assoc_ptr = job_ptr->assoc_ptr;

	/* check to see if we are enforcing associations */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return NULL;

	assoc_mgr_lock(&qos_read_locks);

	acct_policy_set_qos_order(job_ptr, &qos_ptr1, &qos_ptr2);

	_find_qos_grp_node_bitmap(job_ptr, qos_ptr1, &grp_node_bitmap,
				  &per_grp_limit,
				  &per_user_limit,
				  &per_acct_limit);

	_find_qos_grp_node_bitmap(job_ptr, qos_ptr2, &grp_node_bitmap,
				  &per_grp_limit,
				  &per_user_limit,
				  &per_acct_limit);

	while (assoc_ptr && assoc_ptr->usage && !per_grp_limit) {
		if (assoc_ptr->usage->grp_node_bitmap &&
		    (assoc_ptr->grp_tres_ctld[TRES_ARRAY_NODE] != INFINITE64)) {
			per_grp_limit = true;
			if (grp_node_bitmap)
				bit_or(grp_node_bitmap,
				       assoc_ptr->usage->grp_node_bitmap);
			else
				grp_node_bitmap = bit_copy(assoc_ptr->usage->
							   grp_node_bitmap);
			break;
		}
		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
	}

	assoc_mgr_unlock(&qos_read_locks);

	return grp_node_bitmap;
}

/*
 * If the job has required feature counts, then accumulate those
 * required resources using multiple calls to _pick_best_nodes()
 * and adding those selected nodes to the job's required node list.
 * Upon completion, return job's requirements to match the values
 * which were in effect upon calling this function.
 * Input and output are the same as _pick_best_nodes().
 */
static int _get_req_features(struct node_set *node_set_ptr, int node_set_size,
			     bitstr_t **select_bitmap, job_record_t *job_ptr,
			     part_record_t *part_ptr, uint32_t min_nodes,
			     uint32_t max_nodes, uint32_t req_nodes,
			     bool test_only, list_t **preemptee_job_list,
			     bool can_reboot, bool submission)
{
	uint32_t saved_min_nodes, saved_job_min_nodes, saved_job_num_tasks;
	bitstr_t *saved_req_node_bitmap = NULL;
	bitstr_t *inactive_bitmap = NULL;
	uint32_t saved_min_cpus, saved_req_nodes;
	int resv_rc = SLURM_SUCCESS, tmp_node_set_size;
	int mcs_select = 0;
	struct node_set *tmp_node_set_ptr, *prev_node_set_ptr;
	int error_code = SLURM_SUCCESS, i;
	bitstr_t *feature_bitmap, *accumulate_bitmap = NULL;
	bitstr_t *save_avail_node_bitmap = NULL, *resv_bitmap = NULL;
	bitstr_t *save_share_node_bitmap = NULL;
	list_t *preemptee_candidates = NULL;
	bool old_feat_change = false;
	bool has_xand = false;
	bool resv_overlap = false;
	resv_exc_t resv_exc = { 0 };
	/*
	 * Mark nodes reserved for other jobs as off limit for this job.
	 * If the job has a reservation, we've already limited the contents
	 * of select_bitmap to those nodes. Assume node reboot required
	 * since we have not selected the compute nodes yet.
	 */
	if (job_ptr->resv_name == NULL) {
		time_t start_res = time(NULL);
		resv_rc = job_test_resv(job_ptr, &start_res, false,
					&resv_bitmap, &resv_exc,
					&resv_overlap, true);
		if ((resv_rc == ESLURM_NODES_BUSY) ||
		    (resv_rc == ESLURM_RESERVATION_MAINT)) {
			save_avail_node_bitmap = avail_node_bitmap;
			avail_node_bitmap = bit_alloc(node_record_count);
			FREE_NULL_BITMAP(resv_bitmap);
			/*
			 * Continue executing through _pick_best_nodes() below
			 * in order reject job if it can never run
			 */
		} else if (resv_rc != SLURM_SUCCESS) {
			FREE_NULL_BITMAP(resv_bitmap);
			reservation_delete_resv_exc_parts(&resv_exc);
			return ESLURM_NODES_BUSY;	/* reserved */
		} else if (resv_bitmap &&
			   (!bit_equal(resv_bitmap, avail_node_bitmap))) {
			bit_and(resv_bitmap, avail_node_bitmap);
			save_avail_node_bitmap = avail_node_bitmap;
			if (slurm_conf.debug_flags & DEBUG_FLAG_RESERVATION &&
			    !bit_equal(avail_node_bitmap, resv_bitmap)) {
				bitstr_t *removed_nodes =
					bit_copy(save_avail_node_bitmap);
				bit_and_not(removed_nodes, resv_bitmap);
				log_flag(RESERVATION, "Advanced reservation removed nodes:%s from consideration for %pJ",
					 bitmap2node_name(removed_nodes),
					 job_ptr);
				FREE_NULL_BITMAP(removed_nodes);
			}
			avail_node_bitmap = resv_bitmap;
			resv_bitmap = NULL;
		} else {
			FREE_NULL_BITMAP(resv_bitmap);
		}
	} else {
		time_t start_res = time(NULL);
		/*
		 * We do not care about return value.
		 * We are just interested in resv_exc being filled in
		 */
		(void) job_test_resv(job_ptr, &start_res, false, &resv_bitmap,
				     &resv_exc, &resv_overlap, true);
		FREE_NULL_BITMAP(resv_bitmap);
	}

	if (submission)
		resv_overlap = false;

	if (!save_avail_node_bitmap)
		save_avail_node_bitmap = bit_copy(avail_node_bitmap);
	save_share_node_bitmap = bit_copy(share_node_bitmap);
	filter_by_node_owner(job_ptr, share_node_bitmap);

	if (can_reboot && !test_only)
		_filter_by_node_feature(job_ptr, node_set_ptr, node_set_size);

	if (!test_only) {
		mcs_select = slurm_mcs_get_select(job_ptr);
		filter_by_node_mcs(job_ptr, mcs_select, share_node_bitmap);
	}

	/* save job and request state */
	saved_min_nodes = min_nodes;
	saved_req_nodes = req_nodes;
	saved_job_min_nodes = job_ptr->details->min_nodes;
	if (job_ptr->details->req_node_bitmap) {
		accumulate_bitmap = job_ptr->details->req_node_bitmap;
		saved_req_node_bitmap = bit_copy(accumulate_bitmap);
		job_ptr->details->req_node_bitmap = NULL;
	}
	saved_min_cpus = job_ptr->details->min_cpus;
	/*
	 * Don't mess with max_cpus here since it is only set to be a limit
	 * and not user configurable.
	 */
	job_ptr->details->min_cpus = 1;
	tmp_node_set_ptr = xcalloc((node_set_size * 2), sizeof(struct node_set));

	/* Accumulate nodes with required feature counts. */
	preemptee_candidates = slurm_find_preemptable_jobs(job_ptr);
	if (job_ptr->details->feature_list_use) {
		list_itr_t *feat_iter;
		job_feature_t *feat_ptr;
		int last_paren_cnt = 0, last_paren_opt = FEATURE_OP_AND;
		bitstr_t *paren_bitmap = NULL, *work_bitmap;
		uint64_t smallest_min_mem = INFINITE64;
		uint64_t orig_req_mem = job_ptr->details->pn_min_memory;
		bool feat_change = false;

		feat_iter = list_iterator_create(
				job_ptr->details->feature_list_use);
		while ((feat_ptr = list_next(feat_iter))) {
			bool sort_again = false;
			if (last_paren_cnt < feat_ptr->paren) {
				/* Start of expression in parenthesis */
				if (paren_bitmap) {
					error("%s@%d: %pJ has bad feature expression: %s",
					      __func__, __LINE__, job_ptr,
					      job_ptr->details->features_use);
					FREE_NULL_BITMAP(paren_bitmap);
				}
				feat_change |= feat_ptr->changeable;
				paren_bitmap =
					bit_copy(feat_ptr->node_bitmap_avail);
				last_paren_opt = feat_ptr->op_code;
				last_paren_cnt = feat_ptr->paren;
				continue;
			} else if (last_paren_cnt > 0) {
				feat_change |= feat_ptr->changeable;
				if (last_paren_opt == FEATURE_OP_AND) {
					bit_and(paren_bitmap,
						feat_ptr->node_bitmap_avail);
				} else {
					bit_or(paren_bitmap,
					       feat_ptr->node_bitmap_avail);
				}
				last_paren_opt = feat_ptr->op_code;
				last_paren_cnt = feat_ptr->paren;
				if (last_paren_cnt)
					continue;
				work_bitmap = paren_bitmap;
			} else {
				/* Outside of parenthesis */
				feat_change = feat_ptr->changeable;
				work_bitmap = feat_ptr->node_bitmap_avail;
			}
			if (feat_ptr->count == 0) {
				FREE_NULL_BITMAP(paren_bitmap);
				continue;
			}
			tmp_node_set_size = 0;
			/*
			 * _pick_best_nodes() is destructive of the node_set
			 * data structure, so we need to make a copy and then
			 * purge it
			 */
			for (i = 0; i < node_set_size; i++) {
				if (!bit_overlap_any(node_set_ptr[i].my_bitmap,
						     work_bitmap))
					continue;
				tmp_node_set_ptr[tmp_node_set_size].
					cpus_per_node =
					node_set_ptr[i].cpus_per_node;
				tmp_node_set_ptr[tmp_node_set_size].
					real_memory =
					node_set_ptr[i].real_memory;
				tmp_node_set_ptr[tmp_node_set_size].node_weight =
					node_set_ptr[i].node_weight;
				tmp_node_set_ptr[tmp_node_set_size].sched_weight =
					node_set_ptr[i].sched_weight;
				tmp_node_set_ptr[tmp_node_set_size].flags =
					node_set_ptr[i].flags;
				tmp_node_set_ptr[tmp_node_set_size].features =
					xstrdup(node_set_ptr[i].features);
				tmp_node_set_ptr[tmp_node_set_size].
					feature_bits =
					bit_copy(node_set_ptr[i].feature_bits);
				tmp_node_set_ptr[tmp_node_set_size].my_bitmap =
					bit_copy(node_set_ptr[i].my_bitmap);
				bit_and(tmp_node_set_ptr[tmp_node_set_size].
					my_bitmap, work_bitmap);
				if (accumulate_bitmap && has_xand) {
					bit_and_not(tmp_node_set_ptr[
						tmp_node_set_size].my_bitmap,
						accumulate_bitmap);
				}
				tmp_node_set_ptr[tmp_node_set_size].node_cnt =
					bit_set_count(tmp_node_set_ptr
					[tmp_node_set_size].my_bitmap);
				prev_node_set_ptr = tmp_node_set_ptr +
						    tmp_node_set_size;
				tmp_node_set_size++;

				if (test_only || !can_reboot ||
				    (prev_node_set_ptr->flags &
				     NODE_SET_REBOOT))
					continue;
				inactive_bitmap =
					bit_copy(node_set_ptr[i].my_bitmap);
				bit_and_not(inactive_bitmap,
					    feat_ptr->node_bitmap_active);
				if (bit_ffs(inactive_bitmap) == -1) {
					/* No inactive nodes (require reboot) */
					FREE_NULL_BITMAP(inactive_bitmap);
					continue;
				}
				sort_again = true;
				if (bit_equal(prev_node_set_ptr->my_bitmap,
					      inactive_bitmap)) {
					prev_node_set_ptr->flags |=
						NODE_SET_REBOOT;
					FREE_NULL_BITMAP(inactive_bitmap);
					continue;
				}
				tmp_node_set_ptr[tmp_node_set_size].
					cpus_per_node =
					node_set_ptr[i].cpus_per_node;
				tmp_node_set_ptr[tmp_node_set_size].
					real_memory =
					node_set_ptr[i].real_memory;
				tmp_node_set_ptr[tmp_node_set_size].flags |=
					NODE_SET_REBOOT;
				tmp_node_set_ptr[tmp_node_set_size].features =
					xstrdup(node_set_ptr[i].features);
				tmp_node_set_ptr[tmp_node_set_size].
					feature_bits =
					bit_copy(node_set_ptr[i].feature_bits);
				tmp_node_set_ptr[tmp_node_set_size].my_bitmap =
					bit_copy(tmp_node_set_ptr
					[tmp_node_set_size-1].my_bitmap);
				bit_and(tmp_node_set_ptr[tmp_node_set_size].
					my_bitmap, inactive_bitmap);
				tmp_node_set_ptr[tmp_node_set_size].node_cnt =
					bit_set_count(tmp_node_set_ptr
					[tmp_node_set_size].my_bitmap);
				bit_and_not(tmp_node_set_ptr[tmp_node_set_size-1].
					my_bitmap, inactive_bitmap);
				tmp_node_set_ptr[tmp_node_set_size-1].node_cnt =
					bit_set_count(tmp_node_set_ptr
					[tmp_node_set_size-1].my_bitmap);
				tmp_node_set_size++;
				FREE_NULL_BITMAP(inactive_bitmap);
			}
			FREE_NULL_BITMAP(paren_bitmap);
			feature_bitmap = NULL;
			min_nodes = feat_ptr->count;
			req_nodes = feat_ptr->count;
			saved_job_num_tasks = job_ptr->details->num_tasks;
			job_ptr->details->min_nodes = feat_ptr->count;
			job_ptr->details->min_cpus = feat_ptr->count;
			/*
			 * Ensure that num_tasks is accurate if ntasks_per_node
			 * is set
			 */
			if (job_ptr->details->ntasks_per_node)
				job_ptr->details->num_tasks = min_nodes *
					job_ptr->details->ntasks_per_node;
			FREE_NULL_LIST(*preemptee_job_list);
			job_ptr->details->pn_min_memory = orig_req_mem;
			if (sort_again) {
				for (i = 0; i < tmp_node_set_size; i++)
					_set_sched_weight(tmp_node_set_ptr + i);
				qsort(tmp_node_set_ptr, tmp_node_set_size,
				      sizeof(struct node_set), _sort_node_set);
			}
			error_code = _pick_best_nodes(tmp_node_set_ptr,
					tmp_node_set_size, &feature_bitmap,
					job_ptr, part_ptr, min_nodes,
					max_nodes, req_nodes, test_only,
					preemptee_candidates,
					preemptee_job_list, false,
					&resv_exc, resv_overlap);
			job_ptr->details->num_tasks = saved_job_num_tasks;
			if (job_ptr->details->pn_min_memory) {
				if (job_ptr->details->pn_min_memory <
				    smallest_min_mem)
					smallest_min_mem =
						job_ptr->details->pn_min_memory;
				else
					job_ptr->details->pn_min_memory =
						smallest_min_mem;
			}
#if _DEBUG
{
			char *tmp_str = bitmap2node_name(feature_bitmap);
			info("%pJ needs %u nodes with feature %s, using %s, error_code=%d",
			     job_ptr, feat_ptr->count, feat_ptr->name,
			     tmp_str, error_code);
			xfree(tmp_str);
}
#endif
			for (i = 0; i < tmp_node_set_size; i++) {
				xfree(tmp_node_set_ptr[i].features);
				FREE_NULL_BITMAP(tmp_node_set_ptr[i].
						 feature_bits);
				FREE_NULL_BITMAP(tmp_node_set_ptr[i].
						 my_bitmap);
			}
			if (error_code != SLURM_SUCCESS) {
				FREE_NULL_BITMAP(feature_bitmap);
				break;
			}
			if (feature_bitmap) {
				if (feat_ptr->op_code == FEATURE_OP_XAND)
					has_xand = true;
				if (has_xand) {
					if (old_feat_change && feat_change) {
						error_code =
						    ESLURM_MULTI_KNL_CONSTRAINT;
						break;
					}
					old_feat_change |= feat_change;
					/*
					 * Don't make nodes required since we
					 * check value on each call to
					 * _pick_best_nodes()
					 */
				} else if (job_ptr->details->req_node_bitmap) {
					bit_or(job_ptr->details->
					       req_node_bitmap,
					       feature_bitmap);
				} else {
					job_ptr->details->req_node_bitmap =
						bit_copy(feature_bitmap);
				}
				if (accumulate_bitmap) {
					bit_or(accumulate_bitmap,
					       feature_bitmap);
					FREE_NULL_BITMAP(feature_bitmap);
				} else
					accumulate_bitmap = feature_bitmap;
			}
		}
		list_iterator_destroy(feat_iter);
		if (paren_bitmap) {
			error("%s@%d: %pJ has bad feature expression: %s",
			      __func__, __LINE__, job_ptr,
			      job_ptr->details->features_use);
			FREE_NULL_BITMAP(paren_bitmap);
		}
	}

	/* restore most of job state and accumulate remaining resources */
	if (saved_req_node_bitmap) {
		FREE_NULL_BITMAP(job_ptr->details->req_node_bitmap);
		job_ptr->details->req_node_bitmap =
				bit_copy(saved_req_node_bitmap);
	}
	if (accumulate_bitmap) {
		uint32_t node_cnt;
		if (job_ptr->details->req_node_bitmap) {
			bit_or(job_ptr->details->req_node_bitmap,
				accumulate_bitmap);
			FREE_NULL_BITMAP(accumulate_bitmap);
		} else
			job_ptr->details->req_node_bitmap = accumulate_bitmap;
		node_cnt = bit_set_count(job_ptr->details->req_node_bitmap);
		job_ptr->details->min_cpus = MAX(saved_min_cpus, node_cnt);
		min_nodes = MAX(saved_min_nodes, node_cnt);
		job_ptr->details->min_nodes = min_nodes;
		req_nodes = MAX(min_nodes, req_nodes);
		if (req_nodes > max_nodes)
			error_code = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
	} else {
		min_nodes = saved_min_nodes;
		req_nodes = saved_req_nodes;
		job_ptr->details->min_cpus = saved_min_cpus;
		job_ptr->details->min_nodes = saved_job_min_nodes;
	}

#if _DEBUG
{
	char *tmp_str = bitmap2node_name(job_ptr->details->req_node_bitmap);
	info("%pJ requires %d:%d:%d req_nodes:%s err:%u",
	     job_ptr, min_nodes, req_nodes, max_nodes, tmp_str, error_code);
	xfree(tmp_str);
}
#endif
	xfree(tmp_node_set_ptr);
	if (error_code == SLURM_SUCCESS) {
		FREE_NULL_LIST(*preemptee_job_list);
		error_code = _pick_best_nodes(node_set_ptr, node_set_size,
				select_bitmap, job_ptr, part_ptr, min_nodes,
				max_nodes, req_nodes, test_only,
				preemptee_candidates, preemptee_job_list,
				has_xand, &resv_exc, resv_overlap);
	}

	if ((resv_rc == ESLURM_RESERVATION_MAINT) &&
	    (error_code == ESLURM_NODE_NOT_AVAIL))
		error_code = ESLURM_RESERVATION_MAINT;
#if _DEBUG
{
	char *tmp_str = bitmap2node_name(*select_bitmap);
	info("%pJ allocated nodes:%s err:%u", job_ptr, tmp_str, error_code);
	xfree(tmp_str);
}
#endif

	FREE_NULL_LIST(preemptee_candidates);

	/* restore job's initial required node bitmap */
	FREE_NULL_BITMAP(job_ptr->details->req_node_bitmap);
	job_ptr->details->req_node_bitmap = saved_req_node_bitmap;
	job_ptr->details->min_cpus = saved_min_cpus;
	job_ptr->details->min_nodes = saved_job_min_nodes;

	/* Restore available node bitmap, ignoring reservations */
	if (save_avail_node_bitmap) {
		FREE_NULL_BITMAP(avail_node_bitmap);
		avail_node_bitmap = save_avail_node_bitmap;
	}
	if (save_share_node_bitmap) {
		FREE_NULL_BITMAP(share_node_bitmap);
		share_node_bitmap = save_share_node_bitmap;
	}
	reservation_delete_resv_exc_parts(&resv_exc);

	return error_code;
}

static void _sync_node_weight(struct node_set *node_set_ptr, int node_set_size)
{
	node_record_t *node_ptr;

	for (int s = 0; s < node_set_size; s++) {
		if (!node_set_ptr[s].my_bitmap)
			continue;	/* No nodes in this set */
		for (int i = 0;
		     (node_ptr = next_node_bitmap(node_set_ptr[s].my_bitmap,
						  &i));
		     i++) {
			node_ptr->sched_weight = node_set_ptr[s].sched_weight;
		}
	}
}

static int _bit_or_cond_internal(void *x, void *arg)
{
	job_record_t *job_ptr = (job_record_t *)x;
	bitstr_t *bitmap = (bitstr_t *)arg;

	if (!IS_JOB_RUNNING(job_ptr) || job_ptr->details->share_res ||
	    !job_ptr->job_resrcs)
		return 0;

	bit_or(bitmap, job_ptr->job_resrcs->node_bitmap);

	return 0;
}

static void _bit_or_cond(job_record_t *job_ptr, bitstr_t *bitmap)
{
	if (!job_ptr->het_job_list)
		_bit_or_cond_internal(job_ptr, bitmap);
	else
		list_for_each_nobreak(job_ptr->het_job_list,
				      _bit_or_cond_internal, bitmap);
}

/*
 * _pick_best_nodes - from a weight order list of all nodes satisfying a
 *	job's specifications, select the "best" for use
 * IN node_set_ptr - pointer to node specification information
 * IN node_set_size - number of entries in records pointed to by node_set_ptr
 * OUT select_bitmap - returns bitmap of selected nodes, must FREE_NULL_BITMAP
 * IN job_ptr - pointer to job being scheduled
 * IN part_ptr - pointer to the partition in which the job is being scheduled
 * IN min_nodes - minimum count of nodes required by the job
 * IN max_nodes - maximum count of nodes required by the job (0==no limit)
 * IN req_nodes - requested (or desired) count of nodes
 * IN test_only - do not actually allocate resources
 * IN/OUT preemptee_job_list - list of pointers to jobs to be preempted
 * IN resv_exc_ptr - Various TRES which can not be used
 *	NULL on first entry
 * IN has_xand - set of the constraint list includes XAND operators *and*
 *		 we have already satisfied them all
 * in resv_overlap - designated reservation overlaps another reservation
 * RET SLURM_SUCCESS on success,
 *	ESLURM_NODES_BUSY if request can not be satisfied now,
 *	ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE if request can never
 *	be satisfied,
 *	ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE if the job can not be
 *	initiated until the partition's configuration changes or
 *	ESLURM_NODE_NOT_AVAIL if required nodes are DOWN or DRAINED
 *	ESLURM_RESERVATION_BUSY if requested reservation overlaps another
 * NOTE: the caller must FREE_NULL_BITMAP memory pointed to by select_bitmap
 * Notes: The algorithm is
 *	1) If required node list is specified, determine implicitly required
 *	   processor and node count
 *	2) Determine how many disjoint required "features" are represented
 *	   (e.g. "FS1|FS2|FS3")
 *	3) For each feature: find matching node table entries, identify nodes
 *	   that are up and available (idle or shared) and add them to a bit
 *	   map
 *	4) Select_g_job_test() to select the "best" of those based upon
 *	   topology and/or workload
 *	5) If request can't be satisfied now, execute select_g_job_test()
 *	   against the list of nodes that exist in any state (perhaps DOWN
 *	   DRAINED or ALLOCATED) to determine if the request can
 *         ever be satisfied.
 */
static int _pick_best_nodes(struct node_set *node_set_ptr, int node_set_size,
			    bitstr_t **select_bitmap, job_record_t *job_ptr,
			    part_record_t *part_ptr, uint32_t min_nodes,
			    uint32_t max_nodes, uint32_t req_nodes,
			    bool test_only, list_t *preemptee_candidates,
			    list_t **preemptee_job_list, bool has_xand,
			    resv_exc_t *resv_exc_ptr, bool resv_overlap)
{
	int error_code = SLURM_SUCCESS, i, j, pick_code = SLURM_SUCCESS;
	int total_nodes = 0, avail_nodes = 0;
	bitstr_t *avail_bitmap = NULL, *total_bitmap = NULL;
	bitstr_t *backup_bitmap = NULL;
	bitstr_t *possible_bitmap = NULL;
	bitstr_t *node_set_map;
	int max_feature, min_feature;
	bool runable_ever  = false;	/* Job can ever run */
	bool runable_avail = false;	/* Job can run with available nodes */
	bool tried_sched = false;	/* Tried to schedule with avail nodes */
	bool preempt_flag = false;
	bool nodes_busy = false;
	bool licenses_unavailable = false;
	int shared = 0, select_mode;
	list_t *preemptee_cand = NULL;

	/*
	 * Since you could potentially have multiple features and the
	 * job might not request memory we need to keep track of a minimum
	 * from the selected features.  This is to fulfill commit
	 * 700e7b1d4e9.
	 * If no memory is requested but we are running with
	 * CR_*_MEMORY and the request is for
	 * nodes of different memory sizes we need to reset the
	 * pn_min_memory as select_g_job_test can
	 * alter that making it so the order of contraints
	 * matter since the first pass through this will set the
	 * pn_min_memory based on that first constraint and if
	 * it isn't smaller than all the other requests they
	 * will fail.  We have to keep track of the
	 * memory for accounting, these next 2 variables do this for us.
	 */
	uint64_t smallest_min_mem = INFINITE64;
	uint64_t orig_req_mem = job_ptr->details->pn_min_memory;
	static int loc_topo_record_cnt = -1;

	if (loc_topo_record_cnt == -1) {
		loc_topo_record_cnt = 0;
		(void) topology_g_get(TOPO_DATA_REC_CNT, &loc_topo_record_cnt);
	}

	if (test_only)
		select_mode = SELECT_MODE_TEST_ONLY;
	else
		select_mode = SELECT_MODE_RUN_NOW;

	if ((job_ptr->details->min_nodes == 0) &&
	    (job_ptr->details->max_nodes == 0)) {
		/* Zero compute node job (burst buffer use only) */
		avail_bitmap = bit_alloc(node_record_count);
		pick_code = select_g_job_test(job_ptr,
					      avail_bitmap,
					      0, 0, 0,
					      select_mode,
					      preemptee_candidates,
					      preemptee_job_list,
					      resv_exc_ptr,
					      NULL);

		if (pick_code == SLURM_SUCCESS) {
			*select_bitmap = avail_bitmap;
			return SLURM_SUCCESS;
		} else {
			FREE_NULL_BITMAP(avail_bitmap);
			if (pick_code == ESLURM_LICENSES_UNAVAILABLE)
				return ESLURM_LICENSES_UNAVAILABLE;
			else
				return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
		}
	} else if (node_set_size == 0) {
		info("%s: empty node set for selection", __func__);
		return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
	}

	shared = _resolve_shared_status(job_ptr, part_ptr->max_share);

	/*
	 * If job preemption is enabled, then do NOT limit the set of available
	 * nodes by their current 'sharable' or 'idle' setting
	 */
	preempt_flag = slurm_preemption_enabled();

	if (job_ptr->details->req_node_bitmap) {  /* specific nodes required */
		/*
		 * We have already confirmed that all of these nodes have a
		 * usable configuration and are in the proper partition.
		 * Check that these nodes can be used by this job.
		 */
		if (min_nodes != 0) {
			total_nodes = bit_set_count(
				job_ptr->details->req_node_bitmap);
		}
		if (total_nodes > max_nodes) {	/* exceeds node limit */
			return ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE;
		}
		if ((job_ptr->details->core_spec != NO_VAL16) &&
		    ((job_ptr->details->core_spec & CORE_SPEC_THREAD) == 0)) {
			i = bit_ffs(job_ptr->details->req_node_bitmap);
			if (i >= 0) {
				j = node_record_table_ptr[i]->tot_cores;
			}
			if ((i >= 0) && (job_ptr->details->core_spec >= j)) {
				if (part_ptr->name) {
					info("%s: %pJ never runnable in partition %s",
					     __func__, job_ptr,
					     part_ptr->name);
				} else {
					info("%s: %pJ never runnable",
					     __func__, job_ptr);
				}
				return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
			}
		}

		/*
		 * Check the availability of these nodes.
		 * Should we check memory availability on these nodes?
		 */
		if (!bit_super_set(job_ptr->details->req_node_bitmap,
				   avail_node_bitmap)) {
			return ESLURM_NODE_NOT_AVAIL;
		}

		/*
		 * Still must go through select_g_job_test() to determine the
		 * validity of request and/or perform set-up before job launch
		 */
		total_nodes = 0;	/* reinitialize */
	}

	/* identify the min and max feature values for possible exclusive OR */
	max_feature = -1;
	min_feature = MAX_FEATURES;
	for (i = 0; i < node_set_size; i++) {
		j = bit_ffs(node_set_ptr[i].feature_bits);
		if ((j >= 0) && (j < min_feature))
			min_feature = j;
		j = bit_fls(node_set_ptr[i].feature_bits);
		if ((j >= 0) && (j > max_feature))
			max_feature = j;
	}

	debug3("%s: %pJ idle_nodes %u share_nodes %u",
	       __func__, job_ptr, bit_set_count(idle_node_bitmap),
		bit_set_count(share_node_bitmap));

	if (slurm_select_cr_type() == SELECT_TYPE_CONS_TRES)
		_sync_node_weight(node_set_ptr, node_set_size);
	/*
	 * Accumulate resources for this job based upon its required
	 * features (possibly with node counts).
	 */
	for (j = min_feature; j <= max_feature; j++) {
		if (job_ptr->details->req_node_bitmap) {
			bool missing_required_nodes = false;
			bool feature_found = false;
			for (i = 0; i < node_set_size; i++) {
				if (!bit_test(node_set_ptr[i].feature_bits, j))
					continue;
				feature_found = true;
				node_set_map =
					bit_copy(node_set_ptr[i].my_bitmap);

				if ((node_set_ptr[i].flags & NODE_SET_REBOOT)) {
					/* Node reboot required */
					bit_and(node_set_map,
						idle_node_bitmap);
					/*
					 * Powered up cloud nodes can't be
					 * rebooted to get new features. Must be
					 * powered down first.
					 */
					bit_and_not(node_set_map,
						    cloud_node_bitmap);
				}

				if (avail_bitmap) {
					bit_or(avail_bitmap, node_set_map);
					FREE_NULL_BITMAP(node_set_map);
				} else {
					avail_bitmap = node_set_map;
				}

			}
			if (!feature_found)
				continue;
			if (!bit_super_set(job_ptr->details->req_node_bitmap,
					   avail_bitmap))
				missing_required_nodes = true;

			if (missing_required_nodes)
				continue;
			FREE_NULL_BITMAP(avail_bitmap);
			avail_bitmap = bit_copy(job_ptr->details->
						req_node_bitmap);
			bit_and_not(avail_bitmap, rs_node_bitmap);
		}
		for (i = 0; i < node_set_size; i++) {
			int count1 = 0, count2 = 0;
			if (!has_xand &&
			    !bit_test(node_set_ptr[i].feature_bits, j)) {
				if ((i+1) < node_set_size || !avail_bitmap)
					continue;
				else
					goto try_sched;
			}

			if (total_bitmap) {
				bit_or(total_bitmap,
				       node_set_ptr[i].my_bitmap);
			} else {
				total_bitmap = bit_copy(
						node_set_ptr[i].my_bitmap);
			}

			if ((node_set_ptr[i].flags & NODE_SET_REBOOT)) {
				/* Node reboot required */
				count1 = bit_set_count(node_set_ptr[i].
						       my_bitmap);
				bit_and(node_set_ptr[i].my_bitmap,
					idle_node_bitmap);
				/*
				 * Powered up cloud nodes can't be rebooted to
				 * get new features. Must be powered down first.
				 */
				bit_and_not(node_set_ptr[i].my_bitmap,
					    cloud_node_bitmap);
				count2 = bit_set_count(node_set_ptr[i].
						       my_bitmap);
				if (count1 != count2)
					nodes_busy = true;
			}

			bit_and(node_set_ptr[i].my_bitmap, avail_node_bitmap);
			if (!nodes_busy) {
				count1 = bit_set_count(node_set_ptr[i].
						       my_bitmap);
			}
			if (!preempt_flag) {
				if (shared) {
					bit_and(node_set_ptr[i].my_bitmap,
						share_node_bitmap);
					bit_and_not(node_set_ptr[i].my_bitmap,
						    cg_node_bitmap);
				} else {
					bit_and(node_set_ptr[i].my_bitmap,
						idle_node_bitmap);
					/* IDLE nodes are not COMPLETING */
				}
			} else {
				bit_and_not(node_set_ptr[i].my_bitmap,
					    cg_node_bitmap);
			}

			/*
			 * We must skip the node *only* in the case it is
			 * rebooted with ASAP flag.
			 */
			bit_and_not(node_set_ptr[i].my_bitmap,
				    asap_node_bitmap);

			if (!nodes_busy) {
				count2 = bit_set_count(node_set_ptr[i].
						       my_bitmap);
				if (count1 != count2)
					nodes_busy = true;
			}
			if (avail_bitmap) {
				bit_or(avail_bitmap,
				       node_set_ptr[i].my_bitmap);
			} else {
				avail_bitmap = bit_copy(node_set_ptr[i].
							my_bitmap);
			}

			tried_sched = false;	/* need to test these nodes */

			if ((slurm_select_cr_type() == SELECT_TYPE_CONS_TRES) &&
			    ((i+1) < node_set_size)) {
				/*
				 * Execute select_g_job_test() _once_ using
				 * sched_weight in node_record_t as set
				 * by _sync_node_weight()
				 */
				continue;
			}

			if ((shared || preempt_flag ||
			    (loc_topo_record_cnt > 1))     &&
			    ((i+1) < node_set_size)	 &&
			    (min_feature == max_feature) &&
			    (node_set_ptr[i].sched_weight ==
			     node_set_ptr[i+1].sched_weight)) {
				/* Keep accumulating so we can pick the
				 * most lightly loaded nodes */
				continue;
			}
try_sched:
			/* NOTE: select_g_job_test() is destructive of
			 * avail_bitmap, so save a backup copy */
			backup_bitmap = bit_copy(avail_bitmap);
			FREE_NULL_LIST(*preemptee_job_list);
			if (job_ptr->details->req_node_bitmap == NULL)
				bit_and(avail_bitmap, avail_node_bitmap);

			bit_and(avail_bitmap, share_node_bitmap);

			avail_nodes = bit_set_count(avail_bitmap);
			if (((avail_nodes  < min_nodes)	||
			     ((avail_nodes >= min_nodes) &&
			      (avail_nodes < req_nodes))) &&
			    ((i+1) < node_set_size)) {
				FREE_NULL_BITMAP(avail_bitmap);
				avail_bitmap = backup_bitmap;
				continue;	/* Keep accumulating nodes */
			}

			/* Only preempt jobs when all possible nodes are being
			 * considered for use, otherwise we would preempt jobs
			 * to use the lowest weight nodes. */
			if ((i+1) < node_set_size || !preemptee_candidates)
				preemptee_cand = NULL;
			else if (preempt_flag) {
				job_record_t *tmp_job_ptr = NULL;
				list_itr_t *job_iterator;
				job_iterator = list_iterator_create(preemptee_candidates);
				while ((tmp_job_ptr = list_next(job_iterator)))
					_bit_or_cond(tmp_job_ptr, avail_bitmap);
				list_iterator_destroy(job_iterator);
				bit_and(avail_bitmap, avail_node_bitmap);
				bit_and(avail_bitmap, total_bitmap);
				preemptee_cand = preemptee_candidates;
			} else
				preemptee_cand = preemptee_candidates;

			job_ptr->details->pn_min_memory = orig_req_mem;
			pick_code = select_g_job_test(job_ptr,
						      avail_bitmap,
						      min_nodes,
						      max_nodes,
						      req_nodes,
						      select_mode,
						      preemptee_cand,
						      preemptee_job_list,
						      resv_exc_ptr,
						      NULL);
			if (job_ptr->details->pn_min_memory) {
				if (job_ptr->details->pn_min_memory <
				    smallest_min_mem)
					smallest_min_mem =
						job_ptr->details->pn_min_memory;
				else
					job_ptr->details->pn_min_memory =
						smallest_min_mem;
			}

#if _DEBUG
{
			char *tmp_str1 = bitmap2node_name(avail_bitmap);
			char *tmp_str2 = bitmap2node_name(backup_bitmap);
			info("%s: %pJ err:%d nodes:%u:%u:%u mode:%u select %s from %s",
			     __func__, job_ptr, pick_code, min_nodes, req_nodes,
			     max_nodes, select_mode, tmp_str1, tmp_str2);
			xfree(tmp_str1);
			xfree(tmp_str2);
}
#endif

			if (pick_code == SLURM_SUCCESS) {
				FREE_NULL_BITMAP(backup_bitmap);
				if (bit_set_count(avail_bitmap) > max_nodes) {
					/* end of tests for this feature */
					avail_nodes = 0;
					break;
				}
				FREE_NULL_BITMAP(total_bitmap);
				FREE_NULL_BITMAP(possible_bitmap);
				*select_bitmap = avail_bitmap;
				return SLURM_SUCCESS;
			} else {
				tried_sched = true;	/* test failed */
				FREE_NULL_BITMAP(avail_bitmap);
				avail_bitmap = backup_bitmap;
			}
		} /* for (i = 0; i < node_set_size; i++) */

		/* try to get req_nodes now for this feature */
		if (avail_bitmap && (!tried_sched)	&&
		    (avail_nodes >= min_nodes)		&&
		    ((job_ptr->details->req_node_bitmap == NULL) ||
		     bit_super_set(job_ptr->details->req_node_bitmap,
				   avail_bitmap))) {
			FREE_NULL_LIST(*preemptee_job_list);
			job_ptr->details->pn_min_memory = orig_req_mem;
			pick_code = select_g_job_test(job_ptr, avail_bitmap,
						      min_nodes, max_nodes,
						      req_nodes,
						      select_mode,
						      preemptee_candidates,
						      preemptee_job_list,
						      resv_exc_ptr,
						      NULL);

			if (job_ptr->details->pn_min_memory) {
				if (job_ptr->details->pn_min_memory <
				    smallest_min_mem)
					smallest_min_mem =
						job_ptr->details->pn_min_memory;
				else
					job_ptr->details->pn_min_memory =
						smallest_min_mem;
			}

			if ((pick_code == SLURM_SUCCESS) &&
			     (bit_set_count(avail_bitmap) <= max_nodes)) {
				FREE_NULL_BITMAP(total_bitmap);
				FREE_NULL_BITMAP(possible_bitmap);
				*select_bitmap = avail_bitmap;
				return SLURM_SUCCESS;
			}
		}

		if (pick_code == ESLURM_LICENSES_UNAVAILABLE)
			licenses_unavailable = true;

		/* determine if job could possibly run (if all configured
		 * nodes available) */
		if (total_bitmap)
			total_nodes = bit_set_count(total_bitmap);
		if (total_bitmap			&&
		    (!runable_ever || !runable_avail)	&&
		    (total_nodes >= min_nodes)		&&
		    ((job_ptr->details->req_node_bitmap == NULL) ||
		     (bit_super_set(job_ptr->details->req_node_bitmap,
					total_bitmap)))) {
			avail_nodes = bit_set_count(avail_bitmap);
			if (!runable_avail && (avail_nodes >= min_nodes)) {
				FREE_NULL_BITMAP(avail_bitmap);
				avail_bitmap = bit_copy(total_bitmap);
				bit_and(avail_bitmap, avail_node_bitmap);
				job_ptr->details->pn_min_memory = orig_req_mem;
				pick_code = select_g_job_test(job_ptr,
						avail_bitmap,
						min_nodes,
						max_nodes,
						req_nodes,
						SELECT_MODE_TEST_ONLY,
						preemptee_candidates, NULL,
						resv_exc_ptr,
						NULL);

				if (job_ptr->details->pn_min_memory) {
					if (job_ptr->details->pn_min_memory <
					    smallest_min_mem)
						smallest_min_mem =
							job_ptr->details->
							pn_min_memory;
					else
						job_ptr->details->
							pn_min_memory =
							smallest_min_mem;
				}

				if (pick_code == SLURM_SUCCESS) {
					runable_ever  = true;
					if (bit_set_count(avail_bitmap) <=
					     max_nodes)
						runable_avail = true;
					FREE_NULL_BITMAP(possible_bitmap);
					possible_bitmap = avail_bitmap;
					avail_bitmap = NULL;
				}
			}
			if (!runable_ever) {
				job_ptr->details->pn_min_memory = orig_req_mem;
				pick_code = select_g_job_test(job_ptr,
						total_bitmap,
						min_nodes,
						max_nodes,
						req_nodes,
						SELECT_MODE_TEST_ONLY,
						preemptee_candidates, NULL,
						resv_exc_ptr,
						NULL);

				if (job_ptr->details->pn_min_memory) {
					if (job_ptr->details->pn_min_memory <
					    smallest_min_mem)
						smallest_min_mem =
							job_ptr->details->
							pn_min_memory;
					else
						job_ptr->details->
							pn_min_memory =
							smallest_min_mem;
				}

				if (pick_code == SLURM_SUCCESS) {
					FREE_NULL_BITMAP(possible_bitmap);
					possible_bitmap = total_bitmap;
					total_bitmap = NULL;
					runable_ever = true;
				}
			}
		}
		FREE_NULL_BITMAP(avail_bitmap);
		FREE_NULL_BITMAP(total_bitmap);
		if (error_code != SLURM_SUCCESS)
			break;
	}
	FREE_NULL_BITMAP(avail_bitmap);
	FREE_NULL_BITMAP(total_bitmap);

	/* The job is not able to start right now, return a
	 * value indicating when the job can start */
	if (!runable_ever && resv_overlap &&
	    (pick_code != ESLURM_REQUESTED_TOPO_CONFIG_UNAVAILABLE)) {
		error_code = ESLURM_RESERVATION_BUSY;
		return error_code;
	}
	if (licenses_unavailable) {
		error_code = ESLURM_LICENSES_UNAVAILABLE;
	} else if (!runable_ever) {
		char *tmp;

		/*
		 * If a job requested extra_constraints, then assume
		 * that the job might be runnable at some point in the
		 * future. FIXME: This is a kludge and this assumption
		 * may be wrong.
		 */
		tmp = job_ptr->extra_constraints ?
			"currently not runnable" : "never runnable";
		if (part_ptr->name) {
			info("%s: %pJ %s in partition %s",
			     __func__, job_ptr, tmp, part_ptr->name);
		} else {
			info("%s: job %pJ %s",
			     __func__, job_ptr, tmp);
		}

		if (pick_code == ESLURM_REQUESTED_TOPO_CONFIG_UNAVAILABLE)
			error_code = pick_code;
		else
			error_code = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;

	} else if (!runable_avail && !nodes_busy) {
		error_code = ESLURM_NODE_NOT_AVAIL;
	} else if (job_ptr->details->req_node_bitmap &&
		   bit_overlap_any(job_ptr->details->req_node_bitmap,
				   rs_node_bitmap)) {
		error_code = ESLURM_NODES_BUSY;
	} else if (!preempt_flag && job_ptr->details->req_node_bitmap) {
		/* specific nodes required */
		if (shared) {
			if (!bit_super_set(job_ptr->details->req_node_bitmap,
					   share_node_bitmap)) {
				error_code = ESLURM_NODES_BUSY;
			}
			if (bit_overlap_any(job_ptr->details->req_node_bitmap,
					    cg_node_bitmap)) {
				error_code = ESLURM_NODES_BUSY;
			}
		} else if (!bit_super_set(job_ptr->details->req_node_bitmap,
					  idle_node_bitmap)) {
			error_code = ESLURM_NODES_BUSY;
			/* Note: IDLE nodes are not COMPLETING */
		}
	} else if (job_ptr->details->req_node_bitmap &&
		   bit_overlap_any(job_ptr->details->req_node_bitmap,
				   cg_node_bitmap)) {
		error_code = ESLURM_NODES_BUSY;
	}

	if (error_code == SLURM_SUCCESS) {
		error_code = ESLURM_NODES_BUSY;
	}

	if (possible_bitmap && runable_ever) {
		*select_bitmap = possible_bitmap;
	} else {
		FREE_NULL_BITMAP(possible_bitmap);
	}
	return error_code;
}

static void _preempt_jobs(list_t *preemptee_job_list, bool kill_pending,
			  int *error_code, job_record_t *preemptor_ptr)
{
	list_itr_t *iter;
	job_record_t *job_ptr;
	uint16_t mode;
	int job_cnt = 0;
	static time_t sched_update = 0;

	if (sched_update != slurm_conf.last_update) {
		preempt_send_user_signal = false;
		if (xstrcasestr(slurm_conf.preempt_params,
				"send_user_signal") ||
		    xstrcasestr(slurm_conf.slurmctld_params,
				"preempt_send_user_signal"))
			preempt_send_user_signal = true;

		sched_update = slurm_conf.last_update;
	}

	iter = list_iterator_create(preemptee_job_list);
	while ((job_ptr = list_next(iter))) {
		mode = slurm_job_preempt_mode(job_ptr);

		if (mode == PREEMPT_MODE_OFF) {
			error("%s: Invalid preempt_mode %u for %pJ",
			      __func__, mode, job_ptr);
			continue;
		}

		if ((mode == PREEMPT_MODE_SUSPEND) &&
		    (slurm_conf.preempt_mode & PREEMPT_MODE_GANG)) {
			debug("preempted %pJ suspended by gang scheduler to reclaim resources for %pJ",
			      job_ptr, preemptor_ptr);
			job_ptr->preempt_time = time(NULL);
			continue;
		}

		job_cnt++;
		if (!kill_pending)
			continue;

		if (slurm_job_preempt(job_ptr, preemptor_ptr, mode, true) !=
		    SLURM_SUCCESS)
			continue;
	}
	list_iterator_destroy(iter);

	if (job_cnt > 0)
		*error_code = ESLURM_NODES_BUSY;
}

/* Return true if this job record is
 * 1) not a job array OR
 * 2) the first task of a job array to begin execution */
static bool _first_array_task(job_record_t *job_ptr)
{
	job_record_t *meta_job_ptr;

	if (job_ptr->array_task_id == NO_VAL)
		return true;

	meta_job_ptr = find_job_record(job_ptr->array_job_id);
	if (!meta_job_ptr || !meta_job_ptr->array_recs) {
		error("%s: Could not find meta job record for %pJ",
		      __func__, job_ptr);
		return true;
	}
	if ((meta_job_ptr->array_recs->tot_run_tasks == 1) &&	/* This task */
	    (meta_job_ptr->array_recs->tot_comp_tasks == 0))
		return true;

	return false;
}

/*
 * This job has zero node count. It is only designed to create or destroy
 * persistent burst buffer resources. Terminate it now.
 */
static void _end_null_job(job_record_t *job_ptr)
{
	time_t now = time(NULL);

	job_ptr->exit_code = 0;
	gres_stepmgr_job_clear_alloc(job_ptr->gres_list_req);
	gres_stepmgr_job_clear_alloc(job_ptr->gres_list_req_accum);
	FREE_NULL_LIST(job_ptr->gres_list_alloc);
	job_state_set(job_ptr, JOB_RUNNING);
	job_ptr->bit_flags |= JOB_WAS_RUNNING;
	FREE_NULL_BITMAP(job_ptr->node_bitmap);
	xfree(job_ptr->nodes);
	xfree(job_ptr->sched_nodes);
	job_ptr->start_time = now;
	job_ptr->state_reason = WAIT_NO_REASON;
	xfree(job_ptr->state_desc);
	job_ptr->time_last_active = now;
	if (!job_ptr->step_list)
		job_ptr->step_list = list_create(free_step_record);

	(void) job_array_post_sched(job_ptr, true);
	(void) bb_g_job_begin(job_ptr);
	job_array_start(job_ptr);
	rebuild_job_part_list(job_ptr);
	if ((job_ptr->mail_type & MAIL_JOB_BEGIN) &&
	    ((job_ptr->mail_type & MAIL_ARRAY_TASKS) ||
	     _first_array_task(job_ptr)))
		mail_job_info(job_ptr, MAIL_JOB_BEGIN);
	slurmctld_diag_stats.jobs_started++;
	/* Call job_set_alloc_tres() before acct_policy_job_begin() */
	job_set_alloc_tres(job_ptr, false);
	acct_policy_job_begin(job_ptr, false);
	/*
	 * If run with slurmdbd, this is handled out of band in the job if
	 * happening right away.  If the job has already become eligible and
	 * registered in the db then the start message.
	 */
	jobacct_storage_g_job_start(acct_db_conn, job_ptr);
	prolog_slurmctld(job_ptr);

	job_ptr->end_time = now;
	job_state_set(job_ptr, JOB_COMPLETE);
	job_completion_logger(job_ptr, false);
	acct_policy_job_fini(job_ptr, false);
	if (select_g_job_fini(job_ptr) != SLURM_SUCCESS)
		error("select_g_job_fini(%pJ): %m", job_ptr);
	epilog_slurmctld(job_ptr);
}

static void _handle_explicit_req(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	list_t **ret_gres_list = arg;

	/* Copy over the explicit gres, skip others */
	if (!(gres_state_job->config_flags & GRES_CONF_EXPLICIT) &&
	    !gres_id_shared(gres_state_job->config_flags))
		return;

	if (!*ret_gres_list)
		*ret_gres_list = list_create(gres_job_list_delete);

	list_append(*ret_gres_list,
		    gres_create_state(
			    gres_state_job,
			    GRES_STATE_SRC_STATE_PTR,
			    GRES_STATE_TYPE_JOB,
			    gres_job_state_dup(gres_state_job->gres_data)));
}

static void _gres_select_explicit(
	list_t *req_gres_list, list_t **ret_gres_list)
{
	if (!req_gres_list)
		return;

	(void) list_for_each(req_gres_list,
			     (ListForF) _handle_explicit_req,
			     ret_gres_list);
}

static list_t *_handle_exclusive_gres(job_record_t *job_ptr,
				      bitstr_t *select_bitmap, bool test_only)
{
	list_t *post_list = NULL;
	node_record_t *node_ptr;

	if (test_only || !gres_get_gres_cnt())
		return NULL;

	xassert(job_ptr);
	xassert(select_bitmap);

	if (!job_ptr->details ||
	    !(job_ptr->details->whole_node & WHOLE_NODE_REQUIRED))
		return NULL;

	if (job_ptr->gres_list_req)
		_gres_select_explicit(job_ptr->gres_list_req, &post_list);

	for (int i = 0; (node_ptr = next_node_bitmap(select_bitmap, &i)); i++) {
		gres_stepmgr_job_select_whole_node(
			&post_list,
			node_ptr->gres_list,
			job_ptr->job_id,
			node_ptr->name);
	}

	return post_list;
}

typedef struct {
	uint64_t gpu_cnt;
	int node_inx;
} foreach_node_gpu_args_t;

static int _get_node_gpu_sum(void *x, void *arg)
{
	foreach_node_gpu_args_t *args = arg;
	gres_state_t *gres_job_state = x;
	gres_job_state_t *gres_js;

	if (gres_job_state->plugin_id != gres_get_gpu_plugin_id())
		return SLURM_SUCCESS;
	gres_js = gres_job_state->gres_data;
	args->gpu_cnt += gres_js->gres_cnt_node_select[args->node_inx];

	return SLURM_SUCCESS;
}

static uint64_t _get_max_node_gpu_cnt(bitstr_t *node_bitmap, list_t* gres_list)
{
	foreach_node_gpu_args_t args;
	uint64_t max_node_gpu_cnt = 0;

	xassert(node_bitmap);
	xassert(gres_list);

	for (int i = 0; (i = bit_ffs_from_bit(node_bitmap, i)) >= 0; i++) {
		args.gpu_cnt = 0;
		args.node_inx = i;
		/* Get the sum of all gpu types on the node */
		list_for_each(gres_list, _get_node_gpu_sum, &args);
		max_node_gpu_cnt = MAX(max_node_gpu_cnt, args.gpu_cnt);
	}

	return max_node_gpu_cnt;
}

static int _get_resv_mpi_ports(job_record_t *job_ptr,
			       uint16_t *orig_resv_port_cnt,
			       uint32_t node_cnt,
			       time_t now)
{
	int error_code = SLURM_SUCCESS;
	bool resv_ports_present = false;

	if (!(job_ptr->bit_flags & STEPMGR_ENABLED))
		return SLURM_SUCCESS;

	if (slurm_conf.mpi_params && xstrstr(slurm_conf.mpi_params, "ports="))
		resv_ports_present = true;

	if (resv_ports_present &&
	    (job_ptr->resv_port_cnt == NO_VAL16)) {
		if (!job_ptr->job_resrcs) {
			error("Select plugin failed to set job resources");
			/*
			* Do not attempt to allocate the select_bitmap nodes
			* since select plugin failed to set job resources
			*/
			error_code = ESLURM_NODES_BUSY;
			job_ptr->start_time = 0;
			job_ptr->time_last_active = 0;
			job_ptr->end_time = 0;
			job_ptr->state_reason = WAIT_RESOURCES;
			last_job_update = now;
			xfree(job_ptr->state_desc);
			return error_code;
		}

		*orig_resv_port_cnt = job_ptr->resv_port_cnt;
		job_ptr->resv_port_cnt = 0;

		/*
		 * reserved port count set to maximum task count on
		 * any node plus one, or if the job is exclusive give all
		 * resvered ports.
		*/
		if ((job_ptr->job_resrcs->node_req == NODE_CR_RESERVED) ||
		    (job_ptr->details->whole_node & WHOLE_NODE_REQUIRED)) {
			job_ptr->resv_port_cnt =
				resv_port_get_resv_port_cnt() - 1;
		} else if (!job_ptr->details->overcommit &&
			   (job_ptr->details->num_tasks ||
			    job_ptr->details->ntasks_per_node ||
			    job_ptr->details->ntasks_per_tres)) {
			for (int i = 0; i < node_cnt; i++) {
				job_ptr->resv_port_cnt =
					MAX(job_ptr->resv_port_cnt,
					    job_ptr->job_resrcs->
					    tasks_per_node[i]) * 2;
			}
		} else if (!job_ptr->details->overcommit) {
			uint16_t max_node_cpus = 0;
			for (int i = 0; i < node_cnt; i++) {
				max_node_cpus =
					MAX(max_node_cpus,
					    job_ptr->job_resrcs->cpus[i] * 2);
			}
			job_ptr->resv_port_cnt = max_node_cpus;
		} else if (job_ptr->details->ntasks_per_node) {
			job_ptr->resv_port_cnt =
				job_ptr->details->ntasks_per_node;
		} else if (job_ptr->details->ntasks_per_tres &&
			   job_ptr->gres_list_req ) {
			uint64_t max_gpu_per_node =
				_get_max_node_gpu_cnt(
					job_ptr->node_bitmap,
					job_ptr->gres_list_req);

			if (max_gpu_per_node > slurm_conf.max_tasks_per_node)
				max_gpu_per_node =
					slurm_conf.max_tasks_per_node;
			job_ptr->resv_port_cnt =
				(uint16_t) max_gpu_per_node *
				job_ptr->details->ntasks_per_tres;
		} else if (job_ptr->details->num_tasks) {
			job_ptr->resv_port_cnt = ROUNDUP(
				job_ptr->details->num_tasks, node_cnt);
		} else {
			job_ptr->resv_port_cnt = ROUNDUP(
				job_ptr->job_resrcs->ncpus, node_cnt);
		}

		job_ptr->resv_port_cnt++;
	}
	if ((job_ptr->resv_port_cnt != NO_VAL16) &&
	    (job_ptr->resv_port_cnt != 0)) {
		error_code = resv_port_job_alloc(job_ptr);
		if (error_code) {
			job_ptr->start_time = 0;
			job_ptr->time_last_active = 0;
			job_ptr->end_time = 0;
			job_ptr->state_reason = WAIT_MPI_PORTS_BUSY;
			last_job_update = now;
			xfree(job_ptr->state_desc);
		}
	}

	return error_code;
}

/*
 * select_nodes - select and allocate nodes to a specific job
 * IN job_node_select - pointer with at least a pointer to the job record
 * IN test_only - if set do not allocate nodes, just confirm they
 *	could be allocated now
 * IN select_node_bitmap - bitmap of nodes to be used for the
 *	job's resource allocation (not returned if NULL), caller
 *	must free
 * IN submission - if set ignore reservations
 * IN scheduler_type - which scheduler is calling this
 *      (i.e. SLURMDB_JOB_FLAG_BACKFILL, SLURMDB_JOB_FLAG_SCHED, etc)
 * RET 0 on success, ESLURM code from slurm_errno.h otherwise
 * globals: list_part - global list of partition info
 *	default_part_loc - pointer to default partition
 *	config_list - global list of node configuration info
 * Notes: The algorithm is
 *	1) Build a table (node_set_ptr) of nodes with the requisite
 *	   configuration. Each table entry includes their weight,
 *	   node_list, features, etc.
 *	2) Call _pick_best_nodes() to select those nodes best satisfying
 *	   the request, (e.g. best-fit or other criterion)
 *	3) Call allocate_nodes() to perform the actual allocation
 */
extern int select_nodes(job_node_select_t *job_node_select,
			bool test_only,	bool submission,
			uint32_t scheduler_type)
{
	int bb, error_code = SLURM_SUCCESS, i, node_set_size = 0;
	bitstr_t *select_bitmap = NULL;
	struct node_set *node_set_ptr = NULL;
	part_record_t *part_ptr = NULL;
	uint8_t orig_whole_node, orig_share_res;
	uint16_t orig_resv_port_cnt = 0;
	uint32_t min_nodes = 0, max_nodes = 0, req_nodes = 0;
	time_t now = time(NULL);
	bool configuring = false;
	list_t *preemptee_job_list = NULL;
	uint32_t selected_node_cnt = NO_VAL;
	uint64_t tres_req_cnt[slurmctld_tres_cnt];
	bool can_reboot;
	uint32_t qos_flags = 0;
	assoc_mgr_lock_t qos_read_lock =
		{ .assoc = READ_LOCK, .qos = READ_LOCK };
	assoc_mgr_lock_t job_read_locks =
		{ .assoc = READ_LOCK, .qos = WRITE_LOCK, .tres = READ_LOCK };
	list_t *gres_list_pre = NULL;
	bool gres_list_pre_set = false;
	job_record_t *tmp_job, *job_ptr = job_node_select->job_ptr;

	xassert(job_ptr);
	xassert(job_ptr->magic == JOB_MAGIC);

	/*
	 * The call path from _get_req_features() (called later in this
	 * function) can eventually call _resolve_shared_status(). This latter
	 * function can alter the job_ptr->details->{whole_node,share_res}.
	 *
	 * Saving the original values here and restoring them at cleanup time
	 * at the bottom of this function if needed.
	 */
	orig_whole_node = job_ptr->details->whole_node;
	orig_share_res = job_ptr->details->share_res;

	if (!acct_policy_job_runnable_pre_select(job_ptr, false))
		return ESLURM_ACCOUNTING_POLICY;

	part_ptr = job_ptr->part_ptr;

	/* identify partition */
	if (part_ptr == NULL) {
		part_ptr = find_part_record(job_ptr->partition);
		xassert(part_ptr);
		job_ptr->part_ptr = part_ptr;
		error("partition pointer reset for %pJ, part %s",
		      job_ptr, job_ptr->partition);
	}

	/* Quick check to see if this QOS is allowed on this partition. */
	assoc_mgr_lock(&qos_read_lock);
	if (job_ptr->qos_ptr)
		qos_flags = job_ptr->qos_ptr->flags;
	if ((error_code = part_policy_valid_qos(job_ptr->part_ptr,
						job_ptr->qos_ptr,
						job_ptr->user_id, job_ptr)) !=
						SLURM_SUCCESS) {
		assoc_mgr_unlock(&qos_read_lock);
		return ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE;
	}

	/* Quick check to see if this account is allowed on this partition. */
	if ((error_code = part_policy_valid_acct(
		     job_ptr->part_ptr,
		     job_ptr->assoc_ptr ? job_ptr->assoc_ptr->acct : NULL,
		     job_ptr))
	    != SLURM_SUCCESS) {
		assoc_mgr_unlock(&qos_read_lock);
		return ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE;
	}
	assoc_mgr_unlock(&qos_read_lock);

	/* Quick check to see if this group is allowed on this partition. */
	if (!validate_group(job_ptr->part_ptr, job_ptr->user_id)) {
		xfree(job_ptr->state_desc);
		xstrfmtcat(job_ptr->state_desc,
			   "uid %u not in group permitted to use this partition (%s). groups allowed: %s",
			   job_ptr->user_id, job_ptr->part_ptr->name,
			   part_ptr->allow_groups);
		debug2("%s: %s", __func__, job_ptr->state_desc);
		job_ptr->state_reason = WAIT_ACCOUNT;
		last_job_update = now;
		return ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE;
	}

	if (job_ptr->priority == 0) {	/* user/admin hold */
		if (job_ptr->state_reason != FAIL_BAD_CONSTRAINTS
		    && (job_ptr->state_reason != FAIL_BURST_BUFFER_OP)
		    && (job_ptr->state_reason != WAIT_HELD)
		    && (job_ptr->state_reason != WAIT_HELD_USER)
		    && (job_ptr->state_reason != WAIT_MAX_REQUEUE)) {
			job_ptr->state_reason = WAIT_HELD;
		}
		return ESLURM_JOB_HELD;
	}

	bb = bb_g_job_test_stage_in(job_ptr, test_only);
	if (bb != 1) {
		if ((bb == -1) &&
		    (job_ptr->state_reason == FAIL_BURST_BUFFER_OP))
			return ESLURM_BURST_BUFFER_WAIT; /* Fatal BB event */
		xfree(job_ptr->state_desc);
		last_job_update = now;
		if (bb == 0)
			job_ptr->state_reason = WAIT_BURST_BUFFER_STAGING;
		else
			job_ptr->state_reason = WAIT_BURST_BUFFER_RESOURCE;
		return ESLURM_BURST_BUFFER_WAIT;
	}

	if ((job_ptr->details->min_nodes == 0) &&
	    (job_ptr->details->max_nodes == 0)) {
		if (!job_ptr->burst_buffer)
			return ESLURM_INVALID_NODE_COUNT;
		if (!test_only)
			_end_null_job(job_ptr);
		return SLURM_SUCCESS;
	}

	/* build sets of usable nodes based upon their configuration */
	can_reboot = node_features_g_user_update(job_ptr->user_id);
	error_code = _build_node_list(job_ptr, &node_set_ptr, &node_set_size,
				      job_node_select->err_msg,
				      test_only, can_reboot);
	if (error_code)
		return error_code;
	if (node_set_ptr == NULL)	/* Should never be true */
		return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;

	for (i = 0; i < node_set_size; i++)
		_set_sched_weight(node_set_ptr + i);
	qsort(node_set_ptr, node_set_size, sizeof(struct node_set),
	      _sort_node_set);
	_log_node_set(job_ptr, node_set_ptr, node_set_size);

	/* ensure that selected nodes are in these node sets */
	if (job_ptr->details->req_node_bitmap) {
		error_code = _nodes_in_sets(job_ptr->details->req_node_bitmap,
					    node_set_ptr, node_set_size);
		if (error_code) {
			info("No nodes satisfy requirements for %pJ in partition %s",
			     job_ptr, job_ptr->part_ptr->name);
			goto cleanup;
		}
	}

	/* enforce both user's and partition's node limits if the qos
	 * isn't set to override them */
	/* info("req: %u-%u, %u", job_ptr->details->min_nodes, */
	/*    job_ptr->details->max_nodes, part_ptr->max_nodes); */
	error_code = get_node_cnts(job_ptr, qos_flags, part_ptr,
				   &min_nodes, &req_nodes, &max_nodes);
	if ((error_code == ESLURM_ACCOUNTING_POLICY) ||
	    (error_code == ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE))
		goto cleanup;
	else if ((error_code != ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE) &&
		 (error_code != ESLURM_RESERVATION_MAINT)) {
		/* Select resources for the job here */
		job_array_pre_sched(job_ptr);
		if (job_ptr->job_resrcs)
			debug2("%s: calling _get_req_features() for %pJ with not NULL job resources",
			       __func__, job_ptr);
		error_code = _get_req_features(node_set_ptr, node_set_size,
					       &select_bitmap, job_ptr,
					       part_ptr, min_nodes, max_nodes,
					       req_nodes, test_only,
					       &preemptee_job_list, can_reboot,
					       submission);
	}

	/* Set this guess here to give the user tools an idea
	 * of how many nodes Slurm is planning on giving the job.
	 * This needs to be done on success or not.  It means the job
	 * could run on nodes.
	 */
	if (select_bitmap) {
		list_t *gres_list_whole_node = _handle_exclusive_gres(
			job_ptr, select_bitmap, test_only);

		selected_node_cnt = bit_set_count(select_bitmap);
		job_ptr->node_cnt_wag = selected_node_cnt;

		if (gres_list_whole_node) {
			gres_list_pre_set = true;
			gres_list_pre = job_ptr->gres_list_req;
			job_ptr->gres_list_req = gres_list_whole_node;
		}

	} else
		selected_node_cnt = req_nodes;

	if (!test_only && select_bitmap && (max_powered_nodes != NO_VAL)) {
		bitstr_t *tmp = bit_copy(select_bitmap);
		hostlist_t *select = NULL, *need = NULL;
		char *select_str = NULL, *need_str = NULL;
		int32_t count, powerup_count, before_count = 0;

		/* selected and powered down */
		bit_and(tmp, power_down_node_bitmap);
		powerup_count = bit_set_count(tmp);
		if (slurm_conf.debug_flags & DEBUG_FLAG_POWER) {
			select = bitmap2hostlist(select_bitmap);
			select_str = slurm_hostlist_ranged_string_xmalloc(
				select);
			need = bitmap2hostlist(tmp);
			need_str = slurm_hostlist_ranged_string_xmalloc(need);
			before_count = bit_set_count(power_up_node_bitmap);
		}
		bit_or(tmp, power_up_node_bitmap);
		count = bit_set_count(tmp);
		log_flag(POWER, "Need to power up %d nodes (%s) from (%s). powered up count before: %d after: %d",
			 powerup_count, need_str, select_str, before_count,
			 count);

		if ((powerup_count > 0) && (count > max_powered_nodes)) {
			error_code = ESLURM_MAX_POWERED_NODES;
			log_flag(POWER, "%s: Cannot power up more nodes for %pJ due to MaxPoweredUpNodes limit",
				 __func__, job_ptr);
		}

		FREE_NULL_BITMAP(tmp);
		FREE_NULL_HOSTLIST(need);
		FREE_NULL_HOSTLIST(select);
		xfree(select_str);
		xfree(need_str);
	}

	memcpy(tres_req_cnt, job_ptr->tres_req_cnt, sizeof(tres_req_cnt));
	tres_req_cnt[TRES_ARRAY_CPU] =
		(uint64_t)(job_ptr->total_cpus ?
			   job_ptr->total_cpus : job_ptr->details->min_cpus);
	tres_req_cnt[TRES_ARRAY_MEM] = job_get_tres_mem(
					job_ptr->job_resrcs,
					job_ptr->details->pn_min_memory,
					tres_req_cnt[TRES_ARRAY_CPU],
					selected_node_cnt, job_ptr->part_ptr,
					job_ptr->gres_list_req,
					job_ptr->bit_flags & JOB_MEM_SET,
					job_get_sockets_per_node(job_ptr),
					job_ptr->details->num_tasks);
	tres_req_cnt[TRES_ARRAY_NODE] = (uint64_t)selected_node_cnt;

	assoc_mgr_lock(&job_read_locks);
	gres_stepmgr_set_job_tres_cnt(
		job_ptr->gres_list_req,
		selected_node_cnt,
		tres_req_cnt,
		true);

	tres_req_cnt[TRES_ARRAY_BILLING] =
		assoc_mgr_tres_weighted(tres_req_cnt,
		                        job_ptr->part_ptr->billing_weights,
		                        slurm_conf.priority_flags, true);

	if (!test_only && (selected_node_cnt != NO_VAL) &&
	    !acct_policy_job_runnable_post_select(job_ptr, tres_req_cnt, true)) {
		assoc_mgr_unlock(&job_read_locks);
		/* If there was an reason we couldn't schedule before hand we
		 * want to check if an accounting limit was also breached.  If
		 * it was we want to override the other reason so if we are
		 * backfilling we don't reserve resources if we don't have to.
		 */
		free_job_resources(&job_ptr->job_resrcs);
		if (error_code != SLURM_SUCCESS)
			debug2("Replacing scheduling error code for %pJ from '%s' to 'Accounting policy'",
			       job_ptr, slurm_strerror(error_code));
		error_code = ESLURM_ACCOUNTING_POLICY;
		goto cleanup;
	}
	assoc_mgr_unlock(&job_read_locks);

	/* set up the cpu_cnt here so we can decrement it as nodes
	 * free up. total_cpus is set within _get_req_features */
	job_ptr->cpu_cnt = job_ptr->total_cpus;

	if (!test_only && preemptee_job_list
	    && (error_code == SLURM_SUCCESS)) {
		job_details_t *detail_ptr = job_ptr->details;
		time_t now = time(NULL);
		bool kill_pending = true;
		if ((detail_ptr->preempt_start_time != 0) &&
		    (detail_ptr->preempt_start_time >
		     (now - slurm_conf.kill_wait - slurm_conf.msg_timeout))) {
			/* Job preemption may still be in progress,
			 * do not cancel or requeue any more jobs yet */
			kill_pending = false;
		}
		_preempt_jobs(preemptee_job_list, kill_pending, &error_code,
			      job_ptr);
		if ((error_code == ESLURM_NODES_BUSY) && kill_pending) {
			detail_ptr->preempt_start_time = now;
			job_ptr->preempt_in_progress = true;
			if (job_ptr->array_recs)
				job_ptr->array_recs->pend_run_tasks++;
		}
	}
	if (error_code) {
		/* Fatal errors for job here */
		if (error_code == ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE) {
			/* Too many nodes requested */
			debug3("%s: %pJ not runnable with present config",
			       __func__, job_ptr);
			job_ptr->state_reason = WAIT_PART_NODE_LIMIT;
			xfree(job_ptr->state_desc);
			last_job_update = now;

		/* Non-fatal errors for job below */
		} else if (error_code == ESLURM_NODE_NOT_AVAIL) {
			/* Required nodes are down or drained */
			char *node_str = NULL, *unavail_node = NULL;
			bitstr_t *unavail_bitmap;
			debug3("%s: %pJ required nodes not avail",
			       __func__, job_ptr);
			job_ptr->state_reason = WAIT_NODE_NOT_AVAIL;
			xfree(job_ptr->state_desc);
			unavail_bitmap = bit_copy(avail_node_bitmap);
			filter_by_node_owner(job_ptr, unavail_bitmap);
			bit_not(unavail_bitmap);
			bit_and_not(unavail_bitmap, future_node_bitmap);
			bit_and(unavail_bitmap, part_ptr->node_bitmap);
			bit_and_not(unavail_bitmap, up_node_bitmap);
			if (job_ptr->details->req_node_bitmap) {
				bit_and(unavail_bitmap,
					job_ptr->details->req_node_bitmap);
			}
			if (bit_ffs(unavail_bitmap) != -1) {
				unavail_node = bitmap2node_name(unavail_bitmap);
				node_str = unavail_node;
			}
			FREE_NULL_BITMAP(unavail_bitmap);
			if (node_str) {
				xstrfmtcat(job_ptr->state_desc,
					   "ReqNodeNotAvail, "
					   "UnavailableNodes:%s",
					   node_str);
			} else {
				xstrfmtcat(job_ptr->state_desc,
					   "ReqNodeNotAvail, May be reserved "
					   "for other job");
			}
			xfree(unavail_node);
			last_job_update = now;
		} else if (error_code == ESLURM_RESERVATION_MAINT) {
			error_code = ESLURM_RESERVATION_BUSY;	/* All reserved */
			job_ptr->state_reason = WAIT_NODE_NOT_AVAIL;
			xfree(job_ptr->state_desc);
			xstrfmtcat(job_ptr->state_desc,
				   "ReqNodeNotAvail, Reserved for maintenance");
		} else if ((error_code == ESLURM_RESERVATION_NOT_USABLE) ||
			   (error_code == ESLURM_RESERVATION_BUSY)) {
			job_ptr->state_reason = WAIT_RESERVATION;
			xfree(job_ptr->state_desc);
		} else if (error_code == ESLURM_LICENSES_UNAVAILABLE) {
			job_ptr->state_reason = WAIT_LICENSES;
			xfree(job_ptr->state_desc);
		} else if ((job_ptr->state_reason == WAIT_HELD) &&
			   (job_ptr->priority == 0)) {
			/* Held by select plugin due to some failure */
		} else if ((error_code ==
			    ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE) &&
			   job_ptr->extra_constraints) {
			/*
			 * If a job requested extra_constraints, then assume
			 * that the job might be runnable at some point in the
			 * future. FIXME: This is a kludge and this assumption
			 * may be wrong.
			 */
			job_ptr->state_reason = FAIL_CONSTRAINTS;
			xfree(job_ptr->state_desc);
		} else if (error_code == ESLURM_MAX_POWERED_NODES) {
			job_ptr->state_reason = WAIT_MAX_POWERED_NODES;
			xfree(job_ptr->state_desc);
		} else {
			job_ptr->state_reason = WAIT_RESOURCES;
			xfree(job_ptr->state_desc);
		}
		goto cleanup;
	}

	if (test_only) {	/* set if job not highest priority */
		error_code = SLURM_SUCCESS;
		goto cleanup;
	}

	/*
	 * This job may be getting requeued, clear vestigial state information
	 * before over-writing and leaking memory or referencing old GRES or
	 * step data.
	 */
	job_ptr->bit_flags &= ~JOB_KILL_HURRY;
	job_state_unset_flag(job_ptr, JOB_POWER_UP_NODE);
	FREE_NULL_BITMAP(job_ptr->node_bitmap);
	xfree(job_ptr->nodes);
	xfree(job_ptr->sched_nodes);
	job_ptr->exit_code = 0;
	gres_stepmgr_job_clear_alloc(job_ptr->gres_list_req);
	gres_stepmgr_job_clear_alloc(job_ptr->gres_list_req_accum);
	FREE_NULL_LIST(job_ptr->gres_list_alloc);
	if (!job_ptr->step_list)
		job_ptr->step_list = list_create(free_step_record);

	job_ptr->node_bitmap = select_bitmap;
	select_bitmap = NULL;	/* nothing left to free */

	if ((error_code = _get_resv_mpi_ports(job_ptr, &orig_resv_port_cnt,
					      selected_node_cnt, now)))
		goto cleanup;

	/*
	 * we need to have these times set to know when the endtime
	 * is for the job when we place it
	 */
	job_ptr->start_time = job_ptr->time_last_active = now;
	if ((job_ptr->limit_set.time != ADMIN_SET_LIMIT) &&
	    ((job_ptr->time_limit == NO_VAL) ||
	     ((job_ptr->time_limit > part_ptr->max_time) &&
	      !(qos_flags & QOS_FLAG_PART_TIME_LIMIT)))) {
		if (part_ptr->default_time != NO_VAL)
			job_ptr->time_limit = part_ptr->default_time;
		else
			job_ptr->time_limit = part_ptr->max_time;
		job_ptr->limit_set.time = 1;
	}

	job_end_time_reset(job_ptr);

	tmp_job = job_array_post_sched(job_ptr, true);
	if (tmp_job && (tmp_job != job_ptr) && (orig_resv_port_cnt == NO_VAL16))
		tmp_job->resv_port_cnt = orig_resv_port_cnt;

	if (bb_g_job_begin(job_ptr) != SLURM_SUCCESS) {
		/* Leave job queued, something is hosed */
		error_code = ESLURM_INVALID_BURST_BUFFER_REQUEST;
		error("bb_g_job_begin(%pJ): %s",
		      job_ptr, slurm_strerror(error_code));
		job_ptr->start_time = 0;
		job_ptr->time_last_active = 0;
		job_ptr->end_time = 0;
		job_ptr->priority = 0;
		job_ptr->state_reason = WAIT_HELD;
		last_job_update = now;
		goto cleanup;
	}
	if (select_g_job_begin(job_ptr) != SLURM_SUCCESS) {
		/* Leave job queued, something is hosed */
		error("select_g_job_begin(%pJ): %m", job_ptr);

		/* Cancel previously started job */
		(void) bb_g_job_revoke_alloc(job_ptr);

		error_code = ESLURM_NODES_BUSY;
		job_ptr->start_time = 0;
		job_ptr->time_last_active = 0;
		job_ptr->end_time = 0;
		job_ptr->state_reason = WAIT_RESOURCES;
		last_job_update = now;
		goto cleanup;
	}

	/* assign the nodes and stage_in the job */
	job_ptr->state_reason = WAIT_NO_REASON;
	xfree(job_ptr->state_desc);

	if (job_ptr->job_resrcs && job_ptr->job_resrcs->nodes) {
		job_ptr->nodes = xstrdup(job_ptr->job_resrcs->nodes);
	} else {
		error("Select plugin failed to set job resources, nodes");
		/* Do not attempt to allocate the select_bitmap nodes since
		 * select plugin failed to set job resources */

		/* Cancel previously started job */
		(void) bb_g_job_revoke_alloc(job_ptr);

		error_code = ESLURM_NODES_BUSY;
		job_ptr->start_time = 0;
		job_ptr->time_last_active = 0;
		job_ptr->end_time = 0;
		job_ptr->state_reason = WAIT_RESOURCES;
		last_job_update = now;
		goto cleanup;
	}

	job_ptr->db_flags &= ~SLURMDB_JOB_CLEAR_SCHED;
	job_ptr->db_flags |= scheduler_type;

	/* This could be set in the select plugin so we want to keep the flag */
	configuring = IS_JOB_CONFIGURING(job_ptr);

	job_state_set(job_ptr, JOB_RUNNING);
	job_ptr->bit_flags |= JOB_WAS_RUNNING;

	if (select_g_select_nodeinfo_set(job_ptr) != SLURM_SUCCESS) {
		error("select_g_select_nodeinfo_set(%pJ): %m", job_ptr);
		if (!job_ptr->job_resrcs) {
			/* If we don't exit earlier the empty job_resrcs might
			 * be dereferenced later */

			/* Cancel previously started job */
			(void) bb_g_job_revoke_alloc(job_ptr);

			error_code = ESLURM_NODES_BUSY;
			job_ptr->start_time = 0;
			job_ptr->time_last_active = 0;
			job_ptr->end_time = 0;
			job_ptr->state_reason = WAIT_RESOURCES;
			job_state_set(job_ptr, JOB_PENDING);
			last_job_update = now;
			goto cleanup;
		}
	}

	allocate_nodes(job_ptr);
	job_array_start(job_ptr);
	build_node_details(job_ptr, true);
	rebuild_job_part_list(job_ptr);

	if ((job_ptr->mail_type & MAIL_JOB_BEGIN) &&
	    ((job_ptr->mail_type & MAIL_ARRAY_TASKS) ||
	     _first_array_task(job_ptr)))
		mail_job_info(job_ptr, MAIL_JOB_BEGIN);

	slurmctld_diag_stats.jobs_started++;

	/* job_set_alloc_tres has to be done before acct_policy_job_begin */
	job_set_alloc_tres(job_ptr, false);
	acct_policy_job_begin(job_ptr, false);

	resv_replace_update(job_ptr);

	/*
	 * If ran with slurmdbd this is handled out of band in the
	 * job if happening right away.  If the job has already
	 * become eligible and registered in the db then the start message.
	 */
	jobacct_storage_g_job_start(acct_db_conn, job_ptr);

	switch_g_job_start(job_ptr);
	prolog_slurmctld(job_ptr);
	reboot_job_nodes(job_ptr);
	gs_job_start(job_ptr);

	if (bit_overlap_any(job_ptr->node_bitmap, power_down_node_bitmap)) {
		job_state_set_flag(job_ptr, JOB_POWER_UP_NODE);
		if (resume_job_list) {
			uint32_t *tmp = xmalloc(sizeof(uint32_t));
			*tmp = job_ptr->job_id;
			list_append(resume_job_list, tmp);
		}
	}
	if (configuring || IS_JOB_POWER_UP_NODE(job_ptr) ||
	    !bit_super_set(job_ptr->node_bitmap, avail_node_bitmap)) {
		/* This handles nodes explicitly requesting node reboot */
		job_state_set_flag(job_ptr, JOB_CONFIGURING);
	}

	/*
	 * Request asynchronous launch of a prolog for a
	 * non-batch job as long as the node is not configuring for
	 * a reboot first.  Job state could be changed above so we need to
	 * recheck its state to see if it's currently configuring.
	 * PROLOG_FLAG_CONTAIN also turns on PROLOG_FLAG_ALLOC.
	 */
	if (!IS_JOB_CONFIGURING(job_ptr)) {
		if (slurm_conf.prolog_flags & PROLOG_FLAG_ALLOC)
			launch_prolog(job_ptr);
	}

cleanup:
	if (job_ptr->array_recs && job_ptr->array_recs->task_id_bitmap &&
	    !IS_JOB_STARTED(job_ptr) &&
	    (bit_ffs(job_ptr->array_recs->task_id_bitmap) != -1)) {
		job_ptr->array_task_id = NO_VAL;
	}
	FREE_NULL_LIST(preemptee_job_list);
	FREE_NULL_BITMAP(select_bitmap);

	if (node_set_ptr) {
		for (i = 0; i < node_set_size; i++) {
			xfree(node_set_ptr[i].features);
			FREE_NULL_BITMAP(node_set_ptr[i].my_bitmap);
			FREE_NULL_BITMAP(node_set_ptr[i].feature_bits);
		}
		xfree(node_set_ptr);
	}

	if (error_code != SLURM_SUCCESS) {
		if (gres_list_pre_set &&
		    (job_ptr->gres_list_req != gres_list_pre)) {
			FREE_NULL_LIST(job_ptr->gres_list_req);
			job_ptr->gres_list_req = gres_list_pre;
		}

		if (orig_resv_port_cnt == NO_VAL16)
			job_ptr->resv_port_cnt = orig_resv_port_cnt;
		if (job_ptr->resv_ports) {
			resv_port_job_free(job_ptr);
			xfree(job_ptr->resv_ports);
		}
		FREE_NULL_BITMAP(job_ptr->node_bitmap);
	} else
		FREE_NULL_LIST(gres_list_pre);

	/*
	 * Unless the job is allocated resources now, we need to restore the
	 * original whole_node/share_res values since _resolve_shared_status()
	 * might have altered them during evaluation, and we don't want to
	 * propagate the changes for potential subsequent evaluations for the
	 * same job in a different partition with different configuration.
	 *
	 * NOTE: If we ever add an early return between the call to
	 * _get_req_features() and the last return below we should ensure to
	 * ammend the restore logic consequently (probably copy this snippet
	 * before such early return).
	 *
	 * NOTE: We could have moved this snippet right after the call to
	 * _get_req_features(), but we need it here since after the call the
	 * error_code might change.
	 *
	 * NOTE: select_nodes() is the first common caller ancestor of the
	 * different call tree ramifications ending in _resolve_shared_status(),
	 * thus considered the appropriate spot for the save/restore logic.
	 */
	if (test_only || (error_code != SLURM_SUCCESS)) {
		job_ptr->details->whole_node = orig_whole_node;
		job_ptr->details->share_res = orig_share_res;
	}

	return error_code;
}

/*
 * get_node_cnts - determine the number of nodes for the requested job.
 * IN job_ptr - pointer to the job record.
 * IN qos_flags - Flags of the job_ptr's qos.  This is so we don't have to send
 *                in a pointer or lock the qos read lock before calling.
 * IN part_ptr - pointer to the job's partition.
 * OUT min_nodes - The minimum number of nodes for the job.
 * OUT req_nodes - The number of node the select plugin should target.
 * OUT max_nodes - The max number of nodes for the job.
 * RET SLURM_SUCCESS on success, ESLURM code from slurm_errno.h otherwise.
 */
extern int get_node_cnts(job_record_t *job_ptr, uint32_t qos_flags,
			 part_record_t *part_ptr, uint32_t *min_nodes,
			 uint32_t *req_nodes, uint32_t *max_nodes)
{
	int error_code = SLURM_SUCCESS, i;
	uint32_t acct_max_nodes;
	uint32_t wait_reason = 0;

	xassert(job_ptr);
	xassert(part_ptr);

	/* On BlueGene systems don't adjust the min/max node limits
	 * here.  We are working on midplane values. */
	if (qos_flags & QOS_FLAG_PART_MIN_NODE)
		*min_nodes = job_ptr->details->min_nodes;
	else
		*min_nodes = MAX(job_ptr->details->min_nodes,
				 part_ptr->min_nodes);
	if (!job_ptr->details->max_nodes)
		*max_nodes = part_ptr->max_nodes;
	else if (qos_flags & QOS_FLAG_PART_MAX_NODE)
		*max_nodes = job_ptr->details->max_nodes;
	else
		*max_nodes = MIN(job_ptr->details->max_nodes,
				 part_ptr->max_nodes);

	if (job_ptr->details->req_node_bitmap && job_ptr->details->max_nodes) {
		i = bit_set_count(job_ptr->details->req_node_bitmap);
		if (i > job_ptr->details->max_nodes) {
			info("%pJ required node list has more nodes than the job can use (%d > %u)",
			     job_ptr, i, job_ptr->details->max_nodes);
			error_code = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
			goto end_it;
		}
	}

	/* Don't call functions in MIN/MAX it will result in the
	 * function being called multiple times. */
	acct_max_nodes = acct_policy_get_max_nodes(job_ptr, &wait_reason);
	*max_nodes = MIN(*max_nodes, acct_max_nodes);
	*max_nodes = MIN(*max_nodes, 500000);	/* prevent overflows */

	if (!job_ptr->limit_set.tres[TRES_ARRAY_NODE] &&
	    job_ptr->details->max_nodes &&
	    !(job_ptr->bit_flags & USE_MIN_NODES))
		*req_nodes = *max_nodes;
	else
		*req_nodes = *min_nodes;

	if (acct_max_nodes < *min_nodes) {
		error_code = ESLURM_ACCOUNTING_POLICY;
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = wait_reason;
		goto end_it;
	} else if (*max_nodes < *min_nodes) {
		error_code = ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE;
		goto end_it;
	}
end_it:
	return error_code;
}

/*
 * Launch prolog via RPC to slurmd. This is useful when we need to run
 * prolog at allocation stage. Then we ask slurmd to launch the prolog
 * asynchroniously and wait on REQUEST_COMPLETE_PROLOG message from slurmd.
 */
extern void launch_prolog(job_record_t *job_ptr)
{
	prolog_launch_msg_t *prolog_msg_ptr;
	uint16_t protocol_version = job_ptr->start_protocol_ver;
	uint16_t msg_flags = 0;
	agent_arg_t *agent_arg_ptr;
	job_resources_t *job_resrcs_ptr;
	slurm_cred_arg_t cred_arg;
#ifndef HAVE_FRONT_END
	node_record_t *node_ptr;
#endif

	xassert(job_ptr);

#ifdef HAVE_FRONT_END
	/* For a batch job the prolog will be
	 * started synchroniously by slurmd.
	 */
	if (job_ptr->batch_flag)
		return;

	xassert(job_ptr->front_end_ptr);
	if (protocol_version > job_ptr->front_end_ptr->protocol_version)
		protocol_version = job_ptr->front_end_ptr->protocol_version;
#else
	for (int i = 0; (node_ptr = next_node_bitmap(job_ptr->node_bitmap, &i));
	     i++) {
		if (protocol_version > node_ptr->protocol_version)
			protocol_version = node_ptr->protocol_version;
		if (PACK_FANOUT_ADDRS(node_ptr))
			msg_flags |= SLURM_PACK_ADDRS;
	}
#endif

	prolog_msg_ptr = xmalloc(sizeof(prolog_launch_msg_t));

	/* Locks: Write job */
	if ((slurm_conf.prolog_flags & PROLOG_FLAG_ALLOC) &&
	    !(slurm_conf.prolog_flags & PROLOG_FLAG_NOHOLD)) {
		job_ptr->state_reason = WAIT_PROLOG;
#ifndef HAVE_FRONT_END
		FREE_NULL_BITMAP(job_ptr->node_bitmap_pr);
		job_ptr->node_bitmap_pr = bit_copy(job_ptr->node_bitmap);
#endif
	}

	prolog_msg_ptr->job_gres_prep =
		 gres_g_prep_build_env(job_ptr->gres_list_alloc,
				       job_ptr->nodes);
	prolog_msg_ptr->job_id = job_ptr->job_id;
	prolog_msg_ptr->het_job_id = job_ptr->het_job_id;
	prolog_msg_ptr->uid = job_ptr->user_id;
	prolog_msg_ptr->gid = job_ptr->group_id;
	if (!job_ptr->user_name)
		job_ptr->user_name = user_from_job(job_ptr);
	prolog_msg_ptr->user_name_deprecated = xstrdup(job_ptr->user_name);
	prolog_msg_ptr->alias_list = xstrdup(job_ptr->alias_list);
	prolog_msg_ptr->nodes = xstrdup(job_ptr->nodes);
	prolog_msg_ptr->work_dir = xstrdup(job_ptr->details->work_dir);
	prolog_msg_ptr->x11 = job_ptr->details->x11;
	if (prolog_msg_ptr->x11) {
		char *x11_alloc_host = NULL;
		prolog_msg_ptr->x11_magic_cookie =
				xstrdup(job_ptr->details->x11_magic_cookie);
		/*
		 * If resp_host is localhost, send slurmctld's hostname instead.
		 * This gives the compute node a better chance of getting the
		 * connection set up - otherwise it'd try to connect back to
		 * itself by mistake.
		 */
		if (!xstrncmp(job_ptr->resp_host, "127.", 4) ||
		    !xstrcmp(job_ptr->resp_host, "::1")) {
			char hostname[HOST_NAME_MAX];
			if (!gethostname(hostname, sizeof(hostname)))
				x11_alloc_host = xstrdup(hostname);
		}
		if (!x11_alloc_host)
			x11_alloc_host = xstrdup(job_ptr->resp_host);
		prolog_msg_ptr->x11_alloc_host = x11_alloc_host;
		prolog_msg_ptr->x11_alloc_port = job_ptr->other_port;
		prolog_msg_ptr->x11_target = xstrdup(job_ptr->details->x11_target);
		prolog_msg_ptr->x11_target_port = job_ptr->details->x11_target_port;
	}
	prolog_msg_ptr->spank_job_env_size = job_ptr->spank_job_env_size;
	prolog_msg_ptr->spank_job_env = xduparray(job_ptr->spank_job_env_size,
						  job_ptr->spank_job_env);

	if (job_ptr->bit_flags & STEPMGR_ENABLED) {
		node_record_t *bit_node;

		/* Only keep pointers to nodes */
		list_t *job_node_array = list_create(NULL);
		for (int i = 0;
		     (bit_node = next_node_bitmap(job_ptr->node_bitmap, &i));
		     i++) {
			list_append(job_node_array, bit_node);
		}

		/*
		 * Pack while we are in locks so that we don't need to make a
		 * copies of job_ptr and job_node_array since the agent queue
		 * doesn't pack until sending.
		 */
		prolog_msg_ptr->job_ptr_buf = init_buf(BUF_SIZE);
		job_record_pack(job_ptr, slurmctld_tres_cnt,
				prolog_msg_ptr->job_ptr_buf, protocol_version);

		prolog_msg_ptr->job_node_array_buf = init_buf(BUF_SIZE);
		slurm_pack_list(job_node_array, node_record_pack,
				prolog_msg_ptr->job_node_array_buf,
				protocol_version);

		prolog_msg_ptr->part_ptr_buf = init_buf(BUF_SIZE);
		part_record_pack(job_ptr->part_ptr,
				 prolog_msg_ptr->part_ptr_buf,
				 protocol_version);

		FREE_NULL_LIST(job_node_array);
	}

	xassert(job_ptr->job_resrcs);
	job_resrcs_ptr = job_ptr->job_resrcs;
	setup_cred_arg(&cred_arg, job_ptr);
	cred_arg.step_id.job_id = job_ptr->job_id;
	cred_arg.step_id.step_id = SLURM_EXTERN_CONT;
	cred_arg.step_id.step_het_comp = NO_VAL;
	if (job_resrcs_ptr->memory_allocated) {
		slurm_array64_to_value_reps(job_resrcs_ptr->memory_allocated,
					    job_resrcs_ptr->nhosts,
					    &cred_arg.job_mem_alloc,
					    &cred_arg.job_mem_alloc_rep_count,
					    &cred_arg.job_mem_alloc_size);
	}

	cred_arg.step_core_bitmap    = job_resrcs_ptr->core_bitmap;

#ifdef HAVE_FRONT_END
	xassert(job_ptr->batch_host);
	/* override */
	cred_arg.job_hostlist    = job_ptr->batch_host;
	cred_arg.step_hostlist   = job_ptr->batch_host;
#else
	cred_arg.step_hostlist   = job_ptr->job_resrcs->nodes;
#endif

	switch_g_extern_stepinfo(&cred_arg.switch_step, job_ptr);

	prolog_msg_ptr->cred = slurm_cred_create(&cred_arg, false,
						 protocol_version);
	switch_g_free_stepinfo(cred_arg.switch_step);
	xfree(cred_arg.job_mem_alloc);
	xfree(cred_arg.job_mem_alloc_rep_count);

	if (!prolog_msg_ptr->cred) {
		error("%s: slurm_cred_create failure for %pJ, holding job",
		      __func__, job_ptr);
		slurm_free_prolog_launch_msg(prolog_msg_ptr);
		job_mgr_handle_cred_failure(job_ptr);
		return;
	}

	agent_arg_ptr = xmalloc(sizeof(agent_arg_t));
	agent_arg_ptr->retry = 0;
	agent_arg_ptr->protocol_version = protocol_version;
#ifdef HAVE_FRONT_END
	xassert(job_ptr->front_end_ptr->name);
	agent_arg_ptr->hostlist = hostlist_create(job_ptr->front_end_ptr->name);
	agent_arg_ptr->node_count = 1;
#else
	agent_arg_ptr->hostlist = hostlist_create(job_ptr->nodes);
	agent_arg_ptr->node_count = job_ptr->node_cnt;
#endif
	agent_arg_ptr->msg_type = REQUEST_LAUNCH_PROLOG;
	agent_arg_ptr->msg_args = (void *) prolog_msg_ptr;
	agent_arg_ptr->msg_flags = msg_flags;

	/* At least on a Cray we have to treat this as a real step, so
	 * this is where to do it.
	 */
	if (slurm_conf.prolog_flags & PROLOG_FLAG_CONTAIN) {
		step_record_t *step_ptr = build_extern_step(job_ptr);
		if (step_ptr)
			select_g_step_start(step_ptr);
		else
			error("%s: build_extern_step failure for %pJ",
			      __func__, job_ptr);
	}

	job_ptr->prolog_launch_time = time(NULL);

	/* Launch the RPC via agent */
	set_agent_arg_r_uid(agent_arg_ptr, SLURM_AUTH_UID_ANY);
	agent_queue_request(agent_arg_ptr);
}

/*
 * valid_feature_counts - validate a job's features can be satisfied
 *	by the selected nodes (NOTE: does not process MOR or XAND operators)
 * IN job_ptr - job to operate on
 * IN use_active - if set, then only consider nodes with the identified features
 *	active, otherwise use available features
 * IN/OUT node_bitmap - nodes available for use, clear if unusable
 * OUT has_mor - set if MOR/XAND found in feature expression
 * RET SLURM_SUCCESS or error
 */
extern int valid_feature_counts(job_record_t *job_ptr, bool use_active,
				bitstr_t *node_bitmap, bool *has_mor)
{
	job_details_t *detail_ptr = job_ptr->details;
	list_itr_t *job_feat_iter;
	job_feature_t *job_feat_ptr;
	int last_op = FEATURE_OP_AND, last_paren_op = FEATURE_OP_AND;
	int last_paren_cnt = 0;
	bitstr_t *feature_bitmap, *paren_bitmap = NULL;
	bitstr_t *tmp_bitmap, *work_bitmap;
	bool have_count = false, user_update;
	int rc = SLURM_SUCCESS;
	list_t *feature_list = NULL;
	char *features;

	xassert(detail_ptr);
	xassert(node_bitmap);
	xassert(has_mor);

	/*
	 * This is used in two different ways.  1 to pick nodes where
	 * feature_use is set and another to set the predicted start time where
	 * it isn't.
	 */
	if (detail_ptr->features_use) {
		feature_list = detail_ptr->feature_list_use;
		features = detail_ptr->features_use;
	} else {
		feature_list = detail_ptr->feature_list;
		features = detail_ptr->features;
	}

	*has_mor = false;
	if (!feature_list)	/* no constraints */
		return rc;

	user_update = node_features_g_user_update(job_ptr->user_id);
	find_feature_nodes(feature_list, user_update);
	feature_bitmap = bit_copy(node_bitmap);
	work_bitmap = feature_bitmap;
	job_feat_iter = list_iterator_create(feature_list);
	while ((job_feat_ptr = list_next(job_feat_iter))) {
		if (last_paren_cnt < job_feat_ptr->paren) {
			/* Start of expression in parenthesis */
			/*
			 * If this pair of parentheses is inside of brackets,
			 * then this is XAND or MOR. Set last_paren_op to
			 * avoid incorrectly doing bit_and() or bit_or() at the
			 * end of parentheses. This only matters if the
			 * parentheses are the first thing inside of brackets,
			 * in which case last_op is AND or OR depending on what
			 * (if anything) came before the brackets. If the
			 * parentheses are not the first thing inside of
			 * brackets then last_op is XAND or MOR.
			 */
			if (job_feat_ptr->bracket &&
			    (last_op != FEATURE_OP_XAND) &&
			    (last_op != FEATURE_OP_MOR))
				last_paren_op = FEATURE_OP_XAND;
			else
				last_paren_op = last_op;
			last_op = FEATURE_OP_AND;
			if (paren_bitmap) {
				if (job_ptr->job_id) {
					error("%s: %pJ has bad feature expression: %s",
					      __func__, job_ptr,
					      features);
				} else {
					error("%s: Reservation has bad feature expression: %s",
					      __func__, features);
				}
				FREE_NULL_BITMAP(paren_bitmap);
			}
			paren_bitmap = bit_copy(node_bitmap);
			work_bitmap = paren_bitmap;
		}

		if (use_active)
			tmp_bitmap = job_feat_ptr->node_bitmap_active;
		else
			tmp_bitmap = job_feat_ptr->node_bitmap_avail;
		if (tmp_bitmap) {
			/*
			 * Here we need to use the current feature for MOR/AND
			 * not the last_op.  For instance fastio&[xeon|nehalem]
			 * should ignore xeon (in valid_feature_count), but if
			 * would be based on last_op it will see AND operation.
			 * This should only be used when dealing with middle
			 * options, not for the end as done in the last_paren
			 * check below.
			 */
			if ((job_feat_ptr->op_code == FEATURE_OP_MOR) ||
			    (job_feat_ptr->op_code == FEATURE_OP_XAND)) {
				*has_mor = true;
			} else if (last_op == FEATURE_OP_AND) {
				bit_and(work_bitmap, tmp_bitmap);
			} else if (last_op == FEATURE_OP_OR) {
				bit_or(work_bitmap, tmp_bitmap);
			}
		} else {	/* feature not found */
			if (last_op == FEATURE_OP_AND)
				bit_clear_all(work_bitmap);
		}
		if (job_feat_ptr->count)
			have_count = true;

		if (last_paren_cnt > job_feat_ptr->paren) {
			/* End of expression in parenthesis */
			if (last_paren_op == FEATURE_OP_AND) {
				bit_and(feature_bitmap, work_bitmap);
			} else if (last_paren_op == FEATURE_OP_OR) {
				bit_or(feature_bitmap, work_bitmap);
			} else {	/* FEATURE_OP_MOR or FEATURE_OP_XAND */
				*has_mor = true;
			}
			FREE_NULL_BITMAP(paren_bitmap);
			work_bitmap = feature_bitmap;
		}

		last_op = job_feat_ptr->op_code;
		last_paren_cnt = job_feat_ptr->paren;

		if (slurm_conf.debug_flags & DEBUG_FLAG_NODE_FEATURES) {
			char *tmp_f, *tmp_w, *tmp_t;
			tmp_f = bitmap2node_name(feature_bitmap);
			tmp_w = bitmap2node_name(work_bitmap);
			tmp_t = bitmap2node_name(tmp_bitmap);
			log_flag(NODE_FEATURES, "%s: feature:%s feature_bitmap:%s work_bitmap:%s tmp_bitmap:%s count:%u",
				 __func__, job_feat_ptr->name, tmp_f, tmp_w,
				 tmp_t, job_feat_ptr->count);
			xfree(tmp_f);
			xfree(tmp_w);
			xfree(tmp_t);
		}
	}
	list_iterator_destroy(job_feat_iter);
	if (!have_count)
		bit_and(node_bitmap, work_bitmap);
	FREE_NULL_BITMAP(feature_bitmap);
	FREE_NULL_BITMAP(paren_bitmap);

	if (slurm_conf.debug_flags & DEBUG_FLAG_NODE_FEATURES) {
		char *tmp = bitmap2node_name(node_bitmap);
		log_flag(NODE_FEATURES, "%s: NODES:%s HAS_MOR:%c status:%s",
			 __func__, tmp, (*has_mor ? 'T' : 'F'),
			 slurm_strerror(rc));
		xfree(tmp);
	}

	return rc;
}

/*
 * job_req_node_filter - job reqeust node filter.
 *	clear from a bitmap the nodes which can not be used for a job
 *	test memory size, required features, processor count, etc.
 * NOTE: Does not support exclusive OR of features.
 *	It just matches first element of MOR and ignores count.
 * IN job_ptr - pointer to node to be scheduled
 * IN/OUT bitmap - set of nodes being considered for use
 * RET SLURM_SUCCESS or EINVAL if can't filter (exclusive OR of features)
 */
extern int job_req_node_filter(job_record_t *job_ptr,
			       bitstr_t *avail_bitmap, bool test_only)
{
	job_details_t *detail_ptr = job_ptr->details;
	multi_core_data_t *mc_ptr;
	node_record_t *node_ptr;
	bool has_mor = false;

	if (detail_ptr == NULL) {
		error("%s: %pJ has no details",
		      __func__, job_ptr);
		return EINVAL;
	}

	mc_ptr = detail_ptr->mc_ptr;
	for (int i = 0; (node_ptr = next_node_bitmap(avail_bitmap, &i)); i++) {
		if ((detail_ptr->pn_min_cpus  > node_ptr->cpus)   ||
		    ((detail_ptr->pn_min_memory & (~MEM_PER_CPU)) >
		     node_ptr->real_memory) 			    ||
		    ((detail_ptr->pn_min_memory & (MEM_PER_CPU)) &&
		     ((detail_ptr->pn_min_memory & (~MEM_PER_CPU)) *
		      detail_ptr->pn_min_cpus) >
		     node_ptr->real_memory) 			    ||
		    (detail_ptr->pn_min_tmp_disk >
		     node_ptr->tmp_disk)) {
			bit_clear(avail_bitmap, i);
			continue;
		}
		if (mc_ptr &&
		    (((mc_ptr->sockets_per_node > node_ptr->tot_sockets) &&
		      (mc_ptr->sockets_per_node != NO_VAL16)) ||
		     ((mc_ptr->cores_per_socket > node_ptr->cores)   &&
		      (mc_ptr->cores_per_socket != NO_VAL16)) ||
		     ((mc_ptr->threads_per_core > node_ptr->threads) &&
		      (mc_ptr->threads_per_core != NO_VAL16)))) {
			bit_clear(avail_bitmap, i);
			continue;
		}
	}

	return valid_feature_counts(job_ptr, false, avail_bitmap, &has_mor);
}

/*
 * Split the node set record in two
 * IN node_set_ptr - array of node_set records
 * IN config_ptr - configuration info for the nodes being added to a node set
 * IN nset_inx_base - index of original/base node_set to split
 * IN nset_inx - index of the new node_set record
 * IN nset_feature_bits - feature bitmap for the new node_set record
 * IN nset_node_bitmap - bitmap of nodes for the new node_set record
 * IN nset_flags - flags of nodes for the new node_set record
 */
static void _split_node_set(struct node_set *node_set_ptr,
			    config_record_t *config_ptr,
			    int nset_inx_base, int nset_inx,
			    bitstr_t *nset_feature_bits,
			    bitstr_t *nset_node_bitmap, uint32_t nset_flags)
{
	node_set_ptr[nset_inx].cpus_per_node = config_ptr->cpus;
	node_set_ptr[nset_inx].features = xstrdup(config_ptr->feature);
	node_set_ptr[nset_inx].feature_bits = bit_copy(nset_feature_bits);
	node_set_ptr[nset_inx].flags = nset_flags;
	node_set_ptr[nset_inx].real_memory = config_ptr->real_memory;
	node_set_ptr[nset_inx].node_weight =
		node_set_ptr[nset_inx_base].node_weight;

	/*
	 * The bitmap of this new nodeset will contain only the nodes that
	 * are present both in the original bitmap AND in the new bitmap.
	 */
	node_set_ptr[nset_inx].my_bitmap =
		bit_copy(node_set_ptr[nset_inx_base].my_bitmap);
	bit_and(node_set_ptr[nset_inx].my_bitmap, nset_node_bitmap);
	node_set_ptr[nset_inx].node_cnt =
		bit_set_count(node_set_ptr[nset_inx].my_bitmap);

	/* Now we remove these nodes from the original bitmap */
	bit_and_not(node_set_ptr[nset_inx_base].my_bitmap,
		    nset_node_bitmap);
	node_set_ptr[nset_inx_base].node_cnt -= node_set_ptr[nset_inx].node_cnt;
}

static void _apply_extra_constraints(job_record_t *job_ptr,
				     bitstr_t *usable_node_mask)
{
	node_record_t *node_ptr = NULL;

	xassert(job_ptr->extra);
	xassert(job_ptr->extra_constraints);

	for (int i = 0; (node_ptr = next_node_bitmap(usable_node_mask, &i));
	     i++) {
		if (!node_ptr->extra_data) {
			bit_clear(usable_node_mask, i);
			continue;
		}

		if (!extra_constraints_test(job_ptr->extra_constraints,
					    node_ptr->extra_data)) {
			bit_clear(usable_node_mask, i);
			continue;
		}
	}
}

/*
 * _build_node_list - identify which nodes could be allocated to a job
 *	based upon node features, memory, processors, etc. Note that a
 *	bitmap is set to indicate which of the job's features that the
 *	nodes satisfy.
 * IN job_ptr - pointer to node to be scheduled
 * OUT node_set_pptr - list of node sets which could be used for the job
 * OUT node_set_size - number of node_set entries
 * OUT err_msg - error message for job, caller must xfree
 * IN  test_only - true if only testing if job can be started at some point
 * IN can_reboot - if true node can use any available feature,
 *     else job can use only active features
 * RET error code
 */
static int _build_node_list(job_record_t *job_ptr,
			    struct node_set **node_set_pptr,
			    int *node_set_size, char **err_msg, bool test_only,
			    bool can_reboot)
{
	int adj_cpus, i, node_set_inx, node_set_len, node_set_inx_base;
	int power_cnt, rc, qos_cnt;
	struct node_set *node_set_ptr, *prev_node_set_ptr;
	config_record_t *config_ptr;
	part_record_t *part_ptr = job_ptr->part_ptr;
	list_itr_t *config_iterator;
	int total_cores;
	job_details_t *detail_ptr = job_ptr->details;
	bitstr_t *usable_node_mask = NULL;
	multi_core_data_t *mc_ptr = detail_ptr->mc_ptr;
	bitstr_t *tmp_feature;
	bitstr_t *grp_node_bitmap;
	bool has_mor = false;
	bool resv_overlap = false;
	bitstr_t *node_maps[NM_TYPES] = { NULL, NULL, NULL, NULL, NULL, NULL };
	bitstr_t *reboot_bitmap = NULL;

	if (job_ptr->resv_name) {
		/*
		 * Limit node selection to those in selected reservation.
		 * Assume node reboot required since we have not selected the
		 * compute nodes yet.
		 */
		time_t start_res = time(NULL);
		rc = job_test_resv(job_ptr, &start_res, false,
				   &usable_node_mask, NULL, &resv_overlap,
				   true);
		if (rc != SLURM_SUCCESS) {
			job_ptr->state_reason = WAIT_RESERVATION;
			xfree(job_ptr->state_desc);
			if (rc == ESLURM_INVALID_TIME_VALUE)
				return ESLURM_RESERVATION_NOT_USABLE;

			if (rc == ESLURM_NODES_BUSY)
				return ESLURM_NODES_BUSY;

			if (err_msg) {
				xfree(*err_msg);
				*err_msg = xstrdup("Problem using reservation");
			}
			return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
		}
		if ((detail_ptr->req_node_bitmap) &&
		    (!bit_super_set(detail_ptr->req_node_bitmap,
				    usable_node_mask))) {
			job_ptr->state_reason = WAIT_RESERVATION;
			xfree(job_ptr->state_desc);
			FREE_NULL_BITMAP(usable_node_mask);
			if (err_msg) {
				xfree(*err_msg);
				*err_msg = xstrdup("Required nodes outside of "
						   "the reservation");
			}
			return ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
		}
		if (resv_overlap && bit_ffs(usable_node_mask) < 0) {
			job_ptr->state_reason = WAIT_NODE_NOT_AVAIL;
			xfree(job_ptr->state_desc);
			xstrfmtcat(job_ptr->state_desc,
				   "ReqNodeNotAvail, Reserved for maintenance");
			FREE_NULL_BITMAP(usable_node_mask);
			return ESLURM_RESERVATION_BUSY; /* All reserved */
		}
	}

	if (detail_ptr->exc_node_bitmap) {
		if (usable_node_mask) {
			bit_and_not(usable_node_mask, detail_ptr->exc_node_bitmap);
		} else {
			usable_node_mask =
				bit_copy(detail_ptr->exc_node_bitmap);
			bit_not(usable_node_mask);
		}
	} else if (usable_node_mask == NULL) {
		usable_node_mask = node_conf_get_active_bitmap();
	}

	if (!test_only && job_ptr->extra_constraints) {
		_apply_extra_constraints(job_ptr, usable_node_mask);
		if (!bit_set_count(usable_node_mask)) {
			rc = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
			debug("%s: No nodes satisfy %pJ extra constraints in partition %s",
				__func__, job_ptr, job_ptr->part_ptr->name);
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = FAIL_CONSTRAINTS;
			debug2("%s: setting %pJ to \"%s\" (%s)",
			       __func__, job_ptr,
			       job_state_reason_string(job_ptr->state_reason),
			       slurm_strerror(rc));
			FREE_NULL_BITMAP(usable_node_mask);
			return rc;
		}
	}

	if ((rc = valid_feature_counts(job_ptr, false, usable_node_mask,
				       &has_mor))) {
		info("%pJ feature requirements can not be satisfied: %s",
		     job_ptr, slurm_strerror(rc));
		FREE_NULL_BITMAP(usable_node_mask);
		if (err_msg) {
			xfree(*err_msg);
			*err_msg = xstrdup("Node feature requirements can not "
					   "be satisfied");
		}
		return rc;
	}

	if (can_reboot)
		reboot_bitmap = bit_alloc(node_record_count);
	node_set_inx = 0;
	node_set_len = list_count(config_list) * 16 + 1;
	node_set_ptr = xcalloc(node_set_len, sizeof(struct node_set));
	config_iterator = list_iterator_create(config_list);
	while ((config_ptr = list_next(config_iterator))) {
		bool cpus_ok = false, mem_ok = false, disk_ok = false;
		bool job_mc_ok = false, config_filter = false;
		total_cores = config_ptr->tot_sockets * config_ptr->cores;
		adj_cpus = adjust_cpus_nppcu(_get_ntasks_per_core(detail_ptr),
					     detail_ptr->cpus_per_task,
					     total_cores, config_ptr->cpus);
		if (detail_ptr->pn_min_cpus <= adj_cpus)
			cpus_ok = true;
		if ((detail_ptr->pn_min_memory & (~MEM_PER_CPU)) <=
		    config_ptr->real_memory)
			mem_ok = true;
		if (detail_ptr->pn_min_tmp_disk <= config_ptr->tmp_disk)
			disk_ok = true;
		if (!mc_ptr)
			job_mc_ok = true;
		if (mc_ptr &&
		    (((mc_ptr->sockets_per_node <= config_ptr->tot_sockets) ||
		      (mc_ptr->sockets_per_node == NO_VAL16))  &&
		     ((mc_ptr->cores_per_socket <= config_ptr->cores)   ||
		      (mc_ptr->cores_per_socket == NO_VAL16))  &&
		     ((mc_ptr->threads_per_core <= config_ptr->threads) ||
		      (mc_ptr->threads_per_core == NO_VAL16))))
			job_mc_ok = true;
		config_filter = !(cpus_ok && mem_ok && disk_ok && job_mc_ok);
		/*
		 * since nodes can register with more resources than defined
		 * in the configuration, we want to use those higher values
		 * for scheduling, but only as needed (slower)
		 */
		node_set_ptr[node_set_inx].my_bitmap =
			bit_copy(config_ptr->node_bitmap);
		bit_and(node_set_ptr[node_set_inx].my_bitmap,
			part_ptr->node_bitmap);
		if (usable_node_mask) {
			bit_and(node_set_ptr[node_set_inx].my_bitmap,
				usable_node_mask);
		}
		node_set_ptr[node_set_inx].node_cnt =
			bit_set_count(node_set_ptr[node_set_inx].my_bitmap);
		if (node_set_ptr[node_set_inx].node_cnt == 0) {
			debug2("%s: JobId=%u matched 0 nodes (%s) due to job partition or features",
			       __func__, job_ptr->job_id, config_ptr->nodes);
			FREE_NULL_BITMAP(node_set_ptr[node_set_inx].my_bitmap);
			continue;
		}

		if (config_filter) {
			_set_err_msg(cpus_ok, mem_ok, disk_ok, job_mc_ok,
				     err_msg);
			debug2("%s: JobId=%u filtered all nodes (%s): %s",
			       __func__, job_ptr->job_id, config_ptr->nodes,
			       err_msg ? *err_msg : NULL);
			FREE_NULL_BITMAP(node_set_ptr[node_set_inx].my_bitmap);
			continue;
		}

		if (has_mor) {
			tmp_feature = _valid_features(job_ptr, config_ptr,
						      can_reboot, reboot_bitmap);
			if (tmp_feature == NULL) {
				debug2("%s: JobId=%u matched 0 nodes (%s) due to MOR job features",
				       __func__, job_ptr->job_id,
				       config_ptr->nodes);
				FREE_NULL_BITMAP(node_set_ptr[node_set_inx].
						 my_bitmap);
				continue;
			}
		} else {
			/* We've already filtered for AND/OR features */
			tmp_feature = bit_alloc(MAX_FEATURES);
			bit_set(tmp_feature, 0);
		}
		/* NOTE: FREE_NULL_BITMAP(tmp_feature) to avoid memory leak */

		node_set_ptr[node_set_inx].cpus_per_node =
			config_ptr->cpus;
		node_set_ptr[node_set_inx].real_memory =
			config_ptr->real_memory;
		node_set_ptr[node_set_inx].node_weight = config_ptr->weight;
		node_set_ptr[node_set_inx].features =
			xstrdup(config_ptr->feature);
		node_set_ptr[node_set_inx].feature_bits = tmp_feature;
		debug2("found %u usable nodes from config containing %s",
		       node_set_ptr[node_set_inx].node_cnt, config_ptr->nodes);
		prev_node_set_ptr = node_set_ptr + node_set_inx;
		node_set_inx++;
		if (node_set_inx >= node_set_len) {
			error("%s: node_set buffer filled", __func__);
			break;
		}

		/*
		 * If we have a FLEX reservation we will want a nodeset for
		 * those nodes outside the reservation.
		 */
		if (job_ptr->resv_ptr &&
		    (job_ptr->resv_ptr->flags & RESERVE_FLAG_FLEX) &&
		    job_ptr->resv_ptr->node_bitmap &&
		    !bit_super_set(prev_node_set_ptr->my_bitmap,
				   job_ptr->resv_ptr->node_bitmap)) {
			node_maps[IN_FL] =
				bit_copy(job_ptr->resv_ptr->node_bitmap);
			node_maps[OUT_FL] =
				bit_copy(prev_node_set_ptr->my_bitmap);
			bit_and_not(node_maps[OUT_FL], node_maps[IN_FL]);
		}

		/* Identify the nodes that need reboot for use */
		if (!test_only && can_reboot) {
			if (has_mor) {
				node_maps[REBOOT] = bit_copy(reboot_bitmap);
			} else {
				(void) _match_feature(
					job_ptr->details->feature_list_use,
					&node_maps[REBOOT]);
			}
			/* No nodes in set require reboot */
			if (node_maps[REBOOT] &&
			    !bit_overlap_any(prev_node_set_ptr->my_bitmap,
					     node_maps[REBOOT]))
				FREE_NULL_BITMAP(node_maps[REBOOT]);
		}

		/* No nodes to split from this node set */
		if (!node_maps[OUT_FL] && !node_maps[REBOOT])
			continue;

		/* Just need to split these nodes that need reboot */
		if (!node_maps[OUT_FL] && node_maps[REBOOT]) {
			if (bit_super_set(prev_node_set_ptr->my_bitmap,
					  node_maps[REBOOT])) {
				/* All nodes in set require reboot */
				prev_node_set_ptr->flags = NODE_SET_REBOOT;
				goto end_node_set;
			}
			node_set_inx_base = node_set_inx - 1;
			_split_node_set(node_set_ptr, config_ptr,
					node_set_inx_base, node_set_inx,
					tmp_feature, node_maps[REBOOT],
					NODE_SET_REBOOT);
			node_set_inx++;
			goto end_node_set;
		}

		/* Just need to split for these nodes that are outside FLEX */
		if (node_maps[OUT_FL] && !node_maps[REBOOT]) {
			if (bit_super_set(prev_node_set_ptr->my_bitmap,
					  node_maps[OUT_FL])) {
				/* All nodes outside of flex reservation */
				prev_node_set_ptr->flags =NODE_SET_OUTSIDE_FLEX;
				goto end_node_set;
			}
			node_set_inx_base = node_set_inx - 1;
			_split_node_set(node_set_ptr, config_ptr,
					node_set_inx_base, node_set_inx,
					tmp_feature, node_maps[OUT_FL],
					NODE_SET_OUTSIDE_FLEX);
			node_set_inx++;
			goto end_node_set;
		}

		/* We may have to split in several subsets */
		if (node_maps[OUT_FL] && node_maps[REBOOT]) {
			node_maps[IN_FL_RE] = bit_copy(node_maps[IN_FL]);
			bit_and(node_maps[IN_FL_RE], node_maps[REBOOT]);

			node_maps[OUT_FL_RE] = bit_copy(node_maps[OUT_FL]);
			bit_and(node_maps[OUT_FL_RE], node_maps[REBOOT]);

			node_maps[OUT_FL_NO_RE] = bit_copy(node_maps[OUT_FL]);
			bit_and_not(node_maps[OUT_FL_NO_RE],
				    node_maps[REBOOT]);
		}

		/*
		 * All nodes in this set should be avoided. No need to split.
		 * Just set the FLAGS and the Weight.
		 */
		if (bit_super_set(prev_node_set_ptr->my_bitmap,
				  node_maps[IN_FL_RE])) {
			prev_node_set_ptr->flags = NODE_SET_REBOOT;
			goto end_node_set;
		}
		if (bit_super_set(prev_node_set_ptr->my_bitmap,
				  node_maps[OUT_FL_NO_RE])) {
			prev_node_set_ptr->flags = NODE_SET_OUTSIDE_FLEX;
			goto end_node_set;
		}
		if (bit_super_set(prev_node_set_ptr->my_bitmap,
				  node_maps[OUT_FL_RE])) {
			prev_node_set_ptr->flags = (NODE_SET_OUTSIDE_FLEX |
						    NODE_SET_REBOOT);
			goto end_node_set;
		}

		/*
		 * At this point we split the node set record in four,
		 * in this order of priority:
		 *
		 * 1. Inside flex reservation and need to reboot
		 * 2. Outside flex reservation and NO need to reboot
		 * 3. Outside flex reservation and need to reboot
		 * 4. Available now, inside the flex reservation and NO need
		 *    to reboot
		 *
		 * If there are no such reservations or need to reboot,
		 * additional nodesets will not be created.
		 */

		node_set_inx_base = node_set_inx - 1;

		if (node_maps[IN_FL_RE]) {
			_split_node_set(node_set_ptr, config_ptr,
					node_set_inx_base, node_set_inx,
					tmp_feature, node_maps[IN_FL_RE],
					NODE_SET_REBOOT);
			FREE_NULL_BITMAP(node_maps[IN_FL_RE]);
			node_set_inx++;
			if (node_set_inx >= node_set_len) {
				error("%s: node_set buffer filled", __func__);
				break;
			}
		}

		if (node_maps[OUT_FL_NO_RE]) {
			_split_node_set(node_set_ptr, config_ptr,
					node_set_inx_base, node_set_inx,
					tmp_feature, node_maps[OUT_FL_NO_RE],
					(NODE_SET_OUTSIDE_FLEX));
			FREE_NULL_BITMAP(node_maps[OUT_FL_NO_RE]);
			node_set_inx++;
			if (node_set_inx >= node_set_len) {
				error("%s: node_set buffer filled", __func__);
				break;
			}
		}

		if (node_maps[OUT_FL_RE]) {
			_split_node_set(node_set_ptr, config_ptr,
					node_set_inx_base, node_set_inx,
					tmp_feature, node_maps[OUT_FL_RE],
					(NODE_SET_OUTSIDE_FLEX |
					 NODE_SET_REBOOT));
			FREE_NULL_BITMAP(node_maps[OUT_FL_RE]);
			node_set_inx++;
			if (node_set_inx >= node_set_len) {
				error("%s: node_set buffer filled", __func__);
				break;
			}
		}

end_node_set:
		for (i = 0; i < NM_TYPES; i++)
			FREE_NULL_BITMAP(node_maps[i]);
		if (node_set_inx >= node_set_len) {
			error("%s: node_set buffer filled", __func__);
			break;
		}
	}
	list_iterator_destroy(config_iterator);

	/* eliminate any incomplete node_set record */
	xfree(node_set_ptr[node_set_inx].features);
	FREE_NULL_BITMAP(node_set_ptr[node_set_inx].my_bitmap);
	FREE_NULL_BITMAP(node_set_ptr[node_set_inx].feature_bits);
	FREE_NULL_BITMAP(usable_node_mask);

	if (node_set_inx == 0) {
		rc = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;
		info("%s: No nodes satisfy %pJ requirements in partition %s",
		     __func__, job_ptr, job_ptr->part_ptr->name);
		xfree(node_set_ptr);
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = FAIL_BAD_CONSTRAINTS;
		debug2("%s: setting %pJ to \"%s\" (%s)",
		       __func__, job_ptr,
		       job_state_reason_string(job_ptr->state_reason),
		       slurm_strerror(rc));
		FREE_NULL_BITMAP(reboot_bitmap);
		return rc;
	}

	/*
	 * Clear message about any nodes which fail to satisfy specific
	 * job requirements as there are some nodes which can be used
	 */
	if (err_msg)
		xfree(*err_msg);

	/*
	 * If any nodes are powered down, put them into a new node_set
	 * record with a higher scheduling weight. This means we avoid
	 * scheduling jobs on powered down nodes where possible.
	 */
	for (i = (node_set_inx-1); i >= 0; i--) {
		power_cnt = bit_overlap(node_set_ptr[i].my_bitmap,
					power_down_node_bitmap);
		if (power_cnt == 0)
			continue;	/* no nodes powered down */
		if (power_cnt == node_set_ptr[i].node_cnt) {
			node_set_ptr[i].flags = NODE_SET_POWER_DN;
			continue;	/* all nodes powered down */
		}

		/* Some nodes powered down, others up, split record */
		node_set_ptr[node_set_inx].cpus_per_node =
			node_set_ptr[i].cpus_per_node;
		node_set_ptr[node_set_inx].real_memory =
			node_set_ptr[i].real_memory;
		node_set_ptr[node_set_inx].node_cnt = power_cnt;
		node_set_ptr[i].node_cnt -= power_cnt;
		node_set_ptr[node_set_inx].flags = NODE_SET_POWER_DN;
		node_set_ptr[node_set_inx].node_weight =
			node_set_ptr[i].node_weight;
		node_set_ptr[node_set_inx].features =
			xstrdup(node_set_ptr[i].features);
		node_set_ptr[node_set_inx].feature_bits =
			bit_copy(node_set_ptr[i].feature_bits);
		node_set_ptr[node_set_inx].my_bitmap =
			bit_copy(node_set_ptr[i].my_bitmap);
		bit_and(node_set_ptr[node_set_inx].my_bitmap,
			power_down_node_bitmap);
		bit_and_not(node_set_ptr[i].my_bitmap, power_down_node_bitmap);

		node_set_inx++;
		if (node_set_inx >= node_set_len) {
			error("%s: node_set buffer filled", __func__);
			break;
		}
	}

	grp_node_bitmap = _find_grp_node_bitmap(job_ptr);

	if (grp_node_bitmap) {
#if _DEBUG
		char node_bitstr[64];
		bit_fmt(node_bitstr, sizeof(node_bitstr), grp_node_bitmap);
		info("%s:  _find_grp_node_bitmap() grp_node_bitmap:%s", __func__, node_bitstr);
#endif
		for (i = (node_set_inx-1); i >= 0; i--) {
			qos_cnt = bit_overlap(node_set_ptr[i].my_bitmap,
						grp_node_bitmap);
			if (qos_cnt == 0) {
				node_set_ptr[node_set_inx].node_weight += 1;
				continue;	/* no nodes overlap */
			}
			if (qos_cnt == node_set_ptr[i].node_cnt) {
				continue;	/* all nodes overlap */
			}
			/* Some nodes overlap, split record */
			node_set_ptr[node_set_inx].cpus_per_node =
				node_set_ptr[i].cpus_per_node;
			node_set_ptr[node_set_inx].real_memory =
				node_set_ptr[i].real_memory;
			node_set_ptr[node_set_inx].node_cnt = qos_cnt;
			node_set_ptr[i].node_cnt -= qos_cnt;
			node_set_ptr[node_set_inx].node_weight =
				node_set_ptr[i].node_weight;
			node_set_ptr[i].node_weight++;
			node_set_ptr[node_set_inx].flags =
				node_set_ptr[i].flags;
			node_set_ptr[node_set_inx].features =
				xstrdup(node_set_ptr[i].features);
			node_set_ptr[node_set_inx].feature_bits =
				bit_copy(node_set_ptr[i].feature_bits);
			node_set_ptr[node_set_inx].my_bitmap =
				bit_copy(node_set_ptr[i].my_bitmap);
			bit_and(node_set_ptr[node_set_inx].my_bitmap,
				grp_node_bitmap);
			bit_and_not(node_set_ptr[i].my_bitmap, grp_node_bitmap);

			node_set_inx++;
			if (node_set_inx >= node_set_len) {
				error("%s: node_set buffer filled", __func__);
				break;
			}
		}
		FREE_NULL_BITMAP(grp_node_bitmap);
	}
	FREE_NULL_BITMAP(reboot_bitmap);
	*node_set_size = node_set_inx;
	*node_set_pptr = node_set_ptr;
	return SLURM_SUCCESS;
}

/*
 * For a given node_set, set a scheduling weight based upon a combination of
 * node_weight and flags (e.g. try to avoid reboot).
 * 0x20000000000 - Requires boot
 * 0x10000000000 - Outside of flex reservation
 * 0x0########00 - Node weight
 * 0x000000000## - Reserved for cons_tres, favor nodes with co-located CPU/GPU
 */
static void _set_sched_weight(struct node_set *node_set_ptr)
{
	xassert(node_set_ptr);

	node_set_ptr->sched_weight = node_set_ptr->node_weight << 8;
	node_set_ptr->sched_weight |= 0xff;
	if ((node_set_ptr->flags & NODE_SET_REBOOT) ||
	    (node_set_ptr->flags & NODE_SET_POWER_DN))	/* Boot required */
		node_set_ptr->sched_weight |= 0x20000000000;
	if (node_set_ptr->flags & NODE_SET_OUTSIDE_FLEX)
		node_set_ptr->sched_weight |= 0x10000000000;
}

static int _sort_node_set(const void *x, const void *y)
{
	struct node_set *node_set_ptr1 = (struct node_set *) x;
	struct node_set *node_set_ptr2 = (struct node_set *) y;

	xassert(node_set_ptr1);
	xassert(node_set_ptr2);

	if (node_set_ptr1->sched_weight < node_set_ptr2->sched_weight)
		return -1;
	if (node_set_ptr1->sched_weight > node_set_ptr2->sched_weight)
		return 1;
	return 0;
}

static void _log_node_set(job_record_t *job_ptr,
			  struct node_set *node_set_ptr,
			  int node_set_size)
{
	char *node_list, feature_bits[64];
	int i;

	if (get_log_level() < LOG_LEVEL_DEBUG2)
		return;

	debug2("NodeSet for %pJ", job_ptr);
	for (i = 0; i < node_set_size; i++) {
		node_list = bitmap2node_name(node_set_ptr[i].my_bitmap);
		if (node_set_ptr[i].feature_bits) {
			bit_fmt(feature_bits, sizeof(feature_bits),
				node_set_ptr[i].feature_bits);
		} else
			feature_bits[0] = '\0';
		debug2("NodeSet[%d] Nodes:%s NodeWeight:%u Flags:%u FeatureBits:%s SchedWeight:%"PRIu64,
		       i, node_list, node_set_ptr[i].node_weight,
		       node_set_ptr[i].flags, feature_bits,
		       node_set_ptr[i].sched_weight);
		xfree(node_list);
	}
}

static void _set_err_msg(bool cpus_ok, bool mem_ok, bool disk_ok,
			 bool job_mc_ok, char **err_msg)
{
	if (!err_msg)
		return;
	if (!cpus_ok) {
		xfree(*err_msg);
		*err_msg = xstrdup("CPU count per node can not be satisfied");
		return;
	}
	if (!mem_ok) {
		xfree(*err_msg);
		*err_msg = xstrdup("Memory specification can not be satisfied");
		return;
	}
	if (!disk_ok) {
		xfree(*err_msg);
		*err_msg = xstrdup("Temporary disk specification can not be "
				   "satisfied");
		return;
	}
	if (!job_mc_ok) {
		xfree(*err_msg);
		*err_msg = xstrdup("Socket, core and/or thread specification "
				   "can not be satisfied");
		return;
	}
}

/*
 * _nodes_in_sets - Determine if required nodes are included in node_set(s)
 * IN req_bitmap - nodes specifically required by the job
 * IN node_set_ptr - sets of valid nodes
 * IN node_set_size - count of node_set entries
 * RET 0 if in set, otherwise an error code
 */
static int _nodes_in_sets(bitstr_t *req_bitmap,
			  struct node_set * node_set_ptr,
			  int node_set_size)
{
	bitstr_t *scratch_bitmap = NULL;
	int error_code = SLURM_SUCCESS, i;

	for (i=0; i<node_set_size; i++) {
		if (scratch_bitmap)
			bit_or(scratch_bitmap,
			       node_set_ptr[i].my_bitmap);
		else {
			scratch_bitmap =
			    bit_copy(node_set_ptr[i].my_bitmap);
		}
	}

	if ((scratch_bitmap == NULL)
	    || (bit_super_set(req_bitmap, scratch_bitmap) != 1))
		error_code = ESLURM_REQUESTED_NODE_CONFIG_UNAVAILABLE;

	FREE_NULL_BITMAP(scratch_bitmap);
	return error_code;
}

/*
 * build_node_details - sets addresses for allocated nodes
 * IN job_ptr - pointer to a job record
 * IN new_alloc - set if new job allocation, cleared if state recovery
 */
extern void build_node_details(job_record_t *job_ptr, bool new_alloc)
{
	hostlist_t *host_list = NULL;
	node_record_t *node_ptr;
	char *this_node_name;
	int node_inx = 0;

	if ((job_ptr->node_bitmap == NULL) || (job_ptr->nodes == NULL)) {
		/* No nodes allocated, we're done... */
		job_ptr->node_cnt = 0;
		return;
	}

	/* Use hostlist here to ensure ordering of info matches that of srun */
	if ((host_list = hostlist_create(job_ptr->nodes)) == NULL)
		fatal("hostlist_create error for %s: %m", job_ptr->nodes);
	job_ptr->total_nodes = job_ptr->node_cnt = hostlist_count(host_list);

#ifdef HAVE_FRONT_END
	if (new_alloc) {
		/* Find available front-end node and assign it to this job */
		xfree(job_ptr->batch_host);
		job_ptr->front_end_ptr = assign_front_end(job_ptr);
		if (job_ptr->front_end_ptr) {
			job_ptr->batch_host = xstrdup(job_ptr->
						      front_end_ptr->name);
		}
	} else if (job_ptr->batch_host) {
		/* Reset pointer to this job's front-end node */
		job_ptr->front_end_ptr = assign_front_end(job_ptr);
		if (!job_ptr->front_end_ptr)
			xfree(job_ptr->batch_host);
	}
#else
	xfree(job_ptr->batch_host);
#endif

	while ((this_node_name = hostlist_shift(host_list))) {
		if ((node_ptr = find_node_record(this_node_name))) {
			node_inx++;
		} else {
			error("Invalid node %s in %pJ",
			      this_node_name, job_ptr);
		}
		if (!job_ptr->batch_host && !job_ptr->batch_features) {
			/*
			 * Do not select until launch_job() as node features
			 * might be changed by node_features plugin between
			 * allocation time (now) and launch.
			 */
			job_ptr->batch_host = xstrdup(this_node_name);
		}
		free(this_node_name);
	}
	hostlist_destroy(host_list);
	if (job_ptr->node_cnt != node_inx) {
		error("Node count mismatch for %pJ (%u,%u)",
		      job_ptr, job_ptr->node_cnt, node_inx);
	}
}

/*
 * Set "batch_host" for this job based upon it's "batch_features" and
 * "node_bitmap". Selection is performed on a best-effort basis (i.e. if no
 * node satisfies the batch_features specification then pick first node).
 * Execute this AFTER any node feature changes are made by the node_features
 * plugin.
 *
 * If changes are made here, see if changes need to be made in
 * test_job_nodes_ready().
 *
 * Return SLURM_SUCCESS or error code
 */
extern int pick_batch_host(job_record_t *job_ptr)
{
	int i, i_first;
	node_record_t *node_ptr;
	char *tmp, *tok, sep, last_sep = '&';
	node_feature_t *feature_ptr;
	list_itr_t *feature_iter;
	bitstr_t *feature_bitmap;

	if (job_ptr->batch_host)
		return SLURM_SUCCESS;

	if (!job_ptr->node_bitmap) {
		error("%s: %pJ lacks a node_bitmap", __func__, job_ptr);
		return SLURM_ERROR;
	}

	i_first = bit_ffs(job_ptr->node_bitmap);
	if (i_first < 0) {
		error("%s: %pJ allocated no nodes", __func__, job_ptr);
		return SLURM_ERROR;
	}
	if (!job_ptr->batch_features) {
		/* Run batch script on first node of job allocation */
		node_ptr = node_record_table_ptr[i_first];
		job_ptr->batch_host = xstrdup(node_ptr->name);
		return SLURM_SUCCESS;
	}

	feature_bitmap = bit_copy(job_ptr->node_bitmap);
	tmp = xstrdup(job_ptr->batch_features);
	tok = tmp;
	for (i = 0; ; i++) {
		if (tmp[i] == '&')
			sep = '&';
		else if (tmp[i] == '|')
			sep = '|';
		else if (tmp[i] == '\0')
			sep = '\0';
		else
			continue;
		tmp[i] = '\0';

		feature_iter = list_iterator_create(active_feature_list);
		while ((feature_ptr = list_next(feature_iter))) {
			if (xstrcmp(feature_ptr->name, tok))
				continue;
			if (last_sep == '&') {
				bit_and(feature_bitmap,
					feature_ptr->node_bitmap);
			} else {
				bit_or(feature_bitmap,
				       feature_ptr->node_bitmap);
			}
			break;
		}
		list_iterator_destroy(feature_iter);
		if (!feature_ptr)	/* No match */
			bit_clear_all(feature_bitmap);
		if (sep == '\0')
			break;
		tok = tmp + i + 1;
		last_sep = sep;
	}
	xfree(tmp);

	bit_and(feature_bitmap, job_ptr->node_bitmap);
	if ((i = bit_ffs(feature_bitmap)) >= 0)
		node_ptr = node_record_table_ptr[i];
	else
		node_ptr = node_record_table_ptr[i_first];
	job_ptr->batch_host = xstrdup(node_ptr->name);
	FREE_NULL_BITMAP(feature_bitmap);

	return SLURM_SUCCESS;
}

/*
 * _valid_features - Determine if the requested features are satisfied by
 *	the available nodes. This is only used for MOR operators.
 * IN job_ptr - job being scheduled
 * IN config_ptr - node's configuration record
 * IN can_reboot - if true node can use any available feature,
 *	else job can use only active features
 * IN reboot_bitmap - bitmap of nodes requiring reboot for use (updated)
 * RET NULL if request is not satisfied, otherwise a bitmap indicating
 *	which mutually exclusive features are satisfied. For example
 *	_valid_features("[fs1|fs2|fs3|fs4]", "fs3") returns a bitmap with
 *	the third bit set. For another example
 *	_valid_features("[fs1|fs2|fs3|fs4]", "fs1,fs3") returns a bitmap
 *	with the first and third bits set. The function returns a bitmap
 *	with the first bit set if requirements are satisfied without a
 *	mutually exclusive feature list.
 */
static bitstr_t *_valid_features(job_record_t *job_ptr,
				 config_record_t *config_ptr,
				 bool can_reboot, bitstr_t *reboot_bitmap)
{
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *result_node_bitmap = NULL, *paren_node_bitmap = NULL;
	bitstr_t *working_node_bitmap, *active_node_bitmap = NULL;
	bitstr_t *tmp_node_bitmap = NULL;
	list_itr_t *feat_iter;
	job_feature_t *job_feat_ptr;
	int last_op = FEATURE_OP_AND, paren_op = FEATURE_OP_AND;
	int last_paren = 0, position = 0;

	if (details_ptr->feature_list_use == NULL) {	/* no constraints */
		result_node_bitmap = bit_alloc(MAX_FEATURES);
		bit_set(result_node_bitmap, 0);
		return result_node_bitmap;
	}

	feat_iter = list_iterator_create(details_ptr->feature_list_use);
	while ((job_feat_ptr = list_next(feat_iter))) {
		if (job_feat_ptr->paren > last_paren) {
			/* Combine features within parenthesis */
			paren_node_bitmap =
				bit_copy(job_feat_ptr->node_bitmap_avail);
			if (can_reboot)
				active_node_bitmap = bit_copy(paren_node_bitmap);
			last_paren = job_feat_ptr->paren;
			paren_op = job_feat_ptr->op_code;
			/*
			 * If this pair of parentheses is inside of brackets,
			 * then this is XAND or MOR. Set last_op so that the
			 * features in parentheses are considered as XAND or
			 * MOR and are evaluated in the if at the bottom of this
			 * loop. This only matters if the parentheses are the
			 * first thing inside of brackets because last_op is
			 * initialized to AND.
			 */
			if (job_feat_ptr->bracket &&
			    (last_op != FEATURE_OP_XAND) &&
			    (last_op != FEATURE_OP_MOR))
				last_op = FEATURE_OP_XAND;

			while ((job_feat_ptr = list_next(feat_iter))) {
				if ((paren_op == FEATURE_OP_AND) &&
				     can_reboot) {
					bit_and(paren_node_bitmap,
						job_feat_ptr->node_bitmap_avail);
					bit_and(active_node_bitmap,
						job_feat_ptr->node_bitmap_active);
				} else if (paren_op == FEATURE_OP_AND) {
					bit_and(paren_node_bitmap,
						job_feat_ptr->node_bitmap_active);
				} else if ((paren_op == FEATURE_OP_OR) &&
					   can_reboot) {
					bit_or(paren_node_bitmap,
					       job_feat_ptr->node_bitmap_avail);
					bit_or(active_node_bitmap,
					       job_feat_ptr->node_bitmap_active);
				} else if (paren_op == FEATURE_OP_OR) {
					bit_or(paren_node_bitmap,
					       job_feat_ptr->node_bitmap_active);
				} else {
					error("%s: Bad feature expression for %pJ: %s",
					      __func__, job_ptr,
					      details_ptr->features_use);
					break;
				}
				paren_op = job_feat_ptr->op_code;
				if (job_feat_ptr->paren < last_paren) {
					last_paren = job_feat_ptr->paren;
					break;
				}
			}
			working_node_bitmap = paren_node_bitmap;
		} else {
			working_node_bitmap = job_feat_ptr->node_bitmap_avail;
		}

		if (!job_feat_ptr) {
			error("%s: Bad feature expression for %pJ: %s",
			      __func__, job_ptr, details_ptr->features_use);
		}
		if ((job_feat_ptr->op_code == FEATURE_OP_XAND) ||
		    (job_feat_ptr->op_code == FEATURE_OP_MOR)  ||
		    ((job_feat_ptr->op_code != FEATURE_OP_XAND) &&
		     (job_feat_ptr->op_code != FEATURE_OP_MOR)  &&
		     ((last_op == FEATURE_OP_XAND) ||
		      (last_op == FEATURE_OP_MOR)))) {
			if (bit_overlap_any(config_ptr->node_bitmap,
					    working_node_bitmap)) {
				if (!result_node_bitmap)
					result_node_bitmap =
						bit_alloc(MAX_FEATURES);
				bit_set(result_node_bitmap, position);
				if (can_reboot && reboot_bitmap &&
				    active_node_bitmap) {
					tmp_node_bitmap = bit_copy(config_ptr->
								   node_bitmap);
					bit_and_not(tmp_node_bitmap,
						    active_node_bitmap);
					bit_or(reboot_bitmap, tmp_node_bitmap);
					FREE_NULL_BITMAP(tmp_node_bitmap);
				}
			}
			position++;
			last_op = job_feat_ptr->op_code;
		}
		FREE_NULL_BITMAP(active_node_bitmap);
		FREE_NULL_BITMAP(paren_node_bitmap);
	}
	list_iterator_destroy(feat_iter);

#if _DEBUG
{
	char tmp[64];
	if (result_node_bitmap)
		bit_fmt(tmp, sizeof(tmp), result_node_bitmap);
	else
		snprintf(tmp, sizeof(tmp), "NONE");
	info("CONFIG_FEATURE:%s FEATURE_MOR_BITS:%s", config_ptr->feature, tmp);
	if (reboot_bitmap && (bit_ffs(reboot_bitmap) >= 0)) {
		char *reboot_node_str = bitmap2node_name(reboot_bitmap);
		info("REBOOT_NODES:%s", reboot_node_str);
		xfree(reboot_node_str);
	}
}
#endif

	return result_node_bitmap;
}

/*
 * re_kill_job - for a given job, deallocate its nodes for a second time,
 *	basically a cleanup for failed deallocate() calls
 * IN job_ptr - pointer to terminating job (already in some COMPLETING state)
 * globals: node_record_count - number of nodes in the system
 *	node_record_table_ptr - pointer to global node table
 */
extern void re_kill_job(job_record_t *job_ptr)
{
	agent_arg_t *agent_args;
	hostlist_t *kill_hostlist;
	char *host_str = NULL;
	static uint32_t last_job_id = 0;
	node_record_t *node_ptr;
	step_record_t *step_ptr;
	list_itr_t *step_iterator;
#ifdef HAVE_FRONT_END
	front_end_record_t *front_end_ptr;
#endif

	xassert(job_ptr);
	xassert(job_ptr->details);

	kill_hostlist = hostlist_create(NULL);

	agent_args = xmalloc(sizeof(agent_arg_t));
	agent_args->msg_type = REQUEST_TERMINATE_JOB;
	agent_args->hostlist = hostlist_create(NULL);
	agent_args->protocol_version = SLURM_PROTOCOL_VERSION;
	agent_args->retry = 0;

	/* On a Cray system this will start the NHC early so it is
	 * able to gather any information it can from the apparent
	 * unkillable processes.
	 * NOTE: do not do a list_for_each here, that will hold on the list
	 * lock while processing the entire list which could
	 * potentially be needed to lock again in
	 * select_g_step_finish which could potentially call
	 * post_job_step which calls delete_step_record which locks
	 * the list to create a list_iterator on the same list and
	 * could cause deadlock :).
	 */
	step_iterator = list_iterator_create(job_ptr->step_list);
	while ((step_ptr = list_next(step_iterator))) {
		if (step_ptr->step_id.step_id == SLURM_PENDING_STEP)
			continue;
		select_g_step_finish(step_ptr, true);
	}
	list_iterator_destroy(step_iterator);

#ifdef HAVE_FRONT_END
	if (job_ptr->batch_host &&
	    (front_end_ptr = find_front_end_record(job_ptr->batch_host))) {
		agent_args->protocol_version = front_end_ptr->protocol_version;
		if (IS_NODE_DOWN(front_end_ptr) &&
		    job_ptr->node_bitmap_cg) {
			for (int i = 0;
			     (node_ptr =
				next_node_bitmap(job_ptr->node_bitmap_cg, &i));
			     i++) {
				bit_clear(job_ptr->node_bitmap_cg,
					  node_ptr->index);
				job_update_tres_cnt(job_ptr, node_ptr->index);
				if (node_ptr->comp_job_cnt)
					(node_ptr->comp_job_cnt)--;
				if ((job_ptr->node_cnt > 0) &&
				    ((--job_ptr->node_cnt) == 0)) {
					last_node_update = time(NULL);
					cleanup_completing(job_ptr);
					batch_requeue_fini(job_ptr);
					last_node_update = time(NULL);
				}
			}
		} else if (!IS_NODE_NO_RESPOND(front_end_ptr)) {
			(void) hostlist_push_host(kill_hostlist,
						  job_ptr->batch_host);
			hostlist_push_host(agent_args->hostlist,
				      job_ptr->batch_host);
			agent_args->node_count++;
		}
	}
#else
	if (job_ptr->node_bitmap_cg) {
		for (int i = 0;
		     (node_ptr = next_node_bitmap(job_ptr->node_bitmap_cg, &i));
		     i++) {
			if (IS_NODE_DOWN(node_ptr)) {
				/* Consider job already completed */
				bit_clear(job_ptr->node_bitmap_cg,
					  node_ptr->index);
				job_update_tres_cnt(job_ptr, node_ptr->index);
				if (node_ptr->comp_job_cnt)
					(node_ptr->comp_job_cnt)--;
				if ((job_ptr->node_cnt > 0) &&
				    ((--job_ptr->node_cnt) == 0)) {
					cleanup_completing(job_ptr);
					batch_requeue_fini(job_ptr);
					last_node_update = time(NULL);
				}
			} else if (!IS_NODE_NO_RESPOND(node_ptr)) {
				(void)hostlist_push_host(kill_hostlist,
							 node_ptr->name);
				if (agent_args->protocol_version >
				    node_ptr->protocol_version)
					agent_args->protocol_version =
						node_ptr->protocol_version;
				hostlist_push_host(agent_args->hostlist,
						   node_ptr->name);
				agent_args->node_count++;
			}
			if (PACK_FANOUT_ADDRS(node_ptr))
				agent_args->msg_flags |= SLURM_PACK_ADDRS;
		}
	}
#endif

	if (agent_args->node_count == 0) {
		FREE_NULL_HOSTLIST(agent_args->hostlist);
		xfree(agent_args);
		hostlist_destroy(kill_hostlist);
		return;
	}
	hostlist_uniq(kill_hostlist);
	host_str = hostlist_ranged_string_xmalloc(kill_hostlist);
	if (job_ptr->job_id != last_job_id) {
		info("Resending TERMINATE_JOB request %pJ Nodelist=%s",
		     job_ptr, host_str);
	} else {
		debug("Resending TERMINATE_JOB request %pJ Nodelist=%s",
		      job_ptr, host_str);
	}

	xfree(host_str);
	last_job_id = job_ptr->job_id;
	hostlist_destroy(kill_hostlist);
	agent_args->msg_args =
		create_kill_job_msg(job_ptr, agent_args->protocol_version);
	set_agent_arg_r_uid(agent_args, SLURM_AUTH_UID_ANY);
	agent_queue_request(agent_args);
}
