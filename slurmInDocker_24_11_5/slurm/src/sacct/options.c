/*****************************************************************************\
 *  options.c - option functions for sacct
 *****************************************************************************
 *  Copyright (C) 2006-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Copyright (C) SchedMD LLC.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>.
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

#include "src/common/data.h"
#include "src/common/parse_time.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/ref.h"
#include "src/common/slurm_time.h"
#include "src/common/xstring.h"
#include "src/interfaces/data_parser.h"
#include "src/interfaces/serializer.h"
#include "sacct.h"
#include <time.h>

/* getopt_long options, integers but not characters */
#define OPT_LONG_DELIMITER 0x100
#define OPT_LONG_LOCAL     0x101
#define OPT_LONG_NAME      0x102
#define OPT_LONG_NOCONVERT 0x103
#define OPT_LONG_UNITS     0x104
#define OPT_LONG_FEDR      0x105
#define OPT_LONG_WHETJOB   0x106
#define OPT_LONG_LOCAL_UID 0x107
#define OPT_LONG_ENV       0x108
#define OPT_LONG_JSON      0x109
#define OPT_LONG_YAML      0x110
#define OPT_LONG_AUTOCOMP  0x111
#define OPT_LONG_ARRAY     0x112
#define OPT_LONG_HELPSTATE 0x113
#define OPT_LONG_HELPREASON 0x114
#define OPT_LONG_EXPAND_PATTERNS 0x115

#define JOB_HASH_SIZE 1000

static void _help_fields_msg(void);
static void _help_msg(void);
static void _init_params(void);
static void _usage(void);

decl_static_data(help_txt);

list_t *selected_parts = NULL;
list_t *selected_steps = NULL;
void *acct_db_conn = NULL;

list_t *print_fields_list = NULL;
list_itr_t *print_fields_itr = NULL;
int field_count = 0;
list_t *g_qos_list = NULL;
list_t *g_tres_list = NULL;

static list_t *_build_cluster_list(slurmdb_federation_rec_t *fed)
{
	slurmdb_cluster_rec_t *cluster;
	list_itr_t *iter;
	list_t *cluster_list;

	cluster_list = list_create(xfree_ptr);
	iter = list_iterator_create(fed->cluster_list);
	while ((cluster = list_next(iter)))
		(void) slurm_addto_char_list(cluster_list, cluster->name);
	list_iterator_destroy(iter);

	return cluster_list;
}

static void _help_fields_msg(void)
{
	int i;

	for (i = 0; fields[i].name; i++) {
		if (i & 3)
			printf(" ");
		else if (i)
			printf("\n");
		printf("%-19s", fields[i].name);
	}
	printf("\n");
	return;
}

static void _help_job_state_msg(void)
{
	for (int idx = 0; idx < JOB_END; idx++) {
		if (idx & 3)
			printf(" ");
		else if (idx)
			printf("\n");
		printf("%-19s", job_state_string(idx));
	}
	printf("\n");
	return;
}

static void _help_job_reason_msg(void)
{
	for (int idx = 0; idx < REASON_END; idx++) {
		if (idx & 1)
			printf(" ");
		else if (idx)
			printf("\n");
		printf("%-39s", job_state_reason_string(idx));
	}
	printf("\n");
	return;
}

/* returns number of objects added to list */
static int _addto_reason_char_list_internal(list_t *char_list, char *name,
					    void *x)
{
	uint32_t c;
	char *tmp_name = NULL;

	c = job_state_reason_num(name);
	if (c == NO_VAL)
		fatal("unrecognized job reason value '%s'", name);
	tmp_name = xstrdup_printf("%u", c);

	if (!list_find_first(char_list, slurm_find_char_in_list, tmp_name)) {
		list_append(char_list, tmp_name);
		return 1;
	} else {
		xfree(tmp_name);
		return 0;
	}
}

/* returns number of objects added to list */
static int _addto_reason_char_list(list_t *char_list, char *names)
{
	if (!char_list) {
		error("No list was given to fill in");
		return 0;
	}

	return slurm_parse_char_list(char_list, names, NULL,
				     _addto_reason_char_list_internal);
}

static bool _supported_state(uint32_t state_num)
{
	/* Not all state and state flags are accounted */
	switch(state_num) {
	case JOB_PENDING:
	case JOB_RUNNING:
	case JOB_SUSPENDED:
	case JOB_COMPLETE:
	case JOB_CANCELLED:
	case JOB_FAILED:
	case JOB_TIMEOUT:
	case JOB_NODE_FAIL:
	case JOB_PREEMPTED:
	case JOB_BOOT_FAIL:
	case JOB_DEADLINE:
	case JOB_OOM:
	case JOB_REQUEUE:
	case JOB_RESIZING:
	case JOB_REVOKED:
		return true;
		break;
	default:
		return false;
		break;
	}
}

static int _addto_state_char_list_internal(list_t *char_list, char *name,
					   void *x)
{
	uint32_t c;
	char *tmp_name = NULL;

	c = job_state_num(name);
	if (c == NO_VAL)
		fatal("unrecognized job state value '%s'", name);
	if (!_supported_state(c))
		fatal("job state %s is not supported / accounted", name);
	tmp_name = xstrdup_printf("%d", c);

	if (!list_find_first(char_list, slurm_find_char_in_list, tmp_name)) {
		list_append(char_list, tmp_name);
		return 1;
	} else {
		xfree(tmp_name);
		return 0;
	}
}

