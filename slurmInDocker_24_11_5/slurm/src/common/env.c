/*****************************************************************************\
 *  src/common/env.c - add an environment variable to environment vector
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>, Danny Auble <da@llnl.gov>.
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

#define _GNU_SOURCE /* For clone */
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "src/common/cpu_frequency.h"
#include "src/common/log.h"
#include "src/common/env.h"
#include "src/common/fd.h"
#include "src/common/macros.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/interfaces/select.h"
#include "src/common/slurm_opt.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_step_layout.h"
#include "src/common/slurmdb_defs.h"
#include "src/common/spank.h"
#include "src/common/strlcpy.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

/*
 * Define slurm-specific aliases for use by plugins, see slurm_xlator.h
 * for details.
 */
strong_alias(setenvf,			slurm_setenvpf);
strong_alias(unsetenvp,			slurm_unsetenvp);
strong_alias(getenvp,			slurm_getenvp);
strong_alias(env_array_create,		slurm_env_array_create);
strong_alias(env_array_merge,		slurm_env_array_merge);
strong_alias(env_array_copy,		slurm_env_array_copy);
strong_alias(env_array_free,		slurm_env_array_free);
strong_alias(env_array_append,		slurm_env_array_append);
strong_alias(env_array_append_fmt,	slurm_env_array_append_fmt);
strong_alias(env_array_overwrite,	slurm_env_array_overwrite);
strong_alias(env_array_overwrite_fmt,	slurm_env_array_overwrite_fmt);
strong_alias(env_array_overwrite_het_fmt, slurm_env_array_overwrite_het_fmt);
strong_alias(env_unset_environment,	slurm_env_unset_environment);

#define ENV_BUFSIZE (256 * 1024)
#define MAX_ENV_STRLEN (32 * 4096)	/* Needed for CPU_BIND and MEM_BIND on
					 * SGI systems with huge CPU counts */
typedef struct {
	char *cmdstr;
	int *fildes;
	int mode;
	bool perform_mount;
	int rlimit;
	char **tmp_env;
	const char *username;
} child_args_t;

/*
 *  Return pointer to `name' entry in environment if found, or
 *   pointer to the last entry (i.e. NULL) if `name' is not
 *   currently set in `env'
 *
 */
static char **
_find_name_in_env(char **env, const char *name)
{
	char **ep;

	ep = env;
	while (*ep != NULL) {
		size_t cnt = 0;

		while ( ((*ep)[cnt] == name[cnt])
		      && ( name[cnt] != '\0')
		      && ((*ep)[cnt] != '\0')    )
			++cnt;

		if (name[cnt] == '\0' && (*ep)[cnt] == '=') {
			break;
		} else
			++ep;
	}

	return (ep);
}

/*
 *  Extend memory allocation for env by 1 entry. Make last entry == NULL.
 *   return pointer to last env entry;
 */
static char **
_extend_env(char ***envp)
{
	char **ep;
	size_t newcnt = PTR_ARRAY_SIZE(*envp) + 1;

	*envp = xrealloc (*envp, newcnt * sizeof (char *));

	(*envp)[newcnt - 1] = NULL;
	ep = &((*envp)[newcnt - 2]);

	/*
	 *  Find last non-NULL entry
	 */
	while (*ep == NULL)
		--ep;

	return (++ep);
}

/* return true if the environment variables should not be set for
 *	srun's --get-user-env option */
static bool _discard_env(char *name, char *value)
{
	if ((xstrcmp(name, "DISPLAY")     == 0) ||
	    (xstrcmp(name, "ENVIRONMENT") == 0) ||
	    (xstrcmp(name, "HOSTNAME")    == 0))
		return true;

	return false;
}

/*
 * Return the number of elements in the environment `env'
 */
int
envcount (char **env)
{
	int envc = 0;
	while (env && env[envc])
		envc++;
	return (envc);
}

/*
 * _setenvfs() (stolen from pdsh)
 *
 * Set a variable in the callers environment.  Args are printf style.
 * XXX Space is allocated on the heap and will never be reclaimed.
 * Example: setenvfs("RMS_RANK=%d", rank);
 */
int
setenvfs(const char *fmt, ...)
{
	va_list ap;
	char *buf, *bufcpy, *loc;
	int rc, size;

	buf = xmalloc(ENV_BUFSIZE);
	va_start(ap, fmt);
	vsnprintf(buf, ENV_BUFSIZE, fmt, ap);
	va_end(ap);

	size = strlen(buf);
	bufcpy = xstrdup(buf);
	xfree(buf);

	if (size >= MAX_ENV_STRLEN) {
		if ((loc = strchr(bufcpy, '=')))
			loc[0] = '\0';
		error("environment variable %s is too long", bufcpy);
		xfree(bufcpy);
		rc = ENOMEM;
	} else {
		rc = putenv(bufcpy);
	}

	return rc;
}

int setenvf(char ***envp, const char *name, const char *fmt, ...)
{
	char *value;
	va_list ap;
	int size, rc;

	if (!name || name[0] == '\0')
		return EINVAL;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, fmt);
	vsnprintf(value, ENV_BUFSIZE, fmt, ap);
	va_end(ap);

	size = strlen(name) + strlen(value) + 2;
	if (size >= MAX_ENV_STRLEN) {
		error("environment variable %s is too long", name);
		return ENOMEM;
	}

	if (envp && *envp) {
		if (env_array_overwrite(envp, name, value) == 1)
			rc = 0;
		else
			rc = 1;
	} else {
		rc = setenv(name, value, 1);
	}

	xfree(value);
	return rc;
}

/*
 *  Remove environment variable `name' from "environment"
 *   contained in `env'
 *
 *  [ This was taken almost verbatim from glibc's
 *    unsetenv()  code. ]
 */
void unsetenvp(char **env, const char *name)
{
	char **ep;

	if (env == NULL)
		return;

	ep = env;
	while ((ep = _find_name_in_env (ep, name)) && (*ep != NULL)) {
		char **dp = ep;
		xfree (*ep);
		do
			dp[0] = dp[1];
		while (*dp++);

		/*  Continue loop in case `name' appears again. */
		++ep;
	}
	return;
}

char *getenvp(char **env, const char *name)
{
	size_t len;
	char **ep;

	if (!name || !env || !env[0])
		return (NULL);

	len = strlen(name);
	ep = _find_name_in_env (env, name);

	if (*ep != NULL)
		return (&(*ep)[len+1]);

	return NULL;
}

