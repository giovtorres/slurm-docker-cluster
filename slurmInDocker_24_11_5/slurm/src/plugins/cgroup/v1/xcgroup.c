/*****************************************************************************\
 *  xcgroup.c - cgroup related primitives
 *****************************************************************************
 *  Copyright (C) 2009 CEA/DAM/DIF
 *  Written by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *  Modified by Felip Moll <felip.moll@schedmd.com> 2021 SchedMD
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

#include "cgroup_v1.h"

extern int xcgroup_ns_create(xcgroup_ns_t *cgns, char *mnt_args,
			     const char *subsys)
{
	cgns->mnt_point = xstrdup_printf("%s/%s",
					 slurm_cgroup_conf.cgroup_mountpoint,
					 subsys);
	cgns->mnt_args = xstrdup(mnt_args);
	cgns->subsystems = xstrdup(subsys);

	if (!xcgroup_ns_is_available(cgns)) {
		error("cgroup namespace '%s' not mounted. aborting", subsys);
		common_cgroup_ns_destroy(cgns);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

extern int xcgroup_ns_mount(xcgroup_ns_t *cgns)
{
	int fstatus;
	char *options;
	char opt_combined[1024];

	char *mnt_point;
	char *p;

	mode_t cmask;
	mode_t omask;

	cmask = S_IWGRP | S_IWOTH;
	omask = umask(cmask);

	fstatus = mkdir(cgns->mnt_point, 0755);
	if (fstatus && errno != EEXIST) {
		if (cgns->mnt_point[0] != '/') {
			error("unable to create cgroup ns directory '%s' : do not start with '/'",
			      cgns->mnt_point);
			umask(omask);
			return SLURM_ERROR;
		}
		mnt_point = xstrdup(cgns->mnt_point);
		p = mnt_point;
		while ((p = xstrchr(p+1, '/')) != NULL) {
			*p = '\0';
			fstatus = mkdir(mnt_point, 0755);
			if (fstatus && errno != EEXIST) {
				error("unable to create cgroup ns required directory '%s'",
				      mnt_point);
				xfree(mnt_point);
				umask(omask);
				return SLURM_ERROR;
			}
			*p='/';
		}
		xfree(mnt_point);
		fstatus = mkdir(cgns->mnt_point, 0755);
	}

	if (fstatus && errno != EEXIST) {
		log_flag(CGROUP, "unable to create cgroup ns directory '%s' : %m",
			 cgns->mnt_point);
		umask(omask);
		return SLURM_ERROR;
	}
	umask(omask);

	if (cgns->mnt_args == NULL ||
	    strlen(cgns->mnt_args) == 0)
		options = cgns->subsystems;
	else {
		if (snprintf(opt_combined, sizeof(opt_combined), "%s,%s",
			     cgns->subsystems, cgns->mnt_args)
		    >= sizeof(opt_combined)) {
			error("unable to build cgroup options string");
			return SLURM_ERROR;
		}
		options = opt_combined;
	}

	if (mount("cgroup", cgns->mnt_point, "cgroup",
		  MS_NOSUID|MS_NOEXEC|MS_NODEV, options))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

extern int xcgroup_ns_umount(xcgroup_ns_t *cgns)
{
	if (umount(cgns->mnt_point))
		return SLURM_ERROR;
	return SLURM_SUCCESS;
}

extern int xcgroup_ns_is_available(xcgroup_ns_t *cgns)
{
	int fstatus = 0;
	char *value;
	size_t s;
	xcgroup_t cg;

	if (common_cgroup_create(cgns, &cg, "/", 0, 0) == SLURM_ERROR)
		return 0;

	if (common_cgroup_get_param(&cg, "tasks", &value, &s) != SLURM_SUCCESS)
		fstatus = 0;
	else {
		xfree(value);
		fstatus = 1;
	}

	common_cgroup_destroy(&cg);

	return fstatus;
}

extern int xcgroup_ns_find_by_pid(xcgroup_ns_t *cgns, xcgroup_t *cg, pid_t pid)
{
	int fstatus = SLURM_ERROR;
	char file_path[PATH_MAX];
	char *buf;
	size_t fsize;
	char *p;
	char *e;
	char *entry;
	char *subsys;

	/* build pid cgroup meta filepath */
	if (snprintf(file_path, PATH_MAX, "/proc/%u/cgroup",
		      pid) >= PATH_MAX) {
		log_flag(CGROUP, "unable to build cgroup meta filepath for pid=%u : %m",
			 pid);
		return SLURM_ERROR;
	}

	/*
	 * read file content multiple lines of the form:
	 * num_mask:subsystems:relative_path
	 */
	fstatus = common_file_read_content(file_path, &buf, &fsize);
	if (fstatus == SLURM_SUCCESS) {
		fstatus = SLURM_ERROR;
		p = buf;
		while ((e = xstrchr(p, '\n')) != NULL) {
			*e='\0';
			/* get subsystems entry */
			subsys = xstrchr(p, ':');
			p = e + 1;
			if (subsys == NULL)
				continue;
			subsys++;
			/* get relative path entry */
			entry = xstrchr(subsys, ':');
			if (entry == NULL)
				continue;
			*entry='\0';
			/* check subsystem versus ns one */
			if (xstrcmp(cgns->subsystems, subsys) != 0) {
				log_flag(CGROUP, "skipping cgroup subsys %s(%s)",
					 subsys, cgns->subsystems);
				continue;
			}
			entry++;
			fstatus = xcgroup_load(cgns, cg, entry);
			break;
		}
		xfree(buf);
	}

	return fstatus;
}

