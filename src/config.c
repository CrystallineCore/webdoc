/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Two files, chosen by the semantics of the data:
 *
 *   $XDG_CONFIG_HOME/webdoc/config.json - user configuration the user may
 *                                         edit and may want in dotfiles:
 *                                         the variable mappings.
 *   $XDG_STATE_HOME/webdoc/state.json   - mutable runtime state that is not
 *                                         hand-authored configuration: the
 *                                         global lock.
 *
 * Neither file ever contains a registry of .web files, per-document URLs or
 * per-document lock state.  The filesystem is the source of truth.
 */

#define CONFIG_BASENAME "config.json"
#define STATE_BASENAME  "state.json"

static webdoc_err config_path(char *out, size_t outlen)
{
    char dir[WEBDOC_MAX_PATH];
    webdoc_err rc = webdoc_config_dir(dir, sizeof(dir));

    if (rc != WEBDOC_OK)
        return rc;
    if (snprintf(out, outlen, "%s/" CONFIG_BASENAME, dir) >= (int)outlen)
        return WEBDOC_ERR_TOO_LARGE;
    return WEBDOC_OK;
}

static webdoc_err state_path(char *out, size_t outlen)
{
    char dir[WEBDOC_MAX_PATH];
    webdoc_err rc = webdoc_state_dir(dir, sizeof(dir));

    if (rc != WEBDOC_OK)
        return rc;
    if (snprintf(out, outlen, "%s/" STATE_BASENAME, dir) >= (int)outlen)
        return WEBDOC_ERR_TOO_LARGE;
    return WEBDOC_OK;
}

webdoc_err webdoc_config_file(char *out, size_t outlen)
{
    if (!out)
        return WEBDOC_ERR_INVALID_ARG;
    return config_path(out, outlen);
}

webdoc_err webdoc_state_file(char *out, size_t outlen)
{
    if (!out)
        return WEBDOC_ERR_INVALID_ARG;
    return state_path(out, outlen);
}

/* Reads a JSON object file.  A missing file yields *out == NULL, WEBDOC_OK. */
static webdoc_err read_json_object(const char *path, json_value **out)
{
    char *buf = NULL;
    size_t len = 0;
    json_value *v = NULL;
    const char *err = NULL;
    webdoc_err rc;

    *out = NULL;
    rc = webdoc_read_file(path, WEBDOC_MAX_FILE_SIZE, &buf, &len);
    if (rc == WEBDOC_ERR_NOT_FOUND)
        return WEBDOC_OK;
    if (rc != WEBDOC_OK)
        return rc;

    if (len == 0) {
        free(buf);
        return WEBDOC_OK;
    }
    if (json_parse(buf, len, &v, &err) != 0) {
        free(buf);
        return WEBDOC_ERR_INVALID_FILE;
    }
    free(buf);
    if (v->type != JSON_OBJECT) {
        json_free(v);
        return WEBDOC_ERR_INVALID_FILE;
    }
    *out = v;
    return WEBDOC_OK;
}

static webdoc_err write_private_file(const char *path, const char *data,
                                   size_t len)
{
    char dir[WEBDOC_MAX_PATH];
    char *slash;
    webdoc_err rc;

    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir))
        return WEBDOC_ERR_TOO_LARGE;
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        rc = webdoc_mkdir_p(dir);
        if (rc != WEBDOC_OK)
            return rc;
    }
    return webdoc_write_atomic(path, data, len, 0600);
}

/* ------------------------------------------------------------- variables -- */

void webdoc_variable_list_free(webdoc_variable_list *list)
{
    size_t i;

    if (!list)
        return;
    for (i = 0; i < list->count; i++) {
        free(list->names[i]);
        free(list->values[i]);
    }
    free(list->names);
    free(list->values);
    list->names = NULL;
    list->values = NULL;
    list->count = 0;
}

