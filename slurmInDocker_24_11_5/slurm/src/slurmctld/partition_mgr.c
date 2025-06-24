/*****************************************************************************\
 *  partition_mgr.c - manage the partition information of slurm
 *	Note: there is a global partition list (part_list) and
 *	time stamp (last_part_update)
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Copyright (C) SchedMD LLC.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette@llnl.gov> et. al.
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

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/common/assoc_mgr.h"
#include "src/common/fd.h"
#include "src/common/hostlist.h"
#include "src/common/list.h"
#include "src/common/pack.h"
#include "src/common/part_record.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/state_save.h"
#include "src/common/uid.h"
#include "src/common/xstring.h"

#include "src/interfaces/burst_buffer.h"
#include "src/interfaces/priority.h"
#include "src/interfaces/select.h"

#include "src/slurmctld/gang.h"
#include "src/slurmctld/groups.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/power_save.h"
#include "src/slurmctld/proc_req.h"
#include "src/slurmctld/read_config.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"

/* No need to change we always pack SLURM_PROTOCOL_VERSION */
#define PART_STATE_VERSION        "PROTOCOL_VERSION"

typedef struct {
	buf_t *buffer;
	uint32_t parts_packed;
	bool privileged;
	uint16_t protocol_version;
	uint16_t show_flags;
	uid_t uid;
	part_record_t **visible_parts;
} _foreach_pack_part_info_t;

/* Global variables */
list_t *part_list = NULL;		/* partition list */
char *default_part_name = NULL;		/* name of default partition */
part_record_t *default_part_loc = NULL;	/* default partition location */
time_t last_part_update = (time_t) 0;	/* time of last update to partition records */
uint16_t part_max_priority = DEF_PART_MAX_PRIORITY;

static int    _dump_part_state(void *x, void *arg);
static void   _list_delete_part(void *part_entry);
static int    _match_part_ptr(void *part_ptr, void *key);
static buf_t *_open_part_state_file(char **state_file);
static void   _unlink_free_nodes(bitstr_t *old_bitmap, part_record_t *part_ptr);

static int _calc_part_tres(void *x, void *arg)
{
	int i, j;
	node_record_t *node_ptr;
	uint64_t *tres_cnt;
	part_record_t *part_ptr = (part_record_t *) x;

	xfree(part_ptr->tres_cnt);
	xfree(part_ptr->tres_fmt_str);
	part_ptr->tres_cnt = xcalloc(slurmctld_tres_cnt, sizeof(uint64_t));
	tres_cnt = part_ptr->tres_cnt;

	/* sum up nodes' tres in the partition. */
	for (i = 0; (node_ptr = next_node_bitmap(part_ptr->node_bitmap, &i));
	     i++) {
		for (j = 0; j < slurmctld_tres_cnt; j++)
			tres_cnt[j] += node_ptr->tres_cnt[j];
	}

	/* Just to be safe, lets do this after the node TRES ;) */
	tres_cnt[TRES_ARRAY_NODE] = part_ptr->total_nodes;

	/* grab the global tres and stick in partition for easy reference. */
	for(i = 0; i < slurmctld_tres_cnt; i++) {
		slurmdb_tres_rec_t *tres_rec = assoc_mgr_tres_array[i];

		if (!xstrcasecmp(tres_rec->type, "bb") ||
		    !xstrcasecmp(tres_rec->type, "license"))
			tres_cnt[i] = tres_rec->count;
	}

	/*
	 * Now figure out the total billing of the partition as the node_ptrs
	 * are configured with the max of all partitions they are in instead of
	 * what is configured on this partition.
	 */
	tres_cnt[TRES_ARRAY_BILLING] = assoc_mgr_tres_weighted(
		tres_cnt, part_ptr->billing_weights,
		slurm_conf.priority_flags, true);

	part_ptr->tres_fmt_str =
		assoc_mgr_make_tres_str_from_array(part_ptr->tres_cnt,
						   TRES_STR_CONVERT_UNITS,
						   true);
	if (part_ptr->qos_ptr) {
		part_ptr->qos_ptr->flags |= QOS_FLAG_PART_QOS;
		assoc_mgr_set_qos_tres_relative_cnt(part_ptr->qos_ptr,
						    part_ptr->tres_cnt);
	}

	return 0;
}

/*
 * Calculate and populate the number of tres' for all partitions.
 */
extern void set_partition_tres(bool assoc_mgr_locked)
{
	assoc_mgr_lock_t locks = {
		.qos = WRITE_LOCK,
		.tres = READ_LOCK,
	};

	xassert(verify_lock(PART_LOCK, WRITE_LOCK));
	xassert(verify_lock(NODE_LOCK, READ_LOCK));

	if (!assoc_mgr_locked)
		assoc_mgr_lock(&locks);
	else {
		xassert(verify_assoc_lock(QOS_LOCK, WRITE_LOCK));
		xassert(verify_assoc_lock(TRES_LOCK, READ_LOCK));
	}

	assoc_mgr_clear_qos_tres_relative_cnt(true);

	list_for_each(part_list, _calc_part_tres, NULL);

	assoc_mgr_set_unset_qos_tres_relative_cnt(true);

	if (!assoc_mgr_locked)
		assoc_mgr_unlock(&locks);
}

/*
 * build_part_bitmap - update the total_cpus, total_nodes, and node_bitmap
 *	for the specified partition, also reset the partition pointers in
 *	the node back to this partition.
 * IN part_ptr - pointer to the partition
 * RET 0 if no error, errno otherwise
 * global: node_record_table_ptr - pointer to global node table
 * NOTE: this does not report nodes defined in more than one partition. this
 *	is checked only upon reading the configuration file, not on an update
 */
extern int build_part_bitmap(part_record_t *part_ptr)
{
	int rc = SLURM_SUCCESS;
	char *this_node_name;
	bitstr_t *old_bitmap;
	node_record_t *node_ptr;
	hostlist_t *host_list, *missing_hostlist = NULL;
	int i;

	part_ptr->total_cpus = 0;
	part_ptr->total_nodes = 0;
	part_ptr->max_cpu_cnt = 0;
	part_ptr->max_core_cnt = 0;

	if (part_ptr->node_bitmap == NULL) {
		part_ptr->node_bitmap = bit_alloc(node_record_count);
		old_bitmap = NULL;
	} else {
		old_bitmap = bit_copy(part_ptr->node_bitmap);
		bit_clear_all(part_ptr->node_bitmap);
	}

	if (!(host_list = nodespec_to_hostlist(part_ptr->orig_nodes, true,
					       &part_ptr->nodesets))) {
		/* Error, restore original bitmap */
		FREE_NULL_BITMAP(part_ptr->node_bitmap);
		part_ptr->node_bitmap = old_bitmap;
		return ESLURM_INVALID_NODE_NAME;
	} else if (!hostlist_count(host_list)) {
		info("%s: No nodes in partition %s", __func__, part_ptr->name);
		/*
		 * Clear "nodes" but leave "orig_nodes" intact.
		 * e.g.
		 * orig_nodes="nodeset1" and all of the nodes in "nodeset1" are
		 * removed. "nodes" should be cleared to show that there are no
		 * nodes in the partition right now. "orig_nodes" needs to stay
		 * intact so that when "nodeset1" nodes come back they are added
		 * to the partition.
		 */
		xfree(part_ptr->nodes);
		_unlink_free_nodes(old_bitmap, part_ptr);
		FREE_NULL_BITMAP(old_bitmap);
		FREE_NULL_HOSTLIST(host_list);
		return 0;
	}

	while ((this_node_name = hostlist_shift(host_list))) {
		node_ptr = find_node_record_no_alias(this_node_name);
		if (node_ptr == NULL) {
			if (!missing_hostlist)
				missing_hostlist =
					hostlist_create(this_node_name);
			else
				hostlist_push_host(missing_hostlist,
						   this_node_name);
			info("%s: invalid node name %s in partition",
			     __func__, this_node_name);
			free(this_node_name);
			rc = ESLURM_INVALID_NODE_NAME;
			continue;
		}
		part_ptr->total_nodes++;
		part_ptr->total_cpus += node_ptr->cpus;
		part_ptr->max_cpu_cnt = MAX(part_ptr->max_cpu_cnt,
					    node_ptr->cpus);
		part_ptr->max_core_cnt = MAX(part_ptr->max_core_cnt,
					     node_ptr->tot_cores);

		for (i = 0; i < node_ptr->part_cnt; i++) {
			if (node_ptr->part_pptr[i] == part_ptr)
				break;
		}
		if (i == node_ptr->part_cnt) { /* Node in new partition */
			node_ptr->part_cnt++;
			xrecalloc(node_ptr->part_pptr, node_ptr->part_cnt,
				  sizeof(part_record_t *));
			node_ptr->part_pptr[node_ptr->part_cnt-1] = part_ptr;
		}
		if (old_bitmap)
			bit_clear(old_bitmap, node_ptr->index);

		bit_set(part_ptr->node_bitmap, node_ptr->index);
		free(this_node_name);
	}
	hostlist_destroy(host_list);

	if ((rc == ESLURM_INVALID_NODE_NAME) && missing_hostlist) {
		/*
		 * Remove missing node from partition nodes so we don't keep
		 * trying to remove them.
		 */
		hostlist_t *hl;
		char *missing_nodes;

		hl = hostlist_create(part_ptr->orig_nodes);
		missing_nodes =
			hostlist_ranged_string_xmalloc(missing_hostlist);
		hostlist_delete(hl, missing_nodes);
		xfree(missing_nodes);
		xfree(part_ptr->orig_nodes);
		part_ptr->orig_nodes = hostlist_ranged_string_xmalloc(hl);
		hostlist_destroy(hl);

	}
	hostlist_destroy(missing_hostlist);
	xfree(part_ptr->nodes);
	part_ptr->nodes = bitmap2node_name(part_ptr->node_bitmap);

	_unlink_free_nodes(old_bitmap, part_ptr);
	last_node_update = time(NULL);
	FREE_NULL_BITMAP(old_bitmap);
	return rc;
}

