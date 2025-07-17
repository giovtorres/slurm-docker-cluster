/*****************************************************************************\
 *  net.c - basic network communications for user application I/O
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona1@llnl.gov>, Kevin Tew <tew1@llnl.gov>,
 *  et. al.
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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/common/read_config.h"

#if defined(__FreeBSD__) || defined(__NetBSD__)
#define	SOL_TCP		IPPROTO_TCP
#endif

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif /* NI_MAXHOST */
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif /* NI_MAXSERV */

#define CON_NAME_PLACE_HOLDER_LEN 25

#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/net.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/util-net.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

/*
 * Define slurm-specific aliases for use by plugins, see slurm_xlator.h
 * for details.
 */
strong_alias(net_stream_listen,		slurm_net_stream_listen);

/* open a stream socket on an ephemereal port and put it into
 * the listen state. fd and port are filled in with the new
 * socket's file descriptor and port #.
 *
 * OUT fd - listening socket file descriptor number
 * OUT port - TCP port number in host byte order
 */
int net_stream_listen(int *fd, uint16_t *port)
{
	slurm_addr_t sin;
	socklen_t len = sizeof(sin);
	int val = 1;

	/* bind ephemeral port */
	slurm_setup_addr(&sin, 0);

	if ((*fd = socket(sin.ss_family, SOCK_STREAM, IPPROTO_TCP)) < 0)
		return -1;

	if (setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
		goto cleanup;
	if (bind(*fd, (struct sockaddr *) &sin, len) < 0)
		goto cleanup;
	if (getsockname(*fd, (struct sockaddr *) &sin, &len) < 0)
		goto cleanup;

	*port = slurm_get_port(&sin);
	if (listen(*fd, SLURM_DEFAULT_LISTEN_BACKLOG) < 0)
		goto cleanup;

	return 1;

cleanup:
	close(*fd);
	return -1;
}

/* set keepalive time on socket */
extern void net_set_keep_alive(int sock)
{
	int opt_int;
	socklen_t opt_len;
	struct linger opt_linger;

	if (slurm_conf.keepalive_time == NO_VAL)
		return;

	opt_len = sizeof(struct linger);
	opt_linger.l_onoff = 1;
	opt_linger.l_linger = slurm_conf.keepalive_time;
	if (setsockopt(sock, SOL_SOCKET, SO_LINGER, &opt_linger, opt_len) < 0)
		error("Unable to set linger socket option: %m");

	opt_len = sizeof(opt_int);
	opt_int = slurm_conf.keepalive_time;
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt_int, opt_len) < 0) {
		error("Unable to set keepalive socket option: %m");
		return;
	}

/*
 * TCP_KEEPIDLE used to be defined in FreeBSD, then went away, then came
 * back in 9.0.
 *
 * Removing this call might decrease the robustness of communications,
 * but will probably have no noticable effect.
 */
#if !defined (__APPLE__) && (! defined(__FreeBSD__) || (__FreeBSD_version > 900000))
	if (slurm_conf.keepalive_interval != NO_VAL) {
		opt_int = slurm_conf.keepalive_interval;
		if (setsockopt(sock, SOL_TCP, TCP_KEEPINTVL,
			       &opt_int, opt_len) < 0) {
			error("Unable to set keepalive interval: %m");
			return;
		}
	}
	if (slurm_conf.keepalive_probes != NO_VAL) {
		opt_int = (int) slurm_conf.keepalive_probes;
		if (setsockopt(sock, SOL_TCP, TCP_KEEPCNT,
			       &opt_int, opt_len) < 0) {
			error("Unable to set keepalive probes: %m");
			return;
		}
	}
	opt_int = slurm_conf.keepalive_time;
	if (setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, &opt_int, opt_len) < 0) {
		error("Unable to set keepalive socket time: %m");
		return;
	}
#endif

#if 0
	/* Used to validate above operations for testing purposes */
	opt_linger.l_onoff = 0;
	opt_linger.l_linger = 0;
	opt_len = sizeof(struct linger);
	getsockopt(sock, SOL_SOCKET, SO_LINGER, &opt_linger, &opt_len);
	info("got linger time of %d:%d on fd %d", opt_linger.l_onoff,
	     opt_linger.l_linger, sock);

	opt_len = sizeof(opt_len);
	getsockopt(sock, SOL_TCP, TCP_KEEPINTVL, &opt_int, &opt_len);
	info("got keepalive_interval is %d on fd %d", opt_int, sock);
	getsockopt(sock, SOL_TCP, TCP_KEEPCNT, &opt_int, &opt_len);
	info("got keepalive_probes is %d on fd %d", opt_int, sock);
	getsockopt(sock, SOL_TCP, TCP_KEEPIDLE, &opt_int, &opt_len);
	info("got keepalive_time is %d on fd %d", opt_int, sock);
#endif
}

