/*****************************************************************************\
 *  slurmscriptd.h - Definitions of functions and structures for slurmscriptd.
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

#ifndef _HAVE_SLURMSCRIPTD_H
#define _HAVE_SLURMSCRIPTD_H

#include <unistd.h>

#define SLURMSCRIPTD_MODE_ENV "SLURMSCRIPTD_MODE"
#define SLURMSCRIPT_READ_FD (STDERR_FILENO + 1)
#define SLURMSCRIPT_WRITE_FD (STDERR_FILENO + 2)
#define SLURMSCRIPT_CLOSEALL (STDERR_FILENO + 3)

/*
 * Run the the slurmscriptd main loop.
 * Does not return - calls exit.
 */
__attribute__((noreturn))
extern void slurmscriptd_run_slurmscriptd(int argc, char **argv,
					  char *binary_path);

extern int slurmscriptd_init(char **argv, char *binary_path);

extern int slurmscriptd_fini(void);

/*
 * slurmscriptd_flush - kill all running scripts.
 *
 * This function blocks until slurmscriptd responds that it is finished.
 */
extern void slurmscriptd_flush(void);

/*
 * slurmscriptd_flush_job - kill all running script for a specific job
 */
extern void slurmscriptd_flush_job(uint32_t job_id);

/*
 * Run a burst_buffer.lua script specified by command line arguments and
 * environment variables. This function calls exit() instead of returning.
 */
extern void slurmscriptd_handle_bb_lua_mode(int argc, char **argv);

/*
 * slurmscriptd_run_bb_lua
 * Tell slurmscriptd to run a specific function in the script in the
 * burst_buffer/lua plugin
 * IN job_id - the job for which we're running the script
 * IN function - the function in the lua script we should run
 * IN argc - number of arguments
 * IN argv - arguments for the script
 * IN timeout - timeout in seconds
 * IN job_buf - packed job info, or NULL
 * OUT resp - response message from the script
 * OUT track_script_signalled - true if the script was killed by track_script,
 *                              false otherwise.
 * RET return code of the script or SLURM_ERROR if there was some other failure
 */
extern int slurmscriptd_run_bb_lua(uint32_t job_id, char *function,
				   uint32_t argc, char **argv, uint32_t timeout,
				   buf_t *job_buf, char **resp,
				   bool *track_script_signalled);

extern int slurmscriptd_run_mail(char *script_path, uint32_t argc, char **argv,
				 char **env, uint32_t timeout, char **resp);

/*
 * slurmscriptd_run_power
 * Run a power script in slurmscriptd
 * IN script_path - fulle path to the script
 * IN hosts - Slurm hostlist expression to pass to the script
 * IN features - node features to pass to the script
 * IN job_id - job id for the script (may be zero if not applicable)
 * IN script_name - description of the script
 * IN timeout - timeout in seconds
 * IN tmp_file_env_name - name of the environment variable in which the path of
 *                        the temporary file is stored
 * IN tmp_file_str - data to put in the temporary file
 */
extern void slurmscriptd_run_power(char *script_path, char *hosts,
				   char *features, uint32_t job_id,
				   char *script_name, uint32_t timeout,
				   char *tmp_file_env_name, char *tmp_file_str);

extern int slurmscriptd_run_reboot(char *script_path, uint32_t argc,
				   char **argv);

extern void slurmscriptd_run_resv(char *script_path, uint32_t argc, char **argv,
				  uint32_t timeout, char *script_name);

/*
 * slurmscriptd_run_prepilog
 * Tell slurmscriptd to run PrologSlurmctld or EpilogSlurmctld for the job
 * IN job_id - Job that wants to run the script
 * IN is_epilog - True if the EpilogSlurmctld should run; false if the
 *                PrologSlurmctld should run
 * IN script - Full path to the script that needs to run
 * IN env - Environment to pass to the script
 */
extern void slurmscriptd_run_prepilog(uint32_t job_id, bool is_epilog,
				      char *script, char **env);

/*
 * slurmscriptd_update_debug_flags
 * Update the debug flags for slurmscriptd.
 */
extern void slurmscriptd_update_debug_flags(uint64_t debug_flags);

/*
 * slurmscriptd_update_log_level
 * Update the logging level for slurmscriptd.
 *
 * IN debug_level
 * IN log_rotate - true if log_rotate (re-open log files)
 */
extern void slurmscriptd_update_log_level(int debug_level, bool log_rotate);

#endif /* !_HAVE_SLURMSCRIPTD_H */