/* unlink nodes removed from a partition */
static void _unlink_free_nodes(bitstr_t *old_bitmap, part_record_t *part_ptr)
{
	int i, j, k, update_nodes = 0;
	node_record_t *node_ptr;

	if (old_bitmap == NULL)
		return;

	for (i = 0; (node_ptr = next_node_bitmap(old_bitmap, &i)); i++) {
		for (j=0; j<node_ptr->part_cnt; j++) {
			if (node_ptr->part_pptr[j] != part_ptr)
				continue;
			node_ptr->part_cnt--;
			for (k=j; k<node_ptr->part_cnt; k++) {
				node_ptr->part_pptr[k] =
					node_ptr->part_pptr[k+1];
			}
			break;
		}
		update_nodes = 1;
	}

	if (update_nodes)
		last_node_update = time(NULL);
}

/*
 * create_ctld_part_record - create a partition record
 * RET a pointer to the record or NULL if error
 * global: part_list - global partition list
 */
part_record_t *create_ctld_part_record(const char *name)
{
	part_record_t *part_ptr = part_record_create();

	last_part_update = time(NULL);

	part_ptr->name = xstrdup(name);

	list_append(part_list, part_ptr);

	return part_ptr;
}

/* dump_all_part_state - save the state of all partitions to file */
int dump_all_part_state(void)
{
	/* Save high-water mark to avoid buffer growth with copies */
	static uint32_t high_buffer_size = BUF_SIZE;
	/* Locks: Read partition */
	slurmctld_lock_t part_read_lock =
	    { READ_LOCK, NO_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };
	buf_t *buffer = init_buf(high_buffer_size);
	DEF_TIMERS;

	START_TIMER;
	/* write header: time */
	packstr(PART_STATE_VERSION, buffer);
	pack16(SLURM_PROTOCOL_VERSION, buffer);
	pack_time(time(NULL), buffer);

	/* write partition records to buffer */
	lock_slurmctld(part_read_lock);
	list_for_each_ro(part_list, _dump_part_state, buffer);
	unlock_slurmctld(part_read_lock);

	save_buf_to_state("part_state", buffer, &high_buffer_size);

	FREE_NULL_BUFFER(buffer);
	END_TIMER2(__func__);
	return 0;
}

/*
 * _dump_part_state - dump the state of a specific partition to a buffer
 * IN part_ptr - pointer to partition for which information
 *	is requested
 * IN/OUT buffer - location to store data, pointers automatically advanced
 *
 * Note: read by load_all_part_state().
 */
static int _dump_part_state(void *x, void *arg)
{
	part_record_t *part_ptr = (part_record_t *) x;
	buf_t *buffer = (buf_t *) arg;

	xassert(part_ptr);
	xassert(part_ptr->magic == PART_MAGIC);

	if (default_part_loc == part_ptr)
		part_ptr->flags |= PART_FLAG_DEFAULT;
	else
		part_ptr->flags &= (~PART_FLAG_DEFAULT);

	part_record_pack(part_ptr, buffer, SLURM_PROTOCOL_VERSION);

	return 0;
}

/* Open the partition state save file, or backup if necessary.
 * state_file IN - the name of the state save file used
 * RET the file description to read from or error code
 */
static buf_t *_open_part_state_file(char **state_file)
{
	buf_t *buf;

	*state_file = xstrdup(slurm_conf.state_save_location);
	xstrcat(*state_file, "/part_state");
	buf = create_mmap_buf(*state_file);
	if (!buf) {
		error("Could not open partition state file %s: %m",
		      *state_file);
	} else 	/* Success */
		return buf;

	error("NOTE: Trying backup partition state save file. Information may be lost!");
	xstrcat(*state_file, ".old");
	buf = create_mmap_buf(*state_file);
	return buf;
}

/*
 * load_all_part_state - load the partition state from file, recover on
 *	slurmctld restart. execute this after loading the configuration
 *	file data.
 *
 * Note: reads dump from _dump_part_state().
 */
extern int load_all_part_state(uint16_t reconfig_flags)
{
	char *state_file = NULL;
	time_t time;
	part_record_t *part_ptr;
	int error_code = 0, part_cnt = 0;
	buf_t *buffer;
	char *ver_str = NULL;
	uint16_t protocol_version = NO_VAL16;

	xassert(verify_lock(CONF_LOCK, READ_LOCK));

	if (!(reconfig_flags & RECONFIG_KEEP_PART_INFO) &&
	    !(reconfig_flags & RECONFIG_KEEP_PART_STAT)) {
		debug("Restoring partition state from state file disabled");
		return SLURM_SUCCESS;
	}

	/* read the file */
	lock_state_files();
	buffer = _open_part_state_file(&state_file);
	if (!buffer) {
		info("No partition state file (%s) to recover",
		     state_file);
		xfree(state_file);
		unlock_state_files();
		return ENOENT;
	}
	xfree(state_file);
	unlock_state_files();

	safe_unpackstr(&ver_str, buffer);
	debug3("Version string in part_state header is %s", ver_str);
	if (ver_str && !xstrcmp(ver_str, PART_STATE_VERSION))
		safe_unpack16(&protocol_version, buffer);

	if (protocol_version == NO_VAL16) {
		if (!ignore_state_errors)
			fatal("Can not recover partition state, data version incompatible, start with '-i' to ignore this. Warning: using -i will lose the data that can't be recovered.");
		error("**********************************************************");
		error("Can not recover partition state, data version incompatible");
		error("**********************************************************");
		xfree(ver_str);
		FREE_NULL_BUFFER(buffer);
		return EFAULT;
	}
	xfree(ver_str);
	safe_unpack_time(&time, buffer);

	while (remaining_buf(buffer) > 0) {
		part_record_t *part_rec_state = NULL;

		if ((error_code = part_record_unpack(&part_rec_state, buffer,
						     protocol_version)))
			goto unpack_error;

		if ((part_rec_state->flags & PART_FLAG_DEFAULT_CLR)   ||
		    (part_rec_state->flags & PART_FLAG_EXC_USER_CLR)  ||
		    (part_rec_state->flags & PART_FLAG_EXC_TOPO_CLR)  ||
		    (part_rec_state->flags & PART_FLAG_HIDDEN_CLR)    ||
		    (part_rec_state->flags & PART_FLAG_NO_ROOT_CLR)   ||
		    (part_rec_state->flags & PART_FLAG_PDOI_CLR)      ||
		    (part_rec_state->flags & PART_FLAG_ROOT_ONLY_CLR) ||
		    (part_rec_state->flags & PART_FLAG_REQ_RESV_CLR)  ||
		    (part_rec_state->flags & PART_FLAG_LLN_CLR)) {
			error("Invalid data for partition %s: flags=%u",
			      part_rec_state->name, part_rec_state->flags);
			error_code = EINVAL;
		}
		/* validity test as possible */
		if (part_rec_state->state_up > PARTITION_UP) {
			error("Invalid data for partition %s: state_up=%u",
			      part_rec_state->name, part_rec_state->state_up);
			error_code = EINVAL;
		}
		if (error_code) {
			error("No more partition data will be processed from "
			      "the checkpoint file");
			part_record_delete(part_rec_state);
			error_code = EINVAL;
			break;
		}

		/* find record and perform update */
		part_ptr = list_find_first(part_list, &list_find_part,
					   part_rec_state->name);
		if (!part_ptr && (reconfig_flags & RECONFIG_KEEP_PART_INFO)) {
			info("%s: partition %s missing from configuration file, creating",
			     __func__, part_rec_state->name);
			part_ptr = create_ctld_part_record(part_rec_state->name);
		} else if (!part_ptr) {
			info("%s: partition %s removed from configuration file, skipping",
			     __func__, part_rec_state->name);
		}

		/* Handle RECONFIG_KEEP_PART_STAT */
		if (part_ptr) {
			part_cnt++;
			part_ptr->state_up = part_rec_state->state_up;
		}

		if (!(reconfig_flags & RECONFIG_KEEP_PART_INFO)) {
			part_record_delete(part_rec_state);
			continue;
		}

		part_ptr->cpu_bind = part_rec_state->cpu_bind;
		part_ptr->flags = part_rec_state->flags;
		if (part_ptr->flags & PART_FLAG_DEFAULT) {
			xfree(default_part_name);
			default_part_name = xstrdup(part_rec_state->name);
			default_part_loc = part_ptr;
		}
		part_ptr->max_time = part_rec_state->max_time;
		part_ptr->default_time = part_rec_state->default_time;
		part_ptr->max_cpus_per_node = part_rec_state->max_cpus_per_node;
		part_ptr->max_cpus_per_socket =
			part_rec_state->max_cpus_per_socket;
		part_ptr->max_nodes = part_rec_state->max_nodes;
		part_ptr->max_nodes_orig = part_rec_state->max_nodes;
		part_ptr->min_nodes = part_rec_state->min_nodes;
		part_ptr->min_nodes_orig = part_rec_state->min_nodes;
		part_ptr->max_share = part_rec_state->max_share;
		part_ptr->grace_time = part_rec_state->grace_time;
		part_ptr->over_time_limit = part_rec_state->over_time_limit;
		if (part_rec_state->preempt_mode != NO_VAL16)
			part_ptr->preempt_mode = part_rec_state->preempt_mode;
		part_ptr->priority_job_factor =
			part_rec_state->priority_job_factor;
		part_ptr->priority_tier = part_rec_state->priority_tier;
		part_ptr->cr_type = part_rec_state->cr_type;

		xfree(part_ptr->allow_accounts);
		part_ptr->allow_accounts = part_rec_state->allow_accounts;
		part_rec_state->allow_accounts = NULL;

		FREE_NULL_LIST(part_ptr->allow_accts_list);
		part_ptr->allow_accts_list =
			accounts_list_build(part_ptr->allow_accounts, false);

		xfree(part_ptr->allow_groups);
		part_ptr->allow_groups   = part_rec_state->allow_groups;
		part_rec_state->allow_groups = NULL;

		xfree(part_ptr->allow_qos);
		part_ptr->allow_qos = part_rec_state->allow_qos;
		part_rec_state->allow_qos = NULL;
		qos_list_build(part_ptr->allow_qos,
			       &part_ptr->allow_qos_bitstr);

		if (part_rec_state->qos_char) {
			slurmdb_qos_rec_t qos_rec;
			xfree(part_ptr->qos_char);
			part_ptr->qos_char = part_rec_state->qos_char;
			part_rec_state->qos_char = NULL;

			memset(&qos_rec, 0, sizeof(slurmdb_qos_rec_t));
			qos_rec.name = part_ptr->qos_char;
			if (assoc_mgr_fill_in_qos(
				    acct_db_conn, &qos_rec, accounting_enforce,
				    (slurmdb_qos_rec_t **)&part_ptr->qos_ptr, 0)
			    != SLURM_SUCCESS) {
				error("Partition %s has an invalid qos (%s), "
				      "please check your configuration",
				      part_ptr->name, qos_rec.name);
				xfree(part_ptr->qos_char);
			}
		}

		xfree(part_ptr->allow_alloc_nodes);
		part_ptr->allow_alloc_nodes = part_rec_state->allow_alloc_nodes;
		part_rec_state->allow_alloc_nodes = NULL;

		xfree(part_ptr->alternate);
		part_ptr->alternate = part_rec_state->alternate;
		part_rec_state->alternate = NULL;

		xfree(part_ptr->deny_accounts);
		part_ptr->deny_accounts = part_rec_state->deny_accounts;
		part_rec_state->deny_accounts = NULL;
		FREE_NULL_LIST(part_ptr->deny_accts_list);
		part_ptr->deny_accts_list =
			accounts_list_build(part_ptr->deny_accounts, false);

		xfree(part_ptr->deny_qos);
		part_ptr->deny_qos = part_rec_state->deny_qos;
		part_rec_state->deny_qos = NULL;
		qos_list_build(part_ptr->deny_qos, &part_ptr->deny_qos_bitstr);

		/*
		 * Store saved nodelist in orig_nodes. nodes will be regenerated
		 * from orig_nodes.
		 */
		xfree(part_ptr->nodes);
		xfree(part_ptr->orig_nodes);
		part_ptr->orig_nodes = part_rec_state->nodes;
		part_rec_state->nodes = NULL;

		part_record_delete(part_rec_state);
	}

	info("Recovered state of %d partitions", part_cnt);
	FREE_NULL_BUFFER(buffer);
	return error_code;

unpack_error:
	if (!ignore_state_errors)
		fatal("Incomplete partition data checkpoint file, start with '-i' to ignore this. Warning: using -i will lose the data that can't be recovered.");
	error("Incomplete partition data checkpoint file");
	info("Recovered state of %d partitions", part_cnt);
	FREE_NULL_BUFFER(buffer);
	return EFAULT;
}

