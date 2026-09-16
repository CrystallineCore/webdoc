/*
 * webdoc.h - public API of libwebdoc
 *
 * A .web file is a first-class Linux userspace datatype representing a web
 * URL plus a small amount of metadata.
 *
 * Invariant: a .web file owns all persistent state belonging to that
 * document.  Global configuration stores only genuinely global state
 * (variable mappings) and global runtime state (the global lock).  It is
 * never a database or registry of individual documents.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBWEBDOC_WEBDOC_H
#define LIBWEBDOC_WEBDOC_H

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBDOC_FILE_VERSION      1      /* .web format version we write     */
#define WEBDOC_EXT               ".web"
#define WEBDOC_MAX_FILE_SIZE     (64u * 1024u)  /* refuse larger .web files */
#define WEBDOC_MAX_URL_LEN       2048u
#define WEBDOC_MAX_VAR_NAME_LEN  64u
#define WEBDOC_MAX_PATH          4096u

/* ------------------------------------------------------------------ errors */

typedef enum {
    WEBDOC_OK = 0,
    WEBDOC_ERR_INVALID_ARG,          /* programming / usage error            */
    WEBDOC_ERR_INVALID_URL,          /* URL is syntactically unacceptable    */
    WEBDOC_ERR_UNSUPPORTED_SCHEME,   /* not http:// or https://              */
    WEBDOC_ERR_INVALID_FILE,         /* malformed .web / malformed JSON      */
    WEBDOC_ERR_UNSUPPORTED_VERSION,  /* .web written by a newer webdoc       */
    WEBDOC_ERR_NOT_FOUND,            /* file / directory does not exist      */
    WEBDOC_ERR_EXISTS,               /* refusing to overwrite                */
    WEBDOC_ERR_NOT_A_DOCUMENT,       /* path exists but is not a regular file*/
    WEBDOC_ERR_IS_DIRECTORY,         /* a directory was given where a single
                                        document was required               */
    WEBDOC_ERR_LOCKED,               /* global or per-document lock denied   */
    WEBDOC_ERR_INVALID_BROWSER,      /* browser spec / executable unusable   */
    WEBDOC_ERR_VAR_NOT_FOUND,        /* variable is not defined              */
    WEBDOC_ERR_PERMISSION,           /* EACCES / EPERM / EROFS               */
    WEBDOC_ERR_TOO_LARGE,            /* resource limit exceeded              */
    WEBDOC_ERR_IO,                   /* generic I/O failure                  */
    WEBDOC_ERR_NOMEM,                /* allocation failure                   */
    WEBDOC_ERR_SPAWN,                /* could not launch the browser         */
    WEBDOC_ERR_INTERNAL
} webdoc_err;

/* Human readable, non-localised description of an error code. */
const char *webdoc_strerror(webdoc_err err);

/* ------------------------------------------------------------- reporting -- */

/*
 * Progress reporting for --verbose.
 *
 * Disabled by default.  When enabled, libwebdoc writes short progress lines
 * describing each step it performs to the stream set with
 * webdoc_set_report_stream() (stderr by default), so that the standard
 * output of a command stays machine readable.
 *
 * Reporting describes what was done in the user's own terms.  It never
 * exposes internal implementation detail such as temporary file names,
 * descriptor numbers or on-disk field encodings.
 */
void webdoc_set_verbose(int enabled);
int  webdoc_verbose(void);
void webdoc_set_report_stream(FILE *stream);

/* ------------------------------------------------------------- webdoc_t --- */

typedef enum {
    WEBDOC_BROWSER_DEFAULT = 0,  /* field absent: use the desktop default    */
    WEBDOC_BROWSER_VARIABLE,     /* "variable:NAME"                          */
    WEBDOC_BROWSER_PATH          /* "path:/absolute/executable"              */
} webdoc_browser_kind;

/*
 * In-memory representation of one .web file.
 *
 * Ownership: every pointer member is owned by the struct and released by
 * webdoc_reset().  Callers must zero-initialise with webdoc_init() before
 * first use.
 */
typedef struct {
    int                  version;      /* format version that was read      */
    char                *url;          /* owned, NUL terminated             */
    webdoc_browser_kind  browser_kind;
    char                *browser_ref;  /* owned: variable name or literal
                                          path, NULL when browser_kind is
                                          WEBDOC_BROWSER_DEFAULT            */
    int                  locked;       /* per-document lock state           */
    char                *extra_json;   /* owned: serialised unknown v1
                                          members, preserved verbatim on
                                          edit, or NULL                     */
} webdoc_t;

void webdoc_init(webdoc_t *doc);
void webdoc_reset(webdoc_t *doc);

/* Rendered value of the "browser" field, e.g. "variable:chrome".
   Returns NULL for WEBDOC_BROWSER_DEFAULT.  Buffer is caller supplied. */
const char *webdoc_browser_string(const webdoc_t *doc, char *buf, size_t buflen);

/* ------------------------------------------------------------ validation --- */

