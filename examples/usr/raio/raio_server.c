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
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/queue.h>
#include "libxio.h"
#include "raio_handlers.h"
#include <arpa/inet.h>


/*---------------------------------------------------------------------------*/
/* preprocessor macros							     */
/*---------------------------------------------------------------------------*/
#define MAX_THREADS		6
#define SIMULATE_DESTROY	0


#ifndef TAILQ_FOREACH_SAFE
#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST(head);					\
	     (var) && ((tvar) = TAILQ_NEXT(var, field), 1);		\
	     (var) = (tvar))
#endif

#ifndef SLIST_FOREACH_SAFE
#define	SLIST_FOREACH_SAFE(var, head, field, tvar)			 \
	for ((var) = SLIST_FIRST((head));				 \
			(var) && ((tvar) = SLIST_NEXT((var), field), 1); \
			(var) = (tvar))
#endif

/*---------------------------------------------------------------------------*/
/* structures								     */
/*---------------------------------------------------------------------------*/
struct portals_vec {
	int				vec_len;
	int				pad;
	const char			*vec[MAX_THREADS];
};

struct raio_thread_data {
	struct raio_server_data		*server_data;
	char				portal[64];
	int				affinity;
	int				pad;
	struct xio_msg			rsp;
	struct xio_context		*ctx;
};

struct raio_portal_data  {
	struct	raio_thread_data	*tdata;
	void				*dd_data;
};
struct raio_server_data;

struct  raio_connection_data {
	struct xio_connection			*connection;
	int					disconnected;
	int					pad;
	TAILQ_ENTRY(raio_connection_data)	conns_list_entry;
};

struct raio_session_data {
	struct	xio_session			*session;
	void					*dd_data;
	struct raio_portal_data			portal_data[MAX_THREADS];

	TAILQ_HEAD(, raio_connection_data)	conns_list;
	TAILQ_ENTRY(raio_session_data)		sessions_list_entry;
};

/* server private data */
struct raio_server_data {
	struct xio_context			*ctx;
	int					last_used;
	int					last_reaped;

	TAILQ_HEAD(, raio_session_data)		sessions_list;

	pthread_t				thread_id[MAX_THREADS];
	struct raio_thread_data			tdata[MAX_THREADS];
};

/*---------------------------------------------------------------------------*/
/* portals_get								     */
/*---------------------------------------------------------------------------*/
static struct portals_vec *portals_get(struct raio_server_data *server_data,
				       const char *uri, void *user_context)
{
	/* fill portals array and return it. */
	int			i, j;
	struct portals_vec	*portals = calloc(1, sizeof(*portals));
	if (server_data->last_reaped != -1) {
		server_data->last_used = server_data->last_reaped;
		server_data->last_reaped = -1;
	}
	for (i = 0; i < MAX_THREADS; i++) {
		j = (server_data->last_used + i)%MAX_THREADS;

		portals->vec[i] = strdup(server_data->tdata[j].portal);
		portals->vec_len++;
	}
	server_data->last_used = (server_data->last_used + 1)%MAX_THREADS;

	return portals;
}

/*---------------------------------------------------------------------------*/
/* portals_free								     */
/*---------------------------------------------------------------------------*/
static void portals_free(struct portals_vec *portals)
{
	int			i;
	for (i = 0; i < portals->vec_len; i++)
		free((char *)(portals->vec[i]));

	free(portals);
}

