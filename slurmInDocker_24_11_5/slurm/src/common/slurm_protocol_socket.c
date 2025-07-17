/*****************************************************************************\
 *  slurm_protocol_socket.c - slurm socket handling functions
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
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

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"
#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/net.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_protocol_socket.h"
#include "src/common/strlcpy.h"
#include "src/common/util-net.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"

#define PORT_RETRIES    3
#define MIN_USER_PORT   (IPPORT_RESERVED + 1)
#define MAX_USER_PORT   0xffff
#define RANDOM_USER_PORT ((uint16_t) ((lrand48() % \
		(MAX_USER_PORT - MIN_USER_PORT + 1)) + MIN_USER_PORT))

/* Static functions */
static int _slurm_connect(int __fd, struct sockaddr const * __addr,
			  socklen_t __len);

/****************************************************************
 * MIDDLE LAYER MSG FUNCTIONS
 ****************************************************************/

/*
 * Return time in msec since "start time"
 */
static int _tot_wait (struct timeval *start_time)
{
	struct timeval end_time;
	int msec_delay;

	gettimeofday(&end_time, NULL);
	msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
	msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
	return msec_delay;
}

/*
 * Pick a random port number to use. Use this if the system
 * selected port can't connect. This may indicate that the
 * port/address of both the client and server match a defunct
 * socket record in TIME_WAIT state.
 */
static void _sock_bind_wild(int sockfd)
{
	int rc, retry;
	slurm_addr_t sin;
	static bool seeded = false;

	if (!seeded) {
		seeded = true;
		srand48((long int) (time(NULL) + getpid()));
	}

	slurm_setup_addr(&sin, RANDOM_USER_PORT);

	for (retry=0; retry < PORT_RETRIES ; retry++) {
		rc = bind(sockfd, (struct sockaddr *) &sin, sizeof(sin));
		if (rc >= 0)
			break;
		slurm_set_port(&sin, RANDOM_USER_PORT);
	}
	return;
}

extern ssize_t slurm_msg_recvfrom_timeout(int fd, char **pbuf, size_t *lenp,
					  int timeout)
{
	ssize_t  len;
	uint32_t msglen;

	len = slurm_recv_timeout(fd, (char *) &msglen, sizeof(msglen), timeout);

	if (len < ((ssize_t) sizeof(msglen)))
		return SLURM_ERROR;

	msglen = ntohl(msglen);

	if (msglen > MAX_MSG_SIZE)
		slurm_seterrno_ret(SLURM_PROTOCOL_INSANE_MSG_LENGTH);

	/*
	 *  Allocate memory on heap for message
	 */
	if (!(*pbuf = try_xmalloc(msglen)))
		slurm_seterrno_ret(ENOMEM);

	if (slurm_recv_timeout(fd, *pbuf, msglen, timeout) != msglen) {
		xfree(*pbuf);
		*pbuf = NULL;
		return SLURM_ERROR;
	}

	*lenp = msglen;

	return (ssize_t) msglen;
}

