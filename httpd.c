#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "httpd.h"
#include "netutils.h"
#include "http_request.h"
#include "compat.h"
#include "logger.h"

struct http_connection_s {
	int connected;
	int socket_fd;
	void *user_data;
	http_request_t *request;
};

struct httpd_s{
	int running;
	int joined;
	thread_handle_t http_thread;
	mutex_handle_t run_mutex;
	
	logger_t *logger;
	httpd_callback_t callbacks;
	
	int open_connections;
	int max_connections;
	http_connection_t *connections;

	int server_fd4;
	int server_fd6;
}


httpd_t *
httpd_init(logger_t *logger, httpd_callbacks_t *callbacks, max_connections)
{
	httpd_t *httpd;
	
	assert (logger);
	assert (callbacks);
	assert (max_connections > 0);

	httpd = calloc (1, sizeof(httpd_t));
	//care for return
	if (!httpd) {
		return NULL;
	}

	httpd->max_connections = max_connections;
	httpd->connections = calloc (max_connections, sizeof(httpd_connection_t));
	if (!httpd->connections){
		free(httpd);
		return NULL;
	}
	
	//memcpy dec src n;
	memcpy(&httpd->callbacks, callbacks, sizeof(httpd_callbacks_t));
	httpd->logger = logger;
	//# define PTHREAD_MUTEX_INITIALIZER \
	//  { { 0, 0, 0, 0, 0, __PTHREAD_SPINS, { 0, 0 } } }
	//MUTEX_CREATE(httpd->run_mutex);
	
	httpd->running = 0;
	httpd->joined = 1;

	return httpd;
}

void 
httpd_destroy (httpd_t *httpd)
{
	if (httpd){
		httpd_stop (httpd);

		free (httpd->connections);
		free (httpd);
	}
}

static void 
httpd_add_connection(httpd_t *httpd, int fd, unsigned char *local, int local_len, unsigned char *remote, int remote_len)
{
	int i;
	for (i=0; i<httpd->max_connections; i++) 
		if (!httpd->connections[i].connected)
			break;

	if (i == httpd->max_connections) {
		logger_log (httpd->logger, LOGGER_INFO, "Max Connections");
		shutdown (fd, SHUT_RDWR);
		closesocket(fd);
		return;
	}

	httpd->open_connections++;
	httpd->connections[i].socket_fd = fd;
	httpd->connections[i].connected = 1;
	httpd->connections[i].user_date = httpd->callbacks.conn_init(httpd->callbacks.opaque, local, local_len, remote, remote_len);
}

static int 
httpd_accept_connection(httpd_t *httpd, int server_fd, int is_ipv6)
{
	struct sockaddr_storage remote_saddr;
	socklen_t remote_saddrlen;
	struct sockaddr_storage local_saddr;
	socklen_t local_saddrlen;
	unsigned char *local, *remote;
	//change ret to err; for use if (err)
	int err, fd;

	remote_saddrlen = sizeof(remote_saddr);
	fd = accept(server_fd, (struct sockaddr *)&remote_saddr, &remote_len);
	if (fd == -1)
		return -1;

	local_saddrlen = sizeof(local_saddr);
	err = getsockname (fd, (struct sockaddr *)&local_saddr, &local_saddrlen);
	if (err == -1){
		closesocket(fd);
		return -2;
	}

	logger_log(httpd->logger, LOGGER_INFO, "Accepted %s client on Socket %d",
				(is_ipv6 ? "IPv6" : "IPv4"), fd);
	local = netutils_get_address (&local_saddr, &local_len);
	remote = netutils_get_address (&remote_saddr, &remote_len);

	httpd_add_connection(httpd, fd, local, local_len, remote, remote_len);
	return 0;
}


static void 
httpd_remove_connection (httpd_t *httpd, http_connection_t *connection)
{
	if (connection->request) {
		http_request_destory (connection->request);
		connection->request = NULL;
	}
	httpd->callbacks.conn_destory(connection->user_data);
	shutdown (connection->socket_fd, SHUT_WR);
	closesocket(connection->socket_fd);
	connection->connected = 0;
	httpd->open_connections--;
}

int
httpd_start (httpd_t *httpd, unsigned short *port)
{
	assert (httpd);
	assert (port);

	MUTEX_LOCK(httpd->run_mutex);
	if (httpd->running || !httpd->joined) {
		MUTEX_UNLOCK(httpd->run_mutex)
		return 0;
	}

	httpd->server_fd4 = netutils_init_socket(port, 0 ,0);
	if (httpd->server_fd4 < 0) {
		logger_log (httpd->logger, LOGGER_ERR, "Error init socket %d", SOCKET_GET_ERROR());
		MUTEX_UNLOCK (httpd->run_mutex);
		return -1;
	}
	httpd->server_fd6 = netutils_init_socket(port, 1, 0);
	if (httpd->server_fd6 < 0) {
		logger_log(httpd->logger, LOGGER_WARNING, "Error init IPv6 socket %d", SOCKET_GET_ERROR());
	}

	if (listen (httpd->server_fd4, 5) < 0) {
		logger_log (httpd->logger, LOGGER_ERR, "Error listening to IPv4");
		closesocket (httpd->server_fd4);
		//?if hava to close? can ,but ?
		closesocket (httpd->server_fd6);
		MUTEX_UNLOCK(httpd->run_mutex);
		return -2;
	}
	if (httpd->server_fd6 > 0 && listen(httpd->server_fd6, 5) < 0){
		logger_log(httpd->logger, LOGGER_ERR, "Error listening to IPv6");
		closesocket(httpd->server_fd4);
		closesocket(httpd->server_fd6);
		MUTEX_UNLOCK(httpd->run_mutex);
		return -2;
	}
	logger_log(httpd->logger, LOGGER_INFO, "Server Socket Init");

	httpd->running = 1;
	httpd->join = 0;
	THREAD_CREATE (httpd->thread, http_thread, httpd);
	MUTEX_UNLOCK (httpd->run_mutex);
	//Change ret 1 to 0
	return 0;
}

int 
httpd_is_running(httpd_t *httpd)
{
	int running;
	assert (httpd);
	//?if need to lock? ReadWriteMutex Need
	MUTEX_LOCK (httpd->run_mutex);
	running = httpd->running || !httpd->join;
	MUTEX_UNLOCK(httpd->run_mutex);

	return running;
}

void 
httpd_stop (httpd_t *httpd)
{
	assert (httpd);

	MUTEX_LOCK(httpd->run_mutex);
	if (!httpd->running || httpd->joined) {
		MUTEX_UNLOCK (httpd->run_mutex);
		return;
	}
	http->running = 0;
	MUTEX_UNLOCK(httpd->run_mutex);

	THREAD_JOIN(httpd->thread);
	MUTEX_LOCK(httpd->run_mutex);
	httpd->joined = 1;
	MUTEX_UNLOCK(httpd->run_mutex);
}