extern int xcgroup_load(xcgroup_ns_t *cgns, xcgroup_t *cg, char *uri)
{
	int fstatus = SLURM_ERROR;
	char file_path[PATH_MAX];

	struct stat buf;

	/* build cgroup absolute path*/
	if (snprintf(file_path, PATH_MAX, "%s%s", cgns->mnt_point,
		      uri) >= PATH_MAX) {
		log_flag(CGROUP, "unable to build cgroup '%s' absolute path in ns '%s' : %m",
			 uri, cgns->subsystems);
		return fstatus;
	}

	if (stat((const char*)file_path, &buf)) {
		log_flag(CGROUP, "unable to get cgroup '%s' entry '%s' properties: %m",
			 cgns->mnt_point, file_path);
		return fstatus;
	}

	/* fill xcgroup structure */
	cg->ns = cgns;
	cg->name = xstrdup(uri);
	cg->path = xstrdup(file_path);
	cg->uid = buf.st_uid;
	cg->gid = buf.st_gid;

	return SLURM_SUCCESS;
}

extern int xcgroup_get_uint32_param(xcgroup_t *cg, char *param, uint32_t *value)
{
	int fstatus = SLURM_ERROR;
	char file_path[PATH_MAX];
	char *cpath = cg->path;
	uint32_t *values = NULL;
	int vnb;

	if (snprintf(file_path, PATH_MAX, "%s/%s", cpath, param) >= PATH_MAX) {
		log_flag(CGROUP, "unable to build filepath for '%s' and parameter '%s' : %m",
			 cpath, param);
	} else {
		fstatus = common_file_read_uint32s(file_path, &values, &vnb);
		if (fstatus != SLURM_SUCCESS) {
			log_flag(CGROUP, "unable to get parameter '%s' for '%s'",
				 param, cpath);
		} else if (vnb < 1) {
			log_flag(CGROUP, "empty parameter '%s' for '%s'",
				 param, cpath);
		} else {
			*value = values[0];
			fstatus = SLURM_SUCCESS;
		}
		xfree(values);
	}
	return fstatus;
}

extern int xcgroup_get_uint64_param(xcgroup_t *cg, char *param, uint64_t *value)
{
	int fstatus = SLURM_ERROR;
	char file_path[PATH_MAX];
	char *cpath = cg->path;
	uint64_t *values = NULL;
	int vnb;

	if (snprintf(file_path, PATH_MAX, "%s/%s", cpath, param) >= PATH_MAX) {
		log_flag(CGROUP, "unable to build filepath for '%s' and parameter '%s' : %m",
			 cpath, param);
	} else {
		fstatus = common_file_read_uint64s(file_path, &values, &vnb);
		if (fstatus != SLURM_SUCCESS) {
			log_flag(CGROUP, "unable to get parameter '%s' for '%s'",
				 param, cpath);
		} else if (vnb < 1) {
			log_flag(CGROUP, "empty parameter '%s' for '%s'",
				 param, cpath);
		} else {
			*value = values[0];
			fstatus = SLURM_SUCCESS;
		}
		xfree(values);
	}
	return fstatus;
}