static int _writev_timeout(int fd, struct iovec *iov, int iovcnt, int timeout)
{
	int tot_bytes_sent = 0;
	size_t size = 0;
	int fd_flags;
	struct pollfd ufds;
	struct timeval tstart;
	char temp[2];

	ufds.fd     = fd;
	ufds.events = POLLOUT;

	fd_flags = fcntl(fd, F_GETFL);
	fd_set_nonblocking(fd);

	gettimeofday(&tstart, NULL);

	for (int i = 0; i < iovcnt; i++)
		size += iov[i].iov_len;

	while (true) {
		ssize_t bytes_sent = 0;
		int rc;
		int timeleft = timeout - _tot_wait(&tstart);

		if (timeleft <= 0) {
			debug("%s at %d of %zu, timeout",
			      __func__, tot_bytes_sent, size);
			errno = SLURM_PROTOCOL_SOCKET_IMPL_TIMEOUT;
			tot_bytes_sent = SLURM_ERROR;
			break;
		}

		if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
			if ((rc == 0) || (errno == EINTR) || (errno == EAGAIN))
 				continue;
			else {
				debug("%s at %d of %zu, poll error: %s",
				      __func__, tot_bytes_sent, size,
				      strerror(errno));
				errno = SLURM_COMMUNICATIONS_SEND_ERROR;
				tot_bytes_sent = SLURM_ERROR;
				break;
			}
		}

		/*
		 * Check here to make sure the socket really is there.
		 * If not then exit out and notify the sender.  This
 		 * is here since a write doesn't always tell you the
		 * socket is gone, but getting 0 back from a
		 * nonblocking read means just that.
		 */
		if (ufds.revents & POLLERR) {
			int e;

			if ((rc = fd_get_socket_error(fd, &e)))
				debug("%s: Socket POLLERR, fd_get_socket_error failed: %s",
				      __func__, slurm_strerror(rc));
			else
				debug("%s: Socket POLLERR: %s",
				      __func__, slurm_strerror(e));

			errno = e;
			tot_bytes_sent = SLURM_ERROR;
			break;
		}
		if ((ufds.revents & POLLHUP) || (ufds.revents & POLLNVAL) ||
		    (recv(fd, &temp, 1, 0) == 0)) {
			int so_err;
			if ((rc = fd_get_socket_error(fd, &so_err)))
				debug2("%s: Socket no longer there, fd_get_socket_error failed: %s",
				       __func__, slurm_strerror(rc));
			else
				debug2("%s: Socket no longer there: %s",
				       __func__, slurm_strerror(so_err));
			errno = so_err;
			tot_bytes_sent = SLURM_ERROR;
			break;
		}
		if ((ufds.revents & POLLOUT) != POLLOUT) {
			error("%s: Poll failure, revents:%d",
			      __func__, ufds.revents);
		}

		bytes_sent = writev(fd, iov, iovcnt);

		if (bytes_sent < 0) {
 			if (errno == EINTR)
				continue;

			log_flag(NET, "%s: [fd:%d] writev() sent %zd/%zu bytes failed: %m",
				 __func__, fd, bytes_sent, size);

			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				/* poll() lied to us */
				usleep(10000);
				continue;
			}
			errno = SLURM_COMMUNICATIONS_SEND_ERROR;
			tot_bytes_sent = SLURM_ERROR;
			break;
		}
		if (bytes_sent == 0) {
			/*
			 * If driver false reports POLLIN but then does not
			 * provide any output: try poll() again.
			 */
			log_flag(NET, "%s: [fd:%d] writev() sent zero bytes out of %d/%zu",
				 __func__, fd, tot_bytes_sent, size);
			continue;
		}

		tot_bytes_sent += bytes_sent;

		if (tot_bytes_sent >= size) {
			log_flag(NET, "%s: [fd:%d] writev() completed sending %d/%zu bytes",
				 __func__, fd, tot_bytes_sent, size);
			break;
		}

		/* partial write, need to adjust iovec before next call */
		for (int i = 0; i < iovcnt; i++) {
			if (bytes_sent < iov[i].iov_len) {
				iov[i].iov_base += bytes_sent;
				iov[i].iov_len -= bytes_sent;
				break;
			}

			bytes_sent -= iov[i].iov_len;
			iov[i].iov_base = NULL;
			iov[i].iov_len = 0;
		}
	}

	/* Reset fd flags to prior state, preserve errno */
	if (fd_flags != -1) {
		int slurm_err = errno;
		if (fcntl(fd, F_SETFL, fd_flags) < 0)
			error("%s: fcntl(F_SETFL) error: %m", __func__);
		errno = slurm_err;
	}

	return tot_bytes_sent;
}

/*
 * Send slurm message with timeout
 * RET message size (as specified in argument) or SLURM_ERROR on error
 */
