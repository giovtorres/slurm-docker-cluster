/*****************************************************************************\
 *  gres_stepmgr.h - Functions for gres used only in the slurmctld
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Derived in large part from code previously in interfaces/gres.h
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

#ifndef _GRES_CTLD_H
#define _GRES_CTLD_H

#include "src/interfaces/gres.h"

typedef struct gres_stepmgr_step_test_args {
	uint16_t cpus_per_task; /* IN cpus_per_task - number of CPUs required
				 * per task */
	int *err_code; /* OUT err_code - If an error occurred, set this to tell
			* the caller why the error happend. */
	bool first_step_node; /* IN first_step_node - true if this is node zero
			       * of the step (do initing) */
	bool ignore_alloc; /* IN ignore_alloc - if set ignore resources already
			    * allocated to running steps */
	list_t *job_gres_list; /* IN job_gres_list - a running job's allocated gres
				* info */
	uint32_t job_id; /* Job ID of the step being allocated */
	job_resources_t *job_resrcs_ptr; /* IN job_resrcs_ptr - pointer to this
					  * job's job_resources_t; used to know
					  * how much of the job's memory is
					  * available. */
	int max_rem_nodes; /* IN max_rem_nodes - maximum nodes remaining for
			    * step (including this one) */
	int node_offset; /* IN node_offset - index into the job's node
			  * allocation */
	list_t *step_gres_list; /* IN/OUT step_gres_list - a pending job step's
				 * gres requirements */
	uint32_t step_id; /* ID of the step being allocated */
	bool test_mem; /* IN test_mem - true if we should test if mem_per_gres
			* would exceed a limit. */
} gres_stepmgr_step_test_args_t;

/*
 * Fill in job_gres_list with the total amount of GRES on a node.
 * OUT job_gres_list - This list will be destroyed and remade with all GRES on
 *                     node.
 * IN node_gres_list - node's gres_list built by
 *		       gres_node_config_validate()
 * IN job_id      - job's ID (for logging)
 * IN node_name   - name of the node (for logging)
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_job_select_whole_node(
	list_t **job_gres_list, list_t *node_gres_list,
	uint32_t job_id, char *node_name);

/*
 * Select and allocate all GRES on a node to a job and update node and job GRES
 * information
 * IN job_gres_list - job's gres_list built by gres_job_whole_node().
 * OUT job_alloc_gres_list - job's list of allocated gres
 * IN node_gres_list - node's gres_list built by
 *		       gres_node_config_validate()
 * IN node_cnt    - total number of nodes originally allocated to the job
 * IN node_index  - zero-origin global node index
 * IN node_offset - zero-origin index in job allocation to the node of interest
 * IN job_id      - job's ID (for logging)
 * IN node_name   - name of the node (for logging)
 * IN core_bitmap - cores allocated to this job on this node (NULL if not
 *                  available)
 * IN new_alloc   - If this is a new allocation or not.
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_job_alloc_whole_node(
	list_t *job_gres_list, list_t **job_alloc_gres_list,
	list_t *node_gres_list, int node_cnt, int node_index, int node_offset,
	uint32_t job_id, char *node_name,
	bitstr_t *core_bitmap, bool new_alloc);

/*
 * Select and allocate GRES to a job and update node and job GRES information
 * IN job_gres_list - job's gres_list built by gres_job_state_validate()
 * OUT job_alloc_gres_list - job's list of allocated gres
 * IN node_gres_list - node's gres_list built by
 *		       gres_node_config_validate()
 * IN node_cnt    - total number of nodes originally allocated to the job
 * IN node_index  - zero-origin global node index
 * IN node_offset - zero-origin index in job allocation to the node of interest
 * IN job_id      - job's ID (for logging)
 * IN node_name   - name of the node (for logging)
 * IN core_bitmap - cores allocated to this job on this node (NULL if not
 *                  available)
 * IN new_alloc   - If this is a new allocation or not.
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_job_alloc(
	list_t *job_gres_list, list_t **job_alloc_gres_list,
	list_t *node_gres_list, int node_cnt,
	int node_index, int node_offset,
	uint32_t job_id, char *node_name,
	bitstr_t *core_bitmap, bool new_alloc);

/*
 * Deallocate resource from a job and update node and job gres information
 * IN job_gres_list - job's gres_list built by gres_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *		gres_node_config_validate()
 * IN node_offset - zero-origin index to the node of interest
 * IN job_id      - job's ID (for logging)
 * IN node_name   - name of the node (for logging)
 * IN old_job     - true if job started before last slurmctld reboot.
 *		    Immediately after slurmctld restart and before the node's
 *		    registration, the GRES type and topology. This results in
 *		    some incorrect internal bookkeeping, but does not cause
 *		    failures in terms of allocating GRES to jobs.
 * IN user_id     - job's user ID
 * IN: resize     - True if dealloc is due to a node being removed via a job
 * 		    resize; false if dealloc is due to a job test or a real job
 * 		    that is terminating.
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_job_dealloc(
	list_t *job_gres_list, list_t *node_gres_list,
	int node_offset, uint32_t job_id,
	char *node_name, bool old_job, bool resize);

/*
 * Merge one job's gres allocation into another job's gres allocation.
 * IN from_job_gres_list - List of gres records for the job being merged
 *			   into another job
 * IN from_job_node_bitmap - bitmap of nodes for the job being merged into
 *			     another job
 * IN/OUT to_job_gres_list - List of gres records for the job being merged
 *			     into job
 * IN to_job_node_bitmap - bitmap of nodes for the job being merged into
 */