/*
 * find_part_record - find a record for partition with specified name
 * IN name - name of the desired partition
 * RET pointer to partition or NULL if not found
 */
part_record_t *find_part_record(char *name)
{
	if (!part_list) {
		error("part_list is NULL");
		return NULL;
	}
	return list_find_first(part_list, &list_find_part, name);
}

/*
 * Create a copy of a job's part_list *partition list
 * IN part_list_src - a job's part_list
 * RET copy of part_list_src, must be freed by caller
 */
extern list_t *part_list_copy(list_t *part_list_src)
{
	part_record_t *part_ptr;
	list_itr_t *iter;
	list_t *part_list_dest = NULL;

	if (!part_list_src)
		return part_list_dest;

	part_list_dest = list_create(NULL);
	iter = list_iterator_create(part_list_src);
	while ((part_ptr = list_next(iter))) {
		list_append(part_list_dest, part_ptr);
	}
	list_iterator_destroy(iter);

	return part_list_dest;
}

/*
 * get_part_list - find record for named partition(s)
 * IN name - partition name(s) in a comma separated list
 * OUT part_ptr_list - sorted list of pointers to the partitions or NULL
 * OUT prim_part_ptr - pointer to the primary partition
 * OUT err_part - The first invalid partition name.
 * NOTE: Caller must free the returned list
 * NOTE: Caller must free err_part
 */
extern void get_part_list(char *name, list_t **part_ptr_list,
			  part_record_t **prim_part_ptr, char **err_part)
{
	part_record_t *part_ptr;
	char *token, *last = NULL, *tmp_name;

	*part_ptr_list = NULL;
	*prim_part_ptr = NULL;

	if (name == NULL)
		return;

	tmp_name = xstrdup(name);
	token = strtok_r(tmp_name, ",", &last);
	while (token) {
		part_ptr = list_find_first(part_list, &list_find_part, token);
		if (part_ptr) {
			if (!(*part_ptr_list))
				*part_ptr_list = list_create(NULL);
			if (!list_find_first(*part_ptr_list, &_match_part_ptr,
					     part_ptr))
				list_append(*part_ptr_list, part_ptr);
		} else {
			FREE_NULL_LIST(*part_ptr_list);
			if (err_part) {
				xfree(*err_part);
				*err_part = xstrdup(token);
			}
			break;
		}
		token = strtok_r(NULL, ",", &last);
	}

	if (*part_ptr_list) {
		/*
		 * Return the first part_ptr in the list before sorting. On
		 * state load, the first partition in the list is the running
		 * partition -- for multi-partition jobs. Other times it doesn't
		 * matter what the returned part_ptr is because it will be
		 * modified when scheduling the different job_queue_rec_t's.
		 *
		 * The part_ptr_list always needs to be sorted by priority_tier.
		 */
		*prim_part_ptr = list_peek(*part_ptr_list);
		list_sort(*part_ptr_list, priority_sort_part_tier);
		if (list_count(*part_ptr_list) == 1)
			FREE_NULL_LIST(*part_ptr_list);
	}
	xfree(tmp_name);
}

/*
 * Create a global partition list.
 *
 * This should be called before creating any partition entries.
 */
void init_part_conf(void)
{
	last_part_update = time(NULL);

	if (part_list)		/* delete defunct partitions */
		list_flush(part_list);
	else
		part_list = list_create(_list_delete_part);

	xfree(default_part_name);
	default_part_loc = NULL;
}

/*
 * _list_delete_part - delete an entry from the global partition list,
 *	see common/list.h for documentation
 * global: node_record_count - count of nodes in the system
 *         node_record_table_ptr - pointer to global node table
 */
static void _list_delete_part(void *part_entry)
{
	part_record_t *part_ptr;
	node_record_t *node_ptr;
	int i, j, k;

	part_ptr = (part_record_t *) part_entry;

	xassert(part_ptr->magic == PART_MAGIC);
	part_ptr->magic = ~PART_MAGIC;

	for (i = 0; (node_ptr = next_node(&i)); i++) {
		for (j=0; j<node_ptr->part_cnt; j++) {
			if (node_ptr->part_pptr[j] != part_ptr)
				continue;
			node_ptr->part_cnt--;
			for (k=j; k<node_ptr->part_cnt; k++) {
				node_ptr->part_pptr[k] =
					node_ptr->part_pptr[k+1];
			}
			break;
		}
	}

	part_record_delete(part_ptr);
}

/*
 * list_find_part - find an entry in the partition list, see common/list.h
 *	for documentation
 * IN key - partition name
 * RET 1 if matches key, 0 otherwise
 * global- part_list - the global partition list
 */
int list_find_part(void *x, void *key)
{
	part_record_t *part_ptr = (part_record_t *) x;
	char *part = (char *)key;

	return (!xstrcmp(part_ptr->name, part));
}

/*
 * _match_part_ptr - find an entry in the partition list, see common/list.h
 *	for documentation
 * IN key - partition pointer
 * RET 1 if partition pointer matches, 0 otherwise
 */
static int _match_part_ptr(void *part_ptr, void *key)
{
	if (part_ptr == key)
		return 1;

	return 0;
}

/* partition is visible to the user */
static bool _part_is_visible(part_record_t *part_ptr, uid_t uid)
{
	xassert(verify_lock(PART_LOCK, READ_LOCK));
	xassert(uid != 0);

	if (part_ptr->flags & PART_FLAG_HIDDEN)
		return false;
	if (validate_group(part_ptr, uid) == 0)
		return false;

	return true;
}

typedef struct {
	uid_t uid;
	part_record_t **visible_parts;
} build_visible_parts_arg_t;

static int _build_visible_parts_foreach(void *elem, void *x)
{
	part_record_t *part_ptr = elem;
	build_visible_parts_arg_t *arg = x;

	if (_part_is_visible(part_ptr, arg->uid)) {
		*(arg->visible_parts) = part_ptr;
		arg->visible_parts++;
		if (get_log_level() >= LOG_LEVEL_DEBUG3) {
			char *tmp_str = NULL;
			for (int i = 0; arg->visible_parts[i]; i++)
				xstrfmtcat(tmp_str, "%s%s", tmp_str ? "," : "",
					   arg->visible_parts[i]->name);
			debug3("%s: uid:%u visible_parts:%s",
			       __func__, arg->uid, tmp_str);
			xfree(tmp_str);
		}
	}

	return SLURM_SUCCESS;
}

static int _find_part_qos(void *x, void *arg)
{
	part_record_t *part_ptr = x;

	if (part_ptr->qos_ptr == arg)
		return 1;
	return 0;
}

extern part_record_t **build_visible_parts(uid_t uid, bool skip)
{
	part_record_t **visible_parts_save;
	part_record_t **visible_parts;
	build_visible_parts_arg_t args = {0};

	/*
	 * The array of visible parts isn't used for privileged (i.e. operators)
	 * users or when SHOW_ALL is requested, so no need to create list.
	 */
	if (skip)
		return NULL;

	visible_parts = xcalloc(list_count(part_list) + 1,
				sizeof(part_record_t *));
	args.uid = uid;
	args.visible_parts = visible_parts;

	/*
	 * Save start pointer to start of the list so can point to start
	 * after appending to the list.
	 */
	visible_parts_save = visible_parts;
	list_for_each(part_list, _build_visible_parts_foreach, &args);

	return visible_parts_save;
}