int setup_env(env_t *env, bool preserve_env)
{
	int rc = SLURM_SUCCESS;
	char *dist = NULL;
	char addrbuf[INET6_ADDRSTRLEN];

	if (env == NULL)
		return SLURM_ERROR;

	/*
	 * Always force SLURM_CONF into the environment. This ensures the
	 * "configless" operation is working, and prevents the client commands
	 * from falling back to separate RPC requests in case the cache dir
	 * is unresponsive.
	 */
	if (setenvf(&env->env, "SLURM_CONF", "%s", getenv("SLURM_CONF"))) {
		error("Unable to set SLURM_CONF environment variable");
		rc = SLURM_ERROR;
	}
	/*
	 * Similarly, prevent this option from leaking in. SLURM_CONF would
	 * always take precedence, but tidy it up anyways.
	 */
	unsetenvp(env->env, "SLURM_CONF_SERVER");

	if (!preserve_env && env->ntasks) {
		if (setenvf(&env->env, "SLURM_NTASKS", "%d", env->ntasks)) {
			error("Unable to set SLURM_NTASKS environment variable");
			rc = SLURM_ERROR;
		}
		if (setenvf(&env->env, "SLURM_NPROCS", "%d", env->ntasks)) {
			error("Unable to set SLURM_NPROCS environment variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->cpus_per_task &&
	    setenvf(&env->env, "SLURM_CPUS_PER_TASK", "%d",
		    env->cpus_per_task) ) {
		error("Unable to set SLURM_CPUS_PER_TASK");
		rc = SLURM_ERROR;
	}

	if (env->ntasks_per_gpu &&
	    setenvf(&env->env, "SLURM_NTASKS_PER_GPU", "%d",
		    env->ntasks_per_gpu)) {
		error("Unable to set SLURM_NTASKS_PER_GPU");
		rc = SLURM_ERROR;
	}

	if (env->ntasks_per_node
	   && setenvf(&env->env, "SLURM_NTASKS_PER_NODE", "%d",
		      env->ntasks_per_node) ) {
		error("Unable to set SLURM_NTASKS_PER_NODE");
		rc = SLURM_ERROR;
	}

	if (env->ntasks_per_socket
	   && setenvf(&env->env, "SLURM_NTASKS_PER_SOCKET", "%d",
		      env->ntasks_per_socket) ) {
		error("Unable to set SLURM_NTASKS_PER_SOCKET");
		rc = SLURM_ERROR;
	}

	if (env->ntasks_per_core
	   && setenvf(&env->env, "SLURM_NTASKS_PER_CORE", "%d",
		      env->ntasks_per_core) ) {
		error("Unable to set SLURM_NTASKS_PER_CORE");
		rc = SLURM_ERROR;
	}

	if (env->ntasks_per_tres
	    && setenvf(&env->env, "SLURM_NTASKS_PER_TRES", "%d",
		      env->ntasks_per_tres) ) {
		error("Unable to set SLURM_NTASKS_PER_TRES");
		rc = SLURM_ERROR;
	}

	if (env->cpus_on_node
	   && setenvf(&env->env, "SLURM_CPUS_ON_NODE", "%d",
		      env->cpus_on_node) ) {
		error("Unable to set SLURM_CPUS_ON_NODE");
		rc = SLURM_ERROR;
	}

	set_distribution(env->distribution, &dist);
	if (dist) {
		if (setenvf(&env->env, "SLURM_DISTRIBUTION", "%s", dist)) {
			error("Can't set SLURM_DISTRIBUTION env variable");
			rc = SLURM_ERROR;
		}
		xfree(dist);
	}

	if ((env->distribution & SLURM_DIST_STATE_BASE) == SLURM_DIST_PLANE)
		if (setenvf(&env->env, "SLURM_DIST_PLANESIZE", "%u",
			    env->plane_size)) {
			error("Can't set SLURM_DIST_PLANESIZE env variable");
			rc = SLURM_ERROR;
		}

	if (env->cpu_bind_type && !env->batch_flag &&
	    (env->stepid != SLURM_INTERACTIVE_STEP)) {
		char *str_verbose, *str_bind1 = NULL, *str_bind2 = NULL;
		char *str_bind_list, *str_bind_type = NULL, *str_bind = NULL;
		bool append_cpu_bind = false;

		unsetenvp(env->env, "SLURM_CPU_BIND");
		unsetenvp(env->env, "SLURM_CPU_BIND_LIST");
		unsetenvp(env->env, "SLURM_CPU_BIND_TYPE");
		unsetenvp(env->env, "SLURM_CPU_BIND_VERBOSE");

		if (env->cpu_bind_type & CPU_BIND_VERBOSE)
			str_verbose = "verbose";
		else
			str_verbose = "quiet";

		if (env->cpu_bind_type & CPU_BIND_TO_THREADS) {
			str_bind1 = "threads";
		} else if (env->cpu_bind_type & CPU_BIND_TO_CORES) {
			str_bind1 = "cores";
		} else if (env->cpu_bind_type & CPU_BIND_TO_SOCKETS) {
			str_bind1 = "sockets";
		} else if (env->cpu_bind_type & CPU_BIND_TO_LDOMS) {
			str_bind1 = "ldoms";
		}

		if (env->cpu_bind_type & CPU_BIND_NONE) {
			str_bind2 = "none";
		} else if (env->cpu_bind_type & CPU_BIND_MAP) {
			str_bind2 = "map_cpu:";
			append_cpu_bind = true;
		} else if (env->cpu_bind_type & CPU_BIND_MASK) {
			str_bind2 = "mask_cpu:";
			append_cpu_bind = true;
		} else if (env->cpu_bind_type & CPU_BIND_LDRANK) {
			str_bind2 = "rank_ldom";
		} else if (env->cpu_bind_type & CPU_BIND_LDMAP) {
			str_bind2 = "map_ldom:";
			append_cpu_bind = true;
		} else if (env->cpu_bind_type & CPU_BIND_LDMASK) {
			str_bind2 = "mask_ldom:";
			append_cpu_bind = true;
		}

		if (env->cpu_bind && append_cpu_bind)
			str_bind_list = env->cpu_bind;
		else
			str_bind_list = "";

		/* combine first and second part with a comma if needed */
		if (str_bind1)
			xstrcat(str_bind_type, str_bind1);
		if (str_bind1 && str_bind2)
			xstrcatchar(str_bind_type, ',');
		if (str_bind2)
			xstrcat(str_bind_type, str_bind2);

		xstrcat(str_bind, str_verbose);
		if (str_bind_type) {
			xstrcatchar(str_bind, ',');
			xstrcat(str_bind, str_bind_type);
			xstrcat(str_bind, str_bind_list);
		} else
			str_bind_type = xstrdup("");

		if (setenvf(&env->env, "SLURM_CPU_BIND", "%s", str_bind)) {
			error("Unable to set SLURM_CPU_BIND");
			rc = SLURM_ERROR;
		}
		if (setenvf(&env->env, "SLURM_CPU_BIND_LIST", "%s",
			    str_bind_list)) {
			error("Unable to set SLURM_CPU_BIND_LIST");
			rc = SLURM_ERROR;
		}
		if (setenvf(&env->env, "SLURM_CPU_BIND_TYPE", "%s",
			    str_bind_type)) {
			error("Unable to set SLURM_CPU_BIND_TYPE");
			rc = SLURM_ERROR;
		}
		if (setenvf(&env->env, "SLURM_CPU_BIND_VERBOSE", "%s",
			    str_verbose)) {
			error("Unable to set SLURM_CPU_BIND_VERBOSE");
			rc = SLURM_ERROR;
		}

		xfree(str_bind);
		xfree(str_bind_type);
	}

	if (env->mem_bind_type && (env->stepid != SLURM_INTERACTIVE_STEP)) {
		char *str_verbose, *str_bind_type = NULL, *str_bind_list;
		char *str_prefer = NULL, *str_bind = NULL;
		char *str_bind_sort = NULL;

		if (env->batch_flag) {
			unsetenvp(env->env, "SBATCH_MEM_BIND");
			unsetenvp(env->env, "SBATCH_MEM_BIND_LIST");
			unsetenvp(env->env, "SBATCH_MEM_BIND_PREFER");
			unsetenvp(env->env, "SBATCH_MEM_BIND_TYPE");
			unsetenvp(env->env, "SBATCH_MEM_BIND_VERBOSE");
		} else {
			unsetenvp(env->env, "SLURM_MEM_BIND");
			unsetenvp(env->env, "SLURM_MEM_BIND_LIST");
			unsetenvp(env->env, "SLURM_MEM_BIND_PREFER");
			unsetenvp(env->env, "SLURM_MEM_BIND_SORT");
			unsetenvp(env->env, "SLURM_MEM_BIND_TYPE");
			unsetenvp(env->env, "SLURM_MEM_BIND_VERBOSE");
		}

		if (env->mem_bind_type & MEM_BIND_VERBOSE)
			str_verbose = "verbose";
		else
			str_verbose = "quiet";
		if (env->mem_bind_type & MEM_BIND_PREFER)
			str_prefer = "prefer";
		if (env->mem_bind_type & MEM_BIND_NONE) {
			str_bind_type = "none";
		} else if (env->mem_bind_type & MEM_BIND_RANK) {
			str_bind_type = "rank";
		} else if (env->mem_bind_type & MEM_BIND_MAP) {
			str_bind_type = "map_mem:";
		} else if (env->mem_bind_type & MEM_BIND_MASK) {
			str_bind_type = "mask_mem:";
		} else if (env->mem_bind_type & MEM_BIND_LOCAL) {
			str_bind_type = "local";
		}

		if (env->mem_bind_type & MEM_BIND_SORT)
			str_bind_sort = "sort";

		if (env->mem_bind)
			str_bind_list = env->mem_bind;
		else
			str_bind_list = "";

		xstrcat(str_bind, str_verbose);
		if (str_prefer) {
			xstrcatchar(str_bind, ',');
			xstrcat(str_bind, str_prefer);
		}
		if (str_bind_type) {
			xstrcatchar(str_bind, ',');
			xstrcat(str_bind, str_bind_type);
			xstrcat(str_bind, str_bind_list);
		} else
			str_bind_type = "";

		if (env->batch_flag) {
			if (setenvf(&env->env, "SBATCH_MEM_BIND", "%s", str_bind)) {
				error("Unable to set SBATCH_MEM_BIND");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND_LIST", "%s",
				    str_bind_list)) {
				error("Unable to set SBATCH_MEM_BIND_LIST");
				rc = SLURM_ERROR;
			}
			if (str_prefer &&
			    setenvf(&env->env, "SBATCH_MEM_BIND_PREFER", "%s",
				    str_prefer)) {
				error("Unable to set SBATCH_MEM_BIND_PREFER");
				rc = SLURM_ERROR;
			}
			if (str_bind_sort &&
			    setenvf(&env->env, "SBATCH_MEM_BIND_SORT", "%s",
				    str_bind_sort)) {
				error("Unable to set SBATCH_MEM_BIND_SORT");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND_TYPE", "%s",
				    str_bind_type)) {
				error("Unable to set SBATCH_MEM_BIND_TYPE");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND_VERBOSE", "%s",
				    str_verbose)) {
				error("Unable to set SBATCH_MEM_BIND_VERBOSE");
				rc = SLURM_ERROR;
			}
		} else {
			if (setenvf(&env->env, "SLURM_MEM_BIND", "%s", str_bind)) {
				error("Unable to set SLURM_MEM_BIND");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND_LIST", "%s",
				    str_bind_list)) {
				error("Unable to set SLURM_MEM_BIND_LIST");
				rc = SLURM_ERROR;
			}
			if (str_prefer &&
			    setenvf(&env->env, "SLURM_MEM_BIND_PREFER", "%s",
				    str_prefer)) {
				error("Unable to set SLURM_MEM_BIND_PREFER");
				rc = SLURM_ERROR;
			}
			if (str_bind_sort &&
			    setenvf(&env->env, "SLURM_MEM_BIND_SORT", "%s",
				    str_bind_sort)) {
				error("Unable to set SLURM_MEM_BIND_SORT");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND_TYPE", "%s",
				    str_bind_type)) {
				error("Unable to set SLURM_MEM_BIND_TYPE");
				rc = SLURM_ERROR;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND_VERBOSE", "%s",
				    str_verbose)) {
				error("Unable to set SLURM_MEM_BIND_VERBOSE");
				rc = SLURM_ERROR;
			}
		}

		xfree(str_bind);
	}

	if (cpu_freq_set_env("SLURM_CPU_FREQ_REQ", env->cpu_freq_min,
			env->cpu_freq_max, env->cpu_freq_gov) != SLURM_SUCCESS)
		rc = SLURM_ERROR;

	if (env->overcommit
	    && (setenvf(&env->env, "SLURM_OVERCOMMIT", "%s", "1"))) {
		error("Unable to set SLURM_OVERCOMMIT environment variable");
		rc = SLURM_ERROR;
	}

	if (env->oom_kill_step != NO_VAL16 &&
	    setenvf(&env->env, "SLURM_OOM_KILL_STEP", "%u", env->oom_kill_step)) {
		error("Unable to set SLURM_OOM_KILL_STEP environment");
		rc = SLURM_ERROR;
	}

	if (env->slurmd_debug
	    && setenvf(&env->env, "SLURMD_DEBUG", "%d", env->slurmd_debug)) {
		error("Can't set SLURMD_DEBUG environment variable");
		rc = SLURM_ERROR;
	}

	if (env->labelio
	   && setenvf(&env->env, "SLURM_LABELIO", "1")) {
		error("Unable to set SLURM_LABELIO environment variable");
		rc = SLURM_ERROR;
	}

	if (env->job_end_time) {
		if (setenvf(&env->env, "SLURM_JOB_END_TIME", "%lu",
			    env->job_end_time)) {
			error("Unable to set SLURM_JOB_END_TIME environment variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->jobid >= 0) {
		if (setenvf(&env->env, "SLURM_JOB_ID", "%d", env->jobid)) {
			error("Unable to set SLURM_JOB_ID environment");
			rc = SLURM_ERROR;
		}
		/* and for backwards compatibility... */
		if (setenvf(&env->env, "SLURM_JOBID", "%d", env->jobid)) {
			error("Unable to set SLURM_JOBID environment");
			rc = SLURM_ERROR;
		}
	}

	if (env->job_licenses) {
		if (setenvf(&env->env, "SLURM_JOB_LICENSES", "%s",
			    env->job_licenses)) {
			error("Unable to set SLURM_JOB_LICENSES environment");
			rc = SLURM_ERROR;
		}
	}

	if (env->job_name) {
		if (setenvf(&env->env, "SLURM_JOB_NAME", "%s", env->job_name)) {
			error("Unable to set SLURM_JOB_NAME environment");
			rc = SLURM_ERROR;
		}
	}

	if (env->job_start_time) {
		if (setenvf(&env->env, "SLURM_JOB_START_TIME", "%lu",
			    env->job_start_time)) {
			error("Unable to set SLURM_JOB_START_TIME environment");
			rc = SLURM_ERROR;
		}
	}

	/*
	 * These aren't relevant to a system not using Slurm as the
	 * launcher. Since there isn't a flag for that we check for
	 * the flags we do have.
	 */
	if (env->task_pid &&
	    setenvf(&env->env, "SLURM_TASK_PID", "%d",
		       (int)env->task_pid)) {
		error("Unable to set SLURM_TASK_PID environment "
		      "variable");
		rc = SLURM_ERROR;
	}
	if ((env->nodeid >= 0) &&
	    setenvf(&env->env, "SLURM_NODEID", "%d", env->nodeid)) {
		error("Unable to set SLURM_NODEID environment");
		rc = SLURM_ERROR;
	}

	if ((env->procid >= 0) &&
	    setenvf(&env->env, "SLURM_PROCID", "%d", env->procid)) {
		error("Unable to set SLURM_PROCID environment");
		rc = SLURM_ERROR;
	}

	if ((env->localid >= 0) &&
	    setenvf(&env->env, "SLURM_LOCALID", "%d", env->localid)) {
		error("Unable to set SLURM_LOCALID environment");
		rc = SLURM_ERROR;
	}

	if (env->stepid >= 0) {
		if (setenvf(&env->env, "SLURM_STEP_ID", "%d", env->stepid)) {
			error("Unable to set SLURM_STEP_ID environment");
			rc = SLURM_ERROR;
		}
		/* and for backwards compatibility... */
		if (setenvf(&env->env, "SLURM_STEPID", "%d", env->stepid)) {
			error("Unable to set SLURM_STEPID environment");
			rc = SLURM_ERROR;
		}
	}

	if (!preserve_env && env->nhosts
	    && setenvf(&env->env, "SLURM_NNODES", "%d", env->nhosts)) {
		error("Unable to set SLURM_NNODES environment var");
		rc = SLURM_ERROR;
	}

	if (env->nhosts
	    && setenvf(&env->env, "SLURM_JOB_NUM_NODES", "%d", env->nhosts)) {
		error("Unable to set SLURM_JOB_NUM_NODES environment var");
		rc = SLURM_ERROR;
	}

	if (env->nodelist &&
	    setenvf(&env->env, "SLURM_NODELIST", "%s", env->nodelist)) {
		error("Unable to set SLURM_NODELIST environment var.");
		rc = SLURM_ERROR;
	}

	if (env->partition
	    && setenvf(&env->env, "SLURM_JOB_PARTITION", "%s", env->partition)) {
		error("Unable to set SLURM_JOB_PARTITION environment var.");
		rc = SLURM_ERROR;
	}

	if (!preserve_env && env->task_count
	    && setenvf (&env->env,
			"SLURM_TASKS_PER_NODE", "%s", env->task_count)) {
		error ("Can't set SLURM_TASKS_PER_NODE env variable");
		rc = SLURM_ERROR;
	}

	if (!preserve_env && env->threads_per_core &&
	    setenvf(&env->env, "SLURM_THREADS_PER_CORE", "%d",
		    env->threads_per_core)) {
		error("Can't set SLURM_THREADS_PER_CORE env variable");
		rc = SLURM_ERROR;
	}

	if (env->comm_port
	    && setenvf (&env->env, "SLURM_SRUN_COMM_PORT", "%u",
			env->comm_port)) {
		error ("Can't set SLURM_SRUN_COMM_PORT env variable");
		rc = SLURM_ERROR;
	}

	if (env->cli) {
		slurm_get_ip_str(env->cli, addrbuf, INET6_ADDRSTRLEN);
		setenvf(&env->env, "SLURM_LAUNCH_NODE_IPADDR", "%s", addrbuf);
	}

	if (env->sgtids &&
	    setenvf(&env->env, "SLURM_GTIDS", "%s", env->sgtids)) {
		error("Unable to set SLURM_GTIDS environment variable");
		rc = SLURM_ERROR;
	}

	if (env->pty_port
	&&  setenvf(&env->env, "SLURM_PTY_PORT", "%hu", env->pty_port)) {
		error("Can't set SLURM_PTY_PORT env variable");
		rc = SLURM_ERROR;
	}
	if (env->ws_col
	&&  setenvf(&env->env, "SLURM_PTY_WIN_COL", "%hu", env->ws_col)) {
		error("Can't set SLURM_PTY_WIN_COL env variable");
		rc = SLURM_ERROR;
	}
	if (env->ws_row
	&&  setenvf(&env->env, "SLURM_PTY_WIN_ROW", "%hu", env->ws_row)) {
		error("Can't set SLURM_PTY_WIN_ROW env variable");
		rc = SLURM_ERROR;
	}

	if (env->restart_cnt &&
	    setenvf(&env->env, "SLURM_RESTART_COUNT", "%u", env->restart_cnt)) {
		error("Can't set SLURM_RESTART_COUNT env variable");
		rc = SLURM_ERROR;
	}

	if (env->uid != SLURM_AUTH_NOBODY) {
		if (setenvf(&env->env, "SLURM_JOB_UID", "%u",
			    (unsigned int) env->uid)) {
			error("Can't set SLURM_JOB_UID env variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->user_name) {
		if (setenvf(&env->env, "SLURM_JOB_USER", "%s", env->user_name)){
			error("Can't set SLURM_JOB_USER env variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->gid != SLURM_AUTH_NOBODY) {
		if (setenvf(&env->env, "SLURM_JOB_GID", "%u", env->gid)) {
			error("Can't set SLURM_JOB_GID env variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->group_name) {
		if (setenvf(&env->env, "SLURM_JOB_GROUP", "%s",
			    env->group_name)) {
			error("Can't set SLURM_JOB_GROUP env variable");
			rc = SLURM_ERROR;
		}
	}

	if (env->account) {
		if (setenvf(&env->env,
			    "SLURM_JOB_ACCOUNT",
			    "%s",
			    env->account)) {
			error("%s: can't set SLURM_JOB_ACCOUNT env variable",
			      __func__);
			rc = SLURM_ERROR;
		}
	}
	if (env->qos) {
		if (setenvf(&env->env,
			    "SLURM_JOB_QOS",
			    "%s",
			    env->qos)) {
			error("%s: can't set SLURM_JOB_QOS env variable",
				__func__);
			rc = SLURM_ERROR;
		}
	}
	if (env->resv_name) {
		if (setenvf(&env->env,
			    "SLURM_JOB_RESERVATION",
			    "%s",
			    env->resv_name)) {
			error("%s: can't set SLURM_JOB_RESERVATION env variable",
				__func__);
			rc = SLURM_ERROR;
		}
	}

	return rc;
}

/**********************************************************************
 * From here on are the new environment variable management functions,
 * used by the "new" commands: salloc, sbatch, and the step launch APIs.
 **********************************************************************/

/*
 * Return a string representation of an array of uint16_t elements.
 * Each value in the array is printed in decimal notation and elements
 * are separated by a comma.  If sequential elements in the array
 * contain the same value, the value is written out just once followed
 * by "(xN)", where "N" is the number of times the value is repeated.
 *
 * Example:
 *   The array "1, 2, 1, 1, 1, 3, 2" becomes the string "1,2,1(x3),3,2"
 *
 * Returns an xmalloc'ed string.  Free with xfree().
 */
extern char *uint16_array_to_str(int array_len, const uint16_t *array)
{
	int i;
	int previous = 0;
	char *sep = ",";  /* seperator */
	char *str = xstrdup("");

	if (array == NULL)
		return str;

	for (i = 0; i < array_len; i++) {
		if ((i+1 < array_len) && (array[i] == array[i+1])) {
			previous++;
			continue;
		}

		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (previous > 0) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], previous+1, sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
		previous = 0;
	}

	return str;
}


/*
 * The cpus-per-node representation in Slurm (and perhaps tasks-per-node
 * in the future) is stored in a compressed format comprised of two
 * equal-length arrays, and an integer holding the array length.  In one
 * array an element represents a count (number of cpus, number of tasks,
 * etc.), and the corresponding element in the other array contains the
 * number of times the count is repeated sequentially in the uncompressed
 * something-per-node array.
 *
 * This function returns the string representation of the compressed
 * array.  Free with xfree().
 */
extern char *uint32_compressed_to_str(uint32_t array_len,
				      const uint16_t *array,
				      const uint32_t *array_reps)
{
	int i;
	char *sep = ","; /* seperator */
	char *str = xstrdup("");

	if (!array || !array_reps)
		return str;

	for (i = 0; i < array_len; i++) {
		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (array_reps[i] > 1) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], array_reps[i], sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
	}

	return str;
}

/*
 * Set in "dest" the environment variables relevant to a Slurm job
 * allocation, overwriting any environment variables of the same name.
 * If the address pointed to by "dest" is NULL, memory will automatically be
 * xmalloc'ed.  The array is terminated by a NULL pointer, and thus is
 * suitable for use by execle() and other env_array_* functions.
 *
 * Sets the variables:
 *	SLURM_JOB_ID
 *	SLURM_JOB_NAME
 *	SLURM_JOB_NUM_NODES
 *	SLURM_JOB_NODELIST
 *	SLURM_JOB_CPUS_PER_NODE
 *	SLURM_NTASKS_PER_NODE
 *
 * dest OUT - array in which to the set environment variables
 * alloc IN - resource allocation response
 * desc IN - job allocation request
 * het_job_offset IN - component offset into hetjob, -1 if not hetjob
 *
 * Sets OBSOLETE variables (needed for MPI, do not remove):
 *	SLURM_JOBID
 *	SLURM_NNODES
 *	SLURM_NODELIST
 *	SLURM_NPROCS
 *	SLURM_TASKS_PER_NODE
 */
extern int env_array_for_job(char ***dest,
			     const resource_allocation_response_msg_t *alloc,
			     const job_desc_msg_t *desc, int het_job_offset)
{
	char *tmp = NULL;
	char *dist = NULL;
	char *key, *value;
	slurm_step_layout_t *step_layout = NULL;
	int i, rc = SLURM_SUCCESS;
	slurm_step_layout_req_t step_layout_req;
	uint16_t cpus_per_task_array[1];
	uint32_t cpus_task_reps[1];

	if (!alloc || !desc)
		return SLURM_ERROR;

	memset(&step_layout_req, 0, sizeof(slurm_step_layout_req_t));
	step_layout_req.num_tasks = desc->num_tasks;
	step_layout_req.num_hosts = alloc->node_cnt;
	cpus_per_task_array[0] = desc->cpus_per_task;
	cpus_task_reps[0] = alloc->node_cnt;

	if (het_job_offset < 1) {
		env_array_overwrite_fmt(dest, "SLURM_JOB_ID", "%u",
					alloc->job_id);
	}
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_ID", het_job_offset,
				    "%u", alloc->job_id);
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_NAME", het_job_offset,
				    "%s", desc->name);
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_NUM_NODES", het_job_offset,
				    "%u", step_layout_req.num_hosts);
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_NODELIST", het_job_offset,
				    "%s", alloc->node_list);
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_PARTITION", het_job_offset,
				    "%s", alloc->partition);

	set_distribution(desc->task_dist, &dist);
	if (dist) {
		env_array_overwrite_het_fmt(dest, "SLURM_DISTRIBUTION",
					    het_job_offset, "%s", dist);
		xfree(dist);
	}
	if ((desc->task_dist & SLURM_DIST_STATE_BASE) == SLURM_DIST_PLANE) {
		env_array_overwrite_het_fmt(dest, "SLURM_DIST_PLANESIZE",
					    het_job_offset, "%u",
					    desc->plane_size);
	}
	tmp = uint32_compressed_to_str(alloc->num_cpu_groups,
					alloc->cpus_per_node,
					alloc->cpu_count_reps);
	env_array_overwrite_het_fmt(dest, "SLURM_JOB_CPUS_PER_NODE",
				    het_job_offset, "%s", tmp);
	xfree(tmp);

	if (desc->threads_per_core != NO_VAL16)
		env_array_overwrite_het_fmt(dest, "SLURM_THREADS_PER_CORE",
					    het_job_offset, "%d",
					    desc->threads_per_core);

	if (alloc->pn_min_memory & MEM_PER_CPU) {
		uint64_t tmp_mem = alloc->pn_min_memory & (~MEM_PER_CPU);
		env_array_overwrite_het_fmt(dest, "SLURM_MEM_PER_CPU",
					    het_job_offset, "%"PRIu64"",
					    tmp_mem);
	} else if (alloc->pn_min_memory) {
		uint64_t tmp_mem = alloc->pn_min_memory;
		env_array_overwrite_het_fmt(dest, "SLURM_MEM_PER_NODE",
					    het_job_offset, "%"PRIu64"",
					    tmp_mem);
	}

	/* OBSOLETE, but needed by MPI, do not remove */
	env_array_overwrite_het_fmt(dest, "SLURM_JOBID", het_job_offset, "%u",
				    alloc->job_id);
	env_array_overwrite_het_fmt(dest, "SLURM_NNODES", het_job_offset, "%u",
				    step_layout_req.num_hosts);
	env_array_overwrite_het_fmt(dest, "SLURM_NODELIST", het_job_offset, "%s",
				    alloc->node_list);

	/*
	 * --ntasks-per-node no-longer sets num_tasks implicitly, so we need
	 * need to calculate num_tasks here to make sure the environment
	 * variable is correct.
	 *
	 * --ntasks-per-tres still implicitly sets ntasks.
	 * --ntasks-per-socket requires --ntasks in order to work.
	 * So neither need to be accounted for here.
	 *
	 * SLURM_TASKS_PER_NODE is used by mpirun so it must be set correctly.
	 */
	if ((step_layout_req.num_tasks == NO_VAL) &&
	    desc->ntasks_per_node && (desc->ntasks_per_node != NO_VAL16)) {
		step_layout_req.num_tasks =
			desc->ntasks_per_node * alloc->node_cnt;
	}

	/*
	 * If we know how many tasks we are going to do then we set
	 * SLURM_TASKS_PER_NODE. If no tasks were given we can figure it out
	 * here by totalling up the number of tasks each node can hold (which is
	 * the cpus in a node divided by the number of cpus per task).
	 */
	if (step_layout_req.num_tasks == NO_VAL) {
		step_layout_req.num_tasks = 0;

		/* Iterate over all kind of cluster nodes. */
		for (int i = 0; i < alloc->num_cpu_groups; i++) {
			/* Get the CPU count for this type of nodes. */
			uint32_t ntasks = alloc->cpus_per_node[i];

			/*
			 * If CPUs/tasks is set, determine how many tasks a node
			 * of this type can hold.
			 */
			if ((desc->cpus_per_task != NO_VAL16) &&
			    (desc->cpus_per_task > 1))
				ntasks /= desc->cpus_per_task;

			/* Accum. the number of tasks all the group can hold. */
			step_layout_req.num_tasks += ntasks *
						     alloc->cpu_count_reps[i];
		}
	}

	if ((desc->task_dist & SLURM_DIST_STATE_BASE) == SLURM_DIST_ARBITRARY) {
		step_layout_req.node_list = desc->req_nodes;
		env_array_overwrite_het_fmt(dest, "SLURM_ARBITRARY_NODELIST",
					    het_job_offset, "%s",
					     step_layout_req.node_list);
	} else
		step_layout_req.node_list = alloc->node_list;

	step_layout_req.cpus_per_node = alloc->cpus_per_node;
	step_layout_req.cpu_count_reps = alloc->cpu_count_reps;
	step_layout_req.cpus_per_task = cpus_per_task_array;
	step_layout_req.cpus_task_reps = cpus_task_reps;
	step_layout_req.task_dist = desc->task_dist;
	step_layout_req.plane_size = desc->plane_size;

	if (!(step_layout = slurm_step_layout_create(&step_layout_req)))
		return SLURM_ERROR;

	tmp = uint16_array_to_str(step_layout->node_cnt, step_layout->tasks);
	slurm_step_layout_destroy(step_layout);
	env_array_overwrite_het_fmt(dest, "SLURM_TASKS_PER_NODE",
				    het_job_offset,
				    "%s", tmp);
	xfree(tmp);

	if (alloc->account) {
		env_array_overwrite_het_fmt(dest, "SLURM_JOB_ACCOUNT",
					    het_job_offset, "%s",
					    alloc->account);
	}
	if (alloc->qos) {
		env_array_overwrite_het_fmt(dest, "SLURM_JOB_QOS",
					    het_job_offset,
					    "%s", alloc->qos);
	}
	if (alloc->resv_name) {
		env_array_overwrite_het_fmt(dest, "SLURM_JOB_RESERVATION",
					    het_job_offset, "%s",
					     alloc->resv_name);
	}

	if (alloc->env_size) {	/* Used to set Burst Buffer environment */
		for (i = 0; i < alloc->env_size; i++) {
			tmp = xstrdup(alloc->environment[i]);
			key = tmp;
			value = strchr(tmp, '=');
			if (value) {
				value[0] = '\0';
				value++;
				env_array_overwrite_het_fmt(dest, key,
							    het_job_offset,
							    "%s",
							    value);
			}
			xfree(tmp);
		}
	}

	if (desc->acctg_freq) {
		env_array_overwrite_het_fmt(dest, "SLURM_ACCTG_FREQ",
					    het_job_offset, "%s",
					     desc->acctg_freq);
	};

	if (desc->network) {
		env_array_overwrite_het_fmt(dest, "SLURM_NETWORK",
					    het_job_offset, "%s",
					    desc->network);
	}

	if (desc->overcommit != NO_VAL8) {
		env_array_overwrite_het_fmt(dest, "SLURM_OVERCOMMIT",
					    het_job_offset, "%u",
					     desc->overcommit);
	}

	/* Add default task counts for srun, if not already set */
	if (desc->bitflags & JOB_NTASKS_SET) {
		env_array_overwrite_het_fmt(dest, "SLURM_NTASKS",
					    het_job_offset,
					    "%d", desc->num_tasks);
		/* maintain for old scripts */
		env_array_overwrite_het_fmt(dest, "SLURM_NPROCS",
					    het_job_offset,
					    "%d", desc->num_tasks);
	}
	if (desc->bitflags & JOB_CPUS_SET) {
		env_array_overwrite_het_fmt(dest, "SLURM_CPUS_PER_TASK",
					    het_job_offset, "%d",
					     desc->cpus_per_task);
	}
	if (desc->ntasks_per_node && (desc->ntasks_per_node != NO_VAL16)) {
		env_array_overwrite_het_fmt(dest, "SLURM_NTASKS_PER_NODE",
					    het_job_offset, "%d",
					     desc->ntasks_per_node);
	}

	return rc;
}

