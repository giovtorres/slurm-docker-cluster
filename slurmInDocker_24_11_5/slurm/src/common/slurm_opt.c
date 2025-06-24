/*****************************************************************************\
 *  slurm_opt.c - salloc/sbatch/srun option processing functions
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

#include "config.h"

#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#endif

#include "src/common/cpu_frequency.h"
#include "src/interfaces/gres.h"
#include "src/common/log.h"
#include "src/common/optz.h"
#include "src/common/parse_time.h"
#include "src/common/proc_args.h"
#include "src/interfaces/acct_gather_profile.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/spank.h"
#include "src/common/tres_bind.h"
#include "src/common/tres_frequency.h"
#include "src/common/uid.h"
#include "src/common/util-net.h"
#include "src/common/x11_util.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/common/slurm_opt.h"

#include "src/interfaces/select.h"

#define COMMON_STRING_OPTION(field)	\
COMMON_STRING_OPTION_SET(field)		\
COMMON_STRING_OPTION_GET(field)		\
COMMON_STRING_OPTION_RESET(field)
#define COMMON_STRING_OPTION_GET_AND_RESET(field)	\
COMMON_STRING_OPTION_GET(field)				\
COMMON_STRING_OPTION_RESET(field)
#define COMMON_STRING_OPTION_SET(field)				\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	xfree(opt->field);					\
	opt->field = xstrdup(arg);				\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_STRING_OPTION_SET_DATA(field)			\
static int arg_set_data_##field(slurm_opt_t *opt,		\
				const data_t *arg,		\
				data_t *errors)			\
__attribute__((nonnull (1, 2)));				\
static int arg_set_data_##field(slurm_opt_t *opt,		\
				const data_t *arg,		\
				data_t *errors)			\
{								\
	xfree(opt->field);					\
	return data_get_string_converted(arg, &opt->field);	\
}
#define COMMON_STRING_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup(opt->field);				\
}
#define COMMON_STRING_OPTION_RESET(field)			\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	xfree(opt->field);					\
}

#define COMMON_SBATCH_STRING_OPTION(field)	\
COMMON_SBATCH_STRING_OPTION_SET(field)		\
COMMON_SBATCH_STRING_OPTION_GET(field)		\
COMMON_SBATCH_STRING_OPTION_RESET(field)
#define COMMON_SBATCH_STRING_OPTION_SET(field)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if (!opt->sbatch_opt)					\
		return SLURM_ERROR;				\
								\
	xfree(opt->sbatch_opt->field);				\
	opt->sbatch_opt->field = xstrdup(arg);			\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_SBATCH_STRING_OPTION_GET(field)			\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	if (!opt->sbatch_opt)					\
		return xstrdup("invalid-context");		\
	return xstrdup(opt->sbatch_opt->field);			\
}
#define COMMON_SBATCH_STRING_OPTION_RESET(field)		\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	if (opt->sbatch_opt)					\
		xfree(opt->sbatch_opt->field);			\
}

#define COMMON_SRUN_STRING_OPTION(field)	\
COMMON_SRUN_STRING_OPTION_SET(field)		\
COMMON_SRUN_STRING_OPTION_GET(field)		\
COMMON_SRUN_STRING_OPTION_RESET(field)
#define COMMON_SRUN_STRING_OPTION_SET(field)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if (!opt->srun_opt)					\
		return SLURM_ERROR;				\
								\
	xfree(opt->srun_opt->field);				\
	opt->srun_opt->field = xstrdup(arg);			\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_SRUN_STRING_OPTION_GET(field)			\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	if (!opt->srun_opt)					\
		return xstrdup("invalid-context");		\
	return xstrdup(opt->srun_opt->field);			\
}
#define COMMON_SRUN_STRING_OPTION_RESET(field)			\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	if (opt->srun_opt)					\
		xfree(opt->srun_opt->field);			\
}

#define COMMON_OPTION_RESET(field, value)			\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	opt->field = value;					\
}

#define COMMON_BOOL_OPTION(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	opt->field = true;					\
								\
	return SLURM_SUCCESS;					\
}								\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup(opt->field ? "set" : "unset");		\
}								\
COMMON_OPTION_RESET(field, false)

#define COMMON_SRUN_BOOL_OPTION(field)				\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if (!opt->srun_opt)					\
		return SLURM_ERROR;				\
								\
	opt->srun_opt->field = true;				\
								\
	return SLURM_SUCCESS;					\
}								\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	if (!opt->srun_opt)					\
		return xstrdup("invalid-context");		\
								\
	return xstrdup(opt->srun_opt->field ? "set" : "unset");	\
}								\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	if (opt->srun_opt)					\
		opt->srun_opt->field = false;			\
}

#define COMMON_INT_OPTION(field, option)			\
COMMON_INT_OPTION_SET(field, option)				\
COMMON_INT_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, 0)
#define COMMON_INT_OPTION_GET_AND_RESET(field)			\
COMMON_INT_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, 0)
#define COMMON_INT_OPTION_SET(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	opt->field = parse_int(option, arg, true);		\
								\
	return SLURM_SUCCESS;					\
}

#define COMMON_INT_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup_printf("%d", opt->field);		\
}

#define COMMON_MBYTES_OPTION(field, option)			\
COMMON_MBYTES_OPTION_SET(field, option)				\
COMMON_MBYTES_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, NO_VAL64)
#define COMMON_MBYTES_OPTION_SET(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if ((opt->field = str_to_mbytes(arg)) == NO_VAL64) {	\
		error("Invalid " #option " specification");	\
		return SLURM_ERROR;				\
	}							\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_MBYTES_OPTION_GET_AND_RESET(field)		\
COMMON_MBYTES_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, NO_VAL64)
#define COMMON_MBYTES_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return mbytes_to_str(opt->field);			\
}

#define COMMON_TIME_DURATION_OPTION_GET_AND_RESET(field)	\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	char time_str[32];					\
	if (opt->field == NO_VAL)				\
		return NULL;					\
	mins2time_str(opt->field, time_str, sizeof(time_str));	\
	return xstrdup(time_str);				\
}								\
COMMON_OPTION_RESET(field, NO_VAL)

typedef struct {
	/*
	 * DO NOT ALTER THESE FIRST FOUR ARGUMENTS
	 * They must match 'struct option', so that some
	 * casting abuse is nice and trivial.
	 */
	const char *name;	/* Long option name. */
	int has_arg;		/* no_argument, required_argument,
				 * or optional_argument */
	int *flag;		/* Always NULL in our usage. */
	int val;		/* Single character, or LONG_OPT_* */
	/*
	 * Add new members below here:
	 */
	bool reset_each_pass;	/* Reset on all HetJob passes or only first */
	bool sbatch_early_pass;	/* For sbatch - run in the early pass. */
				/* For salloc/srun - this is ignored, and will
				 * run alongside all other options. */
	bool srun_early_pass;	/* For srun - run in the early pass. */
	/*
	 * If set_func is set, it will be used, and the command
	 * specific versions must not be set.
	 * Otherwise, command specific versions will be used.
	 */
	int (*set_func)(slurm_opt_t *, const char *);
	int (*set_func_salloc)(slurm_opt_t *, const char *);
	int (*set_func_sbatch)(slurm_opt_t *, const char *);
	int (*set_func_scron)(slurm_opt_t *, const char *);
	int (*set_func_srun)(slurm_opt_t *, const char *);

	/* Return must be xfree()'d */
	char *(*get_func)(slurm_opt_t *);
	void (*reset_func)(slurm_opt_t *);

} slurm_cli_opt_t;

/*
 * Function names should be directly correlated with the slurm_opt_t field
 * they manipulate. But the slurm_cli_opt_t name should always match that
 * of the long-form name for the argument itself.
 *
 * These should be alphabetized by the slurm_cli_opt_t name.
 */

static int arg_set__unknown_salloc(slurm_opt_t *opt, const char *arg)
{
	fprintf(stderr, "Try \"salloc --help\" for more information\n");

	return SLURM_ERROR;
}
static int arg_set__unknown_sbatch(slurm_opt_t *opt, const char *arg)
{
	fprintf(stderr,	"Try \"sbatch --help\" for more information\n");

	return SLURM_ERROR;
}
static int arg_set__unknown_srun(slurm_opt_t *opt, const char *arg)
{
	fprintf(stderr,	"Try \"srun --help\" for more information\n");

	return SLURM_ERROR;
}
static char *arg_get__unknown_(slurm_opt_t *opt)
{
	return NULL; /* no op */
}
static void arg_reset__unknown_(slurm_opt_t *opt)
{
	/* no op */
}
static slurm_cli_opt_t slurm_opt__unknown_ = {
	.name = NULL,
	.has_arg = no_argument,
	.val = '?',
	.set_func_salloc = arg_set__unknown_salloc,
	.set_func_sbatch = arg_set__unknown_sbatch,
	.set_func_srun = arg_set__unknown_srun,
	.get_func = arg_get__unknown_,
	.reset_func = arg_reset__unknown_,
};