static webdoc_err vars_load(webdoc_variable_list *out)
{
    char path[WEBDOC_MAX_PATH];
    json_value *root = NULL;
    const json_value *vars;
    size_t i;
    webdoc_err rc;

    memset(out, 0, sizeof(*out));

    rc = config_path(path, sizeof(path));
    if (rc != WEBDOC_OK)
        return rc;
    rc = read_json_object(path, &root);
    if (rc != WEBDOC_OK)
        return rc;
    if (!root)
        return WEBDOC_OK;

    /* "variables" is the current name.  "browsers" is read as well so that
       configuration written by 0.1.0 keeps working; it is never written. */
    vars = json_object_get(root, "variables");
    if (!vars || vars->type != JSON_OBJECT)
        vars = json_object_get(root, "browsers");
    if (!vars || vars->type != JSON_OBJECT) {
        json_free(root);
        return WEBDOC_OK;
    }

    out->names = calloc(vars->u.object.count + 1, sizeof(char *));
    out->values = calloc(vars->u.object.count + 1, sizeof(char *));
    if (!out->names || !out->values) {
        webdoc_variable_list_free(out);
        json_free(root);
        return WEBDOC_ERR_NOMEM;
    }
    for (i = 0; i < vars->u.object.count; i++) {
        const json_member *m = &vars->u.object.items[i];

        if (m->value->type != JSON_STRING)
            continue;                      /* ignore hostile / odd entries */
        if (webdoc_variable_name_validate(m->key) != WEBDOC_OK)
            continue;
        out->names[out->count] = strdup(m->key);
        out->values[out->count] = strdup(m->value->u.string);
        if (!out->names[out->count] || !out->values[out->count]) {
            webdoc_variable_list_free(out);
            json_free(root);
            return WEBDOC_ERR_NOMEM;
        }
        out->count++;
    }
    json_free(root);
    return WEBDOC_OK;
}

static webdoc_err vars_save(const webdoc_variable_list *list)
{
    char path[WEBDOC_MAX_PATH];
    json_buf b;
    size_t i;
    webdoc_err rc;

    rc = config_path(path, sizeof(path));
    if (rc != WEBDOC_OK)
        return rc;

    json_buf_init(&b);
    json_buf_appendz(&b, "{\n  \"version\": 1,\n  \"variables\": {");
    for (i = 0; i < list->count; i++) {
        json_buf_appendz(&b, i ? ",\n    " : "\n    ");
        json_escape_into(&b, list->names[i]);
        json_buf_appendz(&b, ": ");
        json_escape_into(&b, list->values[i]);
    }
    json_buf_appendz(&b, list->count ? "\n  }\n}\n" : "}\n}\n");
    if (b.failed) {
        json_buf_free(&b);
        return WEBDOC_ERR_NOMEM;
    }
    rc = write_private_file(path, b.data, b.len);
    json_buf_free(&b);
    return rc;
}

webdoc_err webdoc_list_variables(webdoc_variable_list *out)
{
    if (!out)
        return WEBDOC_ERR_INVALID_ARG;
    return vars_load(out);
}

webdoc_err webdoc_get_variable(const char *name, char **value_out)
{
    webdoc_variable_list list;
    size_t i;
    webdoc_err rc;

    if (!value_out)
        return WEBDOC_ERR_INVALID_ARG;
    *value_out = NULL;

    rc = webdoc_variable_name_validate(name);
    if (rc != WEBDOC_OK)
        return rc;
    rc = vars_load(&list);
    if (rc != WEBDOC_OK)
        return rc;

    rc = WEBDOC_ERR_VAR_NOT_FOUND;
    for (i = 0; i < list.count; i++) {
        if (strcmp(list.names[i], name) == 0) {
            *value_out = strdup(list.values[i]);
            rc = *value_out ? WEBDOC_OK : WEBDOC_ERR_NOMEM;
            break;
        }
    }
    webdoc_variable_list_free(&list);
    return rc;
}

