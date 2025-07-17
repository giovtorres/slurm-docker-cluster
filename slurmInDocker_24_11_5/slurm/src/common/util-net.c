/*****************************************************************************\
 *  Copyright (C) 2001-2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  UCRL-CODE-2002-009.
 *
 *  This file is part of ConMan, a remote console management program.
 *  For details, see <http://www.llnl.gov/linux/conman/>.
 *
 *  ConMan is free software; you can redistribute it and/or modify it under
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
 *  ConMan is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with ConMan; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *****************************************************************************
 *  Refer to "util-net.h" for documentation on public functions.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>	/* for PATH_MAX */
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "slurm/slurm.h"

#include "src/common/read_config.h"
#include "src/common/run_in_daemon.h"
#include "src/common/strlcpy.h"
#include "src/common/util-net.h"
#include "src/common/macros.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

static pthread_mutex_t hostentLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t getnameinfo_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

typedef struct {
	slurm_addr_t addr;
	time_t expiration;
	char *host;
} getnameinfo_cache_t;

static list_t *nameinfo_cache = NULL;


static int copy_hostent(const struct hostent *src, char *dst, int len);
#ifndef NDEBUG
static int validate_hostent_copy(
	const struct hostent *src, const struct hostent *dst);
#endif /* !NDEBUG */


struct hostent * get_host_by_name(const char *name,
				  void *buf, int buflen, int *h_err)
{
/*  gethostbyname() is not thread-safe, and there is no frelling standard
 *    for gethostbyname_r() -- the arg list varies from system to system!
 */
	struct hostent *hptr;
	int n = 0;

	xassert(name && buf);

	slurm_mutex_lock(&hostentLock);
	/* It appears gethostbyname leaks memory once.  Under the covers it
	 * calls gethostbyname_r (at least on Ubuntu 16.10).  This leak doesn't
	 * appear to get worst, meaning it only happens once, so we should be
	 * ok.  Though gethostbyname is obsolete now we can't really change
	 * since aliases don't work we can't change.
	 */
	if ((hptr = gethostbyname(name)))
		n = copy_hostent(hptr, buf, buflen);
	if (h_err)
		*h_err = h_errno;
	slurm_mutex_unlock(&hostentLock);

	if (n < 0) {
		errno = ERANGE;
		return(NULL);
	}
	return(hptr ? (struct hostent *) buf : NULL);
}

static int copy_hostent(const struct hostent *src, char *buf, int len)
{
/*  Copies the (src) hostent struct (and all of its associated data)
 *    into the buffer (buf) of length (len).
 *  Returns 0 if the copy is successful, or -1 if the length of the buffer
 *    is not large enough to hold the result.
 *
 *  Note that the order in which data is copied into (buf) is done
 *    in such a way as to ensure everything is properly word-aligned.
 *    There is a method to the madness.
 */
	struct hostent *dst;
	int n;
	char **p, **q;

	xassert(src && buf);

	dst = (struct hostent *) buf;
	if ((len -= sizeof(struct hostent)) < 0)
		return(-1);
	dst->h_addrtype = src->h_addrtype;
	dst->h_length = src->h_length;
	buf += sizeof(struct hostent);

	/*  Reserve space for h_aliases[].
	 */
	dst->h_aliases = (char **) buf;
	for (p=src->h_aliases, q=dst->h_aliases, n=0; *p; p++, q++, n++) {;}
	if ((len -= ++n * sizeof(char *)) < 0)
		return(-1);
	buf = (char *) (q + 1);

	/*  Reserve space for h_addr_list[].
	 */
	dst->h_addr_list = (char **) buf;
	for (p=src->h_addr_list, q=dst->h_addr_list, n=0; *p; p++, q++, n++) {;}
	if ((len -= ++n * sizeof(char *)) < 0)
		return(-1);
	buf = (char *) (q + 1);

	/*  Copy h_addr_list[] in_addr structs.
	 */
	for (p=src->h_addr_list, q=dst->h_addr_list; *p; p++, q++) {
		if ((len -= src->h_length) < 0)
			return(-1);
		memcpy(buf, *p, src->h_length);
		*q = buf;
		buf += src->h_length;
	}
	*q = NULL;

	/*  Copy h_aliases[] strings.
	 */
	for (p=src->h_aliases, q=dst->h_aliases; *p; p++, q++) {
		n = strlcpy(buf, *p, len);
		*q = buf;
		buf += ++n;                     /* allow for trailing NUL char */
		if ((len -= n) < 0)
			return(-1);
	}
	*q = NULL;

	/*  Copy h_name string.
	 */
	dst->h_name = buf;
	n = strlcpy(buf, src->h_name, len);
	buf += ++n;                         /* allow for trailing NUL char */
	if ((len -= n) < 0)
		return(-1);

	xassert(validate_hostent_copy(src, dst) >= 0);
	xassert(buf);	/* Used only to eliminate CLANG error */
	return(0);
}


