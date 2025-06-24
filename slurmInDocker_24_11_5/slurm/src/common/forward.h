/*****************************************************************************\
 *  forward.h - get/print the job state information of slurm
 *****************************************************************************
 *  Copyright (C) 2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <auble1@llnl.gov>
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

#ifndef _FORWARD_H
#define _FORWARD_H

#include <stdint.h>
#include "src/common/slurm_protocol_api.h"

/*
 * forward_init    - initialize forward structure
 * IN: forward     - forward_t *   - struct to store forward info
 * RET: VOID
 */
extern void forward_init(forward_t *forward);

/*
 * forward_msg	      - logic to forward a message which has been received and
 *			accumulate the return codes from processes getting the
 *			forwarded message
 *
 * IN: forward_struct - forward_struct_t *   - holds information about message
 *                                             that needs to be forwarded to
 *      				       children processes
 * IN: header         - header_t             - header from message that came in
 *                                             needing to be forwarded.
 * RET: SLURM_SUCCESS - int
 */
/*********************************************************************
// Code taken from common/slurm_protocol_api.c
// Set up the forward_struct using the remainder of the buffer being received,
// right after header has been removed form the original buffer

forward_struct = xmalloc(sizeof(forward_struct_t));
forward_struct->buf_len = remaining_buf(buffer);
forward_struct->buf = xmalloc(forward_struct->buf_len);
memcpy(forward_struct->buf, &buffer->head[buffer->processed],
       forward_struct->buf_len);
forward_struct->ret_list = ret_list;

forward_struct->timeout = timeout - header.forward.timeout;

// Send the structure created off the buffer and the header from the message
if (forward_msg(forward_struct, &header) == SLURM_ERROR) {
       error("problem with forward msg");
}

*********************************************************************/
extern int forward_msg(forward_struct_t *forward_struct,
		       header_t *header);


/*
 * start_msg_tree  - logic to begin the forward tree and
 *                   accumulate the return codes from processes getting the
 *                   forwarded message
 *
 * IN: hl          - hostlist_t   - list of every node to send message to
 * IN: msg         - slurm_msg_t  - message to send.
 * IN: timeout     - int          - how long to wait in milliseconds.
 * RET List 	   - List containing the responses of the children
 *		     (if any) we forwarded the message to. List
 *		     containing type (ret_data_info_t).
 */
extern list_t *start_msg_tree(hostlist_t *hl, slurm_msg_t *msg, int timeout);

/*
 * mark_as_failed_forward- mark a node as failed and add it to "ret_list"
 *
 * IN: ret_list       - List *   - ret_list to put ret_data_info
 * IN: node_name      - char *   - node name that failed
 * IN: err            - int      - error message from attempt
 *
 */
extern void mark_as_failed_forward(list_t **ret_list, char *node_name, int err);

extern void forward_wait(slurm_msg_t *msg);

extern void fwd_set_alias_addrs(slurm_node_alias_addrs_t *node_alias);

/* destroyers */
extern void destroy_data_info(void *object);
extern void destroy_forward(forward_t *forward);
extern void destroy_forward_struct(forward_struct_t *forward_struct);

#endif
