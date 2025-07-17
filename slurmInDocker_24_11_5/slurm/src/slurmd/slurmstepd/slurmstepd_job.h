/*****************************************************************************\
 *  src/slurmd/slurmstepd/slurmstepd_job.h  stepd_step_rec_t definition
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Copyright (C) 2013      Intel, Inc.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
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

#ifndef _SLURMSTEPD_JOB_H
#define _SLURMSTEPD_JOB_H

#ifdef __FreeBSD__
#include <sys/cpuset.h>
typedef cpuset_t cpu_set_t;
#endif

#define _GNU_SOURCE

#include <pthread.h>
#include <pwd.h>

#include "src/common/data.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/list.h"
#include "src/common/eio.h"
#include "src/common/env.h"
#include "src/common/stepd_api.h"

#define STEP_CONTAINER_MAGIC 0xa0b9b2ba

typedef struct {
	char *key;                 /* srun key for IO verification         */
	slurm_addr_t resp_addr;	   /* response addr for task exit msg      */
	slurm_addr_t ioaddr;       /* Address to connect on for normal I/O.
				      Spawn IO uses messages to the normal
				      resp_addr. */
	uid_t uid;		   /* user id for job */
	uint16_t protocol_version; /* protocol_version of the srun */
} srun_info_t;

typedef enum {
	STEPD_STEP_TASK_INIT,
	STEPD_STEP_TASK_STARTING,
	STEPD_STEP_TASK_RUNNING,
	STEPD_STEP_TASK_COMPLETE
} stepd_step_task_state_t;

typedef struct {
	pthread_mutex_t mutex;	    /* mutex to protect task state          */
	stepd_step_task_state_t state;  /* task state                       */
	cpu_set_t *cpu_set;

	int             id;	    /* local task id                        */
	uint32_t        gtid;	    /* global task id                       */
	pid_t           pid;	    /* task pid                             */

	char           *ifname;     /* standard input file name             */
	char           *ofname;     /* standard output file name            */
	char           *efname;     /* standard error file name             */
	int             stdin_fd;   /* standard input file descriptor       */
	int             stdout_fd;  /* standard output file descriptor      */
	int             stderr_fd;  /* standard error file descriptor       */
	int             to_stdin;   /* write file descriptor for task stdin */
	int             from_stdout;/* read file descriptor from task stdout*/
	int             from_stderr;/* read file descriptor from task stderr*/
	eio_obj_t      *in;         /* standard input event IO object       */
	eio_obj_t      *out;        /* standard output event IO object      */
	eio_obj_t      *err;        /* standard error event IO object       */

	bool            killed_by_cmd; /* true if task killed by our signal */
	bool            aborted;    /* true if task called abort            */
	bool            esent;      /* true if exit status has been sent    */
	bool            exited;     /* true if task has exited              */
	int             estatus;    /* this task's exit status              */

	uint32_t	argc;
	char	      **argv;
} stepd_step_task_info_t;

typedef struct {
	int magic;
	char *bundle; /* OCI Container Bundle path	*/
	data_t *config; /* OCI Container config.json contents */
	char *mount_spool_dir; /* target path to mount container spool dir */
	char *rootfs; /* path to container rootfs */
	char *spool_dir; /* path to container spool dir */
} step_container_t;

