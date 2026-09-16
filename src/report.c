/* SPDX-License-Identifier: MIT
 *
 * Verbose progress reporting.
 *
 * Reporting is off by default, so a successful command is silent in the
 * usual Unix way.  When it is on, every step that the library performs
 * announces itself in one short line.
 *
 * Two rules govern what may be reported:
 *
 *   - Lines go to the report stream (stderr by default), never to stdout.
 *     `web show`, `web variables` and `web config` write data to stdout that
 *     a user may pipe into another program, and progress notes must not end
 *     up in that pipe.
 *
 *   - Lines describe the operation in the vocabulary of the user interface:
 *     names, URLs, browsers, locks.  Internal mechanics -- temporary file
 *     names, file descriptors, the on-disk tagging of the browser field,
 *     function names, errno values -- are deliberately not reported.  A
 *     verbose run explains what the tool did, not how it is built.
 */
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

static int   verbose_enabled;
static FILE *report_stream;

void webdoc_set_verbose(int enabled)
{
    verbose_enabled = enabled ? 1 : 0;
}

int webdoc_verbose(void)
{
    return verbose_enabled;
}

void webdoc_set_report_stream(FILE *stream)
{
    report_stream = stream;
}

void webdoc_report(const char *fmt, ...)
{
    FILE *out = report_stream ? report_stream : stderr;
    va_list ap;

    if (!verbose_enabled || !fmt)
        return;

    fputs("web: ", out);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}
