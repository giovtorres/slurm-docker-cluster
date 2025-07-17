/*****************************************************************************\
 *  job_resources.c - Functions for structures dealing with resources unique to
 *                    the select plugin.
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Derived in large part from select/cons_[res|tres] plugins
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

#include "select_cons_tres.h"

#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/licenses.h"

#include "src/stepmgr/gres_stepmgr.h"

bool select_state_initializing = true;

typedef enum {
	HANDLE_JOB_RES_ADD,
	HANDLE_JOB_RES_REM,
	HANDLE_JOB_RES_TEST
} handle_job_res_t;

static bitstr_t *_create_core_bitmap(int node_inx)
{
	xassert(node_inx < node_record_count);

	if (!node_record_table_ptr[node_inx])
		return NULL;

	return bit_alloc(node_record_table_ptr[node_inx]->tot_cores);
}

/*
 * Handle job resource allocation to record of resources allocated to all nodes
 * IN job_resrcs_ptr - resources allocated to a job
 * IN r_ptr - row we are trying to fit
 *            IN/OUT r_ptr->row_bitmap - bitmap array (one per node) of
 *                                       available cores, allocated as needed
 * IN type - add/rem/test
 * RET 1 on success, 0 otherwise
 */
static int _handle_job_res(job_resources_t *job_resrcs_ptr,
			   part_row_data_t *r_ptr,
			   handle_job_res_t type)
{
	int c, c_off = 0;
	bitstr_t **core_array;
	uint16_t cores_per_node;
	node_record_t *node_ptr;

	if (!job_resrcs_ptr->core_bitmap)
		return 1;

	/* create row_bitmap data structure as needed */
	if (!r_ptr->row_bitmap) {
		if (type == HANDLE_JOB_RES_TEST)
			return 1;
		core_array = build_core_array();
		r_ptr->row_bitmap = core_array;
		r_ptr->row_set_count = 0;
		for (int i = 0; i < node_record_count; i++)
			core_array[i] = _create_core_bitmap(i);
	} else
		core_array = r_ptr->row_bitmap;

	for (int i = 0;
	     (node_ptr = next_node_bitmap(job_resrcs_ptr->node_bitmap, &i));
	     i++) {
		cores_per_node = node_ptr->tot_cores;

		/*
		 * This segment properly handles the core counts when whole
		 * nodes are allocated, including when explicitly requesting
		 * specialized cores.
		 */
		if (job_resrcs_ptr->whole_node == 1) {
			if (!core_array[i]) {
				if (type != HANDLE_JOB_RES_TEST)
					error("core_array for node %d is NULL %d",
					      i, type);
				continue;	/* Move to next node */
			}

			switch (type) {
			case HANDLE_JOB_RES_ADD:
				bit_set_all(core_array[i]);
				r_ptr->row_set_count += cores_per_node;
				break;
			case HANDLE_JOB_RES_REM:
				bit_clear_all(core_array[i]);
				r_ptr->row_set_count -= cores_per_node;
				break;
			case HANDLE_JOB_RES_TEST:
				if (bit_ffs(core_array[i]) != -1)
					return 0;
				break;
			}
			continue;	/* Move to next node */
		}

		for (c = 0; c < cores_per_node; c++) {
			if (!bit_test(job_resrcs_ptr->core_bitmap, c_off + c))
				continue;
			if (!core_array[i]) {
				if (type != HANDLE_JOB_RES_TEST)
					error("core_array for node %d is NULL %d",
					      i, type);
				continue;	/* Move to next node */
			}
			switch (type) {
			case HANDLE_JOB_RES_ADD:
				bit_set(core_array[i], c);
			        r_ptr->row_set_count++;
				break;
			case HANDLE_JOB_RES_REM:
				bit_clear(core_array[i], c);
				r_ptr->row_set_count--;
				break;
			case HANDLE_JOB_RES_TEST:
				if (bit_test(core_array[i], c))
					return 0;    /* Core conflict on node */
				break;
			}
		}
		c_off += cores_per_node;
	}

	return 1;
}

