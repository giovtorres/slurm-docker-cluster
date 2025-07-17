/*****************************************************************************\
 *  plugrack.c - an intelligent container for plugins
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jay Windley <jwindley@lnxi.com>.
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

#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/common/macros.h"
#include "src/common/plugrack.h"
#include "src/common/read_config.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

strong_alias(plugrack_create,         slurm_plugrack_create);
strong_alias(plugrack_destroy,        slurm_plugrack_destroy);
strong_alias(plugrack_read_dir,       slurm_plugrack_read_dir);
strong_alias(plugrack_use_by_type,    slurm_plugrack_use_by_type);

/*
 * Represents a plugin in the rack.
 *
 * full_type is the fully-qualified plugin type, e.g., "auth/kerberos".
 * For the low-level plugin interface the type can be whatever it needs
 * to be.  For the rack-level interface, the type exported by the plugin
 * must be of the form "<major>/<minor>".
 *
 * fq_path is the fully-qualified pathname to the plugin.
 *
 * plug is the plugin handle.  If it is equal to PLUGIN_INVALID_HANDLE
 * then the plugin is not currently loaded in memory.
 *
 * refcount shows how many clients have requested to use the plugin.
 * If this is zero, the rack code may decide to unload the plugin.
 */
typedef struct _plugrack_entry {
	const char *full_type;
	const char *fq_path;
	plugin_handle_t	plug;
	int refcount;
} plugrack_entry_t;

/*
 * Implementation of the plugin rack.
 *
 * entries is the list of plugrack_entry_t.
 */
struct _plugrack {
	list_t *entries;
	const char *major_type;
};

static bool _match_major(const char *path_name, const char *major_type);
static int _plugrack_read_single_dir(plugrack_t *rack, char *dir);
static bool _so_file(char *pathname);

/*
 * Destructor function for the List code.  This should entirely
 * clean up a plugin_entry_t.
 */
static void plugrack_entry_destructor(void *v)
{
	plugrack_entry_t *victim = v;

	if (victim == NULL)
		return;

	/*
	 * Free memory and unload the plugin if necessary.  The assert
	 * is to make sure we were actually called from the List destructor
	 * which should only be callable from plugrack_destroy().
	 */
	xassert(victim->refcount == 0);
	xfree(victim->full_type);
	xfree(victim->fq_path);
	if (victim->plug != PLUGIN_INVALID_HANDLE)
		plugin_unload(victim->plug);
	xfree(victim);
}

plugrack_t *plugrack_create(const char *major_type)
{
	plugrack_t *rack = xmalloc(sizeof(*rack));

	rack->major_type = xstrdup(major_type);
	rack->entries = list_create(plugrack_entry_destructor);
	return rack;
}

int plugrack_destroy(plugrack_t *rack)
{
	list_itr_t *it;
	plugrack_entry_t *e;

	if (!rack)
		return SLURM_ERROR;

	/*
	 * See if there are any plugins still being used.  If we unload them,
	 * the program might crash because cached virtual mapped addresses
	 * will suddenly be outside our virtual address space.
	 */
	it = list_iterator_create(rack->entries);
	while ((e = list_next(it))) {
		if (e->refcount > 0) {
			debug2("%s: attempt to destroy %s plugin rack that is still in use",
			       __func__, rack->major_type);
			list_iterator_destroy(it);
			return SLURM_ERROR; /* plugins still in use. */
		}
	}
	list_iterator_destroy(it);

	FREE_NULL_LIST(rack->entries);
	xfree(rack->major_type);
	xfree(rack);
	return SLURM_SUCCESS;
}

static int plugrack_add_plugin_path(plugrack_t *rack,
				    const char *full_type,
				    const char *fq_path)
{
	plugrack_entry_t *e;

	if ((!rack) || (!fq_path))
		return SLURM_ERROR;

	e = xmalloc(sizeof(*e));

	e->full_type = xstrdup(full_type);
	e->fq_path   = xstrdup(fq_path);
	e->plug      = PLUGIN_INVALID_HANDLE;
	e->refcount  = 0;
	list_append(rack->entries, e);

	return SLURM_SUCCESS;
}

/* test for the plugin in the various colon separated directories */
int plugrack_read_dir(plugrack_t *rack, const char *dir)
{
	char *head, *dir_array;
	int i, rc = SLURM_SUCCESS;

	if ((!rack) || (!dir))
		return SLURM_ERROR;

	dir_array = xstrdup(dir);
	head = dir_array;
	for (i = 0; ; i++) {
		if (dir_array[i] == '\0') {
			if (_plugrack_read_single_dir(rack, head) ==
			    SLURM_ERROR)
				rc = SLURM_ERROR;
			break;
		} else if (dir_array[i] == ':') {
			dir_array[i] = '\0';
			if (_plugrack_read_single_dir(rack, head) ==
			    SLURM_ERROR)
				rc = SLURM_ERROR;
			head = dir_array + i + 1;
		}
	}
	xfree(dir_array);
	return rc;
}

