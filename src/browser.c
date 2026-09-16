/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

/*
 * Resolves the browser configuration of a web document to an executable path.
 *
 * The .web file stores an indirection ("variable:chrome"), never a resolved
 * path, so `web set chrome /usr/bin/chromium` immediately affects every web document
 * that references the variable.  Resolution therefore happens here, at open
 * time, on every open.
 */
webdoc_err webdoc_browser_resolve(const webdoc_t *doc, char *out, size_t outlen,
                              int *use_default)
{
    char *value = NULL;
    webdoc_err rc;

    if (!doc || !out || !use_default)
        return WEBDOC_ERR_INVALID_ARG;

    *use_default = 0;

    switch (doc->browser_kind) {
    case WEBDOC_BROWSER_DEFAULT:
        *use_default = 1;
        return WEBDOC_OK;

    case WEBDOC_BROWSER_PATH:
        if (!doc->browser_ref || doc->browser_ref[0] != '/')
            return WEBDOC_ERR_INVALID_BROWSER;
        if (snprintf(out, outlen, "%s", doc->browser_ref) >= (int)outlen)
            return WEBDOC_ERR_TOO_LARGE;
        return WEBDOC_OK;

    case WEBDOC_BROWSER_VARIABLE:
        rc = webdoc_get_variable(doc->browser_ref, &value);
        if (rc != WEBDOC_OK)
            return rc;
        if (!value || value[0] != '/') {
            free(value);
            return WEBDOC_ERR_INVALID_BROWSER;
        }
        if (snprintf(out, outlen, "%s", value) >= (int)outlen) {
            free(value);
            return WEBDOC_ERR_TOO_LARGE;
        }
        free(value);
        return WEBDOC_OK;
    }
    return WEBDOC_ERR_INVALID_BROWSER;
}

/*
 * A browser path comes from an untrusted .web file or from configuration,
 * and we are about to execute it, so validate before spawning:
 *
 *   - absolute path only (no PATH search of an attacker-chosen name)
 *   - must be a regular file (not a directory, device or FIFO)
 *   - must be executable by us
 *   - must not be world-writable, and must not live in a world-writable
 *     directory: either would let any local user swap the binary we run.
 */
webdoc_err webdoc_browser_validate_exe(const char *path)
{
    struct stat st;
    char dirbuf[WEBDOC_MAX_PATH];
    char *slash;

    if (!path || path[0] != '/')
        return WEBDOC_ERR_INVALID_BROWSER;
    if (strstr(path, "/../") || webdoc_str_has_suffix(path, "/.."))
        return WEBDOC_ERR_INVALID_BROWSER;

    if (stat(path, &st) != 0)
        return (errno == ENOENT || errno == ENOTDIR)
               ? WEBDOC_ERR_INVALID_BROWSER : webdoc_errno_map(errno);
    if (!S_ISREG(st.st_mode))
        return WEBDOC_ERR_INVALID_BROWSER;
    if (st.st_mode & S_IWOTH)
        return WEBDOC_ERR_INVALID_BROWSER;
    if (access(path, X_OK) != 0)
        return WEBDOC_ERR_INVALID_BROWSER;

    if (snprintf(dirbuf, sizeof(dirbuf), "%s", path) >= (int)sizeof(dirbuf))
        return WEBDOC_ERR_TOO_LARGE;
    slash = strrchr(dirbuf, '/');
    if (slash) {
        if (slash == dirbuf)
            dirbuf[1] = '\0';
        else
            *slash = '\0';
        if (stat(dirbuf, &st) == 0 && (st.st_mode & S_IWOTH) &&
            !(st.st_mode & S_ISVTX))
            return WEBDOC_ERR_INVALID_BROWSER;
    }
    return WEBDOC_OK;
}

/*
 * Launches exe with the URL as a single argv element.
 *
 * There is no shell anywhere in this path: no system(), no popen(), no
 * command string.  The URL cannot be word-split, globbed or interpreted as
 * shell metacharacters because it never passes through a shell parser.
 *
 * A double fork detaches the browser: the intermediate child exits at once,
 * so the browser is reparented to init and `web open` does not block for the
 * lifetime of the browser or leave a zombie behind.
 *
 * All three of the child's standard streams are redirected to /dev/null
 * before the spawn, unconditionally.  This is not a tuneable:
 *
 *   - stdout/stderr left attached means the browser writes into the caller's
 *     terminal long after `web` has exited.  Its startup chatter ("Opening
 *     in existing browser session.") then lands in the middle of the next
 *     shell prompt, and the terminal stays tied to a process the user
 *     believes is gone, so the session appears to hang until interrupted.
 *   - stdin left attached means the browser competes with the shell for the
 *     user's keystrokes.
 *
 * Verbose mode therefore changes what *webdoc* reports, never how the
 * browser is connected: there is no code path in which a launched browser
 * shares the caller's terminal.  A user who needs the browser's own
 * diagnostics runs the browser directly.
 */
webdoc_err webdoc_spawn_detached(const char *exe, int search_path,
                                 const char *url)
{
    pid_t mid;
    int status = 0;

    if (!exe || !url)
        return WEBDOC_ERR_INVALID_ARG;

    mid = fork();
    if (mid < 0)
        return WEBDOC_ERR_SPAWN;

    if (mid == 0) {
        char *argv[3];
        posix_spawnattr_t attr;
        sigset_t empty;
        pid_t child;
        int rc, devnull;

        /* New session: the child has no controlling terminal at all, so it
           can neither read from nor be signalled by the caller's one. */
        if (setsid() < 0)
            _exit(2);

        devnull = open("/dev/null", O_RDWR);
        if (devnull < 0) {
            /* Without /dev/null the only safe thing is to leave the child
               with no standard streams rather than the caller's. */
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        } else {
            if (dup2(devnull, STDIN_FILENO) < 0 ||
                dup2(devnull, STDOUT_FILENO) < 0 ||
                dup2(devnull, STDERR_FILENO) < 0)
                _exit(2);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        argv[0] = (char *)exe;
        argv[1] = (char *)url;
        argv[2] = NULL;

        posix_spawnattr_init(&attr);
        sigemptyset(&empty);
        posix_spawnattr_setsigmask(&attr, &empty);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);

        if (search_path)
            rc = posix_spawnp(&child, exe, NULL, &attr, argv, environ);
        else
            rc = posix_spawn(&child, exe, NULL, &attr, argv, environ);

        posix_spawnattr_destroy(&attr);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Waits only for the short-lived intermediate child, never for the
       browser itself. */
    while (waitpid(mid, &status, 0) < 0) {
        if (errno != EINTR)
            return WEBDOC_ERR_SPAWN;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return WEBDOC_ERR_SPAWN;
    return WEBDOC_OK;
}
