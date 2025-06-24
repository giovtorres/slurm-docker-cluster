/****************************************************************************\
 *  slurmdbd_agent.c - functions to the agent talking to the SlurmDBD
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
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

#include "src/common/slurm_xlator.h"

#include "src/common/fd.h"
#include "src/common/slurmdbd_pack.h"
#include "src/common/xstring.h"

#include "slurmdbd_agent.h"

enum {
	MAX_DBD_ACTION_DISCARD,
	MAX_DBD_ACTION_EXIT
};

typedef struct {
	uint32_t msg_size;
	list_t *my_list;
} foreach_get_my_list_t;

persist_conn_t *slurmdbd_conn = NULL;


#define DBD_MAGIC		0xDEAD3219
#define DEBUG_PRINT_MAX_MSG_TYPES 10
#define MAX_DBD_DEFAULT_ACTION MAX_DBD_ACTION_DISCARD

static pthread_mutex_t agent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  agent_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t shutdown_cond = PTHREAD_COND_INITIALIZER;
static list_t *agent_list = NULL;
static pthread_t agent_tid      = 0;

static bool      halt_agent          = 0;
static time_t    slurmdbd_shutdown   = 0;
static bool      agent_running       = 0;

static pthread_mutex_t slurmdbd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  slurmdbd_cond = PTHREAD_COND_INITIALIZER;

static int max_dbd_msg_action = MAX_DBD_DEFAULT_ACTION;

typedef struct {
	list_t *id_rc_list;
	int rc;
} rc_msg_t;

extern int jobacct_storage_p_job_heavy(void *db_conn, job_record_t *job_ptr);

static int _sending_script_env(void *x, void *args)
{
	dbd_id_rc_msg_t *id_ptr = x;
	job_record_t *job_ptr;

	xassert(id_ptr);

	if (!(job_ptr = find_job_record(id_ptr->job_id)))
		return 0;

	xassert(job_ptr);
	xassert(job_ptr->details);

	if ((slurm_conf.conf_flags & CONF_FLAG_SJS) &&
	    (id_ptr->flags & JOB_SEND_SCRIPT) &&
	    job_ptr->details->script_hash)
		job_ptr->bit_flags |= JOB_SEND_SCRIPT;
	if ((slurm_conf.conf_flags & CONF_FLAG_SJE) &&
	    (id_ptr->flags & JOB_SEND_ENV) &&
	    job_ptr->details->env_hash)
		job_ptr->bit_flags |= JOB_SEND_ENV;

	if (jobacct_storage_p_job_heavy(slurmdbd_conn, job_ptr) ==
	    SLURM_SUCCESS) {
		job_ptr->bit_flags &= ~JOB_SEND_SCRIPT;
		job_ptr->bit_flags &= ~JOB_SEND_ENV;
	}

	return 0;
}

static bool _add_sending_script_env(dbd_id_rc_msg_t *id_ptr, rc_msg_t *rc_msg)
{
	xassert(id_ptr);

	if (!(id_ptr->flags & (JOB_SEND_SCRIPT | JOB_SEND_ENV)))
		return false;
	/*
	 * We are in the agent_lock here, we can't call
	 * jobacct_storage_p_job_heavy() which will call slurmdbd_agent_send()
	 * creating deadlock. Add message to a list to handle things later.
	 */
	if (!rc_msg->id_rc_list)
		rc_msg->id_rc_list = list_create(slurmdbd_free_id_rc_msg);
	list_append(rc_msg->id_rc_list, id_ptr);

	return true;
}

static void _process_id_rc_list(list_t *id_rc_list)
{
	slurmctld_lock_t job_write_lock = {
		.job = WRITE_LOCK,
	};

	if (!id_rc_list)
		return;

	lock_slurmctld(job_write_lock);
	(void) list_for_each(id_rc_list, _sending_script_env, NULL);
	unlock_slurmctld(job_write_lock);
	FREE_NULL_LIST(id_rc_list);
}