extern int slurm_send_timeout(int fd, char *buf, size_t size, int timeout)
{
	struct iovec iov = { .iov_base = buf, .iov_len = size };
	return _writev_timeout(fd, &iov, 1, timeout);
}

extern ssize_t slurm_msg_sendto(int fd, char *buffer, size_t size)
{
	struct iovec iov[2];
	uint32_t usize;
	int len;
	SigFunc *ohandler;
	int timeout = slurm_conf.msg_timeout * 1000;

	/*
	 *  Ignore SIGPIPE so that send can return a error code if the
	 *    other side closes the socket
	 */
	ohandler = xsignal(SIGPIPE, SIG_IGN);

	iov[0].iov_base = &usize;
	iov[0].iov_len = sizeof(usize);
	iov[1].iov_base = buffer;
	iov[1].iov_len = size;

	usize = htonl(iov[1].iov_len);

	len = _writev_timeout(fd, iov, 2, timeout);

	xsignal(SIGPIPE, ohandler);

	/* Returned size should not include the 4-byte length header. */
	if (len < 0)
		return SLURM_ERROR;
	return size;
}

extern ssize_t slurm_bufs_sendto(int fd, msg_bufs_t *buffers)
{
	struct iovec iov[4];
	int len;
	uint32_t usize;
	SigFunc *ohandler;
	int timeout = slurm_conf.msg_timeout * 1000;

	xassert(buffers);

	/*
	 * Ignore SIGPIPE so that send can return a error code if the other
	 * side closes the socket
	 */
	ohandler = xsignal(SIGPIPE, SIG_IGN);

	/* auth portion is optional. header and body are mandatory. */
	iov[0].iov_base = &usize;
	iov[0].iov_len = sizeof(usize);
	iov[1].iov_base = get_buf_data(buffers->header);
	iov[1].iov_len = get_buf_offset(buffers->header);
	iov[2].iov_base = buffers->auth ? get_buf_data(buffers->auth) : NULL;
	iov[2].iov_len = buffers->auth ? get_buf_offset(buffers->auth) : 0;
	iov[3].iov_base = get_buf_data(buffers->body);
	iov[3].iov_len = get_buf_offset(buffers->body);

	usize = htonl(iov[1].iov_len + iov[2].iov_len + iov[3].iov_len);

	len = _writev_timeout(fd, iov, 4, timeout);

	xsignal(SIGPIPE, ohandler);
	return len;
}

/* Get slurm message with timeout
 * RET message size (as specified in argument) or SLURM_ERROR on error */
