/*****************************************************************************\
 *  cgroup.c - driver for cgroup plugin
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
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

#include "src/interfaces/cgroup.h"

/* Define slurm-specific aliases for use by plugins, see slurm_xlator.h. */
strong_alias(cgroup_conf_init, slurm_cgroup_conf_init);
strong_alias(cgroup_conf_destroy, slurm_cgroup_conf_destroy);
strong_alias(autodetect_cgroup_version, slurm_autodetect_cgroup_version);

#define DEFAULT_CGROUP_BASEDIR "/sys/fs/cgroup"
#define DEFAULT_CGROUP_PLUGIN "autodetect"

/* Symbols provided by the plugin */
typedef struct {
	int	(*initialize)		(cgroup_ctl_type_t sub);
	int	(*system_create)	(cgroup_ctl_type_t sub);
	int	(*system_addto)		(cgroup_ctl_type_t sub, pid_t *pids,
					 int npids);
	int	(*system_destroy)	(cgroup_ctl_type_t sub);
	int	(*step_create)		(cgroup_ctl_type_t sub,
					 stepd_step_rec_t *step);
	int	(*step_addto)		(cgroup_ctl_type_t sub, pid_t *pids,
					 int npids);
	int	(*step_get_pids)	(pid_t **pids, int *npids);
	int	(*step_suspend)		(void);
	int	(*step_resume)		(void);
	int	(*step_destroy)		(cgroup_ctl_type_t sub);
	bool	(*has_pid)		(pid_t pid);
	cgroup_limits_t *(*constrain_get) (cgroup_ctl_type_t sub,
					   cgroup_level_t level);
	int	(*constrain_set)	(cgroup_ctl_type_t sub,
					 cgroup_level_t level,
					 cgroup_limits_t *limits);
        int	(*constrain_apply)	(cgroup_ctl_type_t sub,
                                         cgroup_level_t level,
                                         uint32_t task_id);
	int	(*step_start_oom_mgr)	(stepd_step_rec_t *step);
	cgroup_oom_t *(*step_stop_oom_mgr) (stepd_step_rec_t *step);
	int	(*task_addto)		(cgroup_ctl_type_t sub,
					 stepd_step_rec_t *step, pid_t pid,
					 uint32_t task_id);
	cgroup_acct_t *(*task_get_acct_data) (uint32_t taskid);
	long int (*get_acct_units)	(void);
	bool (*has_feature) (cgroup_ctl_feature_t f);
	char *(*get_scope_path)(void);
	int (*setup_scope)(char *scope_path);
} slurm_ops_t;

/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_ops_t.
 */
static const char *syms[] = {
	"cgroup_p_initialize",
	"cgroup_p_system_create",
	"cgroup_p_system_addto",
	"cgroup_p_system_destroy",
	"cgroup_p_step_create",
	"cgroup_p_step_addto",
	"cgroup_p_step_get_pids",
	"cgroup_p_step_suspend",
	"cgroup_p_step_resume",
	"cgroup_p_step_destroy",
	"cgroup_p_has_pid",
	"cgroup_p_constrain_get",
	"cgroup_p_constrain_set",
        "cgroup_p_constrain_apply",
	"cgroup_p_step_start_oom_mgr",
	"cgroup_p_step_stop_oom_mgr",
	"cgroup_p_task_addto",
	"cgroup_p_task_get_acct_data",
	"cgroup_p_get_acct_units",
	"cgroup_p_has_feature",
	"cgroup_p_get_scope_path",
	"cgroup_p_setup_scope",
};

/* Local variables */
static slurm_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock =	PTHREAD_MUTEX_INITIALIZER;
static plugin_init_t plugin_inited = PLUGIN_NOT_INITED;

cgroup_conf_t slurm_cgroup_conf;

static pthread_rwlock_t cg_conf_lock = PTHREAD_RWLOCK_INITIALIZER;
static buf_t *cg_conf_buf = NULL;
static bool cg_conf_inited = false;
static bool cg_conf_exist = true;

static char scope_path[PATH_MAX] = "";