static int _plugrack_read_single_dir(plugrack_t *rack, char *dir)
{
	char *fq_path;
	char *tail;
	DIR *dirp;
	struct dirent *e;
	struct stat st;
	static const size_t type_len = 64;
	char plugin_type[type_len];
	static int max_path_len = 0;

	/* Allocate a buffer for fully-qualified path names. */
	if (max_path_len == 0) {
		max_path_len = pathconf("/", _PC_NAME_MAX);
		if (max_path_len <= 0)
			max_path_len = 256;
	}
	fq_path = xmalloc(strlen(dir) + max_path_len + 1);

	/*
	 * Write the directory name in it, then a separator, then
	 * keep track of where we want to write the individual file
	 * names.
	 */
	strcpy(fq_path, dir);
	tail = &fq_path[strlen(dir)];
	*tail = '/';
	++tail;

	/* Open the directory. */
	dirp = opendir(dir);
	if (dirp == NULL) {
		error("cannot open plugin directory %s", dir);
		xfree(fq_path);
		return SLURM_ERROR;
	}

	while (1) {
		e = readdir(dirp);
		if (e == NULL)
			break;

		/*
		 * Compose file name.  Where NAME_MAX is defined it represents
		 * the largest file name given in a dirent.  This macro is used
		 * in the  allocation of "tail" above, so this unbounded copy
		 * should work.
		 */
		strcpy(tail, e->d_name);

		/* Check only regular files. */
		if ((xstrncmp(e->d_name, ".", 1) == 0) ||
		    (stat(fq_path, &st) < 0) ||
		    (!S_ISREG(st.st_mode)))
			continue;

		/* Check only shared object files */
		if (!_so_file(e->d_name))
			continue;

		/* file's prefix must match specified major_type
		 * to avoid having some program try to open a
		 * plugin designed for a different program and
		 * discovering undefined symbols */
		if ((rack->major_type) &&
		    (!_match_major(e->d_name, rack->major_type)))
			continue;

		/* Test the type. */
		if (plugin_peek(fq_path, plugin_type, type_len) !=
		    SLURM_SUCCESS) {
			continue;
		}

		if (rack->major_type &&
		    (xstrncmp(rack->major_type, plugin_type,
			      strlen(rack->major_type)) != 0)) {
			continue;
		}

		/* Add it to the list. */
		(void) plugrack_add_plugin_path(rack, plugin_type, fq_path);
	}

	closedir(dirp);

	xfree(fq_path);
	return SLURM_SUCCESS;
}

/*
 * Return true if the specified pathname is recognized as that of a shared
 * object (i.e. containing ".so\0")
 */
static bool _so_file(char *file_name)
{
	int i;

	if (file_name == NULL)
		return false;

	for (i=0; file_name[i]; i++) {
		if ((file_name[i]   == '.') && (file_name[i+1] == 's') &&
		    (file_name[i+2] == 'o') && (file_name[i+3] == '\0'))
			return true;
	}
	return false;
}

/* Return true of the specified major_type is a prefix of the shared object
 * pathname (i.e. either "<major_name>..." or "lib<major_name>...") */
static bool _match_major(const char *path_name, const char *major_type)
{
	char *head = (char *)path_name;

	if (xstrncmp(head, "lib", 3) == 0)
		head += 3;
	if (xstrncmp(head, major_type, strlen(major_type)))
		return false;
	return true;
}

plugin_handle_t plugrack_use_by_type(plugrack_t *rack, const char *full_type)
{
	list_itr_t *it;
	plugrack_entry_t *e;

	if ((!rack) || (!full_type))
		return PLUGIN_INVALID_HANDLE;

	it = list_iterator_create(rack->entries);
	while ((e = list_next(it))) {
		int err;

		if (xstrcmp(full_type, e->full_type) != 0)
			continue;

		/* See if plugin is loaded. */
		if (e->plug == PLUGIN_INVALID_HANDLE  &&
		    (err = plugin_load_from_file(&e->plug, e->fq_path)))
			error("%s: %s", e->fq_path, slurm_strerror(err));

		/* If load was successful, increment the reference count. */
		if (e->plug != PLUGIN_INVALID_HANDLE) {
			e->refcount++;
			debug3("%s: loaded plugin %s for type:%s",
			       __func__, e->fq_path, full_type);
		}

		/*
		 * Return the plugin, even if it failed to load -- this serves
		 * as an error return value.
		 */
		list_iterator_destroy(it);
		return e->plug;
	}

	/* Couldn't find a suitable plugin. */
	list_iterator_destroy(it);
	return PLUGIN_INVALID_HANDLE;
}