typedef struct {
	char *alias_list; /* node name to address aliases */
	step_container_t *container; /* populated if step is a container */
	slurmstepd_state_t state;	/* Job state			*/
	pthread_cond_t state_cond;	/* Job state conditional	*/
	pthread_mutex_t state_mutex;	/* Job state mutex		*/
	slurm_step_id_t step_id; /* Current step id (or NO_VAL)               */
	uint32_t       array_job_id;  /* job array master job ID            */
	uint32_t       array_task_id; /* job array ID                       */
	uint32_t       nnodes; /* number of nodes in current job            */
	uint32_t       ntasks; /* total number of tasks in current job      */
	uint32_t       nodeid; /* relative position of this node in job     */
	uint32_t       node_tasks;	/* number of tasks on *this* node   */
	slurm_addr_t *node_addrs; /* allocated node addrs -- from cred */
	char *node_list; /* allocated nodes -- from cred */
	uint32_t       het_job_id;	/* Hetjob ID or NO_VAL */
	uint32_t       het_job_nnodes;	/* total node count for entire hetjob */
	char          *het_job_node_list; /* Hetjob step node list */
	uint32_t       het_job_node_offset;/* Hetjob node offset or NO_VAL   */
	uint32_t       het_job_ntasks;	/* total task count for entire hetjob */
	uint32_t       het_job_offset;	/* Hetjob offset or NO_VAL        */
	uint32_t       het_job_step_cnt;  /* number of steps for entire hetjob */
	uint32_t       het_job_task_offset;/* Hetjob task offset or NO_VAL   */
	uint16_t      *het_job_task_cnts; /* Number of tasks on each node in hetjob */
	uint32_t     **het_job_tids;       /* Task IDs on each node of hetjob */
	uint32_t      *het_job_tid_offsets;/* map of tasks (by id) to originating hetjob*/
	uint16_t      *task_cnts;  /* Number of tasks on each node in job   */
	uint32_t       cpus_per_task;	/* number of cpus desired per task  */
	uint32_t       debug;  /* debug level for job slurmd                */
	uint64_t       job_mem;  /* MB of memory reserved for the job       */
	uint64_t       step_mem; /* MB of memory reserved for the step      */
	uint16_t       cpus;   /* number of cpus to use for this job        */
	uint32_t       argc;   /* number of commandline arguments           */
	char         **env;    /* job environment                           */
	char         **argv;   /* job argument vector                       */
	char          *cwd;    /* path to current working directory         */
	task_dist_states_t task_dist;/* -m distribution                     */
	char          *node_name; /* node name of node running job
				   * needed for front-end systems           */
	cpu_bind_type_t cpu_bind_type; /* --cpu-bind=                       */
	char          *cpu_bind;       /* binding map for map/mask_cpu      */
	mem_bind_type_t mem_bind_type; /* --mem-bind=                       */
	char          *mem_bind;       /* binding map for tasks to memory   */
	uint16_t accel_bind_type;  /* --accel_bind= */
	uint32_t cpu_freq_min; /* Minimum cpu frequency  */
	uint32_t cpu_freq_max; /* Maximum cpu frequency  */
	uint32_t cpu_freq_gov; /* cpu frequency governor */
	dynamic_plugin_data_t *switch_step; /* switch-specific job information */
	uid_t         uid;     /* user id for job                           */
	char          *user_name;
	/* fields from the launch cred used to support nss_slurm	    */
	char *pw_gecos;
	char *pw_dir;
	char *pw_shell;
	gid_t         gid;     /* group ID for job                          */
	int           ngids;   /* length of the following gids array        */
	char **gr_names;
	gid_t        *gids;    /* array of gids for user specified in uid   */
	bool           aborted;    /* true if already aborted               */
	bool           batch;      /* true if this is a batch job           */
	bool           run_prolog; /* true if need to run prolog            */
	time_t         timelimit;  /* time at which job must stop           */
	uint32_t       profile;	   /* Level of acct_gather_profile          */
	char          *task_prolog; /* per-task prolog                      */
	char          *task_epilog; /* per-task epilog                      */
	stepd_step_task_info_t  **task;  /* array of task information pointers*/
	eio_handle_t  *eio;
	list_t *sruns;		/* list of srun_info_t pointers */
	list_t *clients;	/* list of struct client_io_info pointers */
	list_t *stdout_eio_objs;/* list of objs that gather stdout from tasks */
	list_t *stderr_eio_objs;/* list of objs that gather stderr from tasks */
	list_t *free_incoming;	/* list of free struct io_buf * for incoming
				 * traffic. "incoming" means traffic from srun
				 * to the tasks.
				 */
	list_t *free_outgoing;	/* list of free struct io_buf * for outgoing
				 * traffic "outgoing" means traffic from the
				 * tasks to srun.
				 */
	int incoming_count;   /* Count of total incoming message buffers
			       * including free_incoming buffers and
			       * buffers in use.
			       */
	int outgoing_count;   /* Count of total outgoing message buffers
			       * including free_outgoing buffers and
			       * buffers in use.
			       */

	list_t *outgoing_cache;	/* cache of outgoing stdio messages
				 * used when a new client attaches
				 */

	bool io_running;		/* I/O thread running */
	pthread_cond_t io_cond;		/* I/O thread state conditional */
	pthread_mutex_t io_mutex;	/* I/O thread state mutex */
	pthread_t      msgid; /* pthread id of message thread               */
	eio_handle_t  *msg_handle; /* eio handle for the message thread     */

	pid_t          jmgr_pid;     /* job manager pid                     */
	pid_t          pgid;         /* process group id for tasks          */
	uint32_t       flags;        /* See LAUNCH_* flags defined in slurm_protocol_defs.h */
	env_t          *envtp;
	uint64_t       cont_id;

	char          *batchdir;
	jobacctinfo_t *jobacct;
	uint8_t        open_mode;	/* stdout/err append or truncate */
	list_t *options;
	uint16_t       restart_cnt;	/* batch job restart count	*/
	char	      *job_alloc_cores;	/* needed by the SPANK cpuset plugin */
	char	      *step_alloc_cores;/* needed by the SPANK cpuset plugin */
	list_t *job_gres_list;		/* Needed by GRES plugin */
	list_t *step_gres_list;		/* Needed by GRES plugin */
	char          *tres_bind;	/* TRES binding */
	char          *tres_freq;	/* TRES frequency */
	time_t job_end_time;            /* job end time */
	char *job_licenses;		/* Licenses allocated to job */
	time_t job_start_time;          /* job start time */
	launch_tasks_request_msg_t *msg; /* When a non-batch step this
					  * is the message sent.  DO
					  * NOT FREE, IT IS JUST A
					  * POINTER. */
	uint16_t	job_core_spec;	/* count of specialized cores */
	bool		oom_error;	/* step out of memory error */

	uint16_t x11;			/* only set for extern step */
	int x11_display;		/* display number if x11 forwarding setup */
	char *x11_alloc_host;		/* remote host to proxy through */
	uint16_t x11_alloc_port;	/* remote port to proxy through */
	char *x11_magic_cookie;		/* xauth magic cookie value */
	char *x11_target;		/* remote target. unix socket if port == 0 */
	uint16_t x11_target_port;	/* remote x11 port to connect back to */
	char *x11_xauthority;		/* temporary XAUTHORITY location, or NULL */

	char *selinux_context;
	char *stepmgr;
	bool oom_kill_step;
} stepd_step_rec_t;


stepd_step_rec_t * stepd_step_rec_create(launch_tasks_request_msg_t *msg,
					 uint16_t protocol_version);
stepd_step_rec_t * batch_stepd_step_rec_create(batch_job_launch_msg_t *msg);

void stepd_step_rec_destroy(stepd_step_rec_t *step);

srun_info_t * srun_info_create(slurm_cred_t *cred, slurm_addr_t *respaddr,
			       slurm_addr_t *ioaddr, uid_t uid,
			       uint16_t protocol_version);

void  srun_info_destroy(srun_info_t *srun);

stepd_step_task_info_t * task_info_create(int taskid, int gtaskid,
					  char *ifname, char *ofname,
					  char *efname);

/*
 *  Return a task info structure corresponding to pid.
 *   We inline it here so that it can be included from src/common/plugstack.c
 *   without undefined symbol warnings.
 */
static inline stepd_step_task_info_t *
job_task_info_by_pid (stepd_step_rec_t *step, pid_t pid)
{
	uint32_t i;

	if (!step)
		return NULL;

	for (i = 0; i < step->node_tasks; i++) {
		if (step->task[i]->pid == pid)
			return (step->task[i]);
	}
	return (NULL);
}

#endif /* !_SLURMSTEPD_JOB_H */