/* local functions */
static void _cgroup_conf_fini();
static void _clear_slurm_cgroup_conf();
static void _pack_cgroup_conf(buf_t *buffer);
static int _unpack_cgroup_conf(buf_t *buffer);
static void _read_slurm_cgroup_conf(void);

/* Local functions */
static int _defunct_option(void **dest, slurm_parser_enum_t type,
			  const char *key, const char *value,
			  const char *line, char **leftover)
{
	error_in_daemon("The option \"%s\" is defunct, please remove it from cgroup.conf.",
			key);
	return 0;
}

static void _cgroup_conf_fini()
{
	slurm_rwlock_wrlock(&cg_conf_lock);

	_clear_slurm_cgroup_conf();
	cg_conf_inited = false;
	FREE_NULL_BUFFER(cg_conf_buf);

	slurm_rwlock_unlock(&cg_conf_lock);
}

static void _clear_slurm_cgroup_conf(void)
{
	xfree(slurm_cgroup_conf.cgroup_mountpoint);
	xfree(slurm_cgroup_conf.cgroup_plugin);
	xfree(slurm_cgroup_conf.cgroup_prepend);

	memset(&slurm_cgroup_conf, 0, sizeof(slurm_cgroup_conf));
}

static void _init_slurm_cgroup_conf(void)
{
	_clear_slurm_cgroup_conf();

	slurm_cgroup_conf.allowed_ram_space = 100;
	slurm_cgroup_conf.allowed_swap_space = 0;
	slurm_cgroup_conf.cgroup_mountpoint = xstrdup(DEFAULT_CGROUP_BASEDIR);
	slurm_cgroup_conf.cgroup_plugin = xstrdup(DEFAULT_CGROUP_PLUGIN);
#ifndef MULTIPLE_SLURMD
	slurm_cgroup_conf.cgroup_prepend = xstrdup("/slurm");
#else
	slurm_cgroup_conf.cgroup_prepend = xstrdup("/slurm_%n");
#endif
	slurm_cgroup_conf.constrain_cores = false;
	slurm_cgroup_conf.constrain_devices = false;
	slurm_cgroup_conf.constrain_ram_space = false;
	slurm_cgroup_conf.constrain_swap_space = false;
	slurm_cgroup_conf.enable_controllers = false;
	slurm_cgroup_conf.ignore_systemd = false;
	slurm_cgroup_conf.ignore_systemd_on_failure = false;
	slurm_cgroup_conf.max_ram_percent = 100;
	slurm_cgroup_conf.max_swap_percent = 100;
	slurm_cgroup_conf.memory_swappiness = NO_VAL64;
	slurm_cgroup_conf.min_ram_space = XCGROUP_DEFAULT_MIN_RAM;
	slurm_cgroup_conf.signal_children_processes = false;
	slurm_cgroup_conf.systemd_timeout = 1000;
}

static void _pack_cgroup_conf(buf_t *buffer)
{
	/*
	 * No protocol version needed, at the time of writing we are only
	 * sending at slurmstepd startup.
	 */

	if (!cg_conf_exist) {
		packbool(0, buffer);
		return;
	}
	packbool(1, buffer);
	packstr(slurm_cgroup_conf.cgroup_mountpoint, buffer);

	packstr(slurm_cgroup_conf.cgroup_prepend, buffer);

	packbool(slurm_cgroup_conf.constrain_cores, buffer);

	packbool(slurm_cgroup_conf.constrain_ram_space, buffer);
	packfloat(slurm_cgroup_conf.allowed_ram_space, buffer);
	packfloat(slurm_cgroup_conf.max_ram_percent, buffer);

	pack64(slurm_cgroup_conf.min_ram_space, buffer);

	packbool(slurm_cgroup_conf.constrain_swap_space, buffer);
	packfloat(slurm_cgroup_conf.allowed_swap_space, buffer);
	packfloat(slurm_cgroup_conf.max_swap_percent, buffer);
	pack64(slurm_cgroup_conf.memory_swappiness, buffer);

	packbool(slurm_cgroup_conf.constrain_devices, buffer);
	packstr(slurm_cgroup_conf.cgroup_plugin, buffer);

	packbool(slurm_cgroup_conf.ignore_systemd, buffer);
	packbool(slurm_cgroup_conf.ignore_systemd_on_failure, buffer);

	packbool(slurm_cgroup_conf.enable_controllers, buffer);
	packbool(slurm_cgroup_conf.signal_children_processes, buffer);
	pack64(slurm_cgroup_conf.systemd_timeout, buffer);
}

