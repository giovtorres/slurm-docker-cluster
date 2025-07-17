/*****************************************************************************\
 *  acct_gather_energy_xcc.c - slurm energy accounting plugin for xcc.
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Based on IPMI plugin by Thomas Cadeau/Yoann Blein @ Bull
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
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
\*****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <math.h>

#include "src/common/slurm_xlator.h"
#include "src/interfaces/acct_gather_energy.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/fd.h"
#include "src/common/xstring.h"

#include "src/interfaces/proctrack.h"

#include "src/slurmd/slurmd/slurmd.h"

/*
 * freeipmi includes for the lib
 */
#include <freeipmi/freeipmi.h>

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
extern slurmd_conf_t *conf __attribute__((weak_import));
#else
slurmd_conf_t *conf = NULL;
#endif

#define DEFAULT_IPMI_FREQ 30
#define DEFAULT_IPMI_USER "USERID"
#define DEFAULT_IPMI_PASS "PASSW0RD"
#define DEFAULT_IPMI_TIMEOUT 10

#define IPMI_VERSION 2		/* Data structure version number */
#define MAX_LOG_ERRORS 5	/* Max sensor reading errors log messages */
#define XCC_MIN_RES 50		/* Minimum resolution for XCC readings, in ms */
#define IPMI_RAW_MAX_ARGS 256 /* Max XCC response length in bytes*/
/*FIXME: Investigate which is the OVERFLOW limit for XCC*/
#define IPMI_XCC_OVERFLOW INFINITE /* XCC overflows at X */

#define XCC_FLAG_NONE 0x00000000
#define XCC_FLAG_FAKE 0x00000001
/* Match cmd_rq[] response expectations */
#define XCC_SD650_RESPONSE_LEN 16
#define XCC_SD650V2_RESPONSE_LEN 40

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for Slurm job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */

const char plugin_name[] = "AcctGatherEnergy XCC plugin";
const char plugin_type[] = "acct_gather_energy/xcc";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

typedef struct slurm_ipmi_conf {
	/* Adjust/approach the consumption
	 * in function of time between ipmi update and read call */
	bool adjustment;
	/* authentication type to use
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_NONE                  = 0x00,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_STRAIGHT_PASSWORD_KEY = 0x01,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_MD2                   = 0x02,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_MD5                   = 0x03,
	 * Pass < 0 for default of IPMI_MONITORING_AUTHENTICATION_TYPE_MD5*/
	uint32_t authentication_type;
	/* Cipher suite identifier to determine authentication, integrity,
	 * and confidentiality algorithms to use.
	 * Supported Cipher Suite IDs
	 * (Key: A - Authentication Algorithm
	 *       I - Integrity Algorithm
	 *       C - Confidentiality Algorithm)
	 *   0 - A = None; I = None; C = None
	 *   1 - A = HMAC-SHA1; I = None; C = None
	 *   2 - A = HMAC-SHA1; I = HMAC-SHA1-96; C = None
	 *   3 - A = HMAC-SHA1; I = HMAC-SHA1-96; C = AES-CBC-128
	 *   6 - A = HMAC-MD5; I = None; C = None
	 *   7 - A = HMAC-MD5; I = HMAC-MD5-128; C = None
	 *   8 - A = HMAC-MD5; I = HMAC-MD5-128; C = AES-CBC-128
	 *   11 - A = HMAC-MD5; I = MD5-128; C = None
	 *   12 - A = HMAC-MD5; I = MD5-128; C = AES-CBC-128
	 *   15 - A = HMAC-SHA256; I = None; C = None
	 *   16 - A = HMAC-SHA256; I = HMAC-SHA256-128; C = None
	 *   17 - A = HMAC-SHA256; I = HMAC-SHA256-128; C = AES-CBC-128
	 * Pass < 0 for default.of 3.*/
	uint32_t cipher_suite_id;
	/* Flag informs the library if in-band driver information should be
	 * probed or not.*/
	uint32_t disable_auto_probe;
	/* Use this specified driver address instead of a probed one.*/
	uint32_t driver_address;
	/* Use this driver device for the IPMI driver.*/
	char *driver_device;
	/* Options for IPMI configuration*/
	/* Use a specific in-band driver.
	 *   IPMI_MONITORING_DRIVER_TYPE_KCS      = 0x00,
	 *   IPMI_MONITORING_DRIVER_TYPE_SSIF     = 0x01,
	 *   IPMI_MONITORING_DRIVER_TYPE_OPENIPMI = 0x02,
	 *   IPMI_MONITORING_DRIVER_TYPE_SUNBMC   = 0x03,
	 *    Pass < 0 for default of IPMI_MONITORING_DRIVER_TYPE_KCS.*/
	uint32_t driver_type;
	uint32_t flags;
	/* frequency for ipmi call*/
	uint32_t freq;
	uint32_t ipmi_flags;
	/* BMC password. Pass NULL ptr for default password.  Standard
	 * default is the null (e.g. empty) password.  Maximum length of 20
	 * bytes.*/
	char *password;
	/* privilege level to authenticate with.
	 * Supported privilege levels:
	 *   0 = IPMICONSOLE_PRIVILEGE_USER
	 *   1 = IPMICONSOLE_PRIVILEGE_OPERATOR
	 *   2 = IPMICONSOLE_PRIVILEGE_ADMIN
	 * Pass < 0 for default of IPMICONSOLE_PRIVILEGE_ADMIN.*/
	uint32_t privilege_level;
	/* Flag informs the library if in-band driver information should be
	 * probed or not.*/
	/* Indicate the IPMI protocol version to use
	 * IPMI_MONITORING_PROTOCOL_VERSION_1_5 = 0x00,
	 * IPMI_MONITORING_PROTOCOL_VERSION_2_0 = 0x01,
	 * Pass < 0 for default of IPMI_MONITORING_VERSION_1_5.*/
	uint32_t protocol_version;
	/* Use this register space instead of the probed one.*/
	uint32_t register_spacing;
	/* Specifies the packet retransmission timeout length in
	 * milliseconds.  Pass <= 0 to default 500 (0.5 seconds).*/
	uint32_t retransmission_timeout;
	/* Specifies the session timeout length in milliseconds.  Pass <= 0
	 * to default 60000 (60 seconds).*/
	uint32_t session_timeout;
	uint8_t target_channel_number;
	bool target_channel_number_is_set;
	uint8_t target_address;
	bool target_address_is_set;
	/* Timeout for the ipmi thread */
	uint32_t timeout;
	/* BMC username. Pass NULL ptr for default username.  Standard
	 * default is the null (e.g. empty) username.  Maximum length of 16
	 * bytes.*/
	char *username;
	/* Bitwise OR of flags indicating IPMI implementation changes.  Some
	 * BMCs which are non-compliant and may require a workaround flag
	 * for correct operation. Pass IPMICONSOLE_WORKAROUND_DEFAULT for
	 * default.  Standard default is 0, no modifications to the IPMI
	 * protocol.*/
	uint32_t workaround_flags;
} slurm_ipmi_conf_t;

