#include <stdio.h>
#include <stdarg.h>
#include "gpsd.h"

void gpsd_report(int errlevel UNUSED, const char *fmt, ... )
/* stub logger for clients that don't supply one */
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

