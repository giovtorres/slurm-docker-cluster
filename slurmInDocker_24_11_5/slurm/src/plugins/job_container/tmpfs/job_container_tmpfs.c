/*****************************************************************************\
 *  job_container_tmpfs.c - Define job container plugin for creating a
 *			    temporary mount namespace for the job, to provide
 *			    quota based access to node local memory.
 *****************************************************************************
 *  Copyright (C) 2019-2021 Regents of the University of California
 *  Produced at Lawrence Berkeley National Laboratory
 *  Written by Aditi Gaur <agaur@lbl.gov>
 *  All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
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
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _GNU_SOURCE
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <semaphore.h>

#include "src/common/slurm_xlator.h"

#include "src/common/env.h"
#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/run_command.h"
#include "src/common/stepd_api.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/interfaces/switch.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"

#include "read_jcconf.h"

static int _create_ns(uint32_t job_id, stepd_step_rec_t *step);
static int _delete_ns(uint32_t job_id);

#if defined (__APPLE__)
extern slurmd_conf_t *conf __attribute__((weak_import));
#else
slurmd_conf_t *conf = NULL;
#endif

const char plugin_name[]        = "job_container tmpfs plugin";
const char plugin_type[]        = "job_container/tmpfs";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

static slurm_jc_conf_t *jc_conf = NULL;
static int step_ns_fd = -1;
static bool plugin_disabled = false;

static void _create_paths(uint32_t job_id, char **job_mount, char **ns_holder,
			  char **src_bind)
{
	xassert(job_mount);

	xstrfmtcat(*job_mount, "%s/%u", jc_conf->basepath, job_id);

	if (ns_holder)
		xstrfmtcat(*ns_holder, "%s/.ns", *job_mount);

	if (src_bind)
		xstrfmtcat(*src_bind, "%s/.%u", *job_mount, job_id);
}

static int _find_step_in_list(step_loc_t *stepd, uint32_t *job_id)
{
	return (stepd->step_id.job_id == *job_id);
}

static bool _is_plugin_disabled(char *basepath)
{
	return ((!basepath) || (!xstrncasecmp(basepath, "none", 4)));
}

static int _restore_ns(list_t *steps, const char *d_name)
{
	char *endptr;
	int fd;
	unsigned long job_id;
	step_loc_t *stepd;

	errno = 0;
	job_id = strtoul(d_name, &endptr, 10);
	if ((errno != 0) || (job_id >= NO_VAL) || (*endptr != '\0')) {
		debug3("ignoring %s, could not convert to jobid.", d_name);
		return SLURM_SUCCESS;
	}

	/* here we think this is a job container */
	log_flag(JOB_CONT, "determine if job %lu is still running", job_id);
	stepd = list_find_first(steps, (ListFindF)_find_step_in_list, &job_id);
	if (!stepd) {
		debug("%s: Job %lu not found, deleting the namespace",
		      __func__, job_id);
		return _delete_ns(job_id);
	}

	fd = stepd_connect(stepd->directory, stepd->nodename,
			   &stepd->step_id, &stepd->protocol_version);
	if (fd == -1) {
		error("%s: failed to connect to stepd for %lu.",
		      __func__, job_id);
		return _delete_ns(job_id);
	}

	close(fd);

	return SLURM_SUCCESS;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 *	are called.  Put global initialization here.
 */