extern int slurm_recv_timeout(int fd, char *buffer, size_t size, int timeout)
{
	int rc;
	int recvlen = 0;
	int fd_flags;
	struct pollfd  ufds;
	struct timeval tstart;
	int timeleft = timeout;

	ufds.fd     = fd;
	ufds.events = POLLIN;

	fd_flags = fcntl(fd, F_GETFL);
	fd_set_nonblocking(fd);

	gettimeofday(&tstart, NULL);

	while (recvlen < size) {
		timeleft = timeout - _tot_wait(&tstart);
		if (timeleft <= 0) {
			debug("%s at %d of %zu, timeout", __func__, recvlen,
			      size);
			errno = SLURM_PROTOCOL_SOCKET_IMPL_TIMEOUT;
			recvlen = SLURM_ERROR;
			goto done;
		}

		if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
			if ((errno == EINTR) || (errno == EAGAIN) || (rc == 0))
				continue;
			else {
				debug("%s at %d of %zu, poll error: %m",
				      __func__, recvlen, size);
				errno = SLURM_COMMUNICATIONS_RECEIVE_ERROR;
 				recvlen = SLURM_ERROR;
  				goto done;
			}
		}

		if (ufds.revents & POLLERR) {
			int e, rc;

			if ((rc = fd_get_socket_error(fd, &e)))
				debug("%s: Socket POLLERR: fd_get_socket_error failed: %s",
				      __func__, slurm_strerror(rc));
			else
				debug("%s: Socket POLLERR: %s",
				      __func__, slurm_strerror(e));

			errno = e;
			recvlen = SLURM_ERROR;
			goto done;
		}
		if ((ufds.revents & POLLNVAL) ||
		    ((ufds.revents & POLLHUP) &&
		     ((ufds.revents & POLLIN) == 0))) {
			int so_err, rc;
			if ((rc = fd_get_socket_error(fd, &so_err))) {
				debug2("%s: Socket no longer there: fd_get_socket_error failed: %s",
				       __func__, slurm_strerror(rc));
				errno = rc;
			} else {
				debug2("%s: Socket no longer there: %s",
				       __func__, slurm_strerror(so_err));
				errno = so_err;
			}
			recvlen = SLURM_ERROR;
			goto done;
		}
		if ((ufds.revents & POLLIN) != POLLIN) {
			error("%s: Poll failure, revents:%d",
			      __func__, ufds.revents);
			continue;
		}

		rc = recv(fd, &buffer[recvlen], (size - recvlen), 0);
		if (rc < 0)  {
			if ((errno == EINTR) || (errno == EAGAIN)) {
				log_flag(NET, "%s: recv(fd:%d) got %m. retrying.",
					 __func__, fd);
				continue;
			} else {
				debug("%s at %d of %zu, recv error: %m",
				      __func__, recvlen, size);
				errno = SLURM_COMMUNICATIONS_RECEIVE_ERROR;
				recvlen = SLURM_ERROR;
				goto done;
			}
		}
		if (rc == 0) {
			debug("%s at %d of %zu, recv zero bytes",
			      __func__, recvlen, size);
			errno = SLURM_PROTOCOL_SOCKET_ZERO_BYTES_SENT;
			recvlen = SLURM_ERROR;
			goto done;
		}
		recvlen += rc;
	}


    done:
	/* Reset fd flags to prior state, preserve errno */
	if (fd_flags != -1) {
		int slurm_err = errno;
		if (fcntl(fd, F_SETFL, fd_flags) < 0)
			error("%s: fcntl(F_SETFL) error: %m", __func__);
		errno = slurm_err;
	}

	return recvlen;
}

extern int slurm_init_msg_engine(slurm_addr_t *addr, bool quiet)
{
	int rc;
	int fd;
	int log_lvl = LOG_LEVEL_ERROR;
	const int one = 1;
	const size_t sz1 = sizeof(one);

	if (quiet)
		log_lvl = LOG_LEVEL_DEBUG;

	if ((fd = socket(addr->ss_family, SOCK_STREAM | SOCK_CLOEXEC,
			 IPPROTO_TCP)) < 0) {
		format_print(log_lvl, "Error creating slurm stream socket: %m");
		return fd;
	}

	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sz1);
	if (rc < 0) {
		format_print(log_lvl, "setsockopt SO_REUSEADDR failed: %m");
		goto error;
	}

	rc = bind(fd, (struct sockaddr const *) addr, sizeof(*addr));
	if (rc < 0) {
		format_print(log_lvl, "Error binding slurm stream socket: %m");
		goto error;
	}

	if (listen(fd, SLURM_DEFAULT_LISTEN_BACKLOG) < 0) {
		format_print(log_lvl,
			     "Error listening on slurm stream socket: %m");
		rc = SLURM_ERROR;
		goto error;
	}

	return fd;

error:
	(void) close(fd);
	return rc;
}

/* Await a connection on socket FD.
 * When a connection arrives, open a new socket to communicate with it,
 * set *ADDR (which is *ADDR_LEN bytes long) to the address of the connecting
 * peer and *ADDR_LEN to the address's actual length, and return the
 * new socket's descriptor, or -1 for errors.  */
extern int slurm_accept_msg_conn(int fd, slurm_addr_t *addr)
{
	socklen_t len = sizeof(*addr);
	int sock = accept4(fd, (struct sockaddr *) addr, &len, SOCK_CLOEXEC);
	net_set_nodelay(sock, true, NULL);
	return sock;
}