/* returns number of objects added to list */
/* also checks if states are supported by sacct and fatals if not */
static int _addto_state_char_list(list_t *char_list, char *names)
{
	if (!char_list) {
		error("No list was given to fill in");
		return 0;
	}

	return slurm_parse_char_list(char_list, names, NULL,
				     _addto_state_char_list_internal);
}

static void _help_msg(void)
{
	char *txt;
	static_ref_to_cstring(txt, help_txt);
	printf("%s", txt);
	xfree(txt);
}

static void _usage(void)
{
	printf("Usage: sacct [options]\n\tUse --help for help\n");
}

static void _init_params(void)
{
	memset(&params, 0, sizeof(sacct_parameters_t));
	params.job_cond = xmalloc(sizeof(slurmdb_job_cond_t));
	params.job_cond->db_flags = SLURMDB_JOB_FLAG_NOTSET;
	params.job_cond->flags |= JOBCOND_FLAG_NO_TRUNC;
	params.convert_flags = CONVERT_NUM_UNIT_EXACT;
	params.units = NO_VAL;
}

static int _sort_desc_submit_time(void *x, void *y)
{
	slurmdb_job_rec_t *j1 = *(slurmdb_job_rec_t **)x;
	slurmdb_job_rec_t *j2 = *(slurmdb_job_rec_t **)y;

	if (j1->submit < j2->submit)
		return -1;
	else if (j1->submit > j2->submit)
		return 1;

	if (j1->array_job_id < j2->array_job_id)
		return -1;
	else if (j1->array_job_id > j2->array_job_id)
		return 1;

	if (j1->array_task_id < j2->array_task_id)
		return -1;
	else if (j1->array_task_id > j2->array_task_id)
		return 1;

	if (j1->jobid < j2->jobid)
		return -1;
	else if (j1->jobid > j2->jobid)
		return 1;

	return 0;
}

static int _sort_asc_submit_time(void *x, void *y)
{
	slurmdb_job_rec_t *j1 = *(slurmdb_job_rec_t **)x;
	slurmdb_job_rec_t *j2 = *(slurmdb_job_rec_t **)y;

	if (j1->submit < j2->submit)
		return 1;
	else if (j1->submit > j2->submit)
		return -1;

	return 0;
}

static void _remove_duplicate_fed_jobs(list_t *jobs)
{
	int i, j;
	uint32_t hash_inx;
	bool found = false;
	uint32_t *hash_tbl_size = NULL;
	slurmdb_job_rec_t ***hash_job = NULL;
	slurmdb_job_rec_t *job = NULL;
	list_itr_t *itr = NULL;

	xassert(jobs);

	hash_tbl_size = xmalloc(sizeof(uint32_t) * JOB_HASH_SIZE);
	hash_job = xmalloc(sizeof(slurmdb_job_rec_t **) * JOB_HASH_SIZE);

	for (i = 0; i < JOB_HASH_SIZE; i++) {
		hash_tbl_size[i] = 100;
		hash_job[i] = xmalloc(sizeof(slurmdb_job_rec_t *) *
				      hash_tbl_size[i]);
	}

	/* Put newest jobs at the front so that the later jobs can be removed
	 * easily */
	list_sort(jobs, _sort_asc_submit_time);

	itr = list_iterator_create(jobs);
	while ((job = list_next(itr))) {
		found = false;

		hash_inx = job->jobid % JOB_HASH_SIZE;
		for (j = 0; (j < hash_tbl_size[hash_inx] &&
			     hash_job[hash_inx][j]); j++) {
			if (job->jobid == hash_job[hash_inx][j]->jobid) {
				found = true;
				break;
			}
		}
		if (found) {
			/* Show sibling jobs that are related. e.g. when a
			 * pending sibling job is cancelled all siblings have
			 * the state as cancelled. Since jobids won't roll in a
			 * day -- unless the system is amazing -- just remove
			 * jobs that are older than a day. */
			if (hash_job[hash_inx][j]->submit > (job->submit +
							     86400))
				list_delete_item(itr);
		} else {
			if (j >= hash_tbl_size[hash_inx]) {
				hash_tbl_size[hash_inx] *= 2;
				xrealloc(hash_job[hash_inx],
					 sizeof(slurmdb_job_rec_t *) *
					 hash_tbl_size[hash_inx]);
			}
			hash_job[hash_inx][j] = job;
		}
	}
	list_iterator_destroy(itr);

	/* Put jobs back in desc order */
	list_sort(jobs, _sort_desc_submit_time);

	for (i = 0; i < JOB_HASH_SIZE; i++)
		xfree(hash_job[i]);
	xfree(hash_tbl_size);
	xfree(hash_job);
}

extern int get_data(void)
{
	slurmdb_job_rec_t *job = NULL;
	slurmdb_step_rec_t *step = NULL;
	list_itr_t *itr = NULL;
	list_itr_t *itr_step = NULL;
	slurmdb_job_cond_t *job_cond = params.job_cond;
	int cnt;

	if (params.opt_completion) {
		jobs = slurmdb_jobcomp_jobs_get(job_cond);
		return SLURM_SUCCESS;
	} else {
		jobs = slurmdb_jobs_get(acct_db_conn, job_cond);
	}

	if (!jobs)
		return SLURM_ERROR;

	/*
	 * Remove duplicate federated jobs. The db will remove duplicates for
	 * one cluster but not when jobs for multiple clusters are requested.
	 * Remove the current job if there were jobs with the same id submitted
	 * in the future.
	 * Else sort the jobs to order the jobs so the last task of arrays don't
	 * appear to run before any of the other tasks.
	 */
	if (params.cluster_name && !(job_cond->flags & JOBCOND_FLAG_DUP))
		_remove_duplicate_fed_jobs(jobs);
	else
		list_sort(jobs, _sort_desc_submit_time);

	itr = list_iterator_create(jobs);
	while ((job = list_next(itr))) {

		if (!job->steps || !(cnt = list_count(job->steps)))
			continue;

		itr_step = list_iterator_create(job->steps);
		while ((step = list_next(itr_step))) {
			/* now aggregate the aggregatable */

			if (step->state < JOB_COMPLETE)
				continue;
			job->tot_cpu_sec += step->tot_cpu_sec;
			job->tot_cpu_usec += step->tot_cpu_usec;
			job->user_cpu_sec +=
				step->user_cpu_sec;
			job->user_cpu_usec +=
				step->user_cpu_usec;
			job->sys_cpu_sec +=
				step->sys_cpu_sec;
			job->sys_cpu_usec +=
				step->sys_cpu_usec;
		}
		list_iterator_destroy(itr_step);
	}
	list_iterator_destroy(itr);

	return SLURM_SUCCESS;
}

