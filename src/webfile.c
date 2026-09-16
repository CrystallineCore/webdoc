/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

void webdoc_init(webdoc_t *doc)
{
    if (doc)
        memset(doc, 0, sizeof(*doc));
}

void webdoc_reset(webdoc_t *doc)
{
    if (!doc)
        return;
    free(doc->url);
    free(doc->browser_ref);
    free(doc->extra_json);
    memset(doc, 0, sizeof(*doc));
}

const char *webdoc_browser_string(const webdoc_t *doc, char *buf, size_t buflen)
{
    if (!doc || !buf || doc->browser_kind == WEBDOC_BROWSER_DEFAULT ||
        !doc->browser_ref)
        return NULL;
    if (snprintf(buf, buflen, "%s:%s",
                 doc->browser_kind == WEBDOC_BROWSER_VARIABLE ? "variable" : "path",
                 doc->browser_ref) >= (int)buflen)
        return NULL;
    return buf;
}

/* ------------------------------------------------------------- resolution -- */

webdoc_err webdoc_resolve_name(const char *reference, int explicit_path,
                           char *out, size_t outlen)
{
    char expanded[WEBDOC_MAX_PATH];
    webdoc_err rc;
    size_t len;
    int n;

    if (!reference || !reference[0] || !out)
        return WEBDOC_ERR_INVALID_ARG;

    if (explicit_path) {
        /* --path: the argument is a filesystem path, used verbatim. */
        if (snprintf(out, outlen, "%s", reference) >= (int)outlen)
            return WEBDOC_ERR_TOO_LARGE;
        return WEBDOC_OK;
    }

    rc = webdoc_expand_tilde(reference, expanded, sizeof(expanded));
    if (rc != WEBDOC_OK)
        return rc;

    len = strlen(expanded);
    if (len == 0 || expanded[len - 1] == '/')
        return WEBDOC_ERR_INVALID_ARG;

    if (webdoc_str_has_suffix(expanded, WEBDOC_EXT))
        n = snprintf(out, outlen, "%s", expanded);
    else
        n = snprintf(out, outlen, "%s%s", expanded, WEBDOC_EXT);

    if (n < 0 || (size_t)n >= outlen)
        return WEBDOC_ERR_TOO_LARGE;
    return WEBDOC_OK;
}

/* ------------------------------------------------------------------ load -- */

static webdoc_err parse_browser_field(const char *s, webdoc_t *doc)
{
    const char *val;

    if (strncmp(s, "variable:", 9) == 0) {
        val = s + 9;
        if (webdoc_variable_name_validate(val) != WEBDOC_OK)
            return WEBDOC_ERR_INVALID_BROWSER;
        doc->browser_kind = WEBDOC_BROWSER_VARIABLE;
    } else if (strncmp(s, "path:", 5) == 0) {
        val = s + 5;
        if (val[0] != '/' || strlen(val) >= WEBDOC_MAX_PATH)
            return WEBDOC_ERR_INVALID_BROWSER;
        doc->browser_kind = WEBDOC_BROWSER_PATH;
    } else {
        return WEBDOC_ERR_INVALID_BROWSER;
    }
    doc->browser_ref = strdup(val);
    return doc->browser_ref ? WEBDOC_OK : WEBDOC_ERR_NOMEM;
}

static int is_known_key(const char *k)
{
    return strcmp(k, "version") == 0 || strcmp(k, "url") == 0 ||
           strcmp(k, "browser") == 0 || strcmp(k, "locked") == 0;
}

/* Serialises members we do not know about, so that `web edit` on a file
   written by a future (same-version) writer does not silently drop data. */
