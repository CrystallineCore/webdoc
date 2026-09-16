/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "internal.h"

#include <ctype.h>
#include <strings.h>
#include <string.h>

const char *webdoc_strerror(webdoc_err err)
{
    switch (err) {
    case WEBDOC_OK:                      return "success";
    case WEBDOC_ERR_INVALID_ARG:         return "invalid argument";
    case WEBDOC_ERR_INVALID_URL:         return "invalid URL";
    case WEBDOC_ERR_UNSUPPORTED_SCHEME:  return "unsupported URL scheme (only http:// and https:// are supported)";
    case WEBDOC_ERR_INVALID_FILE:        return "not a valid web document";
    case WEBDOC_ERR_UNSUPPORTED_VERSION: return "unsupported web document format version";
    case WEBDOC_ERR_NOT_FOUND:           return "no such file or directory";
    case WEBDOC_ERR_EXISTS:              return "file already exists";
    case WEBDOC_ERR_NOT_A_DOCUMENT:      return "not a regular file";
    case WEBDOC_ERR_IS_DIRECTORY:        return "is a directory";
    case WEBDOC_ERR_LOCKED:              return "locked";
    case WEBDOC_ERR_INVALID_BROWSER:     return "invalid browser configuration";
    case WEBDOC_ERR_VAR_NOT_FOUND:       return "variable is not defined";
    case WEBDOC_ERR_PERMISSION:          return "permission denied";
    case WEBDOC_ERR_TOO_LARGE:           return "value or file too large";
    case WEBDOC_ERR_IO:                  return "I/O error";
    case WEBDOC_ERR_NOMEM:               return "out of memory";
    case WEBDOC_ERR_SPAWN:               return "could not launch browser";
    case WEBDOC_ERR_INTERNAL:            return "internal error";
    }
    return "unknown error";
}

/*
 * URL policy.
 *
 * URLs are untrusted input that later becomes argv[1] of a browser process,
 * so the parser is deliberately conservative:
 *
 *   - only http:// and https://
 *   - bounded length
 *   - no control characters, no whitespace, no bytes >= 0x7F: these are the
 *     characters that smuggle newlines into .desktop files, terminals and
 *     logs.  Non-ASCII must be percent-encoded or IDN-encoded by the caller.
 *   - no characters that RFC 3986 excludes from URIs anyway
 *     (" \"<>\\^`{|}")
 *   - a non-empty host, restricted to the host character set
 *   - no userinfo ('@' in the authority): credentials in a stored file are a
 *     phishing and secret-leak hazard, and are not needed.
 *
 * The URL is never concatenated into a shell command anywhere in libwebdoc,
 * so this validation is defence in depth, not the only line of defence.
 */
webdoc_err webdoc_url_validate(const char *url)
{
    const char *rest, *host_end, *p;
    size_t len, host_len;

    if (!url)
        return WEBDOC_ERR_INVALID_ARG;

    len = strlen(url);
    if (len == 0)
        return WEBDOC_ERR_INVALID_URL;
    if (len > WEBDOC_MAX_URL_LEN)
        return WEBDOC_ERR_TOO_LARGE;

    if (strncasecmp(url, "https://", 8) == 0)
        rest = url + 8;
    else if (strncasecmp(url, "http://", 7) == 0)
        rest = url + 7;
    else
        return WEBDOC_ERR_UNSUPPORTED_SCHEME;

    for (p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x21 || c >= 0x7F)
            return WEBDOC_ERR_INVALID_URL;
        if (strchr("\"<>\\^`{|}", c))
            return WEBDOC_ERR_INVALID_URL;
    }

    host_end = rest;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;
    host_len = (size_t)(host_end - rest);
    if (host_len == 0 || host_len > 255)
        return WEBDOC_ERR_INVALID_URL;

    for (p = rest; p < host_end; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '@')
            return WEBDOC_ERR_INVALID_URL;      /* no userinfo */
        if (!(isalnum(c) || strchr("-._~%:[]", c)))
            return WEBDOC_ERR_INVALID_URL;
    }
    return WEBDOC_OK;
}

/*
 * Variables are a general name -> value mapping in global configuration.
 * Browser selection is the first consumer, not the only possible one, so
 * nothing here is browser-specific: a value is validated as an executable
 * path only at the point where it is about to be used as one.
 *
 * Names become object keys in the config file and are referenced from .web
 * files, so keep them to a small, obviously safe character set.
 */
webdoc_err webdoc_variable_name_validate(const char *name)
{
    size_t i;

    if (!name || !name[0])
        return WEBDOC_ERR_INVALID_ARG;
    if (strlen(name) > WEBDOC_MAX_VAR_NAME_LEN)
        return WEBDOC_ERR_TOO_LARGE;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '-'))
            return WEBDOC_ERR_INVALID_ARG;
    }
    return WEBDOC_OK;
}

/*
 * Variable values are arbitrary text, bounded and free of control
 * characters: a value may end up in a terminal, a log line or an argv
 * element, and embedded newlines or escape sequences are how text turns into
 * something the reader did not intend.
 */
webdoc_err webdoc_variable_value_validate(const char *value)
{
    size_t i;

    if (!value)
        return WEBDOC_ERR_INVALID_ARG;
    if (strlen(value) >= WEBDOC_MAX_PATH)
        return WEBDOC_ERR_TOO_LARGE;
    for (i = 0; value[i]; i++)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] == 0x7F)
            return WEBDOC_ERR_INVALID_ARG;
    return WEBDOC_OK;
}
