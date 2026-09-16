/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * The one and only opening pipeline.  `web open`, the .desktop MIME handler
 * and any future front end all enter here, so lock checks can never be
 * bypassed by opening a document "a different way".
 *
 *   load .web -> validate URL -> global lock -> per-document lock ->
 *   resolve browser -> validate executable -> safe spawn
 *
 * Each clause reports itself when verbose reporting is on.  What it reports
 * is the decision, not the mechanism: which browser was chosen, that the
 * locks were clear, that the browser was started detached.  The browser's
 * own chatter is never part of it -- see webdoc_spawn_detached().
 */
webdoc_err webdoc_open(const char *path)
{
    webdoc_t doc;
    char exe[WEBDOC_MAX_PATH];
    int global_locked = 0, use_default = 0;
    webdoc_err rc;

    if (!path)
        return WEBDOC_ERR_INVALID_ARG;

    rc = webdoc_load(path, &doc);         /* also validates the URL */
    if (rc != WEBDOC_OK)
        return rc;
    webdoc_report("read %s", path);
    webdoc_report("address %s", doc.url);

    rc = webdoc_global_lock_get(&global_locked);
    if (rc != WEBDOC_OK)
        goto out;

    /* Two independent scopes.  Either one denies the open; neither clears
       the other. */
    if (global_locked || doc.locked) {
        if (global_locked)
            webdoc_report("refused: the global lock is active");
        if (doc.locked)
            webdoc_report("refused: this document is locked");
        rc = WEBDOC_ERR_LOCKED;
        goto out;
    }
    webdoc_report("lock check passed");

    rc = webdoc_browser_resolve(&doc, exe, sizeof(exe), &use_default);
    if (rc != WEBDOC_OK) {
        if (rc == WEBDOC_ERR_VAR_NOT_FOUND)
            webdoc_report("browser variable '%s' is not defined",
                          doc.browser_ref ? doc.browser_ref : "");
        goto out;
    }

    if (use_default) {
        /* No browser configured: hand over to the desktop's own default
           handler.  We never change the user's system default ourselves. */
        webdoc_report("browser: system default");
        rc = webdoc_spawn_detached("xdg-open", 1, doc.url);
    } else {
        if (doc.browser_kind == WEBDOC_BROWSER_VARIABLE)
            webdoc_report("browser: %s (%s)", doc.browser_ref, exe);
        else
            webdoc_report("browser: %s", exe);

        rc = webdoc_browser_validate_exe(exe);
        if (rc != WEBDOC_OK) {
            webdoc_report("browser rejected: %s", webdoc_strerror(rc));
            goto out;
        }
        webdoc_report("browser checks passed");
        rc = webdoc_spawn_detached(exe, 0, doc.url);
    }
    if (rc == WEBDOC_OK)
        webdoc_report("browser started and detached from this terminal");

out:
    webdoc_reset(&doc);
    return rc;
}