extern int part_not_on_list(part_record_t **parts, part_record_t *x)
{
	for (int i = 0; parts[i]; i++) {
		if (parts[i] == x) {
			debug3("%s: partition: %s on visible part list",
			       __func__, x->name);
			return false;
		} else
			debug3("%s: partition: %s not on visible part list",
			       __func__, x->name);
	}
	return true;
}

static int _pack_part(void *object, void *arg)
{
	part_record_t *part_ptr = object;
	_foreach_pack_part_info_t *pack_info = arg;

	xassert(part_ptr->magic == PART_MAGIC);

	if (!(pack_info->show_flags & SHOW_ALL) &&
	    !pack_info->privileged &&
	    part_not_on_list(pack_info->visible_parts, part_ptr))
		return SLURM_SUCCESS;

	pack_part(part_ptr, pack_info->buffer, pack_info->protocol_version);
	pack_info->parts_packed++;

	return SLURM_SUCCESS;
}

/*
 * pack_all_part - dump all partition information for all partitions in
 *	machine independent form (for network transmission)
 * IN show_flags - partition filtering options
 * IN uid - uid of user making request (for partition filtering)
 * global: part_list - global list of partition records
 * OUT buffer
 * NOTE: change slurm_load_part() in api/part_info.c if data format changes
 */
extern buf_t *pack_all_part(uint16_t show_flags, uid_t uid,
			    uint16_t protocol_version)
{
	int tmp_offset;
	time_t now = time(NULL);
	bool privileged = validate_operator(uid);
	_foreach_pack_part_info_t pack_info = {
		.buffer = init_buf(BUF_SIZE),
		.parts_packed = 0,
		.privileged = privileged,
		.protocol_version = protocol_version,
		.show_flags = show_flags,
		.uid = uid,
		.visible_parts = build_visible_parts(uid, privileged),
	};

	/* write header: version and time */
	pack32(0, pack_info.buffer);
	pack_time(now, pack_info.buffer);

	list_for_each_ro(part_list, _pack_part, &pack_info);

	/* put the real record count in the message body header */
	tmp_offset = get_buf_offset(pack_info.buffer);
	set_buf_offset(pack_info.buffer, 0);
	pack32(pack_info.parts_packed, pack_info.buffer);
	set_buf_offset(pack_info.buffer, tmp_offset);

	xfree(pack_info.visible_parts);
	return pack_info.buffer;
}


/*
 * pack_part - dump all configuration information about a specific partition
 *	in machine independent form (for network transmission)
 * IN part_ptr - pointer to partition for which information is requested
 * IN/OUT buffer - buffer in which data is placed, pointers automatically
 *	updated
 * global: default_part_loc - pointer to the default partition
 * NOTE: if you make any changes here be sure to make the corresponding changes
 *	to _unpack_partition_info_members() in common/slurm_protocol_pack.c
 */
void pack_part(part_record_t *part_ptr, buf_t *buffer, uint16_t protocol_version)
{
	if (protocol_version >= SLURM_24_05_PROTOCOL_VERSION) {
		if (default_part_loc == part_ptr)
			part_ptr->flags |= PART_FLAG_DEFAULT;
		else
			part_ptr->flags &= (~PART_FLAG_DEFAULT);

		packstr(part_ptr->name, buffer);
		pack32(part_ptr->cpu_bind, buffer);
		pack32(part_ptr->grace_time, buffer);
		pack32(part_ptr->max_time, buffer);
		pack32(part_ptr->default_time, buffer);
		pack32(part_ptr->max_nodes_orig, buffer);
		pack32(part_ptr->min_nodes_orig, buffer);
		pack32(part_ptr->total_nodes, buffer);
		pack32(part_ptr->total_cpus, buffer);
		pack64(part_ptr->def_mem_per_cpu, buffer);
		pack32(part_ptr->max_cpus_per_node, buffer);
		pack32(part_ptr->max_cpus_per_socket, buffer);
		pack64(part_ptr->max_mem_per_cpu, buffer);

		pack32(part_ptr->flags, buffer);
		pack16(part_ptr->max_share, buffer);
		pack16(part_ptr->over_time_limit, buffer);
		pack16(part_ptr->preempt_mode, buffer);
		pack16(part_ptr->priority_job_factor, buffer);
		pack16(part_ptr->priority_tier, buffer);
		pack16(part_ptr->state_up, buffer);
		pack16(part_ptr->cr_type, buffer);
		pack16(part_ptr->resume_timeout, buffer);
		pack16(part_ptr->suspend_timeout, buffer);
		pack32(part_ptr->suspend_time, buffer);

		packstr(part_ptr->allow_accounts, buffer);
		packstr(part_ptr->allow_groups, buffer);
		packstr(part_ptr->allow_alloc_nodes, buffer);
		packstr(part_ptr->allow_qos, buffer);
		packstr(part_ptr->qos_char, buffer);
		packstr(part_ptr->alternate, buffer);
		packstr(part_ptr->deny_accounts, buffer);
		packstr(part_ptr->deny_qos, buffer);
		packstr(part_ptr->nodes, buffer);
		packstr(part_ptr->nodesets, buffer);
		pack_bit_str_hex(part_ptr->node_bitmap, buffer);
		packstr(part_ptr->billing_weights_str, buffer);
		packstr(part_ptr->tres_fmt_str, buffer);
		(void)slurm_pack_list(part_ptr->job_defaults_list,
				      job_defaults_pack, buffer,
				      protocol_version);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		uint16_t tmp_uint16;
		if (default_part_loc == part_ptr)
			part_ptr->flags |= PART_FLAG_DEFAULT;
		else
			part_ptr->flags &= (~PART_FLAG_DEFAULT);

		packstr(part_ptr->name, buffer);
		pack32(part_ptr->cpu_bind, buffer);
		pack32(part_ptr->grace_time, buffer);
		pack32(part_ptr->max_time, buffer);
		pack32(part_ptr->default_time, buffer);
		pack32(part_ptr->max_nodes_orig, buffer);
		pack32(part_ptr->min_nodes_orig, buffer);
		pack32(part_ptr->total_nodes, buffer);
		pack32(part_ptr->total_cpus, buffer);
		pack64(part_ptr->def_mem_per_cpu, buffer);
		pack32(part_ptr->max_cpus_per_node, buffer);
		pack32(part_ptr->max_cpus_per_socket, buffer);
		pack64(part_ptr->max_mem_per_cpu, buffer);

		tmp_uint16 = part_ptr->flags;
		pack16(tmp_uint16, buffer);
		pack16(part_ptr->max_share, buffer);
		pack16(part_ptr->over_time_limit, buffer);
		pack16(part_ptr->preempt_mode, buffer);
		pack16(part_ptr->priority_job_factor, buffer);
		pack16(part_ptr->priority_tier, buffer);
		pack16(part_ptr->state_up, buffer);
		pack16(part_ptr->cr_type, buffer);
		pack16(part_ptr->resume_timeout, buffer);
		pack16(part_ptr->suspend_timeout, buffer);
		pack32(part_ptr->suspend_time, buffer);

		packstr(part_ptr->allow_accounts, buffer);
		packstr(part_ptr->allow_groups, buffer);
		packstr(part_ptr->allow_alloc_nodes, buffer);
		packstr(part_ptr->allow_qos, buffer);
		packstr(part_ptr->qos_char, buffer);
		packstr(part_ptr->alternate, buffer);
		packstr(part_ptr->deny_accounts, buffer);
		packstr(part_ptr->deny_qos, buffer);
		packstr(part_ptr->nodes, buffer);
		packstr(part_ptr->nodesets, buffer);
		pack_bit_str_hex(part_ptr->node_bitmap, buffer);
		packstr(part_ptr->billing_weights_str, buffer);
		packstr(part_ptr->tres_fmt_str, buffer);
		(void)slurm_pack_list(part_ptr->job_defaults_list,
				      job_defaults_pack, buffer,
				      protocol_version);
	} else {
		error("%s: protocol_version %hu not supported",
		      __func__, protocol_version);
	}
}

/*
 * Process string and set partition fields to appropriate values if valid
 *
 * IN billing_weights_str - suggested billing weights
 * IN part_ptr - pointer to partition
 * IN fail - whether the inner function should fatal if the string is invalid.
 * RET return SLURM_ERROR on error, SLURM_SUCCESS otherwise.
 */
extern int set_partition_billing_weights(char *billing_weights_str,
					 part_record_t *part_ptr, bool fail)
{
	double *tmp = NULL;

	if (!billing_weights_str || *billing_weights_str == '\0') {
		/* Clear the weights */
		xfree(part_ptr->billing_weights_str);
		xfree(part_ptr->billing_weights);
	} else {
		if (!(tmp = slurm_get_tres_weight_array(billing_weights_str,
							slurmctld_tres_cnt,
							fail)))
		    return SLURM_ERROR;

		xfree(part_ptr->billing_weights_str);
		xfree(part_ptr->billing_weights);
		part_ptr->billing_weights_str =
			xstrdup(billing_weights_str);
		part_ptr->billing_weights = tmp;
	}

	return SLURM_SUCCESS;
}

/*
 * update_part - create or update a partition's configuration data
 * IN part_desc - description of partition changes
 * IN create_flag - create a new partition
 * RET 0 or an error code
 * global: part_list - list of partition entries
 *	last_part_update - update time of partition records
 */