static int _foreach_release_plugin(void *x, void *arg)
{
	plugrack_entry_t *entry = (plugrack_entry_t *) x;
	const char *type = (const char *) arg;

	if (entry->plug == PLUGIN_INVALID_HANDLE)
		return 0;

	if (xstrcmp(entry->full_type, type))
		return 0;

	entry->refcount--;

	if (entry->refcount <= 0) {
		debug5("%s: closing plugin type: %s", __func__, type);
		if (dlclose(entry->plug))
			fatal_abort("%s: unable to dlclose plugin type: %s",
				    __func__, type);
		entry->plug = PLUGIN_INVALID_HANDLE;
	}

	return 0;
}

void plugrack_release_by_type(plugrack_t *rack, const char *type)
{
	(void) list_for_each(rack->entries, _foreach_release_plugin,
			     (void *) type);
}

typedef struct {
	plugrack_foreach_t f;
	void *arg;
} plugrack_foreach_args_t;

extern int plugrack_print_mpi_plugins(plugrack_t *rack)
{
	list_itr_t *itr;
	plugrack_entry_t *e = NULL;
	char *sep, tmp[64], *pmix_vers = NULL, *comma = "";
	int i;

	xassert(rack->entries);
	itr = list_iterator_create(rack->entries);
	printf("MPI plugin types are...\n");
	printf("\tnone\n");
	while ((e = list_next(itr))) {
		/*
		 * Support symbolic links for various pmix plugins with names
		 * that contain version numbers without listing duplicates
		 */
		sep = strstr(e->fq_path, "/mpi_");
		if (sep) {
			sep += 5;
			i = snprintf(tmp, sizeof(tmp), "%s", sep);
			if (i >= sizeof(tmp))
				tmp[sizeof(tmp)-1] = '\0';
			sep = strstr(tmp, ".so");
			if (sep)
				sep[0] = '\0';
			sep = tmp;

			if (!xstrncmp(sep, "pmix_", 5)) {
				xstrfmtcat(pmix_vers, "%s%s", comma, sep);
				comma = ",";
				continue;
			}
		} else
			sep = (char *) e->full_type;	/* Remove "const" */
		printf("\t%s\n", sep);
	}
	list_iterator_destroy(itr);

	if (pmix_vers)
		printf("specific pmix plugin versions available: %s\n", pmix_vers);
	xfree(pmix_vers);

	return SLURM_SUCCESS;
}

static int _foreach_plugin(void *x, void *arg)
{
	plugrack_entry_t *entry = (plugrack_entry_t *) x;
	plugrack_foreach_args_t *args = arg;

	args->f(entry->full_type, entry->fq_path, entry->plug, args->arg);

	return 0;
}

extern void plugrack_foreach(plugrack_t *rack, plugrack_foreach_t f, void *arg)
{
	plugrack_foreach_args_t args = {
		.f = f,
		.arg = arg,
	};

	(void) list_for_each(rack->entries, _foreach_plugin, &args);
}

static bool _plugin_loaded(plugins_t *plugins, const char *plugin)
{
	xassert(plugins->magic == PLUGINS_MAGIC);

	if (plugins->count <= 0)
		return false;

	for (int i = 0; i < plugins->count; i++)
		if (!xstrcasecmp(plugin, plugins->types[i]))
			return true;

	return false;
}

static void _plugrack_foreach(const char *full_type, const char *fq_path,
			      const plugin_handle_t id, void *arg)
{
	plugins_t *plugins = arg;
	size_t i = plugins->count;

	xassert(plugins->magic == PLUGINS_MAGIC);

	if (_plugin_loaded(plugins, full_type)) {
		debug("%s: %s plugin type %s already loaded",
			 __func__, plugins->rack->major_type, full_type);
		/* effectively a no-op if already loaded */
		return;
	}

	plugins->count++;
	xrecalloc(plugins->handles, plugins->count, sizeof(*plugins->handles));
	xrecalloc(plugins->types, plugins->count, sizeof(*plugins->types));

	plugins->types[i] = xstrdup(full_type);
	plugins->handles[i] = id;

	debug("%s: %s plugin type:%s path:%s",
	      __func__, plugins->rack->major_type, full_type, fq_path);
}