static int _unpack_cgroup_conf(buf_t *buffer)
{
	bool tmpbool = false;
	/*
	 * No protocol version needed, at the time of writing we are only
	 * reading on slurmstepd startup.
	 */
	safe_unpackbool(&tmpbool, buffer);
	if (!tmpbool) {
		cg_conf_exist = false;
		return SLURM_SUCCESS;
	}

	_clear_slurm_cgroup_conf();

	safe_unpackstr(&slurm_cgroup_conf.cgroup_mountpoint, buffer);

	safe_unpackstr(&slurm_cgroup_conf.cgroup_prepend, buffer);

	safe_unpackbool(&slurm_cgroup_conf.constrain_cores, buffer);

	safe_unpackbool(&slurm_cgroup_conf.constrain_ram_space, buffer);
	safe_unpackfloat(&slurm_cgroup_conf.allowed_ram_space, buffer);
	safe_unpackfloat(&slurm_cgroup_conf.max_ram_percent, buffer);

	safe_unpack64(&slurm_cgroup_conf.min_ram_space, buffer);

	safe_unpackbool(&slurm_cgroup_conf.constrain_swap_space, buffer);
	safe_unpackfloat(&slurm_cgroup_conf.allowed_swap_space, buffer);
	safe_unpackfloat(&slurm_cgroup_conf.max_swap_percent, buffer);
	safe_unpack64(&slurm_cgroup_conf.memory_swappiness, buffer);

	safe_unpackbool(&slurm_cgroup_conf.constrain_devices, buffer);
	safe_unpackstr(&slurm_cgroup_conf.cgroup_plugin, buffer);

	safe_unpackbool(&slurm_cgroup_conf.ignore_systemd, buffer);
	safe_unpackbool(&slurm_cgroup_conf.ignore_systemd_on_failure, buffer);

	safe_unpackbool(&slurm_cgroup_conf.enable_controllers, buffer);
	safe_unpackbool(&slurm_cgroup_conf.signal_children_processes, buffer);
	safe_unpack64(&slurm_cgroup_conf.systemd_timeout, buffer);

	return SLURM_SUCCESS;

unpack_error:
	_clear_slurm_cgroup_conf();

	return SLURM_ERROR;
}

/*
 * read_slurm_cgroup_conf - load the Slurm cgroup configuration from the
 *	cgroup.conf file.
 */