extern int net_set_nodelay(int sock, bool set, const char *con_name)
{
	int opt_int;

	if (sock < 0)
		return EBADF;

	if (set)
		opt_int = 1;
	else
		opt_int = 0;

	if (setsockopt(sock, SOL_TCP, TCP_NODELAY, &opt_int, sizeof(int))) {
		int rc = errno;
		char lcon_name[CON_NAME_PLACE_HOLDER_LEN] = {0};

		if (!con_name) {
			snprintf(lcon_name, sizeof(lcon_name), "fd:%d", sock);
			con_name = lcon_name;
		}

		error("[%s] Unable to set TCP_NODELAY: %s",
		      con_name, slurm_strerror(rc));

		return rc;
	}

	return SLURM_SUCCESS;
}

/*
 * Check if we can bind() the socket s to port port.
 *
 * IN: s - socket
 * IN: port - port number to attempt to bind
 * IN: local - only bind to localhost if true
 * OUT: true/false if port was bound successfully
 */
static bool _is_port_ok(int s, uint16_t port, bool local)
{
	slurm_addr_t addr;
	slurm_setup_addr(&addr, port);

	if (!local) {
		debug3("%s: requesting non-local port", __func__);
	} else if (addr.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *) &addr;
		sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	} else if (addr.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin = (struct sockaddr_in6 *) &addr;
		sin->sin6_addr = in6addr_loopback;
	} else {
		error("%s: protocol family %u unsupported",
		      __func__, addr.ss_family);
		return false;
	}

	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		log_flag(NET, "%s: bind() failed on port:%d fd:%d: %m",
			 __func__, port, s);
		return false;
	}

	return true;
}

/* net_stream_listen_ports()
 */
int net_stream_listen_ports(int *fd, uint16_t *port, uint16_t *ports, bool local)
{
	slurm_addr_t sin;
	uint32_t min = ports[0], max = ports[1];
	uint32_t num = max - min + 1;

	xassert(num > 0);

	srandom(getpid());
	*port = min + (random() % num);

	slurm_setup_addr(&sin, 0); /* Decide on IPv4 or IPv6 */

	*fd = -1;

	for (int i = 0; i < num; i++) {
		if (*fd < 0) {
			const int one = 1;

			if ((*fd = socket(sin.ss_family, SOCK_STREAM,
					  IPPROTO_TCP)) < 0) {
				log_flag(NET, "%s: socket() failed: %m",
					 __func__);
				return -1;
			}

			if (setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &one,
				       sizeof(int)) < 0) {
				log_flag(NET, "%s: setsockopt() failed: %m",
					 __func__);
				close(*fd);
				return -1;
			}
		}

		if (_is_port_ok(*fd, *port, local)) {
			if (!listen(*fd, SLURM_DEFAULT_LISTEN_BACKLOG))
				return *fd;

			log_flag(NET, "%s: listen() failed: %m",
				 __func__);

			/*
			 * If bind() succeeds but listen() fails we need to
			 * close and reestablish the socket before trying
			 * again on another port number.
			 */
			if (close(*fd)) {
				log_flag(NET, "%s: close(%d) failed: %m",
					 __func__, *fd);
			}
			*fd = -1;
		}

		if (*port == max)
			*port = min;
		else
			++(*port);
	}

	if (*fd >= 0)
		close(*fd);

	error("%s: all ports in range (%u, %u) exhausted, cannot establish listening port",
	      __func__, min, max);

	return -1;
}

extern char *sockaddr_to_string(const slurm_addr_t *addr, socklen_t addrlen)
{
	int prev_errno = errno;
	char *resp = NULL;
	int port = 0;
	char *host = NULL;

	if (addr->ss_family == AF_UNSPEC)
		return NULL;

	if (addr->ss_family == AF_UNIX) {
		const struct sockaddr_un *addr_un =
			(const struct sockaddr_un *) addr;

		xassert(addr_un->sun_path[sizeof(addr_un->sun_path) - 1] ==
			'\0');

		/* path may not be set */
		if (addr_un->sun_path[0])
			return xstrdup_printf("unix:%s", addr_un->sun_path);
		else
			return NULL;
	}

	if (addr->ss_family == AF_INET)
		port = ((struct sockaddr_in *) addr)->sin_port;
	else if (addr->ss_family == AF_INET6)
		port = ((struct sockaddr_in6 *) addr)->sin6_port;

	host = xgetnameinfo(addr);

	/* construct RFC3986 host port pair */
	if (host && port)
		xstrfmtcat(resp, "[%s]:%d", host, port);
	else if (port)
		xstrfmtcat(resp, "[::]:%d", port);

	xfree(host);

	/*
	 * Avoid clobbering errno as this function is likely to be used for
	 * error logging, and stepping on errno prevents %m from working.
	 */
	errno = prev_errno;
	return resp;
}

extern char *addrinfo_to_string(const struct addrinfo *addr)
{
	return sockaddr_to_string((const slurm_addr_t *) addr->ai_addr,
				  addr->ai_addrlen);
}

extern slurm_addr_t sockaddr_from_unix_path(const char *path)
{
	slurm_addr_t addr = {
		.ss_family = AF_UNSPEC,
	};
	struct sockaddr_un *un = (struct sockaddr_un *) &addr;

	if (!path)
		return addr;

	if (strlcpy(un->sun_path, path, sizeof(un->sun_path)) != strlen(path))
		return addr;

	/* Did not overflow - set family to indicate success */
	addr.ss_family = AF_UNIX;
	return addr;
}