extern int load_plugins(plugins_t **plugins_ptr, const char *major_type,
			const char *plugin_list, plugrack_foreach_t listf,
			const char **syms, size_t syms_count)
{
	int rc = SLURM_SUCCESS;
	plugins_t *plugins;

	xassert(plugins_ptr);
	if (!*plugins_ptr) {
		const char *plugin_dir;

		plugins = xmalloc(sizeof(*plugins));
		plugins->magic = PLUGINS_MAGIC;
		plugins->rack = plugrack_create(major_type);

		if (slurm_conf.plugindir)
			plugin_dir = slurm_conf.plugindir;
		else
			plugin_dir = default_plugin_path;

		if ((rc = plugrack_read_dir(plugins->rack, plugin_dir))) {
			error("%s: plugrack_read_dir(%s) failed: %s",
			      __func__, slurm_conf.plugindir, slurm_strerror(rc));
			goto cleanup;
		}
	} else {
		plugins = *plugins_ptr;
		xassert(plugins->magic == PLUGINS_MAGIC);
	}

	if (listf && !xstrcasecmp(plugin_list, "list")) {
		/* call list function ptr and then load all */
		plugrack_foreach(plugins->rack, listf, NULL);
		rc = SLURM_SUCCESS;
		/* reusing plugins on a list request makes no sense */
		xassert(!*plugins_ptr);
		goto cleanup;
	}

	if (!plugin_list) {
		/* no filter specified: load them all */
		plugrack_foreach(plugins->rack, _plugrack_foreach, plugins);
	} else if (plugin_list[0] == '\0') {
		debug("%s: not loading any %s plugins", __func__, major_type);
	} else {
		/* Caller provided which plugins they want */
		char *type, *last = NULL, *pl;
		char *typeslash = xstrdup_printf("%s/", major_type);

		pl = xstrdup(plugin_list);
		type = strtok_r(pl, ",", &last);
		while (type) {
			char *ntype, *otype;
			const size_t offset = strlen(typeslash);

			/*
			 * Permit both prefix and no-prefix for
			 * plugin names.
			 */
			if (!xstrncmp(type, typeslash, offset))
				otype = type + offset;
			else
				otype = type;

			ntype = xstrdup_printf("%s/%s", major_type, otype);
			_plugrack_foreach(ntype, NULL, PLUGIN_INVALID_HANDLE,
					  plugins);
			xfree(ntype);

			type = strtok_r(NULL, ",", &last);
		}

		xfree(pl);
		xfree(typeslash);
	}

	for (size_t i = 0; i < plugins->count; i++) {
		if (plugins->handles[i] == PLUGIN_INVALID_HANDLE) {
			plugins->handles[i] = plugrack_use_by_type(
				plugins->rack, plugins->types[i]);

			if (plugins->handles[i] == PLUGIN_INVALID_HANDLE) {
				error("%s: unable to find plugin: %s",
				      __func__, plugins->types[i]);
				rc = ESLURM_PLUGIN_INVALID;
				break;
			}
		}
	}

	xrecalloc(plugins->functions, plugins->count,
		  sizeof(*plugins->functions));

	if (!plugins->count || rc)
		goto cleanup;

	for (size_t i = 0; (i < plugins->count); i++) {
		if (plugins->functions[i]) {
			/* already resolved symbols */
			continue;
		}

		if (plugins->handles[i] == PLUGIN_INVALID_HANDLE)
			fatal("Invalid plugin to load?");

		xrecalloc(plugins->functions[i], syms_count + 1, sizeof(void *));

		if (plugin_get_syms(plugins->handles[i], syms_count,
				    syms, plugins->functions[i])
		    < syms_count) {
			rc = ESLURM_PLUGIN_INCOMPLETE;
			break;
		}
	}

cleanup:
	if (!rc)
		*plugins_ptr = plugins;
	else
		unload_plugins(plugins);

	return rc;
}

extern void unload_plugins(plugins_t *plugins)
{
	if (!plugins)
		return;

	if (plugins->rack) {
		for (size_t i = 0; i < plugins->count; i++)
			plugrack_release_by_type(plugins->rack,
						 plugins->types[i]);

		(void) plugrack_destroy(plugins->rack);
	}

	for (size_t i = 0; i < plugins->count; i++) {
		if (plugins->functions)
			xfree(plugins->functions[i]);
		if (plugins->types)
			xfree(plugins->types[i]);
	}

	xfree(plugins->functions);
	xfree(plugins->handles);
	xfree(plugins->types);
	xfree(plugins);
}