/*
 * Set in "dest" the environment variables strings relevant to a Slurm batch
 * job allocation, overwriting any environment variables of the same name.
 * If the address pointed to by "dest" is NULL, memory will automatically be
 * xmalloc'ed.  The array is terminated by a NULL pointer, and thus is
 * suitable for use by execle() and other env_array_* functions.
 *
 * Sets the variables:
 *	SLURM_CLUSTER_NAME
 *	SLURM_JOB_ID
 *	SLURM_JOB_NUM_NODES
 *	SLURM_JOB_NODELIST
 *	SLURM_JOB_CPUS_PER_NODE
 *	ENVIRONMENT=BATCH
 *	HOSTNAME
 *
 * Sets OBSOLETE variables (needed for MPI, do not remove):
 *	SLURM_JOBID
 *	SLURM_NNODES
 *	SLURM_NODELIST
 *	SLURM_NTASKS
 *	SLURM_TASKS_PER_NODE
 */
extern int
env_array_for_batch_job(char ***dest, const batch_job_launch_msg_t *batch,
			const char *node_name)
{
	char *tmp = NULL;
	int i;
	slurm_step_layout_t *step_layout = NULL;
	uint16_t cpus_per_task;
	uint32_t task_dist;
	slurm_step_layout_req_t step_layout_req;
	uint16_t cpus_per_task_array[1];
	uint32_t cpus_task_reps[1];
	char *tres_per_task = NULL;

	if (!batch)
		return SLURM_ERROR;

	memset(&step_layout_req, 0, sizeof(slurm_step_layout_req_t));
	step_layout_req.num_tasks = batch->ntasks;

	/*
	 * There is no explicit node count in the batch structure,
	 * so we need to calculate the node count.
	 */
	for (i = 0; i < batch->num_cpu_groups; i++) {
		step_layout_req.num_hosts += batch->cpu_count_reps[i];
	}

	/*
	 * --ntasks-per-node no-longer sets num_tasks implicitly, so we need
	 * need to calculate num_tasks here to make sure the
	 * SLURM_TASKS_PER_NODE environment variable is correct. Also make sure
	 * that the SLURM_NTASKS environment variable is set.
	 *
	 * --ntasks-per-tres still implicitly sets ntasks.
	 * --ntasks-per-socket requires --ntasks in order to work.
	 * So neither need to be accounted for here.
	 *
	 * SLURM_TASKS_PER_NODE is used by mpirun so it must be set correctly.
	 */
	if (!step_layout_req.num_tasks) {
		char *tmp_env_ntasks_per_node =
			getenvp(batch->environment, "SLURM_NTASKS_PER_NODE");
		if (tmp_env_ntasks_per_node) {
			step_layout_req.num_tasks =
				atoi(tmp_env_ntasks_per_node) *
				step_layout_req.num_hosts;
		}
	}

	env_array_overwrite_fmt(dest, "SLURM_CLUSTER_NAME", "%s",
	                        slurm_conf.cluster_name);

	env_array_overwrite_fmt(dest, "SLURM_JOB_ID", "%u", batch->job_id);
	env_array_overwrite_fmt(dest, "SLURM_JOB_NUM_NODES", "%u",
				step_layout_req.num_hosts);
	if (batch->array_task_id != NO_VAL) {
		env_array_overwrite_fmt(dest, "SLURM_ARRAY_JOB_ID", "%u",
					batch->array_job_id);
		env_array_overwrite_fmt(dest, "SLURM_ARRAY_TASK_ID", "%u",
					batch->array_task_id);
	}
	env_array_overwrite_fmt(dest, "SLURM_JOB_NODELIST", "%s", batch->nodes);
	env_array_overwrite_fmt(dest, "SLURM_JOB_PARTITION", "%s",
				batch->partition);

	tmp = uint32_compressed_to_str(batch->num_cpu_groups,
				       batch->cpus_per_node,
				       batch->cpu_count_reps);
	env_array_overwrite_fmt(dest, "SLURM_JOB_CPUS_PER_NODE", "%s", tmp);
	xfree(tmp);

	env_array_overwrite_fmt(dest, "ENVIRONMENT", "BATCH");
	if (node_name)
		env_array_overwrite_fmt(dest, "HOSTNAME", "%s", node_name);

	/* OBSOLETE, but needed by MPI, do not remove */
	env_array_overwrite_fmt(dest, "SLURM_JOBID", "%u", batch->job_id);
	env_array_overwrite_fmt(dest, "SLURM_NNODES", "%u",
				step_layout_req.num_hosts);
	env_array_overwrite_fmt(dest, "SLURM_NODELIST", "%s", batch->nodes);

	if ((batch->cpus_per_task != 0) &&
	    (batch->cpus_per_task != NO_VAL16))
		cpus_per_task = batch->cpus_per_task;
	else
		cpus_per_task = 1;	/* default value */
	cpus_per_task_array[0] = cpus_per_task;
	cpus_task_reps[0] = step_layout_req.num_hosts;

	/* Only overwrite this if it is set.  They are set in
	 * sbatch directly and could have changed. */
	if (getenvp(*dest, "SLURM_CPUS_PER_TASK"))
		env_array_overwrite_fmt(dest, "SLURM_CPUS_PER_TASK", "%u",
					cpus_per_task);
	if ((tres_per_task = getenvp(*dest, "SLURM_TRES_PER_TASK")) &&
	    xstrstr(tres_per_task, "cpu=")) {
		char *new_tres_per_task = xstrdup(tres_per_task);
		slurm_option_update_tres_per_task(cpus_per_task, "cpu",
						  &new_tres_per_task);
		env_array_overwrite_fmt(dest, "SLURM_TRES_PER_TASK", "%s",
					new_tres_per_task);
		xfree(new_tres_per_task);
	}

	if (step_layout_req.num_tasks) {
		env_array_overwrite_fmt(dest, "SLURM_NTASKS", "%u",
					step_layout_req.num_tasks);
		/* keep around for old scripts */
		env_array_overwrite_fmt(dest, "SLURM_NPROCS", "%u",
					step_layout_req.num_tasks);
	} else if (!step_layout_req.num_tasks) {
		/*
		 * Figure out num_tasks if it was not set by either
		 * batch->ntasks or SLURM_NTASKS_PER_NODE above
		 * Iterate over all kind of cluster nodes, and accum. the number
		 * of tasks all the group can hold.
		 */
		for (int i = 0; i < batch->num_cpu_groups; i++)
			step_layout_req.num_tasks += (batch->cpus_per_node[i] /
						      cpus_per_task) *
						     batch->cpu_count_reps[i];
	}

	if ((step_layout_req.node_list =
	     getenvp(*dest, "SLURM_ARBITRARY_NODELIST"))) {
		task_dist = SLURM_DIST_ARBITRARY;
	} else {
		step_layout_req.node_list = batch->nodes;
		task_dist = SLURM_DIST_BLOCK;
	}

	step_layout_req.cpus_per_node = batch->cpus_per_node;
	step_layout_req.cpu_count_reps = batch->cpu_count_reps;
	step_layout_req.cpus_per_task = cpus_per_task_array;
	step_layout_req.cpus_task_reps = cpus_task_reps;
	step_layout_req.task_dist = task_dist;
	step_layout_req.plane_size = NO_VAL16;

	if (!(step_layout = slurm_step_layout_create(&step_layout_req)))
		return SLURM_ERROR;

	tmp = uint16_array_to_str(step_layout->node_cnt, step_layout->tasks);
	slurm_step_layout_destroy(step_layout);
	env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE", "%s", tmp);
	xfree(tmp);

	if (batch->pn_min_memory & MEM_PER_CPU) {
		uint64_t tmp_mem = batch->pn_min_memory & (~MEM_PER_CPU);
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_CPU", "%"PRIu64"",
					tmp_mem);
	} else if (batch->pn_min_memory) {
		uint64_t tmp_mem = batch->pn_min_memory;
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_NODE", "%"PRIu64"",
					tmp_mem);
	}

	/* Set the SLURM_JOB_ACCOUNT,  SLURM_JOB_QOS
	 * and SLURM_JOB_RESERVATION if set by
	 * the controller.
	 */
	if (batch->account) {
		env_array_overwrite_fmt(dest,
					"SLURM_JOB_ACCOUNT",
					"%s",
					batch->account);
	}

	if (batch->qos) {
		env_array_overwrite_fmt(dest,
					"SLURM_JOB_QOS",
					"%s",
					batch->qos);
	}

	if (batch->resv_name) {
		env_array_overwrite_fmt(dest,
					"SLURM_JOB_RESERVATION",
					"%s",
					batch->resv_name);
	}

	return SLURM_SUCCESS;
}