static void _read_slurm_cgroup_conf(void)
{
	s_p_options_t options[] = {
		{"CgroupAutomount", S_P_BOOLEAN, _defunct_option},
		{"CgroupMountpoint", S_P_STRING},
		{"CgroupReleaseAgentDir", S_P_STRING},
		{"ConstrainCores", S_P_BOOLEAN},
		{"ConstrainRAMSpace", S_P_BOOLEAN},
		{"AllowedRAMSpace", S_P_FLOAT},
		{"MaxRAMPercent", S_P_FLOAT},
		{"MinRAMSpace", S_P_UINT64},
		{"ConstrainSwapSpace", S_P_BOOLEAN},
		{"AllowedSwapSpace", S_P_FLOAT},
		{"MaxSwapPercent", S_P_FLOAT},
		{"MemoryLimitEnforcement", S_P_BOOLEAN},
		{"MemoryLimitThreshold", S_P_FLOAT},
		{"ConstrainDevices", S_P_BOOLEAN},
		{"AllowedDevicesFile", S_P_STRING},
		{"MemorySwappiness", S_P_UINT64},
		{"CgroupPlugin", S_P_STRING},
		{"IgnoreSystemd", S_P_BOOLEAN},
		{"IgnoreSystemdOnFailure", S_P_BOOLEAN},
		{"EnableControllers", S_P_BOOLEAN},
		{"SignalChildrenProcesses", S_P_BOOLEAN},
		{"SystemdTimeout", S_P_UINT64},
		{NULL} };
	s_p_hashtbl_t *tbl = NULL;
	char *conf_path = NULL, *tmp_str;
	struct stat buf;
	size_t sz;

	/* Get the cgroup.conf path and validate the file */
	conf_path = get_extra_conf_path("cgroup.conf");
	if ((conf_path == NULL) || (stat(conf_path, &buf) == -1)) {
		info("%s: No cgroup.conf file (%s), using defaults",
		     __func__, conf_path);
		cg_conf_exist = false;
	} else {
		debug("Reading cgroup.conf file %s", conf_path);

		tbl = s_p_hashtbl_create(options);
		if (s_p_parse_file(tbl, NULL, conf_path, 0, NULL) ==
		    SLURM_ERROR) {
			fatal("Could not open/read/parse cgroup.conf file %s",
			      conf_path);
		}

		/* cgroup initialization parameters */
		if (s_p_get_string(&tmp_str, "CgroupMountpoint", tbl)) {
			/* Remove the trailing / if any. */
			sz = strlen(tmp_str);
			if (*(tmp_str + sz - 1) == '/')
				*(tmp_str + sz - 1) = '\0';
			xfree(slurm_cgroup_conf.cgroup_mountpoint);
			slurm_cgroup_conf.cgroup_mountpoint = tmp_str;
			tmp_str = NULL;
		}
		if (s_p_get_string(&tmp_str, "CgroupReleaseAgentDir", tbl)) {
			xfree(tmp_str);
			fatal("Support for CgroupReleaseAgentDir option has been removed.");
		}

		/* Cores constraints related conf items */
		(void) s_p_get_boolean(&slurm_cgroup_conf.constrain_cores,
				       "ConstrainCores", tbl);

		/* RAM and Swap constraints related conf items */
		(void) s_p_get_boolean(&slurm_cgroup_conf.constrain_ram_space,
				       "ConstrainRAMSpace", tbl);

		(void) s_p_get_float(&slurm_cgroup_conf.allowed_ram_space,
				     "AllowedRAMSpace", tbl);

		(void) s_p_get_float(&slurm_cgroup_conf.max_ram_percent,
				     "MaxRAMPercent", tbl);

		(void) s_p_get_boolean(&slurm_cgroup_conf.constrain_swap_space,
				       "ConstrainSwapSpace", tbl);

		(void) s_p_get_float(&slurm_cgroup_conf.allowed_swap_space,
				     "AllowedSwapSpace", tbl);

		(void) s_p_get_float(&slurm_cgroup_conf.max_swap_percent,
				     "MaxSwapPercent", tbl);

		(void) s_p_get_uint64 (&slurm_cgroup_conf.min_ram_space,
				      "MinRAMSpace", tbl);

		if (s_p_get_uint64(&slurm_cgroup_conf.memory_swappiness,
				     "MemorySwappiness", tbl)) {
			if (slurm_cgroup_conf.memory_swappiness > 100) {
				error("Value for MemorySwappiness is too high, rounding down to 100.");
				slurm_cgroup_conf.memory_swappiness = 100;
			}
		}

		/* Devices constraint related conf items */
		(void) s_p_get_boolean(&slurm_cgroup_conf.constrain_devices,
				       "ConstrainDevices", tbl);

		if (s_p_get_string(&tmp_str, "AllowedDevicesFile", tbl)) {
			xfree(tmp_str);
			warning("AllowedDevicesFile option is obsolete, please remove it from your configuration.");
		}

		if (s_p_get_string(&tmp_str, "CgroupPlugin", tbl)) {
			xfree(slurm_cgroup_conf.cgroup_plugin);
			slurm_cgroup_conf.cgroup_plugin = tmp_str;
			tmp_str = NULL;
		}

		if (s_p_get_boolean(&slurm_cgroup_conf.ignore_systemd,
				    "IgnoreSystemd", tbl)) {
			/* Implicitly set these other one. */
			slurm_cgroup_conf.ignore_systemd_on_failure = true;
		}

		if (!slurm_cgroup_conf.ignore_systemd &&
		    (!s_p_get_boolean(
			    &slurm_cgroup_conf.ignore_systemd_on_failure,
			    "IgnoreSystemdOnFailure", tbl)))
			slurm_cgroup_conf.ignore_systemd_on_failure = false;

		(void) s_p_get_boolean(&slurm_cgroup_conf.enable_controllers,
				       "EnableControllers", tbl);
		(void) s_p_get_boolean(
			&slurm_cgroup_conf.signal_children_processes,
			"SignalChildrenProcesses", tbl);

		(void) s_p_get_uint64(&slurm_cgroup_conf.systemd_timeout,
				      "SystemdTimeout", tbl);

		s_p_hashtbl_destroy(tbl);
	}

	xfree(conf_path);

	return;
}