extern int init(void)
{
	if (running_in_slurmd()) {
		/*
		 * Only init the config here for the slurmd. It will be sent by
		 * the slurmd to the slurmstepd at launch time.
		 */
		if (!(jc_conf = init_slurm_jc_conf())) {
			error("%s: Configuration not read correctly: Does '%s' not exist?",
			      plugin_type, tmpfs_conf_file);
			return SLURM_ERROR;
		}
		plugin_disabled = _is_plugin_disabled(jc_conf->basepath);
		debug("job_container.conf read successfully");
	}

	debug("%s loaded", plugin_name);

	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is removed. Clear any allocated
 *	storage here.
 */
extern int fini(void)
{
	int rc = SLURM_SUCCESS;
	debug("%s unloaded", plugin_name);

	if (step_ns_fd != -1) {
		close(step_ns_fd);
		step_ns_fd = -1;
	}

#ifdef MEMORY_LEAK_DEBUG
	free_jc_conf();
#endif
	return rc;
}

extern int container_p_restore(char *dir_name, bool recover)
{
	DIR *dp;
	struct dirent *ep;
	list_t *steps;
	int rc = SLURM_SUCCESS;

	if (plugin_disabled)
		return SLURM_SUCCESS;

	if (jc_conf->auto_basepath) {
		int fstatus;
		mode_t omask = umask(S_IWGRP | S_IWOTH);

		if (jc_conf->basepath[0] != '/') {
			debug("%s: unable to create ns directory '%s' : does not start with '/'",
			      __func__, jc_conf->basepath);
			umask(omask);
			return SLURM_ERROR;
		}

		if ((fstatus = mkdirpath(jc_conf->basepath, 0755, true))) {
			debug("%s: unable to create ns directory '%s' : %s",
			      __func__, jc_conf->basepath,
			      slurm_strerror(fstatus));
			umask(omask);
			return SLURM_ERROR;
		}

		umask(omask);
	}

	steps = stepd_available(conf->spooldir, conf->node_name);

	/*
	 * Iterate over basepath, restore only the folders that seem bounded to
	 * real jobs (have .ns file). NOTE: Restoring the state could be either
	 * deleting the folder if the job is died and resources are free, or
	 * mount it otherwise.
	 */
	if (!(dp = opendir(jc_conf->basepath))) {
		error("%s: Unable to open %s", __func__, jc_conf->basepath);
		return SLURM_ERROR;
	}

	while ((ep = readdir(dp))) {
		/* If possible, only check directories */
		if ((ep->d_type == DT_DIR) || (ep->d_type == DT_UNKNOWN)) {
			if (_restore_ns(steps, ep->d_name))
				rc = SLURM_ERROR;
		}
	}
	closedir(dp);
	FREE_NULL_LIST(steps);

	if (rc)
		error("Encountered an error while restoring job containers.");

	return rc;
}

static int _mount_private_dirs(char *path, uid_t uid)
{
	char *buffer = NULL, *mount_path = NULL, *save_ptr = NULL, *token;
	int rc = 0;

	if (!path) {
		error("%s: no path to private directories specified.",
		      __func__);
		return -1;
	}
	buffer = xstrdup(jc_conf->dirs);
	token = strtok_r(buffer, ",", &save_ptr);
	while (token) {
		/* skip /dev/shm, this is handled elsewhere */
		if (!xstrcmp(token, "/dev/shm")) {
			token = strtok_r(NULL, ",", &save_ptr);
			continue;
		}
		xstrfmtcat(mount_path, "%s/%s", path, token);
		for (char *t = mount_path + strlen(path) + 1; *t; t++) {
			if (*t == '/')
				*t = '_';
		}
		rc = mkdir(mount_path, 0700);
		if (rc && errno != EEXIST) {
			error("%s: Failed to create %s, %m",
			      __func__, mount_path);
			goto private_mounts_exit;
		}
		rc = lchown(mount_path, uid, -1);
		if (rc) {
			error("%s: lchown failed for %s: %m",
			      __func__, mount_path);
			goto private_mounts_exit;
		}
		if (mount(mount_path, token, NULL, MS_BIND, NULL)) {
			error("%s: %s mount failed, %m", __func__, token);
			rc = -1;
			goto private_mounts_exit;
		}
		token = strtok_r(NULL, ",", &save_ptr);
		xfree(mount_path);
	}

private_mounts_exit:
	xfree(buffer);
	xfree(mount_path);
	return rc;
}

static int _mount_private_shm(void)
{
	char *loc = NULL;
	int rc = 0;

	/* return early if "/dev/shm" is not in the mount list */
	if (!(loc = xstrcasestr(jc_conf->dirs, "/dev/shm")))
		return rc;
	if (!((loc[8] == ',') || (loc[8] == 0)))
		return rc;

	/* handle mounting a new /dev/shm */
	if (!jc_conf->shared) {
		/*
		 * only unmount old /dev/shm if private, otherwise this can
		 * impact the root namespace
		 */
		rc = umount("/dev/shm");
		if (rc && errno != EINVAL) {
			error("%s: umount /dev/shm failed: %m", __func__);
			return rc;
		}
	}
	rc = mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
	if (rc) {
		error("%s: /dev/shm mount failed: %m", __func__);
		return -1;
	}
	return rc;
}

static int _clean_job_basepath(uint32_t job_id)
{
	DIR *dp;
	struct dirent *ep;
	char *path = NULL;

	if (!(dp = opendir(jc_conf->basepath))) {
		error("%s: Unable to open %s", __func__, jc_conf->basepath);
		return SLURM_ERROR;
	}

	while ((ep = readdir(dp))) {
		if (!xstrcmp(ep->d_name, ".") || !xstrcmp(ep->d_name, ".."))
			continue;
		/* If possible, only attempt with directories */
		if ((ep->d_type == DT_DIR) || (ep->d_type == DT_UNKNOWN)) {
			xstrfmtcat(path, "%s/%s",
				   jc_conf->basepath, ep->d_name);
			/* it is not important if this fails */
			if (umount2(path, MNT_DETACH))
				log_flag(JOB_CONT, "failed to unmount %s for job %u",
					 path, job_id);
			xfree(path);
		}
	}
	closedir(dp);

	return SLURM_SUCCESS;
}

static char **_setup_script_env(uint32_t job_id,
				stepd_step_rec_t *step,
				char *src_bind,
				char *ns_holder)
{
	char **env = env_array_create();

	env_array_overwrite_fmt(&env, "SLURM_JOB_ID", "%u", job_id);
	env_array_overwrite_fmt(&env, "SLURM_CONF", "%s", conf->conffile);
	env_array_overwrite_fmt(&env, "SLURMD_NODENAME", "%s", conf->node_name);
	if (src_bind)
		env_array_overwrite_fmt(&env, "SLURM_JOB_MOUNTPOINT_SRC", "%s",
					src_bind);
	if (step) {
		if (step->het_job_id && (step->het_job_id != NO_VAL))
			env_array_overwrite_fmt(&env, "SLURM_HET_JOB_ID", "%u",
						step->het_job_id);
		env_array_overwrite_fmt(&env, "SLURM_JOB_GID", "%u",
					step->gid);
		env_array_overwrite_fmt(&env, "SLURM_JOB_UID", "%u",
					step->uid);
		env_array_overwrite_fmt(&env, "SLURM_JOB_USER", "%s",
					step->user_name);
		if (step->alias_list)
			env_array_overwrite_fmt(&env, "SLURM_NODE_ALIASES",
						"%s", step->alias_list);
		if (step->cwd)
			env_array_overwrite_fmt(&env, "SLURM_JOB_WORK_DIR",
						"%s", step->cwd);
	}

	if (ns_holder)
		env_array_overwrite_fmt(&env, "SLURM_NS", "%s", ns_holder);

	return env;
}

static int _create_ns(uint32_t job_id, stepd_step_rec_t *step)
{
	char *job_mount = NULL, *ns_holder = NULL, *src_bind = NULL;
	char *result = NULL;
	int fd;
	int rc = 0;
	sem_t *sem1 = NULL;
	sem_t *sem2 = NULL;
	pid_t cpid;

	_create_paths(job_id, &job_mount, &ns_holder, &src_bind);

	if (mkdir(job_mount, 0700)) {
		error("%s: mkdir %s failed: %m", __func__, job_mount);
		rc = SLURM_ERROR;
		goto end_it;
	}

	/*
	 * MS_BIND mountflag would make mount() ignore all other mountflags
	 * except MS_REC. We need MS_PRIVATE mountflag as well to make the
	 * mount (as well as all mounts inside it) private, which needs to be
	 * done by calling mount() a second time with MS_PRIVATE and MS_REC
	 * flags.
	 */
	if (mount(job_mount, job_mount, NULL, MS_BIND, NULL)) {
		error("%s: Initial base mount failed: %m", __func__);
		rc = SLURM_ERROR;
		goto end_it;
	}
	if (mount(job_mount, job_mount, NULL, MS_PRIVATE | MS_REC, NULL)) {
		error("%s: Initial base mount failed: %m", __func__);
		rc = SLURM_ERROR;
		goto end_it;
	}

	fd = open(ns_holder, O_CREAT|O_RDWR, S_IRWXU);
	if (fd == -1) {
		error("%s: open failed %s: %m", __func__, ns_holder);
		rc = -1;
		goto exit2;
	}
	close(fd);

	/* run any initialization script- if any*/
	if (jc_conf->initscript) {
		run_command_args_t run_command_args = {
			.max_wait = 10 * MSEC_IN_SEC,
			.script_path = jc_conf->initscript,
			.script_type = "initscript",
			.status = &rc,
		};
		run_command_args.env = _setup_script_env(job_id, step,
							 src_bind, NULL);

		log_flag(JOB_CONT, "Running InitScript");
		result = run_command(&run_command_args);
		log_flag(JOB_CONT, "InitScript rc: %d, stdout: %s", rc, result);
		env_array_free(run_command_args.env);
		xfree(result);

		if (rc) {
			error("%s: InitScript: %s failed with rc: %d",
			      __func__, jc_conf->initscript, rc);
			goto exit2;
		}
	}

	rc = mkdir(src_bind, 0700);
	if (rc && (errno != EEXIST)) {
		error("%s: mkdir failed %s, %m", __func__, src_bind);
		goto exit2;
	}

	sem1 = mmap(NULL, sizeof(*sem1), PROT_READ|PROT_WRITE,
		    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (sem1 == MAP_FAILED) {
		error("%s: mmap failed: %m", __func__);
		rc = -1;
		goto exit2;
	}

	sem2 = mmap(NULL, sizeof(*sem2), PROT_READ|PROT_WRITE,
		    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (sem2 == MAP_FAILED) {
		error("%s: mmap failed: %m", __func__);
		sem_destroy(sem1);
		munmap(sem1, sizeof(*sem1));
		rc = -1;
		goto exit2;
	}

	rc = sem_init(sem1, 1, 0);
	if (rc) {
		error("%s: sem_init: %m", __func__);
		goto exit1;
	}
	rc = sem_init(sem2, 1, 0);
	if (rc) {
		error("%s: sem_init: %m", __func__);
		goto exit1;
	}

	cpid = fork();

	if (cpid == -1) {
		error("%s: fork Failed: %m", __func__);
		rc = -1;
		goto exit1;
	}

	if (cpid == 0) {
		rc = unshare(CLONE_NEWNS);
		if (rc) {
			error("%s: %m", __func__);
			goto child_exit;
		}
		if (sem_post(sem1) < 0) {
			error("%s: sem_post failed: %m", __func__);
			rc = -1;
			goto child_exit;
		}
		if (sem_wait(sem2) < 0) {
			error("%s: sem_wait failed %m", __func__);
			rc = -1;
			goto child_exit;
		}
		if (!jc_conf->shared) {
			/* Set root filesystem to private */
			if (mount(NULL, "/", NULL, MS_PRIVATE|MS_REC, NULL)) {
				error("%s: Failed to make root private: %m",
				      __func__);
				rc = -1;
				goto child_exit;
			}
		} else {
			/* Set root filesystem to shared */
			if (mount(NULL, "/", NULL, MS_SHARED | MS_REC, NULL)) {
				error("%s: Failed to make root shared: %m",
				      __func__);
				rc = -1;
				goto child_exit;
			}
			/* Set root filesystem to slave */
			if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL)) {
				error("%s: Failed to make root slave: %m",
				      __func__);
				rc = -1;
				goto child_exit;
			}
		}

		/*
		 * Now we have a persistent mount namespace.
		 * Mount private directories inside the namespace.
		 */
		if (_mount_private_dirs(src_bind, step->uid) == -1) {
			rc = -1;
			goto child_exit;
		}

		/*
		 * this happens when restarting the slurmd, the ownership should
		 * already be correct here.
		 */
		rc = chown(src_bind, step->uid, -1);
		if (rc) {
			error("%s: chown failed for %s: %m",
			      __func__, src_bind);
			rc = -1;
			goto child_exit;
		}

		/*
		 * switch/nvidia_imex needs to create an ephemeral device
		 * node under /dev in this new namespace.
		 */
		if ((rc = switch_g_fs_init(step))) {
			error("%s: switch_g_fs_init failed", __func__);
			rc = -1;
			goto child_exit;
		}

		/*
		 * This umount is to remove the basepath mount from being
		 * visible inside the namespace. So if a user looks up the
		 * mounts inside the job, they will only see their job mount
		 * but not the basepath mount.
		 */
		if (jc_conf->shared)
			rc = _clean_job_basepath(job_id);
		else
			rc = umount2(job_mount, MNT_DETACH);
		if (rc) {
			error("%s: failed to clean job mount(s): %m", __func__);
			goto child_exit;
		}
	child_exit:
		sem_destroy(sem1);
		munmap(sem1, sizeof(*sem1));
		sem_destroy(sem2);
		munmap(sem2, sizeof(*sem2));

		if (!rc) {
			rc = _mount_private_shm();
			if (rc)
				error("%s: could not mount private shm",
				      __func__);
		}
		exit(rc);
	} else {
		int wstatus;
		char *proc_path = NULL;

		if (sem_wait(sem1) < 0) {
			error("%s: sem_Wait failed: %m", __func__);
			rc = -1;
			goto exit1;
		}

		xstrfmtcat(proc_path, "/proc/%u/ns/mnt", cpid);

		/*
		 * Bind mount /proc/pid/ns/mnt to hold namespace active
		 * without a process attached to it
		 */
		rc = mount(proc_path, ns_holder, NULL, MS_BIND, NULL);
		xfree(proc_path);
		if (rc) {
			error("%s: ns base mount failed: %m", __func__);
			if (sem_post(sem2) < 0)
				error("%s: Could not release semaphore: %m",
				      __func__);
			goto exit1;
		}
		if (sem_post(sem2) < 0) {
			error("%s: sem_post failed: %m", __func__);
			goto exit1;
		}

		if ((waitpid(cpid, &wstatus, 0) != cpid) || WEXITSTATUS(wstatus)) {
			error("%s: waitpid failed", __func__);
			rc = SLURM_ERROR;
			goto exit1;
		}

		rc = 0;
	}

	/* run any post clone initialization script */
	if (jc_conf->clonensscript) {
		run_command_args_t run_command_args = {
			.max_wait = jc_conf->clonensscript_wait * MSEC_IN_SEC,
			.script_path = jc_conf->clonensscript,
			.script_type = "clonensscript",
			.status = &rc,
		};
		run_command_args.env = _setup_script_env(job_id, step,
							 src_bind, ns_holder);

		log_flag(JOB_CONT, "Running CloneNSScript");
		result = run_command(&run_command_args);
		log_flag(JOB_CONT, "CloneNSScript rc: %d, stdout: %s",
			 rc, result);
		xfree(result);
		env_array_free(run_command_args.env);

		if (rc) {
			error("%s: CloneNSScript %s failed with rc=%d",
			      __func__, jc_conf->clonensscript, rc);
			goto exit2;
		}
	}

exit1:
	sem_destroy(sem1);
	munmap(sem1, sizeof(*sem1));
	sem_destroy(sem2);
	munmap(sem2, sizeof(*sem2));

exit2:
	if (rc) {
		int failures;
		/* cleanup the job mount */
		if ((failures = rmdir_recursive(job_mount, false))) {
			error("%s: failed to remove %d files from %s",
			      __func__, failures, job_mount);
			rc = SLURM_ERROR;
			goto end_it;
		}
		if (umount2(job_mount, MNT_DETACH))
			error("%s: umount2 %s failed: %m",
			      __func__, job_mount);
		if (rmdir(job_mount))
			error("rmdir %s failed: %m", job_mount);
	}

end_it:
	xfree(job_mount);
	xfree(src_bind);
	xfree(ns_holder);

	return rc;
}

extern int container_p_join_external(uint32_t job_id)
{
	char *job_mount = NULL, *ns_holder = NULL;

	if (plugin_disabled)
		return SLURM_SUCCESS;

	_create_paths(job_id, &job_mount, &ns_holder, NULL);

	if (step_ns_fd == -1) {
		step_ns_fd = open(ns_holder, O_RDONLY);
		if (step_ns_fd == -1)
			error("%s: %m", __func__);
	}

	xfree(job_mount);
	xfree(ns_holder);

	return step_ns_fd;
}

extern int container_p_join(slurm_step_id_t *step_id, uid_t uid,
			    bool step_create)
{
	char *job_mount = NULL, *ns_holder = NULL;
	int fd;
	int rc = SLURM_SUCCESS;

	if (plugin_disabled)
		return SLURM_SUCCESS;

	/*
	 * Handle EntireStepInNS setting. If set, the join needs to happen
	 * during the fork+exec chain that creates the slurmstepd process, and
	 * all successive calls within slurmstepd need to be skipped. If not
	 * set, do the opposite.
	 */
	if ((!jc_conf->entire_step_in_ns && running_in_slurmd() &&
	     step_create) ||
	    (jc_conf->entire_step_in_ns && running_in_slurmstepd() &&
	     step_id->step_id != SLURM_EXTERN_CONT))
		return SLURM_SUCCESS;

	/*
	 * Jobid 0 means we are not a real job, but a script running instead we
	 * do not need to handle this request.
	 */
	if (step_id->job_id == 0)
		return SLURM_SUCCESS;

	_create_paths(step_id->job_id, &job_mount, &ns_holder, NULL);

	/* This is called on the slurmd so we can't use ns_fd. */
	fd = open(ns_holder, O_RDONLY);
	if (fd == -1) {
		error("%s: open failed for %s: %m", __func__, ns_holder);
		xfree(job_mount);
		xfree(ns_holder);
		return SLURM_ERROR;
	}

	rc = setns(fd, CLONE_NEWNS);
	if (rc) {
		error("%s: setns failed for %s: %m", __func__, ns_holder);
		/* closed after error() */
		close(fd);
		xfree(job_mount);
		xfree(ns_holder);
		return SLURM_ERROR;
	} else {
		log_flag(JOB_CONT, "job %u entered namespace", step_id->job_id);
	}

	close(fd);
	xfree(job_mount);
	xfree(ns_holder);

	return SLURM_SUCCESS;
}

static int _delete_ns(uint32_t job_id)
{
	char *job_mount = NULL, *ns_holder = NULL;
	int rc = 0, failures = 0;
	char *result = NULL;

	_create_paths(job_id, &job_mount, &ns_holder, NULL);

	/* run any post clone epilog script */
	/* initialize environ variable to include jobid and namespace file */
	if (jc_conf->clonensepilog) {
		run_command_args_t run_command_args = {
			.max_wait = jc_conf->clonensepilog_wait * MSEC_IN_SEC,
			.script_path = jc_conf->clonensepilog,
			.script_type = "clonensepilog",
			.status = &rc,
		};
		run_command_args.env = _setup_script_env(job_id, NULL,
							 NULL, ns_holder);
		log_flag(JOB_CONT, "Running CloneNSEpilog");
		result = run_command(&run_command_args);
		env_array_free(run_command_args.env);
		log_flag(JOB_CONT, "CloneNSEpilog rc: %d, stdout: %s",
			 rc, result);
		xfree(result);

		if (rc) {
			error("%s: CloneNSEpilog script %s failed with rc=%d",
			      __func__, jc_conf->clonensepilog, rc);
		}
	}

	errno = 0;

	/*
	 * Close the step_ns_fd if it was opened.  If close fails here, it
	 * should be safe to continue since ns_holder is lazy unmounted later
	 * and will get cleaned up when the slurmstepd process ends.
	 */
	if (step_ns_fd != -1) {
		if (close(step_ns_fd))
			log_flag(JOB_CONT, "job %u close step_ns_fd(%d) failed: %m",
				 job_id, step_ns_fd);

		else
			step_ns_fd = -1;
	}

	/*
	 * umount2() sets errno to EINVAL if the target is not a mount point
	 * but also if called with invalid flags.  Consider this if changing the
	 * flags to umount2().
	 */
	rc = umount2(ns_holder, MNT_DETACH);
	if (rc) {
		if ((errno == EINVAL) || (errno == ENOENT)) {
			log_flag(JOB_CONT, "%s: umount2 %s failed: %m",
				 __func__, ns_holder);
		} else {
			error("%s: umount2 %s failed: %m",
			      __func__, ns_holder);
			xfree(job_mount);
			xfree(ns_holder);
			return SLURM_ERROR;
		}
	}

	if ((failures = rmdir_recursive(job_mount, false)))
		error("%s: failed to remove %d files from %s",
		      __func__, failures, job_mount);
	if (umount2(job_mount, MNT_DETACH))
		log_flag(JOB_CONT, "umount2: %s failed: %m", job_mount);
	if (rmdir(job_mount))
		error("rmdir %s failed: %m", job_mount);

	xfree(job_mount);
	xfree(ns_holder);

	return SLURM_SUCCESS;
}

extern int container_p_stepd_create(uint32_t job_id, stepd_step_rec_t *step)
{
	if (plugin_disabled)
		return SLURM_SUCCESS;

	return _create_ns(job_id, step);
}

extern int container_p_stepd_delete(uint32_t job_id)
{
	if (plugin_disabled)
		return SLURM_SUCCESS;

	return _delete_ns(job_id);
}

extern int container_p_send_stepd(int fd)
{
	int len;
	buf_t *buf;

	buf = get_slurm_jc_conf_buf();

	/* The config should have been inited by now */
	xassert(buf);

	len = get_buf_offset(buf);
	safe_write(fd, &len, sizeof(len));
	safe_write(fd, get_buf_data(buf), len);

	return SLURM_SUCCESS;
rwfail:
	error("%s: failed", __func__);
	return SLURM_ERROR;
}

extern int container_p_recv_stepd(int fd)
{
	int len;
	buf_t *buf;

	safe_read(fd, &len, sizeof(len));

	buf = init_buf(len);
	safe_read(fd, buf->head, len);

	if (!(jc_conf = set_slurm_jc_conf(buf)))
		goto rwfail;

	plugin_disabled = _is_plugin_disabled(jc_conf->basepath);

	return SLURM_SUCCESS;
rwfail:
	error("%s: failed", __func__);
	return SLURM_ERROR;
}
