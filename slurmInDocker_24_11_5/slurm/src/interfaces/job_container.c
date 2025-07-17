/*****************************************************************************\
 *  job_container_plugin.c - job container plugin stub.
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Written by Morris Jette
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

#include <pthread.h>

#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/job_container.h"

#include "src/slurmd/slurmstepd/slurmstepd_job.h"

typedef struct job_container_ops {
	int (*container_p_join)(slurm_step_id_t *step_id, uid_t uid,
				bool step_create);
	int	(*container_p_join_external)(uint32_t job_id);
	int	(*container_p_restore)	(char *dir_name, bool recover);
	int	(*container_p_stepd_create)	(uint32_t job_id,
					         stepd_step_rec_t *step);
	int	(*container_p_stepd_delete)	(uint32_t job_id);
	int	(*container_p_send_stepd)(int fd);
	int	(*container_p_recv_stepd)(int fd);

} job_container_ops_t;

/*
 * Must be synchronized with job_container_ops_t above.
 */
static const char *syms[] = {
	"container_p_join",
	"container_p_join_external",
	"container_p_restore",
	"container_p_stepd_create",
	"container_p_stepd_delete",
	"container_p_send_stepd",
	"container_p_recv_stepd",
};

static job_container_ops_t	*ops = NULL;
static plugin_context_t		**g_container_context = NULL;
static int			g_container_context_num = -1;
static pthread_mutex_t		g_container_context_lock =
					PTHREAD_MUTEX_INITIALIZER;

/*
 * Initialize the job container plugin.
 *
 * RET - slurm error code
 */
extern int job_container_init(void)
{
	int retval = SLURM_SUCCESS;
	char *plugin_type = "job_container";
	char *type = NULL, *last = NULL, *plugin_list, *job_container = NULL;

	slurm_mutex_lock(&g_container_context_lock);

	if (g_container_context_num >= 0)
		goto done;

	g_container_context_num = 0; /* mark it before anything else */
	if (!slurm_conf.job_container_plugin ||
	    !slurm_conf.job_container_plugin[0])
		goto done;

	type = plugin_list = xstrdup(slurm_conf.job_container_plugin);
	while ((job_container = strtok_r(plugin_list, ",", &last))) {
		xrecalloc(ops, g_container_context_num + 1,
			  sizeof(job_container_ops_t));
		xrecalloc(g_container_context, g_container_context_num + 1,
			  sizeof(plugin_context_t *));
		if (xstrncmp(job_container, "job_container/", 14) == 0)
			job_container += 14; /* backward compatibility */
		job_container = xstrdup_printf("job_container/%s",
					       job_container);
		g_container_context[g_container_context_num] =
			plugin_context_create(
				plugin_type, job_container,
				(void **)&ops[g_container_context_num],
				syms, sizeof(syms));
		if (!g_container_context[g_container_context_num]) {
			error("cannot create %s context for %s",
			      plugin_type, job_container);
			xfree(job_container);
			retval = SLURM_ERROR;
			break;
		}

		xfree(job_container);
		g_container_context_num++;
		plugin_list = NULL; /* for next iteration */
	}

 done:
	slurm_mutex_unlock(&g_container_context_lock);
	xfree(type);

	if (retval != SLURM_SUCCESS)
		job_container_fini();

	return retval;
}

/*
 * Terminate the job container plugin, free memory.
 *
 * RET - slurm error code
 */
extern int job_container_fini(void)
{
	int i, rc = SLURM_SUCCESS;

	slurm_mutex_lock(&g_container_context_lock);
	if (!g_container_context)
		goto done;

	for (i = 0; i < g_container_context_num; i++) {
		if (g_container_context[i]) {
			if (plugin_context_destroy(g_container_context[i])
			    != SLURM_SUCCESS) {
				rc = SLURM_ERROR;
			}
		}
	}

	xfree(ops);
	xfree(g_container_context);
	g_container_context_num = -1;

done:
	slurm_mutex_unlock(&g_container_context_lock);
	return rc;
}

/*
 * Add the calling process to the specified job's container.
 */
extern int container_g_join(slurm_step_id_t *step_id, uid_t uid,
			    bool step_create)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; ((i < g_container_context_num) && (rc == SLURM_SUCCESS));
	     i++) {
		rc = (*(ops[i].container_p_join))(step_id, uid, step_create);
	}

	return rc;
}

/*
 * Allow external processes (eg. via PAM) to join the job container.
 */
extern int container_g_join_external(uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; ((i < g_container_context_num) && (rc == SLURM_SUCCESS));
	     i++) {
		rc = (*(ops[i].container_p_join_external))(job_id);
	}

	return rc;
}

/* Restore container information */
extern int container_g_restore(char * dir_name, bool recover)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; ((i < g_container_context_num) && (rc == SLURM_SUCCESS));
	     i++) {
		rc = (*(ops[i].container_p_restore))(dir_name, recover);
	}

	return rc;
}

/* Create a container for the specified job, actions run in slurmstepd */
extern int container_g_stepd_create(uint32_t job_id, stepd_step_rec_t *step)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; ((i < g_container_context_num) && (rc == SLURM_SUCCESS));
	     i++) {
		rc = (*(ops[i].container_p_stepd_create))(job_id, step);
	}

	return rc;
}

/* Delete the container for the specified job, actions run in slurmstepd */
extern int container_g_stepd_delete(uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; ((i < g_container_context_num) && (rc == SLURM_SUCCESS));
	     i++) {
		rc = (*(ops[i].container_p_stepd_delete))(job_id);
	}

	return rc;
}

extern int container_g_send_stepd(int fd)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; (i < g_container_context_num) && (rc == SLURM_SUCCESS); i++)
		rc = (*(ops[i].container_p_send_stepd))(fd);

	return rc;
}

extern int container_g_recv_stepd(int fd)
{
	int i, rc = SLURM_SUCCESS;

	xassert(g_container_context_num >= 0);

	for (i = 0; (i < g_container_context_num) && (rc == SLURM_SUCCESS); i++)
		rc = (*(ops[i].container_p_recv_stepd))(fd);

	return rc;
}
