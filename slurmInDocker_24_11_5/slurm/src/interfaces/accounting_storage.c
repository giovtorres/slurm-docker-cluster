/*****************************************************************************\
 *  slurm_accounting_storage.c - account storage plugin wrapper.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
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

#define _GNU_SOURCE

#include <pthread.h>
#include <string.h>

#include "src/common/list.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/interfaces/select.h"
#include "src/interfaces/accounting_storage.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

uid_t db_api_uid = -1;
/*
 * Local data
 */

typedef struct slurm_acct_storage_ops {
	void *(*get_conn)          (int conn_num, uint16_t *persist_conn_flags,
				    bool rollback, char *cluster_name);
	int  (*close_conn)         (void **db_conn);
	int  (*commit)             (void *db_conn, bool commit);
	int  (*add_users)          (void *db_conn, uint32_t uid,
				    list_t *user_list);
	char *(*add_users_cond)    (void *db_conn, uint32_t uid,
				    slurmdb_add_assoc_cond_t *add_assoc,
				    slurmdb_user_rec_t *user);
	int  (*add_coord)          (void *db_conn, uint32_t uid,
				    list_t *acct_list,
				    slurmdb_user_cond_t *user_cond);
	int  (*add_accts)          (void *db_conn, uint32_t uid,
				    list_t *acct_list);
	char *(*add_accts_cond)    (void *db_conn, uint32_t uid,
				    slurmdb_add_assoc_cond_t *add_assoc,
				    slurmdb_account_rec_t *acct);
	int  (*add_clusters)       (void *db_conn, uint32_t uid,
				    list_t *cluster_list);
	int  (*add_federations)    (void *db_conn, uint32_t uid,
				    list_t *federation_list);
	int  (*add_tres)           (void *db_conn, uint32_t uid,
				    list_t *tres_list_in);
	int  (*add_assocs)         (void *db_conn, uint32_t uid,
				    list_t *assoc_list);
	int  (*add_qos)            (void *db_conn, uint32_t uid,
				    list_t *qos_list);
	int  (*add_res)            (void *db_conn, uint32_t uid,
				    list_t *res_list);
	int  (*add_wckeys)         (void *db_conn, uint32_t uid,
				    list_t *wckey_list);
	int  (*add_reservation)    (void *db_conn,
				    slurmdb_reservation_rec_t *resv);
	list_t *(*modify_users)    (void *db_conn, uint32_t uid,
				    slurmdb_user_cond_t *user_cond,
				    slurmdb_user_rec_t *user);
	list_t *(*modify_accts)    (void *db_conn, uint32_t uid,
				    slurmdb_account_cond_t *acct_cond,
				    slurmdb_account_rec_t *acct);
	list_t *(*modify_clusters) (void *db_conn, uint32_t uid,
				    slurmdb_cluster_cond_t *cluster_cond,
				    slurmdb_cluster_rec_t *cluster);
	list_t *(*modify_assocs)   (void *db_conn, uint32_t uid,
				    slurmdb_assoc_cond_t *assoc_cond,
				    slurmdb_assoc_rec_t *assoc);
	list_t *(*modify_federations) (void *db_conn, uint32_t uid,
				    slurmdb_federation_cond_t *fed_cond,
				    slurmdb_federation_rec_t *fed);
	list_t *(*modify_job)      (void *db_conn, uint32_t uid,
				    slurmdb_job_cond_t *job_cond,
				    slurmdb_job_rec_t *job);
	list_t *(*modify_qos)      (void *db_conn, uint32_t uid,
				    slurmdb_qos_cond_t *qos_cond,
				    slurmdb_qos_rec_t *qos);
	list_t *(*modify_res)      (void *db_conn, uint32_t uid,
				    slurmdb_res_cond_t *res_cond,
				    slurmdb_res_rec_t *res);
	list_t *(*modify_wckeys)   (void *db_conn, uint32_t uid,
				    slurmdb_wckey_cond_t *wckey_cond,
				    slurmdb_wckey_rec_t *wckey);
	int  (*modify_reservation) (void *db_conn,
				    slurmdb_reservation_rec_t *resv);
	list_t *(*remove_users)    (void *db_conn, uint32_t uid,
				    slurmdb_user_cond_t *user_cond);
	list_t *(*remove_coord)    (void *db_conn, uint32_t uid,
				    list_t *acct_list,
				    slurmdb_user_cond_t *user_cond);
	list_t *(*remove_accts)    (void *db_conn, uint32_t uid,
				    slurmdb_account_cond_t *acct_cond);
	list_t *(*remove_clusters) (void *db_conn, uint32_t uid,
				    slurmdb_cluster_cond_t *cluster_cond);
	list_t *(*remove_assocs)   (void *db_conn, uint32_t uid,
				    slurmdb_assoc_cond_t *assoc_cond);
	list_t *(*remove_federations) (void *db_conn, uint32_t uid,
				    slurmdb_federation_cond_t *fed_cond);
	list_t *(*remove_qos)      (void *db_conn, uint32_t uid,
				    slurmdb_qos_cond_t *qos_cond);
	list_t *(*remove_res)      (void *db_conn, uint32_t uid,
				    slurmdb_res_cond_t *res_cond);
	list_t *(*remove_wckeys)   (void *db_conn, uint32_t uid,
				    slurmdb_wckey_cond_t *wckey_cond);
	int  (*remove_reservation) (void *db_conn,
				    slurmdb_reservation_rec_t *resv);
	list_t *(*get_users)       (void *db_conn, uint32_t uid,
				    slurmdb_user_cond_t *user_cond);
	list_t *(*get_accts)       (void *db_conn, uint32_t uid,
				    slurmdb_account_cond_t *acct_cond);
	list_t *(*get_clusters)    (void *db_conn, uint32_t uid,
				    slurmdb_cluster_cond_t *cluster_cond);
	list_t *(*get_federations) (void *db_conn, uint32_t uid,
				    slurmdb_federation_cond_t *fed_cond);
	list_t *(*get_config)      (void *db_conn, char *config_name);
	list_t *(*get_tres)        (void *db_conn, uint32_t uid,
				    slurmdb_tres_cond_t *tres_cond);
	list_t *(*get_assocs)      (void *db_conn, uint32_t uid,
				    slurmdb_assoc_cond_t *assoc_cond);
	list_t *(*get_events)      (void *db_conn, uint32_t uid,
				    slurmdb_event_cond_t *event_cond);
	list_t *(*get_instances)   (void *db_conn, uint32_t uid,
				    slurmdb_instance_cond_t *instance_cond);
	list_t *(*get_problems)    (void *db_conn, uint32_t uid,
				    slurmdb_assoc_cond_t *assoc_cond);
	list_t *(*get_qos)         (void *db_conn, uint32_t uid,
				    slurmdb_qos_cond_t *qos_cond);
	list_t *(*get_res)         (void *db_conn, uint32_t uid,
				    slurmdb_res_cond_t *res_cond);
	list_t *(*get_wckeys)      (void *db_conn, uint32_t uid,
				    slurmdb_wckey_cond_t *wckey_cond);
	list_t *(*get_resvs)       (void *db_conn, uint32_t uid,
				    slurmdb_reservation_cond_t *resv_cond);
	list_t *(*get_txn)         (void *db_conn, uint32_t uid,
				    slurmdb_txn_cond_t *txn_cond);
	int  (*get_usage)          (void *db_conn, uint32_t uid,
				    void *in, int type,
				    time_t start,
				    time_t end);
	int (*roll_usage)          (void *db_conn,
				    time_t sent_start, time_t sent_end,
				    uint16_t archive_data,
				    list_t **rollup_stats_list_in);
	int  (*fix_runaway_jobs)   (void *db_conn, uint32_t uid, list_t *jobs);
	int  (*node_down)          (void *db_conn, node_record_t *node_ptr,
				    time_t event_time, char *reason,
				    uint32_t reason_uid);
	char *(*node_inx)          (void *db_conn, char *nodes);
	int  (*node_up)            (void *db_conn, node_record_t *node_ptr,
				    time_t event_time);
	int  (*node_update)	   (void *db_conn, node_record_t *node_ptr);
	int  (*cluster_tres)       (void *db_conn, char *cluster_nodes,
				    char *tres_str_in, time_t event_time,
				    uint16_t rpc_version);
	int  (*register_ctld)      (void *db_conn, uint16_t port);
	int  (*register_disconn_ctld)(void *db_conn, char *control_host);
	int  (*fini_ctld)          (void *db_conn,
				    slurmdb_cluster_rec_t *cluster_rec);
	int  (*job_start)          (void *db_conn, job_record_t *job_ptr);
	int  (*job_heavy)          (void *db_conn, job_record_t *job_ptr);
	int  (*job_complete)       (void *db_conn, job_record_t *job_ptr);
	int  (*step_start)         (void *db_conn, step_record_t *step_ptr);
	int  (*step_complete)      (void *db_conn, step_record_t *step_ptr);
	int  (*job_suspend)        (void *db_conn, job_record_t *job_ptr);
	list_t *(*get_jobs_cond)   (void *db_conn, uint32_t uid,
				    slurmdb_job_cond_t *job_cond);
	int (*archive_dump)        (void *db_conn,
				    slurmdb_archive_cond_t *arch_cond);
	int (*archive_load)        (void *db_conn,
				    slurmdb_archive_rec_t *arch_rec);
	int (*update_shares_used)  (void *db_conn,
				    list_t *shares_used);
	int (*flush_jobs)          (void *db_conn,
				    time_t event_time);
	int (*reconfig)            (void *db_conn, bool dbd);
	int (*relay_msg)           (void *db_conn, persist_msg_t *msg);
	int (*reset_lft_rgt)       (void *db_conn, uid_t uid,
				    list_t *cluster_list);
	int (*get_stats)           (void *db_conn, slurmdb_stats_rec_t **stats);
	int (*clear_stats)         (void *db_conn);
	int (*get_data)            (void *db_conn, acct_storage_info_t dinfo,
				    void *data);
	void (*send_all) (void *db_conn, time_t event_time,
			  slurm_msg_type_t msg_type);
	int (*shutdown)            (void *db_conn);
} slurm_acct_storage_ops_t;
/*
 * Must be synchronized with slurm_acct_storage_ops_t above.
 */