static int _unpack_return_code(uint16_t rpc_version, buf_t *buffer,
			       rc_msg_t *rc_msg)
{
	uint16_t msg_type = -1;
	persist_rc_msg_t *msg;
	dbd_id_rc_msg_t *id_msg;
	persist_msg_t resp;
	int rc = SLURM_ERROR;

	xassert(rc_msg);

	memset(&resp, 0, sizeof(persist_msg_t));
	if ((rc = unpack_slurmdbd_msg(&resp, slurmdbd_conn->version, buffer))
	    != SLURM_SUCCESS) {
		error("unpack message error");
		return rc;
	}

	switch (resp.msg_type) {
	case DBD_ID_RC:
		id_msg = resp.data;
		rc = id_msg->return_code;

		log_flag(PROTOCOL, "msg_type:DBD_ID_RC return_code:%s JobId=%u db_index=%"PRIu64,
			 slurm_strerror(rc), id_msg->job_id,
			 id_msg->db_index);
		if (!_add_sending_script_env(id_msg, rc_msg))
			slurmdbd_free_id_rc_msg(id_msg);
		if (rc != SLURM_SUCCESS)
			error("DBD_ID_RC is %d", rc);
		break;
	case PERSIST_RC:
		msg = resp.data;
		rc = msg->rc;

		log_flag(PROTOCOL, "msg_type:PERSIST_RC return_code:%s ret_info:%hu flags=%#x comment:%s",
			 slurm_strerror(rc), msg->ret_info,
			 msg->flags, msg->comment);

		if (rc != SLURM_SUCCESS) {
			if (msg->ret_info == DBD_REGISTER_CTLD &&
			    slurm_conf.accounting_storage_enforce) {
				error("PERSIST_RC is %d from "
				      "%s(%u): %s",
				      rc,
				      slurmdbd_msg_type_2_str(
					      msg->ret_info, 1),
				      msg->ret_info,
				      msg->comment);
				fatal("You need to add this cluster "
				      "to accounting if you want to "
				      "enforce associations, or no "
				      "jobs will ever run.");
			} else
				debug("PERSIST_RC is %d from "
				      "%s(%u): %s",
				      rc,
				      slurmdbd_msg_type_2_str(
					      msg->ret_info, 1),
				      msg->ret_info,
				      msg->comment);
		}
		slurm_persist_free_rc_msg(msg);
		break;
	default:
		error("bad message type %s != PERSIST_RC",
		      slurmdbd_msg_type_2_str(msg_type, true));
	}

	return rc;
}

static int _get_return_code(rc_msg_t *rc_msg)
{
	int rc = SLURM_ERROR;
	buf_t *buffer = slurm_persist_recv_msg(slurmdbd_conn);
	if (buffer == NULL)
		return rc;

	rc = _unpack_return_code(slurmdbd_conn->version, buffer, rc_msg);

	FREE_NULL_BUFFER(buffer);
	return rc;
}

static int _get_return_codes(void *x, void *arg)
{
	buf_t *out_buf = x;
	rc_msg_t *rc_msg = arg;
	buf_t *b;

	if ((rc_msg->rc = _unpack_return_code(
		     slurmdbd_conn->version, out_buf, rc_msg)) !=
	    SLURM_SUCCESS)
		return -1;

	if ((b = list_dequeue(agent_list))) {
		FREE_NULL_BUFFER(b);
	} else {
		error("DBD_GOT_MULT_MSG unpack message error");
	}

	return 0;
}

static int _handle_mult_rc_ret(void)
{
	buf_t *buffer;
	uint16_t msg_type;
	persist_rc_msg_t *msg = NULL;
	dbd_list_msg_t *list_msg = NULL;
	int rc = SLURM_ERROR;
	rc_msg_t rc_msg = { 0 };

	buffer = slurm_persist_recv_msg(slurmdbd_conn);
	if (buffer == NULL)
		return rc;

	safe_unpack16(&msg_type, buffer);
	switch (msg_type) {
	case DBD_GOT_MULT_MSG:
		if (slurmdbd_unpack_list_msg(
			    &list_msg, slurmdbd_conn->version,
			    DBD_GOT_MULT_MSG, buffer)
		    != SLURM_SUCCESS) {
			error("unpack message error");
			break;
		}

		slurm_mutex_lock(&agent_lock);
		if (agent_list) {
			list_for_each(list_msg->my_list, _get_return_codes,
				      &rc_msg);
		}
		slurm_mutex_unlock(&agent_lock);
		rc = rc_msg.rc;

		_process_id_rc_list(rc_msg.id_rc_list);

		slurmdbd_free_list_msg(list_msg);
		break;
	case PERSIST_RC:
		if (slurm_persist_unpack_rc_msg(
			    &msg, buffer, slurmdbd_conn->version)
		    == SLURM_SUCCESS) {
			rc = msg->rc;
			if (rc != SLURM_SUCCESS) {
				if (msg->ret_info == DBD_REGISTER_CTLD &&
				    slurm_conf.accounting_storage_enforce) {
					error("PERSIST_RC is %d from "
					      "%s(%u): %s",
					      rc,
					      slurmdbd_msg_type_2_str(
						      msg->ret_info, 1),
					      msg->ret_info,
					      msg->comment);
					fatal("You need to add this cluster "
					      "to accounting if you want to "
					      "enforce associations, or no "
					      "jobs will ever run.");
				} else
					debug("PERSIST_RC is %d from "
					      "%s(%u): %s",
					      rc,
					      slurmdbd_msg_type_2_str(
						      msg->ret_info, 1),
					      msg->ret_info,
					      msg->comment);
			}
			slurm_persist_free_rc_msg(msg);
		} else
			error("unpack message error");
		break;
	default:
		error("bad message type %s != PERSIST_RC",
		      slurmdbd_msg_type_2_str(msg_type, true));
	}

unpack_error:
	FREE_NULL_BUFFER(buffer);
	return rc;
}

