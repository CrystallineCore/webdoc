/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

webdoc_err webdoc_errno_map(int e)
{
    switch (e) {
    case 0:            return WEBDOC_OK;
    case ENOENT:
    case ENOTDIR:      return WEBDOC_ERR_NOT_FOUND;
    case EEXIST:       return WEBDOC_ERR_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS:        return WEBDOC_ERR_PERMISSION;
    case ENOMEM:       return WEBDOC_ERR_NOMEM;
    case ENAMETOOLONG:
    case EFBIG:
    case ENOSPC:       return WEBDOC_ERR_TOO_LARGE;
    case EINVAL:       return WEBDOC_ERR_INVALID_ARG;
    case ELOOP:        return WEBDOC_ERR_INVALID_ARG;
    default:           return WEBDOC_ERR_IO;
    }
}

int webdoc_str_has_suffix(const char *s, const char *suffix)
{
    size_t ls, lsuf;

    if (!s || !suffix)
        return 0;
    ls = strlen(s);
    lsuf = strlen(suffix);
    return ls >= lsuf && strcmp(s + ls - lsuf, suffix) == 0;
}

webdoc_err webdoc_expand_tilde(const char *in, char *out, size_t outlen)
{
    const char *home;
    int n;

    if (!in || !out)
        return WEBDOC_ERR_INVALID_ARG;

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        home = getenv("HOME");
        if (!home || home[0] != '/')
            return WEBDOC_ERR_INTERNAL;
        n = snprintf(out, outlen, "%s%s", home, in + 1);
    } else {
        n = snprintf(out, outlen, "%s", in);
    }
    if (n < 0 || (size_t)n >= outlen)
        return WEBDOC_ERR_TOO_LARGE;
    return WEBDOC_OK;
}

webdoc_err webdoc_read_file(const char *path, size_t max_size,
                        char **out, size_t *out_len)
{
    int fd;
    struct stat st;
    char *buf = NULL;
    size_t total = 0;
    webdoc_err rc;

    if (!path || !out)
        return WEBDOC_ERR_INVALID_ARG;
    *out = NULL;
    if (out_len)
        *out_len = 0;

    /* O_NONBLOCK: opening a FIFO without it blocks until a writer appears,
       so a .web that is really a FIFO could hang us before we ever get to
       check the file type. */
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return webdoc_errno_map(errno);

    if (fstat(fd, &st) != 0) {
        rc = webdoc_errno_map(errno);
        goto out;
    }
    /* Reject directories, FIFOs, devices: a .web is a regular file.  This
       also stops open() on a FIFO-turned-symlink from blocking us forever. */
    if (!S_ISREG(st.st_mode)) {
        rc = WEBDOC_ERR_NOT_A_DOCUMENT;
        goto out;
    }
    if (st.st_size >= 0 && (size_t)st.st_size > max_size) {
        rc = WEBDOC_ERR_TOO_LARGE;
        goto out;
    }

    buf = malloc(max_size + 1);
    if (!buf) {
        rc = WEBDOC_ERR_NOMEM;
        goto out;
    }
    while (total < max_size) {
        ssize_t n = read(fd, buf + total, max_size - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = webdoc_errno_map(errno);
            goto out;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    if (total >= max_size) {
        /* The file grew between fstat() and read(). */
        rc = WEBDOC_ERR_TOO_LARGE;
        goto out;
    }
    buf[total] = '\0';
    *out = buf;
    buf = NULL;
    if (out_len)
        *out_len = total;
    rc = WEBDOC_OK;

out:
    free(buf);
    close(fd);
    return rc;
}

static webdoc_err write_all(int fd, const char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return webdoc_errno_map(errno);
        }
        off += (size_t)n;
    }
    return WEBDOC_OK;
}

webdoc_err webdoc_write_atomic(const char *path, const char *data, size_t len,
                           mode_t mode)
{
    char real[WEBDOC_MAX_PATH];
    char dirbuf[WEBDOC_MAX_PATH];
    char tmpl[WEBDOC_MAX_PATH];
    const char *target = path;
    char *slash;
    const char *dir;
    int fd = -1, dfd = -1, n;
    webdoc_err rc;

    if (!path || !data)
        return WEBDOC_ERR_INVALID_ARG;

    /*
     * If the target exists and is (or is reached through) a symlink, resolve
     * it first and replace the real file.  Replacing the symlink itself would
     * silently detach the web document from where the user expects it to live; and
     * resolving up-front means the rename() lands inside the directory we
     * actually checked.
     */
    if (realpath(path, real) != NULL)
        target = real;

    if (snprintf(dirbuf, sizeof(dirbuf), "%s", target) >= (int)sizeof(dirbuf))
        return WEBDOC_ERR_TOO_LARGE;
    slash = strrchr(dirbuf, '/');
    if (slash) {
        if (slash == dirbuf)
            dirbuf[1] = '\0';
        else
            *slash = '\0';
        dir = dirbuf;
    } else {
        dir = ".";
    }

    n = snprintf(tmpl, sizeof(tmpl), "%s/.webdoc-tmp-XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(tmpl))
        return WEBDOC_ERR_TOO_LARGE;

    fd = mkstemp(tmpl);        /* O_EXCL, mode 0600 */
    if (fd < 0)
        return webdoc_errno_map(errno);

    rc = write_all(fd, data, len);
    if (rc != WEBDOC_OK)
        goto fail;

    if (fchmod(fd, mode) != 0) {
        rc = webdoc_errno_map(errno);
        goto fail;
    }
    if (fsync(fd) != 0) {
        rc = webdoc_errno_map(errno);
        goto fail;
    }
    if (close(fd) != 0) {
        fd = -1;
        rc = webdoc_errno_map(errno);
        goto fail;
    }
    fd = -1;

    if (rename(tmpl, target) != 0) {
        rc = webdoc_errno_map(errno);
        goto fail;
    }

    /* Make the rename itself durable. */
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return WEBDOC_OK;

fail:
    if (fd >= 0)
        close(fd);
    unlink(tmpl);
    return rc;
}

webdoc_err webdoc_mkdir_p(const char *path)
{
    char buf[WEBDOC_MAX_PATH];
    size_t i;

    if (!path || path[0] != '/')
        return WEBDOC_ERR_INVALID_ARG;
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
        return WEBDOC_ERR_TOO_LARGE;

    for (i = 1; buf[i]; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST)
                return webdoc_errno_map(errno);
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        return webdoc_errno_map(errno);
    return WEBDOC_OK;
}

static webdoc_err xdg_dir(const char *env, const char *fallback,
                        char *out, size_t outlen)
{
    const char *base = getenv(env);
    const char *home;
    int n;

    if (base && base[0] == '/') {
        n = snprintf(out, outlen, "%s/webdoc", base);
    } else {
        home = getenv("HOME");
        if (!home || home[0] != '/')
            return WEBDOC_ERR_INTERNAL;
        n = snprintf(out, outlen, "%s/%s/webdoc", home, fallback);
    }
    if (n < 0 || (size_t)n >= outlen)
        return WEBDOC_ERR_TOO_LARGE;
    return WEBDOC_OK;
}

webdoc_err webdoc_config_dir(char *out, size_t outlen)
{
    return xdg_dir("XDG_CONFIG_HOME", ".config", out, outlen);
}

webdoc_err webdoc_state_dir(char *out, size_t outlen)
{
    return xdg_dir("XDG_STATE_HOME", ".local/state", out, outlen);
}