/*
 * Set in "dest" the environment variables relevant to a Slurm job step,
 * overwriting any environment variables of the same name.  If the address
 * pointed to by "dest" is NULL, memory will automatically be xmalloc'ed.
 * The array is terminated by a NULL pointer, and thus is suitable for
 * use by execle() and other env_array_* functions.  If preserve_env is
 * true, the variables SLURM_NNODES, SLURM_NTASKS and SLURM_TASKS_PER_NODE
 * remain unchanged.
 *
 * Sets variables:
 *	SLURM_STEP_ID
 *	SLURM_STEP_NUM_NODES
 *	SLURM_STEP_NUM_TASKS
 *	SLURM_STEP_TASKS_PER_NODE
 *	SLURM_STEP_LAUNCHER_PORT
 *	SLURM_STEP_LAUNCHER_IPADDR
 *	SLURM_STEP_RESV_PORTS
 *      SLURM_STEP_SUB_MP
 *
 * Sets OBSOLETE variables:
 *	SLURM_STEPID
 *      SLURM_NNODES
 *	SLURM_NTASKS
 *	SLURM_NODELIST
 *	SLURM_TASKS_PER_NODE
 *	SLURM_SRUN_COMM_PORT
 *	SLURM_LAUNCH_NODE_IPADDR
 *
 */