static const char *syms[] = {
	"acct_storage_p_get_connection",
	"acct_storage_p_close_connection",
	"acct_storage_p_commit",
	"acct_storage_p_add_users",
	"acct_storage_p_add_users_cond",
	"acct_storage_p_add_coord",
	"acct_storage_p_add_accts",
	"acct_storage_p_add_accts_cond",
	"acct_storage_p_add_clusters",
	"acct_storage_p_add_federations",
	"acct_storage_p_add_tres",
	"acct_storage_p_add_assocs",
	"acct_storage_p_add_qos",
	"acct_storage_p_add_res",
	"acct_storage_p_add_wckeys",
	"acct_storage_p_add_reservation",
	"acct_storage_p_modify_users",
	"acct_storage_p_modify_accts",
	"acct_storage_p_modify_clusters",
	"acct_storage_p_modify_assocs",
	"acct_storage_p_modify_federations",
	"acct_storage_p_modify_job",
	"acct_storage_p_modify_qos",
	"acct_storage_p_modify_res",
	"acct_storage_p_modify_wckeys",
	"acct_storage_p_modify_reservation",
	"acct_storage_p_remove_users",
	"acct_storage_p_remove_coord",
	"acct_storage_p_remove_accts",
	"acct_storage_p_remove_clusters",
	"acct_storage_p_remove_assocs",
	"acct_storage_p_remove_federations",
	"acct_storage_p_remove_qos",
	"acct_storage_p_remove_res",
	"acct_storage_p_remove_wckeys",
	"acct_storage_p_remove_reservation",
	"acct_storage_p_get_users",
	"acct_storage_p_get_accts",
	"acct_storage_p_get_clusters",
	"acct_storage_p_get_federations",
	"acct_storage_p_get_config",
	"acct_storage_p_get_tres",
	"acct_storage_p_get_assocs",
	"acct_storage_p_get_events",
	"acct_storage_p_get_instances",
	"acct_storage_p_get_problems",
	"acct_storage_p_get_qos",
	"acct_storage_p_get_res",
	"acct_storage_p_get_wckeys",
	"acct_storage_p_get_reservations",
	"acct_storage_p_get_txn",
	"acct_storage_p_get_usage",
	"acct_storage_p_roll_usage",
	"acct_storage_p_fix_runaway_jobs",
	"clusteracct_storage_p_node_down",
	"acct_storage_p_node_inx",
	"clusteracct_storage_p_node_up",
	"clusteracct_storage_p_node_update",
	"clusteracct_storage_p_cluster_tres",
	"clusteracct_storage_p_register_ctld",
	"clusteracct_storage_p_register_disconn_ctld",
	"clusteracct_storage_p_fini_ctld",
	"jobacct_storage_p_job_start",
	"jobacct_storage_p_job_heavy",
	"jobacct_storage_p_job_complete",
	"jobacct_storage_p_step_start",
	"jobacct_storage_p_step_complete",
	"jobacct_storage_p_suspend",
	"jobacct_storage_p_get_jobs_cond",
	"jobacct_storage_p_archive",
	"jobacct_storage_p_archive_load",
	"acct_storage_p_update_shares_used",
	"acct_storage_p_flush_jobs_on_cluster",
	"acct_storage_p_reconfig",
	"acct_storage_p_relay_msg",
	"acct_storage_p_reset_lft_rgt",
	"acct_storage_p_get_stats",
	"acct_storage_p_clear_stats",
	"acct_storage_p_get_data",
	"acct_storage_p_send_all",
	"acct_storage_p_shutdown",
};

