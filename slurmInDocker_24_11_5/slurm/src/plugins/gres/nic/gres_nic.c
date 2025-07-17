/*****************************************************************************\
 *  gres_nic.c - Support NICs as a generic resources.
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
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

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/common/bitstring.h"
#include "src/common/env.h"
#include "src/interfaces/gres.h"
#include "src/common/list.h"
#include "src/common/xstring.h"

#include "../common/gres_common.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - A string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - A string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for Slurm authentication) and <method> is a
 * description of how this plugin satisfies that application.  Slurm will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char	plugin_name[]		= "Gres NIC plugin";
const char	plugin_type[]		= "gres/nic";
const uint32_t	plugin_version		= SLURM_VERSION_NUMBER;

static list_t *gres_devices = NULL;

static void _set_env(common_gres_env_t *gres_env)
{
	char *slurm_env_var = NULL;

	if (gres_env->is_job)
			slurm_env_var = "SLURM_JOB_NICS";
	else
			slurm_env_var = "SLURM_STEP_NICS";

	gres_env->prefix = "mlx4_";
	gres_env->use_dev_num = true;

	/*
	 * Set use_dev_num=true so number at end of device file is used as the
	 * global index, rather than an index relative to the total number of
	 * NICs
	 */
	common_gres_set_env(gres_env);

	/*
	 * Set environment variables if GRES is found. Otherwise, unset
	 * environment variables, since this means GRES is not allocated.
	 * This is useful for jobs and steps that request --gres=none within an
	 * existing job allocation with GRES.
	 */
	if (gres_env->global_list) {
		env_array_overwrite(gres_env->env_ptr, slurm_env_var,
				    gres_env->global_list);
		xfree(gres_env->global_list);
	} else {
		unsetenvp(*gres_env->env_ptr, slurm_env_var);
	}

	if (gres_env->local_list) {
		env_array_overwrite(
			gres_env->env_ptr, "OMPI_MCA_btl_openib_if_include",
			gres_env->local_list);
		xfree(gres_env->local_list);
	} else {
		unsetenvp(*gres_env->env_ptr, "OMPI_MCA_btl_openib_if_include");
	}
}

extern int init(void)
{
	debug("%s: %s loaded", __func__, plugin_name);

	return SLURM_SUCCESS;
}
extern int fini(void)
{
	debug("%s: unloading %s", __func__, plugin_name);
	FREE_NULL_LIST(gres_devices);

	return SLURM_SUCCESS;
}

/*
 * We could load gres state or validate it using various mechanisms here.
 * This only validates that the configuration was specified in gres.conf.
 * In the general case, no code would need to be changed.
 */
extern int gres_p_node_config_load(list_t *gres_conf_list,
				   node_config_load_t *config)
{
	int rc = SLURM_SUCCESS;

	if (gres_devices)
		return rc;

	rc = gres_node_config_load(gres_conf_list, config, &gres_devices);

	if (rc != SLURM_SUCCESS)
		fatal("%s failed to load configuration", plugin_name);

	return rc;
}

/*
 * Set environment variables as appropriate for a job (i.e. all tasks) based
 * upon the job's GRES state.
 */
extern void gres_p_job_set_env(char ***job_env_ptr,
			       bitstr_t *gres_bit_alloc,
			       uint64_t gres_cnt,
			       gres_internal_flags_t flags)
{
	common_gres_env_t gres_env = {
		.bit_alloc = gres_bit_alloc,
		.env_ptr = job_env_ptr,
		.flags = flags,
		.gres_cnt = gres_cnt,
		.gres_devices = gres_devices,
		.is_job = true,
	};

	_set_env(&gres_env);
}

/*
 * Set environment variables as appropriate for a job (i.e. all tasks) based
 * upon the job step's GRES state.
 */
extern void gres_p_step_set_env(char ***step_env_ptr,
				bitstr_t *gres_bit_alloc,
				uint64_t gres_cnt,
				gres_internal_flags_t flags)
{
	common_gres_env_t gres_env = {
		.bit_alloc = gres_bit_alloc,
		.env_ptr = step_env_ptr,
		.flags = flags,
		.gres_cnt = gres_cnt,
		.gres_devices = gres_devices,
	};

	_set_env(&gres_env);
}

/*
 * Reset environment variables as appropriate for a job (i.e. this one task)
 * based upon the job step's GRES state and assigned CPUs.
 */
extern void gres_p_task_set_env(char ***task_env_ptr,
				bitstr_t *gres_bit_alloc,
				uint64_t gres_cnt,
				bitstr_t *usable_gres,
				gres_internal_flags_t flags)
{
	common_gres_env_t gres_env = {
		.bit_alloc = gres_bit_alloc,
		.env_ptr = task_env_ptr,
		.flags = flags,
		.gres_cnt = gres_cnt,
		.gres_devices = gres_devices,
		.is_task = true,
		.usable_gres = usable_gres,
	};

	_set_env(&gres_env);
}

/* Send GRES information to slurmstepd on the specified file descriptor*/
extern void gres_p_send_stepd(buf_t *buffer)
{
	gres_send_stepd(buffer, gres_devices);
}

/* Receive GRES information from slurmd on the specified file descriptor */
extern void gres_p_recv_stepd(buf_t *buffer)
{
	gres_recv_stepd(buffer, &gres_devices);
}

/*
 * Return a list of devices of this type. The list elements are of type
 * "gres_device_t" and the list should be freed using FREE_NULL_LIST().
 */
extern list_t *gres_p_get_devices(void)
{
	return gres_devices;
}

extern void gres_p_step_hardware_init(bitstr_t *usable_gres, char *settings)
{
	return;
}

extern void gres_p_step_hardware_fini(void)
{
	return;
}

/*
 * Build record used to set environment variables as appropriate for a job's
 * prolog or epilog based GRES allocated to the job.
 */
extern gres_prep_t *gres_p_prep_build_env(
	gres_job_state_t *gres_js)
{
	return NULL;
}

/*
 * Set environment variables as appropriate for a job's prolog or epilog based
 * GRES allocated to the job.
 */
extern void gres_p_prep_set_env(char ***prep_env_ptr,
				gres_prep_t *gres_prep, int node_inx)
{
	return;
}