/* Autodetect logic inspired from systemd source code */
extern char *autodetect_cgroup_version(void)
{
#ifdef WITH_CGROUP
	struct statfs fs;
	int cgroup_ver = -1;

	if (statfs("/sys/fs/cgroup/", &fs) < 0) {
		error("cgroup filesystem not mounted in /sys/fs/cgroup/");
		return NULL;
	}

	if (F_TYPE_EQUAL(fs.f_type, CGROUP2_SUPER_MAGIC))
		cgroup_ver = 2;
	else if (F_TYPE_EQUAL(fs.f_type, TMPFS_MAGIC)) {
		if (statfs("/sys/fs/cgroup/systemd/", &fs) != 0) {
			error("can't stat /sys/fs/cgroup/systemd/: %m");
			return NULL;
		}

		if (F_TYPE_EQUAL(fs.f_type, CGROUP2_SUPER_MAGIC)) {
			if (statfs("/sys/fs/cgroup/unified/", &fs) != 0) {
				error("can't stat /sys/fs/cgroup/unified/: %m");
				return NULL;
			}
			cgroup_ver = 2;
		} else if (F_TYPE_EQUAL(fs.f_type, CGROUP_SUPER_MAGIC)) {
			cgroup_ver = 1;
		} else {
			error("Unexpected fs type on /sys/fs/cgroup/systemd");
			return NULL;
		}
	} else if (F_TYPE_EQUAL(fs.f_type, SYSFS_MAGIC)) {
		error("No filesystem mounted on /sys/fs/cgroup");
		return NULL;
	} else {
		error("Unknown filesystem type mounted on /sys/fs/cgroup");
		return NULL;
	}

	log_flag(CGROUP, "%s: using cgroup version %d", __func__, cgroup_ver);

	switch (cgroup_ver) {
	case 1:
		return "cgroup/v1";
		break;
	case 2:
		return "cgroup/v2";
		break;
	default:
		error("unsupported cgroup version %d", cgroup_ver);
		break;
	}
#endif

	return NULL;
}

/*
 * cgroup_conf_init - load the cgroup.conf configuration.
 *
 * RET SLURM_SUCCESS if conf file is initialized. If the cgroup conf was
 *     already initialized, return SLURM_ERROR.
 */
extern int cgroup_conf_init(void)
{
	int rc = SLURM_SUCCESS;

	slurm_rwlock_wrlock(&cg_conf_lock);

	if (!cg_conf_inited) {
		_init_slurm_cgroup_conf();
		_read_slurm_cgroup_conf();
		if (running_in_slurmd()) {
			/*
			 * Initialize and pack cgroup.conf info into a buffer
			 * that can be used by slurmd to send to stepd every
			 * time, instead of re-packing every time we want to
			 * send to slurmstepd
			 */
			cg_conf_buf = init_buf(0);
			_pack_cgroup_conf(cg_conf_buf);
		}
		cg_conf_inited = true;
	} else
		rc = SLURM_ERROR;

	slurm_rwlock_unlock(&cg_conf_lock);
	return rc;
}

extern void cgroup_conf_destroy(void)
{
	xassert(cg_conf_inited);
	_cgroup_conf_fini();
}

extern void cgroup_free_limits(cgroup_limits_t *limits)
{
	if (!limits)
		return;

	xfree(limits->allow_cores);
	xfree(limits->allow_mems);
	xfree(limits);
}