typedef enum xcc_version {
	XCC_SD650_VERSION = 0,
	XCC_SD650V2_VERSION
} xcc_version_t;

/* Struct to store the raw single data command reading */
typedef struct xcc_raw_single_data {
	uint16_t fifo_inx;	/* Not used. */
	uint32_t j;		/* Joules. */
	uint16_t mj;		/* Millijoules. */
	uint16_t ms;		/* Milliseconds. */
	uint16_t mw;		/* Milliwatts. */
	uint32_t s;		/* Seconds. */
	xcc_version_t version;	/* Version. */
	uint32_t w;		/* Watts. */
} xcc_raw_single_data_t;

static acct_gather_energy_t xcc_energy;

/* LUN, NetFN, CMD, Data[n]*/
static uint8_t cmd_rq[8] = { 0x00, 0x3A, 0x32, 4, 2, 0, 0, 0 };
static unsigned int cmd_rq_len = 8;


static int dataset_id = -1; /* id of the dataset for profile data */

static slurm_ipmi_conf_t slurm_ipmi_conf;
static bool flag_energy_accounting_shutdown = false;
static bool flag_thread_started = false;
static bool flag_init = false;

static pthread_mutex_t ipmi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ipmi_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t launch_cond = PTHREAD_COND_INITIALIZER;
static pthread_t thread_ipmi_id_launcher = 0;
static pthread_t thread_ipmi_id_run = 0;
static stepd_step_rec_t *step = NULL;
static int context_id = -1;

static void _reset_slurm_ipmi_conf(slurm_ipmi_conf_t *slurm_ipmi_conf)
{
	if (slurm_ipmi_conf) {
		slurm_ipmi_conf->adjustment = false;
		slurm_ipmi_conf->authentication_type = 0;
		slurm_ipmi_conf->cipher_suite_id = 0;
		slurm_ipmi_conf->disable_auto_probe = 0;
		slurm_ipmi_conf->driver_address = 0;
		xfree(slurm_ipmi_conf->driver_device);
		slurm_ipmi_conf->driver_type = NO_VAL;
		slurm_ipmi_conf->flags = XCC_FLAG_NONE;
		slurm_ipmi_conf->freq = DEFAULT_IPMI_FREQ;
		slurm_ipmi_conf->ipmi_flags = IPMI_FLAGS_DEFAULT;
		xfree(slurm_ipmi_conf->password);
		slurm_ipmi_conf->password = xstrdup(DEFAULT_IPMI_PASS);
		slurm_ipmi_conf->privilege_level = 0;
		slurm_ipmi_conf->protocol_version = 0;
		slurm_ipmi_conf->register_spacing = 0;
		slurm_ipmi_conf->retransmission_timeout = 0;
		slurm_ipmi_conf->session_timeout = 0;
		slurm_ipmi_conf->target_channel_number = 0x00;
		slurm_ipmi_conf->target_channel_number_is_set = false;
		slurm_ipmi_conf->target_address = 0x20;
		slurm_ipmi_conf->target_address_is_set = false;
		slurm_ipmi_conf->timeout = DEFAULT_IPMI_TIMEOUT;
		xfree(slurm_ipmi_conf->username);
		slurm_ipmi_conf->username = xstrdup(DEFAULT_IPMI_USER);
		slurm_ipmi_conf->workaround_flags = 0; // See man 8 ipmi-raw
	}
}