extern void
env_array_for_step(char ***dest,
		   const job_step_create_response_msg_t *step,
		   launch_tasks_request_msg_t *launch,
		   uint16_t launcher_port,
		   bool preserve_env)
{
	char *tmp, *tpn;
	uint32_t node_cnt, task_cnt;

	if (!step || !launch)
		return;

	node_cnt = step->step_layout->node_cnt;
	env_array_overwrite_fmt(dest, "SLURM_STEP_ID", "%u", step->job_step_id);

	if (launch->het_job_node_list) {
		tmp = launch->het_job_node_list;
		env_array_overwrite_fmt(dest, "SLURM_NODELIST", "%s", tmp);
		env_array_overwrite_fmt(dest, "SLURM_JOB_NODELIST", "%s", tmp);
	} else {
		tmp = step->step_layout->node_list;
		env_array_append_fmt(dest, "SLURM_JOB_NODELIST", "%s", tmp);
	}
	env_array_overwrite_fmt(dest, "SLURM_STEP_NODELIST", "%s", tmp);

	if (launch->het_job_nnodes && (launch->het_job_nnodes != NO_VAL))
		node_cnt = launch->het_job_nnodes;
	env_array_overwrite_fmt(dest, "SLURM_STEP_NUM_NODES", "%u", node_cnt);

	if (launch->het_job_ntasks && (launch->het_job_ntasks != NO_VAL))
		task_cnt = launch->het_job_ntasks;
	else
		task_cnt = step->step_layout->task_cnt;
	env_array_overwrite_fmt(dest, "SLURM_STEP_NUM_TASKS", "%u", task_cnt);

	if (launch->het_job_task_cnts) {
		tpn = uint16_array_to_str(launch->het_job_nnodes,
					  launch->het_job_task_cnts);
		env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE", "%s",
					tpn);
		env_array_overwrite_fmt(dest, "SLURM_NNODES", "%u",
					launch->het_job_nnodes);
	} else {
		tpn = uint16_array_to_str(step->step_layout->node_cnt,
					  step->step_layout->tasks);
		if (!preserve_env) {
			env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE",
						"%s", tpn);
		}
	}
	env_array_overwrite_fmt(dest, "SLURM_STEP_TASKS_PER_NODE", "%s", tpn);

	env_array_overwrite_fmt(dest, "SLURM_STEP_LAUNCHER_PORT",
				"%hu", launcher_port);
	if (step->resv_ports) {
		env_array_overwrite_fmt(dest, "SLURM_STEP_RESV_PORTS",
					"%s", step->resv_ports);
	}

	/* OBSOLETE, but needed by some MPI implementations, do not remove */
	env_array_overwrite_fmt(dest, "SLURM_STEPID", "%u", step->job_step_id);
	if (!preserve_env) {
		env_array_overwrite_fmt(dest, "SLURM_NNODES", "%u", node_cnt);
		env_array_overwrite_fmt(dest, "SLURM_NTASKS", "%u", task_cnt);
		/* keep around for old scripts */
		env_array_overwrite_fmt(dest, "SLURM_NPROCS",
					"%u", step->step_layout->task_cnt);
	}
	env_array_overwrite_fmt(dest, "SLURM_SRUN_COMM_PORT",
				"%hu", launcher_port);

	xfree(tpn);
}

/*
 * Enviroment variables set elsewhere
 * ----------------------------------
 *
 * Set by slurmstepd:
 *	SLURM_STEP_NODEID
 *	SLURM_STEP_PROCID
 *	SLURM_STEP_LOCALID
 *
 * OBSOLETE set by slurmstepd:
 *	SLURM_NODEID
 *	SLURM_PROCID
 *	SLURM_LOCALID
 */

/***********************************************************************
 * Environment variable array support functions
 ***********************************************************************/

/*
 * Return an empty environment variable array (contains a single
 * pointer to NULL).
 */
char **env_array_create(void)
{
	char **env_array;

	env_array = xmalloc(sizeof(char *));
	env_array[0] = NULL;

	return env_array;
}

static int _env_array_update(char ***array_ptr, const char *name,
			     const char *value, bool over_write)
{
	char **ep = NULL;
	char *str = NULL;

	if (array_ptr == NULL)
		return 0;

	if (*array_ptr == NULL)
		*array_ptr = env_array_create();

	ep = _find_name_in_env(*array_ptr, name);
	if (*ep != NULL) {
		if (!over_write)
			return 0;
		xfree (*ep);
	} else {
		ep = _extend_env(array_ptr);
	}

	xstrfmtcat(str, "%s=%s", name, value);
	*ep = str;

	return 1;
}

