/*****************************************************************************\
 *  opt.c - options processing for salloc
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Copyright (C) SchedMD LLC.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona1@llnl.gov>, et. al.
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

#include "config.h"

#define _GNU_SOURCE

#include <ctype.h>		/* isdigit    */
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>		/* getenv, strtol, etc. */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "src/interfaces/cli_filter.h"
#include "src/common/cpu_frequency.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/parse_time.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h" /* contains getnodename() */
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/slurm_rlimits_info.h"
#include "src/interfaces/acct_gather_profile.h"
#include "src/common/spank.h"
#include "src/common/uid.h"
#include "src/common/x11_util.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/util-net.h"
#include "src/salloc/salloc.h"
#include "src/salloc/opt.h"

static void _help(void);
static void _usage(void);
static void _autocomplete(const char *query);

/*---- global variables, defined in opt.h ----*/
salloc_opt_t saopt;
slurm_opt_t opt = {
	.salloc_opt = &saopt,
	.help_func = _help,
	.usage_func = _usage,
	.autocomplete_func = _autocomplete,
};
int error_exit = 1;
bool first_pass = true;
int immediate_exit = 1;

/*---- forward declarations of static functions  ----*/

typedef struct env_vars env_vars_t;

static void  _opt_env(void);
static void  _opt_args(int argc, char **argv, int het_job_offset);
static bool  _opt_verify(void);
static void  _set_options(int argc, char **argv);

/*---[ end forward declarations of static functions ]---------------------*/

/* process options:
 * 1. set defaults
 * 2. update options with env vars
 * 3. update options with commandline args
 * 4. perform some verification that options are reasonable
 *
 * argc      IN - Count of elements in argv
 * argv      IN - Array of elements to parse
 * argc_off OUT - Offset of first non-parsable element
 * het_job_inx  IN - offset of hetjob
 */
extern int initialize_and_process_args(int argc, char **argv, int *argc_off,
				       int het_job_inx)
{
	/* initialize option defaults */
	slurm_reset_all_options(&opt, first_pass);

	/* cli_filter plugins can change the defaults */
	if (first_pass) {
		if (cli_filter_g_setup_defaults(&opt, false)) {
			error("cli_filter plugin terminated with error");
			exit(error_exit);
		}
	}

	/* initialize options with env vars */
	_opt_env();

	/* initialize options with argv */
	_opt_args(argc, argv, het_job_inx);
	if (argc_off)
		*argc_off = optind;

	if (opt.verbose)
		slurm_print_set_options(&opt);
	first_pass = false;

	return 1;

}

/*
 * If the node list supplied is a file name, translate that into
 *	a list of nodes, we orphan the data pointed to
 * RET true if the node list is a valid one
 */
static bool _valid_node_list(char **node_list_pptr)
{
	int count = NO_VAL;

	/* If we are using Arbitrary and we specified the number of
	   procs to use then we need exactly this many since we are
	   saying, lay it out this way!  Same for max and min nodes.
	   Other than that just read in as many in the hostfile */
	if (opt.ntasks_set)
		count = opt.ntasks;
	else if (opt.nodes_set) {
		if (opt.max_nodes)
			count = opt.max_nodes;
		else if (opt.min_nodes)
			count = opt.min_nodes;
	}

	return verify_node_list(node_list_pptr, opt.distribution, count);
}

/*---[ env var processing ]-----------------------------------------------*/

/*
 * try to use a similar scheme as popt.
 *
 * in order to add a new env var (to be processed like an option):
 *
 * define a new entry into env_vars[], and match to the option used in
 * slurm_opt.c for slurm_process_option()
 */
struct env_vars {
	const char *var;
	int type;
};

