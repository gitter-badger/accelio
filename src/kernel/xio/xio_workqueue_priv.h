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
#ifndef XIO_WORKQUEUE_PRIV_H
#define XIO_WORKQUEUE_PRIV_H

enum xio_work_flags {
	XIO_WORK_PENDING	= 1 << 0,
	XIO_WORK_CANCELED	= 1 << 1,
	XIO_WORK_RUNNING	= 1 << 2,
	XIO_WORK_INITIALIZED	= 1 << 3,
	XIO_WORK_IN_HANDLER	= 1 << 4
};

struct xio_uwork {
	struct xio_ev_data ev_data;
	struct xio_context *ctx;
	void	(*function)(void *data);
	void	*data;
	volatile unsigned long flags;
	struct completion complete;
};

typedef struct xio_work {
	struct work_struct	work;
	struct xio_uwork	uwork;
} xio_work_handle_t;

typedef struct xio_delayed_work {
	struct delayed_work	dwork;
	struct xio_uwork	uwork;
} xio_delayed_work_handle_t;

static inline int xio_is_uwork_pending(struct xio_uwork *uwork)
{
	/* xio_ev_callback sets RUNNING before clearing PENDING */
	if (test_bit(XIO_WORK_PENDING, &uwork->flags))
		return 1;

	if (test_bit(XIO_WORK_RUNNING, &uwork->flags))
		return 1;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_is_work_pending							     */
/*---------------------------------------------------------------------------*/
static inline int xio_is_work_pending(xio_work_handle_t *work)
{
	return xio_is_uwork_pending(&work->uwork);
}

/*---------------------------------------------------------------------------*/
/* xio_is_delayed_work_pending						     */
/*---------------------------------------------------------------------------*/
static inline int xio_is_delayed_work_pending(xio_delayed_work_handle_t *dwork)
{
	return xio_is_uwork_pending(&dwork->uwork);
}

#endif /* XIO_WORKQUEUE_PRIV_H */