static slurm_acct_storage_ops_t ops;
static plugin_context_t *plugin_context = NULL;
static pthread_rwlock_t plugin_context_lock = PTHREAD_RWLOCK_INITIALIZER;
static plugin_init_t plugin_inited = PLUGIN_NOT_INITED;

static uint32_t max_step_records = NO_VAL;

/*
 * Initialize context for acct_storage plugin
 */
extern int acct_storage_g_init(void)
{
	int retval = SLURM_SUCCESS;
	char *plugin_type = "accounting_storage";
	char *tmp_ptr = NULL;

	slurm_rwlock_wrlock(&plugin_context_lock);

	if (plugin_inited)
		goto done;

	if (!slurm_conf.accounting_storage_type) {
		plugin_inited = PLUGIN_NOOP;
		goto done;
	}

	plugin_context = plugin_context_create(
		plugin_type, slurm_conf.accounting_storage_type, (void **)&ops,
		syms, sizeof(syms));

	if (!plugin_context) {
		error("cannot create %s context for %s",
		      plugin_type, slurm_conf.accounting_storage_type);
		retval = SLURM_ERROR;
		plugin_inited = PLUGIN_NOT_INITED;
		goto done;
	}
	plugin_inited = PLUGIN_INITED;

	if ((tmp_ptr = xstrcasestr(slurm_conf.accounting_storage_params,
				   "max_step_records="))) {
		max_step_records = strtol(tmp_ptr + strlen("max_step_records="),
					  NULL, 10);
	}

done:
	slurm_rwlock_unlock(&plugin_context_lock);
	return retval;
}