env_vars_t env_vars[] = {
  { "SALLOC_ACCOUNT", 'A' },
  { "SALLOC_ACCTG_FREQ", LONG_OPT_ACCTG_FREQ },
  { "SALLOC_BELL", LONG_OPT_BELL },
  { "SALLOC_BURST_BUFFER", LONG_OPT_BURST_BUFFER_SPEC },
  { "SALLOC_CLUSTER_CONSTRAINT", LONG_OPT_CLUSTER_CONSTRAINT },
  { "SALLOC_CLUSTERS", 'M' },
  { "SLURM_CLUSTERS", 'M' },
  { "SALLOC_CONTAINER", LONG_OPT_CONTAINER },
  { "SALLOC_CONTAINER_ID", LONG_OPT_CONTAINER_ID },
  { "SALLOC_CONSTRAINT", 'C' },
  { "SALLOC_CORE_SPEC", 'S' },
  { "SALLOC_CPU_FREQ_REQ", LONG_OPT_CPU_FREQ },
  { "SALLOC_CPUS_PER_GPU", LONG_OPT_CPUS_PER_GPU },
  { "SALLOC_DEBUG", 'v' },
  { "SALLOC_DELAY_BOOT", LONG_OPT_DELAY_BOOT },
  { "SALLOC_EXCLUSIVE", LONG_OPT_EXCLUSIVE },
  { "SALLOC_GPUS", 'G' },
  { "SALLOC_GPU_BIND", LONG_OPT_GPU_BIND },
  { "SALLOC_GPU_FREQ", LONG_OPT_GPU_FREQ },
  { "SALLOC_GPUS_PER_NODE", LONG_OPT_GPUS_PER_NODE },
  { "SALLOC_GPUS_PER_SOCKET", LONG_OPT_GPUS_PER_SOCKET },
  { "SALLOC_GPUS_PER_TASK", LONG_OPT_GPUS_PER_TASK },
  { "SALLOC_GRES", LONG_OPT_GRES },
  { "SALLOC_GRES_FLAGS", LONG_OPT_GRES_FLAGS },
  { "SALLOC_IMMEDIATE", 'I' },
  { "SALLOC_HINT", LONG_OPT_HINT },
  { "SLURM_HINT", LONG_OPT_HINT },
  { "SALLOC_KILL_CMD", 'K' },
  { "SALLOC_MEM_BIND", LONG_OPT_MEM_BIND },
  { "SALLOC_MEM_PER_CPU", LONG_OPT_MEM_PER_CPU },
  { "SALLOC_MEM_PER_GPU", LONG_OPT_MEM_PER_GPU },
  { "SALLOC_MEM_PER_NODE", LONG_OPT_MEM },
  { "SALLOC_NETWORK", LONG_OPT_NETWORK },
  { "SALLOC_NO_BELL", LONG_OPT_NO_BELL },
  { "SALLOC_NO_KILL", 'k' },
  { "SALLOC_OVERCOMMIT", 'O' },
  { "SALLOC_PARTITION", 'p' },
  { "SALLOC_POWER", LONG_OPT_POWER },
  { "SALLOC_PROFILE", LONG_OPT_PROFILE },
  { "SALLOC_QOS", 'q' },
  { "SALLOC_REQ_SWITCH", LONG_OPT_SWITCH_REQ },
  { "SALLOC_RESERVATION", LONG_OPT_RESERVATION },
  { "SALLOC_SIGNAL", LONG_OPT_SIGNAL },
  { "SALLOC_SPREAD_JOB", LONG_OPT_SPREAD_JOB },
  { "SALLOC_THREAD_SPEC", LONG_OPT_THREAD_SPEC },
  { "SALLOC_THREADS_PER_CORE", LONG_OPT_THREADSPERCORE },
  { "SALLOC_TIMELIMIT", 't' },
  { "SALLOC_TRES_BIND", LONG_OPT_TRES_BIND },
  { "SALLOC_TRES_PER_TASK", LONG_OPT_TRES_PER_TASK },
  { "SALLOC_USE_MIN_NODES", LONG_OPT_USE_MIN_NODES },
  { "SALLOC_WAIT_ALL_NODES", LONG_OPT_WAIT_ALL_NODES },
  { "SALLOC_WAIT4SWITCH", LONG_OPT_SWITCH_WAIT },
  { "SALLOC_WCKEY", LONG_OPT_WCKEY },
  { NULL }
};


/*
 * _opt_env(): used by initialize_and_process_args to set options via
 *            environment variables. See comments above for how to
 *            extend srun to process different vars
 */
static void _opt_env(void)
{
	char       *val = NULL;
	env_vars_t *e   = env_vars;

	while (e->var) {
		if ((val = getenv(e->var)) != NULL)
			slurm_process_option_or_exit(&opt, e->type, val, true,
						     false);
		e++;
	}

	/* Process spank env options */
	if (spank_process_env_options())
		exit(error_exit);
}

static void _set_options(int argc, char **argv)
{
	char *opt_string = NULL;
	int opt_char, option_index = 0;
	struct option *optz = slurm_option_table_create(&opt, &opt_string);

	opt.submit_line = slurm_option_get_argv_str(argc, argv);

	optind = 0;
	while ((opt_char = getopt_long(argc, argv, opt_string,
				       optz, &option_index)) != -1) {
		slurm_process_option_or_exit(&opt, opt_char, optarg, false,
					     false);
	}

	slurm_option_table_destroy(optz);
	xfree(opt_string);
}