static int arg_set_accel_bind_type(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	if (strchr(arg, 'v'))
		opt->srun_opt->accel_bind_type |= ACCEL_BIND_VERBOSE;
	if (strchr(arg, 'g'))
		opt->srun_opt->accel_bind_type |= ACCEL_BIND_CLOSEST_GPU;
	if (strchr(arg, 'n'))
		opt->srun_opt->accel_bind_type |= ACCEL_BIND_CLOSEST_NIC;

	if (!opt->srun_opt->accel_bind_type) {
		error("Invalid --accel-bind specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_accel_bind_type(slurm_opt_t *opt)
{
	char *tmp = NULL;

	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->srun_opt->accel_bind_type & ACCEL_BIND_VERBOSE)
		xstrcat(tmp, "v");
	if (opt->srun_opt->accel_bind_type & ACCEL_BIND_CLOSEST_GPU)
		xstrcat(tmp, "g");
	if (opt->srun_opt->accel_bind_type & ACCEL_BIND_CLOSEST_NIC)
		xstrcat(tmp, "n");

	return tmp;
}
static void arg_reset_accel_bind_type(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->accel_bind_type = 0;
}
static slurm_cli_opt_t slurm_opt_accel_bind = {
	.name = "accel-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_ACCEL_BIND,
	.set_func_srun = arg_set_accel_bind_type,
	.get_func = arg_get_accel_bind_type,
	.reset_func = arg_reset_accel_bind_type,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(account);
static slurm_cli_opt_t slurm_opt_account = {
	.name = "account",
	.has_arg = required_argument,
	.val = 'A',
	.set_func = arg_set_account,
	.get_func = arg_get_account,
	.reset_func = arg_reset_account,
};

static int arg_set_acctg_freq(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->acctg_freq);
	opt->acctg_freq = xstrdup(arg);
	if (validate_acctg_freq(opt->acctg_freq))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(acctg_freq);
static slurm_cli_opt_t slurm_opt_acctg_freq = {
	.name = "acctg-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_ACCTG_FREQ,
	.set_func = arg_set_acctg_freq,
	.get_func = arg_get_acctg_freq,
	.reset_func = arg_reset_acctg_freq,
};

static int arg_set_alloc_nodelist(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->srun_opt->alloc_nodelist);
	opt->srun_opt->alloc_nodelist = xstrdup(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_alloc_nodelist(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->srun_opt->alloc_nodelist);
}
static void arg_reset_alloc_nodelist(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		xfree(opt->srun_opt->alloc_nodelist);
}
static slurm_cli_opt_t slurm_opt_alloc_nodelist = {
	.name = NULL, /* envvar only */
	.has_arg = required_argument,
	.val = LONG_OPT_ALLOC_NODELIST,
	.set_func = arg_set_alloc_nodelist,
	.get_func = arg_get_alloc_nodelist,
	.reset_func = arg_reset_alloc_nodelist,
	.reset_each_pass = true,
};

COMMON_SBATCH_STRING_OPTION(array_inx);
static slurm_cli_opt_t slurm_opt_array = {
	.name = "array",
	.has_arg = required_argument,
	.val = 'a',
	.set_func_sbatch = arg_set_array_inx,
	.get_func = arg_get_array_inx,
	.reset_func = arg_reset_array_inx,
};

static char *arg_get_argv(slurm_opt_t *opt)
{
	char *argv_string = NULL;
	for (int i = 0; i < opt->argc; i++)
		xstrfmtcat(argv_string, " %s",
			   opt->argv[i]);
	return argv_string;
}
static void arg_reset_argv(slurm_opt_t *opt)
{
	for (int i = 0; i < opt->argc; i++)
		xfree(opt->argv[i]);
	xfree(opt->argv);
	opt->argc = 0;
}
static slurm_cli_opt_t slurm_opt_argv = {
	.name = "argv",
	.has_arg = required_argument,
	.val = LONG_OPT_ARGV,
	.get_func = arg_get_argv,
	.reset_func = arg_reset_argv,
};


COMMON_SBATCH_STRING_OPTION(batch_features);
static slurm_cli_opt_t slurm_opt_batch = {
	.name = "batch",
	.has_arg = required_argument,
	.val = LONG_OPT_BATCH,
	.set_func_sbatch = arg_set_batch_features,
	.get_func = arg_get_batch_features,
	.reset_func = arg_reset_batch_features,
};

COMMON_STRING_OPTION(burst_buffer_file);
static slurm_cli_opt_t slurm_opt_bbf = {
	.name = "bbf",
	.has_arg = required_argument,
	.val = LONG_OPT_BURST_BUFFER_FILE,
	.set_func_salloc = arg_set_burst_buffer_file,
	.set_func_sbatch = arg_set_burst_buffer_file,
	.set_func_srun = arg_set_burst_buffer_file,
	.get_func = arg_get_burst_buffer_file,
	.reset_func = arg_reset_burst_buffer_file,
};

static int arg_set_autocomplete(slurm_opt_t *opt, const char *arg)
{
	if (opt->autocomplete_func)
		(opt->autocomplete_func)(arg);

	exit(0);
	return SLURM_SUCCESS;
}
static char *arg_get_autocomplete(slurm_opt_t *opt)
{
	return NULL; /* no op */
}
static void arg_reset_autocomplete(slurm_opt_t *opt)
{
	/* no op */
}
static slurm_cli_opt_t slurm_opt_autocomplete = {
	.name = "autocomplete",
	.has_arg = required_argument,
	.val = LONG_OPT_COMPLETE_FLAG,
	.set_func = arg_set_autocomplete,
	.get_func = arg_get_autocomplete,
	.reset_func = arg_reset_autocomplete,
};

static int arg_set_bcast(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->bcast_flag = true;
	opt->srun_opt->bcast_file = xstrdup(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_bcast(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->srun_opt->bcast_flag && !opt->srun_opt->bcast_file)
		return xstrdup("set");
	else if (opt->srun_opt->bcast_flag)
		return xstrdup(opt->srun_opt->bcast_file);
	return NULL;
}
static void arg_reset_bcast(slurm_opt_t *opt)
{
	if (opt->srun_opt) {
		opt->srun_opt->bcast_flag = false;
		xfree(opt->srun_opt->bcast_file);
	}
}
static slurm_cli_opt_t slurm_opt_bcast = {
	.name = "bcast",
	.has_arg = optional_argument,
	.val = LONG_OPT_BCAST,
	.set_func_srun = arg_set_bcast,
	.get_func = arg_get_bcast,
	.reset_func = arg_reset_bcast,
	.reset_each_pass = true,
};

static int arg_set_bcast_exclude(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->srun_opt->bcast_exclude);
	opt->srun_opt->bcast_exclude = xstrdup(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_bcast_exclude(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->srun_opt->bcast_exclude)
		return xstrdup(opt->srun_opt->bcast_exclude);

	return NULL;
}
static void arg_reset_bcast_exclude(slurm_opt_t *opt)
{
	if (opt->srun_opt) {
		xfree(opt->srun_opt->bcast_exclude);
		opt->srun_opt->bcast_exclude =
			xstrdup(slurm_conf.bcast_exclude);
	}
}
static slurm_cli_opt_t slurm_opt_bcast_exclude = {
	.name = "bcast-exclude",
	.has_arg = required_argument,
	.val = LONG_OPT_BCAST_EXCLUDE,
	.set_func_srun = arg_set_bcast_exclude,
	.get_func = arg_get_bcast_exclude,
	.reset_func = arg_reset_bcast_exclude,
	.reset_each_pass = true,
};

static int arg_set_begin(slurm_opt_t *opt, const char *arg)
{
	if (!(opt->begin = parse_time(arg, 0))) {
		error("Invalid --begin specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_begin(slurm_opt_t *opt)
{
	char time_str[256];
	slurm_make_time_str(&opt->begin, time_str, sizeof(time_str));
	return xstrdup(time_str);
}
COMMON_OPTION_RESET(begin, 0);
static slurm_cli_opt_t slurm_opt_begin = {
	.name = "begin",
	.has_arg = required_argument,
	.val = 'b',
	.set_func_salloc = arg_set_begin,
	.set_func_sbatch = arg_set_begin,
	.set_func_srun = arg_set_begin,
	.get_func = arg_get_begin,
	.reset_func = arg_reset_begin,
};

/* Also see --no-bell below */
static int arg_set_bell(slurm_opt_t *opt, const char *arg)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_ALWAYS;

	return SLURM_SUCCESS;
}
static char *arg_get_bell(slurm_opt_t *opt)
{
	if (!opt->salloc_opt)
		return xstrdup("invalid-context");

	if (opt->salloc_opt->bell == BELL_ALWAYS)
		return xstrdup("bell-always");
	else if (opt->salloc_opt->bell == BELL_AFTER_DELAY)
		return xstrdup("bell-after-delay");
	else if (opt->salloc_opt->bell == BELL_NEVER)
		return xstrdup("bell-never");
	return NULL;
}
static void arg_reset_bell(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_AFTER_DELAY;
}
static slurm_cli_opt_t slurm_opt_bell = {
	.name = "bell",
	.has_arg = no_argument,
	.val = LONG_OPT_BELL,
	.set_func_salloc = arg_set_bell,
	.get_func = arg_get_bell,
	.reset_func = arg_reset_bell,
};

COMMON_STRING_OPTION(burst_buffer);
static slurm_cli_opt_t slurm_opt_bb = {
	.name = "bb",
	.has_arg = required_argument,
	.val = LONG_OPT_BURST_BUFFER_SPEC,
	.set_func_salloc = arg_set_burst_buffer,
	.set_func_sbatch = arg_set_burst_buffer,
	.set_func_srun = arg_set_burst_buffer,
	.get_func = arg_get_burst_buffer,
	.reset_func = arg_reset_burst_buffer,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(c_constraint);
static slurm_cli_opt_t slurm_opt_c_constraint = {
	.name = "cluster-constraint",
	.has_arg = required_argument,
	.val = LONG_OPT_CLUSTER_CONSTRAINT,
	.set_func_salloc = arg_set_c_constraint,
	.set_func_sbatch = arg_set_c_constraint,
	.set_func_srun = arg_set_c_constraint,
	.get_func = arg_get_c_constraint,
	.reset_func = arg_reset_c_constraint,
};

static int arg_set_chdir(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->chdir);
	if (is_full_path(arg))
		opt->chdir = xstrdup(arg);
	else
		opt->chdir = make_full_path(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(chdir);
static void arg_reset_chdir(slurm_opt_t *opt)
{
	char buf[PATH_MAX];
	xfree(opt->chdir);
	if (opt->salloc_opt || opt->scron_opt)
		return;

	if (!(getcwd(buf, PATH_MAX))) {
		error("getcwd failed: %m");
		exit(-1);
	}
	opt->chdir = xstrdup(buf);
}
static slurm_cli_opt_t slurm_opt_chdir = {
	.name = "chdir",
	.has_arg = required_argument,
	.val = 'D',
	.set_func = arg_set_chdir,
	.get_func = arg_get_chdir,
	.reset_func = arg_reset_chdir,
};

/* --clusters and --cluster are equivalent */
COMMON_STRING_OPTION(clusters);
static slurm_cli_opt_t slurm_opt_clusters = {
	.name = "clusters",
	.has_arg = required_argument,
	.val = 'M',
	.set_func_salloc = arg_set_clusters,
	.set_func_sbatch = arg_set_clusters,
	.set_func_srun = arg_set_clusters,
	.get_func = arg_get_clusters,
	.reset_func = arg_reset_clusters,
};
static slurm_cli_opt_t slurm_opt_cluster = {
	.name = "cluster",
	.has_arg = required_argument,
	.val = LONG_OPT_CLUSTER,
	.set_func_salloc = arg_set_clusters,
	.set_func_sbatch = arg_set_clusters,
	.set_func_srun = arg_set_clusters,
	.get_func = arg_get_clusters,
	.reset_func = arg_reset_clusters,
};

COMMON_STRING_OPTION(comment);
static slurm_cli_opt_t slurm_opt_comment = {
	.name = "comment",
	.has_arg = required_argument,
	.val = LONG_OPT_COMMENT,
	.set_func = arg_set_comment,
	.get_func = arg_get_comment,
	.reset_func = arg_reset_comment,
};

static int arg_set_compress(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->compress = parse_compress_type(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_compress(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->srun_opt->compress == COMPRESS_LZ4)
		return xstrdup("lz4");
	return xstrdup("none");
}
static void arg_reset_compress(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->compress = COMPRESS_OFF;
}
static slurm_cli_opt_t slurm_opt_compress = {
	.name = "compress",
	.has_arg = optional_argument,
	.val = LONG_OPT_COMPRESS,
	.set_func_srun = arg_set_compress,
	.get_func = arg_get_compress,
	.reset_func = arg_reset_compress,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(constraint);
static slurm_cli_opt_t slurm_opt_constraint = {
	.name = "constraint",
	.has_arg = required_argument,
	.val = 'C',
	.set_func = arg_set_constraint,
	.get_func = arg_get_constraint,
	.reset_func = arg_reset_constraint,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(container);
static slurm_cli_opt_t slurm_opt_container = {
	.name = "container",
	.has_arg = required_argument,
	.val = LONG_OPT_CONTAINER,
	.set_func = arg_set_container,
	.get_func = arg_get_container,
	.reset_func = arg_reset_container,
};

COMMON_STRING_OPTION(container_id);
static slurm_cli_opt_t slurm_opt_container_id = {
	.name = "container-id",
	.has_arg = required_argument,
	.val = LONG_OPT_CONTAINER_ID,
	.set_func = arg_set_container_id,
	.get_func = arg_get_container_id,
	.reset_func = arg_reset_container_id,
};

COMMON_STRING_OPTION_SET(context);
COMMON_STRING_OPTION_GET(context);
static void arg_reset_context(slurm_opt_t *opt)
{
	xfree(opt->context);

#ifdef WITH_SELINUX
	if (is_selinux_enabled() == 1) {
		char *context;
		getcon(&context);
		opt->context = xstrdup(context);
		freecon(context);
	}
#endif
}
static slurm_cli_opt_t slurm_opt_context = {
	.name = "context",
	.has_arg = required_argument,
	.val = LONG_OPT_CONTEXT,
	.set_func = arg_set_context,
	.get_func = arg_get_context,
	.reset_func = arg_reset_context,
};

COMMON_BOOL_OPTION(contiguous, "contiguous");
static slurm_cli_opt_t slurm_opt_contiguous = {
	.name = "contiguous",
	.has_arg = no_argument,
	.val = LONG_OPT_CONTIGUOUS,
	.set_func = arg_set_contiguous,
	.get_func = arg_get_contiguous,
	.reset_func = arg_reset_contiguous,
	.reset_each_pass = true,
};

static int arg_set_core_spec(slurm_opt_t *opt, const char *arg)
{
	if (opt->srun_opt)
		opt->srun_opt->core_spec_set = true;

	opt->core_spec = parse_int("--core-spec", arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_core_spec(slurm_opt_t *opt)
{
	if ((opt->core_spec == NO_VAL16) ||
	    (opt->core_spec & CORE_SPEC_THREAD))
		return xstrdup("unset");
	return xstrdup_printf("%d", opt->core_spec);
}
static void arg_reset_core_spec(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->core_spec_set = false;

	opt->core_spec = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_core_spec = {
	.name = "core-spec",
	.has_arg = required_argument,
	.val = 'S',
	.set_func = arg_set_core_spec,
	.get_func = arg_get_core_spec,
	.reset_func = arg_reset_core_spec,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(cores_per_socket, "--cores-per-socket");
COMMON_INT_OPTION_GET(cores_per_socket);
COMMON_OPTION_RESET(cores_per_socket, NO_VAL);
static slurm_cli_opt_t slurm_opt_cores_per_socket = {
	.name = "cores-per-socket",
	.has_arg = required_argument,
	.val = LONG_OPT_CORESPERSOCKET,
	.set_func = arg_set_cores_per_socket,
	.get_func = arg_get_cores_per_socket,
	.reset_func = arg_reset_cores_per_socket,
	.reset_each_pass = true,
};

COMMON_SRUN_STRING_OPTION_SET(cpu_bind);
COMMON_SRUN_STRING_OPTION_GET(cpu_bind);
static void arg_reset_cpu_bind(slurm_opt_t *opt)
{
	/*
	 * Both opt->srun_opt->cpu_bind and opt->srun_opt->cpu_bind_type must
	 * be reset.
	 */
	if (!opt->srun_opt)
		return;
	xfree(opt->srun_opt->cpu_bind);
	opt->srun_opt->cpu_bind_type = 0;
}
static slurm_cli_opt_t slurm_opt_cpu_bind = {
	.name = "cpu-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_CPU_BIND,
	.set_func_srun = arg_set_cpu_bind,
	.get_func = arg_get_cpu_bind,
	.reset_func = arg_reset_cpu_bind,
	.reset_each_pass = true,
};
/*
 * OpenMPI hard-coded --cpu_bind as part of their mpirun/mpiexec launch
 * scripting for a long time, and thus we're stuck supporting this deprecated
 * version indefinitely.
 *
 * Keep this after the preferred --cpu-bind handling so cli_filter sees that
 * and not this form.
 */
static slurm_cli_opt_t slurm_opt_cpu_underscore_bind = {
	.name = "cpu_bind",
	.has_arg = required_argument,
	.val = LONG_OPT_CPU_BIND,
	.set_func_srun = arg_set_cpu_bind,
	.get_func = arg_get_cpu_bind,
	.reset_func = arg_reset_cpu_bind,
	.reset_each_pass = true,
};

static int arg_set_cpu_freq(slurm_opt_t *opt, const char *arg)
{
	if (cpu_freq_verify_cmdline(arg, &opt->cpu_freq_min,
				    &opt->cpu_freq_max, &opt->cpu_freq_gov)) {
		error("Invalid --cpu-freq argument");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_cpu_freq(slurm_opt_t *opt)
{
	return cpu_freq_to_cmdline(opt->cpu_freq_min,
				   opt->cpu_freq_max,
				   opt->cpu_freq_gov);
}
static void arg_reset_cpu_freq(slurm_opt_t *opt)
{
	opt->cpu_freq_min = NO_VAL;
	opt->cpu_freq_max = NO_VAL;
	opt->cpu_freq_gov = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_cpu_freq = {
	.name = "cpu-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_CPU_FREQ,
	.set_func = arg_set_cpu_freq,
	.get_func = arg_get_cpu_freq,
	.reset_func = arg_reset_cpu_freq,
	.reset_each_pass = true,
};

COMMON_INT_OPTION(cpus_per_gpu, "--cpus-per-gpu");
static slurm_cli_opt_t slurm_opt_cpus_per_gpu = {
	.name = "cpus-per-gpu",
	.has_arg = required_argument,
	.val = LONG_OPT_CPUS_PER_GPU,
	.set_func = arg_set_cpus_per_gpu,
	.get_func = arg_get_cpus_per_gpu,
	.reset_func = arg_reset_cpus_per_gpu,
	.reset_each_pass = true,
};

static int arg_set_cpus_per_task(slurm_opt_t *opt, const char *arg)
{
	int old_cpus_per_task = opt->cpus_per_task;
	opt->cpus_per_task = parse_int("--cpus-per-task", arg, true);

	if (opt->cpus_set && opt->srun_opt &&
	    (old_cpus_per_task < opt->cpus_per_task))
		info("Job step's --cpus-per-task value exceeds that of job (%d > %d). Job step may never run.",
		     opt->cpus_per_task, old_cpus_per_task);

	opt->cpus_set = true;
	return SLURM_SUCCESS;
}

COMMON_INT_OPTION_GET(cpus_per_task);
static void arg_reset_cpus_per_task(slurm_opt_t *opt)
{
	opt->cpus_per_task = 0;
	opt->cpus_set = false;
}
static slurm_cli_opt_t slurm_opt_cpus_per_task = {
	.name = "cpus-per-task",
	.has_arg = required_argument,
	.val = 'c',
	.set_func = arg_set_cpus_per_task,
	.get_func = arg_get_cpus_per_task,
	.reset_func = arg_reset_cpus_per_task,
	.reset_each_pass = true,
};

static int arg_set_deadline(slurm_opt_t *opt, const char *arg)
{
	if (!(opt->deadline = parse_time(arg, 0))) {
		error("Invalid --deadline specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static char *arg_get_deadline(slurm_opt_t *opt)
{
	char time_str[256];
	slurm_make_time_str(&opt->deadline, time_str, sizeof(time_str));
	return xstrdup(time_str);
}
COMMON_OPTION_RESET(deadline, 0);
static slurm_cli_opt_t slurm_opt_deadline = {
	.name = "deadline",
	.has_arg = required_argument,
	.val = LONG_OPT_DEADLINE,
	.set_func = arg_set_deadline,
	.get_func = arg_get_deadline,
	.reset_func = arg_reset_deadline,
};

static int arg_set_debugger_test(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->debugger_test = true;

	return SLURM_SUCCESS;
}
static char *arg_get_debugger_test(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return NULL;

	return xstrdup(opt->srun_opt->debugger_test ? "set" : "unset");
}
static void arg_reset_debugger_test(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->debugger_test = false;
}
static slurm_cli_opt_t slurm_opt_debugger_test = {
	.name = "debugger-test",
	.has_arg = no_argument,
	.val = LONG_OPT_DEBUGGER_TEST,
	.set_func_srun = arg_set_debugger_test,
	.get_func = arg_get_debugger_test,
	.reset_func = arg_reset_debugger_test,
};

static int arg_set_delay_boot(slurm_opt_t *opt, const char *arg)
{
	if ((opt->delay_boot = time_str2secs(arg)) == NO_VAL) {
		error("Invalid --delay-boot specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static char *arg_get_delay_boot(slurm_opt_t *opt)
{
	char time_str[32];

	if (opt->delay_boot == NO_VAL)
		return NULL;
	secs2time_str(opt->delay_boot, time_str, sizeof(time_str));

	return xstrdup(time_str);
}
COMMON_OPTION_RESET(delay_boot, NO_VAL);
static slurm_cli_opt_t slurm_opt_delay_boot = {
	.name = "delay-boot",
	.has_arg = required_argument,
	.val = LONG_OPT_DELAY_BOOT,
	.set_func = arg_set_delay_boot,
	.get_func = arg_get_delay_boot,
	.reset_func = arg_reset_delay_boot,
};

static void arg_reset_environment(slurm_opt_t *opt)
{
	env_array_free(opt->environment);
	opt->environment = NULL;
}
static char *arg_get_environment(slurm_opt_t *opt)
{
	return NULL;
}
static slurm_cli_opt_t slurm_opt_environment = {
	.name = "environment",
	.val = LONG_OPT_ENVIRONMENT,
	.has_arg = required_argument,
	.get_func = arg_get_environment,
	.reset_func = arg_reset_environment,
};

COMMON_STRING_OPTION(dependency);
static slurm_cli_opt_t slurm_opt_dependency = {
	.name = "dependency",
	.has_arg = required_argument,
	.val = 'd',
	.set_func = arg_set_dependency,
	.get_func = arg_get_dependency,
	.reset_func = arg_reset_dependency,
};

COMMON_SRUN_BOOL_OPTION(disable_status);
static slurm_cli_opt_t slurm_opt_disable_status = {
	.name = "disable-status",
	.has_arg = no_argument,
	.val = 'X',
	.set_func_srun = arg_set_disable_status,
	.get_func = arg_get_disable_status,
	.reset_func = arg_reset_disable_status,
};

static int arg_set_distribution(slurm_opt_t *opt, const char *arg)
{
	opt->distribution = verify_dist_type(arg, &opt->plane_size);
	if (opt->distribution == SLURM_ERROR) {
		error("Invalid --distribution specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_distribution(slurm_opt_t *opt)
{
	char *dist = NULL;
	set_distribution(opt->distribution, &dist);
	if (opt->distribution == SLURM_DIST_PLANE)
		xstrfmtcat(dist, "=%u", opt->plane_size);
	return dist;
}
static void arg_reset_distribution(slurm_opt_t *opt)
{
	opt->distribution = SLURM_DIST_UNKNOWN;
	opt->plane_size = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_distribution = {
	.name = "distribution",
	.has_arg = required_argument,
	.val = 'm',
	.set_func = arg_set_distribution,
	.get_func = arg_get_distribution,
	.reset_func = arg_reset_distribution,
	.reset_each_pass = true,
};

COMMON_SRUN_STRING_OPTION(epilog);
static slurm_cli_opt_t slurm_opt_epilog = {
	.name = "epilog",
	.has_arg = required_argument,
	.val = LONG_OPT_EPILOG,
	.set_func_srun = arg_set_epilog,
	.get_func = arg_get_epilog,
	.reset_func = arg_reset_epilog,
};

static int arg_set_efname(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt && !opt->scron_opt && !opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->efname);
	if (!xstrcasecmp(arg, "none"))
		opt->efname = xstrdup("/dev/null");
	else
		opt->efname = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(efname);
COMMON_STRING_OPTION_RESET(efname);
static slurm_cli_opt_t slurm_opt_error = {
	.name = "error",
	.has_arg = required_argument,
	.val = 'e',
	.set_func_sbatch = arg_set_efname,
	.set_func_scron = arg_set_efname,
	.set_func_srun = arg_set_efname,
	.get_func = arg_get_efname,
	.reset_func = arg_reset_efname,
};

COMMON_STRING_OPTION(exclude);
static slurm_cli_opt_t slurm_opt_exclude = {
	.name = "exclude",
	.has_arg = required_argument,
	.val = 'x',
	.set_func = arg_set_exclude,
	.get_func = arg_get_exclude,
	.reset_func = arg_reset_exclude,
};

static int arg_set_exclusive(slurm_opt_t *opt, const char *arg)
{
	if (!arg || !xstrcasecmp(arg, "exclusive")) {
		if (opt->srun_opt) {
			opt->srun_opt->exclusive = true;
			opt->srun_opt->exact = true;
		}
		opt->shared = JOB_SHARED_NONE;
	} else if (!xstrcasecmp(arg, "oversubscribe")) {
		opt->shared = JOB_SHARED_OK;
	} else if (!xstrcasecmp(arg, "user")) {
		opt->shared = JOB_SHARED_USER;
	} else if (!xstrcasecmp(arg, "mcs")) {
		opt->shared = JOB_SHARED_MCS;
	} else if (!xstrcasecmp(arg, "topo")) {
		opt->shared = JOB_SHARED_TOPO;
	} else {
		error("Invalid --exclusive specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_exclusive(slurm_opt_t *opt)
{
	if (opt->shared == JOB_SHARED_NONE)
		return xstrdup("exclusive");
	if (opt->shared == JOB_SHARED_OK)
		return xstrdup("oversubscribe");
	if (opt->shared == JOB_SHARED_USER)
		return xstrdup("user");
	if (opt->shared == JOB_SHARED_MCS)
		return xstrdup("mcs");
	if (opt->shared == JOB_SHARED_TOPO)
		return xstrdup("topo");
	if (opt->shared == NO_VAL16)
		return xstrdup("unset");
	return NULL;
}
/* warning: shared with --oversubscribe below */
static void arg_reset_shared(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->exclusive = true;
	opt->shared = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_exclusive = {
	.name = "exclusive",
	.has_arg = optional_argument,
	.val = LONG_OPT_EXCLUSIVE,
	.set_func = arg_set_exclusive,
	.get_func = arg_get_exclusive,
	.reset_func = arg_reset_shared,
	.reset_each_pass = true,
};

COMMON_SRUN_BOOL_OPTION(exact);
static slurm_cli_opt_t slurm_opt_exact = {
	.name = "exact",
	.has_arg = no_argument,
	.val = LONG_OPT_EXACT,
	.set_func_srun = arg_set_exact,
	.get_func = arg_get_exact,
	.reset_func = arg_reset_exact,
};

static int arg_set_export(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt && !opt->scron_opt && !opt->srun_opt)
		return SLURM_ERROR;

	opt->export_env = xstrdup(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_export(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt && !opt->scron_opt && !opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->export_env);

	return NULL;
}
static void arg_reset_export(slurm_opt_t *opt)
{
	xfree(opt->export_env);
}
static slurm_cli_opt_t slurm_opt_export = {
	.name = "export",
	.has_arg = required_argument,
	.val = LONG_OPT_EXPORT,
	.set_func_sbatch = arg_set_export,
	.set_func_scron = arg_set_export,
	.set_func_srun = arg_set_export,
	.get_func = arg_get_export,
	.reset_func = arg_reset_export,
};

COMMON_SBATCH_STRING_OPTION(export_file);
static slurm_cli_opt_t slurm_opt_export_file = {
	.name = "export-file",
	.has_arg = required_argument,
	.val = LONG_OPT_EXPORT_FILE,
	.set_func_sbatch = arg_set_export_file,
	.get_func = arg_get_export_file,
	.reset_func = arg_reset_export_file,
};

COMMON_SRUN_BOOL_OPTION(external_launcher);
static slurm_cli_opt_t slurm_opt_external_launcher = {
	.name = "external-launcher",
	.has_arg = optional_argument,
	.val = LONG_OPT_EXTERNAL_LAUNCHER,
	.set_func_srun = arg_set_external_launcher,
	.get_func = arg_get_external_launcher,
	.reset_func = arg_reset_external_launcher,
};

COMMON_STRING_OPTION(extra);
static slurm_cli_opt_t slurm_opt_extra = {
	.name = "extra",
	.has_arg = required_argument,
	.val = LONG_OPT_EXTRA,
	.set_func = arg_set_extra,
	.get_func = arg_get_extra,
	.reset_func = arg_reset_extra,
};

static int arg_set_extra_node_info(slurm_opt_t *opt, const char *arg)
{
	cpu_bind_type_t *cpu_bind_type = NULL;

	if (opt->srun_opt)
		cpu_bind_type = &opt->srun_opt->cpu_bind_type;
	opt->extra_set = verify_socket_core_thread_count(arg,
							 &opt->sockets_per_node,
							 &opt->cores_per_socket,
							 &opt->threads_per_core,
							 cpu_bind_type);

	if (!opt->extra_set) {
		error("Invalid --extra-node-info specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_extra_node_info(slurm_opt_t *opt)
{
	char *tmp = NULL;
	if (opt->sockets_per_node != NO_VAL)
		xstrfmtcat(tmp, "%d", opt->sockets_per_node);
	if (opt->cores_per_socket != NO_VAL)
		xstrfmtcat(tmp, ":%d", opt->cores_per_socket);
	if (opt->threads_per_core != NO_VAL)
		xstrfmtcat(tmp, ":%d", opt->threads_per_core);

	if (!tmp)
		return xstrdup("unset");
	return tmp;
}
static void arg_reset_extra_node_info(slurm_opt_t *opt)
{
	opt->extra_set = false;
	opt->sockets_per_node = NO_VAL;
	opt->cores_per_socket = NO_VAL;
	opt->threads_per_core = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_extra_node_info = {
	.name = "extra-node-info",
	.has_arg = required_argument,
	.val = 'B',
	.set_func = arg_set_extra_node_info,
	.get_func = arg_get_extra_node_info,
	.reset_func = arg_reset_extra_node_info,
	.reset_each_pass = true,
};

static int arg_set_get_user_env(slurm_opt_t *opt, const char *arg)
{
	char *end_ptr;

	if (!arg) {
		opt->get_user_env_time = 0;
		return SLURM_SUCCESS;
	}

	opt->get_user_env_time = strtol(arg, &end_ptr, 10);

	if (!end_ptr || (end_ptr[0] == '\0'))
		return SLURM_SUCCESS;

	if ((end_ptr[0] == 's') || (end_ptr[0] == 'S'))
		opt->get_user_env_mode = 1;
	else if ((end_ptr[0] == 'l') || (end_ptr[0] == 'L'))
		opt->get_user_env_mode = 2;
	else {
		error("Invalid --get-user-env specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_get_user_env(slurm_opt_t *opt)
{
	if (opt->get_user_env_mode == 1)
		return xstrdup_printf("%dS", opt->get_user_env_time);
	else if (opt->get_user_env_mode == 2)
		return xstrdup_printf("%dL", opt->get_user_env_time);
	else if (opt->get_user_env_time != -1)
		return xstrdup_printf("%d", opt->get_user_env_time);
	return NULL;
}
static void arg_reset_get_user_env(slurm_opt_t *opt)
{
	opt->get_user_env_mode = -1;
	opt->get_user_env_time = -1;
}
static slurm_cli_opt_t slurm_opt_get_user_env = {
	.name = "get-user-env",
	.has_arg = optional_argument,
	.val = LONG_OPT_GET_USER_ENV,
	.set_func_sbatch = arg_set_get_user_env,
	.get_func = arg_get_get_user_env,
	.reset_func = arg_reset_get_user_env,
};

static int arg_set_gid(slurm_opt_t *opt, const char *arg)
{
	if (getuid() != 0) {
		error("--gid only permitted by root user");
		return SLURM_ERROR;
	}

	if (gid_from_string(arg, &opt->gid) < 0) {
		error("Invalid --gid specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET(gid);
COMMON_OPTION_RESET(gid, SLURM_AUTH_NOBODY);
static slurm_cli_opt_t slurm_opt_gid = {
	.name = "gid",
	.has_arg = required_argument,
	.val = LONG_OPT_GID,
	.set_func_sbatch = arg_set_gid,
	.get_func = arg_get_gid,
	.reset_func = arg_reset_gid,
};

static int arg_set_gpu_bind(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->gpu_bind);
	xfree(opt->tres_bind);
	opt->gpu_bind = xstrdup(arg);
	xstrfmtcat(opt->tres_bind, "gres/gpu:%s", opt->gpu_bind);
	if (tres_bind_verify_cmdline(opt->tres_bind)) {
		error("Invalid --gpu-bind argument: %s", opt->gpu_bind);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static void arg_reset_gpu_bind(slurm_opt_t *opt)
{
	xfree(opt->gpu_bind);
	xfree(opt->tres_bind);
}
COMMON_STRING_OPTION_GET(gpu_bind);
static slurm_cli_opt_t slurm_opt_gpu_bind = {
	.name = "gpu-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_GPU_BIND,
	.set_func = arg_set_gpu_bind,
	.get_func = arg_get_gpu_bind,
	.reset_func = arg_reset_gpu_bind,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(tres_bind);
static slurm_cli_opt_t slurm_opt_tres_bind = {
	.name = "tres-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_TRES_BIND,
	.set_func = arg_set_tres_bind,
	.get_func = arg_get_tres_bind,
	.reset_func = arg_reset_tres_bind,
	.reset_each_pass = true,
};

static int arg_set_gpu_freq(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->gpu_freq);
	xfree(opt->tres_freq);
	opt->gpu_freq = xstrdup(arg);
	xstrfmtcat(opt->tres_freq, "gpu:%s", opt->gpu_freq);
	if (tres_freq_verify_cmdline(opt->tres_freq)) {
		error("Invalid --gpu-freq argument: %s", opt->tres_freq);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static void arg_reset_gpu_freq(slurm_opt_t *opt)
{
	xfree(opt->gpu_freq);
	xfree(opt->tres_freq);
}
COMMON_STRING_OPTION_GET(gpu_freq);
static slurm_cli_opt_t slurm_opt_gpu_freq = {
	.name = "gpu-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_GPU_FREQ,
	.set_func = arg_set_gpu_freq,
	.get_func = arg_get_gpu_freq,
	.reset_func = arg_reset_gpu_freq,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(gpus);
static slurm_cli_opt_t slurm_opt_gpus = {
	.name = "gpus",
	.has_arg = required_argument,
	.val = 'G',
	.set_func = arg_set_gpus,
	.get_func = arg_get_gpus,
	.reset_func = arg_reset_gpus,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(gpus_per_node);
static slurm_cli_opt_t slurm_opt_gpus_per_node = {
	.name = "gpus-per-node",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_NODE,
	.set_func = arg_set_gpus_per_node,
	.get_func = arg_get_gpus_per_node,
	.reset_func = arg_reset_gpus_per_node,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(gpus_per_socket);
static slurm_cli_opt_t slurm_opt_gpus_per_socket = {
	.name = "gpus-per-socket",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_SOCKET,
	.set_func = arg_set_gpus_per_socket,
	.get_func = arg_get_gpus_per_socket,
	.reset_func = arg_reset_gpus_per_socket,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(gpus_per_task);
static slurm_cli_opt_t slurm_opt_gpus_per_task = {
	.name = "gpus-per-task",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_TASK,
	.set_func = arg_set_gpus_per_task,
	.get_func = arg_get_gpus_per_task,
	.reset_func = arg_reset_gpus_per_task,
	.reset_each_pass = true,
};

static int arg_set_tree_width(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	if (!xstrcasecmp(arg, "off")) {
		opt->srun_opt->tree_width = 0xfffd;
	} else if (parse_uint16((char *)arg, &opt->srun_opt->tree_width)) {
		error ("Invalid --treewidth value: %s", arg);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_tree_width(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("%u", opt->srun_opt->tree_width);
}
static void arg_reset_tree_width(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->tree_width = 0;
}
static slurm_cli_opt_t slurm_opt_tree_width = {
	.name = "treewidth",
	.has_arg = required_argument,
	.val = LONG_OPT_TREE_WIDTH,
	.set_func_srun = arg_set_tree_width,
	.get_func = arg_get_tree_width,
	.reset_func = arg_reset_tree_width,
};

COMMON_STRING_OPTION(tres_per_task);
static slurm_cli_opt_t slurm_opt_tres_per_task = {
	.name = "tres-per-task",
	.has_arg = required_argument,
	.val = LONG_OPT_TRES_PER_TASK,
	.set_func = arg_set_tres_per_task,
	.get_func = arg_get_tres_per_task,
	.reset_func = arg_reset_tres_per_task,
	.reset_each_pass = true,
};

static int arg_set_gres(slurm_opt_t *opt, const char *arg)
{
	if (!xstrcasecmp(arg, "help") || !xstrcasecmp(arg, "list")) {
		if (opt->scron_opt)
			return SLURM_ERROR;
		print_gres_help();
		exit(0);
	}

	xfree(opt->gres);
	/*
	 * Do not prepend "gres/" to none; none is handled specially by
	 * slurmctld to mean "do not copy the job's GRES to the step" -
	 * see _copy_job_tres_to_step()
	 */
	if (!xstrcasecmp(arg, "none"))
		opt->gres = xstrdup(arg);
	else
		opt->gres = gres_prepend_tres_type(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(gres);
static slurm_cli_opt_t slurm_opt_gres = {
	.name = "gres",
	.has_arg = required_argument,
	.val = LONG_OPT_GRES,
	.set_func = arg_set_gres,
	.get_func = arg_get_gres,
	.reset_func = arg_reset_gres,
	.reset_each_pass = true,
};

static int arg_set_gres_flags(slurm_opt_t *opt, const char *arg)
{
	char *tmp_str, *tok, *last = NULL;

	/* clear gres flag options first */
	opt->job_flags &= ~(GRES_DISABLE_BIND | GRES_ENFORCE_BIND |
			    GRES_ONE_TASK_PER_SHARING);

	if (!arg)
		return SLURM_ERROR;

	tmp_str = xstrdup(arg);
	tok = strtok_r(tmp_str, ",", &last);
	while (tok) {
		if (!xstrcasecmp(tok, "allow-task-sharing")) {
			if (!opt->srun_opt) {
				error("--gres-flags=allow-task-sharing is only used with srun.");
				xfree(tmp_str);
				return SLURM_ERROR;
			}
			opt->job_flags |= GRES_ALLOW_TASK_SHARING;
		} else if (!xstrcasecmp(tok, "disable-binding")) {
			opt->job_flags |= GRES_DISABLE_BIND;
		} else if (!xstrcasecmp(tok, "enforce-binding")) {
			opt->job_flags |= GRES_ENFORCE_BIND;
		} else if (!xstrcasecmp(tok, "multiple-tasks-per-sharing")) {
			opt->job_flags |= GRES_MULT_TASKS_PER_SHARING;
		} else if (!xstrcasecmp(tok, "one-task-per-sharing")) {
			opt->job_flags |= GRES_ONE_TASK_PER_SHARING;
		} else {
			error("Invalid --gres-flags specification: %s", tok);
			xfree(tmp_str);
			return SLURM_ERROR;
		}
		tok = strtok_r(NULL, ",", &last);
	}
	xfree(tmp_str);

	if ((opt->job_flags & GRES_DISABLE_BIND) &&
	    (opt->job_flags & GRES_ENFORCE_BIND)) {
		error("Invalid --gres-flags combo: disable-binding and enforce-binding are mutually exclusive.");
		return SLURM_ERROR;
	}

	if ((opt->job_flags & GRES_MULT_TASKS_PER_SHARING) &&
	    (opt->job_flags & GRES_ONE_TASK_PER_SHARING)) {
		error("Invalid --gres-flags combo: one-task-per-sharing and multiple-tasks-per-sharing are mutually exclusive.");
		return SLURM_ERROR;
	}

	if ((opt->job_flags & GRES_ONE_TASK_PER_SHARING) &&
	    !(slurm_conf.select_type_param & MULTIPLE_SHARING_GRES_PJ)) {
		error("In order to use --gres-flags=one-task-per-sharing you must also have SelectTypeParameters=MULTIPLE_SHARING_GRES_PJ in your slurm.conf");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_gres_flags(slurm_opt_t *opt)
{
	char *tmp = NULL, *tmp_pos = NULL;

	if (opt->job_flags & GRES_ALLOW_TASK_SHARING)
		xstrcatat(tmp, &tmp_pos, "allow-task-sharing,");
	if (opt->job_flags & GRES_DISABLE_BIND)
		xstrcatat(tmp, &tmp_pos, "disable-binding,");
	if (opt->job_flags & GRES_ENFORCE_BIND)
		xstrcatat(tmp, &tmp_pos, "enforce-binding,");
	if (opt->job_flags & GRES_MULT_TASKS_PER_SHARING)
		xstrcatat(tmp, &tmp_pos, "multiple-tasks-per-sharing,");
	if (opt->job_flags & GRES_ONE_TASK_PER_SHARING)
		xstrcatat(tmp, &tmp_pos, "one-task-per-sharing,");

	if (!tmp_pos)
		xstrcat(tmp, "unset");
	else {
		tmp_pos--;
		tmp_pos[0] = '\0'; /* remove trailing ',' */
	}

	return tmp;
}
static void arg_reset_gres_flags(slurm_opt_t *opt)
{
	opt->job_flags &= ~(GRES_DISABLE_BIND);
	opt->job_flags &= ~(GRES_ENFORCE_BIND);
	opt->job_flags &= ~(GRES_MULT_TASKS_PER_SHARING);
	opt->job_flags &= ~(GRES_ONE_TASK_PER_SHARING);
}
static slurm_cli_opt_t slurm_opt_gres_flags = {
	.name = "gres-flags",
	.has_arg = required_argument,
	.val = LONG_OPT_GRES_FLAGS,
	.set_func = arg_set_gres_flags,
	.get_func = arg_get_gres_flags,
	.reset_func = arg_reset_gres_flags,
	.reset_each_pass = true,
};

static int arg_set_help(slurm_opt_t *opt, const char *arg)
{
	if (opt->scron_opt)
		return SLURM_ERROR;

	if (opt->help_func)
		(opt->help_func)();
	else
		error("Could not find --help message");

	exit(0);
	return SLURM_SUCCESS;
}
static char *arg_get_help(slurm_opt_t *opt)
{
	return NULL; /* no op */
}
static void arg_reset_help(slurm_opt_t *opt)
{
	/* no op */
}
static slurm_cli_opt_t slurm_opt_help = {
	.name = "help",
	.has_arg = no_argument,
	.val = 'h',
	.sbatch_early_pass = true,
	.set_func = arg_set_help,
	.get_func = arg_get_help,
	.reset_func = arg_reset_help,
};

COMMON_STRING_OPTION(hint);
static slurm_cli_opt_t slurm_opt_hint = {
	.name = "hint",
	.has_arg = required_argument,
	.val = LONG_OPT_HINT,
	.set_func = arg_set_hint,
	.get_func = arg_get_hint,
	.reset_func = arg_reset_hint,
	.reset_each_pass = true,
};

COMMON_BOOL_OPTION(hold, "hold");
static slurm_cli_opt_t slurm_opt_hold = {
	.name = "hold",
	.has_arg = no_argument,
	.val = 'H',
	.set_func_salloc = arg_set_hold,
	.set_func_sbatch = arg_set_hold,
	.set_func_srun = arg_set_hold,
	.get_func = arg_get_hold,
	.reset_func = arg_reset_hold,
};

static int arg_set_ignore_pbs(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->ignore_pbs = true;

	return SLURM_SUCCESS;
}
static char *arg_get_ignore_pbs(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->sbatch_opt->ignore_pbs ? "set" : "unset");
}
static void arg_reset_ignore_pbs(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->ignore_pbs = false;
}
static slurm_cli_opt_t slurm_opt_ignore_pbs = {
	.name = "ignore-pbs",
	.has_arg = no_argument,
	.val = LONG_OPT_IGNORE_PBS,
	.set_func_sbatch = arg_set_ignore_pbs,
	.get_func = arg_get_ignore_pbs,
	.reset_func = arg_reset_ignore_pbs,
};

static int arg_set_immediate(slurm_opt_t *opt, const char *arg)
{
	if (opt->sbatch_opt)
		return SLURM_ERROR;

	if (arg)
		opt->immediate = parse_int("immediate", arg, false);
	else
		opt->immediate = DEFAULT_IMMEDIATE;

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(immediate);
static slurm_cli_opt_t slurm_opt_immediate = {
	.name = "immediate",
	.has_arg = optional_argument,
	.val = 'I',
	.set_func_salloc = arg_set_immediate,
	.set_func_srun = arg_set_immediate,
	.get_func = arg_get_immediate,
	.reset_func = arg_reset_immediate,
};

static int arg_set_ifname(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt && !opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->ifname);
	if (!xstrcasecmp(arg, "none"))
		opt->ifname = xstrdup("/dev/null");
	else
		opt->ifname = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(ifname);
COMMON_STRING_OPTION_RESET(ifname);
static slurm_cli_opt_t slurm_opt_input = {
	.name = "input",
	.has_arg = required_argument,
	.val = 'i',
	.set_func_sbatch = arg_set_ifname,
	.set_func_scron = arg_set_ifname,
	.set_func_srun = arg_set_ifname,
	.get_func = arg_get_ifname,
	.reset_func = arg_reset_ifname,
};

COMMON_SRUN_BOOL_OPTION(interactive);
static slurm_cli_opt_t slurm_opt_interactive = {
	.name = "interactive",
	.has_arg = no_argument,
	.val = LONG_OPT_INTERACTIVE,
	.set_func_srun = arg_set_interactive,
	.get_func = arg_get_interactive,
	.reset_func = arg_reset_interactive,
};

static int arg_set_jobid(slurm_opt_t *opt, const char *arg)
{
	slurm_selected_step_t *step;
	char *job;

	if (!opt->srun_opt)
		return SLURM_ERROR;

	job = xstrdup(arg);
	/* will modify job, thus the xstrdup() from arg */
	step = slurm_parse_step_str(job);
	opt->srun_opt->jobid = step->step_id.job_id;
	opt->srun_opt->array_task_id = step->array_task_id;
	xfree(job);
	slurm_destroy_selected_step(step);

	return SLURM_SUCCESS;
}
static char *arg_get_jobid(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return NULL;

	if (opt->srun_opt->jobid == NO_VAL)
		return xstrdup("unset");

	return xstrdup_printf("%d", opt->srun_opt->jobid);
}
static void arg_reset_jobid(slurm_opt_t *opt)
{
	if (opt->srun_opt) {
		opt->srun_opt->jobid = NO_VAL;
		opt->srun_opt->array_task_id = NO_VAL;
	}
}
static slurm_cli_opt_t slurm_opt_jobid = {
	.name = "jobid",
	.has_arg = required_argument,
	.val = LONG_OPT_JOBID,
	.set_func_srun = arg_set_jobid,
	.get_func = arg_get_jobid,
	.reset_func = arg_reset_jobid,
};

COMMON_STRING_OPTION(job_name);
static slurm_cli_opt_t slurm_opt_job_name = {
	.name = "job-name",
	.has_arg = required_argument,
	.val = 'J',
	.set_func = arg_set_job_name,
	.get_func = arg_get_job_name,
	.reset_func = arg_reset_job_name,
};

static int arg_set_kill_command(slurm_opt_t *opt, const char *arg)
{
	if (!opt->salloc_opt)
		return SLURM_ERROR;

	/* Optional argument, enables default of SIGTERM if not given. */
	if (!arg) {
		opt->salloc_opt->kill_command_signal = SIGTERM;
		return SLURM_SUCCESS;
	}

	if (!(opt->salloc_opt->kill_command_signal = sig_name2num(arg))) {
		error("Invalid --kill-command specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_kill_command(slurm_opt_t *opt)
{
	if (!opt->salloc_opt)
		return NULL;

	return sig_num2name(opt->salloc_opt->kill_command_signal);
}
static void arg_reset_kill_command(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->kill_command_signal = 0;
}
static slurm_cli_opt_t slurm_opt_kill_command = {
	.name = "kill-command",
	.has_arg = optional_argument,
	.val = 'K',
	.set_func_salloc = arg_set_kill_command,
	.get_func = arg_get_kill_command,
	.reset_func = arg_reset_kill_command,
};

static int arg_set_kill_on_bad_exit(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	if (!arg) {
		opt->srun_opt->kill_bad_exit = 1;
		return SLURM_SUCCESS;
	}

	opt->srun_opt->kill_bad_exit = parse_int("--kill-on-bad-exit",
						 arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_kill_on_bad_exit(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return NULL;

	return xstrdup_printf("%d", opt->srun_opt->kill_bad_exit);
}
static void arg_reset_kill_on_bad_exit(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->kill_bad_exit = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_kill_on_bad_exit = {
	.name = "kill-on-bad-exit",
	.has_arg = optional_argument,
	.val = 'K',
	.set_func_srun = arg_set_kill_on_bad_exit,
	.get_func = arg_get_kill_on_bad_exit,
	.reset_func = arg_reset_kill_on_bad_exit,
};

static int arg_set_kill_on_invalid_dep(slurm_opt_t *opt, const char *arg)
{
	if (!xstrcasecmp(arg, "yes"))
		opt->job_flags |= KILL_INV_DEP;
	else if (!xstrcasecmp(arg, "no"))
		opt->job_flags |= NO_KILL_INV_DEP;
	else {
		error("Invalid --kill-on-invalid-dep specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_kill_on_invalid_dep(slurm_opt_t *opt)
{
	if (opt->job_flags & KILL_INV_DEP)
		return xstrdup("yes");
	else if (opt->job_flags & NO_KILL_INV_DEP)
		return xstrdup("no");
	return xstrdup("unset");
}
static void arg_reset_kill_on_invalid_dep(slurm_opt_t *opt)
{
	opt->job_flags &= ~KILL_INV_DEP;
	opt->job_flags &= ~NO_KILL_INV_DEP;
}
static slurm_cli_opt_t slurm_opt_kill_on_invalid_dep = {
	.name = "kill-on-invalid-dep",
	.has_arg = required_argument,
	.val = LONG_OPT_KILL_INV_DEP,
	.set_func_sbatch = arg_set_kill_on_invalid_dep,
	.get_func = arg_get_kill_on_invalid_dep,
	.reset_func = arg_reset_kill_on_invalid_dep,
};

COMMON_SRUN_BOOL_OPTION(labelio);
static slurm_cli_opt_t slurm_opt_label = {
	.name = "label",
	.has_arg = no_argument,
	.val = 'l',
	.set_func_srun = arg_set_labelio,
	.get_func = arg_get_labelio,
	.reset_func = arg_reset_labelio,
};

COMMON_STRING_OPTION(licenses);
static slurm_cli_opt_t slurm_opt_licenses = {
	.name = "licenses",
	.has_arg = required_argument,
	.val = 'L',
	.set_func = arg_set_licenses,
	.get_func = arg_get_licenses,
	.reset_func = arg_reset_licenses,
	.reset_each_pass = true,
};

static int arg_set_mail_type(slurm_opt_t *opt, const char *arg)
{
	opt->mail_type |= parse_mail_type(arg);
	if (opt->mail_type == INFINITE16) {
		error("Invalid --mail-type specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_mail_type(slurm_opt_t *opt)
{
	return xstrdup(print_mail_type(opt->mail_type));
}
COMMON_OPTION_RESET(mail_type, 0);
static slurm_cli_opt_t slurm_opt_mail_type = {
	.name = "mail-type",
	.has_arg = required_argument,
	.val = LONG_OPT_MAIL_TYPE,
	.set_func = arg_set_mail_type,
	.get_func = arg_get_mail_type,
	.reset_func = arg_reset_mail_type,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(mail_user);
static slurm_cli_opt_t slurm_opt_mail_user = {
	.name = "mail-user",
	.has_arg = required_argument,
	.val = LONG_OPT_MAIL_USER,
	.set_func = arg_set_mail_user,
	.get_func = arg_get_mail_user,
	.reset_func = arg_reset_mail_user,
	.reset_each_pass = true,
};

static int arg_set_max_threads(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->max_threads = parse_int("--threads", arg, true);

	if (opt->srun_opt->max_threads > SRUN_MAX_THREADS)
		error("Thread value --threads=%d exceeds recommended limit of %d",
		      opt->srun_opt->max_threads, SRUN_MAX_THREADS);

	return SLURM_SUCCESS;
}
static char *arg_get_max_threads(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("%d", opt->srun_opt->max_threads);
}
static void arg_reset_max_threads(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->max_threads = SRUN_MAX_THREADS;
}
static slurm_cli_opt_t slurm_opt_max_threads = {
	.name = "threads",
	.has_arg = required_argument,
	.val = 'T',
	.set_func_srun = arg_set_max_threads,
	.get_func = arg_get_max_threads,
	.reset_func = arg_reset_max_threads,
};

COMMON_STRING_OPTION(mcs_label);
static slurm_cli_opt_t slurm_opt_mcs_label = {
	.name = "mcs-label",
	.has_arg = required_argument,
	.val = LONG_OPT_MCS_LABEL,
	.set_func = arg_set_mcs_label,
	.get_func = arg_get_mcs_label,
	.reset_func = arg_reset_mcs_label,
};

static int arg_set_oom_kill_step(slurm_opt_t *opt, const char *arg)
{
	uint16_t res;
	if (!arg) {
		opt->oom_kill_step = 1;
		return SLURM_SUCCESS;
	}
	if (!parse_uint16((char *)arg, &res) && (res <= 1)) {
		opt->oom_kill_step = res;
		return SLURM_SUCCESS;
	}

	error("Invalid --oom-kill-step specification");
	return SLURM_ERROR;
}

static char *arg_get_oom_kill_step(slurm_opt_t *opt)
{
	if (opt->oom_kill_step == NO_VAL16)
		return xstrdup("unset");

	return xstrdup_printf("%u", opt->oom_kill_step);
}

COMMON_OPTION_RESET(oom_kill_step, NO_VAL16);
static slurm_cli_opt_t slurm_opt_oom_kill_step = {
	.name = "oom-kill-step",
	.has_arg = optional_argument,
	.val = LONG_OPT_OOMKILLSTEP,
	.set_func = arg_set_oom_kill_step,
	.get_func = arg_get_oom_kill_step,
	.reset_func = arg_reset_oom_kill_step,
};

static int arg_set_mem(slurm_opt_t *opt, const char *arg)
{
	if ((opt->pn_min_memory = str_to_mbytes(arg)) == NO_VAL64) {
		error("Invalid --mem specification");
		return SLURM_ERROR;
	}

	/*
	 * FIXME: the srun command silently stomps on any --mem-per-cpu
	 * setting, as it was likely inherited from the env var.
	 */
	if (opt->srun_opt)
		opt->mem_per_cpu = NO_VAL64;

	return SLURM_SUCCESS;
}
COMMON_MBYTES_OPTION_GET_AND_RESET(pn_min_memory);
static slurm_cli_opt_t slurm_opt_mem = {
	.name = "mem",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM,
	.set_func = arg_set_mem,
	.get_func = arg_get_pn_min_memory,
	.reset_func = arg_reset_pn_min_memory,
};

static int arg_set_mem_bind(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->mem_bind);
	if (slurm_verify_mem_bind(arg, &opt->mem_bind, &opt->mem_bind_type))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}
static char *arg_get_mem_bind(slurm_opt_t *opt)
{
	char *tmp;
	if (!opt->mem_bind_type)
		return xstrdup("unset");
	tmp = slurm_xstr_mem_bind_type(opt->mem_bind_type);
	if (opt->mem_bind)
		xstrfmtcat(tmp, ":%s", opt->mem_bind);
	return tmp;
}
static void arg_reset_mem_bind(slurm_opt_t *opt)
{
	xfree(opt->mem_bind);
	opt->mem_bind_type = 0;

	if (opt->srun_opt) {
		if (xstrstr(slurm_conf.launch_params, "mem_sort"))
			opt->mem_bind_type |= MEM_BIND_SORT;
	}
}
static slurm_cli_opt_t slurm_opt_mem_bind = {
	.name = "mem-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_BIND,
	.set_func = arg_set_mem_bind,
	.get_func = arg_get_mem_bind,
	.reset_func = arg_reset_mem_bind,
	.reset_each_pass = true,
};

COMMON_MBYTES_OPTION(mem_per_cpu, --mem-per-cpu);
static slurm_cli_opt_t slurm_opt_mem_per_cpu = {
	.name = "mem-per-cpu",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_PER_CPU,
	.set_func = arg_set_mem_per_cpu,
	.get_func = arg_get_mem_per_cpu,
	.reset_func = arg_reset_mem_per_cpu,
	.reset_each_pass = true,
};

COMMON_MBYTES_OPTION(mem_per_gpu, --mem-per-gpu);
static slurm_cli_opt_t slurm_opt_mem_per_gpu = {
	.name = "mem-per-gpu",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_PER_GPU,
	.set_func = arg_set_mem_per_gpu,
	.get_func = arg_get_mem_per_gpu,
	.reset_func = arg_reset_mem_per_gpu,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(pn_min_cpus, "--mincpus");
COMMON_INT_OPTION_GET(pn_min_cpus);
COMMON_OPTION_RESET(pn_min_cpus, -1);
static slurm_cli_opt_t slurm_opt_mincpus = {
	.name = "mincpus",
	.has_arg = required_argument,
	.val = LONG_OPT_MINCPUS,
	.set_func = arg_set_pn_min_cpus,
	.get_func = arg_get_pn_min_cpus,
	.reset_func = arg_reset_pn_min_cpus,
	.reset_each_pass = true,
};

COMMON_SRUN_STRING_OPTION(mpi_type);
static slurm_cli_opt_t slurm_opt_mpi = {
	.name = "mpi",
	.has_arg = required_argument,
	.val = LONG_OPT_MPI,
	.set_func_srun = arg_set_mpi_type,
	.get_func = arg_get_mpi_type,
	.reset_func = arg_reset_mpi_type,
};

static int arg_set_msg_timeout(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->msg_timeout = parse_int("--msg-timeout", arg, true);

	return SLURM_SUCCESS;
}
static char *arg_get_msg_timeout(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("%d", opt->srun_opt->msg_timeout);
}
static void arg_reset_msg_timeout(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->msg_timeout = slurm_conf.msg_timeout;
}
static slurm_cli_opt_t slurm_opt_msg_timeout = {
	.name = "msg-timeout",
	.has_arg = required_argument,
	.val = LONG_OPT_MSG_TIMEOUT,
	.set_func_srun = arg_set_msg_timeout,
	.get_func = arg_get_msg_timeout,
	.reset_func = arg_reset_msg_timeout,
};

COMMON_SRUN_BOOL_OPTION(multi_prog);
static slurm_cli_opt_t slurm_opt_multi_prog = {
	.name = "multi-prog",
	.has_arg = no_argument,
	.val = LONG_OPT_MULTI,
	.set_func_srun = arg_set_multi_prog,
	.get_func = arg_get_multi_prog,
	.reset_func = arg_reset_multi_prog,
};

COMMON_STRING_OPTION(network);
static slurm_cli_opt_t slurm_opt_network = {
	.name = "network",
	.has_arg = required_argument,
	.val = LONG_OPT_NETWORK,
	.set_func = arg_set_network,
	.get_func = arg_get_network,
	.reset_func = arg_reset_network,
	.reset_each_pass = true,
};

static int arg_set_nice(slurm_opt_t *opt, const char *arg)
{
	long long tmp_nice;

	if (arg)
		tmp_nice = strtoll(arg, NULL, 10);
	else
		tmp_nice = 100;

	if (llabs(tmp_nice) > (NICE_OFFSET - 3)) {
		error("Invalid --nice value, out of range (+/- %u)",
		      NICE_OFFSET - 3);
		return SLURM_ERROR;
	}

	opt->nice = (int) tmp_nice;

	return SLURM_SUCCESS;
}
static char *arg_get_nice(slurm_opt_t *opt)
{
	return xstrdup_printf("%d", opt->nice);
}
COMMON_OPTION_RESET(nice, NO_VAL);
static slurm_cli_opt_t slurm_opt_nice = {
	.name = "nice",
	.has_arg = optional_argument,
	.val = LONG_OPT_NICE,
	.set_func = arg_set_nice,
	.get_func = arg_get_nice,
	.reset_func = arg_reset_nice,
};

COMMON_SRUN_BOOL_OPTION(no_alloc);
static slurm_cli_opt_t slurm_opt_no_allocate = {
	.name = "no-allocate",
	.has_arg = no_argument,
	.val = 'Z',
	.set_func_srun = arg_set_no_alloc,
	.get_func = arg_get_no_alloc,
	.reset_func = arg_reset_no_alloc,
};

/* See --bell above as well */
static int arg_set_no_bell(slurm_opt_t *opt, const char *arg)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_NEVER;

	return SLURM_SUCCESS;
}
static slurm_cli_opt_t slurm_opt_no_bell = {
	.name = "no-bell",
	.has_arg = no_argument,
	.val = LONG_OPT_NO_BELL,
	.set_func_salloc = arg_set_no_bell,
	.get_func = arg_get_bell,
	.reset_func = arg_reset_bell,
};

static int arg_set_no_kill(slurm_opt_t *opt, const char *arg)
{
	if (!arg || !xstrcasecmp(arg, "set"))
		opt->no_kill = true;
	else if (!xstrcasecmp(arg, "off") || !xstrcasecmp(arg, "no"))
		opt->no_kill = false;
	else {
		error("Invalid --no-kill specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_no_kill(slurm_opt_t *opt)
{
	return xstrdup(opt->no_kill ? "set" : "unset");
}
COMMON_OPTION_RESET(no_kill, false);
static slurm_cli_opt_t slurm_opt_no_kill = {
	.name = "no-kill",
	.has_arg = optional_argument,
	.val = 'k',
	.set_func = arg_set_no_kill,
	.get_func = arg_get_no_kill,
	.reset_func = arg_reset_no_kill,
};

/* see --requeue below as well */
static int arg_set_no_requeue(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->requeue = 0;

	return SLURM_SUCCESS;
}
static char *arg_get_requeue(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt)
		return xstrdup("invalid-context");

	if (opt->sbatch_opt->requeue == NO_VAL)
		return xstrdup("unset");
	else if (opt->sbatch_opt->requeue == 0)
		return xstrdup("no-requeue");
	return xstrdup("requeue");
}
static void arg_reset_requeue(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->requeue = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_no_requeue = {
	.name = "no-requeue",
	.has_arg = no_argument,
	.val = LONG_OPT_NO_REQUEUE,
	.set_func_sbatch = arg_set_no_requeue,
	.get_func = arg_get_requeue,
	.reset_func = arg_reset_requeue,
};

static int arg_set_no_shell(slurm_opt_t *opt, const char *arg)
{
	if (opt->salloc_opt)
		opt->salloc_opt->no_shell = true;

	return SLURM_SUCCESS;
}
static char *arg_get_no_shell(slurm_opt_t *opt)
{
	if (!opt->salloc_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->salloc_opt->no_shell ? "set" : "unset");
}
static void arg_reset_no_shell(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->no_shell = false;
}
static slurm_cli_opt_t slurm_opt_no_shell = {
	.name = "no-shell",
	.has_arg = no_argument,
	.val = LONG_OPT_NO_SHELL,
	.set_func_salloc = arg_set_no_shell,
	.get_func = arg_get_no_shell,
	.reset_func = arg_reset_no_shell,
};

/*
 * FIXME: --nodefile and --nodelist options should be mutually exclusive.
 * Right now they'll overwrite one another; the last to run wins.
 */
static int arg_set_nodefile(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->nodefile);
	xfree(opt->nodelist);
	opt->nodefile = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(nodefile);
static slurm_cli_opt_t slurm_opt_nodefile = {
	.name = "nodefile",
	.has_arg = required_argument,
	.val = 'F',
	.set_func = arg_set_nodefile,
	.get_func = arg_get_nodefile,
	.reset_func = arg_reset_nodefile,
	.reset_each_pass = true,
};

static int arg_set_nodelist(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->nodefile);
	xfree(opt->nodelist);
	opt->nodelist = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(nodelist);
static slurm_cli_opt_t slurm_opt_nodelist = {
	.name = "nodelist",
	.has_arg = required_argument,
	.val = 'w',
	.set_func = arg_set_nodelist,
	.get_func = arg_get_nodelist,
	.reset_func = arg_reset_nodelist,
	.reset_each_pass = true,
};

static int arg_set_nodes(slurm_opt_t *opt, const char *arg)
{
	if (!(opt->nodes_set = verify_node_count(arg, &opt->min_nodes,
						 &opt->max_nodes,
						 &opt->job_size_str)))
		return SLURM_ERROR;
	return SLURM_SUCCESS;
}

static char *arg_get_nodes(slurm_opt_t *opt)
{
	if (opt->min_nodes != opt->max_nodes)
		return xstrdup_printf("%d-%d", opt->min_nodes, opt->max_nodes);
	return xstrdup_printf("%d", opt->min_nodes);
}
static void arg_reset_nodes(slurm_opt_t *opt)
{
	opt->min_nodes = 1;
	opt->max_nodes = 0;
	opt->nodes_set = false;
}
static slurm_cli_opt_t slurm_opt_nodes = {
	.name = "nodes",
	.has_arg = required_argument,
	.val = 'N',
	.set_func = arg_set_nodes,
	.get_func = arg_get_nodes,
	.reset_func = arg_reset_nodes,
	.reset_each_pass = true,
};

static int arg_set_ntasks(slurm_opt_t *opt, const char *arg)
{
	opt->ntasks = parse_int("--ntasks", arg, true);
	opt->ntasks_set = true;
	opt->ntasks_opt_set = true;
	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET(ntasks);
static void arg_reset_ntasks(slurm_opt_t *opt)
{
	opt->ntasks = 1;
	opt->ntasks_set = false;
	opt->ntasks_opt_set = false;
}
static slurm_cli_opt_t slurm_opt_ntasks = {
	.name = "ntasks",
	.has_arg = required_argument,
	.val = 'n',
	.set_func = arg_set_ntasks,
	.get_func = arg_get_ntasks,
	.reset_func = arg_reset_ntasks,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(ntasks_per_core, "--ntasks-per-core");
COMMON_INT_OPTION_GET(ntasks_per_core);
COMMON_OPTION_RESET(ntasks_per_core, NO_VAL);
static slurm_cli_opt_t slurm_opt_ntasks_per_core = {
	.name = "ntasks-per-core",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERCORE,
	.set_func = arg_set_ntasks_per_core,
	.get_func = arg_get_ntasks_per_core,
	.reset_func = arg_reset_ntasks_per_core,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(ntasks_per_node, "--ntasks-per-node");
COMMON_INT_OPTION_GET(ntasks_per_node);
COMMON_OPTION_RESET(ntasks_per_node, NO_VAL);
static slurm_cli_opt_t slurm_opt_ntasks_per_node = {
	.name = "ntasks-per-node",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERNODE,
	.set_func = arg_set_ntasks_per_node,
	.get_func = arg_get_ntasks_per_node,
	.reset_func = arg_reset_ntasks_per_node,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(ntasks_per_socket, "--ntasks-per-socket");
COMMON_INT_OPTION_GET(ntasks_per_socket);
COMMON_OPTION_RESET(ntasks_per_socket, NO_VAL);
static slurm_cli_opt_t slurm_opt_ntasks_per_socket = {
	.name = "ntasks-per-socket",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERSOCKET,
	.set_func = arg_set_ntasks_per_socket,
	.get_func = arg_get_ntasks_per_socket,
	.reset_func = arg_reset_ntasks_per_socket,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(ntasks_per_tres, "--ntasks-per-tres");
COMMON_INT_OPTION_GET(ntasks_per_tres);
COMMON_OPTION_RESET(ntasks_per_tres, NO_VAL);
static slurm_cli_opt_t slurm_opt_ntasks_per_tres = {
	.name = "ntasks-per-tres",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERTRES,
	.set_func = arg_set_ntasks_per_tres,
	.get_func = arg_get_ntasks_per_tres,
	.reset_func = arg_reset_ntasks_per_tres,
	.reset_each_pass = true,
};

COMMON_INT_OPTION_SET(ntasks_per_gpu, "--ntasks-per-gpu");
COMMON_INT_OPTION_GET(ntasks_per_gpu);
COMMON_OPTION_RESET(ntasks_per_gpu, NO_VAL);
static slurm_cli_opt_t slurm_opt_ntasks_per_gpu = {
	.name = "ntasks-per-gpu",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERGPU,
	.set_func = arg_set_ntasks_per_gpu,
	.get_func = arg_get_ntasks_per_gpu,
	.reset_func = arg_reset_ntasks_per_gpu,
	.reset_each_pass = true,
};

static int arg_set_open_mode(slurm_opt_t *opt, const char *arg)
{
	if (arg && (arg[0] == 'a' || arg[0] == 'A'))
		opt->open_mode = OPEN_MODE_APPEND;
	else if (arg && (arg[0] == 't' || arg[0] == 'T'))
		opt->open_mode = OPEN_MODE_TRUNCATE;
	else {
		error("Invalid --open-mode specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_open_mode(slurm_opt_t *opt)
{
	if (opt->open_mode == OPEN_MODE_APPEND)
		return xstrdup("a");
	if (opt->open_mode == OPEN_MODE_TRUNCATE)
		return xstrdup("t");

	return NULL;
}
COMMON_OPTION_RESET(open_mode, 0);
static slurm_cli_opt_t slurm_opt_open_mode = {
	.name = "open-mode",
	.has_arg = required_argument,
	.val = LONG_OPT_OPEN_MODE,
	.set_func_sbatch = arg_set_open_mode,
	.set_func_scron = arg_set_open_mode,
	.set_func_srun = arg_set_open_mode,
	.get_func = arg_get_open_mode,
	.reset_func = arg_reset_open_mode,
};

static int arg_set_ofname(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt && !opt->scron_opt && !opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->ofname);
	if (!xstrcasecmp(arg, "none"))
		opt->ofname = xstrdup("/dev/null");
	else
		opt->ofname = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(ofname);
COMMON_STRING_OPTION_RESET(ofname);
static slurm_cli_opt_t slurm_opt_output = {
	.name = "output",
	.has_arg = required_argument,
	.val = 'o',
	.set_func_sbatch = arg_set_ofname,
	.set_func_scron = arg_set_ofname,
	.set_func_srun = arg_set_ofname,
	.get_func = arg_get_ofname,
	.reset_func = arg_reset_ofname,
};

COMMON_BOOL_OPTION(overcommit, "overcommit");
static slurm_cli_opt_t slurm_opt_overcommit = {
	.name = "overcommit",
	.has_arg = no_argument,
	.val = 'O',
	.set_func = arg_set_overcommit,
	.get_func = arg_get_overcommit,
	.reset_func = arg_reset_overcommit,
	.reset_each_pass = true,
};

static int arg_set_overlap(slurm_opt_t *opt, const char *arg)
{
	/* --overlap is only valid for srun */
	if (!opt->srun_opt)
		return SLURM_SUCCESS;

	/*
	 * overlap_force means that the step will overlap all resources
	 * (CPUs, memory, GRES).
	 * Make this the only behavior for --overlap.
	 */
	opt->srun_opt->overlap_force = true;
	opt->srun_opt->exclusive = false;

	return SLURM_SUCCESS;
}
static char *arg_get_overlap(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->srun_opt->exclusive ? "unset" : "set");
}
static void arg_reset_overlap(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->exclusive = true;
}

static slurm_cli_opt_t slurm_opt_overlap = {
	.name = "overlap",
	.has_arg = optional_argument,
	.val = LONG_OPT_OVERLAP,
	.set_func_srun = arg_set_overlap,
	.get_func = arg_get_overlap,
	.reset_func = arg_reset_overlap,
};

/*
 * This option is directly tied to --exclusive. Both use the same output
 * function, and the string arguments are designed to mirror one another.
 */
static int arg_set_oversubscribe(slurm_opt_t *opt, const char *arg)
{
	if (opt->srun_opt)
		opt->srun_opt->exclusive = false;

	opt->shared = JOB_SHARED_OK;

	return SLURM_SUCCESS;
}

static slurm_cli_opt_t slurm_opt_oversubscribe = {
	.name = "oversubscribe",
	.has_arg = no_argument,
	.val = 's',
	.set_func = arg_set_oversubscribe,
	.get_func = arg_get_exclusive,
	.reset_func = arg_reset_shared,
	.reset_each_pass = true,
};

static int arg_set_het_group(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->srun_opt->het_group);
	opt->srun_opt->het_group = xstrdup(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_het_group(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->srun_opt->het_group);
}
static void arg_reset_het_group(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		xfree(opt->srun_opt->het_group);
}

/* Continue support for pack-group */
static slurm_cli_opt_t slurm_opt_pack_group = {
	.name = "pack-group",
	.has_arg = required_argument,
	.val = LONG_OPT_HET_GROUP,
	.srun_early_pass = true,
	.set_func_srun = arg_set_het_group,
	.get_func = arg_get_het_group,
	.reset_func = arg_reset_het_group,
};

static slurm_cli_opt_t slurm_opt_het_group = {
	.name = "het-group",
	.has_arg = required_argument,
	.val = LONG_OPT_HET_GROUP,
	.srun_early_pass = true,
	.set_func_srun = arg_set_het_group,
	.get_func = arg_get_het_group,
	.reset_func = arg_reset_het_group,
};

static int arg_set_parsable(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->parsable = true;

	return SLURM_SUCCESS;
}
static char *arg_get_parsable(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->sbatch_opt->parsable ? "set" : "unset");
}
static void arg_reset_parsable(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->parsable = false;
}
static slurm_cli_opt_t slurm_opt_parsable = {
	.name = "parsable",
	.has_arg = no_argument,
	.val = LONG_OPT_PARSABLE,
	.set_func_sbatch = arg_set_parsable,
	.get_func = arg_get_parsable,
	.reset_func = arg_reset_parsable,
};

COMMON_STRING_OPTION(partition);
static slurm_cli_opt_t slurm_opt_partition = {
	.name = "partition",
	.has_arg = required_argument,
	.val = 'p',
	.set_func = arg_set_partition,
	.get_func = arg_get_partition,
	.reset_func = arg_reset_partition,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(prefer);
static slurm_cli_opt_t slurm_opt_prefer = {
	.name = "prefer",
	.has_arg = required_argument,
	.val = LONG_OPT_PREFER,
	.set_func_salloc = arg_set_prefer,
	.set_func_sbatch = arg_set_prefer,
	.set_func_srun = arg_set_prefer,
	.get_func = arg_get_prefer,
	.reset_func = arg_reset_prefer,
};

COMMON_SRUN_BOOL_OPTION(preserve_env);
static slurm_cli_opt_t slurm_opt_preserve_env = {
	.name = "preserve-env",
	.has_arg = no_argument,
	.val = 'E',
	.set_func_srun = arg_set_preserve_env,
	.get_func = arg_get_preserve_env,
	.reset_func = arg_reset_preserve_env,
};

static int arg_set_priority(slurm_opt_t *opt, const char *arg)
{
	if (!xstrcasecmp(arg, "TOP")) {
		opt->priority = NO_VAL - 1;
	} else {
		long long priority = strtoll(arg, NULL, 10);
		if (priority < 0) {
			error("Priority must be >= 0");
			return SLURM_ERROR;
		}
		if (priority >= NO_VAL) {
			error("Priority must be < %u", NO_VAL);
			return SLURM_ERROR;
		}
		opt->priority = priority;
	}

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(priority);
static slurm_cli_opt_t slurm_opt_priority = {
	.name = "priority",
	.has_arg = required_argument,
	.val = LONG_OPT_PRIORITY,
	.set_func = arg_set_priority,
	.get_func = arg_get_priority,
	.reset_func = arg_reset_priority,
};

static int arg_set_profile(slurm_opt_t *opt, const char *arg)
{
	opt->profile = acct_gather_profile_from_string(arg);

	if (opt->profile == ACCT_GATHER_PROFILE_NOT_SET) {
		error("invalid --profile=%s option", arg);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_profile(slurm_opt_t *opt)
{
	return xstrdup(acct_gather_profile_to_string(opt->profile));
}
COMMON_OPTION_RESET(profile, ACCT_GATHER_PROFILE_NOT_SET);
static slurm_cli_opt_t slurm_opt_profile = {
	.name = "profile",
	.has_arg = required_argument,
	.val = LONG_OPT_PROFILE,
	.set_func = arg_set_profile,
	.get_func = arg_get_profile,
	.reset_func = arg_reset_profile,
};

COMMON_SRUN_STRING_OPTION(prolog);
static slurm_cli_opt_t slurm_opt_prolog = {
	.name = "prolog",
	.has_arg = required_argument,
	.val = LONG_OPT_PROLOG,
	.set_func_srun = arg_set_prolog,
	.get_func = arg_get_prolog,
	.reset_func = arg_reset_prolog,
};

static int arg_set_propagate(slurm_opt_t *opt, const char *arg)
{
	const char *tmp = arg;
	if (!opt->sbatch_opt && !opt->srun_opt)
		return SLURM_ERROR;

	if (!tmp)
		tmp = "ALL";

	if (opt->sbatch_opt)
		opt->sbatch_opt->propagate = xstrdup(tmp);
	if (opt->srun_opt)
		opt->srun_opt->propagate = xstrdup(tmp);

	return SLURM_SUCCESS;
}
static char *arg_get_propagate(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt && !opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->sbatch_opt)
		return xstrdup(opt->sbatch_opt->propagate);
	if (opt->srun_opt)
		return xstrdup(opt->srun_opt->propagate);

	return NULL;
}
static void arg_reset_propagate(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		xfree(opt->sbatch_opt->propagate);
	if (opt->srun_opt)
		xfree(opt->srun_opt->propagate);
}
static slurm_cli_opt_t slurm_opt_propagate = {
	.name = "propagate",
	.has_arg = optional_argument,
	.val = LONG_OPT_PROPAGATE,
	.set_func_sbatch = arg_set_propagate,
	.set_func_srun = arg_set_propagate,
	.get_func = arg_get_propagate,
	.reset_func = arg_reset_propagate,
};

static int arg_set_pty(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	xfree(opt->srun_opt->pty);
	opt->srun_opt->pty = xstrdup(arg ? arg : "");

	return SLURM_SUCCESS;
}
COMMON_SRUN_STRING_OPTION_GET(pty)
COMMON_SRUN_STRING_OPTION_RESET(pty)
static slurm_cli_opt_t slurm_opt_pty = {
	.name = "pty",
	.has_arg = optional_argument,
	.val = LONG_OPT_PTY,
	.set_func_srun = arg_set_pty,
	.get_func = arg_get_pty,
	.reset_func = arg_reset_pty,
};

COMMON_STRING_OPTION(qos);
static slurm_cli_opt_t slurm_opt_qos = {
	.name = "qos",
	.has_arg = required_argument,
	.val = 'q',
	.set_func = arg_set_qos,
	.get_func = arg_get_qos,
	.reset_func = arg_reset_qos,
};

static int arg_set_quiet(slurm_opt_t *opt, const char *arg)
{
	opt->quiet++;

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(quiet);
static slurm_cli_opt_t slurm_opt_quiet = {
	.name = "quiet",
	.has_arg = no_argument,
	.val = 'Q',
	.sbatch_early_pass = true,
	.set_func = arg_set_quiet,
	.get_func = arg_get_quiet,
	.reset_func = arg_reset_quiet,
};

COMMON_SRUN_BOOL_OPTION(quit_on_intr);
static slurm_cli_opt_t slurm_opt_quit_on_interrupt = {
	.name = "quit-on-interrupt",
	.has_arg = no_argument,
	.val = LONG_OPT_QUIT_ON_INTR,
	.set_func_srun = arg_set_quit_on_intr,
	.get_func = arg_get_quit_on_intr,
	.reset_func = arg_reset_quit_on_intr,
};

COMMON_BOOL_OPTION(reboot, "reboot");
static slurm_cli_opt_t slurm_opt_reboot = {
	.name = "reboot",
	.has_arg = no_argument,
	.val = LONG_OPT_REBOOT,
	.set_func = arg_set_reboot,
	.get_func = arg_get_reboot,
	.reset_func = arg_reset_reboot,
};

static int arg_set_relative(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->relative = parse_int("--relative", arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_relative(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("%d", opt->srun_opt->relative);
}
static void arg_reset_relative(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->relative = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_relative = {
	.name = "relative",
	.has_arg = required_argument,
	.val = 'r',
	.set_func_srun = arg_set_relative,
	.get_func = arg_get_relative,
	.reset_func = arg_reset_relative,
	.reset_each_pass = true,
};

static int arg_set_requeue(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->requeue = 1;

	return SLURM_SUCCESS;
}
/* arg_get_requeue and arg_reset_requeue defined before with --no-requeue */
static slurm_cli_opt_t slurm_opt_requeue = {
	.name = "requeue",
	.has_arg = no_argument,
	.val = LONG_OPT_REQUEUE,
	.set_func_sbatch = arg_set_requeue,
	.get_func = arg_get_requeue,
	.reset_func = arg_reset_requeue,
};

COMMON_STRING_OPTION(reservation);
static slurm_cli_opt_t slurm_opt_reservation = {
	.name = "reservation",
	.has_arg = required_argument,
	.val = LONG_OPT_RESERVATION,
	.set_func = arg_set_reservation,
	.get_func = arg_get_reservation,
	.reset_func = arg_reset_reservation,
};

static int arg_set_resv_port_cnt(slurm_opt_t *opt, const char *arg)
{
	if (!arg)
		opt->resv_port_cnt = 0;
	else
		opt->resv_port_cnt = parse_int("--resv-port", arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_resv_port_cnt(slurm_opt_t *opt)
{
	if (opt->resv_port_cnt == NO_VAL)
		return xstrdup("unset");

	return xstrdup_printf("%d", opt->resv_port_cnt);
}
static void arg_reset_resv_port_cnt(slurm_opt_t *opt)
{
	opt->resv_port_cnt = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_resv_ports = {
	.name = "resv-ports",
	.has_arg = optional_argument,
	.val = LONG_OPT_RESV_PORTS,
	.set_func = arg_set_resv_port_cnt,
	.get_func = arg_get_resv_port_cnt,
	.reset_func = arg_reset_resv_port_cnt,
	.reset_each_pass = true,
};

static int arg_set_segment_size(slurm_opt_t *opt, const char *arg)
{
	if (parse_uint16((char *) arg, &opt->segment_size)) {
		error("Invalid --segment specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_segment_size(slurm_opt_t *opt)
{
	if (opt->segment_size != 0)
		return xstrdup_printf("%u", opt->segment_size);
	return xstrdup("unset");
}
static void arg_reset_segment_size(slurm_opt_t *opt)
{
	opt->segment_size = 0;
}
static slurm_cli_opt_t slurm_opt_segment_size = {
	.name = "segment",
	.has_arg = required_argument,
	.val = LONG_OPT_SEGMENT_SIZE,
	.set_func = arg_set_segment_size,
	.get_func = arg_get_segment_size,
	.reset_func = arg_reset_segment_size,
	.reset_each_pass = true,
};

static int arg_set_send_libs(slurm_opt_t *opt, const char *arg)
{
	int rc;

	if (!opt->srun_opt)
		return SLURM_ERROR;

	if ((rc = parse_send_libs(arg)) == -1) {
		error("Invalid --send-libs specification");
		exit(-1);
	}

	opt->srun_opt->send_libs = rc ? true : false;

	return SLURM_SUCCESS;
}
static char *arg_get_send_libs(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->srun_opt->send_libs)
		return xstrdup("set");

	return NULL;
}
static void arg_reset_send_libs(slurm_opt_t *opt)
{
	char *tmp = NULL;

	if (opt->srun_opt) {
		tmp = xstrcasestr(slurm_conf.bcast_parameters, "send_libs");
		opt->srun_opt->send_libs = tmp ? true : false;
	}
}
static slurm_cli_opt_t slurm_opt_send_libs = {
	.name = "send-libs",
	.has_arg = optional_argument,
	.val = LONG_OPT_SEND_LIBS,
	.set_func_srun = arg_set_send_libs,
	.get_func = arg_get_send_libs,
	.reset_func = arg_reset_send_libs,
	.reset_each_pass = true,
};

static int arg_set_signal(slurm_opt_t *opt, const char *arg)
{
	if (get_signal_opts((char *) arg, &opt->warn_signal,
			    &opt->warn_time, &opt->warn_flags)) {
		error("Invalid --signal specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_signal(slurm_opt_t *opt)
{
	return signal_opts_to_cmdline(opt->warn_signal, opt->warn_time,
				      opt->warn_flags);
}
static void arg_reset_signal(slurm_opt_t *opt)
{
	opt->warn_flags = 0;
	opt->warn_signal = 0;
	opt->warn_time = 0;
}
static slurm_cli_opt_t slurm_opt_signal = {
	.name = "signal",
	.has_arg = required_argument,
	.val = LONG_OPT_SIGNAL,
	.set_func = arg_set_signal,
	.get_func = arg_get_signal,
	.reset_func = arg_reset_signal,
};

static int arg_set_slurmd_debug(slurm_opt_t *opt, const char *arg)
{
	uid_t uid = getuid();
	if (!opt->srun_opt)
		return SLURM_ERROR;

	if ((uid != 0) && (uid != slurm_conf.slurm_user_id) &&
	    (LOG_LEVEL_ERROR != log_string2num(arg))) {
		error("Use of --slurmd-debug is allowed only for root and SlurmUser(%s)",
		      slurm_conf.slurm_user_name);
		return SLURM_ERROR;
	}

	opt->srun_opt->slurmd_debug = log_string2num(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_slurmd_debug(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup(log_num2string(opt->srun_opt->slurmd_debug));
}
static void arg_reset_slurmd_debug(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->slurmd_debug = LOG_LEVEL_ERROR;
}
static slurm_cli_opt_t slurm_opt_slurmd_debug = {
	.name = "slurmd-debug",
	.has_arg = required_argument,
	.val = LONG_OPT_SLURMD_DEBUG,
	.set_func_srun = arg_set_slurmd_debug,
	.get_func = arg_get_slurmd_debug,
	.reset_func = arg_reset_slurmd_debug,
};

COMMON_INT_OPTION_SET(sockets_per_node, "--sockets-per-node");
COMMON_INT_OPTION_GET(sockets_per_node);
COMMON_OPTION_RESET(sockets_per_node, NO_VAL);
static slurm_cli_opt_t slurm_opt_sockets_per_node = {
	.name = "sockets-per-node",
	.has_arg = required_argument,
	.val = LONG_OPT_SOCKETSPERNODE,
	.set_func = arg_set_sockets_per_node,
	.get_func = arg_get_sockets_per_node,
	.reset_func = arg_reset_sockets_per_node,
	.reset_each_pass = true,
};

static int arg_set_spread_job(slurm_opt_t *opt, const char *arg)
{
	opt->job_flags |= SPREAD_JOB;

	return SLURM_SUCCESS;
}
static char *arg_get_spread_job(slurm_opt_t *opt)
{
	if (opt->job_flags & SPREAD_JOB)
		return xstrdup("set");
	return xstrdup("unset");
}
static void arg_reset_spread_job(slurm_opt_t *opt)
{
	opt->job_flags &= ~SPREAD_JOB;
}
static slurm_cli_opt_t slurm_opt_spread_job = {
	.name = "spread-job",
	.has_arg = no_argument,
	.val = LONG_OPT_SPREAD_JOB,
	.set_func = arg_set_spread_job,
	.get_func = arg_get_spread_job,
	.reset_func = arg_reset_spread_job,
	.reset_each_pass = true,
};

static int arg_set_stepmgr(slurm_opt_t *opt, const char *arg)
{
	opt->job_flags |= STEPMGR_ENABLED;

	return SLURM_SUCCESS;
}
static char *arg_get_stepmgr(slurm_opt_t *opt)
{
	if (opt->job_flags & STEPMGR_ENABLED)
		return xstrdup("set");
	return xstrdup("unset");
}
static void arg_reset_stepmgr(slurm_opt_t *opt)
{
	opt->job_flags &= ~STEPMGR_ENABLED;
}
static slurm_cli_opt_t slurm_opt_stepmgr = {
	.name = "stepmgr",
	.has_arg = no_argument,
	.val = LONG_OPT_STEPMGR,
	.set_func = arg_set_stepmgr,
	.get_func = arg_get_stepmgr,
	.reset_func = arg_reset_stepmgr,
};

static int arg_set_switch_req(slurm_opt_t *opt, const char *arg)
{
	opt->req_switch = parse_int("--switches", arg, true);

	return SLURM_SUCCESS;
}
static char *arg_get_switch_req(slurm_opt_t *opt)
{
	if (opt->req_switch != -1)
		return xstrdup_printf("%d", opt->req_switch);
	return xstrdup("unset");
}
static void arg_reset_switch_req(slurm_opt_t *opt)
{
	opt->req_switch = -1;
}
static slurm_cli_opt_t slurm_opt_switch_req = {
	.name = NULL, /* envvar only */
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCH_REQ,
	.set_func = arg_set_switch_req,
	.get_func = arg_get_switch_req,
	.reset_func = arg_reset_switch_req,
	.reset_each_pass = true,
};

static int arg_set_switch_wait(slurm_opt_t *opt, const char *arg)
{
	opt->wait4switch = time_str2secs(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_switch_wait(slurm_opt_t *opt)
{
	char time_str[32];
	if (opt->wait4switch == NO_VAL)
		return NULL;
	secs2time_str(opt->wait4switch, time_str, sizeof(time_str));
	return xstrdup_printf("%s", time_str);
}
static void arg_reset_switch_wait(slurm_opt_t *opt)
{
	opt->req_switch = -1;
	opt->wait4switch = -1;
}
static slurm_cli_opt_t slurm_opt_switch_wait = {
	.name = NULL, /* envvar only */
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCH_WAIT,
	.set_func = arg_set_switch_wait,
	.get_func = arg_get_switch_wait,
	.reset_func = arg_reset_switch_wait,
	.reset_each_pass = true,
};

static int arg_set_switches(slurm_opt_t *opt, const char *arg)
{
	char *tmparg = xstrdup(arg);
	char *split = xstrchr(tmparg, '@');

	if (split) {
		split[0] = '\0';
		split++;
		opt->wait4switch = time_str2secs(split);
	}

	opt->req_switch = parse_int("--switches", tmparg, true);

	xfree(tmparg);

	return SLURM_SUCCESS;
}

static char *arg_get_switches(slurm_opt_t *opt)
{
	if (opt->wait4switch != -1) {
		char time_str[32];
		secs2time_str(opt->wait4switch, time_str, sizeof(time_str));
		return xstrdup_printf("%d@%s", opt->req_switch, time_str);
	}
	if (opt->req_switch != -1)
		return xstrdup_printf("%d", opt->req_switch);
	return xstrdup("unset");
}
static void arg_reset_switches(slurm_opt_t *opt)
{
	opt->req_switch = -1;
	opt->wait4switch = -1;
}
static slurm_cli_opt_t slurm_opt_switches = {
	.name = "switches",
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCHES,
	.set_func = arg_set_switches,
	.get_func = arg_get_switches,
	.reset_func = arg_reset_switches,
	.reset_each_pass = true,
};

COMMON_SRUN_STRING_OPTION(task_epilog);
static slurm_cli_opt_t slurm_opt_task_epilog = {
	.name = "task-epilog",
	.has_arg = required_argument,
	.val = LONG_OPT_TASK_EPILOG,
	.set_func_srun = arg_set_task_epilog,
	.get_func = arg_get_task_epilog,
	.reset_func = arg_reset_task_epilog,
};

COMMON_SRUN_STRING_OPTION(task_prolog);
static slurm_cli_opt_t slurm_opt_task_prolog = {
	.name = "task-prolog",
	.has_arg = required_argument,
	.val = LONG_OPT_TASK_PROLOG,
	.set_func_srun = arg_set_task_prolog,
	.get_func = arg_get_task_prolog,
	.reset_func = arg_reset_task_prolog,
};

/* Deprecated form of --ntasks-per-node */
static slurm_cli_opt_t slurm_opt_tasks_per_node = {
	.name = "tasks-per-node",
	.has_arg = required_argument,
	.val = LONG_OPT_NTASKSPERNODE,
	.set_func = arg_set_ntasks_per_node,
	.get_func = arg_get_ntasks_per_node,
	.reset_func = arg_reset_ntasks_per_node,
	.reset_each_pass = true,
};

static int arg_set_test_only(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt && !opt->srun_opt)
		return SLURM_ERROR;

	if (opt->sbatch_opt)
		opt->sbatch_opt->test_only = true;
	if (opt->srun_opt)
		opt->srun_opt->test_only = true;

	return SLURM_SUCCESS;
}
static char *arg_get_test_only(slurm_opt_t *opt)
{
	bool tmp = false;

	if (!opt->sbatch_opt && !opt->srun_opt)
		return xstrdup("invalid-context");

	if (opt->sbatch_opt)
		tmp = opt->sbatch_opt->test_only;
	if (opt->srun_opt)
		tmp = opt->srun_opt->test_only;

	return xstrdup(tmp ? "set" : "unset");
}
static void arg_reset_test_only(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->test_only = false;
	if (opt->srun_opt)
		opt->srun_opt->test_only = false;
}
static slurm_cli_opt_t slurm_opt_test_only = {
	.name = "test-only",
	.has_arg = no_argument,
	.val = LONG_OPT_TEST_ONLY,
	.set_func_sbatch = arg_set_test_only,
	.set_func_srun = arg_set_test_only,
	.get_func = arg_get_test_only,
	.reset_func = arg_reset_test_only,
};

/* note this is mutually exclusive with --core-spec above */
static int arg_set_thread_spec(slurm_opt_t *opt, const char *arg)
{
	opt->core_spec = parse_int("--thread-spec", arg, true);
	opt->core_spec |= CORE_SPEC_THREAD;

	return SLURM_SUCCESS;
}
static char *arg_get_thread_spec(slurm_opt_t *opt)
{
	if ((opt->core_spec == NO_VAL16) ||
	    !(opt->core_spec & CORE_SPEC_THREAD))
		return xstrdup("unset");
	return xstrdup_printf("%d", (opt->core_spec & ~CORE_SPEC_THREAD));
}
static slurm_cli_opt_t slurm_opt_thread_spec = {
	.name = "thread-spec",
	.has_arg = required_argument,
	.val = LONG_OPT_THREAD_SPEC,
	.set_func = arg_set_thread_spec,
	.get_func = arg_get_thread_spec,
	.reset_func = arg_reset_core_spec,
	.reset_each_pass = true,
};

static int arg_set_threads_per_core(slurm_opt_t *opt, const char *arg)
{
	opt->threads_per_core = parse_int("--threads-per-core", arg, true);

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET(threads_per_core);
COMMON_OPTION_RESET(threads_per_core, NO_VAL);
static slurm_cli_opt_t slurm_opt_threads_per_core = {
	.name = "threads-per-core",
	.has_arg = required_argument,
	.val = LONG_OPT_THREADSPERCORE,
	.set_func = arg_set_threads_per_core,
	.get_func = arg_get_threads_per_core,
	.reset_func = arg_reset_threads_per_core,
	.reset_each_pass = true,
};

static int arg_set_time_limit(slurm_opt_t *opt, const char *arg)
{
	int time_limit;

	time_limit = time_str2mins(arg);
	if (time_limit == NO_VAL) {
		error("Invalid --time specification");
		return SLURM_ERROR;
	} else if (time_limit == 0) {
		time_limit = INFINITE;
	}

	opt->time_limit = time_limit;
	return SLURM_SUCCESS;
}
COMMON_TIME_DURATION_OPTION_GET_AND_RESET(time_limit);
static slurm_cli_opt_t slurm_opt_time_limit = {
	.name = "time",
	.has_arg = required_argument,
	.val = 't',
	.set_func = arg_set_time_limit,
	.get_func = arg_get_time_limit,
	.reset_func = arg_reset_time_limit,
};

static int arg_set_time_min(slurm_opt_t *opt, const char *arg)
{
	int time_min;

	time_min = time_str2mins(arg);
	if (time_min == NO_VAL) {
		error("Invalid --time-min specification");
		return SLURM_ERROR;
	} else if (time_min == 0) {
		time_min = INFINITE;
	}

	opt->time_min = time_min;
	return SLURM_SUCCESS;
}
COMMON_TIME_DURATION_OPTION_GET_AND_RESET(time_min);
static slurm_cli_opt_t slurm_opt_time_min = {
	.name = "time-min",
	.has_arg = required_argument,
	.val = LONG_OPT_TIME_MIN,
	.set_func = arg_set_time_min,
	.get_func = arg_get_time_min,
	.reset_func = arg_reset_time_min,
};

COMMON_MBYTES_OPTION(pn_min_tmp_disk, --tmp);
static slurm_cli_opt_t slurm_opt_tmp = {
	.name = "tmp",
	.has_arg = required_argument,
	.val = LONG_OPT_TMP,
	.set_func = arg_set_pn_min_tmp_disk,
	.get_func = arg_get_pn_min_tmp_disk,
	.reset_func = arg_reset_pn_min_tmp_disk,
	.reset_each_pass = true,
};

static int arg_set_uid(slurm_opt_t *opt, const char *arg)
{
	if (getuid() != 0) {
		error("--uid only permitted by root user");
		return SLURM_ERROR;
	}

	if (uid_from_string(arg, &opt->uid) < 0) {
		error("Invalid --uid specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET(uid);
COMMON_OPTION_RESET(uid, SLURM_AUTH_NOBODY);
static slurm_cli_opt_t slurm_opt_uid = {
	.name = "uid",
	.has_arg = required_argument,
	.val = LONG_OPT_UID,
	.set_func_sbatch = arg_set_uid,
	.get_func = arg_get_uid,
	.reset_func = arg_reset_uid,
};

/*
 * This is not exposed as an argument in sbatch, but is used
 * in xlate.c to translate a PBS option.
 */
static int arg_set_umask(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->umask = strtol(arg, NULL, 0);

	if ((opt->sbatch_opt->umask < 0) || (opt->sbatch_opt->umask > 0777)) {
		error("Invalid -W umask= specification");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static char *arg_get_umask(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("0%o", opt->sbatch_opt->umask);
}
static void arg_reset_umask(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->umask = -1;
}
static slurm_cli_opt_t slurm_opt_umask = {
	.name = NULL, /* only for use through xlate.c */
	.has_arg = no_argument,
	.val = LONG_OPT_UMASK,
	.set_func_sbatch = arg_set_umask,
	.get_func = arg_get_umask,
	.reset_func = arg_reset_umask,
	.reset_each_pass = true,
};

COMMON_SRUN_BOOL_OPTION(unbuffered);
static slurm_cli_opt_t slurm_opt_unbuffered = {
	.name = "unbuffered",
	.has_arg = no_argument,
	.val = 'u',
	.set_func_srun = arg_set_unbuffered,
	.get_func = arg_get_unbuffered,
	.reset_func = arg_reset_unbuffered,
};

static int arg_set_use_min_nodes(slurm_opt_t *opt, const char *arg)
{
	opt->job_flags |= USE_MIN_NODES;

	return SLURM_SUCCESS;
}
static char *arg_get_use_min_nodes(slurm_opt_t *opt)
{
	if (opt->job_flags & USE_MIN_NODES)
		return xstrdup("set");
	return xstrdup("unset");
}
static void arg_reset_use_min_nodes(slurm_opt_t *opt)
{
	opt->job_flags &= ~(USE_MIN_NODES);
}
static slurm_cli_opt_t slurm_opt_use_min_nodes = {
	.name = "use-min-nodes",
	.has_arg = no_argument,
	.val = LONG_OPT_USE_MIN_NODES,
	.set_func = arg_set_use_min_nodes,
	.get_func = arg_get_use_min_nodes,
	.reset_func = arg_reset_use_min_nodes,
	.reset_each_pass = true,
};

static int arg_set_usage(slurm_opt_t *opt, const char *arg)
{
	if(opt->scron_opt)
		return SLURM_ERROR;

	if (opt->usage_func)
		(opt->usage_func)();
	else
		error("Could not find --usage message");

	exit(0);
	return SLURM_SUCCESS;
}
static char *arg_get_usage(slurm_opt_t *opt)
{
	return NULL; /* no op */
}
static void arg_reset_usage(slurm_opt_t *opt)
{
	/* no op */
}
static slurm_cli_opt_t slurm_opt_usage = {
	.name = "usage",
	.has_arg = no_argument,
	.val = LONG_OPT_USAGE,
	.sbatch_early_pass = true,
	.set_func = arg_set_usage,
	.get_func = arg_get_usage,
	.reset_func = arg_reset_usage,
};

static int arg_set_verbose(slurm_opt_t *opt, const char *arg)
{
	static bool set_by_env = false;
	static bool set_by_cli = false;
	/*
	 * Note that verbose is handled a bit differently. As a cli argument,
	 * it has no_argument set so repeated 'v' characters can be used.
	 * As an environment variable though, it will have a numeric value.
	 * The boolean treatment from slurm_process_option() will still pass
	 * the string form along to us, which we can parse here into the
	 * correct value.
	 */
	if (!arg) {
		if (set_by_env) {
			opt->verbose = 0;
			set_by_env = false;
		}
		set_by_cli = true;
		opt->verbose++;
	} else {
		if (!set_by_cli) {
			set_by_env = true;
			opt->verbose = parse_int("--verbose", arg, false);
		}
	}

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(verbose);
static slurm_cli_opt_t slurm_opt_verbose = {
	.name = "verbose",
	.has_arg = no_argument,	/* sort of */
	.val = 'v',
	.sbatch_early_pass = true,
	.set_func = arg_set_verbose,
	.get_func = arg_get_verbose,
	.reset_func = arg_reset_verbose,
};

static int arg_set_version(slurm_opt_t *opt, const char *arg)
{
	if (opt->scron_opt)
		return SLURM_ERROR;

	print_slurm_version();
	exit(0);
}
static char *arg_get_version(slurm_opt_t *opt)
{
	return NULL; /* no op */
}
static void arg_reset_version(slurm_opt_t *opt)
{
	/* no op */
}
static slurm_cli_opt_t slurm_opt_version = {
	.name = "version",
	.has_arg = no_argument,
	.val = 'V',
	.sbatch_early_pass = true,
	.set_func = arg_set_version,
	.get_func = arg_get_version,
	.reset_func = arg_reset_version,
};

static int arg_set_wait(slurm_opt_t *opt, const char *arg)
{
	if (!opt->sbatch_opt)
		return SLURM_ERROR;

	opt->sbatch_opt->wait = true;

	return SLURM_SUCCESS;
}
static char *arg_get_wait(slurm_opt_t *opt)
{
	if (!opt->sbatch_opt)
		return xstrdup("invalid-context");

	return xstrdup(opt->sbatch_opt->wait ? "set" : "unset");
}
static void arg_reset_wait(slurm_opt_t *opt)
{
	if (opt->sbatch_opt)
		opt->sbatch_opt->wait = false;
}
static slurm_cli_opt_t slurm_opt_wait = {
	.name = "wait",
	.has_arg = no_argument,
	.val = 'W',
	.set_func_sbatch = arg_set_wait,
	.get_func = arg_get_wait,
	.reset_func = arg_reset_wait,
};

static int arg_set_wait_srun(slurm_opt_t *opt, const char *arg)
{
	if (!opt->srun_opt)
		return SLURM_ERROR;

	opt->srun_opt->max_wait = parse_int("--wait", arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_wait_srun(slurm_opt_t *opt)
{
	if (!opt->srun_opt)
		return xstrdup("invalid-context");

	return xstrdup_printf("%d", opt->srun_opt->max_wait);
}
static void arg_reset_wait_srun(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->max_wait = slurm_conf.wait_time;
}
static slurm_cli_opt_t slurm_opt_wait_srun = {
	.name = "wait",
	.has_arg = required_argument,
	.val = 'W',
	.set_func_srun = arg_set_wait_srun,
	.get_func = arg_get_wait_srun,
	.reset_func = arg_reset_wait_srun,
};

static int arg_set_wait_all_nodes(slurm_opt_t *opt, const char *arg)
{
	uint16_t tmp;

	if (!opt->salloc_opt && !opt->sbatch_opt)
		return SLURM_ERROR;

	tmp = parse_int("--wait-all-nodes", arg, false);

	if (tmp > 1) {
		error("Invalid --wait-all-nodes specification");
		return SLURM_ERROR;
	}

	if (opt->salloc_opt)
		opt->salloc_opt->wait_all_nodes = tmp;
	if (opt->sbatch_opt)
		opt->sbatch_opt->wait_all_nodes = tmp;

	return SLURM_SUCCESS;
}
static char *arg_get_wait_all_nodes(slurm_opt_t *opt)
{
	uint16_t tmp = NO_VAL16;

	if (!opt->salloc_opt && !opt->sbatch_opt)
		return xstrdup("invalid-context");

	if (opt->salloc_opt)
		tmp = opt->salloc_opt->wait_all_nodes;
	if (opt->sbatch_opt)
		tmp = opt->sbatch_opt->wait_all_nodes;

	return xstrdup_printf("%u", tmp);
}
static void arg_reset_wait_all_nodes(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->wait_all_nodes = NO_VAL16;
	if (opt->sbatch_opt)
		opt->sbatch_opt->wait_all_nodes = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_wait_all_nodes = {
	.name = "wait-all-nodes",
	.has_arg = required_argument,
	.val = LONG_OPT_WAIT_ALL_NODES,
	.set_func_salloc = arg_set_wait_all_nodes,
	.set_func_sbatch = arg_set_wait_all_nodes,
	.get_func = arg_get_wait_all_nodes,
	.reset_func = arg_reset_wait_all_nodes,
};

COMMON_STRING_OPTION(wckey);
static slurm_cli_opt_t slurm_opt_wckey = {
	.name = "wckey",
	.has_arg = required_argument,
	.val = LONG_OPT_WCKEY,
	.set_func = arg_set_wckey,
	.get_func = arg_get_wckey,
	.reset_func = arg_reset_wckey,
};

COMMON_SRUN_BOOL_OPTION(whole);
static slurm_cli_opt_t slurm_opt_whole = {
	.name = "whole",
	.has_arg = no_argument,
	.val = LONG_OPT_WHOLE,
	.set_func_srun = arg_set_whole,
	.get_func = arg_get_whole,
	.reset_func = arg_reset_whole,
};

COMMON_SBATCH_STRING_OPTION(wrap);
static slurm_cli_opt_t slurm_opt_wrap = {
	.name = "wrap",
	.has_arg = required_argument,
	.val = LONG_OPT_WRAP,
	.sbatch_early_pass = true,
	.set_func_sbatch = arg_set_wrap,
	.get_func = arg_get_wrap,
	.reset_func = arg_reset_wrap,
};

static int arg_set_x11(slurm_opt_t *opt, const char *arg)
{
	if (arg)
		opt->x11 = x11_str2flags(arg);
	else
		opt->x11 = X11_FORWARD_ALL;

	return SLURM_SUCCESS;
}
static char *arg_get_x11(slurm_opt_t *opt)
{
	return xstrdup(x11_flags2str(opt->x11));
}
COMMON_OPTION_RESET(x11, 0);
static slurm_cli_opt_t slurm_opt_x11 = {
#ifdef WITH_SLURM_X11
	.name = "x11",
#else
	/*
	 * Keep the code paths active, but disables the option name itself
	 * so the SPANK plugin can claim it.
	 */
	.name = NULL,
#endif
	.has_arg = optional_argument,
	.val = LONG_OPT_X11,
	.set_func_salloc = arg_set_x11,
	.set_func_srun = arg_set_x11,
	.get_func = arg_get_x11,
	.reset_func = arg_reset_x11,
};

static const slurm_cli_opt_t *common_options[] = {
	&slurm_opt__unknown_,
	&slurm_opt_accel_bind,
	&slurm_opt_account,
	&slurm_opt_acctg_freq,
	&slurm_opt_alloc_nodelist,
	&slurm_opt_array,
	&slurm_opt_argv,
	&slurm_opt_autocomplete,
	&slurm_opt_batch,
	&slurm_opt_bcast,
	&slurm_opt_bcast_exclude,
	&slurm_opt_begin,
	&slurm_opt_bell,
	&slurm_opt_bb,
	&slurm_opt_bbf,
	&slurm_opt_c_constraint,
	&slurm_opt_chdir,
	&slurm_opt_cluster,
	&slurm_opt_clusters,
	&slurm_opt_comment,
	&slurm_opt_compress,
	&slurm_opt_container,
	&slurm_opt_container_id,
	&slurm_opt_context,
	&slurm_opt_contiguous,
	&slurm_opt_constraint,
	&slurm_opt_core_spec,
	&slurm_opt_cores_per_socket,
	&slurm_opt_cpu_bind,
	&slurm_opt_cpu_underscore_bind,
	&slurm_opt_cpu_freq,
	&slurm_opt_cpus_per_gpu,
	&slurm_opt_cpus_per_task,
	&slurm_opt_deadline,
	&slurm_opt_debugger_test,
	&slurm_opt_delay_boot,
	&slurm_opt_environment,
	&slurm_opt_dependency,
	&slurm_opt_disable_status,
	&slurm_opt_distribution,
	&slurm_opt_epilog,
	&slurm_opt_error,
	&slurm_opt_exact,
	&slurm_opt_exclude,
	&slurm_opt_exclusive,
	&slurm_opt_export,
	&slurm_opt_export_file,
	&slurm_opt_external_launcher,
	&slurm_opt_extra,
	&slurm_opt_extra_node_info,
	&slurm_opt_get_user_env,
	&slurm_opt_gid,
	&slurm_opt_gpu_bind,
	&slurm_opt_gpu_freq,
	&slurm_opt_gpus,
	&slurm_opt_gpus_per_node,
	&slurm_opt_gpus_per_socket,
	&slurm_opt_gpus_per_task,
	&slurm_opt_gres,
	&slurm_opt_gres_flags,
	&slurm_opt_help,
	&slurm_opt_het_group,
	&slurm_opt_hint,
	&slurm_opt_hold,
	&slurm_opt_ignore_pbs,
	&slurm_opt_immediate,
	&slurm_opt_input,
	&slurm_opt_interactive,
	&slurm_opt_jobid,
	&slurm_opt_job_name,
	&slurm_opt_kill_command,
	&slurm_opt_kill_on_bad_exit,
	&slurm_opt_kill_on_invalid_dep,
	&slurm_opt_label,
	&slurm_opt_licenses,
	&slurm_opt_mail_type,
	&slurm_opt_mail_user,
	&slurm_opt_max_threads,
	&slurm_opt_mcs_label,
	&slurm_opt_mem,
	&slurm_opt_mem_bind,
	&slurm_opt_mem_per_cpu,
	&slurm_opt_mem_per_gpu,
	&slurm_opt_mincpus,
	&slurm_opt_mpi,
	&slurm_opt_msg_timeout,
	&slurm_opt_multi_prog,
	&slurm_opt_network,
	&slurm_opt_nice,
	&slurm_opt_no_allocate,
	&slurm_opt_no_bell,
	&slurm_opt_no_kill,
	&slurm_opt_no_shell,
	&slurm_opt_no_requeue,
	&slurm_opt_nodefile,
	&slurm_opt_nodelist,
	&slurm_opt_nodes,
	&slurm_opt_ntasks,
	&slurm_opt_ntasks_per_core,
	&slurm_opt_ntasks_per_gpu,
	&slurm_opt_ntasks_per_node,
	&slurm_opt_ntasks_per_socket,
	&slurm_opt_ntasks_per_tres,
	&slurm_opt_oom_kill_step,
	&slurm_opt_open_mode,
	&slurm_opt_output,
	&slurm_opt_overcommit,
	&slurm_opt_overlap,
	&slurm_opt_oversubscribe,
	&slurm_opt_pack_group,
	&slurm_opt_parsable,
	&slurm_opt_partition,
	&slurm_opt_prefer,
	&slurm_opt_preserve_env,
	&slurm_opt_priority,
	&slurm_opt_profile,
	&slurm_opt_prolog,
	&slurm_opt_propagate,
	&slurm_opt_pty,
	&slurm_opt_qos,
	&slurm_opt_quiet,
	&slurm_opt_quit_on_interrupt,
	&slurm_opt_reboot,
	&slurm_opt_relative,
	&slurm_opt_requeue,
	&slurm_opt_reservation,
	&slurm_opt_resv_ports,
	&slurm_opt_segment_size,
	&slurm_opt_send_libs,
	&slurm_opt_signal,
	&slurm_opt_slurmd_debug,
	&slurm_opt_sockets_per_node,
	&slurm_opt_spread_job,
	&slurm_opt_stepmgr,
	&slurm_opt_switch_req,
	&slurm_opt_switch_wait,
	&slurm_opt_switches,
	&slurm_opt_task_epilog,
	&slurm_opt_task_prolog,
	&slurm_opt_tasks_per_node,
	&slurm_opt_test_only,
	&slurm_opt_thread_spec,
	&slurm_opt_threads_per_core,
	&slurm_opt_time_limit,
	&slurm_opt_time_min,
	&slurm_opt_tmp,
	&slurm_opt_tree_width,
	&slurm_opt_tres_bind,
 	&slurm_opt_tres_per_task,
	&slurm_opt_uid,
	&slurm_opt_unbuffered,
	&slurm_opt_use_min_nodes,
	&slurm_opt_verbose,
	&slurm_opt_version,
	&slurm_opt_umask,
	&slurm_opt_usage,
	&slurm_opt_wait,
	&slurm_opt_wait_all_nodes,
	&slurm_opt_wait_srun,
	&slurm_opt_wckey,
	&slurm_opt_whole,
	&slurm_opt_wrap,
	&slurm_opt_x11,
	NULL /* END */
};

struct option *slurm_option_table_create(slurm_opt_t *opt,
					 char **opt_string)
{
	struct option *optz = optz_create(), *spanked;

	*opt_string = xstrdup("+");

	/*
	 * Since the initial elements of slurm_cli_opt_t match
	 * the layout of struct option, we can use this cast to
	 * avoid needing to make a temporary structure.
	 */
	for (int i = 0; common_options[i]; i++) {
		bool set = true;
		/*
		 * Runtime sanity checking for development builds,
		 * as I cannot find a convenient way to instruct the
		 * compiler to handle this. So if you make a mistake,
		 * you'll hit an assertion failure in salloc/srun/sbatch.
		 *
		 * If set_func is set, the others must not be:
		 */
		xassert((common_options[i]->set_func
			 && !common_options[i]->set_func_salloc
			 && !common_options[i]->set_func_sbatch
			 && !common_options[i]->set_func_scron
			 && !common_options[i]->set_func_srun) ||
			!common_options[i]->set_func);
		/*
		 * These two must always be set:
		 */
		xassert(common_options[i]->get_func);
		xassert(common_options[i]->reset_func);

		/*
		 * A few options only exist as environment variables, and
		 * should not be added to the table. They should be marked
		 * with a NULL name field.
		 */
		if (!common_options[i]->name)
			continue;

		if (common_options[i]->set_func)
			optz_add(&optz, (struct option *) common_options[i]);
		else if (opt->salloc_opt && common_options[i]->set_func_salloc)
			optz_add(&optz, (struct option *) common_options[i]);
		else if (opt->sbatch_opt && common_options[i]->set_func_sbatch)
			optz_add(&optz, (struct option *) common_options[i]);
		else if (opt->scron_opt && common_options[i]->set_func_scron)
			optz_add(&optz, (struct option *) common_options[i]);
		else if (opt->srun_opt && common_options[i]->set_func_srun)
			optz_add(&optz, (struct option *) common_options[i]);
		else
			set = false;

		if (set && (common_options[i]->val < LONG_OPT_ENUM_START)) {
			xstrfmtcat(*opt_string, "%c", common_options[i]->val);
			if (common_options[i]->has_arg == required_argument)
				xstrcat(*opt_string, ":");
			if (common_options[i]->has_arg == optional_argument)
				xstrcat(*opt_string, "::");
		}
	}

	spanked = spank_option_table_create(optz);
	optz_destroy(optz);

	return spanked;
}

void slurm_option_table_destroy(struct option *optz)
{
	optz_destroy(optz);
}

extern void slurm_free_options_members(slurm_opt_t *opt)
{
	if (!opt)
		return;

	slurm_reset_all_options(opt, true);

	xfree(opt->chdir);
	xfree(opt->state);
	xfree(opt->submit_line);
}

static void _init_state(slurm_opt_t *opt)
{
	if (opt->state)
		return;

	opt->state = xcalloc(sizeof(common_options),
			     sizeof(slurm_opt_state_t));
}

int slurm_process_option(slurm_opt_t *opt, int optval, const char *arg,
			 bool set_by_env, bool early_pass)
{
	int i;
	const char *setarg = arg;
	bool set = true;

	if (!opt)
		fatal("%s: missing slurm_opt_t struct", __func__);

	for (i = 0; common_options[i]; i++) {
		if (common_options[i]->val != optval)
			continue;

		/* Check that this is a valid match. */
		if (!common_options[i]->set_func &&
		    !(opt->salloc_opt && common_options[i]->set_func_salloc) &&
		    !(opt->sbatch_opt && common_options[i]->set_func_sbatch) &&
		    !(opt->scron_opt && common_options[i]->set_func_scron) &&
		    !(opt->srun_opt && common_options[i]->set_func_srun))
			continue;

		/* Match found */
		break;
	}

	/*
	 * Not a Slurm internal option, so hopefully it's a SPANK option.
	 * Skip this for early pass handling - SPANK options should only be
	 * processed once during the main pass.
	 */
	if (!common_options[i] && !early_pass) {
		if (spank_process_option(optval, arg))
			return SLURM_ERROR;
		return SLURM_SUCCESS;
	} else if (!common_options[i]) {
		/* early pass, assume it is a SPANK option and skip */
		return SLURM_SUCCESS;
	}

	/*
	 * Special handling for the early pass in sbatch.
	 *
	 * Some options are handled in the early pass, but most are deferred
	 * to a later pass, in which case those options are not re-evaluated.
	 * Environment variables are always evaluated by this though - there
	 * is no distinction for them of early vs normal passes.
	 */
	if (!set_by_env && opt->sbatch_opt) {
		if (!early_pass && common_options[i]->sbatch_early_pass)
			return SLURM_SUCCESS;
		if (early_pass && !common_options[i]->sbatch_early_pass)
			return SLURM_SUCCESS;
	} else if (!set_by_env && opt->srun_opt) {
		if (!early_pass && common_options[i]->srun_early_pass)
			return SLURM_SUCCESS;
		if (early_pass && !common_options[i]->srun_early_pass)
			return SLURM_SUCCESS;
	}

	if (arg) {
		if (common_options[i]->has_arg == no_argument) {
			char *end;
			/*
			 * Treat these "flag" arguments specially.
			 * For normal getopt_long() handling, arg is null.
			 * But for envvars, arg may be set, and will be
			 * processed by these rules:
			 * arg == '\0', flag is set
			 * arg == "yes", flag is set
			 * arg is a non-zero number, flag is set
			 * arg is anything else, call reset instead
			 */
			if (arg[0] == '\0') {
				set = true;
			} else if (!xstrcasecmp(arg, "yes")) {
				set = true;
			} else if (strtol(arg, &end, 10) && (*end == '\0')) {
				set = true;
			} else {
				set = false;
			}
		} else if (common_options[i]->has_arg == required_argument) {
			/* no special processing required */
		} else if (common_options[i]->has_arg == optional_argument) {
			/*
			 * If an empty string, convert to null,
			 * as this will let the envvar processing
			 * match the normal getopt_long() behavior.
			 */
			if (arg[0] == '\0')
				setarg = NULL;
		}
	}

	_init_state(opt);

	if (!set) {
		(common_options[i]->reset_func)(opt);
		opt->state[i].set = false;
		opt->state[i].set_by_env = false;
		return SLURM_SUCCESS;
	}

	if (common_options[i]->set_func) {
		if (!(common_options[i]->set_func)(opt, setarg)) {
			opt->state[i].set = true;
			opt->state[i].set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->salloc_opt && common_options[i]->set_func_salloc) {
		if (!(common_options[i]->set_func_salloc)(opt, setarg)) {
			opt->state[i].set = true;
			opt->state[i].set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->sbatch_opt && common_options[i]->set_func_sbatch) {
		if (!(common_options[i]->set_func_sbatch)(opt, setarg)) {
			opt->state[i].set = true;
			opt->state[i].set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->scron_opt && common_options[i]->set_func_scron) {
		if (!(common_options[i]->set_func_scron)(opt, setarg)) {
			opt->state[i].set = true;
			opt->state[i].set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->srun_opt && common_options[i]->set_func_srun) {
		if (!(common_options[i]->set_func_srun)(opt, setarg)) {
			opt->state[i].set = true;
			opt->state[i].set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	}

	return SLURM_ERROR;
}

void slurm_process_option_or_exit(slurm_opt_t *opt, int optval, const char *arg,
				  bool set_by_env, bool early_pass)
{
	if (slurm_process_option(opt, optval, arg, set_by_env, early_pass))
		exit(-1);
}

void slurm_print_set_options(slurm_opt_t *opt)
{
	if (!opt)
		fatal("%s: missing slurm_opt_t struct", __func__);

	info("defined options");
	info("-------------------- --------------------");

	for (int i = 0; common_options[i]; i++) {
		char *val = NULL;

		if (!opt->state || !opt->state[i].set)
			continue;

		if (common_options[i]->get_func)
			val = (common_options[i]->get_func)(opt);
		info("%-20s: %s", common_options[i]->name, val);
		xfree(val);
	}
	info("-------------------- --------------------");
	info("end of defined options");
}

extern void slurm_reset_all_options(slurm_opt_t *opt, bool first_pass)
{
	for (int i = 0; common_options[i]; i++) {
		if (!first_pass && !common_options[i]->reset_each_pass)
			continue;
		if (common_options[i]->reset_func) {
			(common_options[i]->reset_func)(opt);
			if (opt->state)
				opt->state[i].set = false;
		}
	}
}

/*
 * Find the index into common_options for a given option name.
 */
static int _find_option_index_from_optval(int optval)
{
	int i;

	for (i = 0; common_options[i]; i++) {
		if (common_options[i]->val == optval)
			return i;
	}

	xassert(false);

	return 0; /* slurm_opt__unknown_ */
}

/*
 * Was the option set by a cli argument?
 */
static bool _option_index_set_by_cli(slurm_opt_t *opt, int index)
{
	if (!opt) {
		debug3("%s: opt=NULL", __func__);
		return false;
	}

	if (!opt->state)
		return false;

	/*
	 * set is true if the option is set at all. If both set and set_by_env
	 * are true, then the argument was set through the environment not the
	 * cli, and we must return false.
	 */
	return (opt->state[index].set && !opt->state[index].set_by_env);
}

/*
 * Was the option set by an env var?
 */
static bool _option_index_set_by_env(slurm_opt_t *opt, int index)
{
	if (!opt) {
		debug3("%s: opt=NULL", __func__);
		return false;
	}

	if (!opt->state)
		return false;

	return opt->state[index].set_by_env;
}

/*
 * Was the option set by a cli argument?
 */
extern bool slurm_option_set_by_cli(slurm_opt_t *opt, int optval)
{
	int i = _find_option_index_from_optval(optval);
	return _option_index_set_by_cli(opt, i);
}

/*
 * Was the option set by an env var?
 */
extern bool slurm_option_set_by_env(slurm_opt_t *opt, int optval)
{
	int i = _find_option_index_from_optval(optval);
	return _option_index_set_by_env(opt, i);
}

/*
 * Find the index into common_options for a given option name.
 */
static int _find_option_idx(const char *name)
{
	for (int i = 0; common_options[i]; i++)
		if (!xstrcmp(name, common_options[i]->name))
			return i;
	return -1;
}

/*
 * Get option value by common option name.
 */
extern char *slurm_option_get(slurm_opt_t *opt, const char *name)
{
	int i = _find_option_idx(name);
	if (i < 0)
		return NULL;
	return common_options[i]->get_func(opt);
}

/*
 * Is option set? Discover by common option name.
 */
extern bool slurm_option_isset(slurm_opt_t *opt, const char *name)
{
	int i = _find_option_idx(name);
	if (i < 0 || !opt->state)
		return false;
	return opt->state[i].set;
}

/*
 * Replace option value by common option name.
 */
extern int slurm_option_set(slurm_opt_t *opt, const char *name,
                             const char *value, bool early)
{
	int rc = SLURM_ERROR;
	int i = _find_option_idx(name);
	if (i < 0)
		return rc;

	/* Don't set early options if it is not early. */
	if (opt->sbatch_opt && common_options[i]->sbatch_early_pass && !early)
		return SLURM_SUCCESS;
	if (opt->srun_opt && common_options[i]->srun_early_pass && !early)
		return SLURM_SUCCESS;

	/* Run the appropriate set function. */
	if (common_options[i]->set_func)
		rc = common_options[i]->set_func(opt, value);
	else if (common_options[i]->set_func_salloc && opt->salloc_opt)
		rc = common_options[i]->set_func_salloc(opt, value);
	else if (common_options[i]->set_func_sbatch && opt->sbatch_opt)
		rc = common_options[i]->set_func_sbatch(opt, value);
	else if (common_options[i]->set_func_scron && opt->scron_opt)
		rc = common_options[i]->set_func_scron(opt, value);
	else if (common_options[i]->set_func_srun && opt->srun_opt)
		rc = common_options[i]->set_func_srun(opt, value);

	/* Ensure that the option shows up as "set". */
	if (rc == SLURM_SUCCESS) {
		_init_state(opt);
		opt->state[i].set = true;
	}

	return rc;
}

/*
 * Reset option by common option name.
 */
extern bool slurm_option_reset(slurm_opt_t *opt, const char *name)
{
	int i = _find_option_idx(name);
	if (i < 0)
		return false;
	common_options[i]->reset_func(opt);
	if (opt->state)
		opt->state[i].set = false;
	return true;
}

/*
 * Function for iterating through all the common option data structure
 * and returning (via parameter arguments) the name and value of each
 * set slurm option.
 *
 * IN opt	- option data structure being interpreted
 * OUT name	- xmalloc()'d string with the option name
 * OUT value	- xmalloc()'d string with the option value
 * IN/OUT state	- internal state, should be set to 0 for the first call
 * RETURNS      - true if name/value set; false if no more options
 */
extern bool slurm_option_get_next_set(slurm_opt_t *opt, char **name,
				      char **value, size_t *state)
{
	size_t limit = sizeof(common_options) / sizeof(slurm_cli_opt_t *);
	if (*state >= limit)
		return false;

	while (common_options[*state] && (*state < limit) &&
	       (!(opt->state && opt->state[*state].set) ||
		!common_options[*state]->name))
		(*state)++;

	if (*state < limit && common_options[*state]) {
		*name = xstrdup(common_options[*state]->name);
		*value = common_options[*state]->get_func(opt);
		(*state)++;
		return true;
	}
	return false;
}

/*
 * Validate that the three memory options (--mem, --mem-per-cpu, --mem-per-gpu)
 * and their associated environment variables are set mutually exclusively.
 *
 * This will fatal() if multiple CLI options are specified simultaneously.
 * If any of the CLI options are specified, the other options are reset to
 * clear anything that may have been set through the environment.
 * Otherwise, if multiple environment variables are set simultaneously,
 * this will fatal().
 */
static void _validate_memory_options(slurm_opt_t *opt)
{
	if ((slurm_option_set_by_cli(opt, LONG_OPT_MEM) +
	     slurm_option_set_by_cli(opt, LONG_OPT_MEM_PER_CPU) +
	     slurm_option_set_by_cli(opt, LONG_OPT_MEM_PER_GPU)) > 1) {
		fatal("--mem, --mem-per-cpu, and --mem-per-gpu are mutually exclusive.");
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_MEM)) {
		slurm_option_reset(opt, "mem-per-cpu");
		slurm_option_reset(opt, "mem-per-gpu");
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_MEM_PER_CPU)) {
		slurm_option_reset(opt, "mem");
		slurm_option_reset(opt, "mem-per-gpu");
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_MEM_PER_GPU)) {
		slurm_option_reset(opt, "mem");
		slurm_option_reset(opt, "mem-per-cpu");
	} else if ((slurm_option_set_by_env(opt, LONG_OPT_MEM) +
		    slurm_option_set_by_env(opt, LONG_OPT_MEM_PER_CPU) +
		    slurm_option_set_by_env(opt, LONG_OPT_MEM_PER_GPU)) > 1) {
		fatal("SLURM_MEM_PER_CPU, SLURM_MEM_PER_GPU, and SLURM_MEM_PER_NODE are mutually exclusive.");
	}

	if (!(slurm_conf.select_type_param & CR_MEMORY) && opt->verbose) {
		if (slurm_option_isset(opt, "mem-per-cpu"))
			info("Configured SelectTypeParameters doesn't treat memory as a consumable resource. In this case value of --mem-per-cpu is only used to eliminate nodes with lower configured RealMemory value.");
		else if (slurm_option_isset(opt, "mem-per-gpu"))
			info("Configured SelectTypeParameters doesn't treat memory as a consumable resource. In this case value of --mem-per-gpu is ignored.");
	}
}

static void _validate_threads_per_core_option(slurm_opt_t *opt)
{
	if (!slurm_option_isset(opt, "threads-per-core"))
		return;

	if (!slurm_option_isset(opt, "cpu-bind")) {
		if (opt->verbose)
			info("Setting --cpu-bind=threads as a default of --threads-per-core use");
		if (opt->srun_opt)
			slurm_verify_cpu_bind("threads",
					      &opt->srun_opt->cpu_bind,
					      &opt->srun_opt->cpu_bind_type);
	} else if (opt->srun_opt &&
		  (!xstrcasecmp(opt->srun_opt->cpu_bind, "verbose") ||
		   !xstrcasecmp(opt->srun_opt->cpu_bind, "v"))) {
		if (opt->verbose)
			info("Setting --cpu-bind=threads,verbose as a default of --threads-per-core use");
		if (opt->srun_opt)
			slurm_verify_cpu_bind("threads,verbose",
					      &opt->srun_opt->cpu_bind,
					      &opt->srun_opt->cpu_bind_type);
	} else if (opt->verbose > 1) {
		info("Not setting --cpu-bind=threads because of --threads-per-core since --cpu-bind already set by cli option or environment variable");
	}
}

extern int validate_hint_option(slurm_opt_t *opt)
{
	cpu_bind_type_t cpu_bind_type = 0;

	if (opt->srun_opt)
		cpu_bind_type = opt->srun_opt->cpu_bind_type;

	if (slurm_option_set_by_cli(opt, LONG_OPT_HINT) &&
	    ((slurm_option_set_by_cli(opt, LONG_OPT_NTASKSPERCORE) ||
	      slurm_option_set_by_cli(opt, LONG_OPT_THREADSPERCORE) ||
	      slurm_option_set_by_cli(opt, 'B') ||
	      (slurm_option_set_by_cli(opt, LONG_OPT_CPU_BIND) &&
	       (cpu_bind_type & ~CPU_BIND_VERBOSE))))) {
		if (opt->verbose)
			info("Following options are mutually exclusive with --hint: --ntasks-per-core, --threads-per-core, -B and --cpu-bind (other than --cpu-bind=verbose). Ignoring --hint.");
		slurm_option_reset(opt, "hint");
		return SLURM_ERROR;
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_HINT)) {
		slurm_option_reset(opt, "ntasks-per-core");
		slurm_option_reset(opt, "threads-per-core");
		slurm_option_reset(opt, "extra-node-info");
		if (cpu_bind_type & ~CPU_BIND_VERBOSE) {
			bool has_verbose;

			has_verbose = (cpu_bind_type & CPU_BIND_VERBOSE);
			/* Completely clear cpu_bind */
			slurm_option_reset(opt, "cpu-bind");
			if (has_verbose && opt->srun_opt) {
				/* Add verbose back in */
				opt->srun_opt->cpu_bind_type = CPU_BIND_VERBOSE;
				opt->srun_opt->cpu_bind = xstrdup("verbose");
			}
		}
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_NTASKSPERCORE) ||
		   slurm_option_set_by_cli(opt, LONG_OPT_THREADSPERCORE) ||
		   slurm_option_set_by_cli(opt, 'B') ||
		   (slurm_option_set_by_cli(opt, LONG_OPT_CPU_BIND) &&
		    (cpu_bind_type & ~CPU_BIND_VERBOSE))) {
		slurm_option_reset(opt, "hint");
		return SLURM_ERROR;
	} else if (slurm_option_set_by_env(opt, LONG_OPT_HINT) &&
		   (slurm_option_set_by_env(opt, LONG_OPT_NTASKSPERCORE) ||
		    slurm_option_set_by_env(opt, LONG_OPT_THREADSPERCORE) ||
		    slurm_option_set_by_env(opt, 'B') ||
		    (slurm_option_set_by_env(opt, LONG_OPT_CPU_BIND) &&
		     (cpu_bind_type & ~CPU_BIND_VERBOSE)))) {
		if (opt->verbose)
			info("Following options are mutually exclusive with --hint: --ntasks-per-core, --threads-per-core, -B and --cpu-bind, but more than one set by environment variables. Ignoring SLURM_HINT.");
		slurm_option_reset(opt, "hint");
		return SLURM_ERROR;
	}
	return SLURM_SUCCESS;
}

static void _validate_ntasks_per_gpu(slurm_opt_t *opt)
{
	bool tres = slurm_option_set_by_cli(opt, LONG_OPT_NTASKSPERTRES);
	bool gpu = slurm_option_set_by_cli(opt, LONG_OPT_NTASKSPERGPU);
	bool tres_env = slurm_option_set_by_env(opt, LONG_OPT_NTASKSPERTRES);
	bool gpu_env = slurm_option_set_by_env(opt, LONG_OPT_NTASKSPERGPU);
	bool any = (tres || gpu || tres_env || gpu_env);

	if (!any)
		return;

	/* Validate --ntasks-per-gpu and --ntasks-per-tres */
	if (gpu && tres) {
		if (opt->ntasks_per_gpu != opt->ntasks_per_tres)
			fatal("Inconsistent values set to --ntasks-per-gpu=%d and --ntasks-per-tres=%d ",
			      opt->ntasks_per_gpu,
			      opt->ntasks_per_tres);
	} else if (gpu && tres_env) {
		if (opt->verbose)
			info("Ignoring SLURM_NTASKS_PER_TRES since --ntasks-per-gpu given as command line option");
		slurm_option_reset(opt, "ntasks-per-tres");
	} else if (tres && gpu_env) {
		if (opt->verbose)
			info("Ignoring SLURM_NTASKS_PER_GPU since --ntasks-per-tres given as command line option");
		slurm_option_reset(opt, "ntasks-per-gpu");
	} else if (gpu_env && tres_env) {
		if (opt->ntasks_per_gpu != opt->ntasks_per_tres)
			fatal("Inconsistent values set by environment variables SLURM_NTASKS_PER_GPU=%d and SLURM_NTASKS_PER_TRES=%d ",
			      opt->ntasks_per_gpu,
			      opt->ntasks_per_tres);
	}

	if (slurm_option_set_by_cli(opt, LONG_OPT_TRES_PER_TASK))
		fatal("--tres-per-task is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_env(opt, LONG_OPT_TRES_PER_TASK))
		fatal("SLURM_TRES_PER_TASK is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_cli(opt, LONG_OPT_GPUS_PER_TASK))
		fatal("--gpus-per-task is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_env(opt, LONG_OPT_GPUS_PER_TASK))
		fatal("SLURM_GPUS_PER_TASK is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_cli(opt, LONG_OPT_GPUS_PER_SOCKET))
		fatal("--gpus-per-socket is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_env(opt, LONG_OPT_GPUS_PER_SOCKET))
		fatal("SLURM_GPUS_PER_SOCKET is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_cli(opt, LONG_OPT_NTASKSPERNODE))
		fatal("--ntasks-per-node is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");

	if (slurm_option_set_by_env(opt, LONG_OPT_NTASKSPERNODE))
		fatal("SLURM_NTASKS_PER_NODE is mutually exclusive with --ntasks-per-gpu and SLURM_NTASKS_PER_GPU");
}

static void _validate_spec_cores_options(slurm_opt_t *opt)
{
	if (!slurm_option_isset(opt, "thread-spec") &&
	    !slurm_option_isset(opt, "core-spec"))
		return;

	if ((slurm_option_set_by_cli(opt, 'S') +
	     slurm_option_set_by_cli(opt, LONG_OPT_THREAD_SPEC)) > 1)
		fatal("-S/--core-spec and --thred-spec options are mutually exclusive");
	else if (((slurm_option_set_by_env(opt, 'S') +
		   slurm_option_set_by_env(opt, LONG_OPT_THREAD_SPEC)) > 1) &&
		 (((slurm_option_set_by_cli(opt, 'S') +
		    slurm_option_set_by_cli(opt, LONG_OPT_THREAD_SPEC))) == 0))
		fatal("Both --core-spec and --thread-spec set using environment variables. Those options are mutually exclusive.");

	if (!(slurm_conf.conf_flags & CONF_FLAG_ASRU)) {
		error("Ignoring %s since it's not allowed by configuration (AllowSpecResourcesUsage = No)",
		      (opt->core_spec & CORE_SPEC_THREAD) ?
		      "--thread-spec":"-S");
	}
}

static void _validate_share_options(slurm_opt_t *opt)
{
	bool exclusive = slurm_option_set_by_cli(opt, LONG_OPT_EXCLUSIVE);
	bool oversubscribe = slurm_option_set_by_cli(opt, 's');

	if (exclusive && oversubscribe) {
		fatal("--exclusive and --oversubscribe options are mutually exclusive");
	}
}

extern bool slurm_option_get_tres_per_tres(
	char *in_val, char *tres_name, uint64_t *cnt, char **save_ptr, int *rc)
{
	char *name = NULL, *type = NULL, *tres_type = "gres";
	uint64_t value = 0;

	xassert(save_ptr);
	*rc = slurm_get_next_tres(&tres_type, in_val, &name, &type,
				  &value, save_ptr);
	xfree(type);

	if (*rc != SLURM_SUCCESS) {
		*save_ptr = NULL;
		xfree(name);
		return false;
	}

	if (!xstrcasecmp(name, tres_name))
		*cnt += value;
	xfree(name);

	if (!*save_ptr)
		return false;
	else
		return true;
}

/*
 * Update part of the tres_per_task string and match it to the count given
 *
 * If cnt == 0, then just remove the string from tres_per_task.
 *
 * tres_per_task takes a form similar to "cpu=10,gres/gpu:gtx=1,license/iop1=1".
 *
 * IN: cnt - new value
 * IN: tres_str - name of tres we want to to be update
 * OUT: tres_per_task_p - string to update
 */
extern void slurm_option_update_tres_per_task(int cnt, char *tres_str,
					      char **tres_per_task_p)
{
	int tres_cpu_cnt;
	char *prefix, *suffix = NULL, *new_str = NULL;
	char *tres_per_task;
	char *prev_tres_ptr;

	xassert(tres_per_task_p);
	tres_per_task = *tres_per_task_p;
	prev_tres_ptr = xstrcasestr(tres_per_task, tres_str);

	if (!prev_tres_ptr) {
		if (cnt) {
			/* Add tres to tres_per_task. */
			if (tres_per_task) {
				xstrfmtcat(new_str, "%s=%d,%s", tres_str, cnt,
					   tres_per_task);
			} else {
				xstrfmtcat(new_str, "%s=%d", tres_str, cnt);
			}
			xfree(tres_per_task);
			tres_per_task = new_str;
		}
		*tres_per_task_p = tres_per_task;
		return;
	}

	/* Get the count in tres_per_task */
	tres_cpu_cnt = atoi(prev_tres_ptr + strlen(tres_str) + 1);

	/* Nothing to update. */
	if (tres_cpu_cnt == cnt)
		return;

	/* Get suffix string */
	if ((suffix = xstrstr(prev_tres_ptr, ","))) {
		/* Remove the initial comma in the suffix. */
		suffix += 1;
	}

	/* Set the prefix */
	*prev_tres_ptr = '\0';
	prefix = tres_per_task;
	if (prefix) {
		/* Remove the final comma of the prefix. */
		char *pfx_comma = prefix + strlen(prefix) - 1;
		if (*pfx_comma == ',')
			*pfx_comma = '\0';
	}

	/* Skip empty prefix/suffix in the following logic*/
	if (prefix && !*prefix)
		prefix = NULL;
	if (suffix && !*suffix)
		suffix = NULL;

	if (!cnt) {
		/* Exclude the tres string */
		if (prefix && suffix)
			xstrfmtcat(new_str, "%s,%s", prefix, suffix);
		if (prefix && !suffix)
			xstrfmtcat(new_str, "%s", prefix);
		if (!prefix && suffix)
			xstrfmtcat(new_str, "%s", suffix);
	} else {
		/* Compose the new string. */
		if (prefix && suffix)
			xstrfmtcat(new_str, "%s,%s=%d,%s", prefix, tres_str,
				   cnt, suffix);
		if (prefix && !suffix)
			xstrfmtcat(new_str, "%s,%s=%d", prefix, tres_str, cnt);
		if (!prefix && suffix)
			xstrfmtcat(new_str, "%s=%d,%s", tres_str, cnt, suffix);
		if (!prefix && !suffix)
			xstrfmtcat(new_str, "%s=%d", tres_str, cnt);
	}

	xfree(tres_per_task);
	tres_per_task = new_str;
	*tres_per_task_p = tres_per_task;
}

static bool _get_gpu_cnt_and_str(slurm_opt_t *opt, int *gpu_cnt, char **gpu_str)
{
	char *num_str = NULL, sep_char;

	if (!opt->gpus_per_task)
		return false;

	xstrcat(*gpu_str, "gres/gpu");

	if ((num_str = xstrstr(opt->gpus_per_task, ":")))
		sep_char = ':';
	else if ((num_str = xstrstr(opt->gpus_per_task, "=")))
		sep_char = '=';

	if (num_str) { /* Has type string */
		*num_str = '\0';
		/* Add type string to gpu_str */
		xstrfmtcat(*gpu_str, ":%s", opt->gpus_per_task);
		*num_str = sep_char;
		num_str += 1;
	} else {
		num_str = opt->gpus_per_task;
	}

	if (gpu_cnt)
		(*gpu_cnt) = strtol(num_str, NULL, 10);

	return true;
}

static void _set_tres_per_task_from_sibling_opt(slurm_opt_t *opt, int optval)
{
	bool set;
	int tmp_int, cnt = 0, opt_index, tpt_index;
	char *opt_in_tpt_ptr = NULL, *str = NULL;
	char *env_variable;

	/*
	 * See if the sibling option was set with tres-per-task
	 * Either one specified on the command line overrides the other in the
	 * environment.
	 * They can both be in the environment because specifying just
	 * --tres-per-task=cpu=# for example, will cause SLURM_CPUS_PER_TASK to
	 * be set as well. So if they're both in the environment, verify that
	 * they're the same.
	 *
	 * If tres-per-task or a sibling option are set, then make sure that
	 * both are set to the same thing:
	 */

	if (optval == LONG_OPT_GPUS_PER_TASK) {
		set = _get_gpu_cnt_and_str(opt, &cnt, &str);
		env_variable = "SLURM_GPUS_PER_TASK";
	} else if (optval == 'c') {
		cnt = opt->cpus_per_task;
		str = "cpu";
		set = opt->cpus_set;
		env_variable = "SLURM_CPUS_PER_TASK";
	} else {
		/* This function only supports [gpus|cpus]_per_task */
		xassert(0); /* let me know if it isn't */
		return;
	}

	opt_in_tpt_ptr = xstrcasestr(opt->tres_per_task, str);
	if (!opt_in_tpt_ptr) {
		if (set)
			slurm_option_update_tres_per_task(cnt, str,
							  &opt->tres_per_task);
		return;
	}

	opt_index = _find_option_index_from_optval(optval);
	tpt_index = _find_option_index_from_optval(LONG_OPT_TRES_PER_TASK);

	if (_option_index_set_by_cli(opt, opt_index) &&
	    _option_index_set_by_cli(opt, tpt_index)) {
		fatal("You can not have --tres-per-task=%s= and --%s please use one or the other",
		      str, common_options[opt_index]->name);
	} else if (_option_index_set_by_cli(opt, opt_index) &&
		   _option_index_set_by_env(opt, tpt_index)) {
		/*
		 * The value is already in opt->cpus_per_task.
		 * Update the cpus part of the env variable.
		 */
		slurm_option_update_tres_per_task(cnt, str,
						  &opt->tres_per_task);
		if (opt->verbose)
			info("Updating SLURM_TRES_PER_TASK to %s as --%s takes precedence over the environment variables.",
			     opt->tres_per_task,
			     common_options[opt_index]->name);
		return;
	}

	tmp_int = atoi(opt_in_tpt_ptr + strlen(str) + 1);
	if (tmp_int <= 0) {
		fatal("Invalid --tres-per-task=%s=%d", str, tmp_int);
	}

	if (_option_index_set_by_env(opt, opt_index) &&
	    _option_index_set_by_env(opt, tpt_index) &&
	    (tmp_int != opt->cpus_per_task)) {
		fatal("%s set by two different environment variables %s=%d != SLURM_TRES_PER_TASK=cpu=%d",
		      common_options[opt_index]->name, env_variable, cnt,
		      tmp_int);
	}

	/*
	 * Now we know that either tres-per-task is set by cli and the option
	 * is set by env, or only tres-per-task is set either by cli or env.
	 * Either way, set the option from tres-per-task.
	 */
	if (optval == LONG_OPT_GPUS_PER_TASK) {
		opt->gpus_per_task = opt_in_tpt_ptr;
	} else if (optval == 'c') {
		opt->cpus_per_task = tmp_int;
		opt->cpus_set = true;
	}

	if (opt->verbose &&
	    _option_index_set_by_env(opt, opt_index) &&
	    _option_index_set_by_cli(opt, tpt_index))
		info("Ignoring %s since --tres-per-task=%s= was given as a command line option.",
		     env_variable, str);
}

/*
 * Implicitly set tres_bind based off of tres_per_task if tres_bind is not
 * explicitly set already.
 */
static void _implicitly_bind_tres_per_task(slurm_opt_t *opt)
{
	char *name, *type, *save_ptr = NULL;
	/* tres_bind only supports gres currently */
	char *tres_type = "gres";
	uint64_t cnt;

	while ((slurm_get_next_tres(&tres_type,
				    opt->tres_per_task,
				    &name, &type,
				    &cnt, &save_ptr) == SLURM_SUCCESS) &&
	       save_ptr) {
		 /* Skip any explicitly set binding */
		if (opt->tres_bind && xstrstr(opt->tres_bind, name))
			continue;
		xstrfmtcat(opt->tres_bind, "%s%s/%s:per_task:%"PRIu64,
			   opt->tres_bind ? "+" : "", tres_type, name, cnt);
	}
}

static void _validate_tres_per_task(slurm_opt_t *opt)
{
	if (!xstrncasecmp(opt->tres_per_task, "mem", 3) ||
	    xstrcasestr(opt->tres_per_task, ",mem")) {
		fatal("Invalid TRES for --tres-per-task: mem");
	} else if (!xstrncasecmp(opt->tres_per_task, "energy", 6) ||
		   xstrcasestr(opt->tres_per_task, ",energy")) {
		fatal("Invalid TRES for --tres-per-task: energy");
	} else if (!xstrncasecmp(opt->tres_per_task, "node", 4) ||
		   xstrcasestr(opt->tres_per_task, ",node")) {
		fatal("Invalid TRES for --tres-per-task: node");
	} else if (!xstrncasecmp(opt->tres_per_task, "billing", 7) ||
		   xstrcasestr(opt->tres_per_task, ",billing")) {
		fatal("Invalid TRES for --tres-per-task: billing");
	} else if (!xstrncasecmp(opt->tres_per_task, "fs", 2) ||
		   xstrcasestr(opt->tres_per_task, ",fs")) {
		fatal("Invalid TRES for --tres-per-task: fs");
	} else if (!xstrncasecmp(opt->tres_per_task, "vmem", 4) ||
		   xstrcasestr(opt->tres_per_task, ",vmem")) {
		fatal("Invalid TRES for --tres-per-task: vmem");
	} else if (!xstrncasecmp(opt->tres_per_task, "pages", 5) ||
		   xstrcasestr(opt->tres_per_task, ",pages")) {
		fatal("Invalid TRES for --tres-per-task: pages");
	} else if (!xstrncasecmp(opt->tres_per_task, "bb", 2) ||
		   xstrcasestr(opt->tres_per_task, ",bb")) {
		fatal("Invalid TRES for --tres-per-task: bb");
	}

	slurm_format_tres_string(&opt->tres_per_task, "license");
	slurm_format_tres_string(&opt->tres_per_task, "gres");

	_set_tres_per_task_from_sibling_opt(opt, LONG_OPT_GPUS_PER_TASK);
	_set_tres_per_task_from_sibling_opt(opt, 'c');
	_implicitly_bind_tres_per_task(opt);
}

static void _validate_cpus_per_tres(slurm_opt_t *opt)
{
	bool cpt_set_by_cli;
	bool cpt_set_by_env;

	if (xstrcasestr(opt->tres_per_task, "cpu")) {
		cpt_set_by_cli =
			(slurm_option_set_by_cli(opt, 'c') ||
			 slurm_option_set_by_cli(opt, LONG_OPT_TRES_PER_TASK));
		cpt_set_by_env =
			(slurm_option_set_by_env(opt, 'c') ||
			 slurm_option_set_by_env(opt, LONG_OPT_TRES_PER_TASK));
	} else {
		cpt_set_by_cli = slurm_option_set_by_cli(opt, 'c');
		cpt_set_by_env = slurm_option_set_by_env(opt, 'c');
	}

	/* --cpus-per-task and --cpus-per-gres are mutually exclusive */
	if ((cpt_set_by_cli &&
	     slurm_option_set_by_cli(opt, LONG_OPT_CPUS_PER_GPU)) ||
	    (cpt_set_by_env &&
	     slurm_option_set_by_env(opt, LONG_OPT_CPUS_PER_GPU))) {
		fatal("--cpus-per-task, --tres-per-task=cpu:#, and --cpus-per-gpu are mutually exclusive");
	}

	/*
	 * If either is specified on the command line, it should override
	 * anything set by the environment.
	 */
	if (cpt_set_by_cli &&
	    slurm_option_set_by_env(opt, LONG_OPT_CPUS_PER_GPU)) {
		if (opt->verbose) {
			char *env_str = NULL;

			if (opt->salloc_opt)
				env_str = "SALLOC_CPUS_PER_GPU";
			else if (opt->sbatch_opt)
				env_str = "SBATCH_CPUS_PER_GPU";
			else /* opt->srun_opt */
				env_str = "SLURM_CPUS_PER_GPU";
			info("Ignoring %s since --cpus-per-task or --tres-per-task=cpu:# given as command line option",
			     env_str);
		}
		slurm_option_reset(opt, "cpus-per-gpu");
	} else if (slurm_option_set_by_cli(opt, LONG_OPT_CPUS_PER_GPU) &&
		   cpt_set_by_env) {
		if (opt->verbose) {
			info("Ignoring cpus_per_task from the environment since --cpus-per-gpu was given as a command line option");
		}
		slurm_option_reset(opt, "cpus-per-task");
		/* Also clear cpu:# from tres-per-task */
		slurm_option_update_tres_per_task(opt->cpus_per_task, "cpu",
						  &opt->tres_per_task);
	}
}

/*
 * If the node list supplied is a file name, translate that into
 *	a list of nodes, we orphan the data pointed to
 * RET true if the node list is a valid one
 */
static bool _valid_node_list(slurm_opt_t *opt, char **node_list_pptr)
{
	int count = NO_VAL;

	/*
	 * If we are using Arbitrary and we specified the number of procs to use
	 * then we need exactly this many since we are saying, lay it out this
	 * way! Same for max and min nodes. Other than that just read in as many
	 * in the hostfile.
	 */
	if (opt->ntasks_set)
		count = opt->ntasks;
	else if (opt->nodes_set) {
		if (opt->max_nodes)
			count = opt->max_nodes;
		else if (opt->min_nodes)
			count = opt->min_nodes;
	}

	return verify_node_list(node_list_pptr, opt->distribution, count);
}

static void _validate_nodelist(slurm_opt_t *opt)
{
	int error_exit = 1;

	if (opt->nodefile) {
		char *tmp;
		xfree(opt->nodelist);
		if (!(tmp = slurm_read_hostfile(opt->nodefile, 0))) {
			error("Invalid --nodefile node file");
			exit(-1);
		}
		opt->nodelist = xstrdup(tmp);
		free(tmp);
	}

	if (!opt->nodelist) {
		if ((opt->nodelist = xstrdup(getenv("SLURM_HOSTFILE")))) {
			/*
			 * make sure the file being read in has a / in it to
			 * make sure it is a file in the _valid_node_list()
			 * function.
			 */
			if (!xstrstr(opt->nodelist, "/")) {
				char *add_slash = xstrdup("./");
				xstrcat(add_slash, opt->nodelist);
				xfree(opt->nodelist);
				opt->nodelist = add_slash;
			}
			opt->distribution &= SLURM_DIST_STATE_FLAGS;
			opt->distribution |= SLURM_DIST_ARBITRARY;
			if (!_valid_node_list(opt, &opt->nodelist)) {
				error("Failure getting NodeNames from hostfile");
				exit(error_exit);
			} else {
				debug("loaded nodes (%s) from hostfile",
				      opt->nodelist);
			}
		}
	} else {
		if (!_valid_node_list(opt, &opt->nodelist))
			exit(error_exit);
	}
}

static void _validate_arbitrary(slurm_opt_t *opt)
{
	int error_exit = 1;

	if ((opt->distribution & SLURM_DIST_STATE_BASE) != SLURM_DIST_ARBITRARY)
		return;
	if (!opt->nodes_set ||
	    slurm_option_set_by_env(opt, 'N'))
		return;

	error("--nodes is incompatible with --distribution=arbitrary");
	exit(error_exit);
}

static void _validate_gres_flags(slurm_opt_t *opt)
{
	if (!(opt->job_flags & GRES_DISABLE_BIND) &&
	    (slurm_conf.select_type_param & ENFORCE_BINDING_GRES))
		opt->job_flags |= GRES_ENFORCE_BIND;

	if (opt->job_flags & GRES_ONE_TASK_PER_SHARING) {
		char *tres_type = "gres";
		bool found = false;
		char *name, *type, *save_ptr = NULL;
		uint64_t cnt;

		/* Sanity check to assure --tres-per-task has the shared GRES */
		while ((slurm_get_next_tres(&tres_type,
					    opt->tres_per_task,
					    &name, &type,
					    &cnt, &save_ptr) ==
			SLURM_SUCCESS) && save_ptr) {
			/* Skip if gres isn't shared */
			if (gres_is_shared_name(name)) {
				found = true;
				break;
			}
		}

		if (!found) {
			fatal("--gres-flags=one-task-per-sharing requested, but that shared gres needs to appear in --tres-per-task as well.");
		}
	} else if (!(opt->job_flags & GRES_MULT_TASKS_PER_SHARING) &&
		   (slurm_conf.select_type_param & ONE_TASK_PER_SHARING_GRES))
		opt->job_flags |= GRES_ONE_TASK_PER_SHARING;
}

/* Validate shared options between srun, salloc, and sbatch */
extern void validate_options_salloc_sbatch_srun(slurm_opt_t *opt)
{
	_validate_ntasks_per_gpu(opt);
	_validate_spec_cores_options(opt);
	_validate_threads_per_core_option(opt);
	_validate_memory_options(opt);
	_validate_share_options(opt);
	_validate_tres_per_task(opt);
	_validate_cpus_per_tres(opt);
	_validate_nodelist(opt);
	_validate_arbitrary(opt);
	_validate_gres_flags(opt);
}

extern char *slurm_option_get_argv_str(const int argc, char **argv)
{
	char *submit_line;

	if (!argv || !argv[0])
		fatal("%s: no argv given", __func__);

	submit_line = xstrdup(argv[0]);

	for (int i = 1; i < argc; i++)
		xstrfmtcat(submit_line, " %s", argv[i]);

	return submit_line;
}

extern job_desc_msg_t *slurm_opt_create_job_desc(slurm_opt_t *opt_local,
						 bool set_defaults)
{
	job_desc_msg_t *job_desc = xmalloc_nz(sizeof(*job_desc));
	int rc = SLURM_SUCCESS;
	int estimated_ntasks;

	slurm_init_job_desc_msg(job_desc);

	job_desc->account = xstrdup(opt_local->account);
	job_desc->acctg_freq = xstrdup(opt_local->acctg_freq);

	/* admin_comment not filled in here */
	/* alloc_node not filled in here */
	/* alloc_resp_port not filled in here */
	/* alloc_sid not filled in here */
	/* arg[c|v] not filled in here */
	/* array_inx not filled in here */
	/* array_bitmap not filled in here */
	/* batch_features not filled in here */

	job_desc->begin_time = opt_local->begin;
	job_desc->bitflags |= opt_local->job_flags;
	job_desc->burst_buffer = xstrdup(opt_local->burst_buffer);
	job_desc->clusters = xstrdup(opt_local->clusters);
	job_desc->cluster_features = xstrdup(opt_local->c_constraint);
	job_desc->comment = xstrdup(opt_local->comment);
	job_desc->req_context = xstrdup(opt_local->context);

	if (set_defaults || slurm_option_isset(opt_local, "contiguous"))
		job_desc->contiguous = opt_local->contiguous;
	else
		job_desc->contiguous = NO_VAL16;

	job_desc->container = xstrdup(opt_local->container);
	job_desc->container_id = xstrdup(opt_local->container_id);

	if (opt_local->core_spec != NO_VAL16)
		job_desc->core_spec = opt_local->core_spec;

	/* cpu_bind not filled in here */
	/* cpu_bind_type not filled in here */

	job_desc->cpu_freq_min = opt_local->cpu_freq_min;
	job_desc->cpu_freq_max = opt_local->cpu_freq_max;
	job_desc->cpu_freq_gov = opt_local->cpu_freq_gov;

	if (opt_local->cpus_per_gpu)
		xstrfmtcat(job_desc->cpus_per_tres, "gres/gpu:%d",
			   opt_local->cpus_per_gpu);

	/* crontab_entry not filled in here */

	job_desc->deadline = opt_local->deadline;

	if (opt_local->delay_boot != NO_VAL)
		job_desc->delay_boot = opt_local->delay_boot;

	job_desc->dependency = xstrdup(opt_local->dependency);

	/* end_time not filled in here */
	/* environment not filled in here */
	/* env_size not filled in here */

	job_desc->extra = xstrdup(opt_local->extra);
	job_desc->exc_nodes = xstrdup(opt_local->exclude);
	job_desc->features = xstrdup(opt_local->constraint);
	job_desc->prefer = xstrdup(opt_local->prefer);

	/* fed_siblings_active not filled in here */
	/* fed_siblings_viable not filled in here */

	job_desc->group_id = opt_local->gid;

	/* het_job_offset not filled in here */

	if (opt_local->immediate == 1)
		job_desc->immediate = 1;

	/* job_id not filled in here */
	/* job_id_str not filled in here */

	if (opt_local->no_kill)
		job_desc->kill_on_node_fail = 0;

	job_desc->licenses = xstrdup(opt_local->licenses);

	if (set_defaults || slurm_option_isset(opt_local, "mail_type"))
		job_desc->mail_type = opt_local->mail_type;

	job_desc->mail_user = xstrdup(opt_local->mail_user);

	job_desc->mcs_label = xstrdup(opt_local->mcs_label);

	job_desc->mem_bind = xstrdup(opt_local->mem_bind);
	job_desc->mem_bind_type = opt_local->mem_bind_type;

	if (opt_local->mem_per_gpu != NO_VAL64)
		xstrfmtcat(job_desc->mem_per_tres, "gres/gpu:%"PRIu64,
			   opt_local->mem_per_gpu);

	if (set_defaults || slurm_option_isset(opt_local, "name"))
		job_desc->name = xstrdup(opt_local->job_name);

	job_desc->network = xstrdup(opt_local->network);

	if (opt_local->nice != NO_VAL)
		job_desc->nice = NICE_OFFSET + opt_local->nice;

	if (opt_local->ntasks_set) {
		job_desc->bitflags |= JOB_NTASKS_SET;
		job_desc->num_tasks = opt_local->ntasks;
	}

	if (opt_local->open_mode)
		job_desc->open_mode = opt_local->open_mode;

	/* origin_cluster is not filled in here */
	/* other_port not filled in here */

	/*
	 * Estimate ntasks here for use in job_desc->min_cpu calculations
	 * that follow.  ntasks will be filled in later.
	 */
	estimated_ntasks = opt_local->ntasks;
	if ((opt_local->ntasks_per_node > 0) && (!opt_local->ntasks_set) &&
            ((opt_local->min_nodes == opt_local->max_nodes) ||
	     (opt_local->max_nodes == 0)))
		estimated_ntasks =
			opt_local->min_nodes * opt_local->ntasks_per_node;

	if (opt_local->overcommit) {
		if (set_defaults || (opt_local->min_nodes > 0))
			job_desc->min_cpus = MAX(opt_local->min_nodes, 1);
		job_desc->overcommit = opt_local->overcommit;
	} else if (opt_local->cpus_set)
		job_desc->min_cpus =
			estimated_ntasks * opt_local->cpus_per_task;
	else if (opt_local->nodes_set && (opt_local->min_nodes == 0))
		job_desc->min_cpus = 0;
	else if (set_defaults)
		job_desc->min_cpus = estimated_ntasks;

	job_desc->partition = xstrdup(opt_local->partition);

	if (opt_local->plane_size != NO_VAL)
		job_desc->plane_size = opt_local->plane_size;

	if (slurm_option_isset(opt_local, "hold")) {
		if (opt_local->hold)
			job_desc->priority = 0;
		else
			job_desc->priority = INFINITE;
	} else if (opt_local->priority)
		job_desc->priority = opt_local->priority;

	job_desc->profile = opt_local->profile;

	job_desc->qos = xstrdup(opt_local->qos);

	if (opt_local->reboot)
		job_desc->reboot = 1;

	/* resp_host not filled in here */
	/* restart_cnt not filled in here */

	/*
	 * simplify the job allocation nodelist, not laying out tasks until step
	 */
	if (opt_local->nodelist) {
		hostlist_t *hl = hostlist_create(opt_local->nodelist);
		if (!hl) {
			error("Invalid node list specified");
			return NULL;
		}
		xfree(opt_local->nodelist);
		opt_local->nodelist = hostlist_ranged_string_xmalloc(hl);
		if (((opt_local->distribution & SLURM_DIST_STATE_BASE) !=
		      SLURM_DIST_ARBITRARY))
			hostlist_uniq(hl);
		job_desc->req_nodes = hostlist_ranged_string_xmalloc(hl);
		hostlist_destroy(hl);
	}

	if (((opt_local->distribution & SLURM_DIST_STATE_BASE) ==
	     SLURM_DIST_ARBITRARY) && !job_desc->req_nodes) {
		error("With Arbitrary distribution you need to "
		      "specify a nodelist or hostfile with the -w option");
		return NULL;
	}

	/* requeue not filled in here */

	job_desc->reservation = xstrdup(opt_local->reservation);

	if (opt_local->resv_port_cnt != NO_VAL)
		job_desc->resv_port_cnt = opt_local->resv_port_cnt;
	else
		job_desc->resv_port_cnt = NO_VAL16;

	/* script not filled in here */
	/* script_buf not filled in here */

	if (opt_local->segment_size != NO_VAL16)
		job_desc->segment_size = opt_local->segment_size;

	if (opt_local->shared != NO_VAL16)
		job_desc->shared = opt_local->shared;

	/* site_factor not filled in here */

	if (opt_local->spank_job_env_size) {
		job_desc->spank_job_env =
			xcalloc(opt_local->spank_job_env_size,
				sizeof(*job_desc->spank_job_env));
		for (int i = 0; i < opt_local->spank_job_env_size; i++)
			job_desc->spank_job_env[i] =
				xstrdup(opt_local->spank_job_env[i]);
		job_desc->spank_job_env_size = opt_local->spank_job_env_size;
	}

	job_desc->submit_line = opt_local->submit_line;
	job_desc->task_dist = opt_local->distribution;
	job_desc->oom_kill_step = opt_local->oom_kill_step;

	if (opt_local->time_limit != NO_VAL)
		job_desc->time_limit = opt_local->time_limit;
	if (opt_local->time_min != NO_VAL)
		job_desc->time_min = opt_local->time_min;

	job_desc->tres_bind = xstrdup(opt_local->tres_bind);
	job_desc->tres_freq = xstrdup(opt_local->tres_freq);
	xfmt_tres(&job_desc->tres_per_job, "gres/gpu", opt_local->gpus);
	xfmt_tres(&job_desc->tres_per_node, "gres/gpu",
		  opt_local->gpus_per_node);
	/* --gres=none for jobs means no GRES, so don't send it to slurmctld */
	if (opt_local->gres && xstrcasecmp(opt_local->gres, "NONE")) {
		if (job_desc->tres_per_node)
			xstrfmtcat(job_desc->tres_per_node, ",%s",
				   opt_local->gres);
		else
			job_desc->tres_per_node = xstrdup(opt_local->gres);
	}
	xfmt_tres(&job_desc->tres_per_socket, "gres/gpu",
		  opt_local->gpus_per_socket);

	job_desc->tres_per_task = xstrdup(opt_local->tres_per_task);
	job_desc->user_id = opt_local->uid;

	/* wait_all_nodes not filled in here */

	job_desc->warn_flags = opt_local->warn_flags;
	job_desc->warn_signal = opt_local->warn_signal;
	job_desc->warn_time = opt_local->warn_time;

	if (set_defaults || slurm_option_isset(opt_local, "chdir"))
		job_desc->work_dir = xstrdup(opt_local->chdir);

	if (opt_local->cpus_set) {
		job_desc->bitflags |= JOB_CPUS_SET;
		job_desc->cpus_per_task = opt_local->cpus_per_task;
	}

	/* max_cpus not filled in here */

	if (opt_local->nodes_set) {
		job_desc->min_nodes = opt_local->min_nodes;
		if (opt_local->max_nodes) {
			job_desc->max_nodes = opt_local->max_nodes;
			if (opt_local->job_size_str) {
				job_desc->job_size_str =
					xstrdup(opt_local->job_size_str);
			} else
				job_desc->job_size_str = NULL;
		}
	} else if (opt_local->ntasks_set && (opt_local->ntasks == 0)) {
		job_desc->min_nodes = 0;
		job_desc->job_size_str = NULL;
	} else if (opt_local->ntasks_set &&
		   (opt_local->ntasks_per_node != NO_VAL)) {
		job_desc->min_nodes =
			(job_desc->num_tasks /
			 opt_local->ntasks_per_node) +
			((job_desc->num_tasks %
			  opt_local->ntasks_per_node) ? 1 : 0);
	}

	/* boards_per_node not filled in here */
	/* sockets_per_board not filled in here */

	if (opt_local->sockets_per_node != NO_VAL)
		job_desc->sockets_per_node = opt_local->sockets_per_node;
	if (opt_local->cores_per_socket != NO_VAL)
		job_desc->cores_per_socket = opt_local->cores_per_socket;
	if (opt_local->threads_per_core != NO_VAL)
		job_desc->threads_per_core = opt_local->threads_per_core;

	if (opt_local->ntasks_per_node != NO_VAL)
		job_desc->ntasks_per_node = opt_local->ntasks_per_node;
	if (opt_local->ntasks_per_socket != NO_VAL)
		job_desc->ntasks_per_socket = opt_local->ntasks_per_socket;
	if (opt_local->ntasks_per_core != NO_VAL)
		job_desc->ntasks_per_core = opt_local->ntasks_per_core;

	/* ntasks_per_board not filled in here */

	if (opt_local->ntasks_per_tres != NO_VAL)
		job_desc->ntasks_per_tres = opt_local->ntasks_per_tres;
	else if (opt_local->ntasks_per_gpu != NO_VAL)
		job_desc->ntasks_per_tres = opt_local->ntasks_per_gpu;

	if (opt_local->pn_min_cpus > -1)
		job_desc->pn_min_cpus = opt_local->pn_min_cpus;

	if (opt_local->pn_min_memory != NO_VAL64)
		job_desc->pn_min_memory = opt_local->pn_min_memory;
	else if (opt_local->mem_per_cpu != NO_VAL64)
		job_desc->pn_min_memory = opt_local->mem_per_cpu | MEM_PER_CPU;

	if (opt_local->pn_min_tmp_disk != NO_VAL64)
		job_desc->pn_min_tmp_disk = opt_local->pn_min_tmp_disk;

	if (opt_local->req_switch >= 0)
		job_desc->req_switch = opt_local->req_switch;

	/* select_jobinfo not filled in here */
	/* desc->std_[err|in|out] not filled in here */
	/* tres_req_cnt not filled in here */

	if (opt_local->wait4switch >= 0)
		job_desc->wait4switch = opt_local->wait4switch;

	job_desc->wckey = xstrdup(opt_local->wckey);

	job_desc->x11 = opt_local->x11;
	if (job_desc->x11) {
		job_desc->x11_magic_cookie =
			xstrdup(opt_local->x11_magic_cookie);
		job_desc->x11_target = xstrdup(opt_local->x11_target);
		job_desc->x11_target_port = opt_local->x11_target_port;
	}

	/*
	 * If clusters is used we can't validate GRES, since the running
	 * configuration may be using different SelectType than destination
	 * cluster. Validation is still performed on slurmctld.
	 */
	if (!opt_local->clusters) {
		list_t *tmp_gres_list = NULL;
		gres_job_state_validate_t gres_js_val = {
			.cpus_per_tres = job_desc->cpus_per_tres,
			.mem_per_tres = job_desc->mem_per_tres,
			.tres_freq = job_desc->tres_freq,
			.tres_per_job = job_desc->tres_per_job,
			.tres_per_node = job_desc->tres_per_node,
			.tres_per_socket = job_desc->tres_per_socket,
			.tres_per_task = job_desc->tres_per_task,

			.cpus_per_task = &job_desc->cpus_per_task,
			.max_nodes = &job_desc->max_nodes,
			.min_cpus = &job_desc->min_cpus,
			.min_nodes = &job_desc->min_nodes,
			.ntasks_per_node = &job_desc->ntasks_per_node,
			.ntasks_per_socket = &job_desc->ntasks_per_socket,
			.ntasks_per_tres = &job_desc->ntasks_per_tres,
			.num_tasks = &job_desc->num_tasks,
			.sockets_per_node = &job_desc->sockets_per_node,

			.gres_list = &tmp_gres_list,
		};

		rc = gres_job_state_validate(&gres_js_val);
		FREE_NULL_LIST(tmp_gres_list);
	}

	if (rc) {
		error("%s", slurm_strerror(rc));
		return NULL;
	}

	return job_desc;
}

/*
 * Compatible with shell/bash completions.
 */
extern void suggest_completion(struct option *opts, const char *query)
{
	char *suggest = NULL, *flag = NULL;
	bool query_short = false, query_long = false;
	int i = 0;
	char ifs = '\n';

	/* Bail on invalid input. */
	if ((!opts) || (!query) || (query[0] == '\0'))
		return;

	/*
	 * It is desirable to be able to query just for short or long flags.
	 * Being able to query both flag types under certain circumstances
	 * allows flexibility and convenience.
	 */
	query_short = (query[0] == '-') || isalpha(query[0]);
	query_long = (strlen(query) > 1) || isalpha(query[0]);

	for (i = 0; opts[i].name || opts[i].val; i++) {
		/* Handle short flags */
		if (isalpha(opts[i].val) && query_short) {
			flag = xstrdup_printf("-%c", (char)opts[i].val);
			if (xstrstr(flag, query))
				xstrfmtcat(suggest, "%s%c", flag, ifs);

			xfree(flag);
		}

		/* Handle long flags */
		if (opts[i].name && query_long) {
			flag = xstrdup_printf("--%s", opts[i].name);
			if (!xstrstr(flag, query)) {
				xfree(flag);
				continue;
			}
			if (opts[i].has_arg)
				xstrfmtcat(suggest, "%s=%c", flag, ifs);
			if (opts[i].has_arg == optional_argument)
				xstrfmtcat(suggest, "%s %c", flag, ifs);
			if (opts[i].has_arg == no_argument)
				xstrfmtcat(suggest, "%s%c", flag, ifs);
			xfree(flag);
		}
	}

	if (suggest)
		fprintf(stdout, "%s\n", suggest);

	xfree(suggest);
}