extern int acct_storage_g_fini(void)
{
	int rc = SLURM_SUCCESS;

	slurm_rwlock_wrlock(&plugin_context_lock);
	if (plugin_context) {
		rc = plugin_context_destroy(plugin_context);
		plugin_context = NULL;
	}
	plugin_inited = PLUGIN_NOT_INITED;
	slurm_rwlock_unlock(&plugin_context_lock);
	return rc;
}

extern void *acct_storage_g_get_connection(
	int conn_num, uint16_t *persist_conn_flags,
	bool rollback,char *cluster_name)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_conn))(conn_num, persist_conn_flags,
				 rollback, cluster_name);
}

extern int acct_storage_g_close_connection(void **db_conn)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.close_conn))(db_conn);
}

extern int acct_storage_g_commit(void *db_conn, bool commit)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.commit))(db_conn, commit);
}

extern int acct_storage_g_add_users(void *db_conn, uint32_t uid,
				    list_t *user_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_users))(db_conn, uid, user_list);
}

extern char *acct_storage_g_add_users_cond(
	void *db_conn, uint32_t uid,
	slurmdb_add_assoc_cond_t *add_assoc,
	slurmdb_user_rec_t *user)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.add_users_cond))(db_conn, uid, add_assoc, user);
}

extern int acct_storage_g_add_coord(void *db_conn, uint32_t uid,
				    list_t *acct_list,
				    slurmdb_user_cond_t *user_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_coord))(db_conn, uid, acct_list, user_cond);
}

extern int acct_storage_g_add_accounts(void *db_conn, uint32_t uid,
				       list_t *acct_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_accts))(db_conn, uid, acct_list);
}

extern char *acct_storage_g_add_accounts_cond(
	void *db_conn, uint32_t uid,
	slurmdb_add_assoc_cond_t *add_assoc,
	slurmdb_account_rec_t *acct)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.add_accts_cond))(db_conn, uid, add_assoc, acct);
}

extern int acct_storage_g_add_clusters(void *db_conn, uint32_t uid,
				       list_t *cluster_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_clusters))(db_conn, uid, cluster_list);
}

extern int acct_storage_g_add_federations(void *db_conn, uint32_t uid,
					  list_t *federation_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_federations))(db_conn, uid, federation_list);
}