/*---------------------------------------------------------------------------*/
/* on_response_comp							     */
/*---------------------------------------------------------------------------*/
static int on_response_comp(struct xio_session *session,
			    struct xio_msg *rsp,
			    void *cb_user_context)
{
	struct raio_thread_data		*tdata = cb_user_context;
	struct raio_session_data	*session_data, *tmp_session_data;
	int				i = 0;

	TAILQ_FOREACH_SAFE(session_data, &tdata->server_data->sessions_list,
			   sessions_list_entry, tmp_session_data) {
		if (session_data->session == session) {
			for (i = 0; i < MAX_THREADS; i++) {
				if (session_data->portal_data[i].tdata ==
				    tdata) {
					/* process request */
					raio_handler_on_rsp_comp(
					  session_data->dd_data,
					  session_data->portal_data[i].dd_data,
					  rsp);
					return 0;
				}
			}
		}
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_session_disconnect						     */
/*---------------------------------------------------------------------------*/
static void raio_session_disconnect(struct raio_session_data *session_data)
{
	struct raio_connection_data	*connection_entry,
					*tmp_connection_entry;
	TAILQ_FOREACH_SAFE(connection_entry, &session_data->conns_list,
			   conns_list_entry,
			   tmp_connection_entry) {
		if (!connection_entry->disconnected) {
			connection_entry->disconnected = 1;
			xio_disconnect(connection_entry->connection);
		}
	}
}

/*---------------------------------------------------------------------------*/
/* on_request callback							     */
/*---------------------------------------------------------------------------*/
static int on_request(struct xio_session *session, struct xio_msg *req,
		      int more_in_batch, void *cb_user_context)
{
	struct raio_thread_data		*tdata = cb_user_context;
	struct raio_session_data	*session_data, *tmp_session_data;
	int				i, disconnect = 0;

	TAILQ_FOREACH_SAFE(session_data, &tdata->server_data->sessions_list,
			   sessions_list_entry, tmp_session_data) {
		if (session_data->session == session) {
			for (i = 0; i < MAX_THREADS; i++) {
				if (session_data->portal_data[i].tdata ==
				    tdata) {
					/* process request */
					disconnect = raio_handler_on_req(
					  session_data->dd_data,
					  session_data->portal_data[i].dd_data,
					  req);
					if (disconnect)
						raio_session_disconnect(
								session_data);
					return 0;
				}
			}
		}
	}
	fprintf(stdout, "session not found\n");


	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
static struct xio_session_ops  portal_server_ops = {
	.on_session_event		=  NULL,
	.on_new_session			=  NULL,
	.on_msg_send_complete		=  on_response_comp,
	.on_msg				=  on_request,
	.on_msg_error			=  NULL
};
/*---------------------------------------------------------------------------*/
/* worker thread callback						     */
/*---------------------------------------------------------------------------*/
static void *portal_server_cb(void *data)
{
	struct raio_thread_data	*tdata = data;
	cpu_set_t		cpuset;
	pthread_t		thread;
	struct xio_server	*server;

	/* set affinity to thread */
	thread = pthread_self();

	CPU_ZERO(&cpuset);
	CPU_SET(tdata->affinity, &cpuset);

	pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	/* create thread context for the client */
	tdata->ctx = xio_context_create(NULL, 0, tdata->affinity);

	/* bind a listener server to a portal/url */
	server = xio_bind(tdata->ctx, &portal_server_ops, tdata->portal,
			  NULL, 0, tdata);
	if (server == NULL) {
		fprintf(stderr, "failed to bind server\n");
		goto cleanup;
	}

	/* the default xio supplied main loop */
	xio_context_run_loop(tdata->ctx, XIO_INFINITE);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* detach the server */
	xio_unbind(server);

cleanup:
	/* free the context */
	xio_context_destroy(tdata->ctx);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* on_new_connection							     */
/*---------------------------------------------------------------------------*/
static int on_new_connection(struct xio_session *session,
			     struct xio_connection *connection,
			     void *cb_user_context)
{
	struct raio_server_data		*server_data = cb_user_context;
	struct raio_session_data	*session_entry;
	struct raio_connection_data	*connection_entry;

	TAILQ_FOREACH(session_entry, &server_data->sessions_list,
		      sessions_list_entry) {
		if (session_entry->session == session) {
			connection_entry = calloc(1, sizeof(*connection_entry));
			if (connection_entry == NULL)
				return -1;
			connection_entry->connection = connection;
			TAILQ_INSERT_HEAD(&session_entry->conns_list,
					  connection_entry, conns_list_entry);
			break;
		}
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_connection_teardown						     */
/*---------------------------------------------------------------------------*/
static int on_connection_teardown(struct xio_session *session,
				  struct xio_connection *connection,
				  void *cb_user_context)
{
	struct raio_server_data		*server_data = cb_user_context;
	struct raio_session_data	*session_entry;
	struct raio_connection_data	*connection_entry,
					*tmp_connection_entry;
	int				found = 0;

	TAILQ_FOREACH(session_entry, &server_data->sessions_list,
		      sessions_list_entry) {
		if (session_entry->session == session) {
			TAILQ_FOREACH_SAFE(
					connection_entry,
					&session_entry->conns_list,
					conns_list_entry,
					tmp_connection_entry) {
				if (connection_entry->connection ==
						connection) {
					TAILQ_REMOVE(&session_entry->conns_list,
						     connection_entry,
						     conns_list_entry);
					free(connection_entry);
					found = 1;
					break;
				}
			}
			break;
		}
	}
	if (found)
		xio_connection_destroy(connection);

	return 0;
}


/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
			    struct xio_session_event_data *event_data,
			    void *cb_user_context)
{
	struct raio_session_data *session_data, *tmp_session_data;
	struct raio_server_data	 *server_data = cb_user_context;
	int			 i;



	switch (event_data->event) {
	case XIO_SESSION_NEW_CONNECTION_EVENT:
		on_new_connection(session, event_data->conn, cb_user_context);
		break;
	case XIO_SESSION_CONNECTION_CLOSED_EVENT:
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		TAILQ_FOREACH_SAFE(session_data, &server_data->sessions_list,
				   sessions_list_entry, tmp_session_data) {
			if (session_data->session == session) {
				for (i = 0; i < MAX_THREADS; i++) {
					if (session_data->portal_data[i].tdata) {
						raio_handler_free_portal_data(
					   session_data->portal_data[i].dd_data);
					   session_data->portal_data[i].tdata =
						   NULL;
					   server_data->last_reaped = i;
					   break;
					}
				}
				raio_handler_free_session_data(
						session_data->dd_data);
				TAILQ_REMOVE(&server_data->sessions_list,
					     session_data,
					     sessions_list_entry);
				free(session_data);
				break;
			}
		}
		xio_session_destroy(session);
#if SIMULATE_DESTROY
		for (i = 0; i < MAX_THREADS; i++)
			xio_context_stop_loop(server_data->tdata[i].ctx, 0);
		xio_context_stop_loop(server_data->ctx, 0);
#endif
		break;
	case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
		on_connection_teardown(session, event_data->conn,
				       cb_user_context);
		break;
	default:
		printf("unexpected session event: session:%p, %s. reason: %s\n",
		       session,
		       xio_session_event_str(event_data->event),
		       xio_strerror(event_data->reason));
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_new_session							     */
/*---------------------------------------------------------------------------*/
static int on_new_session(struct xio_session *session,
			  struct xio_new_session_req *req,
			  void *cb_user_context)
{
	struct portals_vec *portals;
	struct raio_server_data *server_data = cb_user_context;
	struct raio_session_data *session_data;
	int i;

	portals = portals_get(server_data, req->uri, req->private_data);

	/* alloc and  and initialize */
	session_data = calloc(1, sizeof(*session_data));
	session_data->session = session;
	session_data->dd_data = raio_handler_init_session_data(MAX_THREADS);
	for (i = 0; i < MAX_THREADS; i++) {
		session_data->portal_data[i].tdata = &server_data->tdata[i];
		session_data->portal_data[i].dd_data =
			raio_handler_init_portal_data(
				session_data->dd_data,
				i,
				session_data->portal_data[i].tdata->ctx);
	}
	TAILQ_INSERT_TAIL(&server_data->sessions_list,
			  session_data, sessions_list_entry);

	/* automatic accept the request */
	xio_accept(session, portals->vec, portals->vec_len, NULL, 0);

	portals_free(portals);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
static struct xio_session_ops  server_ops = {
	.on_session_event		=  on_session_event,
	.on_new_session			=  on_new_session,
	.on_msg_send_complete		=  NULL,
	.on_msg				=  NULL,
	.on_msg_error			=  NULL
};

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	struct xio_server	*server;	/* server portal */
	struct raio_server_data	server_data;
	char			url[256];
	int			i;
	uint16_t		port = atoi(argv[2]);
	int			curr_cpu;
	int			max_cpus;

	if (argc < 3) {
		printf("Usage: %s <host> <port> <transport:optional>\n",
		       argv[0]);
		exit(1);
	}

	xio_init();

	curr_cpu = sched_getcpu();
	max_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	memset(&server_data, 0, sizeof(server_data));
	server_data.last_reaped = -1;
	TAILQ_INIT(&server_data.sessions_list);

	/* create thread context for the client */
	server_data.ctx	= xio_context_create(NULL, 0, curr_cpu);

	/* create url to connect to */
	if (argc > 3)
		sprintf(url, "%s://%s:%d", argv[3], argv[1], port);
	else
		sprintf(url, "rdma://%s:%d", argv[1], port);
	/* bind a listener server to a portal/url */
	server = xio_bind(server_data.ctx, &server_ops,
			  url, NULL, 0, &server_data);
	if (server == NULL) {
		fprintf(stderr, "failed to bind server\n");
		goto cleanup;
	}

	/* spawn portals */
	for (i = 0; i < MAX_THREADS; i++) {
		server_data.tdata[i].server_data = &server_data;
		server_data.tdata[i].affinity = (curr_cpu + i)%max_cpus;
		printf("[%d] affinity:%d/%d\n", i,
		       server_data.tdata[i].affinity, max_cpus);
		port += 1;
		if (argc > 3)
			sprintf(server_data.tdata[i].portal, "%s://%s:%d",
				argv[3], argv[1], port);
		else
			sprintf(server_data.tdata[i].portal, "rdma://%s:%d",
				argv[1], port);
		pthread_create(&server_data.thread_id[i], NULL,
			       portal_server_cb, &server_data.tdata[i]);
	}
	xio_context_run_loop(server_data.ctx, XIO_INFINITE);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* join the threads */
	for (i = 0; i < MAX_THREADS; i++)
		pthread_join(server_data.thread_id[i], NULL);

	/* free the server */
	xio_unbind(server);
cleanup:
	/* free the context */
	xio_context_destroy(server_data.ctx);

	xio_shutdown();

	return 0;
}

