/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Prints exactly one document.
 *
 * A directory is refused rather than quietly listed: `show` answers "what is
 * in this document", and a command whose output shape changes with the type
 * of its argument is awkward to script against.  Enumerating a directory is
 * a different question, answered by webdoc_list_dir().
 */
webdoc_err webdoc_show(const char *path, FILE *out)
{
    webdoc_t doc;
    struct stat st;
    char bbuf[WEBDOC_MAX_PATH + 16];
    const char *bs;
    int global_locked = 0;
    webdoc_err rc;

    if (!path || !out)
        return WEBDOC_ERR_INVALID_ARG;

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return WEBDOC_ERR_IS_DIRECTORY;

    rc = webdoc_load(path, &doc);
    if (rc != WEBDOC_OK)
        return rc;

    webdoc_report("read %s", path);
    (void)webdoc_global_lock_get(&global_locked);

    fprintf(out, "path:    %s\n", path);
    fprintf(out, "version: %d\n", doc.version);
    fprintf(out, "url:     %s\n", doc.url);

    /* Presented the way the user wrote it, not the way it is stored: the
       "variable:" / "path:" tagging is a file format detail. */
    (void)bbuf;
    (void)bs;
    switch (doc.browser_kind) {
    case WEBDOC_BROWSER_VARIABLE: {
        char *resolved = NULL;

        if (webdoc_get_variable(doc.browser_ref, &resolved) == WEBDOC_OK) {
            fprintf(out, "browser: %s (%s)\n", doc.browser_ref, resolved);
            free(resolved);
        } else {
            fprintf(out, "browser: %s (not defined)\n", doc.browser_ref);
        }
        break;
    }
    case WEBDOC_BROWSER_PATH:
        fprintf(out, "browser: %s\n", doc.browser_ref);
        break;
    case WEBDOC_BROWSER_DEFAULT:
        fprintf(out, "browser: system default\n");
        break;
    }

    fprintf(out, "locked:  %s%s\n", doc.locked ? "yes" : "no",
            global_locked ? " (global lock active)" : "");

    webdoc_reset(&doc);
    return WEBDOC_OK;
}

/* --------------------------------------------------------------- listing -- */

void webdoc_list_free(webdoc_list *list)
{
    size_t i;

    if (!list)
        return;
    for (i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].path);
        free(list->items[i].url);
        free(list->items[i].browser);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int entry_cmp(const void *a, const void *b)
{
    const webdoc_entry *ea = a, *eb = b;

    return strcmp(ea->name, eb->name);
}

/*
 * Equivalent in spirit to a shell's "ls -l DIR" restricted to .web files,
 * implemented with opendir/readdir/stat.  No shell, no glob expansion, no `ls` process: a
 * directory full of files with adversarial names cannot turn into arguments
 * of another command.
 */
webdoc_err webdoc_list_dir(const char *dir, webdoc_list *out)
{
    DIR *d;
    struct dirent *de;
    webdoc_err rc = WEBDOC_OK;

    if (!dir || !out)
        return WEBDOC_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    d = opendir(dir);
    if (!d)
        return webdoc_errno_map(errno);
    webdoc_report("scanning %s", dir);

    while ((de = readdir(d)) != NULL) {
        char path[WEBDOC_MAX_PATH];
        struct stat st;
        webdoc_entry *grown, *e;
        webdoc_t doc;
        size_t namelen;
        char bbuf[WEBDOC_MAX_PATH + 16];
        const char *bs;

        if (!webdoc_str_has_suffix(de->d_name, WEBDOC_EXT))
            continue;
        if (de->d_name[0] == '.')          /* skip hidden files, like ls */
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
            (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;

        grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
        if (!grown) {
            rc = WEBDOC_ERR_NOMEM;
            goto out;
        }
        out->items = grown;
        e = &out->items[out->count];
        memset(e, 0, sizeof(*e));

        namelen = strlen(de->d_name) - strlen(WEBDOC_EXT);
        e->name = malloc(namelen + 1);
        e->path = strdup(path);
        if (!e->name || !e->path) {
            free(e->name);
            free(e->path);
            rc = WEBDOC_ERR_NOMEM;
            goto out;
        }
        memcpy(e->name, de->d_name, namelen);
        e->name[namelen] = '\0';

        e->mode = st.st_mode;
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->uid = st.st_uid;
        e->gid = st.st_gid;

        e->status = webdoc_load(path, &doc);
        if (e->status == WEBDOC_OK) {
            e->url = strdup(doc.url);
            e->locked = doc.locked;
            bs = webdoc_browser_string(&doc, bbuf, sizeof(bbuf));
            if (bs)
                e->browser = strdup(bs);
            webdoc_reset(&doc);
        }
        out->count++;
    }

    if (out->count > 1)
        qsort(out->items, out->count, sizeof(*out->items), entry_cmp);
    webdoc_report("found %zu document%s", out->count,
                  out->count == 1 ? "" : "s");

out:
    closedir(d);
    if (rc != WEBDOC_OK)
        webdoc_list_free(out);
    return rc;
}
