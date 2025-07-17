/*****************************************************************************\
 *  local.c - Slurm REST auth local plugin
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

#include "config.h"

#define _GNU_SOURCE /* needed for SO_PEERCRED */

#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/un.h>
#endif

#include "slurm/slurm.h"
#include "slurm/slurmdb.h"

#include "src/common/data.h"
#include "src/common/log.h"
#include "src/interfaces/auth.h"
#include "src/common/uid.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmrestd/rest_auth.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "select" for Slurm node selection) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "REST auth/local";
const char plugin_type[] = "rest_auth/local";
const uint32_t plugin_id = 101;
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

extern int slurm_rest_auth_p_apply(rest_auth_context_t *context);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* protected by lock */
static bool become_user = false;

#define MAGIC 0xd11abee2
typedef struct {
	int magic;
	void *db_conn;
} plugin_data_t;

extern void *slurm_rest_auth_p_get_db_conn(rest_auth_context_t *context)
{
	plugin_data_t *data = context->plugin_data;
	xassert(data->magic == MAGIC);
	xassert(context->plugin_id == plugin_id);

	if (slurm_rest_auth_p_apply(context))
		return NULL;

	if (data->db_conn)
		return data->db_conn;

	errno = 0;
	data->db_conn = slurmdb_connection_get(NULL);

	if (!errno && data->db_conn)
		return data->db_conn;

	error("%s: unable to connect to slurmdbd: %m",
	      __func__);
	data->db_conn = NULL;

	return NULL;
}

static int _auth_socket(on_http_request_args_t *args,
			rest_auth_context_t *ctxt,
			const char *header_user_name)
{
	int rc;
	const char *name = conmgr_fd_get_name(args->context->con);
	uid_t cred_uid;
	gid_t cred_gid;
	pid_t cred_pid;

	xassert(!ctxt->user_name);

	if ((rc = conmgr_get_fd_auth_creds(args->context->con, &cred_uid,
					   &cred_gid, &cred_pid))) {
		/* socket may be remote, local auth doesn't apply */
		debug("%s: [%s] unable to get socket ownership: %s",
		      __func__, name, slurm_strerror(rc));
		return ESLURM_AUTH_CRED_INVALID;
	}

	if((cred_uid == -1) || (cred_gid == -1) || !cred_pid) {
		/* *_PEERCRED failed silently */
		error("%s: [%s] rejecting socket connection with invalid SO_PEERCRED response",
		      __func__, name);
		return ESLURM_AUTH_CRED_INVALID;
	} else if ((cred_uid == SLURM_AUTH_NOBODY) ||
		   (cred_gid == SLURM_AUTH_NOBODY)) {
		error("%s: [%s] rejecting connection from nobody",
		      __func__, name);
		return ESLURM_AUTH_CRED_INVALID;
	} else if (!cred_uid) {
		/* requesting socket is root */
		info("%s: [%s] accepted root socket connection with uid:%u gid:%u pid:%ld",
		     __func__, name, cred_uid, cred_gid, (long) cred_pid);

		/*
		 * root can be any user if they want - default to
		 * running user.
		 */
		if (header_user_name)
			ctxt->user_name = xstrdup(header_user_name);
		else
			ctxt->user_name = uid_to_string_or_null(getuid());
	} else if (getuid() == cred_uid) {
		info("%s: [%s] accepted user socket connection with uid:%u gid:%u pid:%ld",
		     __func__, name, cred_uid, cred_gid, (long) cred_pid);

		ctxt->user_name = uid_to_string_or_null(cred_uid);
	} else {
		/*
		 * Use lock to ensure there are no race conditions for different
		 * users trying to connect
		 */
		slurm_mutex_lock(&lock);
		if (become_user) {
			info("%s: [%s] accepted user proxy socket connection with uid:%u gid:%u pid:%ld",
			     __func__, name, cred_uid, cred_gid,
			     (long) cred_pid);

			if (getuid() || getgid())
				fatal("%s: user proxy mode requires running as root",
				      __func__);

			ctxt->user_name = uid_to_string_or_null(cred_uid);

			if (!ctxt->user_name)
				fatal("%s: [%s] unable to resolve user uid %u",
				      __func__, name, cred_uid);

			if (setgroups(0, NULL))
				fatal("Unable to drop supplementary groups: %m");

			if (setuid(cred_uid))
				fatal("%s: [%s] unable to switch to user uid %u: %m",
				      __func__, name, cred_uid);

			if ((getgid() != cred_gid) && setgid(cred_gid))
				fatal("%s: [%s] unable to switch to user gid %u: %m",
				      __func__, name, cred_gid);

			if ((getuid() != cred_uid) || (getgid() != cred_gid))
				fatal("%s: [%s] user switch sanity check failed",
				      __func__, name);

			/*
			 * Only allow user change once to ensure against replay
			 * attacks. Next attempt to connect will be forced to be
			 * the same user.
			 */
			become_user = false;
			slurm_mutex_unlock(&lock);
		} else {
			slurm_mutex_unlock(&lock);
			/* another user -> REJECT */
			error("%s: [%s] rejecting socket connection with uid:%u gid:%u pid:%ld",
			      __func__, name, cred_uid, cred_gid,
			      (long) cred_pid);
			return ESLURM_AUTH_CRED_INVALID;
		}
	}

	if (ctxt->user_name) {
		plugin_data_t *data = xmalloc(sizeof(*data));
		data->magic = MAGIC;
		ctxt->plugin_data = data;
		return SLURM_SUCCESS;
	} else
		return ESLURM_USER_ID_MISSING;
}