extern void parse_command_line(int argc, char **argv)
{
	extern int optind;
	int c, i, option_index = 0;
	char *end = NULL, *start = NULL;
	slurm_selected_step_t *selected_step = NULL;
	list_itr_t *itr = NULL;
	struct stat stat_buf;
	char *dot = NULL;
	char *env_val = NULL;
	bool brief_output = false, long_output = false;
	bool all_users = false;
	bool all_clusters = false;
	char *qos_names = NULL;
	slurmdb_job_cond_t *job_cond = params.job_cond;
	log_options_t opts = LOG_OPTS_STDERR_ONLY ;
	int verbosity;		/* count of -v options */
	bool set;

	static struct option long_options[] = {
		{"autocomplete", required_argument, 0, OPT_LONG_AUTOCOMP},
                {"allusers",       no_argument,       0,    'a'},
                {"accounts",       required_argument, 0,    'A'},
                {"allocations",    no_argument,       0,    'X'},
                {"array",          no_argument,       0,    OPT_LONG_ARRAY},
                {"brief",          no_argument,       0,    'b'},
		{"batch-script",   no_argument,       0,    'B'},
                {"completion",     no_argument,       0,    'c'},
                {"constraints",    required_argument, 0,    'C'},
                {"delimiter",      required_argument, 0,    OPT_LONG_DELIMITER},
                {"duplicates",     no_argument,       0,    'D'},
                {"federation",     no_argument,       0,    OPT_LONG_FEDR},
                {"helpformat",     no_argument,       0,    'e'},
                {"help-fields",    no_argument,       0,    'e'},
                {"helpreason",     no_argument,       0,    OPT_LONG_HELPREASON},
                {"helpstate",      no_argument,       0,    OPT_LONG_HELPSTATE},
                {"endtime",        required_argument, 0,    'E'},
                {"env-vars",       no_argument,       0,    OPT_LONG_ENV},
		{"expand-patterns",no_argument,       0,    OPT_LONG_EXPAND_PATTERNS},
                {"file",           required_argument, 0,    'f'},
                {"flags",          required_argument, 0,    'F'},
                {"gid",            required_argument, 0,    'g'},
                {"group",          required_argument, 0,    'g'},
                {"help",           no_argument,       0,    'h'},
                {"local",          no_argument,       0,    OPT_LONG_LOCAL},
                {"name",           required_argument, 0,    OPT_LONG_NAME},
                {"nnodes",         required_argument, 0,    'i'},
                {"ncpus",          required_argument, 0,    'I'},
                {"jobs",           required_argument, 0,    'j'},
                {"timelimit-min",  required_argument, 0,    'k'},
                {"timelimit-max",  required_argument, 0,    'K'},
                {"long",           no_argument,       0,    'l'},
                {"allclusters",    no_argument,       0,    'L'},
                {"cluster",        required_argument, 0,    'M'},
                {"clusters",       required_argument, 0,    'M'},
                {"nodelist",       required_argument, 0,    'N'},
                {"noconvert",      no_argument,       0,    OPT_LONG_NOCONVERT},
                {"units",          required_argument, 0,    OPT_LONG_UNITS},
                {"noheader",       no_argument,       0,    'n'},
                {"fields",         required_argument, 0,    'o'},
                {"format",         required_argument, 0,    'o'},
                {"parsable",       no_argument,       0,    'p'},
                {"parsable2",      no_argument,       0,    'P'},
                {"qos",            required_argument, 0,    'q'},
                {"partition",      required_argument, 0,    'r'},
                {"reason",         required_argument, 0,    'R'},
                {"state",          required_argument, 0,    's'},
                {"starttime",      required_argument, 0,    'S'},
                {"truncate",       no_argument,       0,    'T'},
                {"uid",            required_argument, 0,    'u'},
		{"use-local-uid",  no_argument,       0,    OPT_LONG_LOCAL_UID},
                {"usage",          no_argument,       0,    'U'},
                {"user",           required_argument, 0,    'u'},
                {"verbose",        no_argument,       0,    'v'},
                {"version",        no_argument,       0,    'V'},
                {"wckeys",         required_argument, 0,    'W'},
                {"whole-hetjob",   optional_argument, 0,    OPT_LONG_WHETJOB},
                {"associations",   required_argument, 0,    'x'},
                {"json", optional_argument, 0, OPT_LONG_JSON},
                {"yaml", optional_argument, 0, OPT_LONG_YAML},
                {0,                0,		      0,    0}};

	params.opt_uid = getuid();
	params.opt_gid = getgid();

	verbosity         = 0;
	log_init("sacct", opts, SYSLOG_FACILITY_DAEMON, NULL);
	opterr = 1;		/* Let getopt report problems to the user */

	if (xstrstr(slurm_conf.fed_params, "fed_display"))
		params.opt_federation = true;

	if (getenv("SACCT_FEDERATION"))
		params.opt_federation = true;
	if (getenv("SACCT_LOCAL"))
		params.opt_local = true;

	while (1) {		/* now cycle through the command line */
		c = getopt_long(argc, argv,
				"aA:bBcC:DeE:f:F:g:hi:I:j:k:K:lLM:nN:o:pPq:r:s:S:Ttu:UvVW:x:X",
				long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
			all_users = true;
			break;
		case 'A':
			if (!job_cond->acct_list)
				job_cond->acct_list = list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->acct_list, optarg);
			break;
		case OPT_LONG_ARRAY:
			params.opt_array = true;
			break;
		case 'b':
			brief_output = true;
			break;
		case 'B':
			job_cond->flags |= JOBCOND_FLAG_SCRIPT;
			job_cond->flags |= JOBCOND_FLAG_NO_STEP;
			break;
		case 'c':
			params.opt_completion = 1;
			break;
		case OPT_LONG_DELIMITER:
			fields_delimiter = optarg;
			break;
		case 'C':
			if (!job_cond->constraint_list)
				job_cond->constraint_list =
					list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->constraint_list,
					      optarg);
			break;
		case 'M':
			if (!xstrcasecmp(optarg, "all") ||
			    !xstrcasecmp(optarg, "-1")) {	/* vestigial */
				all_clusters = true;
				break;
			}
			all_clusters = false;
			params.opt_local = true;
			if (!job_cond->cluster_list)
				job_cond->cluster_list = list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->cluster_list, optarg);
			break;
		case 'D':
			job_cond->flags |= JOBCOND_FLAG_DUP;
			break;
		case 'e':
			params.opt_help = 2;
			break;
		case 'E':
			job_cond->usage_end = parse_time(optarg, 1);
			if (errno == ESLURM_INVALID_TIME_VALUE)
				exit(1);
			break;
		case OPT_LONG_ENV:
			job_cond->flags |= JOBCOND_FLAG_ENV;
			job_cond->flags |= JOBCOND_FLAG_NO_STEP;
			break;
		case OPT_LONG_EXPAND_PATTERNS:
			params.expand_patterns = true;
			break;
		case 'f':
			xfree(slurm_conf.job_comp_loc);
			if ((stat(optarg, &stat_buf) != 0) ||
			    (!S_ISREG(stat_buf.st_mode))) {
				fprintf(stderr, "%s is not a valid file\n",
					optarg);
				exit(1);
			}
			slurm_conf.job_comp_loc = xstrdup(optarg);
			params.opt_completion = 1;
			break;
		case 'F':
			job_cond->db_flags = str_2_job_flags(optarg);
			if (job_cond->db_flags == SLURMDB_JOB_FLAG_NOTSET)
				exit(1);
			break;
		case 'g':
			if (!job_cond->groupid_list)
				job_cond->groupid_list = list_create(xfree_ptr);
			if (slurm_addto_id_char_list(job_cond->groupid_list,
						     optarg, 1) < 1)
				exit(1);
			break;
		case 'h':
			params.opt_help = 1;
			break;
		case 'i':
			set = get_resource_arg_range(
				optarg,
				"requested node range",
				(int *)&job_cond->nodes_min,
				(int *)&job_cond->nodes_max,
				true);

			if (set == false) {
				error("invalid node range -i '%s'",
				      optarg);
				exit(1);
			}
			break;
		case 'I':
			set = get_resource_arg_range(
				optarg,
				"requested cpu range",
				(int *)&job_cond->cpus_min,
				(int *)&job_cond->cpus_max,
				true);

			if (set == false) {
				error("invalid cpu range -i '%s'",
				      optarg);
				exit(1);
			}
			break;
		case 'j':
			if (!job_cond->step_list)
				job_cond->step_list = list_create(
					slurm_destroy_selected_step);
			slurm_addto_step_list(job_cond->step_list, optarg);
			if (!list_count(job_cond->step_list))
				FREE_NULL_LIST(job_cond->step_list);
			break;
		case 'k':
			job_cond->timelimit_min = time_str2mins(optarg);
			if (((int32_t)job_cond->timelimit_min <= 0)
			    && (job_cond->timelimit_min != INFINITE))
				fatal("Invalid time limit specification");
			break;
		case 'K':
			job_cond->timelimit_max = time_str2mins(optarg);
			if (((int32_t)job_cond->timelimit_max <= 0)
			    && (job_cond->timelimit_max != INFINITE))
				fatal("Invalid time limit specification");
			break;
		case 'L':
			all_clusters = true;
			break;
		case 'l':
			long_output = true;
			break;
		case OPT_LONG_FEDR:
			params.opt_federation = true;
			all_clusters = false;
			break;
		case OPT_LONG_LOCAL:
			params.opt_local = true;
			all_clusters = false;
			break;
		case OPT_LONG_NOCONVERT:
			params.convert_flags |= CONVERT_NUM_UNIT_NO;
			break;
		case OPT_LONG_UNITS:
		{
			int type = get_unit_type(*optarg);
			if (type == SLURM_ERROR)
				fatal("Invalid unit type");
			params.units = type;
		}
			break;
		case 'n':
			print_fields_have_header = 0;
			break;
		case 'N':
			if (job_cond->used_nodes) {
				error("Already asked for nodes '%s'",
				      job_cond->used_nodes);
				break;
			}
			job_cond->used_nodes = xstrdup(optarg);
			break;
		case OPT_LONG_NAME:
			if (!job_cond->jobname_list)
				job_cond->jobname_list = list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->jobname_list, optarg);
			break;
		case 'o':
			xstrfmtcat(params.opt_field_list, "%s,", optarg);
			break;
		case 'p':
			print_fields_parsable_print =
				PRINT_FIELDS_PARSABLE_ENDING;
			break;
		case 'P':
			print_fields_parsable_print =
				PRINT_FIELDS_PARSABLE_NO_ENDING;
			break;
		case 'q':
			qos_names = xstrdup(optarg);
			break;
		case 'r':
			if (!job_cond->partition_list)
				job_cond->partition_list =
					list_create(xfree_ptr);

			slurm_addto_char_list(job_cond->partition_list,
					      optarg);
			break;
		case 'R':
			if (!job_cond->reason_list)
				job_cond->reason_list = list_create(xfree_ptr);

			_addto_reason_char_list(job_cond->reason_list, optarg);
			break;
		case 's':
			if (!job_cond->state_list)
				job_cond->state_list = list_create(xfree_ptr);

			_addto_state_char_list(job_cond->state_list, optarg);
			break;
		case 'S':
			job_cond->usage_start = parse_time(optarg, 1);
			if (errno == ESLURM_INVALID_TIME_VALUE)
				exit(1);
			break;
		case 'T':
			job_cond->flags &= ~JOBCOND_FLAG_NO_TRUNC;
			break;
		case 'U':
			params.opt_help = 3;
			break;
		case 'u':
			if (!xstrcmp(optarg, "-1")) {
				all_users = true;
				break;
			}
			all_users = false;
			if (!job_cond->userid_list)
				job_cond->userid_list = list_create(xfree_ptr);
			if (slurm_addto_id_char_list(job_cond->userid_list,
						     optarg, 0) < 1)
				exit(1);
			break;
		case OPT_LONG_LOCAL_UID:
			params.use_local_uid = true;
			break;
		case 'v':
			/* Handle -vvv thusly...
			 */
			verbosity++;
			break;
		case 'W':
			if (!job_cond->wckey_list)
				job_cond->wckey_list = list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->wckey_list, optarg);
			break;
		case OPT_LONG_WHETJOB:
			if (!optarg || !xstrcasecmp(optarg, "yes") ||
			    !xstrcasecmp(optarg, "y"))
				job_cond->flags |= JOBCOND_FLAG_WHOLE_HETJOB;
			else if (!xstrcasecmp(optarg, "no") ||
				 !xstrcasecmp(optarg, "n"))
				job_cond->flags |= JOBCOND_FLAG_NO_WHOLE_HETJOB;
			else if (optarg) {
				error("Invalid --whole-hetjob value \"%s\"."
				      " Valid values: [yes|no].", optarg);
				exit(1);
			}
			break;
		case 'V':
			print_slurm_version();
			exit(0);
		case 'x':
			if (!job_cond->associd_list)
				job_cond->associd_list = list_create(xfree_ptr);
			slurm_addto_char_list(job_cond->associd_list, optarg);
			break;
		case 't':
			/* 't' is deprecated and was replaced with 'X'.	*/
		case 'X':
			job_cond->flags |= JOBCOND_FLAG_NO_STEP;
			break;
		case OPT_LONG_JSON:
			params.mimetype = MIME_TYPE_JSON;
			params.data_parser = optarg;
			if (serializer_g_init(MIME_TYPE_JSON_PLUGIN, NULL))
				fatal("JSON plugin load failure");
			break;
		case OPT_LONG_YAML:
			params.mimetype = MIME_TYPE_YAML;
			params.data_parser = optarg;
			if (serializer_g_init(MIME_TYPE_YAML_PLUGIN, NULL))
				fatal("YAML plugin load failure");
			break;
		case OPT_LONG_AUTOCOMP:
			suggest_completion(long_options, optarg);
			exit(0);
			break;
		case OPT_LONG_HELPSTATE:
			params.opt_help = 4;
			break;
		case OPT_LONG_HELPREASON:
			params.opt_help = 5;
			break;
		case ':':
		case '?':	/* getopt() has explained it */
			exit(1);
		}
	}

	if (!job_cond->step_list || !list_count(job_cond->step_list)) {
		char *reason = NULL;
		if (job_cond->flags & JOBCOND_FLAG_SCRIPT)
			reason = "job scripts";

		if (job_cond->flags & JOBCOND_FLAG_ENV)
			reason = "job environment";

		if (reason)
			fatal("When requesting %s you must also request specific jobs with the '-j' option.", reason);
	}

	if ((job_cond->flags & JOBCOND_FLAG_SCRIPT) &&
	    (job_cond->flags & JOBCOND_FLAG_ENV))
		fatal("Options --batch-script and --env-vars are mutually exclusive");


	if (long_output && params.opt_field_list)
		fatal("Options -o(--format) and -l(--long) are mutually exclusive. Please remove one and retry.");

	if (verbosity) {
		opts.stderr_level += verbosity;
		opts.prefix_level = 1;
		log_alter(opts, 0, NULL);
	}

	slurmdb_job_cond_def_start_end(job_cond);

	if (job_cond->usage_end &&
	    (job_cond->usage_start > job_cond->usage_end)) {
		char start_str[256], end_str[256];
		slurm_make_time_str(&job_cond->usage_start, start_str,
				    sizeof(start_str));
		slurm_make_time_str(&job_cond->usage_end, end_str,
				    sizeof(end_str));
		error("Start time (%s) requested is after end time (%s).",
		      start_str, end_str);
		exit(1);
	}

	if (verbosity > 0) {
		char start_char[25], end_char[25];
		char *verbosity_states = NULL;

		if (job_cond->state_list && list_count(job_cond->state_list)) {
			char *state;
			list_itr_t *itr = list_iterator_create(
				job_cond->state_list);

			while ((state = list_next(itr))) {
				if (verbosity_states)
					xstrcat(verbosity_states, ",");
				xstrfmtcat(verbosity_states, "%s",
					   job_state_string_complete(
						   atol(state)));
			}
			list_iterator_destroy(itr);
		} else
			verbosity_states = xstrdup("Eligible");

		if (!job_cond->usage_start)
			strlcpy(start_char, "Epoch 0", sizeof(start_char));
		else
			slurm_ctime2_r(&job_cond->usage_start, start_char);

		slurm_ctime2_r(&job_cond->usage_end, end_char);

		if (xstrcmp(start_char, end_char))
			info("Jobs %s in the time window from %s to %s",
			     verbosity_states, start_char, end_char);
		else
			info("Jobs %s at the time instant %s",
			     verbosity_states, start_char);
		xfree(verbosity_states);
	}

	debug("Options selected:\n"
	      "\topt_completion=%s\n"
	      "\topt_dup=%s\n"
	      "\topt_field_list=%s\n"
	      "\topt_help=%d\n"
	      "\topt_no_steps=%s\n"
	      "\topt_whole_hetjob=%s",
	      params.opt_completion ? "yes" : "no",
	      (job_cond->flags & JOBCOND_FLAG_DUP) ? "yes" : "no",
	      params.opt_field_list,
	      params.opt_help,
	      (job_cond->flags & JOBCOND_FLAG_NO_STEP) ? "yes" : "no",
	      (job_cond->flags & JOBCOND_FLAG_WHOLE_HETJOB) ? "yes" :
	      (job_cond->flags & JOBCOND_FLAG_NO_WHOLE_HETJOB ? "no" : 0));

	if (params.opt_completion) {
		if (!slurm_conf.job_comp_type) {
			fprintf(stderr, "Slurm job completion is disabled\n");
			exit(1);
		}

		if (slurmdb_jobcomp_init() != SLURM_SUCCESS) {
			fprintf(stderr,
				"Slurm unable to initialize jobcomp plugin\n");
			exit(1);
		}
	} else {
		if (!slurm_conf.accounting_storage_type) {
			fprintf(stderr,
				"Slurm accounting storage is disabled\n");
			exit(1);
		}
		if (acct_storage_g_init() != SLURM_SUCCESS) {
			fprintf(stderr,
				"Slurm unable to initialize storage plugin\n");
			exit(1);
		}
		acct_db_conn = slurmdb_connection_get(NULL);
		if (errno != SLURM_SUCCESS) {
			error("Problem talking to the database: %m");
			exit(1);
		}
	}

	if (qos_names) {
		if (!g_qos_list) {
			slurmdb_qos_cond_t qos_cond = {
				.flags = QOS_COND_FLAG_WITH_DELETED,
			};
			g_qos_list = slurmdb_qos_get(
				acct_db_conn, &qos_cond);
		}

		if (!job_cond->qos_list)
			job_cond->qos_list = list_create(xfree_ptr);

		if (slurmdb_addto_qos_char_list(job_cond->qos_list,
						g_qos_list, qos_names, 0) < 1)
			fatal("problem processing qos list");
		xfree(qos_names);
	}


	/* specific clusters requested? */
	if (params.opt_federation && !all_clusters && !job_cond->cluster_list &&
	    !params.opt_local && !params.opt_completion) {
		/* Test if in federated cluster and if so, get information from
		 * all clusters in that federation */
		slurmdb_federation_rec_t *fed = NULL;
		slurmdb_federation_cond_t fed_cond;
		list_t *fed_list = NULL;
		list_t *cluster_list = list_create(NULL);

		params.cluster_name = xstrdup(slurm_conf.cluster_name);

		list_append(cluster_list, params.cluster_name);
		slurmdb_init_federation_cond(&fed_cond, 0);
		fed_cond.cluster_list = cluster_list;

		if ((fed_list = slurmdb_federations_get(
			     acct_db_conn, &fed_cond)) &&
		    list_count(fed_list) == 1) {
			fed = list_peek(fed_list);
			job_cond->cluster_list = _build_cluster_list(fed);
			/* Leave cluster_name to identify remote only jobs */
			// xfree(params.cluster_name);
		} else
			xfree(params.cluster_name);
		FREE_NULL_LIST(cluster_list);
		FREE_NULL_LIST(fed_list);
	}
	if (all_clusters) {
		if (job_cond->cluster_list
		   && list_count(job_cond->cluster_list)) {
			FREE_NULL_LIST(job_cond->cluster_list);
		}
		debug2("Clusters requested:\tall");
	} else if (job_cond->cluster_list
		   && list_count(job_cond->cluster_list)) {
		debug2( "Clusters requested:");
		itr = list_iterator_create(job_cond->cluster_list);
		while ((start = list_next(itr)))
			debug2("\t: %s", start);
		list_iterator_destroy(itr);
	} else if (!job_cond->cluster_list
		  || !list_count(job_cond->cluster_list)) {
		if (!job_cond->cluster_list)
			job_cond->cluster_list = list_create(xfree_ptr);
		if ((start = xstrdup(slurm_conf.cluster_name))) {
			list_append(job_cond->cluster_list, start);
			debug2("Clusters requested:\t%s", start);
		}
	}

	/* if any jobs or nodes are specified set to look for all users if none
	   are set */
	if (!job_cond->userid_list || !list_count(job_cond->userid_list))
		if ((job_cond->step_list && list_count(job_cond->step_list))
		   || job_cond->used_nodes)
			all_users = true;

	/* set all_users for user root if not requesting any */
	if (!job_cond->userid_list && !params.opt_uid)
		all_users = true;

	if (all_users) {
		if (job_cond->userid_list &&
		    list_count(job_cond->userid_list)) {
			FREE_NULL_LIST(job_cond->userid_list);
		}
		debug2("Userids requested:\tall");
	} else if (job_cond->userid_list && list_count(job_cond->userid_list)) {
		debug2("Userids requested:");
		itr = list_iterator_create(job_cond->userid_list);
		while ((start = list_next(itr)))
			debug2("\t: %s", start);
		list_iterator_destroy(itr);
	} else if (!job_cond->userid_list
		  || !list_count(job_cond->userid_list)) {
		if (!job_cond->userid_list)
			job_cond->userid_list = list_create(xfree_ptr);
		start = xstrdup_printf("%u", params.opt_uid);
		list_append(job_cond->userid_list, start);
		debug2("Userid requested\t: %s", start);
	}

	if (job_cond->groupid_list && list_count(job_cond->groupid_list)) {
		debug2("Groupids requested:");
		itr = list_iterator_create(job_cond->groupid_list);
		while ((start = list_next(itr)))
			debug2("\t: %s", start);
		list_iterator_destroy(itr);
	}

	/* specific partitions requested? */
	if (job_cond->partition_list && list_count(job_cond->partition_list)) {
		debug2("Partitions requested:");
		itr = list_iterator_create(job_cond->partition_list);
		while ((start = list_next(itr)))
			debug2("\t: %s", start);
		list_iterator_destroy(itr);
	}

	/* specific qos' requested? */
	if (job_cond->qos_list && list_count(job_cond->qos_list)) {
		start = get_qos_complete_str(g_qos_list, job_cond->qos_list);
		debug2("QOS requested\t: %s\n", start);
		xfree(start);
	}

	/* specific jobs requested? */
	if (job_cond->step_list && list_count(job_cond->step_list)) {
		debug2("Jobs requested:");
		itr = list_iterator_create(job_cond->step_list);
		while ((selected_step = list_next(itr))) {
			char id[FORMAT_STRING_SIZE];

			debug2("\t: %s", slurm_get_selected_step_id(
				       id, sizeof(id), selected_step));
		}
		list_iterator_destroy(itr);
	}

	/* specific states (completion state) requested? */
	if (job_cond->state_list && list_count(job_cond->state_list)) {
		debug2("States requested:");
		itr = list_iterator_create(job_cond->state_list);
		while ((start = list_next(itr))) {
			debug2("\t: %s",
				job_state_string(atoi(start)));
		}
		list_iterator_destroy(itr);
	}

	if (job_cond->wckey_list && list_count(job_cond->wckey_list)) {
		debug2("Wckeys requested:");
		itr = list_iterator_create(job_cond->wckey_list);
		while ((start = list_next(itr)))
			debug2("\t: %s\n", start);
		list_iterator_destroy(itr);
	}

	if (job_cond->timelimit_min) {
		char time_str[128], tmp1[32], tmp2[32];
		mins2time_str(job_cond->timelimit_min, tmp1, sizeof(tmp1));
		sprintf(time_str, "%s", tmp1);
		if (job_cond->timelimit_max) {
			int len = strlen(tmp1);
			mins2time_str(job_cond->timelimit_max,
				      tmp2, sizeof(tmp2));
			sprintf(time_str+len, " - %s", tmp2);
		}
		debug2("Timelimit requested\t: %s", time_str);
	}

	/* specific jobnames requested? */
	if (job_cond->jobname_list && list_count(job_cond->jobname_list)) {
		debug2("Jobnames requested:");
		itr = list_iterator_create(job_cond->jobname_list);
		while ((start = list_next(itr))) {
			debug2("\t: %s", start);
		}
		list_iterator_destroy(itr);
	}

	/* select the output fields */
	if (brief_output) {
		if (params.opt_completion)
			dot = BRIEF_COMP_FIELDS;
		else
			dot = BRIEF_FIELDS;

		xstrfmtcat(params.opt_field_list, "%s,", dot);
	}

	if (long_output) {
		if (params.opt_completion)
			dot = LONG_COMP_FIELDS;
		else
			dot = LONG_FIELDS;

		xstrfmtcat(params.opt_field_list, "%s,", dot);
	}

	if (params.opt_field_list == NULL) {
		if (params.opt_completion)
			dot = DEFAULT_COMP_FIELDS;
		else if ( ( env_val = getenv("SACCT_FORMAT") ) )
			dot = xstrdup(env_val);
		else
			dot = DEFAULT_FIELDS;

		xstrfmtcat(params.opt_field_list, "%s,", dot);
	}

	start = params.opt_field_list;
	while ((end = strstr(start, ","))) {
		char *tmp_char = NULL;
		int command_len = 0;
		int newlen = 0;
		bool newlen_set = false;

		*end = 0;
		while (isspace(*start))
			start++;	/* discard whitespace */
		if (!(int)*start)
			continue;

		if ((tmp_char = strstr(start, "\%"))) {
			newlen_set = true;
			newlen = atoi(tmp_char+1);
			tmp_char[0] = '\0';
		}

		command_len = strlen(start);

		if (!xstrncasecmp("ALL", start, command_len)) {
			for (i = 0; fields[i].name; i++) {
				if (newlen_set)
					fields[i].len = newlen;
				list_append(print_fields_list, &fields[i]);
				start = end + 1;
			}
			start = end + 1;
			continue;
		}

		for (i = 0; fields[i].name; i++) {
			if (!xstrncasecmp(fields[i].name, start, command_len))
				goto foundfield;
		}

		if (!xstrcasecmp("AllocGRES", start)) {
			fatal("AllocGRES has been removed, please use AllocTRES");
		}
		if (!xstrcasecmp("ReqGRES", start)) {
			fatal("ReqGRES has been removed, please use ReqTRES");
		}
		error("Invalid field requested: \"%s\"", start);
		exit(1);
	foundfield:
		if (newlen_set)
			fields[i].len = newlen;
		list_append(print_fields_list, &fields[i]);
		start = end + 1;
	}
	field_count = list_count(print_fields_list);

	if (optind < argc) {
		error("Unknown arguments:");
		for (i=optind; i<argc; i++)
			error(" %s", argv[i]);
		exit(1);
	}
	return;
}