extern void cgroup_init_limits(cgroup_limits_t *limits)
{
	if (!limits)
		return;

	memset(limits, 0, sizeof(*limits));

	limits->taskid = NO_VAL;
	limits->device.type = DEV_TYPE_NONE;
	limits->device.major = NO_VAL;
	limits->device.minor = NO_VAL;
	limits->limit_in_bytes = NO_VAL64;
	limits->soft_limit_in_bytes = NO_VAL64;
	limits->memsw_limit_in_bytes = NO_VAL64;
	limits->swappiness = NO_VAL64;
}

/*
 * get_slurm_cgroup_conf - load the Slurm cgroup configuration from the
 *      cgroup.conf  file and return a key pair <name,value> ordered list.
 * RET List with cgroup.conf <name,value> pairs if no error, NULL otherwise.
 */
extern list_t *cgroup_get_conf_list(void)
{
	list_t *cgroup_conf_l;
	cgroup_conf_t *cg_conf = &slurm_cgroup_conf;

	xassert(cg_conf_inited);

	cgroup_conf_l = list_create(destroy_config_key_pair);

	slurm_rwlock_rdlock(&cg_conf_lock);

	add_key_pair(cgroup_conf_l, "CgroupMountpoint", "%s",
		     cg_conf->cgroup_mountpoint);
	add_key_pair_bool(cgroup_conf_l, "ConstrainCores",
			  cg_conf->constrain_cores);
	add_key_pair_bool(cgroup_conf_l, "ConstrainRAMSpace",
			  cg_conf->constrain_ram_space);
	add_key_pair(cgroup_conf_l, "AllowedRAMSpace", "%.1f%%",
		     cg_conf->allowed_ram_space);
	add_key_pair(cgroup_conf_l, "MaxRAMPercent", "%.1f%%",
		     cg_conf->max_ram_percent);
	add_key_pair(cgroup_conf_l, "MinRAMSpace", "%"PRIu64"MB",
		     cg_conf->min_ram_space);
	add_key_pair_bool(cgroup_conf_l, "ConstrainSwapSpace",
			  cg_conf->constrain_swap_space);
	add_key_pair(cgroup_conf_l, "AllowedSwapSpace", "%.1f%%",
		     cg_conf->allowed_swap_space);
	add_key_pair(cgroup_conf_l, "MaxSwapPercent", "%.1f%%",
		     cg_conf->max_swap_percent);
	add_key_pair_bool(cgroup_conf_l, "ConstrainDevices",
			  cg_conf->constrain_devices);
	add_key_pair(cgroup_conf_l, "CgroupPlugin", "%s",
		     cg_conf->cgroup_plugin);
	add_key_pair_bool(cgroup_conf_l, "IgnoreSystemd",
			  cg_conf->ignore_systemd);
	add_key_pair_bool(cgroup_conf_l, "IgnoreSystemdOnFailure",
			  cg_conf->ignore_systemd_on_failure);
	add_key_pair_bool(cgroup_conf_l, "EnableControllers",
			  cg_conf->enable_controllers);

	if (cg_conf->memory_swappiness != NO_VAL64)
		add_key_pair(cgroup_conf_l, "MemorySwappiness", "%"PRIu64,
			     cg_conf->memory_swappiness);
	else
		add_key_pair(cgroup_conf_l, "MemorySwappiness", "(null)");

	add_key_pair(cgroup_conf_l, "SystemdTimeout", "%"PRIu64" ms",
		     cg_conf->systemd_timeout);

	slurm_rwlock_unlock(&cg_conf_lock);

	list_sort(cgroup_conf_l, (ListCmpF) sort_key_pairs);

	return cgroup_conf_l;
}

/*
 * This function is called from slurmd to send the cgroup state (at present
 * only the scope path in cgroup/v2) to the recently forked slurmstepd, since
 * slurmstepd might not be able to infer the correct scope path when we are
 * running into a container.
 */