/*
 * Append a single environment variable to an environment variable array,
 * if and only if a variable by that name does not already exist in the
 * array.
 *
 * "value_fmt" supports printf-style formatting.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_append_fmt(char ***array_ptr, const char *name,
			 const char *value_fmt, ...)
{
	int rc;
	char *value;
	va_list ap;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, value_fmt);
	vsnprintf (value, ENV_BUFSIZE, value_fmt, ap);
	va_end(ap);
	rc = env_array_append(array_ptr, name, value);
	xfree(value);

	return rc;
}

/*
 * Append a single environment variable to an environment variable array,
 * if and only if a variable by that name does not already exist in the
 * array.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_append(char ***array_ptr, const char *name,
		     const char *value)
{
	return _env_array_update(array_ptr, name, value, false);
}

/*
 * Append a single environment variable to an environment variable array
 * if a variable by that name does not already exist.  If a variable
 * by the same name is found in the array, it is overwritten with the
 * new value.
 *
 * "value_fmt" supports printf-style formatting.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_overwrite_fmt(char ***array_ptr, const char *name,
			    const char *value_fmt, ...)
{
	int rc;
	char *value;
	va_list ap;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, value_fmt);
	vsnprintf (value, ENV_BUFSIZE, value_fmt, ap);
	va_end(ap);
	rc = env_array_overwrite(array_ptr, name, value);
	xfree(value);

	return rc;
}

/*
 * Append a single environment variable to an environment variable array
 * if a variable by that name does not already exist.  If a variable
 * by the same name is found in the array, it is overwritten with the
 * new value.
 *
 * "value_fmt" supports printf-style formatting.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_overwrite_het_fmt(char ***array_ptr, const char *name,
				    int het_job_offset,
				    const char *value_fmt, ...)
{
	int rc;
	char *value;
	va_list ap;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, value_fmt);
	vsnprintf (value, ENV_BUFSIZE, value_fmt, ap);
	va_end(ap);
	if (het_job_offset != -1) {
		char *het_comp_name = NULL;
		/* Continue support for old hetjob terminology. */
		xstrfmtcat(het_comp_name, "%s_PACK_GROUP_%d", name,
			   het_job_offset);
		rc = env_array_overwrite(array_ptr, het_comp_name, value);
		xfree(het_comp_name);
		xstrfmtcat(het_comp_name, "%s_HET_GROUP_%d", name,
			   het_job_offset);
		rc = env_array_overwrite(array_ptr, het_comp_name, value);
		xfree(het_comp_name);
	} else
		rc = env_array_overwrite(array_ptr, name, value);
	xfree(value);

	return rc;
}

/*
 * Append a single environment variable to an environment variable array
 * if a variable by that name does not already exist.  If a variable
 * by the same name is found in the array, it is overwritten with the
 * new value.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_overwrite(char ***array_ptr, const char *name,
			const char *value)
{
	return _env_array_update(array_ptr, name, value, true);
}

/*
 * Copy env_array must be freed by env_array_free
 */
char **env_array_copy(const char **array)
{
	char **ptr = NULL;

	env_array_merge(&ptr, array);

	return ptr;
}

/*
 * Free the memory used by an environment variable array.
 */
void env_array_free(char **env_array)
{
	char **ptr;

	if (env_array == NULL)
		return;

	for (ptr = env_array; *ptr != NULL; ptr++) {
		xfree(*ptr);
	}
	xfree(env_array);
}

/*
 * Given an environment variable "name=value" string,
 * copy the name portion into the "name" buffer, and the
 * value portion into the "value" buffer.
 *
 * Return 1 on success, 0 on failure.
 */
static int _env_array_entry_splitter(const char *entry,
				     char *name, int name_len,
				     char *value, int value_len)
{
	char *ptr;
	int len;

	ptr = xstrchr(entry, '=');
	if (ptr == NULL)	/* Bad parsing, no '=' found */
		return 0;
	/*
	 * need to consider the  byte pointed by ptr.
	 * example: entry = 0x0 = "a=b"
	 * ptr = 0x1
	 * len = ptr - entry + 1 = 2 because we need
	 * 2 characters to store 'a\0'
	 */
	len = ptr - entry + 1;
	if (len > name_len)
		return 0;
	strlcpy(name, entry, len);

	ptr++;
	/* account for '\0' here */
	len = strlen(ptr) + 1;
	if (len > value_len)
		return 0;
	strlcpy(value, ptr, len);

	return 1;
}

/*
 * Work similarly to putenv() (from C stdlib), but uses setenv()
 * under the covers.  This avoids having pointers from the global
 * array "environ" into "string".
 *
 * Return 1 on success, 0 on failure.
 */
static int _env_array_putenv(const char *string)
{
	int rc = 0;
	char name[256], *value;

	value = xmalloc(ENV_BUFSIZE);
	if ((_env_array_entry_splitter(string, name, sizeof(name),
				       value, ENV_BUFSIZE)) &&
	    (setenv(name, value, 1) != -1))
		rc = 1;

	xfree(value);
	return rc;
}

/*
 * Set all of the environment variables in a supplied environment
 * variable array.
 */
void env_array_set_environment(char **env_array)
{
	char **ptr;

	if (env_array == NULL)
		return;

	for (ptr = env_array; *ptr != NULL; ptr++) {
		_env_array_putenv(*ptr);
	}
}

/*
 * Unset all of the environment variables in a user's current
 * environment.
 *
 * (Note: because the environ array is decrementing with each
 *  unsetenv, only increment the ptr on a failure to unset.)
 */
void env_unset_environment(void)
{
	extern char **environ;
	char **ptr;
	char name[256], *value;

	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)environ; *ptr != NULL; ) {
		if ((_env_array_entry_splitter(*ptr, name, sizeof(name),
					       value, ENV_BUFSIZE)) &&
			(unsetenv(name) != -1))
			;
		else
			ptr++;
	}
	xfree(value);
}

/*
 * Merge all of the environment variables in src_array into the
 * array dest_array.  Any variables already found in dest_array
 * will be overwritten with the value from src_array.
 */
void env_array_merge(char ***dest_array, const char **src_array)
{
	char **ptr;
	char name[256], *value;

	if (src_array == NULL)
		return;

	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)src_array; *ptr != NULL; ptr++) {
		if (_env_array_entry_splitter(*ptr, name, sizeof(name),
					      value, ENV_BUFSIZE))
			env_array_overwrite(dest_array, name, value);
	}
	xfree(value);
}

/*
 * Merge the environment variables in src_array beginning with "SLURM" or
 * SPANK_OPTION_ENV_PREFIX into the array dest_array. Any variables already
 * found in dest_array will be overwritten with the value from src_array.
 */
void env_array_merge_slurm_spank(char ***dest_array, const char **src_array)
{
	char **ptr;
	char name[256], *value;
	int spank_len;

	if (src_array == NULL)
		return;

	spank_len = strlen(SPANK_OPTION_ENV_PREFIX);
	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)src_array; *ptr != NULL; ptr++) {
		if (_env_array_entry_splitter(*ptr, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    ((xstrncmp(name, "SLURM", 5) == 0) ||
		     (xstrncmp(name, SPANK_OPTION_ENV_PREFIX, spank_len) == 0)))
			env_array_overwrite(dest_array, name, value);
	}
	xfree(value);
}

/*
 * Strip out trailing carriage returns and newlines
 */
static void _strip_cr_nl(char *line)
{
	int len = strlen(line);
	char *ptr;

	for (ptr = line+len-1; ptr >= line; ptr--) {
		if (*ptr=='\r' || *ptr=='\n') {
			*ptr = '\0';
		} else {
			return;
		}
	}
}

/* Return the net count of curly brackets in a string
 * '{' adds one and '}' subtracts one (zero means it is balanced).
 * Special case: return -1 if no open brackets are found */
static int _bracket_cnt(char *value)
{
	int count = 0, i;
	for (i=0; value[i]; i++) {
		if (value[i] == '{')
			count++;
		else if (value[i] == '}')
			count--;
	}
	return count;
}

/*
 * Load user environment from a specified file or file descriptor.
 *
 * This will read in a user specified file or fd, that is invoked
 * via the --export-file option in sbatch. The NAME=value entries must
 * be NULL separated to support special characters in the environment
 * definitions.
 *
 * (Note: This is being added to a minor release. For the
 * next major release, it might be a consideration to merge
 * this functionality with that of load_env_cache and update
 * env_cache_builder to use the NULL character.)
 */