extern void do_help(void)
{
	switch (params.opt_help) {
	case 1:
		_help_msg();
		break;
	case 2:
		_help_fields_msg();
		break;
	case 3:
		_usage();
		break;
	case 4:
		_help_job_state_msg();
		break;
	case 5:
		_help_job_reason_msg();
		break;
	default:
		debug2("sacct bug: params.opt_help=%d",
			params.opt_help);
	}
}

/* Return true if the specified job id is local to a cluster
 * (not a federated job) */
static bool _test_local_job(uint32_t job_id)
{
	if ((job_id & (~MAX_JOB_ID)) == 0)
		return true;
	return false;
}

static void _print_script(slurmdb_job_rec_t *job)
{
	if (print_fields_have_header) {
		char *id = slurmdb_get_job_id_str(job);
		printf("Batch Script for %s\n"
		       "--------------------------------------------------------------------------------\n",
		       id);

		xfree(id);
	}
	printf("%s", job->script ? job->script : "NONE\n");
}

static void _print_env(slurmdb_job_rec_t *job)
{
	if (print_fields_have_header) {
		char *id = slurmdb_get_job_id_str(job);
		printf("Environment used for %s (must be batch to display)\n"
		       "--------------------------------------------------------------------------------\n",
		       id);
		xfree(id);
	}
	printf("%s", job->env ? job->env : "NONE\n");
}

