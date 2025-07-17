/*****************************************************************************\
 *  licenses.h - Definitions for handling cluster-wide consumable resources
 *****************************************************************************
 *  Copyright (C) 2008-2011 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette@llnl.gov>, et. al.
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

#ifndef _LICENSES_H
#define _LICENSES_H

#include "src/common/list.h"
#include "src/slurmctld/slurmctld.h"

typedef struct {
	char *		name;		/* name associated with a license */
	uint32_t	total;		/* total license configued */
	uint32_t	used;		/* used licenses */
	uint32_t	reserved;	/* currently reserved licenses */
	uint8_t         remote;	        /* non-zero if remote (from database) */
	uint32_t last_deficit;		/* last calculated deficit */
	uint32_t last_consumed;		/* consumed count (for remote) */
	time_t last_update;		/* last updated timestamp (for remote) */
} licenses_t;

/*
 * In the future this should change to a more performant data structure.
 */
typedef struct xlist bf_licenses_t;

typedef struct {
	char *name;
	uint32_t remaining;
	slurmctld_resv_t *resv_ptr;
} bf_license_t;

extern list_t *cluster_license_list;
extern time_t last_license_update;

/* Initialize licenses on this system based upon slurm.conf */
extern int license_init(char *licenses);

/* Update licenses on this system based upon slurm.conf.
 * Remove all previously allocated licenses */
extern int license_update(char *licenses);

extern void license_add_remote(slurmdb_res_rec_t *rec);
extern void license_update_remote(slurmdb_res_rec_t *rec);
extern void license_remove_remote(slurmdb_res_rec_t *rec);
extern void license_sync_remote(list_t *res_list);

/* Free memory associated with licenses on this system */
extern void license_free(void);

/* Free a license_t record (for use by list_destroy) */
extern void license_free_rec(void *x);

/*
 * license_copy - create a copy of license list
 * IN license_list_src - job license list to be copied
 * RET a copy of the license list
 */
extern list_t *license_copy(list_t *license_list_src);

/*
 * license_job_get - Get the licenses required for a job
 * IN job_ptr - job identification
 * IN restore - is this a new allocation, or are we loading state from disk
 * RET SLURM_SUCCESS or failure code
 */
extern int license_job_get(job_record_t *job_ptr, bool restore);

/*
 * license_job_merge - The licenses from one job have just been merged into
 *	another job by appending one job's licenses to another, possibly
 *	including duplicate names. Reconstruct this job's licenses and
 *	license_list fields to eliminate duplicates.
 */
extern void license_job_merge(job_record_t *job_ptr);

/*
 * license_job_return - Return the licenses allocated to a job
 * IN job_ptr - job identification
 * RET SLURM_SUCCESS or failure code
 */
extern int license_job_return_to_list(job_record_t *job_ptr,
				      list_t *license_list);

/*
 * license_job_return - Return the licenses allocated to a job
 * IN job_ptr - job identification
 * RET SLURM_SUCCESS or failure code
 */
extern int license_job_return(job_record_t *job_ptr);

/*
 * license_job_test_with_list - Test if the licenses required for a job are
 *	available in provided list
 * IN job_ptr - job identification
 * IN when    - time to check
 * IN reboot    - true if node reboot required to start job
 * IN check_preempt_licenses - If false, do not look for licenses to preempt
 * RET: SLURM_SUCCESS, EAGAIN (not available now), SLURM_ERROR (never runnable)
 */
extern int license_job_test_with_list(job_record_t *job_ptr, time_t when,
				      bool reboot, list_t *license_list,
				      bool check_preempt_licenses);

/*
 * license_job_test - Test if the licenses required for a job are available
 * IN job_ptr - job identification
 * IN when    - time to check
 * IN reboot    - true if node reboot required to start job
 * RET: SLURM_SUCCESS, EAGAIN (not available now), SLURM_ERROR (never runnable)
 */
extern int license_job_test(job_record_t *job_ptr, time_t when,
			    bool reboot);

/*
 * license_validate - Test if the required licenses are valid
 * IN licenses - required licenses
 * IN validate_configured - if true, validate that there are enough configured
 *                          licenses for the requested amount.
 * IN validate_existing - if true, validate that licenses exist, otherwise don't
 *                        return them in the final list.
 * OUT tres_req_cnt - appropriate counts for each requested gres
 * OUT valid - true if required licenses are valid and a sufficient number
 *             are configured (though not necessarily available now)
 * RET license_list, must be destroyed by caller
 */
extern list_t *license_validate(char *licenses, bool validate_configured,
				bool validate_existing,
				uint64_t *tres_req_cnt, bool *valid);

/*
 * license_list_overlap - test if there is any overlap in licenses
 *	names found in the two lists
 */
extern bool license_list_overlap(list_t *list_1, list_t *list_2);

/*
 * Given a list of license_t records, return a license string.
 *
 * This can be combined with _build_license_list() to eliminate duplicates
 * (e.g. "tux*2,tux*3" gets changed to "tux*5").
 *
 * IN license_list - list of license_t records
 *
 * RET string representation of licenses. Must be destroyed by caller.
 */
extern char *license_list_to_string(list_t *license_list);

/* pack_all_licenses()
 *
 * Get the licenses and the usage counters in the io buffer
 * to be sent out to the library
 */
extern buf_t *get_all_license_info(uint16_t protocol_version);

/*
 * get_total_license_cnt - give me the total count of a given license name.
 *
 */
extern uint32_t get_total_license_cnt(char *name);

/* node_read should be locked before coming in here
 * returns tres_str of the license_list.
 */
extern char *licenses_2_tres_str(list_t *license_list);


/* node_read should be locked before coming in here
 * fills in tres_cnt of the license_list.
 * locked if assoc_mgr tres read lock is locked or not.
 */
extern void license_set_job_tres_cnt(list_t *license_list,
				     uint64_t *tres_cnt,
				     bool locked);

extern bf_licenses_t *bf_licenses_initial(bool bf_running_job_reserve);

extern char *bf_licenses_to_string(bf_licenses_t *licenses_list);

/*
 * A NULL licenses argument to these functions indicates that backfill
 * license tracking support has been disabled, or that the system has no
 * licenses to track.
 *
 * The backfill scheduler is especially performance sensitive, so each of these
 * functions is wrapped in a macro that avoids the function call when a NULL
 * licenses list is provided as the first argument.
 */
#define bf_licenses_copy(_x) (_x ? slurm_bf_licenses_copy(_x) : NULL)
extern bf_licenses_t *slurm_bf_licenses_copy(bf_licenses_t *licenses_src);

#define bf_licenses_deduct(_x, _y) (_x ? slurm_bf_licenses_deduct(_x, _y) : NULL)
extern void slurm_bf_licenses_deduct(bf_licenses_t *licenses,
				     job_record_t *job_ptr);

#define bf_licenses_transfer(_x, _y) (_x ? slurm_bf_licenses_transfer(_x, _y) : NULL)
extern void slurm_bf_licenses_transfer(bf_licenses_t *licenses,
				       job_record_t *job_ptr);

#define bf_licenses_avail(_x, _y) (_x ? slurm_bf_licenses_avail(_x, _y) : true)
extern bool slurm_bf_licenses_avail(bf_licenses_t *licenses,
				    job_record_t *job_ptr);

#define bf_licenses_equal(_x, _y) (_x ? slurm_bf_licenses_equal(_x, _y) : true)
extern bool slurm_bf_licenses_equal(bf_licenses_t *a, bf_licenses_t *b);

#define FREE_NULL_BF_LICENSES(_x) FREE_NULL_LIST(_x)

#endif /* !_LICENSES_H */