extern int cgroup_write_state(int fd)
{
	int len = 0;
	char *step_path = NULL;

	if (plugin_inited == PLUGIN_INITED) {
		step_path = (*(ops.get_scope_path))();
		if (step_path)
			len = strlen(step_path) + 1;
	}

	safe_write(fd, &len, sizeof(int));
	if (step_path)
		safe_write(fd, step_path, len);

	return SLURM_SUCCESS;
rwfail:
	return SLURM_ERROR;
}

/*
 * This function is called from slurmstepd before the cgroup plugin is
 * initialized. It records the cgroup plugin state passed from slurmd
 * (at present only the scope path in cgroup/v2) in this slurmstepd so it
 * can be later used by the plugin when it is initialized.
 */
extern int cgroup_read_state(int fd)
{
	int len;

	safe_read(fd, &len, sizeof(int));

	if (len)
		safe_read(fd, scope_path, len);

	return SLURM_SUCCESS;
rwfail:
	return SLURM_ERROR;
}

extern int cgroup_write_conf(int fd)
{
	int len;

	xassert(cg_conf_inited);

	slurm_rwlock_rdlock(&cg_conf_lock);
	len = get_buf_offset(cg_conf_buf);
	safe_write(fd, &len, sizeof(int));
	safe_write(fd, get_buf_data(cg_conf_buf), len);
	slurm_rwlock_unlock(&cg_conf_lock);

	return SLURM_SUCCESS;
rwfail:
	slurm_rwlock_unlock(&cg_conf_lock);
	return SLURM_ERROR;
}

extern int cgroup_read_conf(int fd)
{
	int len, rc;
	buf_t *buffer = NULL;

	slurm_rwlock_wrlock(&cg_conf_lock);

	safe_read(fd, &len, sizeof(int));
	buffer = init_buf(len);
	safe_read(fd, buffer->head, len);

	rc = _unpack_cgroup_conf(buffer);

	if (rc == SLURM_ERROR)
		fatal("%s: problem with unpack of cgroup.conf", __func__);

	FREE_NULL_BUFFER(buffer);

	cg_conf_inited = true;
	slurm_rwlock_unlock(&cg_conf_lock);

	return SLURM_SUCCESS;
rwfail:
	slurm_rwlock_unlock(&cg_conf_lock);
	FREE_NULL_BUFFER(buffer);

	return SLURM_ERROR;
}

extern bool cgroup_memcg_job_confinement(void)
{
	bool status = false;

	xassert(cg_conf_inited);

	/* read cgroup configuration */
	slurm_rwlock_rdlock(&cg_conf_lock);

	if (xstrcmp(slurm_cgroup_conf.cgroup_plugin, "disabled") &&
	    ((slurm_cgroup_conf.constrain_ram_space ||
	      slurm_cgroup_conf.constrain_swap_space) &&
	     xstrstr(slurm_conf.task_plugin, "cgroup")))
		status = true;

	slurm_rwlock_unlock(&cg_conf_lock);

	return status;
}

/*
 * Initialize Cgroup plugins.
 *
 * Returns a Slurm errno.
 */
extern int cgroup_g_init(void)
{
	int rc = SLURM_SUCCESS;
	char *plugin_type = "cgroup";
	char *type = NULL;

	slurm_mutex_lock(&g_context_lock);

	if (plugin_inited)
		goto done;

	if (cgroup_conf_init() != SLURM_SUCCESS)
		log_flag(CGROUP, "cgroup conf was already initialized.");

	type = slurm_cgroup_conf.cgroup_plugin;

	if (!xstrcmp(type, "disabled")) {
		plugin_inited = PLUGIN_NOOP;
		goto done;
	}

	if (!xstrcmp(type, "autodetect")) {
		if (!(type = autodetect_cgroup_version())) {
			rc = SLURM_ERROR;
			goto done;
		}
	}

	g_context = plugin_context_create(
		plugin_type, type, (void **)&ops, syms, sizeof(syms));

	if (!g_context) {
		error("cannot create %s context for %s", plugin_type, type);
		rc = SLURM_ERROR;
		plugin_inited = PLUGIN_NOT_INITED;
		goto done;
	}

	/*
	 * We have recorded the scope_path here previously, configure it now in
	 * the plugin.
	 */
	rc = (*(ops.setup_scope))(scope_path);
	if (rc == SLURM_ERROR) {
		error("cannot setup the scope for %s", plugin_type);
		goto done;
	}

	plugin_inited = PLUGIN_INITED;
done:
	slurm_mutex_unlock(&g_context_lock);

	return rc;
}