extern int slurm_open_stream(slurm_addr_t *addr, bool retry)
{
	int retry_cnt;
	int fd;

	if ((slurm_addr_is_unspec(addr)) || (slurm_get_port(addr) == 0)) {
		error("Error connecting, bad data: family = %u, port = %u",
		      addr->ss_family, slurm_get_port(addr));
		return SLURM_ERROR;
	}

	for (retry_cnt=0; ; retry_cnt++) {
		int rc;

		fd = socket(addr->ss_family, SOCK_STREAM | SOCK_CLOEXEC,
			    IPPROTO_TCP);
		if (fd < 0) {
			error("Error creating slurm stream socket: %m");
			return SLURM_ERROR;
		}

		net_set_nodelay(fd, true, NULL);

		if (retry_cnt) {
			if (retry_cnt == 1) {
				debug3("Error connecting, "
				       "picking new stream port");
			}
			_sock_bind_wild(fd);
		}

		rc = _slurm_connect(fd, (struct sockaddr const *)addr,
				    sizeof(*addr));
		/* always set errno as upstream callers expect it */
		errno = rc;

		if (!rc) {
			/* success */
			break;
		}

		if (((rc != ECONNREFUSED) && (rc != ETIMEDOUT)) ||
		    (!retry) || (retry_cnt >= PORT_RETRIES)) {
			errno = rc;
			goto error;
		}

		(void) close(fd);
	}

	return fd;

error:
	debug2("Error connecting slurm stream socket at %pA: %m", addr);
	(void) close(fd);
	return SLURM_ERROR;
}

/* Put the local address of FD into *ADDR and its length in *LEN.  */
extern int slurm_get_stream_addr(int fd, slurm_addr_t *addr )
{
	socklen_t size = sizeof(*addr);
	return getsockname(fd, (struct sockaddr *) addr, &size);
}

/* Open a connection on socket FD to peer at ADDR (which LEN bytes long).
 * For connectionless socket types, just set the default address to send to
 * and the only address from which to accept transmissions.
 * Return SLURM_SUCCESS or error  */
static int _slurm_connect (int __fd, struct sockaddr const * __addr,
			   socklen_t __len)
{
	/* From "man connect": Note that for IP sockets the timeout
	 * may be very long when syncookies are enabled on the server.
	 *
	 * Timeouts in excess of 3 minutes have been observed, resulting
	 * in serious problems for slurmctld. Making the connect call
	 * non-blocking and polling seems to fix the problem. */
	int rc, flags, flags_save;
	struct pollfd ufds;

	flags = fcntl(__fd, F_GETFL);
	flags_save = flags;
	if (flags == -1) {
		error("%s: fcntl(F_GETFL) error: %m", __func__);
		flags = 0;
	}
	if (fcntl(__fd, F_SETFL, flags | O_NONBLOCK) < 0)
		error("%s: fcntl(F_SETFL) error: %m", __func__);

	rc = connect(__fd , __addr , __len);
	if ((rc < 0) && (errno != EINPROGRESS))
		return errno;
	if (rc == 0)
		goto done;  /* connect completed immediately */

	ufds.fd = __fd;
	ufds.events = POLLIN | POLLOUT;
	ufds.revents = 0;

again:
	rc = poll(&ufds, 1, slurm_conf.tcp_timeout * 1000);
	if (rc == -1) {
		int lerrno = errno;

		/* poll failed */
		if (lerrno == EINTR) {
			/* NOTE: connect() is non-interruptible in Linux */
			debug2("%s: poll() failed for %pA: %s",
			      __func__, __addr, slurm_strerror(lerrno));
			goto again;
		}

		error("%s: poll() failed for %pA: %s",
		      __func__, __addr, slurm_strerror(lerrno));
		return lerrno;
	} else if (rc == 0) {
		/* poll timed out before any socket events */
		debug2("%s: connect to %pA in %us: %s",
		      __func__, __addr, slurm_conf.tcp_timeout,
		      slurm_strerror(ETIMEDOUT));
		return ETIMEDOUT;
	} else if (ufds.revents & POLLERR) {
		int err;

		/* poll saw some event on the socket
		 * We need to check if the connection succeeded by
		 * using getsockopt.  The revent is not necessarily
		 * POLLERR when the connection fails! */
		if ((rc = fd_get_socket_error(__fd, &err)))
			return rc;

		/* NOTE: Connection refused is typically reported for
		 * non-responsived nodes plus attempts to communicate
		 * with terminated srun commands. */
		debug2("%s: failed to connect to %pA: %s",
		       __func__, __addr, slurm_strerror(err));
		return err;
	}

done:
	if (flags_save != -1) {
		if (fcntl(__fd, F_SETFL, flags_save) < 0)
			error("%s: fcntl(F_SETFL) error: %m", __func__);
	}

	return SLURM_SUCCESS;
}