extern void gres_stepmgr_job_merge(
	list_t *from_job_gres_list,
	bitstr_t *from_job_node_bitmap,
	list_t *to_job_gres_list,
	bitstr_t *to_job_node_bitmap);

/*
 * Clear any vestigial alloc job gres state. This may be needed on job requeue.
 * This only clears out the allocated portions of the gres list, it does not
 * remove the actual items from the list.
 */
extern void gres_stepmgr_job_clear_alloc(list_t *job_gres_list);

/* Given a job's GRES data structure, return the indecies for selected elements
 * IN job_gres_list  - job's allocated GRES data structure
 * IN nodes - list of nodes allocated to job
 * OUT gres_detail_cnt - Number of elements (nodes) in gres_detail_str
 * OUT gres_detail_str - Description of GRES on each node
 * OUT total_gres_str - String containing all gres in the job and counts.
 */
extern void gres_stepmgr_job_build_details(
	list_t *job_gres_list, char *nodes,
	uint32_t *gres_detail_cnt,
	char ***gres_detail_str,
	char **total_gres_str);

/*
 * Fill in the job allocated tres_cnt based off the gres_list and node_cnt
 * IN gres_list - filled in with gres_job_state_t's
 * IN node_cnt - number of nodes in the job
 * OUT tres_cnt - gres spots filled in with total number of TRES
 *                requested for job that are requested in gres_list
 * IN locked - if the assoc_mgr tres read locked is locked or not
 */
extern void gres_stepmgr_set_job_tres_cnt(
	list_t *gres_list, uint32_t node_cnt, uint64_t *tres_cnt, bool locked);

/*
 * Fill in the node allocated tres_cnt based off the gres_list
 * IN gres_list - filled in with gres_node_state_t's gres_alloc_cnt
 * OUT tres_cnt - gres spots filled in with total number of TRES
 *                allocated on node
 * IN locked - if the assoc_mgr tres read locked is locked or not
 */
extern void gres_stepmgr_set_node_tres_cnt(
	list_t *gres_list, uint64_t *tres_cnt, bool locked);

/*
 * Determine how many cores of a job's allocation can be allocated to a step
 *	on a specific node
 * IN args - see the definition of gres_stepmgr_step_test_args_t
 * RET Count of available cpus on this node (sort of):
 *     NO_VAL64 if no limit or 0 if node is not usable
 */
extern uint64_t gres_stepmgr_step_test(gres_stepmgr_step_test_args_t *args);