/*
 * _opt_args() : set options via commandline args and popt
 */
static void _opt_args(int argc, char **argv, int het_job_offset)
{
	int i;
	char **rest = NULL;

	_set_options(argc, argv);

	if ((optind < argc) && !xstrcmp(argv[optind], ":")) {
		debug("hetjob component separator");
	} else {
		opt.argc = 0;
		if (optind < argc) {
			rest = argv + optind;
			while (rest[opt.argc] != NULL)
				opt.argc++;
		}
		opt.argv = (char **) xmalloc((opt.argc + 1) * sizeof(char *));
		for (i = 0; i < opt.argc; i++) {
			if ((i == 0) && (rest == NULL))
				break;	/* Fix for CLANG false positive */
			opt.argv[i] = xstrdup(rest[i]);
		}
		opt.argv[i] = NULL; /* End of argv's (for possible execv) */
	}

	if (opt.container &&
	    !xstrstr(slurm_conf.launch_params, "use_interactive_step")) {
		error("--container requires LaunchParameters=use_interactive_step");
		exit(error_exit);
	}

	if (cli_filter_g_pre_submit(&opt, het_job_offset)) {
		error("cli_filter plugin terminated with error");
		exit(error_exit);
	}

	if (!_opt_verify())
		exit(error_exit);
}

/* return a string containing the default shell for this user */
static char *_get_shell(void)
{
	uid_t uid = opt.uid;
	char *shell;

	if (uid == SLURM_AUTH_NOBODY)
		uid = getuid();

	if (!(shell = uid_to_shell(uid)))
		fatal("no user information for user %u", uid);

	return shell;
}

static void _salloc_default_command(int *argcp, char **argvp[])
{
	if (xstrstr(slurm_conf.launch_params, "use_interactive_step")) {
		/*
		 * Use srun out of the same directory as this process.
		 */
		char *command, *pos;
		command = xstrdup(argvzero);
		if ((pos = xstrrchr(command, '/')))
			*(++pos) = '\0';
		else
			*command = '\0';
		xstrcat(command, "srun ");

		/* Explicitly pass container if requested */
		if (opt.container) {
			int len = strlen(opt.container);

			xstrcat(command, " --container '");
			/* escape any single quotes if they exist */

			for (int i = 0; i < len; i++) {
				if (opt.container[i] == '\'')
					xstrcat(command, "'\"'\"'");
				else
					xstrcatchar(command, opt.container[i]);
			}

			xstrcat(command, "' ");
		}

		xstrcat(command, slurm_conf.interactive_step_opts);

		*argcp = 3;
		*argvp = xcalloc(4, sizeof (char *));
		(*argvp)[0] = "/bin/sh";
		(*argvp)[1] = "-c";
		(*argvp)[2] = command;
		(*argvp)[3] = NULL;
	} else {
		*argcp = 1;
		*argvp = xmalloc(sizeof (char *) * 2);
		(*argvp)[0] = _get_shell();
		(*argvp)[1] = NULL;
	}
}

/*
 * _opt_verify : perform some post option processing verification
 *
 */
