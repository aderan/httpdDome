#ifndef COMPAT_H
#define COMPAT_H

#if defined(WIN32)
#include <ws2tcpip.h>
#include <windows.h>
#ifndef snprintf
#define snprintf _snprintf
#endif
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#endif

#include "memalign.h"
#include "sockets.h"
#include "threads.h"

#endif
