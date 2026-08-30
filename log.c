#include <stdio.h>
#include <stdarg.h>

int log_quiet = 0;

void logp(const char *format, ...) {
    va_list ap;

    if(!log_quiet) {
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}