extern void slurm_set_addr(slurm_addr_t *addr, uint16_t port, char *host)
{
	struct addrinfo *ai_ptr, *ai_start;

	log_flag(NET, "%s: called with port='%u' host='%s'",
		__func__, port, host);

	/*
	 * xgetaddrinfo uses hints from our config to determine what address
	 * families to return
	 */
	ai_start = xgetaddrinfo_port(host, port);

	if (!ai_start) {
		error_in_daemon("%s: Unable to resolve \"%s\"", __func__, host);
		addr->ss_family = AF_UNSPEC;
		return;
	}

	/*
	 * When host is null, assume we are trying to bind here.
	 * Make sure we return the v6 wildcard address first (when applicable)
	 * since we want v6 to be the default.
	 */
	if (host || !(slurm_conf.conf_flags & CONF_FLAG_IPV6_ENABLED)) {
		ai_ptr = ai_start;
	} else {
		for (ai_ptr = ai_start; ai_ptr; ai_ptr = ai_ptr->ai_next) {
			if (ai_ptr->ai_family == AF_INET6)
				break;
		}
		/* fall back to whatever was available */
		if (!ai_ptr)
			ai_ptr = ai_start;
	}

	memcpy(addr, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
	log_flag(NET, "%s: update addr. addr='%pA'", __func__, addr);
	freeaddrinfo(ai_start);
}

extern void slurm_pack_addr(slurm_addr_t *addr, buf_t *buffer)
{
	pack16(addr->ss_family, buffer);

	if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) addr;
		packmem(in6->sin6_addr.s6_addr, 16, buffer);
		pack16(in6->sin6_port, buffer);
	} else if (addr->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *) addr;
		pack32(in->sin_addr.s_addr, buffer);
		pack16(in->sin_port, buffer);
	}
}

extern int slurm_unpack_addr_no_alloc(slurm_addr_t *addr, buf_t *buffer)
{
	uint16_t tmp_uint16 = 0;

	/* ss_family is only uint8_t on BSD. */
	safe_unpack16(&tmp_uint16, buffer);
	addr->ss_family = tmp_uint16;

	if (addr->ss_family == AF_INET6) {
		uint32_t size;
		char *buffer_addr;
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) addr;

		safe_unpackmem_ptr(&buffer_addr, &size, buffer);
		if (size != 16)
			goto unpack_error;
		memcpy(&in6->sin6_addr.s6_addr, buffer_addr, 16);

		safe_unpack16(&in6->sin6_port, buffer);
	} else if (addr->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *) addr;

		safe_unpack32(&in->sin_addr.s_addr, buffer);
		safe_unpack16(&in->sin_port, buffer);
	} else {
		memset(addr, 0, sizeof(*addr));
	}
	return SLURM_SUCCESS;

unpack_error:
	return SLURM_ERROR;
}
