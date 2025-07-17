/*****************************************************************************\
 *  read_config.c - functions for reading slurmdbd.conf
 *****************************************************************************
 *  Copyright (C) 2003-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
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

#include "config.h"

#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"

#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/parse_config.h"
#include "src/common/parse_time.h"
#include "src/common/read_config.h"
#include "src/common/slurmdb_defs.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/accounting_storage.h"

#include "src/slurmdbd/read_config.h"

/* Global variables */
pthread_mutex_t conf_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Local functions */
static void _clear_slurmdbd_conf(void);

static time_t boot_time;

/*
 * free_slurmdbd_conf - free storage associated with the global variable
 *	slurmdbd_conf
 */
extern void free_slurmdbd_conf(void)
{
	slurm_mutex_lock(&conf_mutex);
	_clear_slurmdbd_conf();
	xfree(slurmdbd_conf);
	slurm_mutex_unlock(&conf_mutex);
}

static void _clear_slurmdbd_conf(void)
{
	init_slurm_conf(&slurm_conf);

	if (slurmdbd_conf) {
		xfree(slurmdbd_conf->archive_dir);
		xfree(slurmdbd_conf->archive_script);
		slurmdbd_conf->commit_delay = 0;
		xfree(slurmdbd_conf->dbd_addr);
		xfree(slurmdbd_conf->dbd_backup);
		xfree(slurmdbd_conf->dbd_host);
		slurmdbd_conf->dbd_port = 0;
		slurmdbd_conf->debug_level = LOG_LEVEL_INFO;
		xfree(slurmdbd_conf->default_qos);
		slurmdbd_conf->flags = 0;
		xfree(slurmdbd_conf->log_file);
		slurmdbd_conf->syslog_debug = LOG_LEVEL_END;
		xfree(slurmdbd_conf->parameters);
		xfree(slurmdbd_conf->pid_file);
		slurmdbd_conf->purge_event = 0;
		slurmdbd_conf->purge_job = 0;
		slurmdbd_conf->purge_resv = 0;
		slurmdbd_conf->purge_step = 0;
		slurmdbd_conf->purge_suspend = 0;
		slurmdbd_conf->purge_txn = 0;
		slurmdbd_conf->purge_usage = 0;
		xfree(slurmdbd_conf->storage_loc);
		slurmdbd_conf->track_wckey = 0;
		slurmdbd_conf->track_ctld = 0;
	}
}

/*
 * read_slurmdbd_conf - load the SlurmDBD configuration from the slurmdbd.conf
 *	file. Store result into global variable slurmdbd_conf.
 *	This function can be called more than once.
 * RET SLURM_SUCCESS if no error, otherwise an error code
 */