extern int acct_storage_g_add_tres(void *db_conn, uint32_t uid,
				   list_t *tres_list_in)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_tres))(db_conn, uid, tres_list_in);
}

extern int acct_storage_g_add_assocs(void *db_conn, uint32_t uid,
				     list_t *assoc_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_assocs))(db_conn, uid, assoc_list);
}

extern int acct_storage_g_add_qos(void *db_conn, uint32_t uid,
				  list_t *qos_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_qos))(db_conn, uid, qos_list);
}

extern int acct_storage_g_add_res(void *db_conn, uint32_t uid,
				  list_t *res_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_res))(db_conn, uid, res_list);
}
extern int acct_storage_g_add_wckeys(void *db_conn, uint32_t uid,
				     list_t *wckey_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_wckeys))(db_conn, uid, wckey_list);
}

extern int acct_storage_g_add_reservation(void *db_conn,
					  slurmdb_reservation_rec_t *resv)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.add_reservation))(db_conn, resv);
}

extern list_t *acct_storage_g_modify_users(void *db_conn, uint32_t uid,
					   slurmdb_user_cond_t *user_cond,
					   slurmdb_user_rec_t *user)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_users))(db_conn, uid, user_cond, user);
}

extern list_t *acct_storage_g_modify_accounts(void *db_conn, uint32_t uid,
					      slurmdb_account_cond_t *acct_cond,
					      slurmdb_account_rec_t *acct)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_accts))(db_conn, uid, acct_cond, acct);
}

extern list_t *acct_storage_g_modify_clusters(
	void *db_conn, uint32_t uid, slurmdb_cluster_cond_t *cluster_cond,
	slurmdb_cluster_rec_t *cluster)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_clusters))(db_conn, uid, cluster_cond, cluster);
}

extern list_t *acct_storage_g_modify_assocs(
	void *db_conn, uint32_t uid,
	slurmdb_assoc_cond_t *assoc_cond,
	slurmdb_assoc_rec_t *assoc)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_assocs))(db_conn, uid, assoc_cond, assoc);
}

extern list_t *acct_storage_g_modify_federations(
				void *db_conn, uint32_t uid,
				slurmdb_federation_cond_t *fed_cond,
				slurmdb_federation_rec_t *fed)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_federations))(db_conn, uid, fed_cond, fed);
}

extern list_t *acct_storage_g_modify_job(void *db_conn, uint32_t uid,
					 slurmdb_job_cond_t *job_cond,
					 slurmdb_job_rec_t *job)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;


	return (*(ops.modify_job))(db_conn, uid, job_cond, job);
}

extern list_t *acct_storage_g_modify_qos(void *db_conn, uint32_t uid,
					 slurmdb_qos_cond_t *qos_cond,
					 slurmdb_qos_rec_t *qos)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_qos))(db_conn, uid, qos_cond, qos);
}

extern list_t *acct_storage_g_modify_res(void *db_conn, uint32_t uid,
					 slurmdb_res_cond_t *res_cond,
					 slurmdb_res_rec_t *res)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_res))(db_conn, uid, res_cond, res);
}

extern list_t *acct_storage_g_modify_wckeys(void *db_conn, uint32_t uid,
					    slurmdb_wckey_cond_t *wckey_cond,
					    slurmdb_wckey_rec_t *wckey)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.modify_wckeys))(db_conn, uid, wckey_cond, wckey);
}

extern int acct_storage_g_modify_reservation(void *db_conn,
					     slurmdb_reservation_rec_t *resv)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.modify_reservation))(db_conn, resv);
}

extern list_t *acct_storage_g_remove_users(void *db_conn, uint32_t uid,
					   slurmdb_user_cond_t *user_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_users))(db_conn, uid, user_cond);
}

extern list_t *acct_storage_g_remove_coord(void *db_conn, uint32_t uid,
					   list_t *acct_list,
					   slurmdb_user_cond_t *user_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_coord))(db_conn, uid, acct_list, user_cond);
}

extern list_t *acct_storage_g_remove_accounts(void *db_conn, uint32_t uid,
					      slurmdb_account_cond_t *acct_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_accts))(db_conn, uid, acct_cond);
}

extern list_t *acct_storage_g_remove_clusters(
	void *db_conn, uint32_t uid, slurmdb_cluster_cond_t *cluster_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_clusters))(db_conn, uid, cluster_cond);
}