static void _log_tres_state(node_use_record_t *node_usage,
			    part_res_record_t *part_record_ptr)
{
#if _DEBUG
	node_record_t *node_ptr;
	part_res_record_t *p_ptr;
	int i;

	for (i = 0; (node_ptr = next_node(&i)); i++) {
		list_t *gres_list;
		info("Node:%s AllocMem:%"PRIu64" of %"PRIu64,
		     node_ptr->name,
		     node_usage[i].alloc_memory,
		     node_ptr->real_memory);
		if (node_usage[node_ptr->index].gres_list)
			gres_list = select_node_usage[node_ptr->index].
					gres_list;
		else
			gres_list = node_ptr->gres_list;
		if (gres_list)
			gres_node_state_log(gres_list, node_ptr->name);
	}
	for (p_ptr = part_record_ptr; p_ptr; p_ptr = p_ptr->next) {
		part_data_dump_res(p_ptr);
	}
#endif
}

extern char *job_res_job_action_string(job_res_job_action_t action)
{
	switch (action) {
	case JOB_RES_ACTION_NORMAL:
		return "normal";
		break;
	case JOB_RES_ACTION_SUSPEND:
		return "suspend";
		break;
	case JOB_RES_ACTION_RESUME:
		return "resume";
		break;
	default:
		return "unknown";
	}
}

/*
 * Add job resource allocation to record of resources allocated to all nodes
 * IN job_resrcs_ptr - resources allocated to a job
 * IN r_ptr - row we are trying to fit
 *            IN/OUT r_ptr->row_bitmap - bitmap array (one per node) of
 *                                       available cores, allocated as needed
 * NOTE: Patterned after add_job_to_cores() in src/common/job_resources.c
 */
extern void job_res_add_cores(job_resources_t *job_resrcs_ptr,
			      part_row_data_t *r_ptr)
{
	(void)_handle_job_res(job_resrcs_ptr, r_ptr,
			      HANDLE_JOB_RES_ADD);
}


/*
 * Remove job resource allocation to record of resources allocated to all nodes
 * IN job_resrcs_ptr - resources allocated to a job
 * IN r_ptr - row we are trying to fit
 *            IN/OUT r_ptr->row_bitmap - bitmap array (one per node) of
 *                                       available cores, allocated as needed
 */
extern void job_res_rm_cores(job_resources_t *job_resrcs_ptr,
			     part_row_data_t *r_ptr)
{
	(void)_handle_job_res(job_resrcs_ptr, r_ptr,
			      HANDLE_JOB_RES_REM);
}

/*
 * Test if job can fit into the given set of core_bitmaps
 * IN job_resrcs_ptr - resources allocated to a job
 * IN r_ptr - row we are trying to fit
 * RET 1 on success, 0 otherwise
 * NOTE: Patterned after job_fits_into_cores() in src/common/job_resources.c
 */
extern int job_res_fit_in_row(job_resources_t *job_resrcs_ptr,
			      part_row_data_t *r_ptr)
{
	if ((r_ptr->num_jobs == 0) || !r_ptr->row_bitmap)
		return 1;

	return _handle_job_res(job_resrcs_ptr, r_ptr,
			       HANDLE_JOB_RES_TEST);
}

/*
 * allocate resources to the given job
 * - add 'struct job_resources' resources to 'part_res_record_t'
 * - add job's memory requirements to 'node_res_record_t'
 *
 * if action = JOB_RES_ACTION_NORMAL then add cores, memory + GRES
 *             (starting new job)
 * if action = JOB_RES_ACTION_SUSPEND then add memory + GRES
 *             (adding suspended job at restart)
 * if action = JOB_RES_ACTION_RESUME then only add cores
 *             (suspended job is resumed)
 *
 * See also: job_res_rm_job()
 */
