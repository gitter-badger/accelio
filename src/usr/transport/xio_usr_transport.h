/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef XIO_COMMON_TRANSPORT_H
#define XIO_COMMON_TRANSPORT_H

#include "xio_transport.h"
#include "xio_context.h"


#define MAX_SGE				(XIO_IOVLEN + 1)

#define DEF_DATA_ALIGNMENT		0
#define SEND_BUF_SZ			9216
#define MAX_HDR_SZ			512

#define NUM_CONN_SETUP_TASKS		2 /* one posted for req rx,
					   * one for reply tx
					   */
#define CONN_SETUP_BUF_SIZE		4096

#define NUM_START_PRIMARY_POOL_TASKS	32
#define NUM_ALLOC_PRIMARY_POOL_TASKS	512

#define USECS_IN_SEC			1000000
#define NSECS_IN_USEC			1000

#define VALIDATE_SZ(sz)	do {			\
		if (optlen != (sz)) {		\
			xio_set_error(EINVAL);	\
			return -1;		\
		}				\
	} while (0)


#define xio_prefetch(p)            __builtin_prefetch(p)

/* header flags */
#define XIO_HEADER_FLAG_NONE		0x00
#define XIO_HEADER_FLAG_SMALL_ZERO_COPY	0x01


/*---------------------------------------------------------------------------*/
/* enums								     */
/*---------------------------------------------------------------------------*/
enum xio_transport_state {
	XIO_STATE_INIT,
	XIO_STATE_LISTEN,
	XIO_STATE_CONNECTED,
	XIO_STATE_DISCONNECTED,
	XIO_STATE_RECONNECT,
	XIO_STATE_CLOSED,
	XIO_STATE_DESTROYED
};

struct xio_mr {
	void				*addr;  /* for new devices */
	size_t				length; /* for new devices */
	int				access; /* for new devices */
	int				addr_alloced;	/* address was
							   allocated by xio */
	struct list_head		dm_list;
	struct list_head		mr_list_entry;
};

/*
 * The next routines deal with comparing 16 bit unsigned ints
 * and worry about wraparound (automatic with unsigned arithmetic).
 */

static inline int16_t before(uint16_t seq1, uint16_t seq2)
{
	return (int16_t)(seq1 - seq2) < 0;
}
#define after(seq2, seq1)       before(seq1, seq2)

static inline int16_t before_eq(uint16_t seq1, uint16_t seq2)
{
	return (int16_t)(seq1 - seq2) <= 0;
}
#define after_eq(seq2, seq1)       before_eq(seq1, seq2)


/* is s2<=s1<s3 ? */
static inline int16_t between(uint16_t seq1, uint16_t seq2, uint16_t seq3)
{
	if (before_eq(seq1, seq2) && before(seq2, seq3))
		return 1;
	return 0;
}

static inline
unsigned long long timespec_to_usecs(struct timespec *time_spec)
{
	unsigned long long retval = 0;

	retval  = time_spec->tv_sec * USECS_IN_SEC;
	retval += time_spec->tv_nsec / NSECS_IN_USEC;

	return retval;
}

int xio_transport_mempool_array_init(struct xio_mempool
				     ***mempool_array,
				     int *mempool_array_len);

void xio_transport_mempool_array_release(struct xio_mempool
					 **mempool_array,
					 int mempool_array_len);

struct xio_mempool *xio_transport_mempool_array_get(
		struct xio_context *ctx,
		struct xio_mempool **mempool_array,
		int mempool_array_len,
		int reg_mr);



#endif  /* XIO_COMMON_TRANSPORT_H */