/****************************************************************************
 * Functions for agent to manage queue of pending message for the Slurm DBD
 ****************************************************************************/
static buf_t *_load_dbd_rec(int fd)
{
	ssize_t size, rd_size;
	uint32_t msg_size, magic;
	char *msg;
	buf_t *buffer;

	size = sizeof(msg_size);
	rd_size = read(fd, &msg_size, size);
	if (rd_size == 0)
		return NULL;
	if (rd_size != size) {
		error("state recover error: %m");
		return NULL;
	}
	if (msg_size > MAX_BUF_SIZE) {
		error("state recover error, msg_size=%u", msg_size);
		return NULL;
	}

	buffer = init_buf((int) msg_size);
	set_buf_offset(buffer, msg_size);
	msg = get_buf_data(buffer);
	size = msg_size;
	while (size) {
		rd_size = read(fd, msg, size);
		if ((rd_size > 0) && (rd_size <= size)) {
			msg += rd_size;
			size -= rd_size;
		} else if ((rd_size == -1) && (errno == EINTR))
			continue;
		else {
			error("state recover error: %m");
			FREE_NULL_BUFFER(buffer);
			return NULL;
		}
	}

	size = sizeof(magic);
	rd_size = read(fd, &magic, size);
	if ((rd_size != size) || (magic != DBD_MAGIC)) {
		error("state recover error");
		FREE_NULL_BUFFER(buffer);
		return NULL;
	}

	return buffer;
}

static void _load_dbd_state(void)
{
	char *dbd_fname = NULL;
	buf_t *buffer;
	int fd, recovered = 0;
	uint16_t rpc_version = 0;

	xstrfmtcat(dbd_fname, "%s/dbd.messages", slurm_conf.state_save_location);
	fd = open(dbd_fname, O_RDONLY);
	if (fd < 0) {
		/* don't print an error message if there is no file */
		if (errno == ENOENT)
			debug4("There is no state save file to "
			       "open by name %s", dbd_fname);
		else
			error("Opening state save file %s: %m",
			      dbd_fname);
	} else {
		char *ver_str = NULL;

		buffer = _load_dbd_rec(fd);
		if (buffer == NULL)
			goto end_it;
		/* This is set to the end of the buffer for send so we
		   need to set it back to 0 */
		set_buf_offset(buffer, 0);
		safe_unpackstr(&ver_str, buffer);
		debug3("Version string in dbd_state header is %s", ver_str);
	unpack_error:
		FREE_NULL_BUFFER(buffer);
		buffer = NULL;
		if (ver_str) {
			/* get the version after VER */
			rpc_version = slurm_atoul(ver_str + 3);
			xfree(ver_str);
		}

		while (1) {
			/* If the buffer was not the VER%d string it
			   was an actual message so we don't want to
			   skip it.
			*/
			if (!buffer)
				buffer = _load_dbd_rec(fd);
			if (buffer == NULL)
				break;
			if (rpc_version != SLURM_PROTOCOL_VERSION) {
				/* unpack and repack with new
				 * PROTOCOL_VERSION just so we keep
				 * things up to date.
				 */
				persist_msg_t msg = {0};
				int rc;
				set_buf_offset(buffer, 0);
				rc = unpack_slurmdbd_msg(
					&msg, rpc_version, buffer);
				FREE_NULL_BUFFER(buffer);
				if (rc == SLURM_SUCCESS)
					buffer = pack_slurmdbd_msg(
						&msg, SLURM_PROTOCOL_VERSION);
				else
					buffer = NULL;
			}
			if (!buffer) {
				error("no buffer given");
				continue;
			}
			list_enqueue(agent_list, buffer);
			recovered++;
			buffer = NULL;
		}

	end_it:
		verbose("recovered %d pending RPCs", recovered);
		(void) close(fd);
	}
	xfree(dbd_fname);
}