static webdoc_err collect_extras(const json_value *root, webdoc_t *doc)
{
    json_buf b;
    size_t i;
    int first = 1;

    json_buf_init(&b);
    for (i = 0; i < root->u.object.count; i++) {
        const json_member *m = &root->u.object.items[i];

        if (is_known_key(m->key))
            continue;
        if (!first)
            json_buf_appendz(&b, ",\n  ");
        first = 0;
        json_escape_into(&b, m->key);
        json_buf_appendz(&b, ": ");
        json_serialize(m->value, &b);
    }
    if (b.failed) {
        json_buf_free(&b);
        return WEBDOC_ERR_NOMEM;
    }
    if (first) {
        json_buf_free(&b);
        doc->extra_json = NULL;
        return WEBDOC_OK;
    }
    doc->extra_json = b.data;      /* ownership transfers */
    return WEBDOC_OK;
}

webdoc_err webdoc_load(const char *path, webdoc_t *out)
{
    char *buf = NULL;
    size_t len = 0;
    json_value *root = NULL;
    const json_value *v;
    const char *perr = NULL;
    webdoc_err rc;

    if (!path || !out)
        return WEBDOC_ERR_INVALID_ARG;
    webdoc_init(out);

    rc = webdoc_read_file(path, WEBDOC_MAX_FILE_SIZE, &buf, &len);
    if (rc != WEBDOC_OK)
        return rc;

    if (json_parse(buf, len, &root, &perr) != 0) {
        free(buf);
        return WEBDOC_ERR_INVALID_FILE;
    }
    free(buf);

    if (root->type != JSON_OBJECT) {
        rc = WEBDOC_ERR_INVALID_FILE;
        goto out;
    }

    v = json_object_get(root, "version");
    if (!v || v->type != JSON_NUMBER || v->u.number < 1 ||
        v->u.number != (double)(int)v->u.number) {
        rc = WEBDOC_ERR_INVALID_FILE;
        goto out;
    }
    out->version = (int)v->u.number;
    if (out->version > WEBDOC_FILE_VERSION) {
        rc = WEBDOC_ERR_UNSUPPORTED_VERSION;
        goto out;
    }

    v = json_object_get(root, "url");
    if (!v || v->type != JSON_STRING) {
        rc = WEBDOC_ERR_INVALID_FILE;
        goto out;
    }
    rc = webdoc_url_validate(v->u.string);
    if (rc != WEBDOC_OK)
        goto out;
    out->url = strdup(v->u.string);
    if (!out->url) {
        rc = WEBDOC_ERR_NOMEM;
        goto out;
    }

    v = json_object_get(root, "browser");
    if (v) {
        if (v->type != JSON_STRING) {
            rc = WEBDOC_ERR_INVALID_FILE;
            goto out;
        }
        rc = parse_browser_field(v->u.string, out);
        if (rc != WEBDOC_OK)
            goto out;
    }

    v = json_object_get(root, "locked");
    if (v) {
        if (v->type != JSON_BOOL) {
            rc = WEBDOC_ERR_INVALID_FILE;
            goto out;
        }
        out->locked = v->u.boolean;
    }

    rc = collect_extras(root, out);

out:
    json_free(root);
    if (rc != WEBDOC_OK)
        webdoc_reset(out);
    return rc;
}

/* ------------------------------------------------------------ serialising - */

static webdoc_err doc_serialize(const webdoc_t *doc, json_buf *b)
{
    char bbuf[WEBDOC_MAX_PATH + 16];
    const char *bs;

    json_buf_init(b);
    json_buf_appendz(b, "{\n  \"version\": ");
    json_buf_appendz(b, "1");
    json_buf_appendz(b, ",\n  \"url\": ");
    json_escape_into(b, doc->url);

    bs = webdoc_browser_string(doc, bbuf, sizeof(bbuf));
    if (bs) {
        json_buf_appendz(b, ",\n  \"browser\": ");
        json_escape_into(b, bs);
    }
    /* Optional fields with no value are omitted, not defaulted. */
    if (doc->locked)
        json_buf_appendz(b, ",\n  \"locked\": true");

    if (doc->extra_json) {
        json_buf_appendz(b, ",\n  ");
        json_buf_appendz(b, doc->extra_json);
    }
    json_buf_appendz(b, "\n}\n");

    if (b->failed) {
        json_buf_free(b);
        return WEBDOC_ERR_NOMEM;
    }
    return WEBDOC_OK;
}