extern int slurm_rest_auth_p_authenticate(on_http_request_args_t *args,
					  rest_auth_context_t *ctxt)
{
	struct stat status = { 0 };
	const char *header_user_name = find_http_header(args->headers,
							HTTP_HEADER_USER_NAME);
	const conmgr_fd_status_t cstatus =
		conmgr_fd_get_status(args->context->con);
	const int input_fd = conmgr_fd_get_input_fd(args->context->con);
	const int output_fd = conmgr_fd_get_output_fd(args->context->con);
	const char *name = conmgr_fd_get_name(args->context->con);

	xassert(!ctxt->user_name);

	if ((input_fd < 0) || (output_fd < 0)) {
		/* local auth requires there to be a valid fd */
		debug3("%s: skipping auth local with invalid input_fd:%u output_fd:%u",
		       __func__, input_fd, output_fd);
		return ESLURM_AUTH_SKIP;
	}

	if (cstatus.is_socket && !cstatus.unix_socket) {
		/*
		 * SO_PEERCRED only works on unix sockets
		 */
		debug("%s: [%s] socket authentication only supported on UNIX sockets",
		      __func__, name);
		return ESLURM_AUTH_SKIP;
	} else if (cstatus.is_socket && cstatus.unix_socket) {
		return _auth_socket(args, ctxt, header_user_name);
	} else if (fstat(input_fd, &status)) {
		error("%s: [%s] unable to stat fd %d: %m",
		      __func__, name, input_fd);
		return ESLURM_AUTH_CRED_INVALID;
	} else if (S_ISCHR(status.st_mode) || S_ISFIFO(status.st_mode) ||
		   S_ISREG(status.st_mode)) {
		bool reject_proxy = false;

		slurm_mutex_lock(&lock);
		if (become_user)
			reject_proxy = true;
		slurm_mutex_unlock(&lock);

		if (reject_proxy) {
			error("%s: [%s] rejecting PIPE connection in become user mode",
			      __func__, name);
			return ESLURM_AUTH_CRED_INVALID;
		}

		if (status.st_mode & (S_ISUID | S_ISGID)) {
			/* FIFO has sticky bits -> REJECT */
			error("%s: [%s] rejecting PIPE connection sticky bits permissions: %07o",
			      __func__, name, status.st_mode);
			return ESLURM_AUTH_CRED_INVALID;
		} else if (status.st_mode & S_IRWXO) {
			/* FIFO has other read/write -> REJECT */
			error("%s: [%s] rejecting PIPE connection other read or write bits permissions: %07o",
			      __func__, name, status.st_mode);
			return ESLURM_AUTH_CRED_INVALID;
		} else if (status.st_uid == getuid()) {
			/* FIFO is owned by same user */
			ctxt->user_name = uid_to_string_or_null(status.st_uid);

			if (ctxt->user_name) {
				plugin_data_t *data = xmalloc(sizeof(*data));
				data->magic = MAGIC;
				ctxt->plugin_data = data;

				info("[%s] accepted connection from user: %s[%u]",
				     name, ctxt->user_name, status.st_uid);
				return SLURM_SUCCESS;
			} else {
				error("[%s] rejecting connection from unresolvable uid:%u",
				      name, status.st_uid);
				return ESLURM_USER_ID_MISSING;
			}
		}

		return ESLURM_AUTH_CRED_INVALID;
	} else {
		error("%s: [%s] rejecting unknown file type with mode:%07o blk:%u char:%u dir:%u fifo:%u reg:%u link:%u",
		      __func__, name, status.st_mode, S_ISBLK(status.st_mode),
		      S_ISCHR(status.st_mode), S_ISDIR(status.st_mode),
		      S_ISFIFO(status.st_mode), S_ISREG(status.st_mode),
		      S_ISLNK(status.st_mode));
		return ESLURM_AUTH_CRED_INVALID;
	}
}

extern int slurm_rest_auth_p_apply(rest_auth_context_t *context)
{
	int rc;
	char *user = uid_to_string_or_null(getuid());

	xassert(((plugin_data_t *) context->plugin_data)->magic == MAGIC);
	xassert(context->plugin_id == plugin_id);

	rc = auth_g_thread_config(NULL, context->user_name);

	xfree(user);

	return rc;
}

extern void slurm_rest_auth_p_free(rest_auth_context_t *context)
{
	plugin_data_t *data = context->plugin_data;
	xassert(data->magic == MAGIC);
	xassert(context->plugin_id == plugin_id);
	data->magic = ~MAGIC;

	if (data->db_conn)
		slurmdb_connection_close(&data->db_conn);

	xfree(context->plugin_data);
}

extern void slurm_rest_auth_p_init(bool bu)
{
	if (!bu) {
		debug3("%s: REST local auth activated", __func__);
	} else if (!getuid()) {
		slurm_mutex_lock(&lock);
		if (become_user)
			fatal("duplicate call to %s", __func__);

		become_user = true;
		slurm_mutex_unlock(&lock);

		debug3("%s: REST local auth with become user mode active",
		       __func__);
	} else {
		fatal("%s: become user mode requires running as root", __func__);
	}
}

extern void slurm_rest_auth_p_fini(void)
{
	debug5("%s: REST local auth deactivated", __func__);
}