static bool _opt_verify(void)
{
	bool verified = true;
	hostlist_t *hl = NULL;
	int hl_cnt = 0;

	validate_options_salloc_sbatch_srun(&opt);

	if (opt.quiet && opt.verbose) {
		error ("don't specify both --verbose (-v) and --quiet (-Q)");
		verified = false;
	}

	if ((opt.resv_port_cnt != NO_VAL) &&
	    !(opt.job_flags & STEPMGR_ENABLED) &&
	    !xstrstr(slurm_conf.slurmctld_params, "enable_stepmgr")) {
		error("Slurmstepd step management must be enabled to use --resv-ports for job allocations");
		verified = false;
	}

	if (opt.burst_buffer && opt.burst_buffer_file) {
		error("Cannot specify both --bb and --bbf");
		exit(error_exit);
	} else if (opt.burst_buffer_file) {
		buf_t *buf = create_mmap_buf(opt.burst_buffer_file);
		if (!buf) {
			error("Invalid --bbf specification");
			exit(error_exit);
		}
		opt.burst_buffer = xstrdup(get_buf_data(buf));
		FREE_NULL_BUFFER(buf);
		xfree(opt.burst_buffer_file);
	}

	if (opt.container && !getenv("SLURM_CONTAINER"))
		setenvf(NULL, "SLURM_CONTAINER", "%s", opt.container);
	if (opt.container_id && !getenv("SLURM_CONTAINER_ID"))
		setenvf(NULL, "SLURM_CONTAINER_ID", "%s", opt.container_id);

	if (opt.hint &&
	    !validate_hint_option(&opt)) {
		xassert(opt.ntasks_per_core == NO_VAL);
		xassert(opt.threads_per_core == NO_VAL);
		if (verify_hint(opt.hint,
				&opt.sockets_per_node,
				&opt.cores_per_socket,
				&opt.threads_per_core,
				&opt.ntasks_per_core,
				NULL)) {
			exit(error_exit);
		}
	}

	if (opt.exclude && !_valid_node_list(&opt.exclude))
		exit(error_exit);

	if (opt.nodelist && !opt.nodes_set && !xstrchr(opt.nodelist, '{')) {
		hl = hostlist_create(opt.nodelist);
		if (!hl)
			fatal("Invalid node list specified");
		hostlist_uniq(hl);
		hl_cnt = hostlist_count(hl);
		opt.min_nodes = hl_cnt;
		opt.nodes_set = true;
	}

	if (opt.cpus_set && (opt.pn_min_cpus < opt.cpus_per_task))
		opt.pn_min_cpus = opt.cpus_per_task;

	/* Set the env var so that the spawned srun can set it */
	if (opt.oom_kill_step != NO_VAL16 && !getenv("SLURM_OOM_KILL_STEP"))
		setenvf(NULL, "SLURM_OOM_KILL_STEP", "%u", opt.oom_kill_step);

	if ((saopt.no_shell == false) && (opt.argc == 0))
		_salloc_default_command(&opt.argc, &opt.argv);

	/* check for realistic arguments */
	if (opt.ntasks <= 0) {
		error("invalid number of tasks (-n %d)",
		      opt.ntasks);
		verified = false;
	}

	if (opt.cpus_set && (opt.cpus_per_task <= 0)) {
		error("invalid number of cpus per task (-c %d)",
		      opt.cpus_per_task);
		verified = false;
	}

	if ((opt.min_nodes < 0) || (opt.max_nodes < 0) ||
	    (opt.max_nodes && (opt.min_nodes > opt.max_nodes))) {
		error("invalid number of nodes (-N %d-%d)",
		      opt.min_nodes, opt.max_nodes);
		verified = false;
	}

        /* Check to see if user has specified enough resources to
	 * satisfy the plane distribution with the specified
	 * plane_size.
	 * if (n/plane_size < N) and ((N-1) * plane_size >= n) -->
	 * problem Simple check will not catch all the problem/invalid
	 * cases.
	 * The limitations of the plane distribution in the cons_tres
	 * environment are more extensive and are documented in the
	 * Slurm reference guide.  */
	if ((opt.distribution & SLURM_DIST_STATE_BASE) == SLURM_DIST_PLANE &&
	    opt.plane_size) {
		if ((opt.ntasks/opt.plane_size) < opt.min_nodes) {
			if (((opt.min_nodes-1)*opt.plane_size) >= opt.ntasks) {
#if (0)
				info("Too few processes ((n/plane_size) %d < N %d) "
				     "and ((N-1)*(plane_size) %d >= n %d)) ",
				     opt.ntasks/opt.plane_size, opt.min_nodes,
				     (opt.min_nodes-1)*opt.plane_size,
				     opt.ntasks);
#endif
				error("Too few processes for the requested "
				      "{plane,node} distribution");
				exit(error_exit);
			}
		}
	}

	/* massage the numbers */
	if ((opt.nodes_set || opt.extra_set)				&&
	    ((opt.min_nodes == opt.max_nodes) || (opt.max_nodes == 0))	&&
	    (opt.ntasks_per_node == NO_VAL) &&
	    !opt.ntasks_set) {
		/* 1 proc / node default */
		opt.ntasks = opt.min_nodes;

		/* 1 proc / min_[socket * core * thread] default */
		if (opt.sockets_per_node != NO_VAL) {
			opt.ntasks *= opt.sockets_per_node;
			opt.ntasks_set = true;
		}
		if (opt.cores_per_socket != NO_VAL) {
			opt.ntasks *= opt.cores_per_socket;
			opt.ntasks_set = true;
		}
		if (opt.threads_per_core != NO_VAL) {
			opt.ntasks *= opt.threads_per_core;
			opt.ntasks_set = true;
		}
		if (opt.ntasks_set && opt.verbose)
			info("Number of tasks implicitly set to %d",
			     opt.ntasks);

	} else if (opt.nodes_set && opt.ntasks_set) {
		/*
		 * Make sure that the number of
		 * max_nodes is <= number of tasks
		 */
		if (opt.ntasks < opt.max_nodes)
			opt.max_nodes = opt.ntasks;

		/*
		 *  make sure # of procs >= min_nodes
		 */
		if (opt.ntasks < opt.min_nodes) {
			warning("can't run %d processes on %d nodes, setting nnodes to %d",
				opt.ntasks, opt.min_nodes, opt.ntasks);

			opt.min_nodes = opt.max_nodes = opt.ntasks;
		}

	} /* else if (opt.ntasks_set && !opt.nodes_set) */

	/* set up the proc and node counts based on the arbitrary list
	   of nodes */
	if (((opt.distribution & SLURM_DIST_STATE_BASE) == SLURM_DIST_ARBITRARY)
	    && (!opt.nodes_set || !opt.ntasks_set)
	    && !xstrchr(opt.nodelist, '{')) {
		FREE_NULL_HOSTLIST(hl);
		hl = hostlist_create(opt.nodelist);
		if (!hl)
			fatal("Invalid node list specified");
		if (!opt.ntasks_set) {
			opt.ntasks_set = 1;
			opt.ntasks = hostlist_count(hl);
		}
		if (!opt.nodes_set) {
			opt.nodes_set = 1;
			hostlist_uniq(hl);
			opt.min_nodes = opt.max_nodes = hostlist_count(hl);
		}
	}

	FREE_NULL_HOSTLIST(hl);

	if ((opt.deadline) && (opt.begin) && (opt.deadline < opt.begin)) {
		error("Incompatible begin and deadline time specification");
		exit(error_exit);
	}

	if (opt.mem_bind_type && (getenv("SLURM_MEM_BIND") == NULL)) {
		char *tmp = slurm_xstr_mem_bind_type(opt.mem_bind_type);
		if (opt.mem_bind) {
			setenvf(NULL, "SLURM_MEM_BIND", "%s:%s",
				tmp, opt.mem_bind);
		} else {
			setenvf(NULL, "SLURM_MEM_BIND", "%s", tmp);
		}
		xfree(tmp);
	}
	if (opt.mem_bind_type && (getenv("SLURM_MEM_BIND_SORT") == NULL) &&
	    (opt.mem_bind_type & MEM_BIND_SORT)) {
		setenvf(NULL, "SLURM_MEM_BIND_SORT", "sort");
	}

	if (opt.mem_bind_type && (getenv("SLURM_MEM_BIND_VERBOSE") == NULL)) {
		if (opt.mem_bind_type & MEM_BIND_VERBOSE) {
			setenvf(NULL, "SLURM_MEM_BIND_VERBOSE", "verbose");
		} else {
			setenvf(NULL, "SLURM_MEM_BIND_VERBOSE", "quiet");
		}
	}

	if ((opt.ntasks_per_core > 0) &&
	    (getenv("SLURM_NTASKS_PER_CORE") == NULL)) {
		setenvf(NULL, "SLURM_NTASKS_PER_CORE", "%d",
			opt.ntasks_per_core);
		if ((opt.threads_per_core !=  NO_VAL) &&
		    (opt.threads_per_core < opt.ntasks_per_core)) {
			error("--ntasks-per-core (%d) can not be bigger than --threads-per-core (%d)",
			opt.ntasks_per_core, opt.threads_per_core);
			verified = false;
		}
	}

	if ((opt.ntasks_per_gpu != NO_VAL) &&
	    (getenv("SLURM_NTASKS_PER_GPU") == NULL)) {
		setenvf(NULL, "SLURM_NTASKS_PER_GPU", "%d",
			opt.ntasks_per_gpu);
	}

	if ((opt.ntasks_per_node > 0) &&
	    (getenv("SLURM_NTASKS_PER_NODE") == NULL)) {
		setenvf(NULL, "SLURM_NTASKS_PER_NODE", "%d",
			opt.ntasks_per_node);
	}

	if ((opt.ntasks_per_socket > 0) &&
	    (getenv("SLURM_NTASKS_PER_SOCKET") == NULL)) {
		setenvf(NULL, "SLURM_NTASKS_PER_SOCKET", "%d",
			opt.ntasks_per_socket);
	}

	if ((opt.ntasks_per_tres != NO_VAL) &&
	    (getenv("SLURM_NTASKS_PER_TRES") == NULL)) {
		setenvf(NULL, "SLURM_NTASKS_PER_TRES", "%d",
			opt.ntasks_per_tres);
	}

	if (opt.profile)
		setenvfs("SLURM_PROFILE=%s",
			 acct_gather_profile_to_string(opt.profile));

	cpu_freq_set_env("SLURM_CPU_FREQ_REQ",
			opt.cpu_freq_min, opt.cpu_freq_max, opt.cpu_freq_gov);

	if ((saopt.wait_all_nodes == NO_VAL16) &&
	    (xstrcasestr(slurm_conf.sched_params, "salloc_wait_nodes")))
			saopt.wait_all_nodes = 1;

	if (opt.x11) {
		x11_get_display(&opt.x11_target_port, &opt.x11_target);
		opt.x11_magic_cookie = x11_get_xauth();
	}

	if (saopt.no_shell && !opt.job_name)
		opt.job_name = xstrdup("no-shell");

	if (!opt.job_name)
		opt.job_name = xstrdup("interactive");

	return verified;
}