extern list_t *acct_storage_g_remove_assocs(
	void *db_conn, uint32_t uid,
	slurmdb_assoc_cond_t *assoc_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_assocs))(db_conn, uid, assoc_cond);
}

extern list_t *acct_storage_g_remove_federations(
					void *db_conn, uint32_t uid,
					slurmdb_federation_cond_t *fed_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_federations))(db_conn, uid, fed_cond);
}

extern list_t *acct_storage_g_remove_qos(void *db_conn, uint32_t uid,
					 slurmdb_qos_cond_t *qos_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_qos))(db_conn, uid, qos_cond);
}

extern list_t *acct_storage_g_remove_res(void *db_conn, uint32_t uid,
					 slurmdb_res_cond_t *res_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_res))(db_conn, uid, res_cond);
}

extern list_t *acct_storage_g_remove_wckeys(void *db_conn, uint32_t uid,
					    slurmdb_wckey_cond_t *wckey_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.remove_wckeys))(db_conn, uid, wckey_cond);
}

extern int acct_storage_g_remove_reservation(void *db_conn,
					     slurmdb_reservation_rec_t *resv)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.remove_reservation))(db_conn, resv);
}

extern list_t *acct_storage_g_get_users(
	void *db_conn, uint32_t uid, slurmdb_user_cond_t *user_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_users))(db_conn, uid, user_cond);
}

extern list_t *acct_storage_g_get_accounts(void *db_conn, uint32_t uid,
					   slurmdb_account_cond_t *acct_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_accts))(db_conn, uid, acct_cond);
}

extern list_t *acct_storage_g_get_clusters(void *db_conn, uint32_t uid,
					   slurmdb_cluster_cond_t *cluster_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_clusters))(db_conn, uid, cluster_cond);
}

extern list_t *acct_storage_g_get_federations(void *db_conn, uint32_t uid,
					      slurmdb_federation_cond_t *fed_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_federations))(db_conn, uid, fed_cond);
}

extern list_t *acct_storage_g_get_config(void *db_conn, char *config_name)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_config))(db_conn, config_name);
}

extern list_t *acct_storage_g_get_tres(
	void *db_conn, uint32_t uid,
	slurmdb_tres_cond_t *tres_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_tres))(db_conn, uid, tres_cond);
}

extern list_t *acct_storage_g_get_assocs(
	void *db_conn, uint32_t uid,
	slurmdb_assoc_cond_t *assoc_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_assocs))(db_conn, uid, assoc_cond);
}

extern list_t *acct_storage_g_get_events(void *db_conn, uint32_t uid,
					 slurmdb_event_cond_t *event_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_events))(db_conn, uid, event_cond);
}

extern list_t *acct_storage_g_get_instances(
	void *db_conn, uint32_t uid, slurmdb_instance_cond_t *instance_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_instances))(db_conn, uid, instance_cond);
}

extern list_t *acct_storage_g_get_problems(void *db_conn, uint32_t uid,
					   slurmdb_assoc_cond_t *assoc_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_problems))(db_conn, uid, assoc_cond);
}

extern list_t *acct_storage_g_get_qos(void *db_conn, uint32_t uid,
				      slurmdb_qos_cond_t *qos_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_qos))(db_conn, uid, qos_cond);
}

extern list_t *acct_storage_g_get_res(void *db_conn, uint32_t uid,
				      slurmdb_res_cond_t *res_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_res))(db_conn, uid, res_cond);
}

extern list_t *acct_storage_g_get_wckeys(void *db_conn, uint32_t uid,
					 slurmdb_wckey_cond_t *wckey_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_wckeys))(db_conn, uid, wckey_cond);
}

extern list_t *acct_storage_g_get_reservations(
	void *db_conn, uint32_t uid, slurmdb_reservation_cond_t *resv_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.get_resvs))(db_conn, uid, resv_cond);
}

extern list_t *acct_storage_g_get_txn(void *db_conn, uint32_t uid,
				      slurmdb_txn_cond_t *txn_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.get_txn))(db_conn, uid, txn_cond);
}

extern int acct_storage_g_get_usage(void *db_conn,  uint32_t uid,
				    void *in, int type,
				    time_t start, time_t end)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.get_usage))(db_conn, uid, in, type, start, end);
}

extern int acct_storage_g_roll_usage(void *db_conn,
				     time_t sent_start, time_t sent_end,
				     uint16_t archive_data,
				     list_t **rollup_stats_list_in)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.roll_usage))(db_conn, sent_start, sent_end, archive_data,
				   rollup_stats_list_in);
}

