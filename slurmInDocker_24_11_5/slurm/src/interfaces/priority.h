/*****************************************************************************\
 *  priority.h - Define priority plugin functions
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
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

#ifndef _INTERFACES_PRIORITY_H
#define _INTERFACES_PRIORITY_H

#include <inttypes.h>

#include "src/slurmctld/slurmctld.h"
#include "src/interfaces/accounting_storage.h"

/*
 * Sort partitions on Priority Tier.
 */
extern int priority_sort_part_tier(void *x, void *y);

/*
 * Sort QOS on Priority.
 */
extern int priority_sort_qos_desc(void *x, void *y);

extern int priority_g_init(void);
extern int priority_g_fini(void);
extern uint32_t priority_g_set(uint32_t last_prio, job_record_t *job_ptr);
extern void priority_g_reconfig(bool assoc_clear);
extern uint32_t priority_g_recover(uint32_t prio_boost);
extern void priority_g_thread_start(void);

/* sets up the normalized usage and the effective usage of an
 * association.
 * IN/OUT: assoc - association to have usage set.
 */
extern void priority_g_set_assoc_usage(slurmdb_assoc_rec_t *assoc);
extern double priority_g_calc_fs_factor(long double usage_efctv,
					long double shares_norm);

extern list_t *priority_g_get_priority_factors_list(uid_t uid);

/* Call at end of job to remove decayable limits at the end of the job
 * at least slurmctld_lock_t job_write_lock = { NO_LOCK, WRITE_LOCK,
 * READ_LOCK, READ_LOCK }; should be locked before calling this
 */
extern void priority_g_job_end(job_record_t *job_ptr);

#endif
