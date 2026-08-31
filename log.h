#include <stdio.h>

extern int log_quiet;

void logp(const char *format, ...);

#define LOG(fmt, args...) logp(fmt, ##args)