char **env_array_from_file(const char *fname)
{
	char *buf = NULL, *ptr = NULL, *eptr = NULL;
	char *value, *p;
	char **env = NULL;
	char name[256];
	int buf_size = BUFSIZ, buf_left;
	int file_size = 0, tmp_size;
	int separator = '\0';
	int fd;

	if (!fname)
		return NULL;

	/*
	 * If file name is a numeric value, then it is assumed to be a
	 * file descriptor.
	 */
	fd = (int)strtol(fname, &p, 10);
	if ((*p != '\0') || (fd < 3) || (fd > sysconf(_SC_OPEN_MAX)) ||
	    (fcntl(fd, F_GETFL) < 0)) {
		fd = open(fname, O_RDONLY);
		if (fd == -1) {
			error("Could not open user environment file %s", fname);
			return NULL;
		}
		verbose("Getting environment variables from %s", fname);
	} else
		verbose("Getting environment variables from fd %d", fd);

	/*
	 * Read in the user's environment data.
	 */
	buf = ptr = xmalloc(buf_size);
	buf_left = buf_size;
	while ((tmp_size = read(fd, ptr, buf_left))) {
		if (tmp_size < 0) {
			if (errno == EINTR)
				continue;
			error("read(environment_file): %m");
			break;
		}
		buf_left  -= tmp_size;
		file_size += tmp_size;
		if (buf_left == 0) {
			buf_size += BUFSIZ;
			xrealloc(buf, buf_size);
		}
		ptr = buf + file_size;
		buf_left = buf_size - file_size;
	}
	close(fd);

	/*
	 * Parse the buffer into individual environment variable names
	 * and build the environment.
	 */
	env   = env_array_create();
	value = xmalloc(ENV_BUFSIZE);
	for (ptr = buf; ; ptr = eptr+1) {
		eptr = strchr(ptr, separator);
		if ((ptr == eptr) || (eptr == NULL))
			break;
		if (_env_array_entry_splitter(ptr, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			/*
			 * Unset the SLURM_SUBMIT_DIR if it is defined so
			 * that this new value does not get overwritten
			 * in the subsequent call to env_array_merge().
			 */
			if (xstrcmp(name, "SLURM_SUBMIT_DIR") == 0)
				unsetenv(name);
			env_array_overwrite(&env, name, value);
		}
	}
	xfree(buf);
	xfree(value);

	return env;
}

int env_array_to_file(const char *filename, const char **env_array,
		      bool newline)
{
	int outfd = -1;
	int rc = SLURM_SUCCESS;
	const char *terminator = newline ? "\n" : "\0";

	outfd = open(filename, (O_WRONLY | O_CREAT | O_EXCL), 0600);
	if (outfd < 0) {
		error("%s: unable to open %s: %m",
		      __func__, filename);
		goto rwfail;
	}

	for (const char **p = env_array; p && *p; p++) {
		/* skip any env variables with a newline in newline mode */
		if (newline && xstrstr(*p, "\n")) {
			log_flag_hex(STEPS, *p, strlen(*p),
				     "%s: skipping environment variable with newline",
				     __func__);
			continue;
		}

		safe_write(outfd, *p, strlen(*p));
		safe_write(outfd, terminator, 1);
	}

	(void) close(outfd);

	return rc;

rwfail:
	rc = errno;

	if (outfd >= 0)
		(void) close(outfd);

	return rc;
}

/*
 * Load user environment from a cache file located in
 * <state_save_location>/env_username
 */
static char **_load_env_cache(const char *username)
{
	char fname[PATH_MAX];
	char *line, name[256], *value;
	char **env = NULL;
	FILE *fp;
	int i;

	i = snprintf(fname, sizeof(fname), "%s/env_cache/%s",
		     slurm_conf.state_save_location, username);
	if (i < 0) {
		error("Environment cache filename overflow");
		return NULL;
	}
	if (!(fp = fopen(fname, "r"))) {
		error("Could not open user environment cache at %s: %m",
			fname);
		return NULL;
	}

	verbose("Getting cached environment variables at %s", fname);
	env = env_array_create();
	line  = xmalloc(ENV_BUFSIZE);
	value = xmalloc(ENV_BUFSIZE);
	while (1) {
		if (!fgets(line, ENV_BUFSIZE, fp))
			break;
		_strip_cr_nl(line);
		if (_env_array_entry_splitter(line, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			if (value[0] == '(') {
				/* This is a bash function.
				 * It may span multiple lines */
				while (_bracket_cnt(value) > 0) {
					if (!fgets(line, ENV_BUFSIZE, fp))
						break;
					_strip_cr_nl(line);
					if ((strlen(value) + strlen(line)) >
					    (ENV_BUFSIZE - 2))
						break;
					strcat(value, "\n");
					strcat(value, line);
				}
			}
			env_array_overwrite(&env, name, value);
		}
	}
	xfree(line);
	xfree(value);

	fclose(fp);
	return env;
}

static int _child_fn(void *arg)
{
	char **tmp_env = NULL;
	int devnull, fd = 3;
	child_args_t *child_args = arg;
	char *cmdstr;
	const char *username;

	username = child_args->username;
	cmdstr = child_args->cmdstr;
	tmp_env = child_args->tmp_env;

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__)
	/*
	 * Setting propagation and mounting our own /proc for this namespace.
	 * This is done to ensure that this cloned process and its children
	 * have coherent /proc contents with their virtual PIDs.
	 * Check _clone_env_child to see namespace flags used in clone.
	 */
	if (child_args->perform_mount) {
		if (mount("none", "/proc", NULL, MS_PRIVATE|MS_REC, NULL))
			_exit(1);
		if (mount("proc", "/proc", "proc",
			  MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL))
			_exit(1);
	}
#endif

	if ((devnull = open("/dev/null", O_RDWR)) != -1) {
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDERR_FILENO);
	}
	dup2(child_args->fildes[1], STDOUT_FILENO);

	/* slow close all fds */
	while (fd < child_args->rlimit)
		close(fd++);

	if (child_args->mode == 1)
		execle(SUCMD, "su", username, "-c", cmdstr, NULL, tmp_env);
	else if (child_args->mode == 2)
		execle(SUCMD, "su", "-", username, "-c", cmdstr, NULL, tmp_env);
	else {	/* Default system configuration */
#ifdef LOAD_ENV_NO_LOGIN
		execle(SUCMD, "su", username, "-c", cmdstr, NULL, tmp_env);
#else
		execle(SUCMD, "su", "-", username, "-c", cmdstr, NULL, tmp_env);
#endif
	}
	if (devnull >= 0)	/* Avoid Coverity resource leak notification */
		(void) close(devnull);

	_exit(1);
}

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__)
static int _clone_env_child(child_args_t *child_args)
{
	char *child_stack;
	int rc = 0;
	child_stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (child_stack == MAP_FAILED) {
		error("Cannot allocate stack for child: %m");
		return -1;
	}
	/*
	 * In Linux (since 2.6.24), use CLONE_NEWPID to clone the child into a
	 * new pid namespace. We are not into a job cgroup so we want to be
	 * able to terminate any possible background process, specially because
	 * we're using sudo here and running some user scripts (e.g. .bashrc).
	 *
	 * Killing the 'child' pid will kill all the namespace, since in the
	 * namespace, this 'child' is pid 1.
	 */
	rc = clone(_child_fn, child_stack + STACK_SIZE,
		   (SIGCHLD|CLONE_NEWPID|CLONE_NEWNS), child_args);
	/* Memory deallocated only in parent address space, child unaffected */
	if (munmap(child_stack, STACK_SIZE))
		error("%s: failed to munmap child stack: %m", __func__);
	return rc;
}

static bool _ns_path_disabled(const char *ns_path)
{
	FILE *fp = NULL;
	size_t line_sz = 0;
	ssize_t nbytes = 0;
	int ns_value;
	char *line = NULL;
	bool ns_disabled = false;

	/* We will assume not having these files as having no limits. */
	fp = fopen(ns_path, "r");
	if (!fp) {
		debug2("%s: could not open %s, assuming no pid namespace limits. Reason: %m",
		       __func__, ns_path);
	} else {
		nbytes = getline(&line, &line_sz, fp);
		if (nbytes < 0) {
			debug2("%s: could not read contents of %s. Assuming no namespace limits. Reason: %m",
			       __func__, ns_path);
		} else if (nbytes == 0) {
			debug2("%s: read 0 bytes from %s. Assuming no namespace limits",
			       __func__, ns_path);
		} else {
			ns_value = xstrntol(line, NULL, nbytes, 10);
			if (ns_value == 0)
				ns_disabled = true;
		}
		fclose(fp);
		free(line);
		line = NULL;
	}

	return ns_disabled;
}

/*
 * Returns a boolean indicating if the required namespaces for the clone
 * calls are disabled. This is performed by checking the contents of
 * "/proc/sys/max_[mnt|pid]_namespaces" and ensuring they are not 0.
 */
static bool _ns_disabled()
{
	static int disabled = -1;
	char *pid_ns_path = "/proc/sys/user/max_pid_namespaces";
	char *mnt_ns_path = "/proc/sys/user/max_mnt_namespaces";

	if (disabled != -1)
		return disabled;

	disabled = false;

	if (_ns_path_disabled(pid_ns_path) ||
	    _ns_path_disabled(mnt_ns_path))
		disabled = true;

	return disabled;
}
#endif

/*
 * Return an array of strings representing the specified user's default
 * environment variables following a two-prongged approach.
 * 1. Execute (more or less): "/bin/su - <username> -c /usr/bin/env"
 *    Depending upon the user's login scripts, this may take a very
 *    long time to complete or possibly never return
 * 2. Load the user environment from a cache file. This is used
 *    in the event that option 1 times out.  This only happens if no_cache isn't
 *    set.  If it is set then NULL will be returned if the normal load fails.
 *
 * timeout value is in seconds or zero for default (2 secs)
 * mode is 1 for short ("su <user>"), 2 for long ("su - <user>")
 * On error, returns NULL.
 *
 * NOTE: The calling process must have an effective uid of root for
 * this function to succeed.
 */
char **env_array_user_default(const char *username, int timeout, int mode,
			      bool no_cache)
{
	char *line = NULL, *last = NULL, name[PATH_MAX], *value, *buffer;
	char **env = NULL;
	char *starttoken = "XXXXSLURMSTARTPARSINGHEREXXXX";
	char *stoptoken  = "XXXXSLURMSTOPPARSINGHEREXXXXX";
	char cmdstr[256], *env_loc = NULL;
	char *stepd_path = NULL;
	int fildes[2], found, fval, len, rc, timeleft;
	int buf_read, buf_rem, config_timeout;
	pid_t child;
	child_args_t child_args = {0};
	struct timeval begin, now;
	struct pollfd ufds;
	struct stat buf;
	struct rlimit rlim;

	if (geteuid() != (uid_t)0) {
		error("SlurmdUser must be root to use --get-user-env");
		return NULL;
	}

	if (!slurm_conf.get_env_timeout)	/* just read directly from cache */
		return _load_env_cache(username);

	if (stat(SUCMD, &buf))
		fatal("Could not locate command: "SUCMD);
	if (stat("/bin/echo", &buf))
		fatal("Could not locate command: /bin/echo");
	stepd_path = slurm_get_stepd_loc();
	if (stat(stepd_path, &buf) == 0) {
		xstrcat(stepd_path, " getenv");
		env_loc = stepd_path;
	} else if (stat("/bin/env", &buf) == 0)
		env_loc = "/bin/env";
	else if (stat("/usr/bin/env", &buf) == 0)
		env_loc = "/usr/bin/env";
	else
		fatal("Could not locate command: env");
	snprintf(cmdstr, sizeof(cmdstr),
		 "/bin/echo; /bin/echo; /bin/echo; "
		 "/bin/echo %s; %s; /bin/echo %s",
		 starttoken, env_loc, stoptoken);
	xfree(stepd_path);

	if (pipe(fildes) < 0) {
		fatal("pipe: %m");
		return NULL;
	}

	child_args.mode = mode;
	child_args.fildes = fildes;
	child_args.username = username;
	child_args.cmdstr = cmdstr;
	child_args.tmp_env = env_array_create();
	child_args.perform_mount = true;
	env_array_overwrite(&child_args.tmp_env, "ENVIRONMENT", "BATCH");
	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
		error("getrlimit(RLIMIT_NOFILE): %m");
		rlim.rlim_cur = 4096;
	}
	child_args.rlimit = rlim.rlim_cur;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)
	child = fork();
	if (child == -1) {
		fatal("fork: %m");
		return NULL;
	}
	if (child == 0)
		_child_fn(&child_args);
#else
	/*
	 * Since we will be using namespaces in the clone calls (CLONE_NEWPID,
	 * CLONE_NEWNS), we need to know if they are disabled . If they are,
	 * we must fall back to fork and warn the user about the risks.
	 */
	if (_ns_disabled()) {
		warning("%s: pid or mnt namespaces are disabled, avoiding clone and falling back to fork. This can produce orphan/unconstrained processes!",
			__func__);
		child_args.perform_mount = false;
		child = fork();
		if (child == -1) {
			fatal("fork: %m");
			return NULL;
		}
		if (child == 0)
			_child_fn(&child_args);
	} else {
		if ((child = _clone_env_child(&child_args)) == -1) {
			fatal("clone: %m");
			return NULL;
		}
	}
