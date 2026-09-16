/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

typedef struct {
    const char *p;
    const char *end;
    int         depth;
    const char *err;
} jctx;

static json_value *parse_value(jctx *c);

static json_value *jv_new(json_type t)
{
    json_value *v = calloc(1, sizeof(*v));
    if (v)
        v->type = t;
    return v;
}

void json_free(json_value *v)
{
    size_t i;

    if (!v)
        return;
    switch (v->type) {
    case JSON_STRING:
        free(v->u.string);
        break;
    case JSON_ARRAY:
        for (i = 0; i < v->u.array.count; i++)
            json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->u.object.count; i++) {
            free(v->u.object.items[i].key);
            json_free(v->u.object.items[i].value);
        }
        free(v->u.object.items);
        break;
    default:
        break;
    }
    free(v);
}

static void skip_ws(jctx *c)
{
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
            c->p++;
        else
            break;
    }
}

static int peek(jctx *c)
{
    return (c->p < c->end) ? (unsigned char)*c->p : -1;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char ch = s[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')
            v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            v |= (unsigned)(ch - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* Appends the UTF-8 encoding of a code point.  Returns bytes written. */
static size_t utf8_encode(unsigned cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a JSON string literal into a freshly allocated C string. */
static char *parse_string_raw(jctx *c)
{
    json_buf b;
    char tmp[4];

    if (peek(c) != '"') {
        c->err = "expected string";
        return NULL;
    }
    c->p++;
    json_buf_init(&b);

    while (1) {
        int ch;

        if (c->p >= c->end) {
            c->err = "unterminated string";
            goto fail;
        }
        ch = (unsigned char)*c->p++;

        if (ch == '"')
            break;

        if (ch < 0x20) {
            c->err = "control character in string";
            goto fail;
        }
        if (b.len > JSON_MAX_STRING) {
            c->err = "string too long";
            goto fail;
        }
        if (ch != '\\') {
            char cch = (char)ch;
            if (json_buf_append(&b, &cch, 1) != 0)
                goto fail;
            continue;
        }

        if (c->p >= c->end) {
            c->err = "unterminated escape";
            goto fail;
        }
        ch = (unsigned char)*c->p++;
        switch (ch) {
        case '"':  json_buf_append(&b, "\"", 1); break;
        case '\\': json_buf_append(&b, "\\", 1); break;
        case '/':  json_buf_append(&b, "/", 1);  break;
        case 'b':  json_buf_append(&b, "\b", 1); break;
        case 'f':  json_buf_append(&b, "\f", 1); break;
        case 'n':  json_buf_append(&b, "\n", 1); break;
        case 'r':  json_buf_append(&b, "\r", 1); break;
        case 't':  json_buf_append(&b, "\t", 1); break;
        case 'u': {
            unsigned cp;
            size_t n;

            if (c->end - c->p < 4 || hex4(c->p, &cp) != 0) {
                c->err = "bad \\u escape";
                goto fail;
            }
            c->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u' ||
                    hex4(c->p + 2, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) {
                    c->err = "unpaired surrogate";
                    goto fail;
                }
                c->p += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                c->err = "unpaired surrogate";
                goto fail;
            }
            if (cp == 0) {
                c->err = "embedded NUL";
                goto fail;
            }
            n = utf8_encode(cp, tmp);
            if (json_buf_append(&b, tmp, n) != 0)
                goto fail;
            break;
        }
        default:
            c->err = "bad escape";
            goto fail;
        }
        if (b.failed)
            goto fail;
    }

    if (json_buf_append(&b, "", 1) != 0)   /* NUL terminate */
        goto fail;
    return b.data;   /* ownership transfers */

fail:
    if (!c->err)
        c->err = "out of memory";
    json_buf_free(&b);
    return NULL;
}

static json_value *parse_number(jctx *c)
{
    char buf[64];
    size_t n = 0;
    const char *start = c->p;
    char *endp = NULL;
    json_value *v;
    double d;

    if (peek(c) == '-')
        c->p++;
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
                             *c->p == 'e' || *c->p == 'E' || *c->p == '+' ||
                             *c->p == '-'))
        c->p++;

    n = (size_t)(c->p - start);
    if (n == 0 || n >= sizeof(buf)) {
        c->err = "bad number";
        return NULL;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';

    /* strtod() accepts more than JSON does ("01", ".5", "1.", "0x10",
       "inf").  Enforce the RFC 8259 grammar first so two parsers cannot
       disagree about what a document means. */
    {
        const char *q = buf;

        if (*q == '-')
            q++;
        if (*q == '0') {
            q++;
        } else if (*q >= '1' && *q <= '9') {
            while (*q >= '0' && *q <= '9')
                q++;
        } else {
            c->err = "bad number";
            return NULL;
        }
        if (*q == '.') {
            q++;
            if (!(*q >= '0' && *q <= '9')) {
                c->err = "bad number";
                return NULL;
            }
            while (*q >= '0' && *q <= '9')
                q++;
        }
        if (*q == 'e' || *q == 'E') {
            q++;
            if (*q == '+' || *q == '-')
                q++;
            if (!(*q >= '0' && *q <= '9')) {
                c->err = "bad number";
                return NULL;
            }
            while (*q >= '0' && *q <= '9')
                q++;
        }
        if (*q != '\0') {
            c->err = "bad number";
            return NULL;
        }
    }

    errno = 0;
    d = strtod(buf, &endp);
    if (!endp || *endp != '\0' || errno == ERANGE) {
        c->err = "bad number";
        return NULL;
    }
    v = jv_new(JSON_NUMBER);
    if (!v) {
        c->err = "out of memory";
        return NULL;
    }
    v->u.number = d;
    return v;
}

static json_value *parse_literal(jctx *c)
{
    size_t avail = (size_t)(c->end - c->p);
    json_value *v;

    if (avail >= 4 && memcmp(c->p, "true", 4) == 0) {
        c->p += 4;
        v = jv_new(JSON_BOOL);
        if (v)
            v->u.boolean = 1;
    } else if (avail >= 5 && memcmp(c->p, "false", 5) == 0) {
        c->p += 5;
        v = jv_new(JSON_BOOL);
        if (v)
            v->u.boolean = 0;
    } else if (avail >= 4 && memcmp(c->p, "null", 4) == 0) {
        c->p += 4;
        v = jv_new(JSON_NULL);
    } else {
        c->err = "unexpected token";
        return NULL;
    }
    if (!v)
        c->err = "out of memory";
    return v;
}

static json_value *parse_array(jctx *c)
{
    json_value *v = jv_new(JSON_ARRAY);

    if (!v) {
        c->err = "out of memory";
        return NULL;
    }
    c->p++;                       /* '[' */
    skip_ws(c);
    if (peek(c) == ']') {
        c->p++;
        return v;
    }
    while (1) {
        json_value **grown;
        json_value  *item;

        if (v->u.array.count >= JSON_MAX_ITEMS) {
            c->err = "too many array items";
            goto fail;
        }
        item = parse_value(c);
        if (!item)
            goto fail;
        grown = realloc(v->u.array.items,
                        (v->u.array.count + 1) * sizeof(*grown));
        if (!grown) {
            json_free(item);
            c->err = "out of memory";
            goto fail;
        }
        v->u.array.items = grown;
        v->u.array.items[v->u.array.count++] = item;

        skip_ws(c);
        if (peek(c) == ',') {
            c->p++;
            skip_ws(c);
            continue;
        }
        if (peek(c) == ']') {
            c->p++;
            return v;
        }
        c->err = "expected ',' or ']'";
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static json_value *parse_object(jctx *c)
{
    json_value *v = jv_new(JSON_OBJECT);

    if (!v) {
        c->err = "out of memory";
        return NULL;
    }
    c->p++;                       /* '{' */
    skip_ws(c);
    if (peek(c) == '}') {
        c->p++;
        return v;
    }
    while (1) {
        json_member *grown;
        char        *key;
        json_value  *val;
        size_t       i;

        if (v->u.object.count >= JSON_MAX_ITEMS) {
            c->err = "too many object members";
            goto fail;
        }
        skip_ws(c);
        key = parse_string_raw(c);
        if (!key)
            goto fail;

        /* Duplicate keys are a classic parser-differential trick: reject. */
        for (i = 0; i < v->u.object.count; i++) {
            if (strcmp(v->u.object.items[i].key, key) == 0) {
                free(key);
                c->err = "duplicate object key";
                goto fail;
            }
        }

        skip_ws(c);
        if (peek(c) != ':') {
            free(key);
            c->err = "expected ':'";
            goto fail;
        }
        c->p++;
        val = parse_value(c);
        if (!val) {
            free(key);
            goto fail;
        }
        grown = realloc(v->u.object.items,
                        (v->u.object.count + 1) * sizeof(*grown));
        if (!grown) {
            free(key);
            json_free(val);
            c->err = "out of memory";
            goto fail;
        }
        v->u.object.items = grown;
        v->u.object.items[v->u.object.count].key = key;
        v->u.object.items[v->u.object.count].value = val;
        v->u.object.count++;

        skip_ws(c);
        if (peek(c) == ',') {
            c->p++;
            continue;
        }
        if (peek(c) == '}') {
            c->p++;
            return v;
        }
        c->err = "expected ',' or '}'";
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static json_value *parse_value(jctx *c)
{
    json_value *v;
    int ch;

    if (++c->depth > JSON_MAX_DEPTH) {
        c->err = "nesting too deep";
        c->depth--;
        return NULL;
    }
    skip_ws(c);
    ch = peek(c);
    if (ch < 0) {
        c->err = "unexpected end of input";
        c->depth--;
        return NULL;
    }
    if (ch == '{') {
        v = parse_object(c);
    } else if (ch == '[') {
        v = parse_array(c);
    } else if (ch == '"') {
        char *s = parse_string_raw(c);
        v = NULL;
        if (s) {
            v = jv_new(JSON_STRING);
            if (v)
                v->u.string = s;
            else
                free(s);
        }
    } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
        v = parse_number(c);
    } else {
        v = parse_literal(c);
    }
    c->depth--;
    return v;
}

int json_parse(const char *buf, size_t len, json_value **out, const char **err)
{
    jctx c;
    json_value *v;

    if (!buf || !out)
        return -1;
    *out = NULL;
    c.p = buf;
    c.end = buf + len;
    c.depth = 0;
    c.err = NULL;

    v = parse_value(&c);
    if (!v) {
        if (err)
            *err = c.err ? c.err : "parse error";
        return -1;
    }
    skip_ws(&c);
    if (c.p != c.end) {
        json_free(v);
        if (err)
            *err = "trailing garbage";
        return -1;
    }
    *out = v;
    return 0;
}

const json_value *json_object_get(const json_value *obj, const char *key)
{
    size_t i;

    if (!obj || obj->type != JSON_OBJECT || !key)
        return NULL;
    for (i = 0; i < obj->u.object.count; i++)
        if (strcmp(obj->u.object.items[i].key, key) == 0)
            return obj->u.object.items[i].value;
    return NULL;
}

/* ------------------------------------------------------------ serialising - */

void json_buf_init(json_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

void json_buf_free(json_buf *b)
{
    free(b->data);
    json_buf_init(b);
}

int json_buf_append(json_buf *b, const char *s, size_t n)
{
    if (b->failed)
        return -1;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 128;
        char *grown;

        while (cap < b->len + n + 1)
            cap *= 2;
        grown = realloc(b->data, cap);
        if (!grown) {
            b->failed = 1;
            return -1;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int json_buf_appendz(json_buf *b, const char *s)
{
    return json_buf_append(b, s, strlen(s));
}

int json_escape_into(json_buf *b, const char *s)
{
    char esc[8];

    json_buf_append(b, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;

        switch (ch) {
        case '"':  json_buf_append(b, "\\\"", 2); break;
        case '\\': json_buf_append(b, "\\\\", 2); break;
        case '\b': json_buf_append(b, "\\b", 2);  break;
        case '\f': json_buf_append(b, "\\f", 2);  break;
        case '\n': json_buf_append(b, "\\n", 2);  break;
        case '\r': json_buf_append(b, "\\r", 2);  break;
        case '\t': json_buf_append(b, "\\t", 2);  break;
        default:
            if (ch < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", ch);
                json_buf_appendz(b, esc);
            } else {
                json_buf_append(b, (const char *)&ch, 1);
            }
        }
    }
    json_buf_append(b, "\"", 1);
    return b->failed ? -1 : 0;
}

int json_serialize(const json_value *v, json_buf *b)
{
    char num[40];
    size_t i;

    if (!v)
        return -1;
    switch (v->type) {
    case JSON_NULL:
        json_buf_appendz(b, "null");
        break;
    case JSON_BOOL:
        json_buf_appendz(b, v->u.boolean ? "true" : "false");
        break;
    case JSON_NUMBER:
        if (v->u.number == (double)(long long)v->u.number)
            snprintf(num, sizeof(num), "%lld", (long long)v->u.number);
        else
            snprintf(num, sizeof(num), "%.17g", v->u.number);
        json_buf_appendz(b, num);
        break;
    case JSON_STRING:
        json_escape_into(b, v->u.string);
        break;
    case JSON_ARRAY:
        json_buf_append(b, "[", 1);
        for (i = 0; i < v->u.array.count; i++) {
            if (i)
                json_buf_append(b, ",", 1);
            json_serialize(v->u.array.items[i], b);
        }
        json_buf_append(b, "]", 1);
        break;
    case JSON_OBJECT:
        json_buf_append(b, "{", 1);
        for (i = 0; i < v->u.object.count; i++) {
            if (i)
                json_buf_append(b, ",", 1);
            json_escape_into(b, v->u.object.items[i].key);
            json_buf_append(b, ":", 1);
            json_serialize(v->u.object.items[i].value, b);
        }
        json_buf_append(b, "}", 1);
        break;
    }
    return b->failed ? -1 : 0;
}