extern int xcgroup_cpuset_init(xcgroup_t *cg)
{
	int fstatus = SLURM_ERROR;
	char *cpuset_metafiles[] = {
		"cpuset.cpus",
		"cpuset.mems",
	};
	char *cpuset_conf;
	size_t csize = 0;
	xcgroup_t acg;
	char *acg_name, *p;

	/* load ancestor cg */
	acg_name = xstrdup(cg->name);
	p = xstrrchr(acg_name, '/');
	if (!p) {
		log_flag(CGROUP, "unable to get ancestor path for cpuset cg '%s' : %m",
			 cg->path);
		xfree(acg_name);
		return fstatus;
	} else
		*p = '\0';

	if (xcgroup_load(cg->ns, &acg, acg_name) != SLURM_SUCCESS) {
		log_flag(CGROUP, "unable to load ancestor for cpuset cg '%s' : %m",
			 cg->path);
		xfree(acg_name);
		return fstatus;
	}
	xfree(acg_name);

	/* inherits ancestor params */
	for (int i = 0; i < 2; i++) {
		if (common_cgroup_get_param(&acg, cpuset_metafiles[i],
					    &cpuset_conf, &csize) !=
		    SLURM_SUCCESS) {
			log_flag(CGROUP, "assuming no cpuset cg support for '%s'",
				 acg.path);
			common_cgroup_destroy(&acg);
			return fstatus;
		}

		if (csize > 0)
			cpuset_conf[csize-1] = '\0';

		if (common_cgroup_set_param(cg, cpuset_metafiles[i],
					    cpuset_conf) != SLURM_SUCCESS) {
			log_flag(CGROUP, "unable to write %s configuration (%s) for cpuset cg '%s'",
				 cpuset_metafiles[i], cpuset_conf, cg->path);
			common_cgroup_destroy(&acg);
			xfree(cpuset_conf);
			return fstatus;
		}
		xfree(cpuset_conf);
	}

	common_cgroup_destroy(&acg);

	return SLURM_SUCCESS;
}

extern int xcgroup_create_slurm_cg(xcgroup_ns_t *ns, xcgroup_t *slurm_cg)
{
	int rc = SLURM_SUCCESS;
	char *pre;

#ifdef MULTIPLE_SLURMD
	if (conf->node_name) {
		pre = slurm_conf_expand_slurmd_path(
			slurm_cgroup_conf.cgroup_prepend,
			conf->node_name,
			conf->hostname);
	} else {
		pre = xstrdup("/slurm");
	}
#else
	pre = xstrdup(slurm_cgroup_conf.cgroup_prepend);
#endif

	/* create slurm cgroup in the ns (it could already exist) */
	if (common_cgroup_create(ns, slurm_cg, pre, getuid(), getgid())
	    != SLURM_SUCCESS) {
		xfree(pre);
		return SLURM_ERROR;
	}

	if (common_cgroup_instantiate(slurm_cg) != SLURM_SUCCESS) {
		error("unable to build slurm cgroup for ns %s: %m",
		      ns->subsystems);
		rc = SLURM_ERROR;
	} else
		debug3("slurm cgroup %s successfully created for ns %s",
		       pre, ns->subsystems);

	xfree(pre);

	return rc;
}