#endif
	close(fildes[1]);
	if ((fval = fcntl(fildes[0], F_GETFL, 0)) < 0)
		error("fcntl(F_GETFL) failed: %m");
	else if (fcntl(fildes[0], F_SETFL, fval | O_NONBLOCK) < 0)
		error("fcntl(F_SETFL) failed: %m");

	gettimeofday(&begin, NULL);
	ufds.fd = fildes[0];
	ufds.events = POLLIN;

	/* Read all of the output from /bin/su into buffer */
	if (timeout == 0)
		timeout = slurm_conf.get_env_timeout;	/* != 0 test above */
	found = 0;
	buf_read = 0;
	buffer = xmalloc(ENV_BUFSIZE);
	while (1) {
		gettimeofday(&now, NULL);
		timeleft = timeout * 1000;
		timeleft -= (now.tv_sec -  begin.tv_sec)  * 1000;
		timeleft -= (now.tv_usec - begin.tv_usec) / 1000;
		if (timeleft <= 0) {
			verbose("timeout waiting for "SUCMD" to complete");
			kill(-child, 9);
			break;
		}
		if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
			if (rc == 0) {
				verbose("timeout waiting for "SUCMD" to complete");
				break;
			}
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			error("poll(): %m");
			break;
		}
		if (!(ufds.revents & POLLIN)) {
			if (ufds.revents & POLLHUP) {	/* EOF */
				found = 1;		/* success */
			} else if (ufds.revents & POLLERR) {
				error("POLLERR");
			} else {
				error("poll() revents=%d", ufds.revents);
			}
			break;
		}
		buf_rem = ENV_BUFSIZE - buf_read;
		if (buf_rem == 0) {
			error("buffer overflow loading env vars");
			break;
		}
		rc = read(fildes[0], &buffer[buf_read], buf_rem);
		if (rc > 0)
			buf_read += rc;
		else if (rc == 0) {	/* EOF */
			found = 1;	/* success */
			break;
		} else {		/* error */
			error("read(env pipe): %m");
			break;
		}
	}
	close(fildes[0]);
	env_array_free(child_args.tmp_env);

	for (config_timeout=0; ; config_timeout++) {
		kill(-child, SIGKILL);	/* Typically a no-op */
		if (config_timeout)
			sleep(1);
		if (waitpid(child, &rc, WNOHANG) > 0)
			break;
		if (config_timeout >= 2) {
			/*
			 * Non-killable processes are indicative of file system
			 * problems. The process will remain as a zombie, but
			 * slurmd/salloc will not otherwise be effected.
			 */
			error("Failed to kill program loading user environment");
			break;
		}
	}

	if (!found) {
		error("Failed to load current user environment variables");
		xfree(buffer);
		return no_cache ? _load_env_cache(username) : NULL;
	}

	/* First look for the start token in the output */
	len = strlen(starttoken);
	found = 0;
	line = strtok_r(buffer, "\n", &last);
	while (!found && line) {
		if (!xstrncmp(line, starttoken, len)) {
			found = 1;
			break;
		}
		line = strtok_r(NULL, "\n", &last);
	}
	if (!found) {
		error("Failed to get current user environment variables");
		xfree(buffer);
		return no_cache ? _load_env_cache(username) : NULL;
	}

	/* Process environment variables until we find the stop token */
	len = strlen(stoptoken);
	found = 0;
	env = env_array_create();
	line = strtok_r(NULL, "\n", &last);
	value = xmalloc(ENV_BUFSIZE);
	while (!found && line) {
		if (!xstrncmp(line, stoptoken, len)) {
			found = 1;
			break;
		}
		if (_env_array_entry_splitter(line, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			if (value[0] == '(') {
				/* This is a bash function.
				 * It may span multiple lines */
				while (_bracket_cnt(value) > 0) {
					line = strtok_r(NULL, "\n", &last);
					if (!line)
						break;
					if ((strlen(value) + strlen(line)) >
					    (ENV_BUFSIZE - 2))
						break;
					strcat(value, "\n");
					strcat(value, line);
				}
			}
			env_array_overwrite(&env, name, value);
		}
		line = strtok_r(NULL, "\n", &last);
	}
	xfree(value);
	xfree(buffer);
	if (!found) {
		error("Failed to get all user environment variables");
		env_array_free(env);
		return no_cache ? _load_env_cache(username) : NULL;
	}

	return env;
}

static void _set_ext_launcher_hydra(char ***dest, char *b_env, char *extra)
{
	char *bootstrap = getenv(b_env);
	bool disabled_slurm_hydra_bootstrap = false;

	if (slurm_conf.mpi_params &&
	    xstrstr(slurm_conf.mpi_params,"disable_slurm_hydra_bootstrap"))
		disabled_slurm_hydra_bootstrap = true;

	if ((!bootstrap && !disabled_slurm_hydra_bootstrap) ||
	    !xstrcmp(bootstrap, "slurm")) {
		env_array_append(dest, b_env, "slurm");
		env_array_append(dest, extra, "--external-launcher");
	}
}

/*
 * Set TRES related env vars. Set here rather than env_array_for_job() since
 * we don't have array of opt values and the raw values are not stored in the
 * job_desc_msg_t structure (only the strings with possibly combined TRES)
 *
 * opt IN - options set by command parsing
 * dest IN/OUT - location to write environment variables
 * het_job_offset IN - component offset into hetjob, -1 if not hetjob
 */
extern void set_env_from_opts(slurm_opt_t *opt, char ***dest,
			      int het_job_offset)
{
	if (opt->cpus_per_gpu) {
		env_array_overwrite_het_fmt(dest, "SLURM_CPUS_PER_GPU",
					    het_job_offset, "%d",
					    opt->cpus_per_gpu);
	}
	if (opt->gpus) {
		env_array_overwrite_het_fmt(dest, "SLURM_GPUS",
					    het_job_offset, "%s",
					    opt->gpus);
	}
	if (opt->gpu_freq) {
		env_array_overwrite_het_fmt(dest, "SLURM_GPU_FREQ",
					    het_job_offset, "%s",
					    opt->gpu_freq);
	}
	if (opt->gpus_per_node) {
		env_array_overwrite_het_fmt(dest, "SLURM_GPUS_PER_NODE",
					    het_job_offset, "%s",
					    opt->gpus_per_node);
	}
	if (opt->gpus_per_socket) {
		env_array_overwrite_het_fmt(dest, "SLURM_GPUS_PER_SOCKET",
					    het_job_offset, "%s",
					    opt->gpus_per_socket);
	}
	if (opt->mem_per_gpu != NO_VAL64) {
		env_array_overwrite_het_fmt(dest, "SLURM_MEM_PER_GPU",
					    het_job_offset, "%"PRIu64,
					    opt->mem_per_gpu);
	}
	if (opt->tres_per_task) {
		env_array_overwrite_het_fmt(dest, "SLURM_TRES_PER_TASK",
					    het_job_offset, "%s",
					    opt->tres_per_task);
	}
	if (opt->tres_bind) {
		env_array_overwrite_het_fmt(dest, "SLURM_TRES_BIND",
					    het_job_offset, "%s",
					    opt->tres_bind);
	}

	/*
	 * In the case that an external launcher (mpirun) is launching instead
	 * of srun let the srun it launches to treat the request differently.
	 */
	env_array_append(dest, "OMPI_MCA_plm_slurm_args",
			 "--external-launcher");
	env_array_append(dest, "PRTE_MCA_plm_slurm_args",
			 "--external-launcher");

	/*
	 * Some mpirun implementations like intel will pass the
	 * bootstrap exec extra args to any bootstrap method (e.g. ssh,
	 * rsh), so force 'slurm' bootstrap if no other one is set.
	 */
	_set_ext_launcher_hydra(dest, "HYDRA_BOOTSTRAP",
				"HYDRA_LAUNCHER_EXTRA_ARGS");
	_set_ext_launcher_hydra(dest, "I_MPI_HYDRA_BOOTSTRAP",
				"I_MPI_HYDRA_BOOTSTRAP_EXEC_EXTRA_ARGS");
}

extern char *find_quote_token(char *tmp, char *sep, char **last)
{
	char *start;
	int i, quote_single = 0, quote_double = 0;

	xassert(last);
	if (*last)
		start = *last;
	else
		start = tmp;
	if (start[0] == '\0')
		return NULL;
	for (i = 0; ; i++) {
		if (start[i] == '\'') {
			if (quote_single)
				quote_single--;
			else
				quote_single++;
		} else if (start[i] == '\"') {
			if (quote_double)
				quote_double--;
			else
				quote_double++;
		} else if (((start[i] == sep[0]) || (start[i] == '\0')) &&
			   (quote_single == 0) && (quote_double == 0)) {
			if (((start[0] == '\'') && (start[i-1] == '\'')) ||
			    ((start[0] == '\"') && (start[i-1] == '\"'))) {
				start++;
				i -= 2;
			}
			if (start[i] == '\0')
				*last = &start[i];
			else
				*last = &start[i] + 1;
			start[i] = '\0';
			return start;
		} else if (start[i] == '\0') {
			error("Improperly formed environment variable (%s)",
			      start);
			*last = &start[i];
			return start;
		}

	}
}

extern void env_merge_filter(slurm_opt_t *opt, job_desc_msg_t *desc)
{
	extern char **environ;
	int i, len;
	char *save_env[2] = { NULL, NULL }, *tmp, *tok, *last = NULL;

	tmp = xstrdup(opt->export_env);
	tok = find_quote_token(tmp, ",", &last);
	while (tok) {

		if (xstrcasecmp(tok, "ALL") == 0) {
			env_array_merge(&desc->environment,
					(const char **)environ);
			tok = find_quote_token(NULL, ",", &last);
			continue;
		}

		if (strchr(tok, '=')) {
			save_env[0] = tok;
			env_array_merge(&desc->environment,
					(const char **)save_env);
		} else {
			len = strlen(tok);
			for (i = 0; environ[i]; i++) {
				if (xstrncmp(tok, environ[i], len) ||
				    (environ[i][len] != '='))
					continue;
				save_env[0] = environ[i];
				env_array_merge(&desc->environment,
						(const char **)save_env);
				break;
			}
		}
		tok = find_quote_token(NULL, ",", &last);
	}
	xfree(tmp);

	env_array_merge_slurm_spank(&desc->environment, (const char **)environ);
}

extern char **env_array_exclude(const char **env, const regex_t *regex)
{
	/* alloc with NULL termination */
	char **purged = xcalloc(1, sizeof(char *));

	/* use regex to skip every matching variable */
	for (; *env; env++) {
		if (!regex_quick_match(*env, regex)) {
			char **e = _extend_env(&purged);
			*e = xstrdup(*env);
		}
	}

	return purged;
}

extern void set_prio_process_env(void)
{
        int retval;

        errno = 0; /* needed to detect a real failure since prio can be -1 */

        if ((retval = getpriority(PRIO_PROCESS, 0)) == -1)  {
                if (errno) {
                        error("getpriority(PRIO_PROCESS): %m");
                        return;
                }
        }

        if (setenvf(NULL, "SLURM_PRIO_PROCESS", "%d", retval) < 0) {
                error("unable to set SLURM_PRIO_PROCESS in environment");
                return;
        }

        debug("propagating SLURM_PRIO_PROCESS=%d", retval);
}
