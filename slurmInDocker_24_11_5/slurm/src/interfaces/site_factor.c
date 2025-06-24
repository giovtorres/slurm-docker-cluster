/*****************************************************************************\
 *  site_factor.c - site priority factor driver
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

#include "src/common/plugin.h"
#include "src/interfaces/site_factor.h"
#include "src/common/timers.h"

#define SITE_FACTOR_TIMER 50000			/* 50 milliseconds */
#define SITE_FACTOR_TIMER_RECONFIG 500000	/* 500 milliseconds */

/* Symbols provided by the plugin */
typedef struct slurm_ops {
	void	(*set)		(job_record_t *job_ptr);
	void	(*update)	(void);
} slurm_ops_t;

/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_ops_t.
 */
static const char *syms[] = {
	"site_factor_p_set",
	"site_factor_p_update",
};

/* Local variables */
static slurm_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock =	PTHREAD_MUTEX_INITIALIZER;
static plugin_init_t plugin_inited = PLUGIN_NOT_INITED;

/*
 * Initialize the site_factor plugin.
 *
 * Returns a Slurm errno.
 */
extern int site_factor_g_init(void)
{
	int retval = SLURM_SUCCESS;
	char *plugin_type = "site_factor";

	slurm_mutex_lock(&g_context_lock);

	if (plugin_inited)
		goto done;

	if (!slurm_conf.site_factor_plugin) {
		plugin_inited = PLUGIN_NOOP;
		goto done;
	}

	g_context = plugin_context_create(plugin_type,
					  slurm_conf.site_factor_plugin,
					  (void **) &ops, syms, sizeof(syms));

	if (!g_context) {
		error("cannot create %s context for %s",
		      plugin_type, slurm_conf.site_factor_plugin);
		retval = SLURM_ERROR;
		plugin_inited = PLUGIN_NOT_INITED;
		goto done;
	}

	debug2("%s: plugin %s loaded", __func__, slurm_conf.site_factor_plugin);

	plugin_inited = PLUGIN_INITED;
done:
	slurm_mutex_unlock(&g_context_lock);

	return retval;
}

extern int site_factor_g_fini(void)
{
	int rc = SLURM_SUCCESS;

	slurm_mutex_lock(&g_context_lock);
	if (g_context) {
		rc = plugin_context_destroy(g_context);
		g_context = NULL;
	}
	plugin_inited = PLUGIN_NOT_INITED;
	slurm_mutex_unlock(&g_context_lock);

	return rc;
}

extern void site_factor_g_set(job_record_t *job_ptr)
{
	DEF_TIMERS;

	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return;

	START_TIMER;
	(*(ops.set))(job_ptr);
	END_TIMER3(__func__, SITE_FACTOR_TIMER);
}

extern void site_factor_g_update(void)
{
	DEF_TIMERS;

	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return;

	START_TIMER;
	(*(ops.update))();
	END_TIMER3(__func__, SITE_FACTOR_TIMER);
}
