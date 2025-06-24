/*****************************************************************************\
 **  pmix_agent.c - PMIx agent thread
 *****************************************************************************
 *  Copyright (C) 2014-2015 Artem Polyakov. All rights reserved.
 *  Copyright (C) 2015-2020 Mellanox Technologies. All rights reserved.
 *  Written by Artem Y. Polyakov <artpol84@gmail.com, artemp@mellanox.com>,
 *             Boris Karasev <karasev.b@gmail.com, boriska@mellanox.com>.
 *  Copyright (C) 2020      Siberian State University of Telecommunications
 *                          and Information Sciences (SibSUTIS).
 *                          All rights reserved.
 *  Written by Boris Bochkarev <boris-bochkaryov@yandex.ru>.
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

#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <arpa/inet.h>
#include <time.h>

#include "pmixp_common.h"
#include "pmixp_server.h"
#include "pmixp_client.h"
#include "pmixp_state.h"
#include "pmixp_debug.h"
#include "pmixp_nspaces.h"
#include "pmixp_utils.h"
#include "pmixp_dconn.h"

static pthread_mutex_t agent_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t agent_running_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t abort_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t abort_mutex_cond = PTHREAD_COND_INITIALIZER;

static eio_handle_t *_io_handle = NULL;
static eio_handle_t *_abort_handle = NULL;

static int _abort_agent_start_count = 0;

static pthread_t _agent_tid = 0;
static pthread_t _timer_tid = 0;
static pthread_t _abort_tid = 0;

struct timer_data_t {
	int work_in, work_out;
	int stop_in, stop_out;
};
static struct timer_data_t timer_data;

static bool _conn_readable(eio_obj_t *obj);
static int _server_conn_read(eio_obj_t *obj, list_t *objs);
static int _abort_conn_close(eio_obj_t *obj, list_t *objs);
static int _timer_conn_read(eio_obj_t *obj, list_t *objs);
static int _abort_conn_read(eio_obj_t *obj, list_t *objs);

static struct io_operations abort_ops = {
	.readable = &_conn_readable,
	.handle_read = &_abort_conn_read,
	.handle_close = &_abort_conn_close,
};

static struct io_operations srv_ops = {
	.readable = &_conn_readable,
	.handle_read = &_server_conn_read
};

static struct io_operations to_ops = {
	.readable = &_conn_readable,
	.handle_read = &_timer_conn_read
};

static bool _conn_readable(eio_obj_t *obj)
{
	if (obj->shutdown == true) {
		if (obj->fd != -1) {
			close(obj->fd);
			obj->fd = -1;
		}
		PMIXP_DEBUG("    false, shutdown");
		return false;
	}
	return true;
}

static int _server_conn_read(eio_obj_t *obj, list_t *objs)
{
	int fd;
	struct sockaddr addr;
	socklen_t size = sizeof(addr);
	int shutdown = 0;

	while (1) {
		/* Return early if fd is not now ready */
		if (!pmixp_fd_read_ready(obj->fd, &shutdown)) {
			if (shutdown) {
				obj->shutdown = true;
				if (shutdown < 0) {
					PMIXP_ERROR_NO(shutdown,
						       "sd=%d failure",
						       obj->fd);
				}
			}
			return 0;
		}

		while ((fd = accept4(obj->fd, &addr, &size,
				     (SOCK_CLOEXEC | SOCK_NONBLOCK))) < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) /* No more connections */
				return 0;
			if ((errno == ECONNABORTED) || (errno == EWOULDBLOCK)) {
				return 0;
			}
			PMIXP_ERROR_STD("accept()ing connection sd=%d",
					obj->fd);
			return 0;
		}

		if (pmixp_info_srv_usock_fd() == obj->fd) {
			PMIXP_DEBUG("Slurm PROTO: accepted connection: sd=%d",
				    fd);
			/* read command from socket and handle it */
			pmixp_server_slurm_conn(fd);
		} else if (pmixp_dconn_poll_fd() == obj->fd) {
			PMIXP_DEBUG("DIRECT PROTO: accepted connection: sd=%d",
				    fd);
			/* read command from socket and handle it */
			pmixp_server_direct_conn(fd);
		} else {
			/* Unexpected trigger */
			close(fd);
		}
	}
	return 0;
}