/* Functions used by SPANK plugins to read and write job environment
 * variables for use within job's Prolog and/or Epilog */
extern char *spank_get_job_env(const char *name)
{
	int i, len;
	char *tmp_str = NULL;

	if ((name == NULL) || (name[0] == '\0') ||
	    (strchr(name, (int)'=') != NULL)) {
		errno = EINVAL;
		return NULL;
	}

	xstrcat(tmp_str, name);
	xstrcat(tmp_str, "=");
	len = strlen(tmp_str);

	for (i=0; i<opt.spank_job_env_size; i++) {
		if (xstrncmp(opt.spank_job_env[i], tmp_str, len))
			continue;
		xfree(tmp_str);
		return (opt.spank_job_env[i] + len);
	}

	return NULL;
}

extern int   spank_set_job_env(const char *name, const char *value,
			       int overwrite)
{
	int i, len;
	char *tmp_str = NULL;

	if ((name == NULL) || (name[0] == '\0') ||
	    (strchr(name, (int)'=') != NULL)) {
		errno = EINVAL;
		return -1;
	}

	xstrcat(tmp_str, name);
	xstrcat(tmp_str, "=");
	len = strlen(tmp_str);
	xstrcat(tmp_str, value);

	for (i=0; i<opt.spank_job_env_size; i++) {
		if (xstrncmp(opt.spank_job_env[i], tmp_str, len))
			continue;
		if (overwrite) {
			xfree(opt.spank_job_env[i]);
			opt.spank_job_env[i] = tmp_str;
		} else
			xfree(tmp_str);
		return 0;
	}

	/* Need to add an entry */
	opt.spank_job_env_size++;
	xrealloc(opt.spank_job_env, sizeof(char *) * opt.spank_job_env_size);
	opt.spank_job_env[i] = tmp_str;
	return 0;
}

