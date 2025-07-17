/*****************************************************************************\
 *  step_terminate_monitor.c - Run an external program if there are
 *    unkillable processes at step termination.
 *****************************************************************************
 *  Copyright (C) 2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Christopher J. Morrone <morrone2@llnl.gov>
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
#include <signal.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <sys/wait.h>
#include <time.h>

#include "src/common/macros.h"
#include "src/common/parse_time.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/read_config.h"
#include "src/interfaces/job_container.h"
#include "src/slurmd/slurmstepd/step_terminate_monitor.h"
#include "src/slurmd/slurmstepd/slurmstepd.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool running_flag = false;
static pthread_t tid;
static uint16_t timeout;
static char *program_name;
static uint32_t recorded_jobid = NO_VAL;
static uint32_t recorded_stepid = NO_VAL;

static void *_monitor(void *);
static int _call_external_program(stepd_step_rec_t *step);

void step_terminate_monitor_start(stepd_step_rec_t *step)
{
	slurm_conf_t *conf;

	slurm_mutex_lock(&lock);

	if (running_flag) {
		slurm_mutex_unlock(&lock);
		return;
	}

	conf = slurm_conf_lock();
	timeout = conf->unkillable_timeout;
	program_name = xstrdup(conf->unkillable_program);
	slurm_conf_unlock();

	running_flag = true;
	slurm_thread_create(&tid, _monitor, step);

	recorded_jobid = step->step_id.job_id;
	recorded_stepid = step->step_id.step_id;

	slurm_mutex_unlock(&lock);
}

void step_terminate_monitor_stop(void)
{
	slurm_mutex_lock(&lock);

	if (!running_flag) {
		error("%s: already stopped", __func__);
		slurm_mutex_unlock(&lock);
		return;
	}

	running_flag = false;
	debug("signaling condition");
	slurm_cond_signal(&cond);
	slurm_mutex_unlock(&lock);

	slurm_thread_join(tid);

	xfree(program_name);
}


static void *_monitor(void *arg)
{
	stepd_step_rec_t *step = (stepd_step_rec_t *)arg;
	struct timespec ts = {0, 0};
	int rc;

	debug2("step_terminate_monitor will run for %d secs", timeout);

	ts.tv_sec = time(NULL) + 1 + timeout;

	slurm_mutex_lock(&lock);
	if (!running_flag)
		goto done;

	rc = pthread_cond_timedwait(&cond, &lock, &ts);
	if (rc == ETIMEDOUT) {
		char entity[45], time_str[256];
		time_t now = time(NULL);

		_call_external_program(step);

		if (step->step_id.step_id == SLURM_BATCH_SCRIPT) {
			snprintf(entity, sizeof(entity),
				 "JOB %u", step->step_id.job_id);
		} else if (step->step_id.step_id == SLURM_EXTERN_CONT) {
			snprintf(entity, sizeof(entity),
				 "EXTERN STEP FOR %u", step->step_id.job_id);
		} else if (step->step_id.step_id == SLURM_INTERACTIVE_STEP) {
			snprintf(entity, sizeof(entity),
				 "INTERACTIVE STEP FOR %u",
				 step->step_id.job_id);
		} else {
			char tmp_char[33];
			log_build_step_id_str(&step->step_id, tmp_char,
					      sizeof(tmp_char),
					      STEP_ID_FLAG_NO_PREFIX);
			snprintf(entity, sizeof(entity), "STEP %s", tmp_char);
		}
		slurm_make_time_str(&now, time_str, sizeof(time_str));

		if (step->state < SLURMSTEPD_STEP_RUNNING) {
			error("*** %s STEPD TERMINATED ON %s AT %s DUE TO JOB NOT RUNNING ***",
			      entity, step->node_name, time_str);
			rc = ESLURMD_JOB_NOTRUNNING;
		} else {
			error("*** %s STEPD TERMINATED ON %s AT %s DUE TO JOB NOT ENDING WITH SIGNALS ***",
			      entity, step->node_name, time_str);
			rc = ESLURMD_KILL_TASK_FAILED;
		}

		stepd_drain_node(slurm_strerror(rc));

		if (!step->batch) {
			/* Notify waiting sruns */
			if (step->step_id.step_id != SLURM_EXTERN_CONT)
				while (stepd_send_pending_exit_msgs(step)) {;}

			if ((step_complete.rank > -1)) {
				if (step->aborted)
					info("unkillable stepd exiting with aborted job");
				else
					stepd_wait_for_children_slurmstepd(
						step);
			}
			/* Notify parent stepd or ctld directly */
			stepd_send_step_complete_msgs(step);
		}

		/* stepd_cleanup always returns rc as a pass-through */
		(void) stepd_cleanup(NULL, step, NULL, rc, false);
	} else if (rc != 0) {
		error("Error waiting on condition in _monitor: %m");
	}

done:
	debug2("step_terminate_monitor is stopping");
	slurm_mutex_unlock(&lock);
	return NULL;
}


static int _call_external_program(stepd_step_rec_t *step)
{
	int status, rc, opt;
	pid_t cpid;
	int max_wait = 300; /* seconds */
	int time_remaining;

	if (program_name == NULL || program_name[0] == '\0')
		return 0;

	debug("step_terminate_monitor: unkillable after %d sec, calling: %s",
	     timeout, program_name);

	if (access(program_name, R_OK | X_OK) < 0) {
		debug("step_terminate_monitor not running %s: %m",
		      program_name);
		return 0;
	}

	if ((cpid = fork()) < 0) {
		error("step_terminate_monitor executing %s: fork: %m",
		      program_name);
		return -1;
	}
	if (cpid == 0) {
		/* child */
		char *argv[2];
		char **env = NULL;

		/* container_g_join needs to be called in the
		   forked process part of the fork to avoid a race
		   condition where if this process makes a file or
		   detacts itself from a child before we add the pid
		   to the container in the parent of the fork.
		*/
		if (container_g_join(&step->step_id, getuid(), false) !=
		    SLURM_SUCCESS)
			error("container_g_join(%u): %m", recorded_jobid);
		env = env_array_create();
		env_array_append_fmt(&env, "SLURM_JOBID", "%u", recorded_jobid);
		env_array_append_fmt(&env, "SLURM_JOB_ID", "%u", recorded_jobid);
		env_array_append_fmt(&env, "SLURM_STEPID", "%u", recorded_stepid);
		env_array_append_fmt(&env, "SLURM_STEP_ID", "%u", recorded_stepid);

		argv[0] = program_name;
		argv[1] = NULL;

		setpgid(0, 0);
		execve(program_name, argv, env);
		error("step_terminate_monitor execv(): %m");
		_exit(127);
	}

	opt = WNOHANG;
	time_remaining = max_wait;
	while (1) {
		rc = waitpid(cpid, &status, opt);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			/* waitpid may very well fail under normal conditions
			   because the wait3() in mgr.c:_wait_for_any_task()
			   may have reaped the return code. */
			return 0;
		} else if (rc == 0) {
			sleep(1);
			if ((--time_remaining) == 0) {
				error("step_terminate_monitor: %s still running"
				      " after %d seconds.  Killing.",
				      program_name, max_wait);
				killpg(cpid, SIGKILL);
				opt = 0;
			}
		} else  {
			return status;
		}
	}

	/* NOTREACHED */
}