extern int job_res_add_job(job_record_t *job_ptr, job_res_job_action_t action)
{
	struct job_resources *job = job_ptr->job_resrcs;
	node_record_t *node_ptr;
	part_res_record_t *p_ptr;
	list_t *node_gres_list;
	int i, n;
	bitstr_t *core_bitmap;
	bool new_alloc = true;

	if (!job || !job->core_bitmap) {
		error("%pJ has no job_resrcs info",
		      job_ptr);
		return SLURM_ERROR;
	}

	debug3("%pJ action:%s", job_ptr,
	       job_res_job_action_string(action));

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE)
		log_job_resources(job_ptr);

	if (job_ptr->gres_list_alloc)
		new_alloc = false;
	for (i = 0, n = -1; (node_ptr = next_node_bitmap(job->node_bitmap, &i));
	     i++) {
		n++;
		if (job->cpus[n] == 0)
			continue;  /* node removed by job resize */

		if (action != JOB_RES_ACTION_RESUME) {
			if (select_node_usage[i].gres_list)
				node_gres_list = select_node_usage[i].gres_list;
			else
				node_gres_list = node_ptr->gres_list;
			core_bitmap = copy_job_resources_node(job, n);
			if (job_ptr->details &&
			    (job_ptr->details->whole_node &
			     WHOLE_NODE_REQUIRED))
				gres_stepmgr_job_alloc_whole_node(
					job_ptr->gres_list_req,
					&job_ptr->gres_list_alloc,
					node_gres_list, job->nhosts,
					i, n, job_ptr->job_id,
					node_ptr->name, core_bitmap, new_alloc);
			else
				gres_stepmgr_job_alloc(
					job_ptr->gres_list_req,
					&job_ptr->gres_list_alloc,
					node_gres_list, job->nhosts,
					i, n, job_ptr->job_id,
					node_ptr->name, core_bitmap, new_alloc);

			gres_node_state_log(node_gres_list, node_ptr->name);
			FREE_NULL_BITMAP(core_bitmap);

			if (job->memory_allocated[n] == 0)
				continue;	/* node lost by job resizing */
			select_node_usage[i].alloc_memory +=
				job->memory_allocated[n];
			if ((select_node_usage[i].alloc_memory >
			     node_ptr->real_memory)) {
				error("node %s memory is "
				      "overallocated (%"PRIu64") for %pJ",
				      node_ptr->name,
				      select_node_usage[i].alloc_memory,
				      job_ptr);
			}
		}
	}

	if (action != JOB_RES_ACTION_RESUME) {
		gres_stepmgr_job_build_details(job_ptr->gres_list_alloc,
					       job_ptr->nodes,
					       &job_ptr->gres_detail_cnt,
					       &job_ptr->gres_detail_str,
					       &job_ptr->gres_used);
	}

	/* add cores */
	if (action != JOB_RES_ACTION_SUSPEND) {
		for (p_ptr = select_part_record; p_ptr; p_ptr = p_ptr->next) {
			if (p_ptr->part_ptr == job_ptr->part_ptr)
				break;
		}
		if (!p_ptr) {
			char *part_name;
			if (job_ptr->part_ptr)
				part_name = job_ptr->part_ptr->name;
			else
				part_name = job_ptr->partition;
			error("could not find partition %s",
			      part_name);
			return SLURM_ERROR;
		}

		if (p_ptr->rebuild_rows)
			part_data_build_row_bitmaps(p_ptr, NULL);

		if (!p_ptr->row) {
			p_ptr->row = xcalloc(p_ptr->num_rows,
					     sizeof(part_row_data_t));
		}

		/* find a row to add this job */
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (!job_res_fit_in_row(job, &(p_ptr->row[i])))
				continue;
			debug3("adding %pJ to part %s row %u",
			       job_ptr,
			       p_ptr->part_ptr->name, i);
			part_data_add_job_to_row(job, &(p_ptr->row[i]));
			break;
		}
		if (i >= p_ptr->num_rows) {
			/*
			 * Job started or resumed and it's allocated resources
			 * are already in use by some other job. Typically due
			 * to manually resuming a job.
			 */
			error("job overflow: "
			      "could not find idle resources for %pJ",
			      job_ptr);
			/* No row available to record this job */
		}
		/* update the node state */
		for (i = 0, n = -1; next_node_bitmap(job->node_bitmap, &i);
		     i++) {
			n++;
			if (job->cpus[n] == 0)
				continue;  /* node lost by job resize */
			select_node_usage[i].node_state += job->node_req;
			if (!select_node_usage[i].jobs)
				select_node_usage[i].jobs = list_create(NULL);
			if (action == JOB_RES_ACTION_NORMAL)
				list_append(select_node_usage[i].jobs, job_ptr);
		}
		if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
			info("DEBUG: (after):");
			part_data_dump_res(p_ptr);
		}
	}
	return SLURM_SUCCESS;
}