static int _abort_conn_close(eio_obj_t *obj, list_t *objs)
{
	close(obj->fd);
	return SLURM_SUCCESS;
}

static int _abort_conn_read(eio_obj_t *obj, list_t *objs)
{
	slurm_addr_t abort_client;
	int client_fd;
	int shutdown = 0;

	while (1) {
		if (!pmixp_fd_read_ready(obj->fd, &shutdown)) {
			if (shutdown) {
				obj->shutdown = true;
				if (shutdown < 0) {
					PMIXP_ERROR_NO(shutdown,
						       "sd=%d failure",
						       obj->fd);
				}
			}
			return SLURM_SUCCESS;
		}

		client_fd = slurm_accept_msg_conn(obj->fd, &abort_client);
		if (client_fd < 0) {
			PMIXP_ERROR("slurm_accept_msg_conn: %m");
			continue;
		}
		PMIXP_DEBUG("New abort client: %pA", &abort_client);
		pmixp_abort_handle(client_fd);
		close(client_fd);
	}
	return SLURM_SUCCESS;
}

static int _timer_conn_read(eio_obj_t *obj, list_t *objs)
{
	char *tmpbuf[32];
	int shutdown;

	/* drain everything from in fd */
	while (32 == pmixp_read_buf(obj->fd, tmpbuf, 32, &shutdown, false))
		;
	if (shutdown) {
		PMIXP_ERROR("readin from timer fd, shouldn't happen");
		obj->shutdown = true;
	}

	/* check direct modex requests */
	pmixp_dmdx_timeout_cleanup();

	/* check collective statuses */
	pmixp_state_coll_cleanup();

	/* cleanup server structures */
	pmixp_server_cleanup();

	return 0;
}

static void _shutdown_timeout_fds(void);

#define SETUP_FDS(fds) { \
	fd_set_nonblocking(fds[0]);	\
	fd_set_nonblocking(fds[1]);	\
	}

static int _setup_timeout_fds(void)
{
	int fds[2];

	timer_data.work_in = timer_data.work_out = -1;
	timer_data.stop_in = timer_data.stop_out = -1;

	if (pipe2(fds, O_CLOEXEC)) {
		return SLURM_ERROR;
	}
	SETUP_FDS(fds);
	timer_data.work_in = fds[0];
	timer_data.work_out = fds[1];

	if (pipe2(fds, O_CLOEXEC)) {
		_shutdown_timeout_fds();
		return SLURM_ERROR;
	}
	SETUP_FDS(fds);
	timer_data.stop_in = fds[0];
	timer_data.stop_out = fds[1];

	return SLURM_SUCCESS;
}

static void _shutdown_timeout_fds(void)
{
	if (0 <= timer_data.work_in) {
		close(timer_data.work_in);
		timer_data.work_in = -1;
	}
	if (0 <= timer_data.work_out) {
		close(timer_data.work_out);
		timer_data.work_out = -1;
	}
	if (0 <= timer_data.stop_in) {
		close(timer_data.stop_in);
		timer_data.stop_in = -1;
	}
	if (0 <= timer_data.stop_out) {
		close(timer_data.stop_out);
		timer_data.stop_out = -1;
	}
}

/*
 * main loop of agent thread
 */