webdoc_err webdoc_url_validate(const char *url);
webdoc_err webdoc_variable_name_validate(const char *name);
webdoc_err webdoc_variable_value_validate(const char *value);

/* ------------------------------------------------------------- resolution -- */

/*
 * Turn a user supplied reference into a filesystem path.
 *
 *   explicit_path == 0 : "postgres", "postgres.web", "~/Web/postgres" and
 *                        "/abs/postgres" all resolve to a path ending in
 *                        exactly one ".web" suffix.
 *   explicit_path != 0 : the argument is used verbatim (no suffix logic,
 *                        no tilde expansion).  This is `--path`.
 */
webdoc_err webdoc_resolve_name(const char *reference, int explicit_path,
                               char *out, size_t outlen);

/* --------------------------------------------------------------- file I/O -- */

webdoc_err webdoc_load(const char *path, webdoc_t *out);
webdoc_err webdoc_save(const char *path, const webdoc_t *doc); /* atomic */

typedef struct {
    const char          *url;          /* required                          */
    webdoc_browser_kind  browser_kind;
    const char          *browser_ref;  /* variable name or absolute path    */
    int                  locked;
} webdoc_create_params;

/* Creates path; fails with WEBDOC_ERR_EXISTS if anything is already there. */
webdoc_err webdoc_create(const char *path, const webdoc_create_params *params);

/*
 * Partial update.  NULL / WEBDOC_EDIT_KEEP members are left untouched.
 */
#define WEBDOC_EDIT_KEEP (-1)

typedef struct {
    const char          *url;          /* NULL: keep                        */
    int                  set_browser;  /* 0: keep, 1: apply kind/ref        */
    webdoc_browser_kind  browser_kind;
    const char          *browser_ref;
    int                  locked;       /* WEBDOC_EDIT_KEEP, 0 or 1          */
} webdoc_edit_params;

webdoc_err webdoc_edit(const char *path, const webdoc_edit_params *params);
webdoc_err webdoc_delete(const char *path);

/* ---------------------------------------------------------------- locking -- */

webdoc_err webdoc_lock(const char *path);    /* path == NULL: global lock    */
webdoc_err webdoc_unlock(const char *path);  /* path == NULL: global unlock  */

/* Reports the effective lock state without opening anything. */
webdoc_err webdoc_lock_state(const char *path, int *global_locked,
                             int *doc_locked);

/* -------------------------------------------------------------- variables -- */

webdoc_err webdoc_set_variable(const char *name, const char *value);
webdoc_err webdoc_get_variable(const char *name, char **value_out); /* free */
webdoc_err webdoc_unset_variable(const char *name);

typedef struct {
    char **names;
    char **values;
    size_t count;
} webdoc_variable_list;

webdoc_err webdoc_list_variables(webdoc_variable_list *out);
void       webdoc_variable_list_free(webdoc_variable_list *list);

/* ---------------------------------------------------------- configuration -- */

/* Absolute path of the configuration file (variables) and of the state file
   (global lock).  Reported so that a user can find, back up or edit them;
   neither file is ever a registry of individual documents. */
webdoc_err webdoc_config_file(char *out, size_t outlen);
webdoc_err webdoc_state_file(char *out, size_t outlen);

/* ------------------------------------------------------------------- open -- */

/*
 * The single opening pipeline, shared by `web open` and by the desktop MIME
 * handler:
 *   load -> validate URL -> global lock -> per-document lock ->
 *   resolve browser -> validate executable -> safe spawn.
 *
 * The launched browser is always fully detached: it runs in its own session,
 * is reparented to init, and all three of its standard streams are
 * redirected to /dev/null.  It therefore can never write to the caller's
 * terminal after the caller has returned, never reads the caller's input,
 * and never holds the terminal open.  This is unconditional: verbose mode
 * changes what libwebdoc itself reports, never how the browser is attached.
 */
webdoc_err webdoc_open(const char *path);

/* --------------------------------------------------------- show / listing -- */

/* Prints one document.  A directory is refused with
   WEBDOC_ERR_IS_DIRECTORY; use webdoc_list_dir() to enumerate one. */
webdoc_err webdoc_show(const char *path, FILE *out);

typedef struct {
    char       *name;      /* file name without the .web suffix             */
    char       *path;      /* full path                                     */
    char       *url;       /* NULL when the entry could not be parsed       */
    char       *browser;   /* rendered browser field, or NULL               */
    int         locked;
    mode_t      mode;
    off_t       size;
    time_t      mtime;
    uid_t       uid;
    gid_t       gid;
    webdoc_err  status;    /* WEBDOC_OK, or why this entry could not be read*/
} webdoc_entry;

typedef struct {
    webdoc_entry *items;
    size_t        count;
} webdoc_list;

webdoc_err webdoc_list_dir(const char *dir, webdoc_list *out);
void       webdoc_list_free(webdoc_list *list);

#ifdef __cplusplus
}
#endif
#endif /* LIBWEBDOC_WEBDOC_H */
