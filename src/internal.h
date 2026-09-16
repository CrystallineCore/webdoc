/* SPDX-License-Identifier: MIT */
#ifndef WEBDOC_INTERNAL_H
#define WEBDOC_INTERNAL_H

#include "webdoc.h"
#include <sys/stat.h>
#include <stddef.h>

/* errno -> webdoc_err mapping used consistently by every syscall wrapper. */
webdoc_err webdoc_errno_map(int e);

/*
 * Emits one verbose progress line, when verbose reporting is enabled.
 *
 * Messages describe the step in the user's own vocabulary.  Internal
 * detail -- temporary file names, descriptor numbers, on-disk field
 * encodings, function names -- never appears here: a verbose run is
 * documentation of what happened, not a debug trace.
 */
void webdoc_report(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Reads at most max_size bytes of a regular file.  Fails with
   WEBDOC_ERR_TOO_LARGE beyond that and WEBDOC_ERR_NOT_A_DOCUMENT for
   non-regular files.  *out is NUL terminated and owned by the caller. */
webdoc_err webdoc_read_file(const char *path, size_t max_size,
                            char **out, size_t *out_len);

/*
 * Atomically replaces path with data.
 *
 * Writes a temporary file in the same directory (same filesystem, so
 * rename() is atomic), fsync()s it, then rename()s over the target and
 * fsync()s the directory.  If path is a symlink the *target* is replaced,
 * never the symlink itself, and never a path outside the resolved target.
 */
webdoc_err webdoc_write_atomic(const char *path, const char *data, size_t len,
                               mode_t mode);

/* mkdir -p with mode 0700 for every created component. */
webdoc_err webdoc_mkdir_p(const char *path);

/* XDG directories, without trailing slash.  Returns WEBDOC_ERR_INTERNAL if
   neither the XDG variable nor HOME is usable. */
webdoc_err webdoc_config_dir(char *out, size_t outlen); /* ~/.config/webdoc  */
webdoc_err webdoc_state_dir(char *out, size_t outlen);  /* ~/.local/state/.. */

/* Expands a leading "~/" using $HOME.  Other uses of '~' are literal. */
webdoc_err webdoc_expand_tilde(const char *in, char *out, size_t outlen);

int webdoc_str_has_suffix(const char *s, const char *suffix);

/* Global state (lock) and configuration (variables). */
webdoc_err webdoc_global_lock_get(int *locked);
webdoc_err webdoc_global_lock_set(int locked);

/* Variable-backed browser resolution + safe launching. */
webdoc_err webdoc_browser_resolve(const webdoc_t *doc, char *out, size_t outlen,
                                  int *use_default);
webdoc_err webdoc_browser_validate_exe(const char *path);

/*
 * Launches exe with url as its single argument, fully detached.
 *
 * The child is always disconnected from the calling terminal; there is no
 * flag to keep it attached, because a browser that outlives the caller and
 * still holds the caller's terminal corrupts the next shell prompt and can
 * consume the user's keystrokes.
 */
webdoc_err webdoc_spawn_detached(const char *exe, int search_path,
                                 const char *url);

#endif /* WEBDOC_INTERNAL_H */