/*
 * Deallocate resources previously allocated to the given job
 * - subtract 'struct job_resources' resources from 'part_res_record_t'
 * - subtract job's memory requirements from 'node_res_record_t'
 *
 * if action = JOB_RES_ACTION_NORMAL then subtract cores, memory + GRES
 *             (running job was terminated)
 * if action = JOB_RES_ACTION_SUSPEND then subtract memory + GRES
 *             (suspended job was terminated)
 * if action = JOB_RES_ACTION_RESUME then only subtract cores
 *             (job is suspended)
 *
 * RET SLURM_SUCCESS or error code
 *
 * See also: job_res_add_job()
 */
extern int job_res_rm_job(part_res_record_t *part_record_ptr,
			  node_use_record_t *node_usage, list_t *license_list,
			  job_record_t *job_ptr, job_res_job_action_t action,
			  bitstr_t *node_map)
{
	struct job_resources *job = job_ptr->job_resrcs;
	node_record_t *node_ptr;
	int i, n;
	bool old_job = false;

	if (select_state_initializing) {
		/*
		 * Ignore job removal until select/cons_tres data structures
		 * values are set by select_p_reconfigure()
		 */
		info("plugin still initializing");
		return SLURM_SUCCESS;
	}
	if (!job || !job->core_bitmap) {
		if (job_ptr->details && (job_ptr->details->min_nodes == 0))
			return SLURM_SUCCESS;
		error("%pJ has no job_resrcs info",
		      job_ptr);
		return SLURM_ERROR;
	}

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("%pJ action:%s",
		     job_ptr, job_res_job_action_string(action));
		log_job_resources(job_ptr);
		_log_tres_state(node_usage, part_record_ptr);
	} else {
		debug3("%pJ action:%s",
		       job_ptr, job_res_job_action_string(action));
	}

	if (license_list)
		license_job_return_to_list(job_ptr, license_list);

	if (job_ptr->start_time < slurmctld_config.boot_time)
		old_job = true;
	for (i = 0, n = -1; (node_ptr = next_node_bitmap(job->node_bitmap, &i));
	     i++) {
		n++;

		if (node_map && !bit_test(node_map, i))
			continue;
		if (job->cpus[n] == 0)
			continue;  /* node lost by job resize */

		if (action != JOB_RES_ACTION_RESUME) {
			list_t *node_gres_list;

			if (node_usage[i].gres_list)
				node_gres_list = node_usage[i].gres_list;
			else
				node_gres_list = node_ptr->gres_list;

			gres_stepmgr_job_dealloc(job_ptr->gres_list_alloc,
						 node_gres_list,
						 n, job_ptr->job_id,
						 node_ptr->name, old_job,
						 false);
			gres_node_state_log(node_gres_list, node_ptr->name);

			if (node_usage[i].alloc_memory <
			    job->memory_allocated[n]) {
				error("node %s memory is "
				      "under-allocated (%"PRIu64"-%"PRIu64") "
				      "for %pJ",
				      node_ptr->name,
				      node_usage[i].alloc_memory,
				      job->memory_allocated[n],
				      job_ptr);
				node_usage[i].alloc_memory = 0;
			} else {
				node_usage[i].alloc_memory -=
					job->memory_allocated[n];
			}
		}
	}

	/*
	 * Subtract cores JOB_RES_ACTION_SUSPEND isn't used at this moment, but
	 * we will keep this check just to be clear what we are doing.
	 */
	if (action != JOB_RES_ACTION_SUSPEND) {
		/* reconstruct rows with remaining jobs */
		part_res_record_t *p_ptr;

		if (!job_ptr->part_ptr) {
			error("removed %pJ does not have a partition assigned",
			      job_ptr);
			return SLURM_ERROR;
		}

		for (p_ptr = part_record_ptr; p_ptr; p_ptr = p_ptr->next) {
			if (p_ptr->part_ptr == job_ptr->part_ptr)
				break;
		}
		if (!p_ptr) {
			error("removed %pJ could not find part %s",
			      job_ptr,
			      job_ptr->part_ptr->name);
			return SLURM_ERROR;
		}

		if (!p_ptr->row)
			return SLURM_SUCCESS;

		/* remove the job from the job_list */
		n = 0;
		for (i = 0; i < p_ptr->num_rows; i++) {
			uint32_t j;
			for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
				if (p_ptr->row[i].job_list[j] != job)
					continue;
				debug3("removed %pJ from part %s row %u",
				       job_ptr,
				       p_ptr->part_ptr->name, i);
				for ( ; j < p_ptr->row[i].num_jobs-1; j++) {
					p_ptr->row[i].job_list[j] =
						p_ptr->row[i].job_list[j+1];
				}
				p_ptr->row[i].job_list[j] = NULL;
				p_ptr->row[i].num_jobs--;
				/* found job - we're done */
				n = 1;
				i = p_ptr->num_rows;
				break;
			}
		}
		if (n) {
			/* job was found and removed, so bitmaps need be rebuild */
			if (p_ptr->num_rows == 1)
				part_data_build_row_bitmaps(p_ptr, job_ptr);
			else
				p_ptr->rebuild_rows = true;
			/*
			 * Adjust the node_state of all nodes affected by
			 * the removal of this job. If all cores are now
			 * available, set node_state = NODE_CR_AVAILABLE
			 */
			for (i = 0, n = -1;
			     (node_ptr =
				      next_node_bitmap(job->node_bitmap, &i));
			     i++) {
				n++;
				if (job->cpus[n] == 0)
					continue;  /* node lost by job resize */
				if (node_map && !bit_test(node_map, i))
					continue;
				if (node_usage[i].node_state >=
				    job->node_req) {
					node_usage[i].node_state -=
						job->node_req;
				} else {
					error("node_state mis-count (%pJ job_cnt:%u node:%s node_cnt:%u)",
					      job_ptr,
					      job->node_req, node_ptr->name,
					      node_usage[i].node_state);
					node_usage[i].node_state =
						NODE_CR_AVAILABLE;
				}
				if ((action == JOB_RES_ACTION_NORMAL) &&
				    node_usage[i].jobs)
					list_delete_first(
						node_usage[i].jobs,
						slurm_find_ptr_in_list,
						job_ptr);
			}
		} else if ((action == JOB_RES_ACTION_NORMAL) &&
			   job_ptr->suspend_time && IS_JOB_FINISHED(job_ptr)) {
			/*
			 * For a previously suspended job, if it has been
			 * finished now:
			 *
			 * 1. At suspend time "node_usage" hadn't got the job
			 *    removed from the job's nodes. This was intended.
			 * 2. If we now want to finish the job, we force-clean
			 *    "node_usage" here, as the other point were we do
			 *    that in this function is unreachable for this
			 *    specific case.
			 */
			for (int i = 0;
			     next_node_bitmap(job_ptr->node_bitmap, &i); i++)
				if (node_usage[i].jobs)
					list_delete_first(
						node_usage[i].jobs,
						slurm_find_ptr_in_list,
						job_ptr);
		}
	}
	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("%pJ finished", job_ptr);
		_log_tres_state(node_usage, part_record_ptr);
	}

	return SLURM_SUCCESS;
}
