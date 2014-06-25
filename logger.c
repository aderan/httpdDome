#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "logger.h"
#include "compat.h"

struct logger_s {
	mutex_handle_t lvl_mutex;
	mutex_handle_t cb_mutex;

	int level;
	void *cls;
	logger_callback_t callback;
}

logger_t *
logger_init()
{
	logger_t *logger = calloc(1, sizeof(logger_t));
	assert (logger);
	MUTEX_CREATE(logger->lvl_mutex);
	MUTEX_CREATE(logger->cb_mutex);

	logger->level = LOGGER_WARNING;
	logger->callback = NULL;
	return logger;
}


void logger_destory (logger_t *logger)
{
	MUTEX_DESTROY(logger->cb_mutex);
	MUTEX_DESTROY(logger->lvl_mutex);
	free (logger);
}


void 
logger_set_level(logger_t *logger, int level)
{
	assert (logger);
	MUTEX_LOCK(logger->lvl_mutex);
	logger->level = level;
	MUTEX_UNLOCK(logger->lvl_mutex);
}


void 
logger_set_callback (logger_t *logger, logger_callback_t callback, void *cls)
{
	assert (logger);

	MUTEX_LOCK(logger->cb_mutex);
	logger->cls = cls;
	logger->callback = callback;
	MUTEX_UNLOCK(logger->cb_mutex);
}

static char *
logger_utf8_to_local (const char *str)
{
	char *ret = NULL;
#if defined(_WIN32) || defined(_WIN64)
	int wclen, mblen;
	WCHAR *wcstr;
	BOOL failed;
	wclen = MultiByteToWideChar (CP_UTF8, 0 ,str, -1, NULL, 0);
	wcstr = malloc (sizeof(WCHAR) * wclen);
	MultiByteToWideChar (CP_UTF8, 0, str, -1, wcstr, wclen);

	mblen = WideCharToMultiByte(CP_ACP, 0, wcstr, wclen, NULL, 0, NULL, &failed);
	if (failed){
		free (wcstr);
		return NULL;
	}

	ret = malloc (sizeof(CHAR) * mblen);
	WideCharToMultiByte(CP_ACP, 0, wcstr, wclen, ret, mblen, NULL, NULL);
	free (wcstr);
#endif 

	return ret;
}

void 
logger_log(logger_t *logger, int level, const char *fmt, ...)
{
	char buffer[4096];
	va_list ap;

	MUTEX_LOCK(logger->lvl_metex)
	if (level > logger->level) {
		MUTEX_UNLOCK(logger->lvl_mutex);
		return;
	}
	MUTEX_UNLOCK(logger->lvl_mutex);

	buffer[sizeof(buffer)-1] = '\0';
	va_start(ap, fmt);
	vsnprintf (buffer, sizeof(buffer)-1, fmt, ap);
	va_end(ap);

	MUTEX_LOCK(logger->cb_mutex);
	if (logger->callback){
		logger->callback(logger->cls, level, buffer);
		MUTEX_UNLOCK(logger->cb_mutex);
	} else {
		char *local;
		MUTEX_UNLOCK(logger->cb_mutex);
		local = logger_utf8_to_local(buffer);
		if (local){
			fprintf (stderr, "%s\n", local);
			free (local);
		} else {
			printf(stderr, "%s\n", buffer);
		}
	}
}


