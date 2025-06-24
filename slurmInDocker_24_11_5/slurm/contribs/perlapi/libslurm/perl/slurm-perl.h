/*
 * slurm-perl.h - prototypes of msg-hv converting functions
 */

#ifndef _SLURM_PERL_H
#define _SLURM_PERL_H

#include <msg.h>

#undef VERSION /* avoid conflicting defintion in config.h */
#include "src/common/slurm_xlator.h"
#include "src/common/job_state_reason.h"
#include "src/common/list.h"
#include "src/common/xmalloc.h"
#include "src/common/slurm_protocol_defs.h"

extern void slurm_api_clear_config(void);

/********** resource allocation related conversion functions **********/
extern int hv_to_job_desc_msg(HV *hv, job_desc_msg_t *job_desc);
extern void free_job_desc_msg_memory(job_desc_msg_t *msg);
extern int resource_allocation_response_msg_to_hv(
    resource_allocation_response_msg_t *resp_msg, HV *hv);
extern int submit_response_msg_to_hv(submit_response_msg_t *resp_msg, HV *hv);
extern int job_sbcast_cred_msg_to_hv(job_sbcast_cred_msg_t *msg, HV *hv);
extern int srun_job_complete_msg_to_hv(srun_job_complete_msg_t *msg, HV *hv);
extern int srun_timeout_msg_to_hv(srun_timeout_msg_t *msg, HV *hv);

/********** resource allocation callback functions **********/
extern void set_sarb_cb(SV *callback);
extern void sarb_cb(uint32_t job_id);
extern void set_sacb(HV *callbacks);
extern slurm_allocation_callbacks_t sacb;

/********** job info conversion functions **********/
extern int job_info_to_hv(job_info_t *job_info, HV *hv);
extern int job_info_msg_to_hv(job_info_msg_t *job_info_msg, HV *hv);
extern int hv_to_job_info(HV *hv, job_info_t *job_info);
extern int hv_to_job_info_msg(HV *hv, job_info_msg_t *job_info_msg);

/********** step info conversion functions **********/
extern int job_step_info_to_hv(job_step_info_t *step_info, HV *hv);
extern int hv_to_job_step_info(HV *hv, job_step_info_t *step_info);
extern int job_step_info_response_msg_to_hv(job_step_info_response_msg_t
					    *job_step_info_msg, HV *hv);
extern int hv_to_job_step_info_response_msg(HV *hv,
		job_step_info_response_msg_t *job_step_info_msg);
extern int slurm_step_layout_to_hv(slurm_step_layout_t *step_layout, HV *hv);
extern int job_step_pids_to_hv(job_step_pids_t *pids, HV *hv);
extern int job_step_pids_response_msg_to_hv(job_step_pids_response_msg_t
		*pids_msg, HV *hv);
extern int job_step_stat_to_hv(job_step_stat_t *stat, HV *hv);
extern int job_step_stat_response_msg_to_hv(job_step_stat_response_msg_t
		*stat_msg, HV *hv);

/********** node info conversion functions **********/
extern int node_info_to_hv(node_info_t *node_info, HV *hv);
extern int hv_to_node_info(HV *hv, node_info_t *node_info);
extern int node_info_msg_to_hv(node_info_msg_t *node_info_msg, HV *hv);
extern int hv_to_node_info_msg(HV *hv, node_info_msg_t *node_info_msg);
extern int hv_to_update_node_msg(HV *hv, update_node_msg_t *update_msg);

/********** partition info conversion functions **********/
extern int partition_info_to_hv(partition_info_t *part_info, HV *hv);
extern int hv_to_partition_info(HV *hv, partition_info_t *part_info);
extern int partition_info_msg_to_hv(partition_info_msg_t *part_info_msg, HV *hv);
extern int hv_to_partition_info_msg(HV *hv, partition_info_msg_t *part_info_msg);
extern int hv_to_update_part_msg(HV *hv, update_part_msg_t *part_msg);
extern int hv_to_delete_part_msg(HV *hv, delete_part_msg_t *delete_msg);

/********** ctl config conversion functions **********/
extern int slurm_ctl_conf_to_hv(slurm_conf_t *conf, HV *hv);
extern int hv_to_slurm_ctl_conf(HV *hv, slurm_conf_t *conf);
extern int slurmd_status_to_hv(slurmd_status_t *status, HV *hv);
extern int hv_to_slurmd_status(HV *hv, slurmd_status_t *status);
extern int hv_to_step_update_request_msg(HV *hv, step_update_request_msg_t *update_msg);

/********** reservation info conversion functions **********/
extern int reserve_info_to_hv(reserve_info_t *reserve_info, HV *hv);
extern int hv_to_reserve_info(HV *hv, reserve_info_t *resv_info);
extern int reserve_info_msg_to_hv(reserve_info_msg_t *resv_info_msg, HV *hv);
extern int hv_to_reserve_info_msg(HV *hv, reserve_info_msg_t *resv_info_msg);
extern int hv_to_update_reservation_msg(HV *hv, resv_desc_msg_t *resv_msg);
extern int hv_to_delete_reservation_msg(HV *hv, reservation_name_msg_t *resv_name);

/********* trigger info conversion functions **********/
extern int trigger_info_to_hv(trigger_info_t *info, HV *hv);
extern int hv_to_trigger_info(HV *hv, trigger_info_t *info);
extern int trigger_info_msg_to_hv(trigger_info_msg_t *msg, HV *hv);

/********** topo info conversion functions **********/
extern int topo_info_to_hv(topo_info_t *topo_info, HV *hv);
extern int hv_to_topo_info(HV *hv, topo_info_t *topo_info);
extern int topo_info_response_msg_to_hv(topo_info_response_msg_t *topo_info_msg, HV *hv);
extern int hv_to_topo_info_response_msg(HV *hv, topo_info_response_msg_t *topo_info_msg);

/********** step launching functions **********/
extern int hv_to_slurm_step_launch_params(HV *hv, slurm_step_launch_params_t
					  *params);
extern void free_slurm_step_launch_params_memory(slurm_step_launch_params_t
						 *params);
/********** step launching callback functions **********/
extern void set_slcb(HV *callbacks);
extern slurm_step_launch_callbacks_t slcb;

#endif /* _SLURM_PERL_H */