/* do_list() -- List the assembled data
 *
 * In:	Nothing explicit.
 * Out:	void.
 *
 * At this point, we have already selected the desired data,
 * so we just need to print it for the user.
 */
extern void do_list(int argc, char **argv)
{
	list_itr_t *itr = NULL;
	list_itr_t *itr_step = NULL;
	slurmdb_job_rec_t *job = NULL;
	slurmdb_step_rec_t *step = NULL;
	slurmdb_job_cond_t *job_cond = params.job_cond;

	if (params.mimetype) {
		DATA_DUMP_CLI_SINGLE(OPENAPI_SLURMDBD_JOBS_RESP, jobs, argc,
				     argv, acct_db_conn, params.mimetype,
				     params.data_parser, errno);
		return;
	}

	if (!jobs)
		return;

	itr = list_iterator_create(jobs);
	while ((job = list_next(itr))) {
		if ((params.cluster_name) &&
		    _test_local_job(job->jobid) &&
		    xstrcmp(params.cluster_name, job->cluster))
			continue;

		if (job_cond->flags & JOBCOND_FLAG_SCRIPT) {
			_print_script(job);
			continue;
		} else if (job_cond->flags & JOBCOND_FLAG_ENV) {
			_print_env(job);
			continue;
		}

		if (job->show_full)
			print_fields(JOB, job);

		if (!(job_cond->flags & JOBCOND_FLAG_NO_STEP)) {
			itr_step = list_iterator_create(job->steps);
			while ((step = list_next(itr_step))) {
				if (step->end == 0)
					step->end = job->end;
				print_fields(JOBSTEP, step);
			}
			list_iterator_destroy(itr_step);
		}
	}
	list_iterator_destroy(itr);
}

