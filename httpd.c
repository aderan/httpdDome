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

static THREAD_RETVAL
httpd_thread(void *arg)
{
	httpd_t *httpd = arg;
	char buffer[1024];
	int i;

	assert(httpd);

	while (1) {
		fd_set rfds;
		struct timeval tv;
		int nfds=0;
		int ret;

		MUTEX_LOCK (httpd->run_mutex);
		if (!httpd->running){
			MUTEX_UNLOCK(httpd->run_mutex);
			break;
		}
		MUTEX_UNLOCK (httpd->run_mutex);

		tv.tv_sec = 0;
		tv.tv_usec = 5000;
		
		FD_ZERO(&rfds);
		if (httpd->open_connections < httpd->max_connections) {
			FD_SET(httpd->server_fd4, &rfds);
			nfds = httpd->server_fd4+1;
			//Should not use magic num;
			if (httpd->server_fd6 != -1) {
				FD_SET(httpd->server_fd6, &rfds);
				if (nfds <= httpd->server_fd6) 
					nfds = httpd->server_fd6 + 1;
			}
		}

		for (i=1; i< httpd->max_connections; i++){
			int socket_fd;
			if (!httpd->connections[i].connected) 
				continue;
			socket_fd = httpd->connections[i].socket_fd;
			FD_SET(socket_fd, &rfds);
			if (nfds <= socket_fd) {
				nfds = socket_fd + 1;
		}

		ret = select (nfds, &rfds, NULL, NULL, &tv);
		if (ret == 0){
			continue;
		} else if (ret == -1) {
			logger_log(httpd->logger, LOGGER_INFO, "Error in select");
			break;
		}

		if (FD_ISSET(httpd->server_fd4, &rfds)) {
			ret = httpd_accept_connection(httpd, httpd->server_fd4, 0);
			if (ret == -1)
				break;
			else if (ret < 0)
				continue;
		}
		for (i=0;i<httpd->max_connections; i++){
			http_connections_t *connection = &httpd->connections[i];

			if (!connection->connected)
				continue;
			if (!FD_ISSET(connection->socket_fd, &rfds))
				continue;

			if (!connection->request) {
				connection->request = http_request_init();
				assert (connection->request);
			}

			logger_log(httpd->logger, LOGGER_DEBUG, "Receiving on socket %d", connection->socket_fd);
			ret = recv(connection->socket_fd, buffer, sizeof(buffer) ,0);
			if (ret == 0){
				logger_log(httpd->logger, LOGGER_INFO, "Connection Close for socket %d", connection->socket_fd);
				httpd_remove_connection(httpd, connection);
				continue;
			}

			if (!strncmp (buffer, "HTTP/", strlen("HTTP/"))) {
				logger_log(httpd->logger, LOGGER_INFO, "Got reverse resones");
				continue;
			}

			http_request_add_data(connection->request, buffer, ret);
			if (http_request_has_error(connection->request)) {
				logger_log(httpd->logger, LOGGER_INFO, "Error in parsing: %s", httpd_request_get_error_name(connection->request));
				httpd_remove_conncetion(httpd, connection);
				continue;
			}

			if (http_request_is_complete(connection->request)){
				http_response_t *response = NULL;
				int datalen;
			}

			httpd->callbacks.conn_request(connection->user_data, connections->socket_fd, connection->request, &response);
			http_request_destroy(connection->request);
			connection->request = NULL;
			
			if (response) {
				const char *data;
				int datalen;
				int written;
				int ret;

				data = http_response_get_data(response, &datalen);

				written = 0;
				while (written < datalen){
					ret = send(connection->socket_fd, data+written, datalen-written, 0);
					if (ret == -1){
						logger_log(httpd->logger, LOGGER_INFO, "Error int sending data");
						break;
					}

					written += ret;
				}

				if (http_response_get_disconect(response)) {
					logger_log (httpd->logger, LOGGER_INFO, "Disconnection on software request");
					httpd_remove_connection(httpd, connection);
				}else {
					logger_log(httpd->logger, LOGGER_INFO, "Didn't get responese");
				}
				http_response_destroy (response);
			} else{
				logger_log(httpd->logger, LOGGER_DEBUG, "Request not complete, waiting for more data...");
			}
		}
	}

	for (i=0; i< httpd->max_connections; i++) {
		http_connection_t *connection = &httpd->connections[i];
		
		//Use Tmp make line Low
		if (!connection->connected) 
			continue;
		logger_log (httpd->logger, LOGGER_INFO, "Removeing connection for socket %d", connection->socket_fd);
		httpd_remove_connection(httpd, connection);
	}

	if (httpd->server_fd4 != -1) {
		closesocket(httpd->server_fd4);
		httpd->server_fd4 = -1;
	}

	if (httpd->server_fd6 != -1){
		closesocket(httpd->server_fd6);
		httpd->server_fd6 = -1;
	}

	logger_log (httpd->logger, LOGGER_INFO, "Exiting HTTP thread");

	return 0;
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