extern int   spank_unset_job_env(const char *name)
{
	int i, j, len;
	char *tmp_str = NULL;

	if ((name == NULL) || (name[0] == '\0') ||
	    (strchr(name, (int)'=') != NULL)) {
		errno = EINVAL;
		return -1;
	}

	xstrcat(tmp_str, name);
	xstrcat(tmp_str, "=");
	len = strlen(tmp_str);

	for (i=0; i<opt.spank_job_env_size; i++) {
		if (xstrncmp(opt.spank_job_env[i], tmp_str, len))
			continue;
		xfree(opt.spank_job_env[i]);
		for (j=(i+1); j<opt.spank_job_env_size; i++, j++)
			opt.spank_job_env[i] = opt.spank_job_env[j];
		opt.spank_job_env_size--;
		if (opt.spank_job_env_size == 0)
			xfree(opt.spank_job_env);
		return 0;
	}

	return 0;	/* not found */
}

static void _autocomplete(const char *query)
{
	char *opt_string = NULL;
	struct option *optz = slurm_option_table_create(&opt, &opt_string);

	suggest_completion(optz, query);

	xfree(opt_string);
	slurm_option_table_destroy(optz);
}

static void _usage(void)
{
 	printf(
"Usage: salloc [-N numnodes|[min nodes]-[max nodes]] [-n num-processors]\n"
"              [-c cpus-per-node] [-r n] [-p partition] [--hold] [-t minutes]\n"
"              [--immediate[=secs]] [--no-kill] [--overcommit] [-D path]\n"
"              [--oversubscribe] [-J jobname] [--verbose] [--licenses=names]\n"
"              [--clusters=cluster_names]\n"
"              [--contiguous] [--mincpus=n] [--mem=MB] [--tmp=MB] [-C list]\n"
"              [--account=name] [--dependency=type:jobid[+time]] [--comment=name]\n"
"              [--mail-type=type] [--mail-user=user] [--nice[=value]]\n"
"              [--bell] [--no-bell] [--kill-command[=signal]] [--spread-job]\n"
"              [--nodefile=file] [--nodelist=hosts] [--exclude=hosts]\n"
"              [--network=type] [--mem-per-cpu=MB] [--qos=qos]\n"
"              [--mem-bind=...] [--reservation=name] [--mcs-label=mcs]\n"
"              [--time-min=minutes] [--gres=list] [--gres-flags=opts]\n"
"              [--cpu-freq=min[-max[:gov]]] [--power=flags] [--profile=...]\n"
"              [--switches=max-switches[@max-time-to-wait]]\n"
"              [--core-spec=cores] [--thread-spec=threads] [--reboot]\n"
"              [--bb=burst_buffer_spec] [--bbf=burst_buffer_file]\n"
"              [--delay-boot=mins] [--use-min-nodes]\n"
"              [--cpus-per-gpu=n] [--gpus=n] [--gpu-bind=...] [--gpu-freq=...]\n"
"              [--gpus-per-node=n] [--gpus-per-socket=n] [--gpus-per-task=n]\n"
"              [--mem-per-gpu=MB] [--tres-bind=...] [--tres-per-task=list]\n"
"              [--oom-kill-step[=0|1]]\n"
"              [command [args...]]\n");
}