static int _save_dbd_rec(int fd, buf_t *buffer)
{
	ssize_t size, wrote;
	uint32_t msg_size = get_buf_offset(buffer);
	uint32_t magic = DBD_MAGIC;
	char *msg = get_buf_data(buffer);

	size = sizeof(msg_size);
	wrote = write(fd, &msg_size, size);
	if (wrote != size) {
		error("state save error: %m");
		return SLURM_ERROR;
	}

	wrote = 0;
	while (wrote < msg_size) {
		wrote = write(fd, msg, msg_size);
		if (wrote > 0) {
			msg += wrote;
			msg_size -= wrote;
		} else if ((wrote == -1) && (errno == EINTR))
			continue;
		else {
			error("state save error: %m");
			return SLURM_ERROR;
		}
	}

	size = sizeof(magic);
	wrote = write(fd, &magic, size);
	if (wrote != size) {
		error("state save error: %m");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static void _save_dbd_state(void)
{
	char *dbd_fname = NULL;
	buf_t *buffer;
	int fd, rc, wrote = 0;
	uint16_t msg_type;
	uint32_t offset;

	xstrfmtcat(dbd_fname, "%s/dbd.messages", slurm_conf.state_save_location);
	(void) unlink(dbd_fname);	/* clear save state */
	fd = open(dbd_fname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		error("Creating state save file %s", dbd_fname);
	} else if (list_count(agent_list)) {
		char curr_ver_str[10];
		snprintf(curr_ver_str, sizeof(curr_ver_str),
			 "VER%d", SLURM_PROTOCOL_VERSION);
		buffer = init_buf(strlen(curr_ver_str));
		packstr(curr_ver_str, buffer);
		rc = _save_dbd_rec(fd, buffer);
		FREE_NULL_BUFFER(buffer);
		if (rc != SLURM_SUCCESS)
			goto end_it;

		while ((buffer = list_dequeue(agent_list))) {
			/*
			 * We do not want to store registration messages. If an
			 * admin puts in an incorrect cluster name we can get a
			 * deadlock unless they add the bogus cluster name to
			 * the accounting system.
			 */
			offset = get_buf_offset(buffer);
			if (offset < 2) {
				FREE_NULL_BUFFER(buffer);
				continue;
			}
			set_buf_offset(buffer, 0);
			(void) unpack16(&msg_type, buffer);  /* checked by offset */
			set_buf_offset(buffer, offset);
			if (msg_type == DBD_REGISTER_CTLD) {
				FREE_NULL_BUFFER(buffer);
				continue;
			}

			rc = _save_dbd_rec(fd, buffer);
			FREE_NULL_BUFFER(buffer);
			if (rc != SLURM_SUCCESS)
				break;
			wrote++;
		}
	}

end_it:
	if (fd >= 0) {
		verbose("saved %d pending RPCs", wrote);
		rc = fsync_and_close(fd, "dbd.messages");
		if (rc)
			error("error from fsync_and_close");
	}
	xfree(dbd_fname);
}

/*
 * Purge queued records from the agent queue
 */
static int _purge_agent_list_req(void *x, void *arg)
{
	uint16_t msg_type;
	uint32_t offset;
	buf_t *buffer = x;
	uint16_t purge_type = *(uint16_t *)arg;

	offset = get_buf_offset(buffer);
	if (offset < 2)
		return 0;
	set_buf_offset(buffer, 0);
	(void) unpack16(&msg_type, buffer);	/* checked by offset */
	set_buf_offset(buffer, offset);
	switch (purge_type) {
	case DBD_STEP_START:
		if ((msg_type == DBD_STEP_START) ||
		    (msg_type == DBD_STEP_COMPLETE))
			return 1;
		break;
	case DBD_JOB_START:
		if (msg_type == DBD_JOB_START)
			return 1;
		break;
	default:
		error("unknown purge type %d", purge_type);
		break;
	}

	return 0;
}

static void _max_dbd_msg_action(uint32_t *msg_cnt)
{
	int purged = 0;
	if (max_dbd_msg_action == MAX_DBD_ACTION_EXIT) {
		if (*msg_cnt < slurm_conf.max_dbd_msgs)
			return;

		_save_dbd_state();
		fatal("agent queue is full (%u), not continuing until slurmdbd is able to process messages.",
		      *msg_cnt);
	}

	/* MAX_DBD_ACTION_DISCARD */
	if (*msg_cnt >= (slurm_conf.max_dbd_msgs - 1)) {
		uint16_t purge_type = DBD_STEP_START;
		purged = list_delete_all(agent_list, _purge_agent_list_req,
					 &purge_type);
		*msg_cnt -= purged;
		info("purge %d step records", purged);
	}
}

static int _print_agent_list_msg_type(void *x, void *arg)
{
	buf_t *buffer = (buf_t *) x;
	char *mlist = (char *) arg;
	uint16_t msg_type;
	uint32_t offset = get_buf_offset(buffer);

	if (offset < 2)
		return SLURM_ERROR;
	set_buf_offset(buffer, 0);
	(void) unpack16(&msg_type, buffer);	/* checked by offset */
	set_buf_offset(buffer, offset);

	xstrfmtcat(mlist, "%s%s", (mlist[0] ? ", " : ""),
		   slurmdbd_msg_type_2_str(msg_type, 1));

	return SLURM_SUCCESS;
}

/*
 * Prints an info line listing msg types of the dbd agent list
 */
static void _print_agent_list_msg_types(void)
{
	/* pre-allocate a large enough buffer to handle most lists */
	char *mlist = xmalloc(2048);
	int processed, max_msgs = DEBUG_PRINT_MAX_MSG_TYPES;

	if ((processed = list_for_each_max(agent_list, &max_msgs,
					   _print_agent_list_msg_type,
					   mlist, true, true)) < 0) {
		error("unable to create msg type list");
		xfree(mlist);
		return;
	}

	/* append "..." to indicate there are further unprinted messages */
	if (max_msgs)
		xstrcat(mlist, ", ...");

	info("slurmdbd agent_count=%d msg_types_agent_list:%s",
	     (processed + max_msgs), mlist);

	xfree(mlist);
}

static int _get_my_list(void *x, void *arg)
{
	buf_t *buffer = x;
	foreach_get_my_list_t *args = arg;

	args->msg_size += size_buf(buffer);
	if (args->msg_size > MAX_MSG_SIZE)
		return -1;
	list_enqueue(args->my_list, buffer);

	return 0;
}

static void *_agent(void *x)
{
	int rc;
	uint32_t cnt;
	buf_t *buffer;
	struct timespec abs_time;
	static time_t fail_time = 0;
	persist_msg_t list_req = {0};
	dbd_list_msg_t list_msg;
	DEF_TIMERS;

	slurm_mutex_lock(&agent_lock);
	agent_running = true;
	slurm_mutex_unlock(&agent_lock);

	list_req.msg_type = DBD_SEND_MULT_MSG;
	list_req.conn = slurmdbd_conn;
	list_req.data = &list_msg;
	memset(&list_msg, 0, sizeof(dbd_list_msg_t));

	log_flag(DBD_AGENT, "slurmdbd agent_count=%d with msg_type=%s",
		 list_count(agent_list),
		 slurmdbd_msg_type_2_str(list_req.msg_type, 1));

	while (*slurmdbd_conn->shutdown == 0) {
		slurm_mutex_lock(&slurmdbd_lock);
		if (halt_agent) {
			log_flag(DBD_AGENT, "slurmdbd agent halt with agent_count=%d",
				 list_count(agent_list));

			slurm_cond_wait(&slurmdbd_cond, &slurmdbd_lock);
		}

		START_TIMER;
		if ((slurmdbd_conn->fd < 0) &&
		    (difftime(time(NULL), fail_time) >= 10)) {
			/* The connection to Slurm DBD is not open */
			dbd_conn_check_and_reopen(slurmdbd_conn);
			if (slurmdbd_conn->fd < 0) {
				fail_time = time(NULL);

				log_flag(DBD_AGENT, "slurmdbd disconnected with agent_count=%d",
					 list_count(agent_list));
			}
		}

		slurm_mutex_lock(&agent_lock);
		cnt = list_count(agent_list);
		if ((cnt == 0) || (slurmdbd_conn->fd < 0) ||
		    (fail_time && (difftime(time(NULL), fail_time) < 10))) {
			slurm_mutex_unlock(&slurmdbd_lock);
			_max_dbd_msg_action(&cnt);
			END_TIMER2("slurmdbd agent: sleep");
			abs_time.tv_sec  = time(NULL) + 10;
			abs_time.tv_nsec = 0;
			if (*slurmdbd_conn->shutdown != 0) {
				slurm_mutex_unlock(&agent_lock);
				break;
			}
			log_flag(AGENT, "slurmdbd agent sleeping with agent_count=%d",
				 list_count(agent_list));
			slurm_cond_timedwait(&agent_cond, &agent_lock,
					     &abs_time);
			slurm_mutex_unlock(&agent_lock);
			continue;
		} else if (((cnt > 0) && ((cnt % 100) == 0)) ||
		           (slurm_conf.debug_flags & DEBUG_FLAG_DBD_AGENT))
			info("agent_count:%d", cnt);
		/* Leave item on the queue until processing complete */
		if (agent_list) {
			if (cnt > 1) {
				int max_rpcs = 1000;
				foreach_get_my_list_t args = {
					.msg_size = sizeof(list_req),
					.my_list = list_create(NULL),
				};

				list_msg.my_list = args.my_list;

				list_for_each_max(agent_list, &max_rpcs,
						  _get_my_list, &args, 1, true);
				buffer = pack_slurmdbd_msg(
					&list_req, SLURM_PROTOCOL_VERSION);
			} else
				buffer = list_peek(agent_list);
		} else
			buffer = NULL;
		slurm_mutex_unlock(&agent_lock);
		if (buffer == NULL) {
			slurm_mutex_unlock(&slurmdbd_lock);

			slurm_mutex_lock(&assoc_cache_mutex);
			if (slurmdbd_conn->fd >= 0 &&
			    (running_cache != RUNNING_CACHE_STATE_NOTRUNNING))
				slurm_cond_signal(&assoc_cache_cond);
			slurm_mutex_unlock(&assoc_cache_mutex);

			END_TIMER2("slurmdbd agent: empty buffer");
			continue;
		}

		/* NOTE: agent_lock is clear here, so we can add more
		 * requests to the queue while waiting for this RPC to
		 * complete. */
		rc = slurm_persist_send_msg(slurmdbd_conn, buffer);
		if (rc != SLURM_SUCCESS) {
			if (*slurmdbd_conn->shutdown) {
				slurm_mutex_unlock(&slurmdbd_lock);
				END_TIMER2("slurmdbd agent: shutdown");
				break;
			}
			error("Failure sending message: %d: %m", rc);
		} else if (list_msg.my_list) {
			rc = _handle_mult_rc_ret();
		} else {
			rc_msg_t rc_msg = { 0 };

			rc = _get_return_code(&rc_msg);

			_process_id_rc_list(rc_msg.id_rc_list);

			if (rc == EAGAIN) {
				if (*slurmdbd_conn->shutdown) {
					slurm_mutex_unlock(&slurmdbd_lock);
					END_TIMER2("slurmdbd agent: EAGAIN on shutdown");
					break;
				}
				error("Failure with "
				      "message need to resend: %d: %m", rc);
			}
		}
		slurm_mutex_unlock(&slurmdbd_lock);
		slurm_mutex_lock(&assoc_cache_mutex);
		if (slurmdbd_conn->fd >= 0 &&
		    (running_cache != RUNNING_CACHE_STATE_NOTRUNNING))
			slurm_cond_signal(&assoc_cache_cond);
		slurm_mutex_unlock(&assoc_cache_mutex);

		slurm_mutex_lock(&agent_lock);
		if (agent_list && (rc == SLURM_SUCCESS)) {
			/*
			 * If we sent a mult_msg we just need to free buffer,
			 * we don't need to requeue, just mark list_msg.my_list
			 * as NULL as that is the sign we sent a mult_msg.
			 */
			if (list_msg.my_list) {
				if (list_msg.my_list != agent_list)
					FREE_NULL_LIST(list_msg.my_list);
				list_msg.my_list = NULL;
			} else
				buffer = list_dequeue(agent_list);

			FREE_NULL_BUFFER(buffer);
			fail_time = 0;
		} else {
			/* We need to free a mult_msg even on failure */
			if (list_msg.my_list) {
				if (list_msg.my_list != agent_list)
					FREE_NULL_LIST(list_msg.my_list);
				list_msg.my_list = NULL;
				FREE_NULL_BUFFER(buffer);
			}

			fail_time = time(NULL);

			if (slurm_conf.debug_flags & DEBUG_FLAG_DBD_AGENT) {
				info("slurmdbd agent failed with rc:%d",
				     rc);
				_print_agent_list_msg_types();
			}
		}
		slurm_mutex_unlock(&agent_lock);
		END_TIMER2("slurmdbd agent: full loop");
	}

	slurm_mutex_lock(&agent_lock);
	_save_dbd_state();

	log_flag(AGENT, "slurmdbd agent ending with agent_count=%d",
		 list_count(agent_list));

	FREE_NULL_LIST(agent_list);
	agent_running = false;
	slurm_cond_signal(&shutdown_cond);
	slurm_mutex_unlock(&agent_lock);
	return NULL;
}

static void _create_agent(void)
{
	xassert(running_in_slurmctld());

	/* this needs to be set because the agent thread will do
	   nothing if the connection was closed and then opened again */
	slurmdbd_shutdown = 0;

	if (agent_list == NULL) {
		agent_list = list_create(slurmdbd_free_buffer);
		_load_dbd_state();
	}

	if (agent_tid == 0) {
		slurm_thread_create(&agent_tid, _agent, NULL);
	}
}

static void _shutdown_agent(void)
{
	if (!agent_tid)
		return;

	slurmdbd_shutdown = time(NULL);
	slurm_mutex_lock(&agent_lock);
	if (agent_running)
		slurm_cond_broadcast(&agent_cond);
	slurm_mutex_unlock(&agent_lock);
	slurm_thread_join(agent_tid);
}

/****************************************************************************
 * Socket open/close/read/write functions
 ****************************************************************************/

extern void slurmdbd_agent_set_conn(persist_conn_t *pc)
{
	if (!running_in_slurmctld())
		return;

	slurm_mutex_lock(&slurmdbd_lock);
	slurmdbd_conn = pc;

	slurmdbd_shutdown = 0;
	slurmdbd_conn->shutdown = &slurmdbd_shutdown;

	slurm_mutex_unlock(&slurmdbd_lock);

	slurm_mutex_lock(&agent_lock);

	if ((agent_tid == 0) || (agent_list == NULL))
		_create_agent();
	else if (agent_list)
		_load_dbd_state();

	slurm_mutex_unlock(&agent_lock);
}

extern void slurmdbd_agent_rem_conn(void)
{
	if (!running_in_slurmctld())
		return;

	_shutdown_agent();

	slurm_mutex_lock(&slurmdbd_lock);
	slurmdbd_conn = NULL;
	slurm_mutex_unlock(&slurmdbd_lock);
}


extern int slurmdbd_agent_send_recv(uint16_t rpc_version,
				    persist_msg_t *req,
				    persist_msg_t *resp)
{
	int rc = SLURM_SUCCESS;

	xassert(req);
	xassert(resp);

	/*
	 * To make sure we can get this to send instead of the agent
	 * sending stuff that can happen anytime we set halt_agent and
	 * then after we get into the mutex we unset.
	 */
	halt_agent = 1;
	slurm_mutex_lock(&slurmdbd_lock);

	halt_agent = 0;

	if (!slurmdbd_conn) {
		slurm_cond_signal(&slurmdbd_cond);
		slurm_mutex_unlock(&slurmdbd_lock);
		return ESLURM_DB_CONNECTION_INVALID;
	}

	if (req->conn && (req->conn != slurmdbd_conn))
		error("We are overriding the connection!!!!!");

	req->conn = slurmdbd_conn;

	rc = dbd_conn_send_recv_direct(rpc_version, req, resp);

	slurm_cond_signal(&slurmdbd_cond);
	slurm_mutex_unlock(&slurmdbd_lock);

	return rc;
}

/* Send an RPC to the SlurmDBD. Do not wait for the reply. The RPC
 * will be queued and processed later if the SlurmDBD is not responding.
 *
 * Returns SLURM_SUCCESS or an error code */
extern int slurmdbd_agent_send(uint16_t rpc_version, persist_msg_t *req)
{
	buf_t *buffer;
	uint32_t cnt, rc = SLURM_SUCCESS;
	static time_t syslog_time = 0;

	xassert(running_in_slurmctld());
	xassert(slurm_conf.max_dbd_msgs);

	log_flag(PROTOCOL, "msg_type:%s protocol_version:%hu agent_count:%d",
		 slurmdbd_msg_type_2_str(req->msg_type, 1),
		 rpc_version, list_count(agent_list));

	buffer = slurm_persist_msg_pack(
		slurmdbd_conn, (persist_msg_t *)req);
	if (!buffer)	/* pack error */
		return SLURM_ERROR;

	slurm_mutex_lock(&agent_lock);
	if ((agent_tid == 0) || (agent_list == NULL)) {
		_create_agent();
		if ((agent_tid == 0) || (agent_list == NULL)) {
			slurm_mutex_unlock(&agent_lock);
			FREE_NULL_BUFFER(buffer);
			return SLURM_ERROR;
		}
	}
	cnt = list_count(agent_list);
	if ((cnt >= (slurm_conf.max_dbd_msgs / 2)) &&
	    (difftime(time(NULL), syslog_time) > 120)) {
		/* Record critical error every 120 seconds */
		syslog_time = time(NULL);
		error("agent queue filling (%u), MaxDBDMsgs=%u, RESTART SLURMDBD NOW",
		      cnt, slurm_conf.max_dbd_msgs);
		syslog(LOG_CRIT, "*** RESTART SLURMDBD NOW ***");
		(slurmdbd_conn->trigger_callbacks.dbd_fail)();
	}

	/* Handle action */
	_max_dbd_msg_action(&cnt);

	if (cnt < slurm_conf.max_dbd_msgs) {
		list_enqueue(agent_list, buffer);
	} else {
		error("agent queue is full (%u), discarding %s:%u request",
		      cnt,
		      slurmdbd_msg_type_2_str(req->msg_type, 1),
		      req->msg_type);
		(slurmdbd_conn->trigger_callbacks.acct_full)();
		FREE_NULL_BUFFER(buffer);
		rc = SLURM_ERROR;
	}

	slurm_cond_broadcast(&agent_cond);
	slurm_mutex_unlock(&agent_lock);
	return rc;
}

/* Return true if connection to slurmdbd is active, false otherwise. */
extern bool slurmdbd_conn_active(void)
{
	if (!slurmdbd_conn || (slurmdbd_conn->fd < 0))
		return false;

	return true;
}

extern int slurmdbd_agent_queue_count(void)
{
	return list_count(agent_list);
}

extern void slurmdbd_agent_config_setup(void)
{
	char *tmp_ptr;
	/*
	 * Whatever our max job count is multiplied by 2 plus node count
	 * multiplied by 4 or DEFAULT_MAX_DBD_MSGS which ever is bigger.
	 */
	if (!slurm_conf.max_dbd_msgs)
		slurm_conf.max_dbd_msgs =
			MAX(DEFAULT_MAX_DBD_MSGS,
			    ((slurm_conf.max_job_cnt * 2) +
			     (node_record_count * 4)));

	/*                          0123456789012345678 */
	if ((tmp_ptr = xstrcasestr(slurm_conf.slurmctld_params,
	                           "max_dbd_msg_action="))) {
		char *type = xstrdup(tmp_ptr + 19);
		tmp_ptr = strchr(type, ',');
		if (tmp_ptr)
			tmp_ptr[0] = '\0';
		if (!xstrcasecmp(type, "discard"))
			max_dbd_msg_action = MAX_DBD_ACTION_DISCARD;
		else if (!xstrcasecmp(type, "exit"))
			max_dbd_msg_action = MAX_DBD_ACTION_EXIT;
		else
			fatal("Unknown SlurmctldParameters option for max_dbd_msg_action '%s'",
			      type);
		xfree(type);
	} else
		max_dbd_msg_action = MAX_DBD_DEFAULT_ACTION;
}