extern int xcgroup_create_hierarchy(const char *calling_func,
				    stepd_step_rec_t *step,
				    xcgroup_ns_t *ns,
				    xcgroup_t int_cg[],
				    char job_cgroup_path[],
				    char step_cgroup_path[],
				    char user_cgroup_path[])
{
	xcgroup_t *job_cg = &int_cg[CG_LEVEL_JOB];
	xcgroup_t *step_cg = &int_cg[CG_LEVEL_STEP];
	xcgroup_t *user_cg = &int_cg[CG_LEVEL_USER];
	xcgroup_t *slurm_cg = &int_cg[CG_LEVEL_SLURM];
	int rc = SLURM_SUCCESS;

	/* build user cgroup relative path if not set (should not be) */
	if (*user_cgroup_path == '\0') {
		if (snprintf(user_cgroup_path, PATH_MAX, "%s/uid_%u",
			     slurm_cg->name, step->uid) >= PATH_MAX) {
			error("%s: unable to build uid %u cgroup relative path : %m",
			      calling_func, step->uid);
			return SLURM_ERROR;
		}
	}

	/* build job cgroup relative path if not set (may not be) */
	if (*job_cgroup_path == '\0') {
		if (snprintf(job_cgroup_path, PATH_MAX, "%s/job_%u",
			     user_cgroup_path, step->step_id.job_id)
		    >= PATH_MAX) {
			error("%s: unable to build job %u cg relative path : %m",
			      calling_func, step->step_id.job_id);
			return SLURM_ERROR;
		}
	}

	/* build job step cgroup relative path if not set (may not be) */
	if (*step_cgroup_path == '\0') {
		int len;
		char tmp_char[64];

		len = snprintf(step_cgroup_path, PATH_MAX,
			       "%s/step_%s", job_cgroup_path,
			       log_build_step_id_str(&step->step_id,
				      tmp_char,
				      sizeof(tmp_char),
				      STEP_ID_FLAG_NO_PREFIX |
				      STEP_ID_FLAG_NO_JOB));

		if (len >= PATH_MAX) {
			error("%s: unable to build %ps cg relative path : %m",
			      calling_func, &step->step_id);
			return SLURM_ERROR;
		}
	}

	/*
	 * Create user cgroup in the memory ns (it could already exist)
	 * Ask for hierarchical memory accounting starting from the user
	 * container in order to track the memory consumption up to the
	 * user.
	 */
	if (common_cgroup_create(ns, user_cg, user_cgroup_path, 0, 0) !=
	    SLURM_SUCCESS) {
		error("%s: unable to create user %u cgroup",
		      calling_func, step->uid);
		rc = SLURM_ERROR;
		goto endit;
	}

	if (common_cgroup_instantiate(user_cg) != SLURM_SUCCESS) {
		common_cgroup_destroy(user_cg);
		error("%s: unable to instantiate user %u cgroup",
		      calling_func, step->uid);
		rc = SLURM_ERROR;
		goto endit;
	}

	/*
	 * Create job cgroup in the memory ns (it could already exist)
	 */
	if (common_cgroup_create(ns, job_cg, job_cgroup_path, 0, 0) !=
	    SLURM_SUCCESS) {
		common_cgroup_destroy(user_cg);
		error("%s: unable to create job %u cgroup",
		      calling_func, step->step_id.job_id);
		rc = SLURM_ERROR;
		goto endit;
	}

	if (common_cgroup_instantiate(job_cg) != SLURM_SUCCESS) {
		common_cgroup_destroy(user_cg);
		common_cgroup_destroy(job_cg);
		error("%s: unable to instantiate job %u cgroup",
		      calling_func, step->step_id.job_id);
		rc = SLURM_ERROR;
		goto endit;
	}

	/*
	 * Create step cgroup in the memory ns (it could already exist)
	 */
	if (common_cgroup_create(ns, step_cg, step_cgroup_path, step->uid,
				 step->gid) != SLURM_SUCCESS) {
		/* do not delete user/job cgroup as they can exist for other
		 * steps, but release cgroup structures */
		common_cgroup_destroy(user_cg);
		common_cgroup_destroy(job_cg);
		error("%s: unable to create %ps cgroup",
		      calling_func, &step->step_id);
		rc = SLURM_ERROR;
		goto endit;
	}

	if (common_cgroup_instantiate(step_cg) != SLURM_SUCCESS) {
		common_cgroup_destroy(user_cg);
		common_cgroup_destroy(job_cg);
		common_cgroup_destroy(step_cg);
		error("%s: unable to instantiate %ps cgroup",
		      calling_func, &step->step_id);
		rc = SLURM_ERROR;
		goto endit;
	}

endit:
	return rc;
}
