#ifndef LOGGER_H
#define LOGGER_H

#define LOGGER_EMERG 0
#define LOGGER_ALERT 1
#define LOGGER_CRIT  2
#define LOGGER_ERR   3
#define LOGGER_WARNING 4
#define LOGGER_NOTICE  5
#define LOGGER_INFO    6
#define LOGGER_DEBUG   7

typedef void (*logger_callback_t)(void *cls, int level, const char *msg);

typedef struct logger_s logger_t;

logger_t* logger_init();
void logger_destroy(logger_t *logger);

void logger_set_level(logger_t *logger, int level);
void logger_set_callback(logger_t *logger, logger_callback_t callback, void *cls);

void logger_log(logger_t *logger, int level, const char*fmt, ...);

#endif