static int _running_profile(void)
{
	static bool run = false;
	static uint32_t profile_opt = ACCT_GATHER_PROFILE_NOT_SET;

	if (profile_opt == ACCT_GATHER_PROFILE_NOT_SET) {
		acct_gather_profile_g_get(ACCT_GATHER_PROFILE_RUNNING,
					  &profile_opt);
		if (profile_opt & ACCT_GATHER_PROFILE_ENERGY)
			run = true;
	}

	return run;
}

static void _close_ipmi_context(ipmi_ctx_t *ipmi_ctx_p)
{
	if (!*ipmi_ctx_p)
		return;

	ipmi_ctx_close(*ipmi_ctx_p);
	ipmi_ctx_destroy(*ipmi_ctx_p);
	*ipmi_ctx_p = NULL;
}

/*
 * _init_ipmi_config initializes parameters for freeipmi library
 */
static int _init_ipmi_config(ipmi_ctx_t *ipmi_ctx_p)
{
	int ret = 0;
	ipmi_ctx_t ipmi_ctx = *ipmi_ctx_p;

	unsigned int workaround_flags_mask =
		(IPMI_WORKAROUND_FLAGS_INBAND_ASSUME_IO_BASE_ADDRESS
		 | IPMI_WORKAROUND_FLAGS_INBAND_SPIN_POLL);

	if (ipmi_ctx) {
		debug("ipmi_ctx already initialized");
		return SLURM_SUCCESS;
	}

	if (!(ipmi_ctx = ipmi_ctx_create())) {
		error("ipmi_ctx_create: %s", strerror(errno));
		goto cleanup;
	}

	if (getuid() != 0) {
		error("%s: must be root to open IPMI devices", __func__);
		goto cleanup;
	}

	/* XCC OEM commands always require to use in-band communication */
	if (((slurm_ipmi_conf.driver_type > 0) &&
	     (slurm_ipmi_conf.driver_type != NO_VAL) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_KCS) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_SSIF) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_OPENIPMI) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_SUNBMC))
	    || (slurm_ipmi_conf.workaround_flags & ~workaround_flags_mask)) {
		/* IPMI ERROR PARAMETERS */
		error("%s: XCC Lenovo plugin only supports in-band communication, incorrect driver type or workaround flags",
		      __func__);

		debug("slurm_ipmi_conf.driver_type=%u slurm_ipmi_conf.workaround_flags=%u",
		      slurm_ipmi_conf.driver_type,
		      slurm_ipmi_conf.workaround_flags);

		goto cleanup;
	}

	if (slurm_ipmi_conf.driver_type == NO_VAL) {
		if ((ret = ipmi_ctx_find_inband(
			     ipmi_ctx,
			     NULL,
			     slurm_ipmi_conf.disable_auto_probe,
			     slurm_ipmi_conf.driver_address,
			     slurm_ipmi_conf.register_spacing,
			     slurm_ipmi_conf.driver_device,
			     slurm_ipmi_conf.workaround_flags,
			     slurm_ipmi_conf.ipmi_flags)) <= 0) {
			error("%s: error on ipmi_ctx_find_inband: %s",
			      __func__, ipmi_ctx_errormsg(ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u",
			      slurm_ipmi_conf.driver_type,
			      slurm_ipmi_conf.disable_auto_probe,
			      slurm_ipmi_conf.driver_address,
			      slurm_ipmi_conf.register_spacing,
			      slurm_ipmi_conf.driver_device,
			      slurm_ipmi_conf.workaround_flags,
			      slurm_ipmi_conf.ipmi_flags);

			goto cleanup;
		}
	} else {
		if ((ipmi_ctx_open_inband(ipmi_ctx,
					  slurm_ipmi_conf.driver_type,
					  slurm_ipmi_conf.disable_auto_probe,
					  slurm_ipmi_conf.driver_address,
					  slurm_ipmi_conf.register_spacing,
					  slurm_ipmi_conf.driver_device,
					  slurm_ipmi_conf.workaround_flags,
					  slurm_ipmi_conf.ipmi_flags) < 0)) {
			error("%s: error on ipmi_ctx_open_inband: %s",
			      __func__, ipmi_ctx_errormsg(ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u",
			      slurm_ipmi_conf.driver_type,
			      slurm_ipmi_conf.disable_auto_probe,
			      slurm_ipmi_conf.driver_address,
			      slurm_ipmi_conf.register_spacing,
			      slurm_ipmi_conf.driver_device,
			      slurm_ipmi_conf.workaround_flags,
			      slurm_ipmi_conf.ipmi_flags);
			goto cleanup;
		}
	}

	if (slurm_ipmi_conf.target_channel_number_is_set
	    || slurm_ipmi_conf.target_address_is_set) {
		if (ipmi_ctx_set_target(
			    ipmi_ctx,
			    slurm_ipmi_conf.target_channel_number_is_set ?
			    &slurm_ipmi_conf.target_channel_number : NULL,
			    slurm_ipmi_conf.target_address_is_set ?
			    &slurm_ipmi_conf.target_address : NULL) < 0) {
			error("%s: error on ipmi_ctx_set_target: %s",
			      __func__, ipmi_ctx_errormsg(ipmi_ctx));
			goto cleanup;
		}
	}

	*ipmi_ctx_p = ipmi_ctx;

	return SLURM_SUCCESS;

cleanup:
	_close_ipmi_context(&ipmi_ctx);
	return SLURM_ERROR;
}

/*
 * _read_ipmi_values read the Power sensor and update last_update_watt and times
 */
static xcc_raw_single_data_t *_read_ipmi_values(ipmi_ctx_t *ipmi_ctx_p)
{
	xcc_raw_single_data_t *xcc_reading;
	ipmi_ctx_t ipmi_ctx = *ipmi_ctx_p;
	uint8_t buf_rs[IPMI_RAW_MAX_ARGS];
	int rs_len = 0;

	if (!IPMI_NET_FN_RQ_VALID(cmd_rq[1])) {
		error("Invalid netfn value");
		return NULL;
	}

	rs_len = ipmi_cmd_raw(ipmi_ctx,
			      cmd_rq[0], // Lun (logical unit number)
			      cmd_rq[1], // Net Function
			      &cmd_rq[2], // Command number + request data
			      cmd_rq_len - 2, // Length (in bytes)
			      &buf_rs, // response buffer
			      IPMI_RAW_MAX_ARGS // max response length
		);

	debug3("ipmi_cmd_raw: %s", ipmi_ctx_errormsg(ipmi_ctx));

	if ((rs_len != XCC_SD650_RESPONSE_LEN) &&
	    (rs_len != XCC_SD650V2_RESPONSE_LEN)) {
		error("Invalid ipmi response length for XCC raw command: %d bytes, expected %d (SD650) or %d (SD650V2)",
		      rs_len, XCC_SD650_RESPONSE_LEN, XCC_SD650V2_RESPONSE_LEN);
		return NULL;
	}

	/* Due to memory alineation we must copy the data from the buffer */
	xcc_reading = xmalloc(sizeof(xcc_raw_single_data_t));
	if (slurm_ipmi_conf.flags & XCC_FLAG_FAKE) {
		static uint32_t fake_past_read = 10774496;
		static bool fake_inited = false;

		if (!fake_inited) {
			srand((unsigned) time(NULL));
			fake_inited = true;
		}

		xcc_reading->version = XCC_SD650_VERSION;
		xcc_reading->fifo_inx = 0;
		// Fake metric j
		xcc_reading->j = fake_past_read + 550 + rand() % 200;
		fake_past_read = xcc_reading->j;
		xcc_reading->mj = 0;
		xcc_reading->w = 0;
		xcc_reading->mw = 0;
		xcc_reading->s = time(NULL); //Fake metric timestamp
		xcc_reading->ms = 0;
	} else if (rs_len == XCC_SD650_RESPONSE_LEN) {
		/*
		 * Ignore response header first 2 bytes:
		 * Byte 0: command number, cmd_rq[2]. Set by freeipmi.
		 * Byte 1: command completion code. Set by XCC IPMI.
		 */
		xcc_reading->version = XCC_SD650_VERSION;
		xcc_reading->w = 0;
		xcc_reading->mw = 0;
		memcpy(&xcc_reading->fifo_inx, buf_rs + 2, 2);
		memcpy(&xcc_reading->j, buf_rs + 4, 4);
		memcpy(&xcc_reading->mj, buf_rs + 8, 2);
		memcpy(&xcc_reading->s, buf_rs + 10, 4);
		memcpy(&xcc_reading->ms, buf_rs + 14, 2);
	} else {
		/* Ignore response header first 2 bytes like above. */
		int count = 0;

		xcc_reading->version = XCC_SD650V2_VERSION;
		xcc_reading->fifo_inx = 0;
		xcc_reading->j = 0;
		xcc_reading->mj = 0;
		memcpy(&count, buf_rs + 2, 2);
		if (count) {
			memcpy(&xcc_reading->w, buf_rs + 4, 4);
			xcc_reading->w /= count;
			memcpy(&xcc_reading->mw, buf_rs + 8, 2);
		}
		memcpy(&xcc_reading->s, buf_rs + 34, 4);
		memcpy(&xcc_reading->ms, buf_rs + 38, 2);
	}

	return xcc_reading;
}

static void _sd650_update_node_energy(xcc_raw_single_data_t *xcc_raw)
{
	static uint16_t overflows = 0; /* Number of overflows of the counter */
	int elapsed = 0;
	static uint64_t first_consumed_energy = 0;

	if (!xcc_energy.poll_time) {
		/*
		 * First number from the slurmd.  We will figure out the usage
		 * by subtracting this each time.
		 */
		first_consumed_energy =	xcc_raw->j;
		xcc_energy.consumed_energy = 0;
		xcc_energy.base_consumed_energy = 0;
		xcc_energy.previous_consumed_energy = 0;
		xcc_energy.ave_watts = 0;
	} else {
		xcc_energy.previous_consumed_energy =
			xcc_energy.consumed_energy;

		/* Detect first overflow */
		if (!overflows && xcc_raw->j < xcc_energy.consumed_energy) {
			xcc_energy.consumed_energy = IPMI_XCC_OVERFLOW -
						first_consumed_energy +
						xcc_raw->j;
			overflows++;
		} else if (!overflows &&
			   (xcc_raw->j >= xcc_energy.consumed_energy)) {
			xcc_energy.consumed_energy = xcc_raw->j;
			xcc_energy.consumed_energy -= first_consumed_energy;
		} else if (overflows) {
			/*
			 * Offset = First overflow + consecutive overflows
			 * If it happens that the offset + xcc_raw->j is less
			 * than the past consumed energy, it means that we
			 * overflowed and must add a new overflow to the count
			 */
			uint64_t offset = IPMI_XCC_OVERFLOW -
				first_consumed_energy +
				(IPMI_XCC_OVERFLOW * (overflows - 1));

			if ((offset + xcc_raw->j) <
			    xcc_energy.consumed_energy) {
				overflows++;
				xcc_energy.consumed_energy = offset +
					IPMI_XCC_OVERFLOW + xcc_raw->j;
			} else {
				xcc_energy.consumed_energy += offset +
					xcc_raw->j;
			}
		}

		xcc_energy.base_consumed_energy =
			xcc_energy.consumed_energy -
			xcc_energy.previous_consumed_energy;

		elapsed = xcc_raw->s - xcc_energy.poll_time;
	}

	xcc_energy.poll_time = xcc_raw->s;

	if (elapsed && xcc_energy.base_consumed_energy) {
		static uint64_t readings = 0;
		xcc_energy.current_watts =
			round((double)xcc_energy.base_consumed_energy /
			      (double)elapsed);

		/* ave_watts is used as TresUsageOutAve (AvePower) */
		xcc_energy.ave_watts = ((xcc_energy.ave_watts * readings) +
					 xcc_energy.current_watts) /
					 (readings + 1);
		readings++;
	}

	log_flag(ENERGY, "current_watts: %u consumed energy last interval: %"PRIu64"(current reading %"PRIu64") Joules, elapsed time: %u seconds, first read energy counter val: %"PRIu64" ave watts: %u",
		 xcc_energy.current_watts, xcc_energy.base_consumed_energy,
		 xcc_energy.consumed_energy, elapsed, first_consumed_energy,
		 xcc_energy.ave_watts);
}

static void _sd650v2_update_node_energy(xcc_raw_single_data_t *xcc_raw)
{
	static uint64_t readings = 0;
	int elapsed = 0;

	if (!xcc_energy.poll_time) {
		/* First number from the slurmd. */
		xcc_energy.consumed_energy = 0;
		xcc_energy.base_consumed_energy = 0;
		xcc_energy.previous_consumed_energy = 0;
		xcc_energy.ave_watts = xcc_raw->w;
		xcc_energy.current_watts = xcc_raw->w;
		readings++;
	} else {
		elapsed = xcc_raw->s - xcc_energy.poll_time;
		if (elapsed) {
			xcc_energy.previous_consumed_energy =
						xcc_energy.consumed_energy;

			/*
			 * We have no clue about the watt variation in between
			 * the sampling interval. Best effort: mean value.
			 */
			xcc_energy.consumed_energy +=
			round((double)elapsed * 0.5 *
			      (double)(xcc_energy.current_watts + xcc_raw->w));

			xcc_energy.base_consumed_energy =
					xcc_energy.consumed_energy -
					xcc_energy.previous_consumed_energy;

			xcc_energy.current_watts = xcc_raw->w;
			/* ave_watts is used as TresUsageOutAve (AvePower) */
			xcc_energy.ave_watts =
					((xcc_energy.ave_watts * readings) +
					 xcc_energy.current_watts) /
					 (readings + 1);
			readings++;
		}
	}

	xcc_energy.poll_time = xcc_raw->s;

	log_flag(ENERGY, "XCC current_watts: %u consumed energy last interval: %"PRIu64"(current reading %"PRIu64") Joules, elapsed time: %u seconds, ave watts: %u",
		 xcc_energy.current_watts, xcc_energy.base_consumed_energy,
		 xcc_energy.consumed_energy, elapsed, xcc_energy.ave_watts);
}

/*
 * _thread_update_node_energy calls _read_ipmi_values and updates all values
 * for node consumption.
 */
static int _thread_update_node_energy(ipmi_ctx_t *ipmi_ctx_p)
{
	int rc = SLURM_SUCCESS;
	xcc_raw_single_data_t *xcc_raw;

	xcc_raw = _read_ipmi_values(ipmi_ctx_p);

	if (!xcc_raw) {
		error("%s could not read XCC ipmi values", __func__);
		return SLURM_ERROR;
	}

	switch(xcc_raw->version) {
	case XCC_SD650_VERSION:
		_sd650_update_node_energy(xcc_raw);
		break;
	case XCC_SD650V2_VERSION:
		_sd650v2_update_node_energy(xcc_raw);
		break;
	default:
		error("Unimplemented energy calculation for returned data");
		rc = SLURM_ERROR;
	}

	xfree(xcc_raw);

	return rc;
}

static int _ipmi_send_profile(void)
{
	/*
	 * This enum is directly related to the xcc_labels below.  If this is
	 * changed it should be altered as well.
	 */
	enum {
		XCC_ENERGY = 0,
		XCC_CURR_POWER,
		XCC_LABEL_CNT
	};

	static char *xcc_labels[] = { "Energy", "CurrPower", NULL };
	uint16_t i;
	uint64_t data[XCC_LABEL_CNT];

	if (!_running_profile())
		return SLURM_SUCCESS;

	if (dataset_id < 0) {
		acct_gather_profile_dataset_t dataset[XCC_LABEL_CNT + 1];
		for (i = 0; i < XCC_LABEL_CNT; i++) {
			dataset[i].name = xcc_labels[i];
			dataset[i].type = PROFILE_FIELD_UINT64;
		}
		dataset[i].name = NULL;
		dataset[i].type = PROFILE_FIELD_NOT_SET;

		dataset_id = acct_gather_profile_g_create_dataset(
			"Energy", NO_PARENT, dataset);

		log_flag(ENERGY, "Energy: dataset created (id = %d)",
			 dataset_id);
		if (dataset_id == SLURM_ERROR) {
			error("Energy: Failed to create the dataset for IPMI");
			return SLURM_ERROR;
		}
	}

	/* pack an array of uint64_t with current power of sensors */
	memset(data, 0, sizeof(data));
	data[XCC_ENERGY] = xcc_energy.base_consumed_energy;
	data[XCC_CURR_POWER] = xcc_energy.current_watts;
	if (slurm_conf.debug_flags & DEBUG_FLAG_PROFILE)
		for (i = 0; i < XCC_LABEL_CNT; i++)
			info("PROFILE-Energy: %s=%"PRIu64,
			     xcc_labels[i], data[i]);

	return acct_gather_profile_g_add_sample_data(
		dataset_id, (void *)data, xcc_energy.poll_time);
}


/*
 * _thread_ipmi_run is the thread calling ipmi and launching _thread_ipmi_write
 */
static void *_thread_ipmi_run(void *no_data)
{
// need input (attr)
	struct timeval tvnow;
	struct timespec abs;
	ipmi_ctx_t ipmi_ctx = NULL;

	flag_energy_accounting_shutdown = false;
	log_flag(ENERGY, "ipmi-thread: launched");

	(void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	(void) pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	slurm_mutex_lock(&ipmi_mutex);
	if (_init_ipmi_config(&ipmi_ctx) != SLURM_SUCCESS) {
		log_flag(ENERGY, "ipmi-thread: aborted");
		slurm_mutex_unlock(&ipmi_mutex);

		slurm_cond_signal(&launch_cond);

		return NULL;
	}

	(void) pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	slurm_mutex_unlock(&ipmi_mutex);
	flag_thread_started = true;

	slurm_cond_signal(&launch_cond);

	/* setup timer */
	gettimeofday(&tvnow, NULL);
	abs.tv_sec = tvnow.tv_sec;
	abs.tv_nsec = tvnow.tv_usec * 1000;

	//loop until slurm stop
	slurm_mutex_lock(&ipmi_mutex);
	while (!flag_energy_accounting_shutdown) {
		_thread_update_node_energy(&ipmi_ctx);

		/* Sleep until the next time. */
		abs.tv_sec += slurm_ipmi_conf.freq;
		slurm_cond_timedwait(&ipmi_cond, &ipmi_mutex, &abs);
	}
	slurm_mutex_unlock(&ipmi_mutex);

	_close_ipmi_context(&ipmi_ctx);
	log_flag(ENERGY, "ipmi-thread: ended");

	return NULL;
}

static void *_thread_launcher(void *no_data)
{
	//what arg would countain? frequency, socket?
	struct timeval tvnow;
	struct timespec abs;

	slurm_thread_create(&thread_ipmi_id_run, _thread_ipmi_run, NULL);

	/* setup timer */
	gettimeofday(&tvnow, NULL);
	abs.tv_sec = tvnow.tv_sec + slurm_ipmi_conf.timeout;
	abs.tv_nsec = tvnow.tv_usec * 1000;

	slurm_mutex_lock(&launch_mutex);
	slurm_cond_timedwait(&launch_cond, &launch_mutex, &abs);
	slurm_mutex_unlock(&launch_mutex);

	if (!flag_thread_started) {
		error("%s threads failed to start in a timely manner",
		      plugin_name);

		flag_energy_accounting_shutdown = true;

		/*
		 * It is a known thing we can hang up on IPMI calls cancel if
		 * we must.
		 */
		pthread_cancel(thread_ipmi_id_run);

		/*
		 * Unlock just to make sure since we could have canceled the
		 * thread while in the lock.
		 */
		slurm_mutex_unlock(&ipmi_mutex);
	}

	return NULL;
}

static int _get_joules_task(uint16_t delta)
{
	acct_gather_energy_t *new = NULL;
	uint16_t sensor_cnt = 0;
	static bool first = true;
	static uint64_t first_consumed_energy = 0;

	xassert(context_id != -1);

	/*
	 * 'delta' parameter means "use cache" if data is newer than delta
	 * seconds ago, otherwise just inquiry ipmi again.
	 */
	if (slurm_get_node_energy(conf->node_name, context_id, delta,
				  &sensor_cnt, &new)) {
		if (errno == ESLURMD_TOO_MANY_RPCS)
			log_flag(ENERGY, "energy RPC limit reached on slurmd, request dropped");
		else
			error("%s: can't get info from slurmd", __func__);
		return SLURM_ERROR;
	}

	if (sensor_cnt != 1) {
		error("%s: received %u XCC sensors expected 1",
		      __func__, sensor_cnt);
		acct_gather_energy_destroy(new);
		return SLURM_ERROR;
	}

	new->previous_consumed_energy = xcc_energy.consumed_energy;

	if (!first) {
		/* if slurmd is reloaded while the step is alive */
		if (xcc_energy.consumed_energy > new->consumed_energy)
			new->base_consumed_energy = new->consumed_energy;
		else {
			new->consumed_energy -= first_consumed_energy;
			new->base_consumed_energy =
				new->consumed_energy -
				new->previous_consumed_energy;
		}
	} else {
		if (!new->consumed_energy) {
			info("we got a blank");
			goto end_it;
		}

		/*
		 * This is just for the step, so take all the previous
		 * consumption out of the mix.
		 */
		first_consumed_energy = new->consumed_energy;
		new->base_consumed_energy = 0;
		first = false;
	}

	new->consumed_energy = new->previous_consumed_energy
		+ new->base_consumed_energy;
	memcpy(&xcc_energy, new, sizeof(acct_gather_energy_t));

	log_flag(ENERGY, "consumed %"PRIu64" Joules (received %"PRIu64"(%u watts) from slurmd)",
		 xcc_energy.consumed_energy, xcc_energy.base_consumed_energy,
		 xcc_energy.current_watts);

end_it:
	acct_gather_energy_destroy(new);

	return SLURM_SUCCESS;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	return SLURM_SUCCESS;
}

extern int fini(void)
{
	if (!running_in_slurmd_stepd())
		return SLURM_SUCCESS;

	flag_energy_accounting_shutdown = true;

	/* clean up the launch thread */
	slurm_cond_signal(&launch_cond);

	slurm_thread_join(thread_ipmi_id_launcher);

	/* clean up the run thread */
	slurm_cond_signal(&ipmi_cond);

	slurm_mutex_lock(&ipmi_mutex);
	_reset_slurm_ipmi_conf(&slurm_ipmi_conf);
	slurm_mutex_unlock(&ipmi_mutex);

	slurm_thread_join(thread_ipmi_id_run);

	return SLURM_SUCCESS;
}

extern int acct_gather_energy_p_update_node_energy(void)
{
	int rc = SLURM_SUCCESS;
	xassert(running_in_slurmd_stepd());

	return rc;
}

extern int acct_gather_energy_p_get_data(enum acct_energy_type data_type,
					 void *data)
{
	int rc = SLURM_SUCCESS;
	acct_gather_energy_t *energy = (acct_gather_energy_t *)data;
	time_t *last_poll = (time_t *)data;
	uint16_t *sensor_cnt = (uint16_t *)data;

	xassert(running_in_slurmd_stepd());

	switch (data_type) {
	case ENERGY_DATA_NODE_ENERGY_UP:
	case ENERGY_DATA_JOULES_TASK:
		if (running_in_slurmd()) {
			/* Signal the thread to update node energy */
			slurm_cond_signal(&ipmi_cond);
			slurm_mutex_lock(&ipmi_mutex);
		} else {
			slurm_mutex_lock(&ipmi_mutex);
			_get_joules_task(10);
		}
		memcpy(energy, &xcc_energy, sizeof(acct_gather_energy_t));
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_NODE_ENERGY:
	case ENERGY_DATA_STRUCT:
		slurm_mutex_lock(&ipmi_mutex);
		memcpy(energy, &xcc_energy, sizeof(acct_gather_energy_t));
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_LAST_POLL:
		slurm_mutex_lock(&ipmi_mutex);
		*last_poll = xcc_energy.poll_time;
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_SENSOR_CNT:
		*sensor_cnt = 1;
		break;
	default:
		error("acct_gather_energy_p_get_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern int acct_gather_energy_p_set_data(enum acct_energy_type data_type,
					 void *data)
{
	int rc = SLURM_SUCCESS;
	int *delta = (int *)data;

	xassert(running_in_slurmd_stepd());

	switch (data_type) {
	case ENERGY_DATA_RECONFIG:
		break;
	case ENERGY_DATA_PROFILE:
		slurm_mutex_lock(&ipmi_mutex);
		_get_joules_task(*delta);
		_ipmi_send_profile();
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_STEP_PTR:
		/* set global job if needed later */
		step = (stepd_step_rec_t *)data;
		break;
	default:
		error("acct_gather_energy_p_set_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern void acct_gather_energy_p_conf_options(s_p_options_t **full_options,
					      int *full_options_cnt)
{
//	s_p_options_t *full_options_ptr;
	s_p_options_t options[] = {
		{"EnergyIPMIAuthenticationType", S_P_UINT32},
		{"EnergyIPMICalcAdjustment", S_P_BOOLEAN},
		{"EnergyIPMICipherSuiteId", S_P_UINT32},
		{"EnergyIPMIDisableAutoProbe", S_P_UINT32},
		{"EnergyIPMIDriverAddress", S_P_UINT32},
		{"EnergyIPMIDriverDevice", S_P_STRING},
		{"EnergyIPMIDriverType", S_P_UINT32},
		{"EnergyIPMIFrequency", S_P_UINT32},
		{"EnergyIPMIPassword", S_P_STRING},
		{"EnergyIPMIPrivilegeLevel", S_P_UINT32},
		{"EnergyIPMIProtocolVersion", S_P_UINT32},
		{"EnergyIPMIRegisterSpacing", S_P_UINT32},
		{"EnergyIPMIRetransmissionTimeout", S_P_UINT32},
		{"EnergyIPMISessionTimeout", S_P_UINT32},
		{"EnergyIPMITimeout", S_P_UINT32},
		{"EnergyIPMIUsername", S_P_STRING},
		{"EnergyIPMIWorkaroundFlags", S_P_UINT32},
		{"EnergyXCCFake", S_P_BOOLEAN},
		{NULL} };

	transfer_s_p_options(full_options, options, full_options_cnt);
}

extern void acct_gather_energy_p_conf_set(int context_id_in, s_p_hashtbl_t *tbl)
{
	bool tmp_bool;

	/* Set initial values */
	_reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	if (tbl) {
		/* ipmi initialization parameters */
		s_p_get_uint32(&slurm_ipmi_conf.authentication_type,
			       "EnergyIPMIAuthenticationType", tbl);
		(void) s_p_get_boolean(&(slurm_ipmi_conf.adjustment),
				       "EnergyIPMICalcAdjustment", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.cipher_suite_id,
			       "EnergyIPMICipherSuiteId", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.disable_auto_probe,
			       "EnergyIPMIDisableAutoProbe", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_address,
			       "EnergyIPMIDriverAddress", tbl);
		s_p_get_string(&slurm_ipmi_conf.driver_device,
			       "EnergyIPMIDriverDevice", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_type,
			       "EnergyIPMIDriverType", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.freq,
			       "EnergyIPMIFrequency", tbl);
		if ((int)slurm_ipmi_conf.freq <= 0)
			fatal("EnergyIPMIFrequency must be a positive integer in acct_gather.conf.");
		s_p_get_string(&slurm_ipmi_conf.password,
			       "EnergyIPMIPassword", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.privilege_level,
			       "EnergyIPMIPrivilegeLevel", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.protocol_version,
			       "EnergyIPMIProtocolVersion", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.register_spacing,
			       "EnergyIPMIRegisterSpacing", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.retransmission_timeout,
			       "EnergyIPMIRetransmissionTimeout", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.session_timeout,
			       "EnergyIPMISessionTimeout", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.timeout,
			       "EnergyIPMITimeout", tbl);
		s_p_get_string(&slurm_ipmi_conf.username,
			       "EnergyIPMIUsername", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.workaround_flags,
			       "EnergyIPMIWorkaroundFlags", tbl);
		(void) s_p_get_boolean(&tmp_bool, "EnergyXCCFake", tbl);
		if (tmp_bool) {
			slurm_ipmi_conf.flags |= XCC_FLAG_FAKE;
			/*
			 * This is just to do a random query and get error if
			 * ipmi is not initialized
			 */
			cmd_rq[0] = 0x00;
			cmd_rq[1] = 0x04;
			cmd_rq[2] = 0x2d;
			cmd_rq[3] = 0x36;
			cmd_rq_len = 4;
		}
	}

	context_id = context_id_in;

	if (!running_in_slurmd_stepd())
		return;

	if (!flag_init) {
		flag_init = true;
		memset(&xcc_energy, 0, sizeof(acct_gather_energy_t));
		if (running_in_slurmd()) {
			slurm_thread_create(&thread_ipmi_id_launcher,
					    _thread_launcher, NULL);
			log_flag(ENERGY, "thread launched");
		} else
			_get_joules_task(0);
	}

	verbose("%s loaded", plugin_name);
}

extern void acct_gather_energy_p_conf_values(list_t **data)
{
	xassert(*data);

	add_key_pair(*data, "EnergyIPMIAuthenticationType", "%u",
		     slurm_ipmi_conf.authentication_type);

	add_key_pair_bool(*data, "EnergyIPMICalcAdjustment",
			  slurm_ipmi_conf.adjustment);

	add_key_pair(*data, "EnergyIPMICipherSuiteId", "%u",
		     slurm_ipmi_conf.cipher_suite_id);

	add_key_pair(*data, "EnergyIPMIDisableAutoProbe", "%u",
		     slurm_ipmi_conf.disable_auto_probe);

	add_key_pair(*data, "EnergyIPMIDriverAddress", "%u",
		     slurm_ipmi_conf.driver_address);

	add_key_pair(*data, "EnergyIPMIDriverDevice", "%s",
		     slurm_ipmi_conf.driver_device);

	add_key_pair(*data, "EnergyIPMIDriverType", "%u",
		     slurm_ipmi_conf.driver_type);

	add_key_pair(*data, "EnergyIPMIFrequency", "%u",
		     slurm_ipmi_conf.freq);

	/*
	 * Don't give out the password
	 * add_key_pair(*data, "EnergyIPMIPassword", "%s",
			slurm_ipmi_conf.password);
	 */

	add_key_pair(*data, "EnergyIPMIPrivilegeLevel", "%u",
		     slurm_ipmi_conf.privilege_level);

	add_key_pair(*data, "EnergyIPMIProtocolVersion", "%u",
		     slurm_ipmi_conf.protocol_version);

	add_key_pair(*data, "EnergyIPMIRegisterSpacing", "%u",
		     slurm_ipmi_conf.register_spacing);

	add_key_pair(*data, "EnergyIPMIRetransmissionTimeout", "%u",
		     slurm_ipmi_conf.retransmission_timeout);

	add_key_pair(*data, "EnergyIPMISessionTimeout", "%u",
		     slurm_ipmi_conf.session_timeout);

	add_key_pair(*data, "EnergyIPMITimeout", "%u",
		     slurm_ipmi_conf.timeout);

	add_key_pair(*data, "EnergyIPMIUsername", "%s",
		     slurm_ipmi_conf.username);

	add_key_pair(*data, "EnergyIPMIWorkaroundFlags", "%u",
		     slurm_ipmi_conf.workaround_flags);
}