static void *_agent_thread(void *unused)
{
	PMIXP_DEBUG("Start agent thread");
	eio_obj_t *obj;

	_io_handle = eio_handle_create(0);

	obj = eio_obj_create(pmixp_info_srv_usock_fd(), &srv_ops,
			     (void *)(-1));
	eio_new_initial_obj(_io_handle, obj);

	obj = eio_obj_create(timer_data.work_in, &to_ops, (void *)(-1));
	eio_new_initial_obj(_io_handle, obj);

	pmixp_info_io_set(_io_handle);

	if (PMIXP_DCONN_PROGRESS_SW == pmixp_dconn_progress_type()) {
		obj = eio_obj_create(pmixp_dconn_poll_fd(), &srv_ops,
				     (void *)(-1));
		eio_new_initial_obj(_io_handle, obj);
	} else {
		pmixp_dconn_regio(_io_handle);
	}

	slurm_mutex_lock(&agent_mutex);
	slurm_cond_signal(&agent_running_cond);
	slurm_mutex_unlock(&agent_mutex);

	eio_handle_mainloop(_io_handle);

	PMIXP_DEBUG("agent thread exit");

	return NULL;
}

static void *_pmix_timer_thread(void *unused)
{
	struct pollfd pfds[1];

	PMIXP_DEBUG("Start timer thread");

	pfds[0].fd = timer_data.stop_in;
	pfds[0].events = POLLIN;

	/* our job is to sleep 1 sec and then trigger
	 * the timer event in the main loop */

	while (1) {
		/* during normal operation there should be no
		 * activity on the stop fd.
		 * So normally we need to exit by the timeout.
		 * This forses periodic timer events (once each second) */
		int ret = poll(pfds, 1, 1000);
		char c = 1;
		if (0 < ret) {
			/* there was an event on stop_fd, exit */
			break;
		}
		/* activate main thread's timer event */
		safe_write(timer_data.work_out, &c, 1);
	}

rwfail:
	return NULL;
}

/*
 * thread for codes from abort
 */
static void *_pmix_abort_thread(void *args)
{
	PMIXP_DEBUG("Start abort thread");

	slurm_mutex_lock(&abort_mutex);
	slurm_cond_signal(&abort_mutex_cond);
	slurm_mutex_unlock(&abort_mutex);

	eio_handle_mainloop(_abort_handle);
	PMIXP_DEBUG("Abort thread exit");
	return NULL;
}

/* Must be called inside locks */
static void _abort_agent_cleanup(void)
{
	if (_abort_tid) {
		eio_signal_shutdown(_abort_handle);
		slurm_thread_join(_abort_tid);
	}
	/* Close FDs and free structure */
	if (_abort_handle) {
		eio_handle_destroy(_abort_handle);
		_abort_handle = NULL;
	}
}

int pmixp_abort_agent_start(char ***env)
{
	int abort_server_socket = -1, rc = SLURM_SUCCESS;
	slurm_addr_t abort_server;
	eio_obj_t *obj;
	uint16_t *ports;

	slurm_mutex_lock(&abort_mutex);
	/*
	 * Only the 1st thread to reach pmixp_abort_agent_start will try to
	 * spawn the agent.
	 * If it fails, we keep increasing the counter to prevent from
	 * subsequent threads to initialize again.
	 */
	_abort_agent_start_count++;
	if (_abort_agent_start_count != 1)
		goto done;

	if ((ports = slurm_get_srun_port_range()))
		abort_server_socket = slurm_init_msg_engine_ports(ports);
	else
		abort_server_socket = slurm_init_msg_engine_port(0);
	if (abort_server_socket < 0) {
		PMIXP_ERROR("slurm_init_msg_engine_port() failed: %m");
		rc = SLURM_ERROR;
		goto done;
	}

	memset(&abort_server, 0, sizeof(slurm_addr_t));

	if (slurm_get_stream_addr(abort_server_socket, &abort_server)) {
		PMIXP_ERROR("slurm_get_stream_addr() failed: %m");
		close(abort_server_socket);
		rc = SLURM_ERROR;
		goto done;
	}
	PMIXP_DEBUG("Abort agent port: %d", slurm_get_port(&abort_server));
	setenvf(env, PMIXP_SLURM_ABORT_AGENT_PORT, "%d",
		slurm_get_port(&abort_server));

	_abort_handle = eio_handle_create(0);
	obj = eio_obj_create(abort_server_socket, &abort_ops, (void *)(-1));
	eio_new_initial_obj(_abort_handle, obj);
	slurm_thread_create(&_abort_tid, _pmix_abort_thread, NULL);

	/* wait for the abort EIO thread to initialize */
	slurm_cond_wait(&abort_mutex_cond, &abort_mutex);

done:
	/* If start failed, 1st thread will clean here */
	if ((_abort_agent_start_count == 1) && (rc != SLURM_SUCCESS))
		_abort_agent_cleanup();
	slurm_mutex_unlock(&abort_mutex);
	return rc;
}