extern int acct_storage_g_fix_runaway_jobs(void *db_conn,
					   uint32_t uid, list_t *jobs)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.fix_runaway_jobs))(db_conn, uid, jobs);
}

extern int clusteracct_storage_g_node_down(void *db_conn,
					   node_record_t *node_ptr,
					   time_t event_time,
					   char *reason, uint32_t reason_uid)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.node_down))(db_conn, node_ptr, event_time,
				  reason, reason_uid);
}

extern char *acct_storage_g_node_inx(void *db_conn, char *nodes)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return NULL;

	return (*(ops.node_inx))(db_conn, nodes);
}

extern int clusteracct_storage_g_node_up(void *db_conn,
					 node_record_t *node_ptr,
					 time_t event_time)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;


	xfree(node_ptr->reason);
	node_ptr->reason_time = 0;
	node_ptr->reason_uid = NO_VAL;

	return (*(ops.node_up))(db_conn, node_ptr, event_time);
}

extern int clusteracct_storage_g_node_update(void *db_conn,
					     node_record_t *node_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.node_update))(db_conn, node_ptr);
}

extern int clusteracct_storage_g_cluster_tres(void *db_conn,
					      char *cluster_nodes,
					      char *tres_str_in,
					      time_t event_time,
					      uint16_t rpc_version)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.cluster_tres))(db_conn, cluster_nodes,
				     tres_str_in, event_time, rpc_version);
}

extern int clusteracct_storage_g_register_ctld(void *db_conn, uint16_t port)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.register_ctld))(db_conn, port);
}

extern int clusteracct_storage_g_register_disconn_ctld(
	void *db_conn, char *control_host)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.register_disconn_ctld))(db_conn, control_host);
}

extern int clusteracct_storage_g_fini_ctld(void *db_conn,
					   slurmdb_cluster_rec_t *cluster_rec)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.fini_ctld))(db_conn, cluster_rec);
}

/*
 * load into the storage information about a job,
 * typically when it begins execution, but possibly earlier
 */
extern int jobacct_storage_g_job_start(void *db_conn,
				       job_record_t *job_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_JOBS)
		return SLURM_SUCCESS;

	/* A pending job's start_time is it's expected initiation time
	 * (changed in slurm v2.1). Rather than changing a bunch of code
	 * in the accounting_storage plugins and SlurmDBD, just clear
	 * start_time before accounting and restore it later.
	 * If an update for a job that is being requeued[hold] happens,
	 * we don't want to modify the start_time of the old record.
	 * Pending + Completing is equivalent to Requeue.
	 */
	if (IS_JOB_PENDING(job_ptr) && !IS_JOB_COMPLETING(job_ptr)) {
		int rc;
		time_t orig_start_time = job_ptr->start_time;
		job_ptr->start_time = (time_t) 0;
		rc = (*(ops.job_start))(db_conn, job_ptr);
		job_ptr->start_time = orig_start_time;
		return rc;
	}

	return (*(ops.job_start))(db_conn, job_ptr);
}

/*
 * load into the storage heavy information of a job
 */
extern int jobacct_storage_g_job_heavy(void *db_conn, job_record_t *job_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_JOBS)
		return SLURM_SUCCESS;
	return (*(ops.job_heavy))(db_conn, job_ptr);
}

/*
 * load into the storage the end of a job
 */
extern int jobacct_storage_g_job_complete(void *db_conn,
					  job_record_t *job_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_JOBS)
		return SLURM_SUCCESS;
	return (*(ops.job_complete))(db_conn, job_ptr);
}

/*
 * load into the storage the start of a job step
 */
extern int jobacct_storage_g_step_start(void *db_conn, step_record_t *step_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_STEPS)
		return SLURM_SUCCESS;
	if ((max_step_records != NO_VAL) &&
	    (step_ptr->step_id.step_id < SLURM_MAX_NORMAL_STEP_ID) &&
	    (step_ptr->step_id.step_id >= max_step_records))
		return SLURM_SUCCESS;
	return (*(ops.step_start))(db_conn, step_ptr);
}

/*
 * load into the storage the end of a job step
 */
extern int jobacct_storage_g_step_complete(void *db_conn,
					   step_record_t *step_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_STEPS)
		return SLURM_SUCCESS;
	if ((max_step_records != NO_VAL) &&
	    (step_ptr->step_id.step_id < SLURM_MAX_NORMAL_STEP_ID) &&
	    (step_ptr->step_id.step_id >= max_step_records))
		return SLURM_SUCCESS;
	return (*(ops.step_complete))(db_conn, step_ptr);
}