#ifndef NDEBUG
static int validate_hostent_copy(
	const struct hostent *src, const struct hostent *dst)
{
/*  Validates the src hostent struct has been successfully copied into dst.
 *  Returns 0 if the copy is good; o/w, returns -1.
 */
	char **p, **q;

	xassert(src && dst);

	if (!dst->h_name)
		return(-1);
	if (src->h_name == dst->h_name)
		return(-1);
	if (xstrcmp(src->h_name, dst->h_name))
		return(-1);
	if (src->h_addrtype != dst->h_addrtype)
		return(-1);
	if (src->h_length != dst->h_length)
		return(-1);
	for (p=src->h_aliases, q=dst->h_aliases; *p; p++, q++)
		if ((!q) || (p == q) || (xstrcmp(*p, *q)))
			return(-1);
	for (p=src->h_addr_list, q=dst->h_addr_list; *p; p++, q++)
		if ((!q) || (p == q) || (memcmp(*p, *q, src->h_length)))
			return(-1);
	return(0);
}
#endif /* !NDEBUG */

/* is_full_path()
 *
 * Test if the given path is a full or relative one.
 */
extern
bool is_full_path(const char *path)
{
	if (path && path[0] == '/')
		return true;

	return false;
}

/* make_full_path()
 *
 * Given a relative path in input make it full relative
 * to the current working directory.
 */
extern char *make_full_path(const char *rpath)
{
	char *cwd;
	char *cwd2 = NULL;

#ifdef HAVE_GET_CURRENT_DIR_NAME
	cwd = get_current_dir_name();
#else
	cwd = malloc(PATH_MAX);
	cwd = getcwd(cwd, PATH_MAX);
#endif
	xstrfmtcat(cwd2, "%s/%s", cwd, rpath);
	free(cwd);

	return cwd2;
}

static struct addrinfo *_xgetaddrinfo(const char *hostname, const char *serv,
				      const struct addrinfo *hints)
{
	struct addrinfo *result = NULL;
	int err;

	err = getaddrinfo(hostname, serv, hints, &result);
	if (err == EAI_SYSTEM) {
		error_in_daemon("%s: getaddrinfo(%s:%s) failed: %s: %m",
				__func__, hostname, serv, gai_strerror(err));
		return NULL;
	} else if (err != 0) {
		error_in_daemon("%s: getaddrinfo(%s:%s) failed: %s",
				__func__, hostname, serv, gai_strerror(err));
		return NULL;
	}

	return result;
}

extern struct addrinfo *xgetaddrinfo_port(const char *hostname, uint16_t port)
{
	char serv[6];
	snprintf(serv, sizeof(serv), "%hu", port);
	return xgetaddrinfo(hostname, serv);
}

extern struct addrinfo *xgetaddrinfo(const char *hostname, const char *serv)
{
	struct addrinfo hints;
	bool v4_enabled = slurm_conf.conf_flags & CONF_FLAG_IPV4_ENABLED;
	bool v6_enabled = slurm_conf.conf_flags & CONF_FLAG_IPV6_ENABLED;

	memset(&hints, 0, sizeof(hints));

	/* use configured IP support to hint at what address types to return */
	if (v4_enabled && !v6_enabled)
		hints.ai_family = AF_INET;
	else if (!v4_enabled && v6_enabled)
		hints.ai_family = AF_INET6;
	else
		hints.ai_family = AF_UNSPEC;

	/* RFC4291 2.4 "Unspecified" address type or IPv4 INADDR_ANY */
	if (!xstrcmp("::", hostname)) {
		/*
		 * Only specify one address instead of NULL if possible to avoid
		 * EADDRINUSE when trying to bind on IPv4 and IPv6 INADDR_ANY.
		 */
		if (v6_enabled)
			hostname = "0::0";
		else if (v4_enabled)
			hostname = "0.0.0.0";
		else
			hostname = NULL;
	}
	/* RFC4291 2.4 "Loopback" address type */
	if (v6_enabled && !xstrcmp("::1", hostname))
		hostname = "0::1";

	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
	if (hostname)
		hints.ai_flags |= AI_CANONNAME;
	hints.ai_socktype = SOCK_STREAM;

	return _xgetaddrinfo(hostname, serv, &hints);
}

extern int host_has_addr_family(const char *hostname, const char *srv,
				bool *ipv4, bool *ipv6)
{
	struct addrinfo hints;
	struct addrinfo *ai_ptr, *ai_start;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
	if (hostname)
		hints.ai_flags |= AI_CANONNAME;
	hints.ai_socktype = SOCK_STREAM;

	ai_start = _xgetaddrinfo(hostname, srv, &hints);

	if (!ai_start)
		return SLURM_ERROR;

	*ipv4 = *ipv6 = false;
	for (ai_ptr = ai_start; ai_ptr; ai_ptr = ai_ptr->ai_next) {
		if (ai_ptr->ai_family == AF_INET6)
			*ipv6 = true;
		else if (ai_ptr->ai_family == AF_INET)
			*ipv4 = true;
	}

	freeaddrinfo(ai_start);

	return SLURM_SUCCESS;
}