extern int read_slurmdbd_conf(void)
{
	s_p_options_t options[] = {
		{"AllowNoDefAcct", S_P_BOOLEAN},
		{"AllResourcesAbsolute", S_P_BOOLEAN},
		{"ArchiveDir", S_P_STRING},
		{"ArchiveEvents", S_P_BOOLEAN},
		{"ArchiveJobs", S_P_BOOLEAN},
		{"ArchiveResvs", S_P_BOOLEAN},
		{"ArchiveScript", S_P_STRING},
		{"ArchiveSteps", S_P_BOOLEAN},
		{"ArchiveSuspend", S_P_BOOLEAN},
		{"ArchiveTXN", S_P_BOOLEAN},
		{"ArchiveUsage", S_P_BOOLEAN},
		{"AuthAltTypes", S_P_STRING},
		{"AuthAltParameters", S_P_STRING},
		{"AuthInfo", S_P_STRING},
		{"AuthType", S_P_STRING},
		{"CommitDelay", S_P_UINT16},
		{"CommunicationParameters", S_P_STRING},
		{"DbdAddr", S_P_STRING},
		{"DbdBackupHost", S_P_STRING},
		{"DbdHost", S_P_STRING},
		{"DbdPort", S_P_UINT16},
		{"DebugFlags", S_P_STRING},
		{"DebugLevel", S_P_STRING},
		{"DebugLevelSyslog", S_P_STRING},
		{"DefaultQOS", S_P_STRING},
		{"DisableCoordDBD", S_P_BOOLEAN},
		{"HashPlugin", S_P_STRING},
		{"JobPurge", S_P_UINT32},
		{"LogFile", S_P_STRING},
		{"LogTimeFormat", S_P_STRING},
		{"MaxQueryTimeRange", S_P_STRING},
		{"MessageTimeout", S_P_UINT16},
		{"Parameters", S_P_STRING},
		{"PidFile", S_P_STRING},
		{"PluginDir", S_P_STRING},
		{"PrivateData", S_P_STRING},
		{"PurgeEventAfter", S_P_STRING},
		{"PurgeJobAfter", S_P_STRING},
		{"PurgeResvAfter", S_P_STRING},
		{"PurgeStepAfter", S_P_STRING},
		{"PurgeSuspendAfter", S_P_STRING},
		{"PurgeTXNAfter", S_P_STRING},
		{"PurgeUsageAfter", S_P_STRING},
		{"PurgeEventMonths", S_P_UINT32},
		{"PurgeJobMonths", S_P_UINT32},
		{"PurgeStepMonths", S_P_UINT32},
		{"PurgeSuspendMonths", S_P_UINT32},
		{"PurgeTXNMonths", S_P_UINT32},
		{"PurgeUsageMonths", S_P_UINT32},
		{"SlurmUser", S_P_STRING},
		{"StepPurge", S_P_UINT32},
		{"StorageBackupHost", S_P_STRING},
		{"StorageHost", S_P_STRING},
		{"StorageLoc", S_P_STRING},
		{"StorageParameters", S_P_STRING},
		{"StoragePass", S_P_STRING},
		{"StoragePort", S_P_UINT16},
		{"StorageType", S_P_STRING},
		{"StorageUser", S_P_STRING},
		{"TCPTimeout", S_P_UINT16},
		{"TLSParameters", S_P_STRING},
		{"TLSType", S_P_STRING},
		{"TrackWCKey", S_P_BOOLEAN},
		{"TrackSlurmctldDown", S_P_BOOLEAN},
		{NULL} };
	s_p_hashtbl_t *tbl = NULL;
	char *conf_path = NULL;
	char *temp_str = NULL;
	struct stat buf;

	/* Set initial values */
	slurm_mutex_lock(&conf_mutex);
	if (slurmdbd_conf == NULL) {
		slurmdbd_conf = xmalloc(sizeof(*slurmdbd_conf));
		boot_time = time(NULL);
	}
	_clear_slurmdbd_conf();

	/* set slurmdbd specific defaults */
	slurm_conf.keepalive_interval = DEFAULT_SLURMDBD_KEEPALIVE_INTERVAL;
	slurm_conf.keepalive_probes = DEFAULT_SLURMDBD_KEEPALIVE_PROBES;
	slurm_conf.keepalive_time = DEFAULT_SLURMDBD_KEEPALIVE_TIME;

	/* Get the slurmdbd.conf path and validate the file */
	conf_path = get_extra_conf_path("slurmdbd.conf");
	if ((conf_path == NULL) || (stat(conf_path, &buf) == -1)) {
		info("No slurmdbd.conf file (%s)", conf_path);
	} else {
		bool a_events = false, a_jobs = false, a_resv = false;
		bool a_steps = false, a_suspend = false, a_txn = false;
		bool a_usage = false;
		bool tmp_bool = false;
		uint32_t parse_flags = 0;
		uid_t conf_path_uid;
		debug3("Checking slurmdbd.conf file:%s access permissions",
		       conf_path);
		if ((buf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0600)
			fatal("slurmdbd.conf file %s should be 600 is %o accessible for group or others",
			      conf_path,
			      buf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));

		debug("Reading slurmdbd.conf file %s", conf_path);

		tbl = s_p_hashtbl_create(options);
		parse_flags |= PARSE_FLAGS_CHECK_PERMISSIONS;
		if (s_p_parse_file(tbl, NULL, conf_path, parse_flags, NULL)
		    == SLURM_ERROR) {
			fatal("Could not open/read/parse slurmdbd.conf file %s",
			      conf_path);
		}
		conf_path_uid = buf.st_uid;


		if (!s_p_get_string(&slurmdbd_conf->archive_dir, "ArchiveDir",
				    tbl))
			slurmdbd_conf->archive_dir =
				xstrdup(DEFAULT_SLURMDBD_ARCHIVE_DIR);

		tmp_bool = false;
		s_p_get_boolean(&tmp_bool, "AllowNoDefAcct", tbl);
		if (tmp_bool)
			slurmdbd_conf->flags |= DBD_CONF_FLAG_ALLOW_NO_DEF_ACCT;
		s_p_get_boolean(&tmp_bool, "AllResourcesAbsolute", tbl);
		if (tmp_bool)
			slurmdbd_conf->flags |= DBD_CONF_FLAG_ALL_RES_ABS;

		s_p_get_boolean(&a_events, "ArchiveEvents", tbl);
		s_p_get_boolean(&a_jobs, "ArchiveJobs", tbl);
		s_p_get_boolean(&a_resv, "ArchiveResvs", tbl);
		s_p_get_string(&slurmdbd_conf->archive_script, "ArchiveScript",
			       tbl);
		s_p_get_boolean(&a_steps, "ArchiveSteps", tbl);
		s_p_get_boolean(&a_suspend, "ArchiveSuspend", tbl);
		s_p_get_boolean(&a_txn, "ArchiveTXN", tbl);
		s_p_get_boolean(&a_usage, "ArchiveUsage", tbl);
		s_p_get_string(&slurm_conf.authalttypes, "AuthAltTypes", tbl);
		s_p_get_string(&slurm_conf.authalt_params, "AuthAltParameters",
			       tbl);
		s_p_get_string(&slurm_conf.authinfo, "AuthInfo", tbl);
		s_p_get_string(&slurm_conf.authtype, "AuthType", tbl);
		s_p_get_uint16(&slurmdbd_conf->commit_delay,
			       "CommitDelay", tbl);
		s_p_get_string(&slurm_conf.comm_params,
			       "CommunicationParameters", tbl);

		/*
		 * IPv4 on by default, can be disabled.
		 * IPv6 off by default, can be turned on.
		 */
		slurm_conf.conf_flags |= CONF_FLAG_IPV4_ENABLED;
		if (xstrcasestr(slurm_conf.comm_params, "EnableIPv6"))
			slurm_conf.conf_flags |= CONF_FLAG_IPV6_ENABLED;
		if (xstrcasestr(slurm_conf.comm_params, "DisableIPv4"))
			slurm_conf.conf_flags &= ~CONF_FLAG_IPV4_ENABLED;

		if (!(slurm_conf.conf_flags & CONF_FLAG_IPV4_ENABLED) &&
		    !(slurm_conf.conf_flags & CONF_FLAG_IPV6_ENABLED))
			fatal("Both IPv4 and IPv6 support disabled, cannot communicate");

		if ((temp_str = xstrcasestr(slurm_conf.comm_params,
					    "keepaliveinterval="))) {
			long tmp_val = strtol(temp_str + 18, NULL, 10);
			if (tmp_val >= 0 && tmp_val <= INT_MAX)
				slurm_conf.keepalive_interval = tmp_val;
			else
				error("CommunicationParameters option keepaliveinterval=%ld is invalid, ignored",
				      tmp_val);
		}

		if ((temp_str = xstrcasestr(slurm_conf.comm_params,
					    "keepaliveprobes="))) {
			long tmp_val = strtol(temp_str + 16, NULL, 10);
			if (tmp_val >= 0 && tmp_val <= INT_MAX)
				slurm_conf.keepalive_probes = tmp_val;
			else
				error("CommunicationParameters option keepaliveprobes=%ld is invalid, ignored",
				      tmp_val);
		}

		if ((temp_str = xstrcasestr(slurm_conf.comm_params,
					    "keepalivetime="))) {
			long tmp_val = strtol(temp_str + 14, NULL, 10);
			if (tmp_val >= 0 && tmp_val <= INT_MAX)
				slurm_conf.keepalive_time = tmp_val;
			else
				error("CommunicationParameters option keepalivetime=%ld is invalid, ignored",
				      tmp_val);
		}

		s_p_get_string(&slurmdbd_conf->dbd_backup,
			       "DbdBackupHost", tbl);
		s_p_get_string(&slurmdbd_conf->dbd_host, "DbdHost", tbl);
		s_p_get_string(&slurmdbd_conf->dbd_addr, "DbdAddr", tbl);
		s_p_get_uint16(&slurmdbd_conf->dbd_port, "DbdPort", tbl);

		if (s_p_get_string(&temp_str, "DebugFlags", tbl)) {
			if (debug_str2flags(temp_str, &slurm_conf.debug_flags)
			    != SLURM_SUCCESS)
				fatal("DebugFlags invalid: %s", temp_str);
			xfree(temp_str);
		} else	/* Default: no DebugFlags */
			slurm_conf.debug_flags = 0;

		if (s_p_get_string(&temp_str, "DebugLevel", tbl)) {
			slurmdbd_conf->debug_level = log_string2num(temp_str);
			if (slurmdbd_conf->debug_level == NO_VAL16)
				fatal("Invalid DebugLevel %s", temp_str);
			xfree(temp_str);
		}

		s_p_get_string(&slurmdbd_conf->default_qos, "DefaultQOS", tbl);
		if (s_p_get_uint32(&slurmdbd_conf->purge_job,
				   "JobPurge", tbl)) {
			if (!slurmdbd_conf->purge_job)
				slurmdbd_conf->purge_job = NO_VAL;
			else
				slurmdbd_conf->purge_job |=
					SLURMDB_PURGE_MONTHS;
		}

		s_p_get_boolean(&tmp_bool, "DisableCoordDBD", tbl);
		if (tmp_bool)
			slurmdbd_conf->flags |=
				DBD_CONF_FLAG_DISABLE_COORD_DBD;

		if (!s_p_get_string(&slurm_conf.hash_plugin, "HashPlugin", tbl))
			slurm_conf.hash_plugin = xstrdup(DEFAULT_HASH_PLUGIN);

		s_p_get_string(&slurmdbd_conf->log_file, "LogFile", tbl);

		if (s_p_get_string(&temp_str, "DebugLevelSyslog", tbl)) {
			slurmdbd_conf->syslog_debug = log_string2num(temp_str);
			if (slurmdbd_conf->syslog_debug == NO_VAL16)
				fatal("Invalid DebugLevelSyslog %s", temp_str);
			xfree(temp_str);
		}

		/* Default log time format */
		slurm_conf.log_fmt = LOG_FMT_ISO8601_MS;
		if (s_p_get_string(&temp_str, "LogTimeFormat", tbl)) {
			if (xstrcasestr(temp_str, "iso8601_ms"))
				slurm_conf.log_fmt = LOG_FMT_ISO8601_MS;
			else if (xstrcasestr(temp_str, "iso8601"))
				slurm_conf.log_fmt = LOG_FMT_ISO8601;
			else if (xstrcasestr(temp_str, "rfc5424_ms"))
				slurm_conf.log_fmt = LOG_FMT_RFC5424_MS;
			else if (xstrcasestr(temp_str, "rfc5424"))
				slurm_conf.log_fmt = LOG_FMT_RFC5424;
			else if (xstrcasestr(temp_str, "rfc3339"))
				slurm_conf.log_fmt = LOG_FMT_RFC3339;
			else if (xstrcasestr(temp_str, "clock"))
				slurm_conf.log_fmt = LOG_FMT_CLOCK;
			else if (xstrcasestr(temp_str, "short"))
				slurm_conf.log_fmt = LOG_FMT_SHORT;
			else if (xstrcasestr(temp_str, "thread_id"))
				slurm_conf.log_fmt = LOG_FMT_THREAD_ID;
			if (xstrcasestr(temp_str, "format_stderr"))
				slurm_conf.log_fmt |= LOG_FMT_FORMAT_STDERR;
			xfree(temp_str);
		}

		if (s_p_get_string(&temp_str, "MaxQueryTimeRange", tbl)) {
			slurmdbd_conf->max_time_range = time_str2secs(temp_str);
			xfree(temp_str);
		} else {
			slurmdbd_conf->max_time_range = INFINITE;
		}

		if (!s_p_get_uint16(&slurm_conf.msg_timeout, "MessageTimeout",
		                    tbl))
			slurm_conf.msg_timeout = DEFAULT_MSG_TIMEOUT;
		else if (slurm_conf.msg_timeout > 100)
			warning("MessageTimeout is too high for effective fault-tolerance");

		s_p_get_string(&slurmdbd_conf->parameters, "Parameters", tbl);
		if (slurmdbd_conf->parameters) {
			if (xstrcasestr(slurmdbd_conf->parameters,
					"PreserveCaseUser"))
				slurmdbd_conf->persist_conn_rc_flags |=
					PERSIST_FLAG_P_USER_CASE;
		}

		s_p_get_string(&slurmdbd_conf->pid_file, "PidFile", tbl);
		s_p_get_string(&slurm_conf.plugindir, "PluginDir", tbl);

		slurm_conf.private_data = 0; /* default visible to all */
		if (s_p_get_string(&temp_str, "PrivateData", tbl)) {
			if (xstrcasestr(temp_str, "account"))
				slurm_conf.private_data
					|= PRIVATE_DATA_ACCOUNTS;
			if (xstrcasestr(temp_str, "job"))
				slurm_conf.private_data
					|= PRIVATE_DATA_JOBS;
			if (xstrcasestr(temp_str, "event"))
				slurm_conf.private_data
					|= PRIVATE_DATA_EVENTS;
			if (xstrcasestr(temp_str, "node"))
				slurm_conf.private_data
					|= PRIVATE_DATA_NODES;
			if (xstrcasestr(temp_str, "partition"))
				slurm_conf.private_data
					|= PRIVATE_DATA_PARTITIONS;
			if (xstrcasestr(temp_str, "reservation"))
				slurm_conf.private_data
					|= PRIVATE_DATA_RESERVATIONS;
			if (xstrcasestr(temp_str, "usage"))
				slurm_conf.private_data
					|= PRIVATE_DATA_USAGE;
			if (xstrcasestr(temp_str, "user"))
				slurm_conf.private_data
					|= PRIVATE_DATA_USERS;
			if (xstrcasestr(temp_str, "all"))
				slurm_conf.private_data = 0xffff;
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeEventAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
			if ((slurmdbd_conf->purge_event =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeEventAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeJobAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
  			if ((slurmdbd_conf->purge_job =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeJobAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeResvAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
			if ((slurmdbd_conf->purge_resv =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeResvAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeStepAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
  			if ((slurmdbd_conf->purge_step =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeStepAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeSuspendAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
 			if ((slurmdbd_conf->purge_suspend =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeSuspendAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeTXNAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
 			if ((slurmdbd_conf->purge_txn =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeTXNAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_string(&temp_str, "PurgeUsageAfter", tbl)) {
			/* slurmdb_parse_purge will set SLURMDB_PURGE_FLAGS */
			if ((slurmdbd_conf->purge_usage =
			     slurmdb_parse_purge(temp_str)) == NO_VAL) {
				fatal("Bad value \"%s\" for PurgeUsageAfter",
				      temp_str);
			}
			xfree(temp_str);
		}
		if (s_p_get_uint32(&slurmdbd_conf->purge_event,
				   "PurgeEventMonths", tbl)) {
			if (!slurmdbd_conf->purge_event)
				slurmdbd_conf->purge_event = NO_VAL;
			else
				slurmdbd_conf->purge_event |=
					SLURMDB_PURGE_MONTHS;
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_job,
				   "PurgeJobMonths", tbl)) {
			if (!slurmdbd_conf->purge_job)
				slurmdbd_conf->purge_job = NO_VAL;
			else
				slurmdbd_conf->purge_job |=
					SLURMDB_PURGE_MONTHS;
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_step,
				   "PurgeStepMonths", tbl)) {
			if (!slurmdbd_conf->purge_step)
				slurmdbd_conf->purge_step = NO_VAL;
			else
				slurmdbd_conf->purge_step |=
					SLURMDB_PURGE_MONTHS;
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_suspend,
				   "PurgeSuspendMonths", tbl)) {
			if (!slurmdbd_conf->purge_suspend)
				slurmdbd_conf->purge_suspend = NO_VAL;
			else
				slurmdbd_conf->purge_suspend
					|= SLURMDB_PURGE_MONTHS;
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_txn,
				   "PurgeTXNMonths", tbl)) {
			if (!slurmdbd_conf->purge_txn)
				slurmdbd_conf->purge_txn = NO_VAL;
			else
				slurmdbd_conf->purge_txn
					|= SLURMDB_PURGE_MONTHS;
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_usage,
				   "PurgeUsageMonths", tbl)) {
			if (!slurmdbd_conf->purge_usage)
				slurmdbd_conf->purge_usage = NO_VAL;
			else
				slurmdbd_conf->purge_usage
					|= SLURMDB_PURGE_MONTHS;
		}

		s_p_get_string(&slurm_conf.slurm_user_name, "SlurmUser", tbl);

		if (slurm_conf.slurm_user_name) {
			uid_t uid;

			if (uid_from_string(slurm_conf.slurm_user_name, &uid) < 0)
				fatal("failed to look up SlurmUser uid");

			if (conf_path_uid != uid)
				fatal("slurmdbd.conf owned by %u not SlurmUser(%u)",
				      conf_path_uid, uid);
		}

		if (s_p_get_uint32(&slurmdbd_conf->purge_step,
				   "StepPurge", tbl)) {
			if (!slurmdbd_conf->purge_step)
				slurmdbd_conf->purge_step = NO_VAL;
			else
				slurmdbd_conf->purge_step |=
					SLURMDB_PURGE_MONTHS;
		}

		s_p_get_string(&slurm_conf.accounting_storage_backup_host,
			       "StorageBackupHost", tbl);
		s_p_get_string(&slurm_conf.accounting_storage_host,
			       "StorageHost", tbl);
		s_p_get_string(&slurmdbd_conf->storage_loc,
			       "StorageLoc", tbl);
		s_p_get_string(&slurm_conf.accounting_storage_params,
			       "StorageParameters", tbl);
		s_p_get_string(&slurm_conf.accounting_storage_pass,
			       "StoragePass", tbl);
		s_p_get_uint16(&slurm_conf.accounting_storage_port,
		               "StoragePort", tbl);
		s_p_get_string(&slurm_conf.accounting_storage_type,
		               "StorageType", tbl);
		s_p_get_string(&slurm_conf.accounting_storage_user,
			       "StorageUser", tbl);

		if (!s_p_get_uint16(&slurm_conf.tcp_timeout, "TCPTimeout", tbl))
			slurm_conf.tcp_timeout = DEFAULT_TCP_TIMEOUT;

		s_p_get_string(&slurm_conf.tls_params, "TLSParameters", tbl);
		if (!s_p_get_string(&slurm_conf.tls_type, "TLSType", tbl))
			slurm_conf.tls_type = xstrdup(DEFAULT_TLS_TYPE);

		if (!s_p_get_boolean((bool *)&slurmdbd_conf->track_wckey,
				     "TrackWCKey", tbl))
			slurmdbd_conf->track_wckey = false;

		if (!s_p_get_boolean((bool *)&slurmdbd_conf->track_ctld,
				     "TrackSlurmctldDown", tbl))
			slurmdbd_conf->track_ctld = false;

		if (a_events && slurmdbd_conf->purge_event)
			slurmdbd_conf->purge_event |= SLURMDB_PURGE_ARCHIVE;
		if (a_jobs && slurmdbd_conf->purge_job)
			slurmdbd_conf->purge_job |= SLURMDB_PURGE_ARCHIVE;
		if (a_resv && slurmdbd_conf->purge_resv)
			slurmdbd_conf->purge_resv |= SLURMDB_PURGE_ARCHIVE;
		if (a_steps && slurmdbd_conf->purge_step)
			slurmdbd_conf->purge_step |= SLURMDB_PURGE_ARCHIVE;
		if (a_suspend && slurmdbd_conf->purge_suspend)
			slurmdbd_conf->purge_suspend |= SLURMDB_PURGE_ARCHIVE;
		if (a_txn && slurmdbd_conf->purge_txn)
			slurmdbd_conf->purge_txn |= SLURMDB_PURGE_ARCHIVE;
		if (a_usage && slurmdbd_conf->purge_usage)
			slurmdbd_conf->purge_usage |= SLURMDB_PURGE_ARCHIVE;

		s_p_hashtbl_destroy(tbl);
	}

	xfree(conf_path);
	if (!slurm_conf.authtype)
		slurm_conf.authtype = xstrdup(DEFAULT_SLURMDBD_AUTHTYPE);
	if (slurmdbd_conf->dbd_host == NULL) {
		error("slurmdbd.conf lacks DbdHost parameter, "
		      "using 'localhost'");
		slurmdbd_conf->dbd_host = xstrdup("localhost");
	}
	if (slurmdbd_conf->dbd_addr == NULL)
		slurmdbd_conf->dbd_addr = xstrdup(slurmdbd_conf->dbd_host);
	if (slurmdbd_conf->pid_file == NULL)
		slurmdbd_conf->pid_file = xstrdup(DEFAULT_SLURMDBD_PIDFILE);
	if (slurmdbd_conf->dbd_port == 0)
		slurmdbd_conf->dbd_port = SLURMDBD_PORT;
	if (!slurm_conf.plugindir)
		slurm_conf.plugindir = xstrdup(default_plugin_path);
	if (slurm_conf.slurm_user_name) {
		if (uid_from_string(slurm_conf.slurm_user_name,
		                    &slurm_conf.slurm_user_id) < 0)
			fatal("Invalid user for SlurmUser %s, ignored",
			      slurm_conf.slurm_user_name);
	} else {
		slurm_conf.slurm_user_name = xstrdup("root");
		slurm_conf.slurm_user_id = 0;
	}

	if (!slurm_conf.accounting_storage_type)
		fatal("StorageType must be specified");
	if (!xstrcmp(slurm_conf.accounting_storage_type,
	             "accounting_storage/slurmdbd")) {
		fatal("StorageType=%s is invalid in slurmdbd.conf",
		      slurm_conf.accounting_storage_type);
	}

	if (!slurm_conf.accounting_storage_host)
		slurm_conf.accounting_storage_host =
			xstrdup(DEFAULT_STORAGE_HOST);

	if (!slurm_conf.accounting_storage_user)
		slurm_conf.accounting_storage_user = xstrdup(getlogin());

	if (!xstrcmp(slurm_conf.accounting_storage_type,
	             "accounting_storage/mysql")) {
		if (!slurm_conf.accounting_storage_port)
			slurm_conf.accounting_storage_port =
				DEFAULT_MYSQL_PORT;
		if (!slurmdbd_conf->storage_loc)
			slurmdbd_conf->storage_loc =
				xstrdup(DEFAULT_ACCOUNTING_DB);
	} else {
		if (!slurm_conf.accounting_storage_port)
			slurm_conf.accounting_storage_port =
				DEFAULT_STORAGE_PORT;
		if (!slurmdbd_conf->storage_loc)
			slurmdbd_conf->storage_loc =
				xstrdup(DEFAULT_STORAGE_LOC);
	}

	if (slurmdbd_conf->archive_dir) {
		if (stat(slurmdbd_conf->archive_dir, &buf) < 0)
			fatal("Failed to stat the archive directory %s: %m",
			      slurmdbd_conf->archive_dir);
		if (!(buf.st_mode & S_IFDIR))
			fatal("archive directory %s isn't a directory",
			      slurmdbd_conf->archive_dir);

		if (access(slurmdbd_conf->archive_dir, W_OK) < 0)
			fatal("archive directory %s is not writable",
			      slurmdbd_conf->archive_dir);
	}

	if (slurmdbd_conf->archive_script) {
		if (stat(slurmdbd_conf->archive_script, &buf) < 0)
			fatal("Failed to stat the archive script %s: %m",
			      slurmdbd_conf->archive_dir);

		if (!(buf.st_mode & S_IFREG))
			fatal("archive script %s isn't a regular file",
			      slurmdbd_conf->archive_script);

		if (access(slurmdbd_conf->archive_script, X_OK) < 0)
			fatal("archive script %s is not executable",
			      slurmdbd_conf->archive_script);
	}

	if (!slurmdbd_conf->purge_event)
		slurmdbd_conf->purge_event = NO_VAL;
	if (!slurmdbd_conf->purge_job)
		slurmdbd_conf->purge_job = NO_VAL;
	if (!slurmdbd_conf->purge_resv)
		slurmdbd_conf->purge_resv = NO_VAL;
	if (!slurmdbd_conf->purge_step)
		slurmdbd_conf->purge_step = NO_VAL;
	if (!slurmdbd_conf->purge_suspend)
		slurmdbd_conf->purge_suspend = NO_VAL;
	if (!slurmdbd_conf->purge_txn)
		slurmdbd_conf->purge_txn = NO_VAL;
	if (!slurmdbd_conf->purge_usage)
		slurmdbd_conf->purge_usage = NO_VAL;

	slurm_conf.last_update = time(NULL);
	slurm_mutex_unlock(&conf_mutex);
	return SLURM_SUCCESS;
}

/* Log the current configuration using verbose() */
extern void log_config(void)
{
	list_t *dbd_config_list;
	config_key_pair_t *key_pair;
	list_itr_t *itr;

	if (slurmdbd_conf->debug_level < LOG_LEVEL_DEBUG2)
		return;

	dbd_config_list = dump_config();

	itr = list_iterator_create(dbd_config_list);
	while ((key_pair = list_next(itr)))
		debug2("%-22s = %s", key_pair->name, key_pair->value);
	list_iterator_destroy(itr);

	FREE_NULL_LIST(dbd_config_list);
}

/*
 * Dump the configuration in name,value pairs for output to
 * "sacctmgr show config", caller must call list_destroy()
 */
extern list_t *dump_config(void)
{
	char time_str[32];
	char *tmp_ptr = NULL;
	list_t *my_list = list_create(destroy_config_key_pair);

	add_key_pair_bool(my_list, "AllowNoDefAcct",
		(slurmdbd_conf->flags & DBD_CONF_FLAG_ALLOW_NO_DEF_ACCT));

	add_key_pair(my_list, "ArchiveDir", "%s", slurmdbd_conf->archive_dir);

	add_key_pair_bool(my_list, "ArchiveEvents",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_event));

	add_key_pair_bool(my_list, "ArchiveJobs",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_job));

	add_key_pair_bool(my_list, "ArchiveResvs",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_resv));

	add_key_pair(my_list, "ArchiveScript", "%s",
		     slurmdbd_conf->archive_script);

	add_key_pair_bool(my_list, "ArchiveSteps",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_step));

	add_key_pair_bool(my_list, "ArchiveSuspend",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_suspend));

	add_key_pair_bool(my_list, "ArchiveTXN",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_txn));

	add_key_pair_bool(my_list, "ArchiveUsage",
		SLURMDB_PURGE_ARCHIVE_SET(slurmdbd_conf->purge_usage));

	add_key_pair(my_list, "AuthAltTypes", "%s", slurm_conf.authalttypes);

	add_key_pair(my_list, "AuthAltParameters", "%s",
		     slurm_conf.authalt_params);

	add_key_pair(my_list, "AuthInfo", "%s", slurm_conf.authinfo);

	add_key_pair(my_list, "AuthType", "%s", slurm_conf.authtype);

	tmp_ptr = xmalloc(256);
	slurm_make_time_str((time_t *)&boot_time, tmp_ptr, 256);
	add_key_pair(my_list, "BOOT_TIME", "%s", tmp_ptr);
	xfree(tmp_ptr);

	add_key_pair_bool(my_list, "CommitDelay", slurmdbd_conf->commit_delay);

	add_key_pair(my_list, "CommunicationParameters", "%s",
		     slurm_conf.comm_params);

	add_key_pair(my_list, "DbdAddr", "%s", slurmdbd_conf->dbd_addr);

	add_key_pair(my_list, "DbdBackupHost", "%s", slurmdbd_conf->dbd_backup);

	add_key_pair(my_list, "DbdHost", "%s", slurmdbd_conf->dbd_host);

	add_key_pair(my_list, "DbdPort", "%u", slurmdbd_conf->dbd_port);

	add_key_pair_own(my_list, "DebugFlags",
			 debug_flags2str(slurm_conf.debug_flags));

	add_key_pair(my_list, "DebugLevel", "%s",
		     log_num2string(slurmdbd_conf->debug_level));

	add_key_pair(my_list, "DebugLevelSyslog", "%s",
		     log_num2string(slurmdbd_conf->syslog_debug));

	add_key_pair(my_list, "DefaultQOS", "%s", slurmdbd_conf->default_qos);

	add_key_pair_bool(my_list, "DisableCoordDBD",
			  (slurmdbd_conf->flags &
			   DBD_CONF_FLAG_DISABLE_COORD_DBD));

	add_key_pair(my_list, "HashPlugin", "%s", slurm_conf.hash_plugin);

	add_key_pair(my_list, "LogFile", "%s", slurmdbd_conf->log_file);

	secs2time_str(slurmdbd_conf->max_time_range, time_str,
		      sizeof(time_str));
	add_key_pair(my_list, "MaxQueryTimeRange", "%s", time_str);

	add_key_pair(my_list, "MessageTimeout", "%u secs",
		     slurm_conf.msg_timeout);

	add_key_pair(my_list, "Parameters", "%s", slurmdbd_conf->parameters);

	add_key_pair(my_list, "PidFile", "%s", slurmdbd_conf->pid_file);

	add_key_pair(my_list, "PluginDir", "%s", slurm_conf.plugindir);

	tmp_ptr = xmalloc(128);
	private_data_string(slurm_conf.private_data, tmp_ptr, 128);
	add_key_pair(my_list, "PrivateData", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_event != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_event,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeEventAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_job != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_job,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeJobAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_resv != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_resv,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeResvAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_step != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_step,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeStepAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_suspend != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_suspend,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeSuspendAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_txn != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_txn,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeTXNAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	if (slurmdbd_conf->purge_usage != NO_VAL) {
		tmp_ptr = xmalloc(32);
		slurmdb_purge_string(slurmdbd_conf->purge_usage,
				     tmp_ptr, 32 , 1);
	} else
		tmp_ptr = xstrdup("NONE");

	add_key_pair(my_list, "PurgeUsageAfter", "%s", tmp_ptr);
	xfree(tmp_ptr);

	add_key_pair_own(my_list, "SLURMDBD_CONF",
			 get_extra_conf_path("slurmdbd.conf"));

	add_key_pair(my_list, "SLURMDBD_VERSION", "%s", SLURM_VERSION_STRING);

	add_key_pair(my_list, "SlurmUser", "%s(%u)", slurm_conf.slurm_user_name,
		     slurm_conf.slurm_user_id);

	add_key_pair(my_list, "StorageBackupHost", "%s",
		     slurm_conf.accounting_storage_backup_host);

	add_key_pair(my_list, "StorageHost", "%s",
		     slurm_conf.accounting_storage_host);

	add_key_pair(my_list, "StorageLoc", "%s", slurmdbd_conf->storage_loc);

	add_key_pair(my_list, "StorageParameters", "%s",
		     slurm_conf.accounting_storage_params);

	/* StoragePass should NOT be passed due to security reasons */

	add_key_pair(my_list, "StoragePort", "%u",
		     slurm_conf.accounting_storage_port);

	add_key_pair(my_list, "StorageType", "%s",
		     slurm_conf.accounting_storage_type);

	add_key_pair(my_list, "StorageUser", "%s",
		     slurm_conf.accounting_storage_user);

	add_key_pair(my_list, "TCPTimeout", "%u secs", slurm_conf.tcp_timeout);

	add_key_pair(my_list, "TLSParameters", "%s", slurm_conf.tls_params);

	add_key_pair(my_list, "TLSType", "%s", slurm_conf.tls_type);

	add_key_pair_bool(my_list, "TrackWCKey",
			  slurmdbd_conf->track_wckey);

	add_key_pair_bool(my_list, "TrackSlurmctldDown",
			  slurmdbd_conf->track_ctld);

	return my_list;
}