/*
 * load into the storage a suspension of a job
 */
extern int jobacct_storage_g_job_suspend(void *db_conn,
					 job_record_t *job_ptr)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	if (slurm_conf.accounting_storage_enforce & ACCOUNTING_ENFORCE_NO_JOBS)
		return SLURM_SUCCESS;
	return (*(ops.job_suspend))(db_conn, job_ptr);
}

static int _sort_desc_submit_time(void *x, void *y)
{
	slurmdb_job_rec_t *j1 = *(slurmdb_job_rec_t **)x;
	slurmdb_job_rec_t *j2 = *(slurmdb_job_rec_t **)y;

	if (j1->submit < j2->submit)
		return -1;
	else if (j1->submit > j2->submit)
		return 1;

	return 0;
}

/*
 * get info from the storage
 * returns List of job_rec_t *
 * note List needs to be freed when called
 */
extern list_t *jobacct_storage_g_get_jobs_cond(void *db_conn, uint32_t uid,
					       slurmdb_job_cond_t *job_cond)
{
	list_t *ret_list;

	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	ret_list = (*(ops.get_jobs_cond))(db_conn, uid, job_cond);

	/* If multiple clusters were requested, the jobs are grouped together by
	 * cluster -- each group sorted by submit time. Sort all the jobs by
	 * submit time */
	if (ret_list && job_cond && job_cond->cluster_list &&
	    (list_count(job_cond->cluster_list) > 1))
		list_sort(ret_list, (ListCmpF)_sort_desc_submit_time);

	return ret_list;
}

/*
 * expire old info from the storage
 */
extern int jobacct_storage_g_archive(void *db_conn,
				     slurmdb_archive_cond_t *arch_cond)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.archive_dump))(db_conn, arch_cond);
}

/*
 * load expired info into the storage
 */
extern int jobacct_storage_g_archive_load(void *db_conn,
					  slurmdb_archive_rec_t *arch_rec)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.archive_load))(db_conn, arch_rec);
}

/*
 * record shares used information for backup in case slurmctld restarts
 * IN:  account_list List of shares_used_object_t *
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_update_shares_used(void *db_conn, list_t *acct_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.update_shares_used))(db_conn, acct_list);
}

/*
 * This should be called when a cluster does a cold start to flush out
 * any jobs that were running during the restart so we don't have any
 * jobs in the database "running" forever since no endtime will be
 * placed in there other wise.
 * IN:  char * = cluster name
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_flush_jobs_on_cluster(
	void *db_conn, time_t event_time)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.flush_jobs))(db_conn, event_time);

}

/*
 * When a reconfigure happens this should be called.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_reconfig(void *db_conn, bool dbd)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.reconfig))(db_conn, dbd);

}

/*
 * Reset the lft and rights of an association table.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_reset_lft_rgt(void *db_conn, uid_t uid,
					list_t *cluster_list)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.reset_lft_rgt))(db_conn, uid, cluster_list);

}

/*
 * Get performance statistics.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_get_stats(void *db_conn, slurmdb_stats_rec_t **stats)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.get_stats))(db_conn, stats);
}

/*
 * Clear performance statistics.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_clear_stats(void *db_conn)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.clear_stats))(db_conn);
}

/*
 * Get generic data.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_get_data(void *db_conn, acct_storage_info_t dinfo,
				    void *data)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.get_data))(db_conn, dinfo, data);
}

/*
 * Send all relavant information to the DBD.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern void acct_storage_g_send_all(void *db_conn, time_t event_time,
				    slurm_msg_type_t msg_type)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return;

	(*(ops.send_all))(db_conn, event_time, msg_type);
}

/*
 * Shutdown database server.
 * RET: SLURM_SUCCESS on success SLURM_ERROR else
 */
extern int acct_storage_g_shutdown(void *db_conn)
{
	xassert(plugin_inited != PLUGIN_NOT_INITED);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.shutdown))(db_conn);
}

extern int acct_storage_g_relay_msg(void *db_conn, persist_msg_t *msg)
{
	xassert(plugin_inited);

	if (plugin_inited == PLUGIN_NOOP)
		return SLURM_SUCCESS;

	return (*(ops.relay_msg))(db_conn, msg);
}