/*
 * If a step gres request used gres_per_step it must be tested more than just in
 * gres_stepmgr_step_test. This function only acts when gres_per_step is used
 * IN step_gres_list  - step's requested GRES data structure
 * IN job_ptr - Job data
 * IN/OUT nodes_avail - Bitstring of nodes available for this step to use
 * IN min_nodes - minimum nodes required for this step
 */
extern void gres_stepmgr_step_test_per_step(
	list_t *step_gres_list,
	job_record_t *job_ptr,
	bitstr_t *nodes_avail,
	int min_nodes);

/*
 * Allocate resource to a step and update job and step gres information
 * IN step_gres_list - step's gres_list built by
 *		gres_step_state_validate()
 * OUT step_gres_list_alloc - step's list of allocated gres
 * IN job_gres_list - job's allocated gres_list built by gres_stepmgr_job_alloc()
 * IN node_offset - job's zero-origin index to the node of interest
 * IN first_step_node - true if this is node zero of the step
 *                      (do initialization)
 * IN tasks_on_node - number of tasks to be launched on this node
 * IN rem_nodes - desired additional node count to allocate, including this node
 * IN job_id, step_id - ID of the step being allocated.
 * IN decr_job_alloc - whether or not to decrement the step allocation from the
 *                     job allocation.
 * OUT step_node_mem_alloc - the amount of memory allocated to the step on this
 * 		node based on mem_per_gres requirements.
 * IN node_gres_list - node's gres list
 * IN core_bitmap - bitmap of all cores available for the step
 * OUT total_gres_cpu_cnt - sum of (cpus_per_gres * alloc_gres_cnt) for each
 *                          gres allocated to the step on this node
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_step_alloc(
	list_t *step_gres_list,
	list_t **step_gres_list_alloc,
	list_t *job_gres_list,
	int node_offset, bool first_step_node,
	uint16_t tasks_on_node, uint32_t rem_nodes,
	uint32_t job_id, uint32_t step_id,
	bool decr_job_alloc,
	uint64_t *step_node_mem_alloc,
	list_t *node_gres_list,
	bitstr_t *core_bitmap,
	int *total_gres_cpu_cnt);

/*
 * Deallocate resource to a step and update job and step gres information
 * IN step_gres_list_alloc - step's list of allocated gres
 * IN job_gres_list - job's allocated gres_list built by
 *                    gres_stepmgr_job_alloc()
 * IN job_id, step_id - ID of the step being allocated.
 * IN node_offset - job's zero-origin index to the node of interest
 * IN decr_job_alloc - whether or not to decrement the step allocation from the
 *                     job allocation.
 * RET SLURM_SUCCESS or error code
 */
extern int gres_stepmgr_step_dealloc(list_t *step_gres_list_alloc,
				     list_t *job_gres_list,
				     uint32_t job_id, uint32_t step_id,
				     int node_offset, bool decr_job_alloc);

/*
 * A job allocation size has changed. Update the job step gres information
 * bitmaps and other data structures.
 * IN gres_list - List of Gres records for this step to track usage
 * IN orig_job_node_bitmap - bitmap of nodes in the original job allocation
 * IN new_job_node_bitmap - bitmap of nodes in the new job allocation
 */
extern void gres_stepmgr_step_state_rebase(
	list_t *gres_list,
	bitstr_t *orig_job_node_bitmap,
	bitstr_t *new_job_node_bitmap);

/*
 * Given a job's GRES data structure, return a simple tres string of gres
 * allocated on the node_inx requested
 * IN job_gres_list  - job's allocated GRES data structure
 * IN node_inx - position of node in job_state_ptr->gres_cnt_node_alloc
 *
 * RET - simple string containing gres this job is allocated on the node
 * requested.
 */
extern char *gres_stepmgr_gres_on_node_as_tres(
	list_t *job_gres_list, int node_inx, bool locked);

/*
 * Translate a gres_list into a tres_str
 * IN gres_list - filled in with gres_job_state_t or gres_step_state_t's
 * IN locked - if the assoc_mgr tres read locked is locked or not
 * RET char * in a simple TRES format
 */
extern char *gres_stepmgr_gres_2_tres_str(
	list_t *gres_list, bool locked);

#endif /* _GRES_CTLD_H */