int pmixp_abort_agent_stop(void)
{
	int rc;

	slurm_mutex_lock(&abort_mutex);

	_abort_agent_start_count--;

	/* Wait for abort code to be ready. */
	if (_abort_agent_start_count) {
		slurm_cond_wait(&abort_mutex_cond, &abort_mutex);
	} else {
		_abort_agent_cleanup();
		/*
		 * Signal the other threads to let them know rc has now a valid
		 * value.
		 */
		slurm_cond_broadcast(&abort_mutex_cond);
	}
	rc = pmixp_abort_code_get();
	slurm_mutex_unlock(&abort_mutex);

	return rc;
}

/* Must be called inside locks */
static int _agent_cleanup(void)
{
	int rc = SLURM_SUCCESS;
	char c = 1;

	if (_agent_tid) {
		eio_signal_shutdown(_io_handle);
		/* wait for the agent thread to stop */
		slurm_thread_join(_agent_tid);
	}
	/* Close FDs and free structure */
	if(_io_handle) {
		eio_handle_destroy(_io_handle);
		_io_handle = NULL;
	}

	if (_timer_tid) {
		/* cancel timer */
		if (write(timer_data.stop_out, &c, 1) == -1)
			rc = SLURM_ERROR;
		slurm_thread_join(_timer_tid);

		/* close timer fds */
		_shutdown_timeout_fds();
	}

	return rc;
}

int pmixp_agent_start(void)
{
	int rc = SLURM_SUCCESS;

	slurm_mutex_lock(&agent_mutex);

	_setup_timeout_fds();

	/* start agent thread */
	slurm_thread_create(&_agent_tid, _agent_thread, NULL);

	/* wait for the agent thread to initialize */
	slurm_cond_wait(&agent_running_cond, &agent_mutex);

	/* Establish the early direct connection */
	if (pmixp_info_srv_direct_conn_early()) {
		if (pmixp_server_direct_conn_early()) {
			rc = SLURM_ERROR;
			goto done;
		}
	}
	/* Check if a ping-pong run was requested by user
	 * NOTE: enabled only if `--enable-debug` configuration
	 * option was passed
	 */
	if (pmixp_server_want_pp()) {
		pmixp_server_run_pp();
	}

	/* Check if a collective test was requested by user
	 * NOTE: enabled only if `--enable-debug` configuration
	 * option was passed
	 */
	if (pmixp_server_want_cperf()) {
		pmixp_server_run_cperf();
	}

	PMIXP_DEBUG("agent thread started: tid = %lu",
		    (unsigned long) _agent_tid);

	slurm_thread_create(&_timer_tid, _pmix_timer_thread, NULL);

	PMIXP_DEBUG("timer thread started: tid = %lu",
		    (unsigned long) _timer_tid);

done:
	if (rc != SLURM_SUCCESS)
		_agent_cleanup();
	slurm_mutex_unlock(&agent_mutex);
	return rc;
}

int pmixp_agent_stop(void)
{
	int rc;

	rc = _agent_cleanup();
	return rc;
}
