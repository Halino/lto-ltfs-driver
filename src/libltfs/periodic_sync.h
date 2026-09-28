/*
**
**  OO_Copyright_BEGIN
**
**
**  Copyright 2010, 2025 IBM Corp. All rights reserved.
**
**  Redistribution and use in source and binary forms, with or without
**   modification, are permitted provided that the following conditions
**  are met:
**  1. Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**  2. Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in the
**  documentation and/or other materials provided with the distribution.
**  3. Neither the name of the copyright holder nor the names of its
**     contributors may be used to endorse or promote products derived from
**     this software without specific prior written permission.
**
**  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
**  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
**  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
**  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
**  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
**  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
**  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
**  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
**  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
**  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
**  POSSIBILITY OF SUCH DAMAGE.
**
**
**  OO_Copyright_END
**
*************************************************************************************
**
** COMPONENT NAME:  IBM Linear Tape File System
**
** FILE NAME:       periodic_sync.c
**
** DESCRIPTION:     Prototypes for the periodic sync feature
**
** AUTHOR:          Atsushi Abe
**                  IBM Yamato, Japan
**                  PISTE@jp.ibm.com
*************************************************************************************
*/
#ifndef __periodic_sync_h
#define __periodic_sync_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ltfs_volume;
struct ltfs_operation_failure;

int periodic_sync_thread_init(unsigned int sec, uint64_t byte_threshold,
	struct ltfs_volume *vol, struct ltfs_operation_failure *failure);
int periodic_sync_thread_destroy(struct ltfs_volume *vol);
bool periodic_sync_thread_initialized(struct ltfs_volume *vol);
void periodic_sync_add_dirty_bytes(struct ltfs_volume *vol, uint64_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* __periodic_sync_h */