webdoc_err webdoc_set_variable(const char *name, const char *value)
{
    webdoc_variable_list list;
    size_t i;
    webdoc_err rc;
    char **gn, **gv;

    rc = webdoc_variable_name_validate(name);
    if (rc != WEBDOC_OK)
        return rc;
    rc = webdoc_variable_value_validate(value);
    if (rc != WEBDOC_OK)
        return rc;

    rc = vars_load(&list);
    if (rc != WEBDOC_OK)
        return rc;

    for (i = 0; i < list.count; i++) {
        if (strcmp(list.names[i], name) == 0) {
            char *dup = strdup(value);
            if (!dup) {
                webdoc_variable_list_free(&list);
                return WEBDOC_ERR_NOMEM;
            }
            free(list.values[i]);
            list.values[i] = dup;
            webdoc_report("redefining variable '%s'", name);
            rc = vars_save(&list);
            webdoc_variable_list_free(&list);
            return rc;
        }
    }

    gn = realloc(list.names, (list.count + 1) * sizeof(char *));
    if (gn)
        list.names = gn;
    gv = realloc(list.values, (list.count + 1) * sizeof(char *));
    if (gv)
        list.values = gv;
    if (!gn || !gv) {
        webdoc_variable_list_free(&list);
        return WEBDOC_ERR_NOMEM;
    }
    list.names[list.count] = strdup(name);
    list.values[list.count] = strdup(value);
    if (!list.names[list.count] || !list.values[list.count]) {
        free(list.names[list.count]);
        free(list.values[list.count]);
        webdoc_variable_list_free(&list);
        return WEBDOC_ERR_NOMEM;
    }
    list.count++;
    webdoc_report("defining variable '%s'", name);
    rc = vars_save(&list);
    webdoc_variable_list_free(&list);
    return rc;
}

webdoc_err webdoc_unset_variable(const char *name)
{
    webdoc_variable_list list;
    size_t i;
    int found = 0;
    webdoc_err rc;

    rc = webdoc_variable_name_validate(name);
    if (rc != WEBDOC_OK)
        return rc;
    rc = vars_load(&list);
    if (rc != WEBDOC_OK)
        return rc;

    for (i = 0; i < list.count; i++) {
        if (strcmp(list.names[i], name) == 0) {
            free(list.names[i]);
            free(list.values[i]);
            memmove(&list.names[i], &list.names[i + 1],
                    (list.count - i - 1) * sizeof(char *));
            memmove(&list.values[i], &list.values[i + 1],
                    (list.count - i - 1) * sizeof(char *));
            list.count--;
            found = 1;
            break;
        }
    }
    if (!found) {
        webdoc_variable_list_free(&list);
        return WEBDOC_ERR_VAR_NOT_FOUND;
    }
    webdoc_report("removing variable '%s'", name);
    rc = vars_save(&list);
    webdoc_variable_list_free(&list);
    return rc;
}

/* ------------------------------------------------------------ global lock - */

webdoc_err webdoc_global_lock_get(int *locked)
{
    char path[WEBDOC_MAX_PATH];
    json_value *root = NULL;
    const json_value *v;
    webdoc_err rc;

    if (!locked)
        return WEBDOC_ERR_INVALID_ARG;
    *locked = 0;

    rc = state_path(path, sizeof(path));
    if (rc != WEBDOC_OK)
        return rc;
    rc = read_json_object(path, &root);
    if (rc != WEBDOC_OK)
        return rc;
    if (!root)
        return WEBDOC_OK;                    /* absent state == unlocked */

    v = json_object_get(root, "locked");
    if (v && v->type == JSON_BOOL)
        *locked = v->u.boolean;
    json_free(root);
    return WEBDOC_OK;
}

webdoc_err webdoc_global_lock_set(int locked)
{
    char path[WEBDOC_MAX_PATH];
    char body[128];
    int n;
    webdoc_err rc;

    rc = state_path(path, sizeof(path));
    if (rc != WEBDOC_OK)
        return rc;
    webdoc_report("global lock %s", locked ? "enabled" : "disabled");
    n = snprintf(body, sizeof(body),
                 "{\n  \"version\": 1,\n  \"locked\": %s\n}\n",
                 locked ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body))
        return WEBDOC_ERR_INTERNAL;
    return write_private_file(path, body, (size_t)n);
}