static void _help(void)
{
	slurm_conf_t *conf;

        printf (
"Usage: salloc [OPTIONS(0)...] [ : [OPTIONS(N)]] [command(0) [args(0)...]]\n"
"\n"
"Parallel run options:\n"
"  -A, --account=name          charge job to specified account\n"
"  -b, --begin=time            defer job until HH:MM MM/DD/YY\n"
"      --bell                  ring the terminal bell when the job is allocated\n"
"      --bb=<spec>             burst buffer specifications\n"
"      --bbf=<file_name>       burst buffer specification file\n"
"  -c, --cpus-per-task=ncpus   number of cpus required per task\n"
"      --comment=name          arbitrary comment\n"
"      --container             Path to OCI container bundle\n"
"      --container-id          OCI container ID\n"
"      --cpu-freq=min[-max[:gov]] requested cpu frequency (and governor)\n"
"      --delay-boot=mins       delay boot for desired node features\n"
"  -d, --dependency=type:jobid[:time] defer job until condition on jobid is satisfied\n"
"      --deadline=time         remove the job if no ending possible before\n"
"                              this deadline (start > (deadline - time[-min]))\n"
"  -D, --chdir=path            change working directory\n"
"      --get-user-env          used by Moab.  See srun man page.\n"
"      --gres=list             required generic resources\n"
"      --gres-flags=opts       flags related to GRES management\n"
"  -H, --hold                  submit job in held state\n"
"  -I, --immediate[=secs]      exit if resources not available in \"secs\"\n"
"  -J, --job-name=jobname      name of job\n"
"  -k, --no-kill               do not kill job on node failure\n"
"  -K, --kill-command[=signal] signal to send terminating job\n"
"  -L, --licenses=names        required license, comma separated\n"
"  -M, --clusters=names        Comma separated list of clusters to issue\n"
"                              commands to.  Default is current cluster.\n"
"                              Name of 'all' will submit to run on all clusters.\n"
"                              NOTE: SlurmDBD must up.\n"
"  -m, --distribution=type     distribution method for processes to nodes\n"
"                              (type = block|cyclic|arbitrary)\n"
"      --mail-type=type        notify on state change: BEGIN, END, FAIL or ALL\n"
"      --mail-user=user        who to send email notification for job state\n"
"                              changes\n"
"      --mcs-label=mcs         mcs label if mcs plugin mcs/group is used\n"
"  -n, --ntasks=N              number of processors required\n"
"      --nice[=value]          decrease scheduling priority by value\n"
"      --no-bell               do NOT ring the terminal bell\n"
"      --ntasks-per-node=n     number of tasks to invoke on each node\n"
"  -N, --nodes=N               number of nodes on which to run (N = min[-max])\n"
"      --oom-kill-step[=0|1]   set the OOMKillStep behaviour\n"
"  -O, --overcommit            overcommit resources\n"
"      --power=flags           power management options\n"
"      --priority=value        set the priority of the job to value\n"
"      --profile=value         enable acct_gather_profile for detailed data\n"
"                              value is all or none or any combination of\n"
"                              energy, lustre, network or task\n"
"  -p, --partition=partition   partition requested\n"
"  -q, --qos=qos               quality of service\n"
"  -Q, --quiet                 quiet mode (suppress informational messages)\n"
"      --reboot                reboot compute nodes before starting job\n"
"  -s, --oversubscribe         oversubscribe resources with other jobs\n"
"      --signal=[R:]num[@time] send signal when time limit within time seconds\n"
"      --spread-job            spread job across as many nodes as possible\n"
"      --switches=max-switches{@max-time-to-wait}\n"
"                              Optimum switches and max time to wait for optimum\n"
"  -S, --core-spec=cores       count of reserved cores\n"
"      --thread-spec=threads   count of reserved threads\n"
"  -t, --time=minutes          time limit\n"
"      --time-min=minutes      minimum time limit (if distinct)\n"
"      --tres-bind=...         task to tres binding options\n"
"      --tres-per-task=list    list of tres required per task\n"
"      --use-min-nodes         if a range of node counts is given, prefer the\n"
"                              smaller count\n"
"  -v, --verbose               verbose mode (multiple -v's increase verbosity)\n"
"      --wckey=wckey           wckey to run job under\n"
"\n"
"Constraint options:\n"
"      --cluster-constraint=list specify a list of cluster constraints\n"
"      --contiguous            demand a contiguous range of nodes\n"
"  -C, --constraint=list       specify a list of constraints\n"
"  -F, --nodefile=filename     request a specific list of hosts\n"
"      --mem=MB                minimum amount of real memory\n"
"      --mincpus=n             minimum number of logical processors (threads)\n"
"                              per node\n"
"      --reservation=name      allocate resources from named reservation\n"
"      --tmp=MB                minimum amount of temporary disk\n"
"  -w, --nodelist=hosts...     request a specific list of hosts\n"
"  -x, --exclude=hosts...      exclude a specific list of hosts\n"
"\n"
"Consumable resources related options:\n"
"      --exclusive[=user]      allocate nodes in exclusive mode when\n"
"                              cpu consumable resource is enabled\n"
"      --exclusive[=mcs]       allocate nodes in exclusive mode when\n"
"                              cpu consumable resource is enabled\n"
"                              and mcs plugin is enabled\n"
"      --mem-per-cpu=MB        maximum amount of real memory per allocated\n"
"                              cpu required by the job.\n"
"                              --mem >= --mem-per-cpu if --mem is specified.\n"
"      --resv-ports            reserve communication ports\n"
"\n"
"Affinity/Multi-core options: (when the task/affinity plugin is enabled)\n"
"                              For the following 4 options, you are\n"
"                              specifying the minimum resources available for\n"
"                              the node(s) allocated to the job.\n"
"      --sockets-per-node=S    number of sockets per node to allocate\n"
"      --cores-per-socket=C    number of cores per socket to allocate\n"
"      --threads-per-core=T    number of threads per core to allocate\n"
"  -B, --extra-node-info=S[:C[:T]]  combine request of sockets per node,\n"
"                              cores per socket and threads per core.\n"
"                              Specify an asterisk (*) as a placeholder,\n"
"                              a minimum value, or a min-max range.\n"
"\n"
"      --ntasks-per-core=n     number of tasks to invoke on each core\n"
"      --ntasks-per-socket=n   number of tasks to invoke on each socket\n");
	conf = slurm_conf_lock();
	if (xstrstr(conf->task_plugin, "affinity")) {
		printf(
"      --hint=                 Bind tasks according to application hints\n"
"                              (see \"--hint=help\" for options)\n"
"      --mem-bind=             Bind memory to locality domains (ldom)\n"
"                              (see \"--mem-bind=help\" for options)\n");
	}
	slurm_conf_unlock();

	printf("\n"
"GPU scheduling options:\n"
"      --cpus-per-gpu=n        number of CPUs required per allocated GPU\n"
"  -G, --gpus=n                count of GPUs required for the job\n"
"      --gpu-bind=...          task to gpu binding options\n"
"      --gpu-freq=...          frequency and voltage of GPUs\n"
"      --gpus-per-node=n       number of GPUs required per allocated node\n"
"      --gpus-per-socket=n     number of GPUs required per allocated socket\n"
"      --gpus-per-task=n       number of GPUs required per spawned task\n"
"      --mem-per-gpu=n         real memory required per allocated GPU\n"
		);
	spank_print_options(stdout, 6, 30);

	printf("\n"
"\n"
"Help options:\n"
"  -h, --help                  show this help message\n"
"      --usage                 display brief usage message\n"
"\n"
"Other options:\n"
"  -V, --version               output version information and exit\n"
"\n"
		);
}