extern int update_part(update_part_msg_t * part_desc, bool create_flag)
{
	int error_code;
	part_record_t *part_ptr;

	if (part_desc->name == NULL) {
		info("%s: invalid partition name, NULL", __func__);
		return ESLURM_INVALID_PARTITION_NAME;
	}

	error_code = SLURM_SUCCESS;
	part_ptr = list_find_first(part_list, &list_find_part,
				   part_desc->name);

	if (create_flag) {
		if (part_ptr) {
			verbose("%s: Duplicate partition name for create (%s)",
				__func__, part_desc->name);
			return ESLURM_INVALID_PARTITION_NAME;
		}
		info("%s: partition %s being created", __func__,
		     part_desc->name);
		part_ptr = create_ctld_part_record(part_desc->name);
	} else {
		if (!part_ptr) {
			verbose("%s: Update for partition not found (%s)",
				__func__, part_desc->name);
			return ESLURM_INVALID_PARTITION_NAME;
		}
	}

	last_part_update = time(NULL);

	if (part_desc->billing_weights_str &&
	    set_partition_billing_weights(part_desc->billing_weights_str,
					  part_ptr, false)) {
		error_code = ESLURM_INVALID_TRES_BILLING_WEIGHTS;
		goto fini;
	}
	if (part_desc->cpu_bind) {
		char tmp_str[128];
		slurm_sprint_cpu_bind_type(tmp_str, part_desc->cpu_bind);
		info("%s: setting CpuBind to %s for partition %s", __func__,
		     tmp_str, part_desc->name);
		if (part_desc->cpu_bind == CPU_BIND_OFF)
			part_ptr->cpu_bind = 0;
		else
			part_ptr->cpu_bind = part_desc->cpu_bind;
	}

	if (part_desc->max_cpus_per_node != NO_VAL) {
		info("%s: setting MaxCPUsPerNode to %u for partition %s",
		     __func__, part_desc->max_cpus_per_node, part_desc->name);
		part_ptr->max_cpus_per_node = part_desc->max_cpus_per_node;
	}

	if (part_desc->max_cpus_per_socket != NO_VAL) {
		info("%s: setting MaxCPUsPerSocket to %u for partition %s",
		     __func__, part_desc->max_cpus_per_socket, part_desc->name);
		part_ptr->max_cpus_per_socket = part_desc->max_cpus_per_socket;
	}

	if (part_desc->max_time != NO_VAL) {
		info("%s: setting max_time to %u for partition %s", __func__,
		     part_desc->max_time, part_desc->name);
		part_ptr->max_time = part_desc->max_time;
	}

	if ((part_desc->default_time != NO_VAL) &&
	    (part_desc->default_time > part_ptr->max_time)) {
		info("%s: DefaultTime would exceed MaxTime for partition %s",
		     __func__, part_desc->name);
	} else if (part_desc->default_time != NO_VAL) {
		info("%s: setting default_time to %u for partition %s",
		     __func__, part_desc->default_time, part_desc->name);
		part_ptr->default_time = part_desc->default_time;
	}

	if (part_desc->max_nodes != NO_VAL) {
		info("%s: setting max_nodes to %u for partition %s", __func__,
		     part_desc->max_nodes, part_desc->name);
		part_ptr->max_nodes      = part_desc->max_nodes;
		part_ptr->max_nodes_orig = part_desc->max_nodes;
	}

	if (part_desc->min_nodes != NO_VAL) {
		info("%s: setting min_nodes to %u for partition %s", __func__,
		     part_desc->min_nodes, part_desc->name);
		part_ptr->min_nodes      = part_desc->min_nodes;
		part_ptr->min_nodes_orig = part_desc->min_nodes;
	}

	if (part_desc->grace_time != NO_VAL) {
		info("%s: setting grace_time to %u for partition %s", __func__,
		     part_desc->grace_time, part_desc->name);
		part_ptr->grace_time = part_desc->grace_time;
	}

	if (part_desc->flags & PART_FLAG_HIDDEN) {
		info("%s: setting hidden for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_HIDDEN;
	} else if (part_desc->flags & PART_FLAG_HIDDEN_CLR) {
		info("%s: clearing hidden for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_HIDDEN);
	}

	if (part_desc->flags & PART_FLAG_REQ_RESV) {
		info("%s: setting req_resv for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_REQ_RESV;
	} else if (part_desc->flags & PART_FLAG_REQ_RESV_CLR) {
		info("%s: clearing req_resv for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_REQ_RESV);
	}

	if (part_desc->flags & PART_FLAG_ROOT_ONLY) {
		info("%s: setting root_only for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_ROOT_ONLY;
	} else if (part_desc->flags & PART_FLAG_ROOT_ONLY_CLR) {
		info("%s: clearing root_only for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_ROOT_ONLY);
	}

	if (part_desc->flags & PART_FLAG_NO_ROOT) {
		info("%s: setting no_root for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_NO_ROOT;
	} else if (part_desc->flags & PART_FLAG_NO_ROOT_CLR) {
		info("%s: clearing no_root for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_NO_ROOT);
	}

	if (part_desc->flags & PART_FLAG_PDOI) {
		info("%s: setting PDOI for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_PDOI;
	} else if (part_desc->flags & PART_FLAG_PDOI_CLR) {
		info("%s: clearing PDOI for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_PDOI);
	}

	if (part_desc->flags & PART_FLAG_EXCLUSIVE_USER) {
		info("%s: setting exclusive_user for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_EXCLUSIVE_USER;
	} else if (part_desc->flags & PART_FLAG_EXC_USER_CLR) {
		info("%s: clearing exclusive_user for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_EXCLUSIVE_USER);
	}

	if (part_desc->flags & PART_FLAG_EXCLUSIVE_TOPO) {
		info("%s: setting exclusive_topo for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_EXCLUSIVE_TOPO;
	} else if (part_desc->flags & PART_FLAG_EXC_TOPO_CLR) {
		info("%s: clearing exclusive_topo for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_EXCLUSIVE_TOPO);
	}

	if (part_desc->flags & PART_FLAG_DEFAULT) {
		if (default_part_name == NULL) {
			info("%s: setting default partition to %s", __func__,
			     part_desc->name);
		} else if (xstrcmp(default_part_name, part_desc->name) != 0) {
			info("%s: changing default partition from %s to %s",
			     __func__, default_part_name, part_desc->name);
		}
		xfree(default_part_name);
		default_part_name = xstrdup(part_desc->name);
		default_part_loc = part_ptr;
		part_ptr->flags |= PART_FLAG_DEFAULT;
	} else if ((part_desc->flags & PART_FLAG_DEFAULT_CLR) &&
		   (default_part_loc == part_ptr)) {
		info("%s: clearing default partition from %s", __func__,
		     part_desc->name);
		xfree(default_part_name);
		default_part_loc = NULL;
		part_ptr->flags &= (~PART_FLAG_DEFAULT);
	}

	if (part_desc->flags & PART_FLAG_LLN) {
		info("%s: setting LLN for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags |= PART_FLAG_LLN;
	} else if (part_desc->flags & PART_FLAG_LLN_CLR) {
		info("%s: clearing LLN for partition %s", __func__,
		     part_desc->name);
		part_ptr->flags &= (~PART_FLAG_LLN);
	}

	if (part_desc->state_up != NO_VAL16) {
		info("%s: setting state_up to %u for partition %s", __func__,
		     part_desc->state_up, part_desc->name);
		part_ptr->state_up = part_desc->state_up;
	}

	if (part_desc->max_share != NO_VAL16) {
		uint16_t force = part_desc->max_share & SHARED_FORCE;
		uint16_t val = part_desc->max_share & (~SHARED_FORCE);
		char tmp_str[24];
		if (val == 0)
			snprintf(tmp_str, sizeof(tmp_str), "EXCLUSIVE");
		else if (force)
			snprintf(tmp_str, sizeof(tmp_str), "FORCE:%u", val);
		else if (val == 1)
			snprintf(tmp_str, sizeof(tmp_str), "NO");
		else
			snprintf(tmp_str, sizeof(tmp_str), "YES:%u", val);
		info("%s: setting share to %s for partition %s", __func__,
		     tmp_str, part_desc->name);
		part_ptr->max_share = part_desc->max_share;
	}

	if (part_desc->over_time_limit != NO_VAL16) {
		info("%s: setting OverTimeLimit to %u for partition %s",
		     __func__, part_desc->over_time_limit, part_desc->name);
		part_ptr->over_time_limit = part_desc->over_time_limit;
	}

	if (part_desc->preempt_mode != NO_VAL16) {
		if (!(part_desc->preempt_mode & PREEMPT_MODE_GANG)) {
			uint16_t new_mode;

			new_mode =
				part_desc->preempt_mode & (~PREEMPT_MODE_GANG);

			if (new_mode <= PREEMPT_MODE_CANCEL) {
				/*
				 * This is a valid mode, but if GANG was enabled
				 * at cluster level, always leave it set.
				 */
				if ((part_ptr->preempt_mode != NO_VAL16) &&
				    (part_ptr->preempt_mode &
				     PREEMPT_MODE_GANG))
					new_mode = new_mode | PREEMPT_MODE_GANG;
				info("%s: setting preempt_mode to %s for partition %s",
				     __func__,
				     preempt_mode_string(new_mode),
				     part_desc->name);
				part_ptr->preempt_mode = new_mode;
			} else {
				info("%s: invalid preempt_mode %u", __func__, new_mode);
			}
		} else {
			info("%s: PreemptMode=GANG is a cluster-wide option and cannot be set at partition level",
			      __func__);
		}
	}

	if (part_desc->priority_tier != NO_VAL16) {
		bool changed =
			part_ptr->priority_tier != part_desc->priority_tier;
		info("%s: setting PriorityTier to %u for partition %s",
		     __func__, part_desc->priority_tier, part_desc->name);
		part_ptr->priority_tier = part_desc->priority_tier;

		/* Need to resort all job partition lists */
		if (changed)
			sort_all_jobs_partition_lists();
	}

	if (part_desc->priority_job_factor != NO_VAL16) {
		int redo_prio = 0;
		info("%s: setting PriorityJobFactor to %u for partition %s",
		     __func__, part_desc->priority_job_factor, part_desc->name);

		if ((part_ptr->priority_job_factor == part_max_priority) &&
		    (part_desc->priority_job_factor < part_max_priority))
			redo_prio = 2;
		else if (part_desc->priority_job_factor > part_max_priority)
			redo_prio = 1;

		part_ptr->priority_job_factor = part_desc->priority_job_factor;

		/* If the max_priority changes we need to change all
		 * the normalized priorities of all the other
		 * partitions. If not then just set this partition.
		 */
		if (redo_prio) {
			list_itr_t *itr = list_iterator_create(part_list);
			part_record_t *part2 = NULL;

			if (redo_prio == 2) {
				part_max_priority = DEF_PART_MAX_PRIORITY;
				while ((part2 = list_next(itr))) {
					if (part2->priority_job_factor >
					    part_max_priority)
						part_max_priority =
							part2->priority_job_factor;
				}
				list_iterator_reset(itr);
			} else
				part_max_priority = part_ptr->priority_job_factor;

			while ((part2 = list_next(itr))) {
				part2->norm_priority =
					(double)part2->priority_job_factor /
					(double)part_max_priority;
			}
			list_iterator_destroy(itr);
		} else {
			part_ptr->norm_priority =
				(double)part_ptr->priority_job_factor /
				(double)part_max_priority;
		}
	}

	if (part_desc->allow_accounts != NULL) {
		xfree(part_ptr->allow_accounts);
		if ((xstrcasecmp(part_desc->allow_accounts, "ALL") == 0) ||
		    (part_desc->allow_accounts[0] == '\0')) {
			info("%s: setting AllowAccounts to ALL for partition %s",
			     __func__, part_desc->name);
		} else {
			part_ptr->allow_accounts = part_desc->allow_accounts;
			part_desc->allow_accounts = NULL;
			info("%s: setting AllowAccounts to %s for partition %s",
			     __func__, part_ptr->allow_accounts,
			     part_desc->name);
		}
		FREE_NULL_LIST(part_ptr->allow_accts_list);
		part_ptr->allow_accts_list =
			accounts_list_build(part_ptr->allow_accounts, false);
	}

	if (part_desc->allow_groups != NULL) {
		xfree(part_ptr->allow_groups);
		xfree(part_ptr->allow_uids);
		part_ptr->allow_uids_cnt = 0;
		if ((xstrcasecmp(part_desc->allow_groups, "ALL") == 0) ||
		    (part_desc->allow_groups[0] == '\0')) {
			info("%s: setting allow_groups to ALL for partition %s",
			     __func__, part_desc->name);
		} else {
			part_ptr->allow_groups = part_desc->allow_groups;
			part_desc->allow_groups = NULL;
			info("%s: setting allow_groups to %s for partition %s",
			     __func__, part_ptr->allow_groups, part_desc->name);
			part_ptr->allow_uids =
				get_groups_members(part_ptr->allow_groups,
						   &part_ptr->allow_uids_cnt);
			clear_group_cache();
		}
	}

	if (part_desc->allow_qos != NULL) {
		xfree(part_ptr->allow_qos);
		if ((xstrcasecmp(part_desc->allow_qos, "ALL") == 0) ||
		    (part_desc->allow_qos[0] == '\0')) {
			info("%s: setting AllowQOS to ALL for partition %s",
			     __func__, part_desc->name);
		} else {
			part_ptr->allow_qos = part_desc->allow_qos;
			part_desc->allow_qos = NULL;
			info("%s: setting AllowQOS to %s for partition %s",
			     __func__, part_ptr->allow_qos, part_desc->name);
		}
		qos_list_build(part_ptr->allow_qos,&part_ptr->allow_qos_bitstr);
	}

	if (part_desc->qos_char && part_desc->qos_char[0] == '\0') {
		slurmdb_qos_rec_t *qos = part_ptr->qos_ptr;
		xfree(part_ptr->qos_char);
		part_ptr->qos_ptr = NULL;
		if (qos) {
			assoc_mgr_lock_t locks = {
				.qos = WRITE_LOCK,
				.tres = READ_LOCK,
			};
			assoc_mgr_lock(&locks);
			info("%s: removing partition QOS '%s' from partition '%s'",
			     __func__, qos->name, part_ptr->name);
		        if (!list_find_first(part_list, _find_part_qos, qos))
				qos->flags &= ~QOS_FLAG_PART_QOS;
			/*
			 * Reset relative QOS to the full system cnts
			 */
			if ((qos->flags & QOS_FLAG_RELATIVE) &&
			    !(qos->flags & QOS_FLAG_PART_QOS)) {
				qos->flags &= ~QOS_FLAG_RELATIVE_SET;
				assoc_mgr_set_qos_tres_relative_cnt(qos, NULL);
			}
			assoc_mgr_unlock(&locks);
		}
	} else if (part_desc->qos_char) {
		assoc_mgr_lock_t locks = {
			.qos = WRITE_LOCK,
			.tres = READ_LOCK,
		};
		slurmdb_qos_rec_t qos_rec, *backup_qos_ptr = part_ptr->qos_ptr;
		slurmdb_qos_rec_t *qos = NULL;
		part_record_t *qos_part_ptr = NULL;
		memset(&qos_rec, 0, sizeof(slurmdb_qos_rec_t));
		qos_rec.name = part_desc->qos_char;
		assoc_mgr_lock(&locks);
		if (assoc_mgr_fill_in_qos(
			    acct_db_conn, &qos_rec, accounting_enforce,
			    (slurmdb_qos_rec_t **)&qos, true)
		    != SLURM_SUCCESS) {
			error("%s: invalid qos (%s) given",
			      __func__, qos_rec.name);
			error_code = ESLURM_INVALID_QOS;
			part_ptr->qos_ptr = backup_qos_ptr;
		} else if ((qos->flags & QOS_FLAG_RELATIVE) &&
			   (qos_part_ptr = list_find_first(
				   part_list, _find_part_qos, qos))) {
			error_code = ESLURM_INVALID_RELATIVE_QOS;
			error("%s: %s Partition %s already uses relative QOS (%s).",
			      __func__, slurm_strerror(error_code),
			      qos_part_ptr->name, qos_rec.name);
			part_ptr->qos_ptr = backup_qos_ptr;
		} else {
			info("%s: changing partition QOS from "
			     "%s to %s for partition %s",
			     __func__, part_ptr->qos_char, part_desc->qos_char,
			     part_ptr->name);

			xfree(part_ptr->qos_char);
			part_ptr->qos_char = xstrdup(part_desc->qos_char);
			part_ptr->qos_ptr = qos;
			part_ptr->qos_ptr->flags |= QOS_FLAG_PART_QOS;
			/*
			 * Set a relative QOS' counts based on the partition.
			 */
			if (qos->flags & QOS_FLAG_RELATIVE) {
				qos->flags &= ~QOS_FLAG_RELATIVE_SET;
				assoc_mgr_set_qos_tres_relative_cnt(
					qos, part_ptr->tres_cnt);
			}

			if (backup_qos_ptr) {
				if (!list_find_first(part_list, _find_part_qos,
						     backup_qos_ptr))
					backup_qos_ptr->flags &=
						~QOS_FLAG_PART_QOS;

				/*
				 * Reset relative QOS to the full system cnts
				 */
				if ((backup_qos_ptr->flags &
				     QOS_FLAG_RELATIVE) &&
				    !(backup_qos_ptr->flags &
				      QOS_FLAG_PART_QOS)) {
					backup_qos_ptr->flags &=
						~QOS_FLAG_RELATIVE_SET;
					assoc_mgr_set_qos_tres_relative_cnt(
						backup_qos_ptr, NULL);
				}

			}
		}
		assoc_mgr_unlock(&locks);
	}

	if (part_desc->allow_alloc_nodes != NULL) {
		xfree(part_ptr->allow_alloc_nodes);
		if ((part_desc->allow_alloc_nodes[0] == '\0') ||
		    (xstrcasecmp(part_desc->allow_alloc_nodes, "ALL") == 0)) {
			part_ptr->allow_alloc_nodes = NULL;
			info("%s: setting allow_alloc_nodes to ALL for partition %s",
			     __func__, part_desc->name);
		}
		else {
			part_ptr->allow_alloc_nodes = part_desc->
						      allow_alloc_nodes;
			part_desc->allow_alloc_nodes = NULL;
			info("%s: setting allow_alloc_nodes to %s for partition %s",
			     __func__, part_ptr->allow_alloc_nodes,
			     part_desc->name);
		}
	}
	if (part_desc->alternate != NULL) {
		xfree(part_ptr->alternate);
		if ((xstrcasecmp(part_desc->alternate, "NONE") == 0) ||
		    (part_desc->alternate[0] == '\0'))
			part_ptr->alternate = NULL;
		else
			part_ptr->alternate = xstrdup(part_desc->alternate);
		part_desc->alternate = NULL;
		info("%s: setting alternate to %s for partition %s",
		     __func__, part_ptr->alternate, part_desc->name);
	}

	if (part_desc->def_mem_per_cpu != NO_VAL64) {
		char *key;
		uint32_t value;
		if (part_desc->def_mem_per_cpu & MEM_PER_CPU) {
			key = "DefMemPerCpu";
			value = part_desc->def_mem_per_cpu & (~MEM_PER_CPU);
		} else {
			key = "DefMemPerNode";
			value = part_desc->def_mem_per_cpu;
		}
		info("%s: setting %s to %u for partition %s", __func__,
		     key, value, part_desc->name);
		part_ptr->def_mem_per_cpu = part_desc->def_mem_per_cpu;
	}

	if (part_desc->deny_accounts != NULL) {
		xfree(part_ptr->deny_accounts);
		if (part_desc->deny_accounts[0] == '\0')
			xfree(part_desc->deny_accounts);
		part_ptr->deny_accounts = part_desc->deny_accounts;
		part_desc->deny_accounts = NULL;
		info("%s: setting DenyAccounts to %s for partition %s",
		     __func__, part_ptr->deny_accounts, part_desc->name);
		FREE_NULL_LIST(part_ptr->deny_accts_list);
		part_ptr->deny_accts_list =
			accounts_list_build(part_ptr->deny_accounts, false);
	}
	if (part_desc->allow_accounts && part_desc->deny_accounts) {
		error("%s: Both AllowAccounts and DenyAccounts are defined, DenyAccounts will be ignored",
		      __func__);
	}

	if (part_desc->deny_qos != NULL) {
		xfree(part_ptr->deny_qos);
		if (part_desc->deny_qos[0] == '\0')
			xfree(part_ptr->deny_qos);
		part_ptr->deny_qos = part_desc->deny_qos;
		part_desc->deny_qos = NULL;
		info("%s: setting DenyQOS to %s for partition %s", __func__,
		     part_ptr->deny_qos, part_desc->name);
		qos_list_build(part_ptr->deny_qos, &part_ptr->deny_qos_bitstr);
	}
	if (part_desc->allow_qos && part_desc->deny_qos) {
		error("%s: Both AllowQOS and DenyQOS are defined, DenyQOS will be ignored",
		      __func__);
	}

	if (part_desc->max_mem_per_cpu != NO_VAL64) {
		char *key;
		uint32_t value;
		if (part_desc->max_mem_per_cpu & MEM_PER_CPU) {
			key = "MaxMemPerCpu";
			value = part_desc->max_mem_per_cpu & (~MEM_PER_CPU);
		} else {
			key = "MaxMemPerNode";
			value = part_desc->max_mem_per_cpu;
		}
		info("%s: setting %s to %u for partition %s", __func__,
		     key, value, part_desc->name);
		part_ptr->max_mem_per_cpu = part_desc->max_mem_per_cpu;
	}

	if (part_desc->job_defaults_str) {
		list_t *new_job_def_list = NULL;
		if (part_desc->job_defaults_str[0] == '\0') {
			FREE_NULL_LIST(part_ptr->job_defaults_list);
		} else if (job_defaults_list(part_desc->job_defaults_str,
					     &new_job_def_list)
					!= SLURM_SUCCESS) {
			error("%s: Invalid JobDefaults(%s) given",
			      __func__, part_desc->job_defaults_str);
			error_code = ESLURM_INVALID_JOB_DEFAULTS;
		} else {	/* New list successfully built */
			FREE_NULL_LIST(part_ptr->job_defaults_list);
			part_ptr->job_defaults_list = new_job_def_list;
			info("%s: Setting JobDefaults to %s for partition %s",
			      __func__, part_desc->job_defaults_str,
			      part_desc->name);
		}
	}

	if (part_desc->nodes != NULL) {
		assoc_mgr_lock_t assoc_tres_read_lock = {
			.qos = WRITE_LOCK,
			.tres = READ_LOCK,
		};
		int rc;
		char *backup_orig_nodes = xstrdup(part_ptr->orig_nodes);

		if (part_desc->nodes[0] == '\0')
			part_ptr->nodes = NULL;	/* avoid empty string */
		else if ((part_desc->nodes[0] != '+') &&
			 (part_desc->nodes[0] != '-')) {
			xfree(part_ptr->nodes);
			part_ptr->nodes = xstrdup(part_desc->nodes);
		} else {
			char *p, *tmp, *tok, *save_ptr = NULL;
			hostset_t *hs = hostset_create(part_ptr->nodes);

			p = tmp = xstrdup(part_desc->nodes);
			errno = 0;
			while ((tok = node_conf_nodestr_tokenize(p,
								 &save_ptr))) {
				bool plus_minus = false;
				if (tok[0] == '+') {
					hostset_insert(hs, tok + 1);
					plus_minus = true;
				} else if (tok[0] == '-') {
					hostset_delete(hs, tok + 1);
					plus_minus = true;
				}
				/* errno set in hostset functions */
				if (!plus_minus || errno) {
					error("%s: invalid node name %s",
					      __func__, tok);
					xfree(tmp);
					hostset_destroy(hs);
					error_code = ESLURM_INVALID_NODE_NAME;
					goto fini;
				}
				p = NULL;
			}
			xfree(tmp);
			part_ptr->nodes = hostset_ranged_string_xmalloc(hs);
			hostset_destroy(hs);
		}
		xfree(part_ptr->orig_nodes);
		part_ptr->orig_nodes = xstrdup(part_ptr->nodes);

		if ((rc = build_part_bitmap(part_ptr))) {
			error_code = rc;

			if (!create_flag) {
				/* Restore previous nodes */
				xfree(part_ptr->orig_nodes);
				part_ptr->orig_nodes = backup_orig_nodes;

				/*
				 * build_part_bitmap() is destructive of the
				 * partition record. We need to rebuild the
				 * partition record with the original nodelists
				 * and nodesets.
				 */
				(void) build_part_bitmap(part_ptr);
			} else {
				xfree(backup_orig_nodes);
			}
		} else {
			info("%s: setting nodes to %s for partition %s",
			     __func__, part_ptr->nodes, part_desc->name);
			xfree(backup_orig_nodes);

			update_part_nodes_in_resv(part_ptr);
			power_save_set_timeouts(NULL);

			assoc_mgr_lock(&assoc_tres_read_lock);
			if (part_ptr->qos_ptr)
				part_ptr->qos_ptr->flags &= ~QOS_FLAG_RELATIVE_SET;
			_calc_part_tres(part_ptr, NULL);
			assoc_mgr_unlock(&assoc_tres_read_lock);
		}
	} else if (part_ptr->node_bitmap == NULL) {
		/* Newly created partition needs a bitmap, even if empty */
		part_ptr->node_bitmap = bit_alloc(node_record_count);
	}

fini:
	if (error_code == SLURM_SUCCESS) {
		gs_reconfig();
		select_g_reconfigure();		/* notify select plugin too */
	} else if (create_flag) {
		/* Delete the created partition in case of failure */
		list_delete_all(part_list, &list_find_part, part_desc->name);
	}
	return error_code;
}


/*
 * validate_group - validate that the uid is authorized to access the partition
 * IN part_ptr - pointer to a partition
 * IN run_uid - user to run the job as
 * RET 1 if permitted to run, 0 otherwise
 */
extern int validate_group(part_record_t *part_ptr, uid_t run_uid)
{
	static uid_t last_fail_uid = 0;
	static part_record_t *last_fail_part_ptr = NULL;
	static time_t last_fail_time = 0;
	time_t now;
	gid_t primary_gid;
	char *primary_group = NULL;
	char *groups, *saveptr = NULL, *one_group_name;
	int ret = 0;

	if (part_ptr->allow_groups == NULL)
		return 1;	/* all users allowed */
	if (validate_slurm_user(run_uid))
		return 1;	/* super-user can run anywhere */
	if (!part_ptr->allow_uids_cnt)
		return 0;

	for (int i = 0; i < part_ptr->allow_uids_cnt; i++) {
		if (part_ptr->allow_uids[i] == run_uid)
			return 1;
	}

	/* If this user has failed AllowGroups permission check on this
	 * partition in past 5 seconds, then do not test again for performance
	 * reasons. */
	now = time(NULL);
	if ((run_uid == last_fail_uid) &&
	    (part_ptr == last_fail_part_ptr) &&
	    (difftime(now, last_fail_time) < 5)) {
		return 0;
	}

	/*
	 * The allow_uids list is built from the allow_groups list.  If
	 * user/group enumeration has been disabled, it's possible that the
	 * user's primary group is not returned as a member of a group.
	 * Enumeration is problematic if the user/group database is large
	 * (think university-wide central account database or such), as in such
	 * environments enumeration would load the directory servers a lot, so
	 * the recommendation is to have it disabled (e.g. enumerate=False in
	 * sssd.conf). So check explicitly whether the primary group is allowed
	 * as a final resort.
	 * This should (hopefully) not happen that often.
	 */

	/* First figure out the primary GID.  */
	primary_gid = gid_from_uid(run_uid);

	if (primary_gid == (gid_t) -1) {
		error("%s: Could not find passwd entry for uid %u",
		      __func__, run_uid);
		goto fini;
	}

	/* Then use the primary GID to figure out the name of the
	 * group with that GID.  */

	primary_group = gid_to_string_or_null(primary_gid);

	if (!primary_group) {
		error("%s: Could not find group with gid %u",
		      __func__, primary_gid);
		goto fini;
	}

	/* And finally check the name of the primary group against the
	 * list of allowed group names.  */
	groups = xstrdup(part_ptr->allow_groups);
	one_group_name = strtok_r(groups, ",", &saveptr);
	while (one_group_name) {
		if (!xstrcmp(one_group_name, primary_group)) {
			ret = 1;
			break;
		}
		one_group_name = strtok_r(NULL, ",", &saveptr);
	}
	xfree(groups);
	xfree(primary_group);

	if (ret == 1) {
		debug("UID %u added to AllowGroup %s of partition %s",
		      run_uid, primary_group, part_ptr->name);
		part_ptr->allow_uids =
			xrealloc(part_ptr->allow_uids,
				 (sizeof(uid_t) *
				  (part_ptr->allow_uids_cnt + 1)));
		part_ptr->allow_uids[part_ptr->allow_uids_cnt++] = run_uid;
	}

fini:	if (ret == 0) {
		last_fail_uid = run_uid;
		last_fail_part_ptr = part_ptr;
		last_fail_time = now;
	}
	return ret;
}

/*
 * validate_alloc_node - validate that the allocating node
 * is allowed to use this partition
 * IN part_ptr - pointer to a partition
 * IN alloc_node - allocating node of the request
 * RET 1 if permitted to run, 0 otherwise
 */
extern int validate_alloc_node(part_record_t *part_ptr, char *alloc_node)
{
	int status;

 	if (part_ptr->allow_alloc_nodes == NULL)
 		return 1;	/* all allocating nodes allowed */
 	if (alloc_node == NULL)
		return 0;	/* if no allocating node deny */

	hostlist_t *hl = hostlist_create(part_ptr->allow_alloc_nodes);
 	status=hostlist_find(hl,alloc_node);
 	hostlist_destroy(hl);

 	if (status == -1)
		status = 0;
 	else
		status = 1;

 	return status;
}

static int _update_part_uid_access_list(void *x, void *arg)
{
	part_record_t *part_ptr = (part_record_t *)x;
	int *updated = (int *)arg;
	int i = 0;
	uid_t *tmp_uids = part_ptr->allow_uids;
	int tmp_uid_cnt = part_ptr->allow_uids_cnt;

	part_ptr->allow_uids =
		get_groups_members(part_ptr->allow_groups,
				   &part_ptr->allow_uids_cnt);

	if ((!part_ptr->allow_uids) && (!tmp_uids)) {
		/* no changes, because no arrays to compare */
	} else if ((!part_ptr->allow_uids) || (!tmp_uids) ||
		   (part_ptr->allow_uids_cnt != tmp_uid_cnt)) {
		/* creating, removing, or updating list, but sizes mismatch */
		*updated = 1;
	} else {
		/* updating with same size, we need to compare 1 by 1 */
		for (i = 0; i < part_ptr->allow_uids_cnt; i++) {
			if (tmp_uids[i] != part_ptr->allow_uids[i]) {
				*updated = 1;
				break;
			}
		}
	}

	xfree(tmp_uids);
	return 0;
}

static int _find_acct_in_list(void *x, void *arg)
{
	slurmdb_assoc_rec_t *acct_assoc_ptr = x;
	slurmdb_assoc_rec_t *query_assoc_ptr = arg;

	while (query_assoc_ptr) {
		if (acct_assoc_ptr == query_assoc_ptr)
			return 1;
		query_assoc_ptr = query_assoc_ptr->usage->parent_assoc_ptr;
	}
	return 0;
}

/*
 * load_part_uid_allow_list - reload the allow_uid list of partitions
 *	if required (updated group file or force set)
 * IN force - if set then always reload the allow_uid list
 */
void load_part_uid_allow_list(bool force)
{
	static time_t last_update_time;
	int updated = 0;
	time_t temp_time;
	DEF_TIMERS;

	START_TIMER;
	temp_time = get_group_tlm();
	if (!force && (temp_time == last_update_time))
		return;
	debug("Updating partition uid access list");
	last_update_time = temp_time;

	list_for_each(part_list, _update_part_uid_access_list, &updated);

	/* only update last_part_update when changes made to avoid restarting
	 * backfill scheduler unnecessarily */
	if (updated) {
		debug2("%s: list updated, resetting last_part_update time",
		       __func__);
		last_part_update = time(NULL);
	}

	clear_group_cache();
	END_TIMER2(__func__);
}

/* part_fini - free all memory associated with partition records */
void part_fini (void)
{
	FREE_NULL_LIST(part_list);
	default_part_loc = NULL;
}

extern int delete_partition(delete_part_msg_t *part_desc_ptr)
{
	part_record_t *part_ptr;

	part_ptr = find_part_record (part_desc_ptr->name);
	if (part_ptr == NULL)	/* No such partition */
		return ESLURM_INVALID_PARTITION_NAME;

	if (partition_in_use(part_desc_ptr->name))
		return ESLURM_PARTITION_IN_USE;

	if (default_part_loc == part_ptr) {
		error("Deleting default partition %s", part_ptr->name);
		default_part_loc = NULL;
	}
	(void) kill_job_by_part_name(part_desc_ptr->name);
	list_delete_all(part_list, list_find_part, part_desc_ptr->name);
	last_part_update = time(NULL);

	gs_reconfig();
	select_g_reconfigure();		/* notify select plugin too */

	return SLURM_SUCCESS;
}

/*
 * Validate a job's account against the partition's AllowAccounts or
 *	DenyAccounts parameters.
 * IN part_ptr - Partition pointer
 * IN acct - account name
 * in job_ptr - Job pointer or NULL. If set and job can not run, then set the
 *		job's state_desc and state_reason fields
 * RET SLURM_SUCCESS or error code
 */
extern int part_policy_valid_acct(part_record_t *part_ptr, char *acct,
				  job_record_t *job_ptr)
{
	int rc = SLURM_SUCCESS;
	slurmdb_assoc_rec_t *assoc_ptr = NULL;

	xassert(verify_assoc_lock(ASSOC_LOCK, READ_LOCK));

	if (!(accounting_enforce & ACCOUNTING_ENFORCE_ASSOCS))
		return SLURM_SUCCESS;

	if (job_ptr)
		assoc_ptr = job_ptr->assoc_ptr;
	else if (acct) {
		slurmdb_assoc_rec_t assoc_rec = {
			.acct = acct,
			.uid = NO_VAL,
		};
		if (assoc_mgr_fill_in_assoc(
			    acct_db_conn, &assoc_rec,
			    accounting_enforce,
			    &assoc_ptr, true) != SLURM_SUCCESS)
			rc = ESLURM_INVALID_ACCOUNT;
	} else
		rc = ESLURM_INVALID_ACCOUNT;

	if (!assoc_ptr)
		return rc;

	if (part_ptr->allow_accts_list) {
		if (!list_find_first(part_ptr->allow_accts_list,
				     _find_acct_in_list,
				     assoc_ptr))
			rc = ESLURM_INVALID_ACCOUNT;
	} else if (part_ptr->deny_accts_list) {
		if (list_find_first(part_ptr->deny_accts_list,
				    _find_acct_in_list,
				    assoc_ptr))
			rc = ESLURM_INVALID_ACCOUNT;
	}


	return rc;
}

/*
 * Validate a job's QOS against the partition's AllowQOS or DenyQOS parameters.
 * IN part_ptr - Partition pointer
 * IN qos_ptr - QOS pointer
 * IN submit_uid - uid of user issuing the request
 * in job_ptr - Job pointer or NULL. If set and job can not run, then set the
 *		job's state_desc and state_reason fields
 * RET SLURM_SUCCESS or error code
 */
extern int part_policy_valid_qos(part_record_t *part_ptr,
				 slurmdb_qos_rec_t *qos_ptr,
				 uid_t submit_uid,
				 job_record_t *job_ptr)
{
	char *tmp_err = NULL;

	if (part_ptr->allow_qos_bitstr) {
		int match = 0;
		if (!qos_ptr) {
			xstrfmtcat(tmp_err,
				   "Job's QOS not known, so it can't use this partition (%s allows %s)",
				   part_ptr->name, part_ptr->allow_qos);
			info("%s: %s (%pJ submit_uid=%u)",
			     __func__, tmp_err, job_ptr, submit_uid);
			if (job_ptr) {
				xfree(job_ptr->state_desc);
				job_ptr->state_desc = tmp_err;
				job_ptr->state_reason = WAIT_QOS;
				last_job_update = time(NULL);
			} else {
				xfree(tmp_err);
			}
			return ESLURM_INVALID_QOS;
		}
		if ((qos_ptr->id < bit_size(part_ptr->allow_qos_bitstr)) &&
		    bit_test(part_ptr->allow_qos_bitstr, qos_ptr->id))
			match = 1;
		if (match == 0) {
			xstrfmtcat(tmp_err,
				   "Job's QOS not permitted to use this partition (%s allows %s not %s)",
				   part_ptr->name, part_ptr->allow_qos,
				   qos_ptr->name);
			info("%s: %s (%pJ submit_uid=%u)",
			     __func__, tmp_err, job_ptr, submit_uid);
			if (job_ptr) {
				xfree(job_ptr->state_desc);
				job_ptr->state_desc = tmp_err;
				job_ptr->state_reason = WAIT_QOS;
				last_job_update = time(NULL);
			} else {
				xfree(tmp_err);
			}
			return ESLURM_INVALID_QOS;
		}
	} else if (part_ptr->deny_qos_bitstr) {
		int match = 0;
		if (!qos_ptr) {
			debug2("%s: Job's QOS not known, so couldn't check if it was denied or not",
			       __func__);
			return SLURM_SUCCESS;
		}
		if ((qos_ptr->id < bit_size(part_ptr->deny_qos_bitstr)) &&
		    bit_test(part_ptr->deny_qos_bitstr, qos_ptr->id))
			match = 1;
		if (match == 1) {
			xstrfmtcat(tmp_err,
				   "Job's QOS not permitted to use this partition (%s denies %s including %s)",
				   part_ptr->name, part_ptr->deny_qos,
				   qos_ptr->name);
			info("%s: %s (%pJ submit_uid=%u)",
			     __func__, tmp_err, job_ptr, submit_uid);
			if (job_ptr) {
				xfree(job_ptr->state_desc);
				job_ptr->state_desc = tmp_err;
				job_ptr->state_reason = WAIT_QOS;
				last_job_update = time(NULL);
			} else {
				xfree(tmp_err);
			}
			return ESLURM_INVALID_QOS;
		}
	}

	return SLURM_SUCCESS;
}

extern void part_list_update_assoc_lists(void)
{
	/* Write lock on part */
	slurmctld_lock_t part_write_lock = {
		.part = WRITE_LOCK,
	};
	assoc_mgr_lock_t locks = { .assoc = READ_LOCK };

	if (!part_list)
		return;

	lock_slurmctld(part_write_lock);
	assoc_mgr_lock(&locks);
	list_for_each(part_list, part_update_assoc_lists, NULL);
	assoc_mgr_unlock(&locks);
	unlock_slurmctld(part_write_lock);
}

extern int part_update_assoc_lists(void *x, void *arg)
{
	part_record_t *part_ptr = x;

	xassert(verify_assoc_lock(ASSOC_LOCK, READ_LOCK));

	FREE_NULL_LIST(part_ptr->allow_accts_list);
	part_ptr->allow_accts_list =
		accounts_list_build(part_ptr->allow_accounts, true);
	FREE_NULL_LIST(part_ptr->deny_accts_list);
	part_ptr->deny_accts_list =
		accounts_list_build(part_ptr->deny_accounts, true);

	return 0;
}

typedef struct {
	char *names;
	char *pos;
} _foreach_part_names_t;

static int _foreach_part_name_to_xstr(void *x, void *arg)
{
	part_record_t *part_ptr = x;
	_foreach_part_names_t *part_names = arg;

	xstrfmtcatat(part_names->names, &part_names->pos, "%s%s",
		     part_names->names ? "," : "", part_ptr->name);

	return SLURM_SUCCESS;
}

extern char *part_list_to_xstr(list_t *list)
{
	_foreach_part_names_t part_names = {0};

	xassert(list);

	list_for_each(list, _foreach_part_name_to_xstr, &part_names);

	return part_names.names;
}
