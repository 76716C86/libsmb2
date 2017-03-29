/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
   Copyright (C) 2016 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef STDC_HEADERS
#include <stddef.h>
#endif

#include <errno.h>

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-private.h"

static int
smb2_encode_create_request(struct smb2_context *smb2,
                           struct smb2_pdu *pdu,
                           struct smb2_create_request *req)
{
        int len;
        char *buf;
        struct ucs2 *name = NULL;
        struct smb2_iovec *iov;

        len = SMB2_CREATE_REQUEST_SIZE & 0xfffffffe;
        buf = malloc(len);
        if (buf == NULL) {
                smb2_set_error(smb2, "Failed to allocate create buffer");
                return -1;
        }
        memset(buf, 0, len);
        
        iov = smb2_add_iovector(smb2, &pdu->out, buf, len, free);

        /* Name */
        if (req->name && req->name[0]) {
                name = utf8_to_ucs2(req->name);
                if (name == NULL) {
                        smb2_set_error(smb2, "Could not convert name into UCS2");
                        free(buf);
                        return -1;
                }
                /* name length */
                smb2_set_uint16(iov, 46, 2 * name->len);
        }

        smb2_set_uint16(iov, 0, SMB2_CREATE_REQUEST_SIZE);
        smb2_set_uint8(iov, 2, req->security_flags);
        smb2_set_uint8(iov, 3, req->requested_oplock_level);
        smb2_set_uint32(iov, 4, req->impersonation_level);
        smb2_set_uint64(iov, 8, req->smb_create_flags);
        smb2_set_uint32(iov, 24, req->desired_access);
        smb2_set_uint32(iov, 28, req->file_attributes);
        smb2_set_uint32(iov, 32, req->share_access);
        smb2_set_uint32(iov, 36, req->create_disposition);
        smb2_set_uint32(iov, 40, req->create_options);
        /* name offset */
        smb2_set_uint16(iov, 44, SMB2_HEADER_SIZE + 56);
        smb2_set_uint32(iov, 52, req->create_context_length);

        /* Name */
        if (name) {
                iov = smb2_add_iovector(smb2, &pdu->out,
                                        malloc(2 * name->len),
                                        2 * name->len,
                                        free);

                memcpy(iov->buf, &name->val[0], 2 * name->len);
        }
        free(name);

        /* Create Context */
        if (req->create_context_length) {
                smb2_set_error(smb2, "Create context not implemented, yet");
                return -1;
        }

        /* The buffer must contain at least one byte, even if name is "" 
         * and there is no create context.
         */
        if (name == NULL && !req->create_context_length) {
                static char zero;

                iov = smb2_add_iovector(smb2, &pdu->out,
                                        &zero, 1, NULL);
        }
        
        return 0;
}

static int
smb2_decode_create_reply(struct smb2_context *smb2,
                         struct smb2_pdu *pdu,
                         struct smb2_create_reply *rep)
{
        uint16_t struct_size;

        smb2_get_uint16(&pdu->in.iov[0], 0, &struct_size);
        if (struct_size != SMB2_CREATE_REPLY_SIZE ||
            struct_size < pdu->in.iov[0].len) {
                smb2_set_error(smb2, "Unexpected size of Create reply. "
                               "Expected %d, got %d",
                               SMB2_CLOSE_REPLY_SIZE,
                               (int)pdu->in.iov[0].len);
                return -1;
        }

        smb2_get_uint8(&pdu->in.iov[0], 2, &rep->oplock_level);
        smb2_get_uint8(&pdu->in.iov[0], 3, &rep->flags);
        smb2_get_uint32(&pdu->in.iov[0], 4, &rep->create_action);
        smb2_get_uint64(&pdu->in.iov[0], 8, &rep->creation_time);
        smb2_get_uint64(&pdu->in.iov[0], 16, &rep->last_access_time);
        smb2_get_uint64(&pdu->in.iov[0], 24, &rep->last_write_time);
        smb2_get_uint64(&pdu->in.iov[0], 32, &rep->change_time);
        smb2_get_uint64(&pdu->in.iov[0], 40, &rep->allocation_size);
        smb2_get_uint64(&pdu->in.iov[0], 48, &rep->end_of_file);
        smb2_get_uint32(&pdu->in.iov[0], 56, &rep->file_attributes);
        memcpy(rep->file_id, pdu->in.iov[0].buf + 64, SMB2_FD_SIZE);
        smb2_get_uint32(&pdu->in.iov[0], 84, &rep->create_context_length);

        /* No support for createcontext yet*/
        /* Create Context */
        if (rep->create_context_length) {
                smb2_set_error(smb2, "Create context not implemented, yet");
                return -1;
        }

        return 0;
}

int smb2_cmd_create_async(struct smb2_context *smb2,
                          struct smb2_create_request *req,
                          smb2_command_cb cb, void *cb_data)
{
        struct smb2_pdu *pdu;

        pdu = smb2_allocate_pdu(smb2, SMB2_CREATE, cb, cb_data);
        if (pdu == NULL) {
                return -1;
        }

        if (smb2_encode_create_request(smb2, pdu, req)) {
                smb2_free_pdu(smb2, pdu);
                return -1;
        }
        
        if (smb2_queue_pdu(smb2, pdu)) {
                smb2_free_pdu(smb2, pdu);
                return -1;
        }

        return 0;
}

int smb2_process_create_reply(struct smb2_context *smb2,
                              struct smb2_pdu *pdu)
{
        struct smb2_create_reply reply;

        if (smb2_decode_create_reply(smb2, pdu, &reply) < 0) {
                pdu->cb(smb2, -EBADMSG, NULL, pdu->cb_data);
                return -1;
        }

        pdu->cb(smb2, pdu->header.status, &reply, pdu->cb_data);

        return 0;
}
