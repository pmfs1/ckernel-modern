/*
 * snprintf()
 *
 * Implement snprintf() in terms of vsnprintf()
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "aslib.h"

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    int rv;

    va_start(ap, format);
    rv = vsnprintf(str, size, format, ap);
    va_end(ap);

    return rv;
}
