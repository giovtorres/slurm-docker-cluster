/*****************************************************************************\
 *  gpu.c - driver for gpu plugin
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

#include <dlfcn.h>

#include "src/common/assoc_mgr.h"
#include "src/interfaces/gpu.h"
#include "src/common/plugin.h"

/* Gres symbols provided by the plugin */
typedef struct slurm_ops {
	list_t *(*get_system_gpu_list) 	(node_config_load_t *node_conf);
	void	(*step_hardware_init)	(bitstr_t *usable_gpus,
					 char *tres_freq);
	void	(*step_hardware_fini)	(void);
	char   *(*test_cpu_conv)	(char *cpu_range);
	int     (*energy_read)          (uint32_t dv_ind, gpu_status_t *gpu);
	void    (*get_device_count)     (uint32_t *device_count);
	int (*usage_read) (pid_t pid, acct_gather_data_t *data);

} slurm_ops_t;

/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_ops_t.
 */
static const char *syms[] = {
	"gpu_p_get_system_gpu_list",
	"gpu_p_step_hardware_init",
	"gpu_p_step_hardware_fini",
	"gpu_p_test_cpu_conv",
	"gpu_p_energy_read",
	"gpu_p_get_device_count",
	"gpu_p_usage_read",
};

/* Local variables */
static slurm_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock =	PTHREAD_MUTEX_INITIALIZER;
static void *ext_lib_handle = NULL;
/*
 *  Common function to dlopen() the appropriate gpu libraries, and
 *   report back type needed.
 */
static char *_get_gpu_type(void)
{
	/*
	 *  Here we are dlopening the gpu .so to verify it exists on this node.
	 *
	 *  NOTE: We are dlopening these genericly on purpose.  This is to make
	 *  it so we always use the lib that the card is running regardless of
	 *  what was put in configure.  This dlopen is what will load the
	 *  symbols for the plugin to use.
	 *
	 *  We are also doing this outside of the plugins on purpose as we want
	 *  to be able to deal with heterogenous systems where not all the nodes
	 *  will have cards and we want the slurmds to still run there with only
	 *  1 gres.conf file.
	 */
	uint32_t autodetect_flags = gres_get_autodetect_flags();

	if (autodetect_flags & GRES_AUTODETECT_GPU_NVML) {
#ifdef HAVE_NVML
		(void) dlerror();
		if (!(ext_lib_handle = dlopen("libnvidia-ml.so",
					      RTLD_NOW | RTLD_GLOBAL)) &&
		      !(ext_lib_handle = dlopen("libnvidia-ml.so.1",
						RTLD_NOW | RTLD_GLOBAL)))
			info("We were configured with nvml functionality, but that lib wasn't found on the system. Attempted loading libnvidia-ml.so and libnvidia-ml.so.1 without success. Last error is: %s",
			     dlerror());
		else
			return "gpu/nvml";
#else
		info("We were configured to autodetect nvml functionality, but we weren't able to find that lib when Slurm was configured.");
#endif
	} else if (autodetect_flags & GRES_AUTODETECT_GPU_RSMI) {
#ifdef HAVE_RSMI
		(void) dlerror();
		if (!(ext_lib_handle = dlopen("librocm_smi64.so",
					      RTLD_NOW | RTLD_GLOBAL)))
			info("Configured with rsmi, but that lib wasn't found. %s",
			     dlerror());
		else
			return "gpu/rsmi";
#else
		info("Configured with rsmi, but rsmi isn't enabled during the build.");
#endif
	} else if (autodetect_flags & GRES_AUTODETECT_GPU_ONEAPI) {
#ifdef HAVE_ONEAPI
		(void) dlerror();
		if (!(ext_lib_handle = dlopen("libze_loader.so",
					      RTLD_NOW | RTLD_GLOBAL)))
			info("Configured with oneAPI, but that lib wasn't found. %s",
			     dlerror());
		else
			return "gpu/oneapi";
#else
		info("Configured with oneAPI, but oneAPI isn't enabled during the build.");
#endif
	} else if (autodetect_flags & GRES_AUTODETECT_GPU_NRT) {
		return "gpu/nrt";
	} else if (autodetect_flags & GRES_AUTODETECT_GPU_NVIDIA) {
		return "gpu/nvidia";
	}

	return "gpu/generic";
}


/*
 * Initialize the GRES plugins.
 *
 * Returns a Slurm errno.
 */
extern int gpu_plugin_init(void)
{
	int retval = SLURM_SUCCESS;
	char *plugin_type = "gpu";
	char *type = NULL;

	slurm_mutex_lock(&g_context_lock);

	if (g_context)
		goto done;

	type = _get_gpu_type();

	g_context = plugin_context_create(
		plugin_type, type, (void **)&ops, syms, sizeof(syms));

	if (!g_context) {
		error("cannot create %s context for %s", plugin_type, type);
		retval = SLURM_ERROR;
		goto done;
	}

done:
	slurm_mutex_unlock(&g_context_lock);

	return retval;
}

extern int gpu_plugin_fini(void)
{
	int rc;

	if (!g_context)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&g_context_lock);

	if (ext_lib_handle)
		dlclose(ext_lib_handle);

	rc = plugin_context_destroy(g_context);
	g_context = NULL;
	slurm_mutex_unlock(&g_context_lock);

	return rc;
}

extern void gpu_get_tres_pos(int *gpumem_pos, int *gpuutil_pos)
{
	static int loc_gpumem_pos = -1;
	static int loc_gpuutil_pos = -1;
	static bool inited = false;

	if (!inited) {
		slurmdb_tres_rec_t tres_rec;

		memset(&tres_rec, 0, sizeof(slurmdb_tres_rec_t));
		tres_rec.type = "gres";
		tres_rec.name = "gpuutil";
		loc_gpuutil_pos = assoc_mgr_find_tres_pos(&tres_rec, false);
		tres_rec.name = "gpumem";
		loc_gpumem_pos = assoc_mgr_find_tres_pos(&tres_rec, false);
		inited = true;
	}

	if (gpumem_pos)
		*gpumem_pos = loc_gpumem_pos;
	if (gpuutil_pos)
		*gpuutil_pos = loc_gpuutil_pos;
}

extern list_t *gpu_g_get_system_gpu_list(node_config_load_t *node_conf)
{
	xassert(g_context);
	return (*(ops.get_system_gpu_list))(node_conf);
}

extern void gpu_g_step_hardware_init(bitstr_t *usable_gpus, char *tres_freq)
{
	xassert(g_context);
	(*(ops.step_hardware_init))(usable_gpus, tres_freq);
}

extern void gpu_g_step_hardware_fini(void)
{
	xassert(g_context);
	(*(ops.step_hardware_fini))();
}

extern char *gpu_g_test_cpu_conv(char *cpu_range)
{
	xassert(g_context);
	return (*(ops.test_cpu_conv))(cpu_range);

}

extern int gpu_g_energy_read(uint32_t dv_ind, gpu_status_t *gpu)
{
	xassert(g_context);
	return (*(ops.energy_read))(dv_ind, gpu);
}

extern void gpu_g_get_device_count(uint32_t *device_count)
{
	xassert(g_context);
	(*(ops.get_device_count))(device_count);
}

extern int gpu_g_usage_read(pid_t pid, acct_gather_data_t *data)
{
	xassert(g_context);
	return (*(ops.usage_read))(pid, data);

}
