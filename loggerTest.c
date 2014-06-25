#include <stdio.h>
#include "logger.h"

void cb(void *cls, int level, const char *msg)
{
	printf ("cb msg is %s", msg);
}

int main(int argc, char **argv)
{
	logger_t *lg_test;
	lg_test = logger_init();
	logger_set_level (lg_test, LOGGER_DEBUG);
	logger_set_callback(lg_test, cb, NULL);

	logger_log(lg_test, LOGGER_DEBUG, "test %s", "Hello world\n" );
	return 0;
}