extern int cgroup_g_fini(void)
{
	int rc = SLURM_SUCCESS;

	slurm_mutex_lock(&g_context_lock);
	if (g_context) {
		rc = plugin_context_destroy(g_context);
		g_context = NULL;
	}

	cgroup_conf_destroy();
	plugin_inited = PLUGIN_NOT_INITED;

	slurm_mutex_unlock(&g_context_lock);
	return rc;
}

extern int cgroup_g_initialize(cgroup_ctl_type_t sub)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		fatal("%s: Trying to initialize cgroups but CgroupPlugin=disabled is set in cgroup.conf. Please, unset any configuration that is using cgroups.",
		      __func__);

	return (*(ops.initialize))(sub);
}

extern int cgroup_g_system_create(cgroup_ctl_type_t sub)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.system_create))(sub);
}

extern int cgroup_g_system_addto(cgroup_ctl_type_t sub, pid_t *pids, int npids)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.system_addto))(sub, pids, npids);
}

extern int cgroup_g_system_destroy(cgroup_ctl_type_t sub)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.system_destroy))(sub);
}

extern int cgroup_g_step_create(cgroup_ctl_type_t sub, stepd_step_rec_t *step)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_create))(sub, step);
}

extern int cgroup_g_step_addto(cgroup_ctl_type_t sub, pid_t *pids, int npids)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_addto))(sub, pids, npids);
}

extern int cgroup_g_step_get_pids(pid_t **pids, int *npids)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP) {
		*npids = 0;
		*pids = NULL;
		return SLURM_SUCCESS;
	}

	return (*(ops.step_get_pids))(pids, npids);
}

extern int cgroup_g_step_suspend(void)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_suspend))();
}

extern int cgroup_g_step_resume(void)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_resume))();
}

extern int cgroup_g_step_destroy(cgroup_ctl_type_t sub)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_destroy))(sub);
}

extern bool cgroup_g_has_pid(pid_t pid)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return false;

	return (*(ops.has_pid))(pid);
}

extern cgroup_limits_t *cgroup_g_constrain_get(cgroup_ctl_type_t sub,
					       cgroup_level_t level)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.constrain_get))(sub, level);
}

extern int cgroup_g_constrain_set(cgroup_ctl_type_t sub, cgroup_level_t level,
				  cgroup_limits_t *limits)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.constrain_set))(sub, level, limits);
}

extern int cgroup_g_constrain_apply(cgroup_ctl_type_t sub, cgroup_level_t level,
                                    uint32_t task_id)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.constrain_apply))(sub, level, task_id);
}

extern int cgroup_g_step_start_oom_mgr(stepd_step_rec_t *step)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.step_start_oom_mgr))(step);
}

extern cgroup_oom_t *cgroup_g_step_stop_oom_mgr(stepd_step_rec_t *step)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP) {
		cgroup_oom_t *empty_oom = xmalloc(sizeof(*empty_oom));
		return empty_oom;
	}

	return (*(ops.step_stop_oom_mgr))(step);
}

extern int cgroup_g_task_addto(cgroup_ctl_type_t sub, stepd_step_rec_t *step,
			       pid_t pid, uint32_t task_id)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.task_addto))(sub, step, pid, task_id);
}

extern cgroup_acct_t *cgroup_g_task_get_acct_data(uint32_t taskid)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP) {
		cgroup_acct_t *empty_acct = xmalloc(sizeof(*empty_acct));
		return empty_acct;
	}

	return (*(ops.task_get_acct_data))(taskid);
}

extern long int cgroup_g_get_acct_units(void)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return (long int)USEC_IN_SEC;

	return (*(ops.get_acct_units))();
}

extern bool cgroup_g_has_feature(cgroup_ctl_feature_t f)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return false;

	return (*(ops.has_feature))(f);
}
