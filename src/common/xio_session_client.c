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

#include "xio_os.h"
#include "libxio.h"
#include "xio_common.h"
#include "xio_protocol.h"
#include "xio_observer.h"
#include "xio_task.h"
#include "xio_context.h"
#include "xio_transport.h"
#include "xio_sessions_cache.h"
#include "xio_hash.h"
#include "xio_session.h"
#include "xio_nexus.h"
#include "xio_connection.h"
#include "xio_session_priv.h"

/*---------------------------------------------------------------------------*/
/* xio_session_write_setup_req						     */
/*---------------------------------------------------------------------------*/
struct xio_msg *xio_session_write_setup_req(struct xio_session *session)
{
	struct xio_msg		*msg;
	void			*buf;
	uint8_t			*ptr;
	uint16_t		len;


	/* allocate message */
	buf = kcalloc(SETUP_BUFFER_LEN + sizeof(struct xio_msg),
		      sizeof(uint8_t), GFP_KERNEL);
	if (buf == NULL) {
		ERROR_LOG("message allocation failed\n");
		xio_set_error(ENOMEM);
		return NULL;
	}

	/* fill the message */
	msg = buf;
	msg->out.header.iov_base = msg + 1;
	msg->out.header.iov_len = 0;
	msg->out.sgl_type = XIO_SGL_TYPE_IOV;
	msg->out.data_iov.nents = 1;
	msg->out.data_iov.max_nents = XIO_IOVLEN;
	msg->in.sgl_type = XIO_SGL_TYPE_IOV;
	msg->in.data_iov.nents = 0;
	msg->in.data_iov.max_nents = XIO_IOVLEN;

	ptr = msg->out.header.iov_base;
	len = 0;

	/* serialize message on the buffer */
	len = xio_write_uint32(session->session_id , 0, ptr);
	ptr  = ptr + len;

	/* uri length */
	len = xio_write_uint16((uint16_t)session->uri_len , 0, ptr);
	ptr  = ptr + len;

	/* private length */
	len = xio_write_uint16((uint16_t)(session->hs_private_data_len),
			       0, ptr);
	ptr  = ptr + len;

	if (session->uri_len) {
		len = xio_write_array((uint8_t *)session->uri,
				      session->uri_len, 0, ptr);
		ptr  = ptr + len;
	}
	if (session->hs_private_data_len) {
		len = xio_write_array(session->hs_private_data,
				      session->hs_private_data_len,
				      0, ptr);
		ptr  = ptr + len;
	}
	msg->out.header.iov_len = ptr - (uint8_t *)msg->out.header.iov_base;

	if (msg->out.header.iov_len > SETUP_BUFFER_LEN)  {
		ERROR_LOG("primary task pool is empty\n");
		xio_set_error(XIO_E_MSG_SIZE);
		kfree(buf);
		return NULL;
	}

	return msg;
}

static void xio_xmit_messages(void *_connection)
{
	struct xio_connection *connection = _connection;

	xio_connection_xmit_msgs(connection);
}

/*---------------------------------------------------------------------------*/
/* xio_on_connection_hello_rsp_recv			                     */
/*---------------------------------------------------------------------------*/
int xio_on_connection_hello_rsp_recv(struct xio_connection *connection,
				     struct xio_task *task)
{
	int is_last = 1;
	struct xio_connection *tmp_connection;
	struct xio_session    *session = connection->session;

	TRACE_LOG("got hello response. session:%p, connection:%p\n",
		  session, connection);

	xio_connection_release_hello(connection, task->sender_task->omsg);
	/* recycle the task */
	xio_tasks_pool_put(task->sender_task);
	task->sender_task = NULL;
	xio_tasks_pool_put(task);


	/* set the new connection to ESTABLISHED */
	xio_connection_set_state(connection,
				 XIO_CONNECTION_STATE_ESTABLISHED);
	xio_session_notify_connection_established(session, connection);