static int _name_cache_find(void *x, void *y)
{
	getnameinfo_cache_t *cache_ent = x;
	const slurm_addr_t *addr_x = &cache_ent->addr;
	const slurm_addr_t *addr_y = y;

	xassert(addr_x);
	xassert(addr_y);
	xassert(addr_x->ss_family != AF_UNIX);
	xassert(addr_y->ss_family != AF_UNIX);

	if (addr_x->ss_family != addr_y->ss_family)
		return false;
	if (addr_x->ss_family == AF_INET) {
		struct sockaddr_in *x4 = (void *)addr_x;
		struct sockaddr_in *y4 = (void *)addr_y;
		if (x4->sin_addr.s_addr != y4->sin_addr.s_addr)
			return false;
	} else if (addr_x->ss_family == AF_INET6) {
		struct sockaddr_in6 *x6 = (void *)addr_x;
		struct sockaddr_in6 *y6 = (void *)addr_y;
		if (memcmp(x6->sin6_addr.s6_addr, y6->sin6_addr.s6_addr,
		    sizeof(x6->sin6_addr.s6_addr)))
			return false;
	}
	return true;
}

static void _getnameinfo_cache_destroy(void *obj)
{
	getnameinfo_cache_t *entry = obj;

	xfree(entry->host);
	xfree(entry);
}

extern void getnameinfo_cache_purge(void)
{
	slurm_rwlock_wrlock(&getnameinfo_cache_lock);
	FREE_NULL_LIST(nameinfo_cache);
	slurm_rwlock_unlock(&getnameinfo_cache_lock);
}

static char *_getnameinfo(const slurm_addr_t *addr)
{
	char hbuf[NI_MAXHOST] = "\0";
	int err;

	err = getnameinfo((const struct sockaddr *) addr, sizeof(*addr),
			  hbuf, sizeof(hbuf), NULL, 0, NI_NAMEREQD);
	if (err == EAI_SYSTEM) {
		log_flag(NET, "%s: getnameinfo(%pA) failed: %s: %m",
			 __func__, addr, gai_strerror(err));
		return NULL;
	} else if (err) {
		log_flag(NET, "%s: getnameinfo(%pA) failed: %s",
			 __func__, addr, gai_strerror(err));
		return NULL;
	}

	return xstrdup(hbuf);
}

extern char *xgetnameinfo(const slurm_addr_t *addr)
{
	getnameinfo_cache_t *cache_ent = NULL;
	char *name = NULL;
	time_t now = 0;
	bool new = false;

	if (!slurm_conf.getnameinfo_cache_timeout)
		return _getnameinfo(addr);

	slurm_rwlock_rdlock(&getnameinfo_cache_lock);
	now = time(NULL);
	if (nameinfo_cache) {
		cache_ent = list_find_first_ro(nameinfo_cache, _name_cache_find,
					       (void *) addr);
		if (cache_ent && (cache_ent->expiration > now)) {
			name = xstrdup(cache_ent->host);
			slurm_rwlock_unlock(&getnameinfo_cache_lock);
			log_flag(NET, "%s: %pA = %s (cached)",
				 __func__, addr, name);
			return name;
		}
	}
	slurm_rwlock_unlock(&getnameinfo_cache_lock);

	/*
	 * Errors will leave expired cache records in place.
	 * That is okay, we'll find them and attempt to update them again.
	 */
	if (!(name = _getnameinfo(addr)))
		return NULL;

	slurm_rwlock_wrlock(&getnameinfo_cache_lock);
	if (!nameinfo_cache)
		nameinfo_cache = list_create(_getnameinfo_cache_destroy);

	cache_ent = list_find_first(nameinfo_cache, _name_cache_find,
				    (void *) addr);

	if (!cache_ent) {
		cache_ent = xmalloc(sizeof(*cache_ent));
		cache_ent->addr = *addr;
		new = true;
	}

	/*
	 * The host name could have changed for expired cache records, so just
	 * blindly update the cache record every time to be safe.
	 */
	xfree(cache_ent->host);
	cache_ent->host = xstrdup(name);
	cache_ent->expiration = now + slurm_conf.getnameinfo_cache_timeout;

	if (new) {
		log_flag(NET, "%s: Adding to cache - %pA = %s",
			 __func__, addr, name);
		list_append(nameinfo_cache, cache_ent);
	} else {
		log_flag(NET, "%s: Updating cache - %pA = %s",
			 __func__, addr, name);
	}
	slurm_rwlock_unlock(&getnameinfo_cache_lock);

	return name;
}