webdoc_err webdoc_save(const char *path, const webdoc_t *doc)
{
    json_buf b;
    struct stat st;
    mode_t mode = 0644;
    webdoc_err rc;

    if (!path || !doc || !doc->url)
        return WEBDOC_ERR_INVALID_ARG;
    rc = webdoc_url_validate(doc->url);
    if (rc != WEBDOC_OK)
        return rc;

    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        mode = st.st_mode & 07777;          /* preserve existing permissions */

    rc = doc_serialize(doc, &b);
    if (rc != WEBDOC_OK)
        return rc;
    rc = webdoc_write_atomic(path, b.data, b.len, mode);
    json_buf_free(&b);
    if (rc == WEBDOC_OK)
        webdoc_report("saved %s", path);
    return rc;
}

/* ---------------------------------------------------------------- create -- */

webdoc_err webdoc_create(const char *path, const webdoc_create_params *params)
{
    webdoc_t doc;
    json_buf b;
    int fd = -1, dfd;
    char dirbuf[WEBDOC_MAX_PATH];
    char *slash;
    webdoc_err rc;
    size_t off;

    if (!path || !params || !params->url)
        return WEBDOC_ERR_INVALID_ARG;

    rc = webdoc_url_validate(params->url);
    if (rc != WEBDOC_OK)
        return rc;
    webdoc_report("address accepted: %s", params->url);

    webdoc_init(&doc);
    doc.version = WEBDOC_FILE_VERSION;
    doc.url = strdup(params->url);
    if (!doc.url)
        return WEBDOC_ERR_NOMEM;
    doc.locked = params->locked ? 1 : 0;
    doc.browser_kind = params->browser_kind;
    if (params->browser_kind != WEBDOC_BROWSER_DEFAULT) {
        if (!params->browser_ref) {
            rc = WEBDOC_ERR_INVALID_ARG;
            goto out_doc;
        }
        if (params->browser_kind == WEBDOC_BROWSER_VARIABLE) {
            rc = webdoc_variable_name_validate(params->browser_ref);
            if (rc != WEBDOC_OK)
                goto out_doc;
        } else if (params->browser_ref[0] != '/') {
            rc = WEBDOC_ERR_INVALID_BROWSER;
            goto out_doc;
        }
        doc.browser_ref = strdup(params->browser_ref);
        if (!doc.browser_ref) {
            rc = WEBDOC_ERR_NOMEM;
            goto out_doc;
        }
    }

    rc = doc_serialize(&doc, &b);
    if (rc != WEBDOC_OK)
        goto out_doc;

    /*
     * O_EXCL|O_CREAT is the atomic "must not already exist" primitive: there
     * is no TOCTOU window between a stat() and the create, and O_NOFOLLOW
     * additionally refuses to be redirected through a pre-planted symlink.
     */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
        rc = (errno == ELOOP) ? WEBDOC_ERR_EXISTS : webdoc_errno_map(errno);
        goto out_buf;
    }

    off = 0;
    while (off < b.len) {
        ssize_t n = write(fd, b.data + off, b.len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = webdoc_errno_map(errno);
            close(fd);
            unlink(path);
            goto out_buf;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        rc = webdoc_errno_map(errno);
        close(fd);
        unlink(path);
        goto out_buf;
    }
    close(fd);

    snprintf(dirbuf, sizeof(dirbuf), "%s", path);
    slash = strrchr(dirbuf, '/');
    if (slash) {
        if (slash == dirbuf)
            dirbuf[1] = '\0';
        else
            *slash = '\0';
    } else {
        snprintf(dirbuf, sizeof(dirbuf), ".");
    }
    dfd = open(dirbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    webdoc_report("created %s", path);
    rc = WEBDOC_OK;

out_buf:
    json_buf_free(&b);
out_doc:
    webdoc_reset(&doc);
    return rc;
}

/* ------------------------------------------------------------------ edit -- */

webdoc_err webdoc_edit(const char *path, const webdoc_edit_params *params)
{
    webdoc_t doc;
    webdoc_err rc;
    char *dup;

    if (!path || !params)
        return WEBDOC_ERR_INVALID_ARG;

    rc = webdoc_load(path, &doc);
    if (rc != WEBDOC_OK)
        return rc;
    webdoc_report("read %s", path);

    if (params->url) {
        rc = webdoc_url_validate(params->url);
        if (rc != WEBDOC_OK)
            goto out;
        dup = strdup(params->url);
        if (!dup) {
            rc = WEBDOC_ERR_NOMEM;
            goto out;
        }
        free(doc.url);
        doc.url = dup;
        webdoc_report("address set to %s", doc.url);
    }

    if (params->set_browser) {
        if (params->browser_kind == WEBDOC_BROWSER_DEFAULT) {
            free(doc.browser_ref);
            doc.browser_ref = NULL;
            doc.browser_kind = WEBDOC_BROWSER_DEFAULT;
            webdoc_report("browser set to the system default");
        } else {
            if (!params->browser_ref) {
                rc = WEBDOC_ERR_INVALID_ARG;
                goto out;
            }
            if (params->browser_kind == WEBDOC_BROWSER_VARIABLE) {
                rc = webdoc_variable_name_validate(params->browser_ref);
                if (rc != WEBDOC_OK)
                    goto out;
            } else if (params->browser_ref[0] != '/') {
                rc = WEBDOC_ERR_INVALID_BROWSER;
                goto out;
            }
            dup = strdup(params->browser_ref);
            if (!dup) {
                rc = WEBDOC_ERR_NOMEM;
                goto out;
            }
            free(doc.browser_ref);
            doc.browser_ref = dup;
            doc.browser_kind = params->browser_kind;
            webdoc_report("browser set to %s", doc.browser_ref);
        }
    }

    if (params->locked != WEBDOC_EDIT_KEEP) {
        doc.locked = params->locked ? 1 : 0;
        webdoc_report("document %s", doc.locked ? "locked" : "unlocked");
    }

    rc = webdoc_save(path, &doc);

out:
    webdoc_reset(&doc);
    return rc;
}

/* ---------------------------------------------------------------- delete -- */

webdoc_err webdoc_delete(const char *path)
{
    struct stat st;

    if (!path)
        return WEBDOC_ERR_INVALID_ARG;

    /* lstat, not stat: deleting a symlink named foo.web removes the symlink
       itself, which is what the user asked for.  Directories are refused. */
    if (lstat(path, &st) != 0)
        return webdoc_errno_map(errno);
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
        return WEBDOC_ERR_NOT_A_DOCUMENT;

    if (unlink(path) != 0)
        return webdoc_errno_map(errno);
    webdoc_report("deleted %s", path);
    return WEBDOC_OK;
}

/* ---------------------------------------------------------------- locking - */

webdoc_err webdoc_lock(const char *path)
{
    webdoc_edit_params p;

    if (!path)
        return webdoc_global_lock_set(1);

    memset(&p, 0, sizeof(p));
    p.locked = 1;
    return webdoc_edit(path, &p);
}

webdoc_err webdoc_unlock(const char *path)
{
    webdoc_edit_params p;

    if (!path)
        return webdoc_global_lock_set(0);

    memset(&p, 0, sizeof(p));
    p.locked = 0;
    return webdoc_edit(path, &p);
}

webdoc_err webdoc_lock_state(const char *path, int *global_locked, int *webdoc_locked)
{
    webdoc_t doc;
    webdoc_err rc;

    if (global_locked) {
        rc = webdoc_global_lock_get(global_locked);
        if (rc != WEBDOC_OK)
            return rc;
    }
    if (webdoc_locked) {
        *webdoc_locked = 0;
        if (path) {
            rc = webdoc_load(path, &doc);
            if (rc != WEBDOC_OK)
                return rc;
            *webdoc_locked = doc.locked;
            webdoc_reset(&doc);
        }
    }
    return WEBDOC_OK;
}
