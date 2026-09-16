/*
 * json.h - minimal, strict, hardened JSON reader/writer for libwebdoc.
 *
 * Deliberately small: .web files are tiny, so a vendored ~400 line parser
 * removes a runtime dependency from packaging while giving us full control
 * over the hardening limits (depth, member count, string length, no
 * duplicate keys, no trailing garbage).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WEBDOC_JSON_H
#define WEBDOC_JSON_H

#include <stddef.h>

#define JSON_MAX_DEPTH    32
#define JSON_MAX_ITEMS    1024
#define JSON_MAX_STRING   (16u * 1024u)

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;

typedef struct {
    char       *key;
    json_value *value;
} json_member;

struct json_value {
    json_type type;
    union {
        int    boolean;
        double number;
        char  *string;              /* NUL terminated, no embedded NULs     */
        struct { json_value **items; size_t count; } array;
        struct { json_member  *items; size_t count; } object;
    } u;
};

/* Parses a complete document.  Returns 0 on success.  *err (optional) gets a
   static description.  Trailing non-whitespace is rejected. */
int  json_parse(const char *buf, size_t len, json_value **out, const char **err);
void json_free(json_value *v);

const json_value *json_object_get(const json_value *obj, const char *key);

/* Dynamic string buffer used for serialisation. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} json_buf;

void json_buf_init(json_buf *b);
void json_buf_free(json_buf *b);
int  json_buf_append(json_buf *b, const char *s, size_t n);
int  json_buf_appendz(json_buf *b, const char *s);
int  json_escape_into(json_buf *b, const char *s);   /* writes "quoted" form */
int  json_serialize(const json_value *v, json_buf *b);

#endif /* WEBDOC_JSON_H */