	if (session->state == XIO_SESSION_STATE_ACCEPTED) {
		/* is this the last to accept */
		spin_lock(&session->connections_list_lock);
		list_for_each_entry(tmp_connection,
				    &session->connections_list,
				    connections_list_entry) {
			if (tmp_connection->state !=
					XIO_CONNECTION_STATE_ESTABLISHED) {
				is_last = 0;
				break;
			}
		}
		spin_unlock(&session->connections_list_lock);
		if (is_last) {
			session->state = XIO_SESSION_STATE_ONLINE;
			TRACE_LOG("session state is now ONLINE. session:%p\n",
				  session);
			if (session->ses_ops.on_session_established)
				session->ses_ops.on_session_established(
						session,
						&session->new_ses_rsp,
						session->cb_user_context);

			/* send one message to pass sending to the
			 * right thread */
			kfree(session->new_ses_rsp.private_data);

			spin_lock(&session->connections_list_lock);
			list_for_each_entry(tmp_connection,
					    &session->connections_list,
					    connections_list_entry) {
				/* set the new connection to online */
				xio_connection_set_state(
						tmp_connection,
						XIO_CONNECTION_STATE_ONLINE);
				xio_ctx_add_work(
						tmp_connection->ctx,
						tmp_connection,
						xio_xmit_messages,
						&tmp_connection->hello_work);
			}
			spin_unlock(&session->connections_list_lock);
		}
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_session_accept_connection					     */
/*---------------------------------------------------------------------------*/
int xio_session_accept_connection(struct xio_session *session)
{
	struct xio_connection	*connection, *tmp_connection;
	struct xio_nexus		*nexus;
	int			retval = 0;
	char			*portal;

	list_for_each_entry_safe(connection, tmp_connection,
				 &session->connections_list,
				 connections_list_entry) {
		if (connection->nexus == NULL) {
			if (connection->conn_idx == 0) {
				portal = session->portals_array[
						session->last_opened_portal++];
				if (session->last_opened_portal ==
				    session->portals_array_len)
					session->last_opened_portal = 0;
			} else {
				int pid = (connection->conn_idx %
					   session->portals_array_len);
				portal = session->portals_array[pid];
			}
			nexus = xio_nexus_open(connection->ctx, portal,
					       &session->observer,
					       session->session_id);

			if (nexus == NULL) {
				ERROR_LOG("failed to open connection to %s\n",
					  portal);
				retval = -1;
				break;
			}
			connection = xio_session_assign_nexus(session, nexus);
			if (connection == NULL) {
				ERROR_LOG("failed to assign connection\n");
				retval = -1;
				break;
			}
			DEBUG_LOG("reconnecting to %s\n", portal);
			retval = xio_nexus_connect(nexus, portal,
						   &session->observer, NULL);
			if (retval != 0) {
				ERROR_LOG("connection connect failed\n");
				retval = -1;
				break;
			}
		}
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_session_redirect_connection					     */
/*---------------------------------------------------------------------------*/
int xio_session_redirect_connection(struct xio_session *session)
{
	struct xio_nexus		*nexus, *tmp_nexus;
	int			retval;
	char			*service;

	service = session->services_array[session->last_opened_service++];
	if (session->last_opened_service == session->services_array_len)
		session->last_opened_service = 0;

	nexus = xio_nexus_open(session->lead_connection->ctx, service, NULL, 0);
	if (nexus == NULL) {
		ERROR_LOG("failed to open connection to %s\n",
			  service);
		return -1;
	}
	/* initialize the redirected connection */
	tmp_nexus = session->lead_connection->nexus;
	session->redir_connection = session->lead_connection;
	xio_connection_set_nexus(session->redir_connection, nexus);

	ERROR_LOG("connection redirected to %s\n", service);
	retval = xio_nexus_connect(nexus, service, &session->observer, NULL);
	if (retval != 0) {
		ERROR_LOG("connection connect failed\n");
		goto cleanup;
	}

	kfree(session->uri);
	session->uri = kstrdup(service, GFP_KERNEL);

	/* prep the lead connection for close */
	session->lead_connection = xio_connection_init(
			session,
			session->lead_connection->ctx,
			session->lead_connection->conn_idx,
			session->lead_connection->cb_user_context);
	xio_connection_set_nexus(session->lead_connection, tmp_nexus);

	return 0;

cleanup:
	xio_nexus_close(nexus, &session->observer);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_on_session_rejected			                             */
/*---------------------------------------------------------------------------*/
int xio_on_session_rejected(struct xio_session *session)
{
	struct xio_connection *pconnection, *tmp_connection;

	/* also send disconnect to connections that do no have nexus */
	list_for_each_entry_safe(pconnection, tmp_connection,
				 &session->connections_list,
				 connections_list_entry) {
		session->disable_teardown   = 0;
		pconnection->disable_notify = 0;
		pconnection->close_reason = XIO_E_SESSION_REJECTED;
		if (pconnection->nexus)
			xio_disconnect_initial_connection(pconnection);
		else
			xio_connection_disconnected(pconnection);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_read_setup_rsp							     */
/*---------------------------------------------------------------------------*/
int xio_read_setup_rsp(struct xio_connection *connection,
		       struct xio_task *task,
		       uint16_t *action)
{
	struct xio_msg			*msg = &task->imsg;
	struct xio_session_hdr		hdr;
	struct xio_session		*session = connection->session;
	struct xio_new_session_rsp	*rsp = &session->new_ses_rsp;
	uint8_t				*ptr;
	uint16_t			len;
	int				i = 0;
	uint16_t			str_len;

	/* read session header */
	xio_session_read_header(task, &hdr);

	task->imsg.sn = hdr.serial_num;

	/* free the outgoing message */
	kfree(task->sender_task->omsg);
	task->sender_task->omsg = NULL;

	/* read the message */
	ptr = msg->in.header.iov_base;

	/* read the payload */
	len = xio_read_uint32(&session->peer_session_id , 0, ptr);
	ptr  = ptr + len;

	len = xio_read_uint16(action, 0, ptr);
	ptr = ptr + len;

	switch (*action) {
	case XIO_ACTION_ACCEPT:
		len = xio_read_uint16(&session->portals_array_len, 0, ptr);
		ptr = ptr + len;

		len = xio_read_uint16(&rsp->private_data_len, 0, ptr);
		ptr = ptr + len;

		if (session->portals_array_len) {
			session->portals_array = kcalloc(
					session->portals_array_len,
				       sizeof(char *), GFP_KERNEL);
			if (session->portals_array == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}
			for (i = 0; i < session->portals_array_len; i++) {
				len = xio_read_uint16(&str_len, 0, ptr);
				ptr = ptr + len;

				session->portals_array[i] =
					kstrndup((char *)ptr, str_len,
						 GFP_KERNEL);
				session->portals_array[i][str_len] = 0;
				ptr = ptr + str_len;
			}

		} else {
			session->portals_array = NULL;
		}

		if (session->new_ses_rsp.private_data_len) {
			rsp->private_data = kcalloc(rsp->private_data_len,
					sizeof(uint8_t), GFP_KERNEL);
			if (rsp->private_data == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			len = xio_read_array(rsp->private_data,
					     rsp->private_data_len, 0, ptr);
			ptr = ptr + len;
		} else {
			rsp->private_data = NULL;
		}
		break;
	case XIO_ACTION_REDIRECT:
		len = xio_read_uint16(&session->services_array_len, 0, ptr);
		ptr = ptr + len;

		len = xio_read_uint16(&rsp->private_data_len, 0, ptr);
		ptr = ptr + len;

		if (session->services_array_len) {
			session->services_array = kcalloc(
					session->services_array_len,
					sizeof(char *), GFP_KERNEL);
			if (session->services_array == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			for (i = 0; i < session->services_array_len; i++) {
				len = xio_read_uint16(&str_len, 0, ptr);
				ptr = ptr + len;

				session->services_array[i] =
					kstrndup((char *)ptr, str_len,
						 GFP_KERNEL);
				session->services_array[i][str_len] = 0;
				ptr = ptr + str_len;
			}

		} else {
			session->services_array = NULL;
		}
		break;

	case XIO_ACTION_REJECT:
		len = xio_read_uint32(&session->reject_reason , 0, ptr);
		ptr  = ptr + len;

		len = xio_read_uint16(&rsp->private_data_len, 0, ptr);
		ptr = ptr + len;

		if (session->new_ses_rsp.private_data_len) {
			rsp->private_data = kcalloc(
						rsp->private_data_len,
						sizeof(uint8_t), GFP_KERNEL);
			if (rsp->private_data == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			len = xio_read_array(rsp->private_data,
					     rsp->private_data_len, 0, ptr);
			ptr = ptr + len;
		} else {
			rsp->private_data = NULL;
		}
		break;
	default:
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_prep_portal							     */
/*---------------------------------------------------------------------------*/
static int xio_prep_portal(struct xio_connection *connection)
{
	struct xio_session *session = connection->session;
	char portal[64];

	/* extract portal from uri */
	if (xio_uri_get_portal(session->uri, portal, sizeof(portal)) != 0) {
		xio_set_error(EADDRNOTAVAIL);
		ERROR_LOG("parsing uri failed. uri: %s\n", session->uri);
		return -1;
	}
	session->portals_array = kcalloc(
			1,
			sizeof(char *), GFP_KERNEL);
	if (session->portals_array == NULL) {
		ERROR_LOG("allocation failed\n");
		xio_set_error(ENOMEM);
		return -1;
	}
	session->portals_array_len = 1;
	session->portals_array[0] = kstrdup(portal, GFP_KERNEL);

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_on_setup_rsp_recv			                             */
/*---------------------------------------------------------------------------*/
int xio_on_setup_rsp_recv(struct xio_connection *connection,
			  struct xio_task *task)
{
	uint16_t			action = 0;
	struct xio_session		*session = connection->session;
	struct xio_new_session_rsp	*rsp = &session->new_ses_rsp;
	int				retval = 0;
	struct xio_connection		*tmp_connection;


	retval = xio_read_setup_rsp(connection, task, &action);

	/* the tx task is returend back to pool */
	xio_tasks_pool_put(task->sender_task);
	task->sender_task = NULL;

	xio_tasks_pool_put(task);
	DEBUG_LOG("task recycled\n");

	if (retval != 0) {
		ERROR_LOG("failed to read setup response\n");
		return -1;
	}

	switch (action) {
	case XIO_ACTION_ACCEPT:
		if (session->portals_array == NULL)  {
			xio_connection_set_state(
					connection,
					XIO_CONNECTION_STATE_ESTABLISHED);
			xio_session_notify_connection_established(
						session, connection);

			xio_prep_portal(connection);

			/* insert the connection into list */
			xio_session_assign_nexus(session, connection->nexus);
			session->lead_connection = NULL;
			session->redir_connection = NULL;
			session->disable_teardown = 0;

			if (session->connections_nr > 1) {
				session->state = XIO_SESSION_STATE_ACCEPTED;

				/* open new connections */
				retval = xio_session_accept_connection(session);
				if (retval != 0) {
					ERROR_LOG(
						"failed to accept connection\n");
					return -1;
				}
			} else {
				session->state = XIO_SESSION_STATE_ONLINE;
				connection->state = XIO_CONNECTION_STATE_ONLINE;
				TRACE_LOG(
				     "session state is now ONLINE. session:%p\n",
				     session);

				/* notify the upper layer */
				if (session->ses_ops.on_session_established)
					session->ses_ops.on_session_established(
						session, rsp,
						session->cb_user_context);

				kfree(rsp->private_data);
				rsp->private_data = NULL;
			}
			/* start connection transmission */
			xio_connection_xmit_msgs(connection);

			return 0;
		} else { /* reconnect to peer other session */
			TRACE_LOG("session state is now ACCEPT. session:%p\n",
				  session);

			/* clone temporary connection */
			tmp_connection = xio_connection_init(
				session,
				session->lead_connection->ctx,
				session->lead_connection->conn_idx,
				session->lead_connection->cb_user_context);

			xio_connection_set_nexus(tmp_connection,
						 connection->nexus);
			connection->nexus = NULL;
			session->lead_connection = tmp_connection;

			/* close the lead/redirected connection */
			/* temporary disable teardown */
			session->disable_teardown = 1;
			session->lead_connection->disable_notify = 1;
			session->lead_connection->state	=
					XIO_CONNECTION_STATE_ONLINE;
			xio_disconnect(session->lead_connection);

			/* temporary disable teardown - on cached nexuss close
			 * callback may jump immediately and since there are no
			 * connections. teardown may notified
			 */
			session->state = XIO_SESSION_STATE_ACCEPTED;
			/* open new connections */
			retval = xio_session_accept_connection(session);
			if (retval != 0) {
				ERROR_LOG("failed to accept connection\n");
				return -1;
			}
			TRACE_LOG("sending fin request. session:%p, " \
				  "connection:%p\n",
				  session->lead_connection->session,
				  session->lead_connection);

			return 0;
		}
		break;
	case XIO_ACTION_REDIRECT:
		TRACE_LOG("session state is now REDIRECT. session:%p\n",
			  session);

		session->state = XIO_SESSION_STATE_REDIRECTED;

		/* open new connections */
		retval = xio_session_redirect_connection(session);
		if (retval != 0) {
			ERROR_LOG("failed to redirect connection\n");
			return -1;
		}

		/* close the lead connection */
		session->disable_teardown = 1;
		session->lead_connection->disable_notify = 1;
		session->lead_connection->state	= XIO_CONNECTION_STATE_ONLINE;
		xio_disconnect_initial_connection(session->lead_connection);

		return 0;
		break;
	case XIO_ACTION_REJECT:
		xio_connection_set_state(connection,
					 XIO_CONNECTION_STATE_ESTABLISHED);
		xio_session_notify_connection_established(session,
							  connection);

		session->state = XIO_SESSION_STATE_REJECTED;
		session->disable_teardown = 0;
		session->lead_connection = NULL;

		TRACE_LOG("session state is now REJECT. session:%p\n",
			  session);

		retval = xio_on_session_rejected(session);
		if (retval != 0)
			ERROR_LOG("failed to reject session\n");

		kfree(rsp->private_data);
		rsp->private_data = NULL;

		return retval;

		break;
	}

	return -1;
}


/*---------------------------------------------------------------------------*/
/* xio_on_nexus_refused							     */
/*---------------------------------------------------------------------------*/
int xio_on_nexus_refused(struct xio_session *session,
			 struct xio_nexus *nexus,
			 union xio_nexus_event_data *event_data)
{
	struct xio_connection *connection,
			      *curr_connection, *next_connection;

	/* enable the teardown */
	session->disable_teardown  = 0;
	session->lead_connection = NULL;
	session->redir_connection = NULL;

	switch (session->state) {
	case XIO_SESSION_STATE_CONNECT:
	case XIO_SESSION_STATE_REDIRECTED:
		session->state = XIO_SESSION_STATE_REFUSED;
		list_for_each_entry_safe(curr_connection, next_connection,
					 &session->connections_list,
					 connections_list_entry) {
					 connection = list_first_entry(
						&session->connections_list,
						struct xio_connection,
						connections_list_entry);
					 xio_connection_refused(connection);
		}
		break;
	default:
		connection = xio_session_find_connection(session, nexus);
		xio_connection_refused(connection);
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_client_nexus_established					     */
/*---------------------------------------------------------------------------*/
int xio_on_client_nexus_established(struct xio_session *session,
				    struct xio_nexus *nexus,
				    union xio_nexus_event_data *event_data)
{
	int				retval = 0;
	struct xio_connection		*connection;
	struct xio_msg			*msg;
	struct xio_session_event_data	ev_data = {
		.event	=	XIO_SESSION_ERROR_EVENT,
		.reason =	XIO_E_SESSION_REFUSED
	};

	switch (session->state) {
	case XIO_SESSION_STATE_CONNECT:
		msg = xio_session_write_setup_req(session);
		if (msg == NULL) {
			ERROR_LOG("setup request creation failed\n");
			return -1;
		}

		msg->type = XIO_SESSION_SETUP_REQ;
		retval = xio_connection_send(session->lead_connection,
					     msg);
		if (retval && retval != -EAGAIN) {
			TRACE_LOG("failed to send session "\
					"setup request\n");
			ev_data.conn =  session->lead_connection;
			ev_data.conn_user_context =
				session->lead_connection->cb_user_context;
			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &ev_data,
						session->cb_user_context);
		}

		break;
	case XIO_SESSION_STATE_REDIRECTED:
		msg = xio_session_write_setup_req(session);
		if (msg == NULL) {
			ERROR_LOG("setup request creation failed\n");
			return -1;
		}
		session->state = XIO_SESSION_STATE_CONNECT;

		msg->type      = XIO_SESSION_SETUP_REQ;

		retval = xio_connection_send(session->redir_connection,
					     msg);
		if (retval && retval != -EAGAIN) {
			TRACE_LOG("failed to send session setup request\n");
			ev_data.conn =  session->redir_connection;
			ev_data.conn_user_context =
				session->redir_connection->cb_user_context;
			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &ev_data,
						session->cb_user_context);
		}
		break;
	case XIO_SESSION_STATE_ACCEPTED:
		connection = xio_session_find_connection(session, nexus);
		if (connection == NULL) {
			ERROR_LOG("failed to find connection session:%p," \
				  "nexus:%p\n", session, nexus);
			return -1;
		}
		session->disable_teardown = 0;

		/* introduce the connection to the session */
		xio_connection_send_hello_req(connection);

		break;
	case XIO_SESSION_STATE_ONLINE:
		connection = xio_session_find_connection(session, nexus);
		if (connection == NULL)  {
			ERROR_LOG("failed to find connection\n");
			return -1;
		}
		DEBUG_LOG("connection established: " \
			  "connection:%p, session:%p, nexus:%p\n",
			   connection, connection->session,
			   connection->nexus);
		/* now try to send */
		xio_connection_set_state(connection,
					 XIO_CONNECTION_STATE_ONLINE);
		xio_connection_xmit_msgs(connection);
		break;
	default:
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_client_on_nexus_event						     */
/*---------------------------------------------------------------------------*/
int xio_client_on_nexus_event(void *observer, void *sender, int event,
			      void *event_data)
{
	struct xio_session	*session = observer;
	struct xio_nexus	*nexus	= sender;
	int			retval  = 0;


	switch (event) {
	case XIO_NEXUS_EVENT_NEW_MESSAGE:
/*
		TRACE_LOG("session: [notification] - new message. " \
			 "session:%p, nexus:%p\n", observer, sender);

*/		xio_on_new_message(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_SEND_COMPLETION:
/*		TRACE_LOG("session: [notification] - send_completion. " \
			 "session:%p, nexus:%p\n", observer, sender);
*/
		xio_on_send_completion(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_ASSIGN_IN_BUF:
/*		TRACE_LOG("session: [notification] - assign in buf. " \
			 "session:%p, nexus:%p\n", observer, sender);
*/
		xio_on_assign_in_buf(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_CANCEL_REQUEST:
		DEBUG_LOG("session: [notification] - cancel request. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_cancel_request(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_CANCEL_RESPONSE:
		DEBUG_LOG("session: [notification] - cancel response. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_cancel_response(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_ESTABLISHED:
		DEBUG_LOG("session: [notification] - nexus established. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_client_nexus_established(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_DISCONNECTED:
		DEBUG_LOG("session: [notification] - nexus disconnected" \
			 " session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_disconnected(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_RECONNECTED:
		DEBUG_LOG("session: [notification] - connection reconnected" \
			 " session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_reconnected(session, nexus);
		break;
	case XIO_NEXUS_EVENT_CLOSED:
		DEBUG_LOG("session: [notification] - nexus closed. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_closed(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_REFUSED:
		DEBUG_LOG("session: [notification] - nexus refused. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_refused(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_ERROR:
		DEBUG_LOG("session: [notification] - nexus error. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_error(session, nexus, event_data);
		break;
	case XIO_NEXUS_EVENT_MESSAGE_ERROR:
		DEBUG_LOG("session: [notification] - nexus message error. " \
			 "session:%p, nexus:%p\n", observer, sender);
		xio_on_nexus_message_error(session, nexus, event_data);
		break;
	default:
		DEBUG_LOG("session: [notification] - unexpected event. " \
			 "event:%d, session:%p, nexus:%p\n",
			 event, observer, sender);
		xio_on_nexus_error(session, nexus, event_data);
		break;
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_connect								     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_connect(struct xio_session  *session,
				   struct xio_context  *ctx,
				   uint32_t connection_idx,
				   const char *out_if,
				   void *connection_user_context)
{
	struct xio_session	*psession = NULL;
	struct xio_connection	*connection = NULL, *tmp_connection;
	int			retval;

	if ((ctx == NULL) || (session == NULL)) {
		ERROR_LOG("invalid parameters ctx:%p, session:%p\n",
			  ctx, session);
		xio_set_error(EINVAL);
		return NULL;
	}
	/* lookup for session in cache */
	psession = xio_sessions_cache_lookup(session->session_id);
	if (psession == NULL) {
		ERROR_LOG("failed to find session\n");
		xio_set_error(EINVAL);
		return NULL;
	}

	mutex_lock(&session->lock);

	/* only one connection per context allowed */
	connection = xio_session_find_connection_by_ctx(session, ctx);
	if (connection != NULL) {
		ERROR_LOG("context:%p, already assigned connection:%p\n",
			  ctx, connection);
		goto cleanup;
	}
	if (session->state == XIO_SESSION_STATE_INIT) {
		char portal[64];
		struct xio_nexus	*nexus;
		/* extract portal from uri */
		if (xio_uri_get_portal(session->uri, portal,
				       sizeof(portal)) != 0) {
			xio_set_error(EADDRNOTAVAIL);
			ERROR_LOG("parsing uri failed. uri: %s\n",
				  session->uri);
			goto cleanup;
		}
		nexus = xio_nexus_open(ctx, portal, &session->observer,
				       session->session_id);
		if (nexus == NULL) {
			ERROR_LOG("failed to create connection\n");
			goto cleanup;
		}
		/* initialize the lead connection */
		session->lead_connection = xio_session_alloc_connection(
				session, ctx,
				connection_idx,
				connection_user_context);
		session->lead_connection->nexus = nexus;

		connection  = session->lead_connection;

		/* get transport class routines */
		session->validators_cls = xio_nexus_get_validators_cls(nexus);

		session->state = XIO_SESSION_STATE_CONNECT;

		retval = xio_nexus_connect(nexus, portal,
					   &session->observer, out_if);
		if (retval != 0) {
			ERROR_LOG("connection connect failed\n");
			session->state = XIO_SESSION_STATE_INIT;
			goto cleanup;
		}
	} else if ((session->state == XIO_SESSION_STATE_CONNECT) ||
		   (session->state == XIO_SESSION_STATE_REDIRECTED)) {
		connection  = xio_session_alloc_connection(
						session,
						ctx, connection_idx,
						connection_user_context);
	} else if (session->state == XIO_SESSION_STATE_ONLINE ||
		   session->state == XIO_SESSION_STATE_ACCEPTED) {
		struct xio_nexus *nexus;
		char *portal;
		if (connection_idx == 0) {
			portal = session->portals_array[
					session->last_opened_portal++];
			if (session->last_opened_portal ==
			    session->portals_array_len)
					session->last_opened_portal = 0;
		} else {
			int pid = (connection_idx % session->portals_array_len);
			portal = session->portals_array[pid];
		}
		connection  = xio_session_alloc_connection(
						session, ctx,
						connection_idx,
						connection_user_context);
		nexus = xio_nexus_open(ctx, portal, &session->observer,
				       session->session_id);
		if (nexus == NULL) {
			ERROR_LOG("failed to open connection\n");
			goto cleanup;
		}
		tmp_connection = xio_session_assign_nexus(session, nexus);
		if (tmp_connection != connection) {
			ERROR_LOG("failed to open connection nexus:%p, %p %p\n",
				  nexus, tmp_connection, connection);
			goto cleanup;
		}
		DEBUG_LOG("reconnecting to %s, ctx:%p\n", portal, ctx);
		retval = xio_nexus_connect(nexus, portal,
					   &session->observer, out_if);
		if (retval != 0) {
			ERROR_LOG("connection connect failed\n");
			goto cleanup;
		}
		connection = tmp_connection;
		if (session->state == XIO_SESSION_STATE_ONLINE)
			xio_connection_send_hello_req(connection);
	}
	mutex_unlock(&session->lock);

	return connection;

cleanup:
	mutex_unlock(&session->lock);

	return NULL;
}