/* do_list_completion() -- List the assembled data
 *
 * In:	Nothing explicit.
 * Out:	void.
 *
 * NOTE: This data is from the job completion data and not federation compliant.
 * At this point, we have already selected the desired data,
 * so we just need to print it for the user.
 */
extern void do_list_completion(void)
{
	list_itr_t *itr = NULL;
	jobcomp_job_rec_t *job = NULL;

	if (!jobs)
		return;

	itr = list_iterator_create(jobs);
	while ((job = list_next(itr))) {
		print_fields(JOBCOMP, job);
	}
	list_iterator_destroy(itr);
}

extern void sacct_init(void)
{
	_init_params();
	print_fields_list = list_create(NULL);
	print_fields_itr = list_iterator_create(print_fields_list);
}

extern void sacct_fini(void)
{
	if (print_fields_itr)
		list_iterator_destroy(print_fields_itr);
	FREE_NULL_LIST(print_fields_list);
	FREE_NULL_LIST(jobs);
	FREE_NULL_LIST(g_qos_list);
	FREE_NULL_LIST(g_tres_list);

	if (params.opt_completion)
		slurmdb_jobcomp_fini();
	else {
		slurmdb_connection_close(&acct_db_conn);
		acct_storage_g_fini();
	}

	xfree(params.opt_field_list);
	slurmdb_destroy_job_cond(params.job_cond);
}
