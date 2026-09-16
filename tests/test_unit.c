/* SPDX-License-Identifier: MIT
 *
 * Unit tests for libwebdoc.  Runs entirely inside a private temporary
 * directory with XDG_* and HOME redirected, so it never touches the real
 * configuration of whoever runs `make check`.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "webdoc.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;
static int checks;
static char tmpdir[] = "/tmp/web-test-XXXXXX";

#define CHECK(cond) do {                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                            \
    } while (0)

#define CHECK_EQ(expr, expected) do {                                \
        webdoc_err rc_ = (expr);                                       \
        checks++;                                                    \
        if (rc_ != (expected)) {                                     \
            failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: %s -> %s (wanted %s)\n",    \
                    __FILE__, __LINE__, #expr, webdoc_strerror(rc_),   \
                    webdoc_strerror(expected));                        \
        }                                                            \
    } while (0)

static char *tpath(const char *rel)
{
    static char buf[4096];

    snprintf(buf, sizeof(buf), "%s/%s", tmpdir, rel);
    return buf;
}

/* ------------------------------------------------------------------ URLs -- */

static void test_url(void)
{
    CHECK_EQ(webdoc_url_validate("https://www.postgresql.org/"), WEBDOC_OK);
    CHECK_EQ(webdoc_url_validate("http://example.com"), WEBDOC_OK);
    CHECK_EQ(webdoc_url_validate("HTTPS://EXAMPLE.COM/x"), WEBDOC_OK);
    CHECK_EQ(webdoc_url_validate("https://example.com/a?b=1&c=%20"), WEBDOC_OK);
    CHECK_EQ(webdoc_url_validate("https://[2001:db8::1]:8443/x"), WEBDOC_OK);

    CHECK_EQ(webdoc_url_validate("ftp://example.com/"), WEBDOC_ERR_UNSUPPORTED_SCHEME);
    CHECK_EQ(webdoc_url_validate("file:///etc/passwd"), WEBDOC_ERR_UNSUPPORTED_SCHEME);
    CHECK_EQ(webdoc_url_validate("javascript:alert(1)"), WEBDOC_ERR_UNSUPPORTED_SCHEME);
    CHECK_EQ(webdoc_url_validate("https://"), WEBDOC_ERR_INVALID_URL);
    CHECK_EQ(webdoc_url_validate("https://exa mple.com/"), WEBDOC_ERR_INVALID_URL);
    CHECK_EQ(webdoc_url_validate("https://example.com/\nX-Evil: 1"), WEBDOC_ERR_INVALID_URL);
    CHECK_EQ(webdoc_url_validate("https://example.com/`id`"), WEBDOC_ERR_INVALID_URL);
    CHECK_EQ(webdoc_url_validate("https://user:pw@example.com/"), WEBDOC_ERR_INVALID_URL);
    CHECK_EQ(webdoc_url_validate(""), WEBDOC_ERR_INVALID_URL);
}

/* ------------------------------------------------------------ name rules -- */

static void test_resolve(void)
{
    char out[WEBDOC_MAX_PATH];

    CHECK_EQ(webdoc_resolve_name("postgres", 0, out, sizeof(out)), WEBDOC_OK);
    CHECK(strcmp(out, "postgres.web") == 0);

    CHECK_EQ(webdoc_resolve_name("postgres.web", 0, out, sizeof(out)), WEBDOC_OK);
    CHECK(strcmp(out, "postgres.web") == 0);      /* never postgres.web.web */

    CHECK_EQ(webdoc_resolve_name("/a/b/postgres", 0, out, sizeof(out)), WEBDOC_OK);
    CHECK(strcmp(out, "/a/b/postgres.web") == 0);

    setenv("HOME", "/home/someone", 1);
    CHECK_EQ(webdoc_resolve_name("~/Bookmarks/pg", 0, out, sizeof(out)), WEBDOC_OK);
    CHECK(strcmp(out, "/home/someone/Bookmarks/pg.web") == 0);
    setenv("HOME", tmpdir, 1);

    /* --path is verbatim: no suffix, no expansion. */
    CHECK_EQ(webdoc_resolve_name("/tmp/x", 1, out, sizeof(out)), WEBDOC_OK);
    CHECK(strcmp(out, "/tmp/x") == 0);

    CHECK_EQ(webdoc_resolve_name("", 0, out, sizeof(out)), WEBDOC_ERR_INVALID_ARG);
    CHECK_EQ(webdoc_resolve_name("dir/", 0, out, sizeof(out)), WEBDOC_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ JSON -- */

static void test_json(void)
{
    json_value *v;
    const char *err;
    char deep[4096];
    size_t i;

    CHECK(json_parse("{\"a\":1}", 7, &v, &err) == 0);
    json_free(v);

    /* Hostile or sloppy documents must be rejected, not tolerated. */
    CHECK(json_parse("{\"a\":1", 6, &v, &err) != 0);
    CHECK(json_parse("{\"a\":1} trailing", 16, &v, &err) != 0);
    CHECK(json_parse("{\"a\":1,\"a\":2}", 13, &v, &err) != 0);   /* duplicate key */
    CHECK(json_parse("{'a':1}", 7, &v, &err) != 0);
    CHECK(json_parse("{\"a\":01}", 8, &v, &err) != 0);          /* leading zero */
    CHECK(json_parse("{\"a\":1.}", 8, &v, &err) != 0);          /* bad number  */
    CHECK(json_parse("[1,]", 4, &v, &err) != 0);                /* trailing comma */
    CHECK(json_parse("\"\\ud800\"", 8, &v, &err) != 0);          /* lone surrogate */
    CHECK(json_parse("\"a\tb\"", 5, &v, &err) != 0);             /* raw control */

    for (i = 0; i < sizeof(deep) - 1; i++)
        deep[i] = '[';
    deep[sizeof(deep) - 1] = '\0';
    CHECK(json_parse(deep, strlen(deep), &v, &err) != 0);        /* depth limit */
}

/* -------------------------------------------------------- file lifecycle -- */

static void test_lifecycle(void)
{
    webdoc_create_params cp;
    webdoc_edit_params ep;
    webdoc_t doc;
    char *path = strdup(tpath("pg.web"));
    struct stat st;

    memset(&cp, 0, sizeof(cp));
    cp.url = "https://www.postgresql.org/";
    CHECK_EQ(webdoc_create(path, &cp), WEBDOC_OK);

    /* create never overwrites */
    CHECK_EQ(webdoc_create(path, &cp), WEBDOC_ERR_EXISTS);

    CHECK_EQ(webdoc_load(path, &doc), WEBDOC_OK);
    CHECK(doc.version == 1);
    CHECK(strcmp(doc.url, "https://www.postgresql.org/") == 0);
    CHECK(doc.browser_kind == WEBDOC_BROWSER_DEFAULT);
    CHECK(doc.locked == 0);
    webdoc_reset(&doc);

    /* invalid URLs are refused at creation time */
    cp.url = "ftp://example.com/";
    CHECK_EQ(webdoc_create(tpath("bad.web"), &cp), WEBDOC_ERR_UNSUPPORTED_SCHEME);
    CHECK(stat(tpath("bad.web"), &st) != 0);

    memset(&ep, 0, sizeof(ep));
    ep.locked = WEBDOC_EDIT_KEEP;
    ep.set_browser = 1;
    ep.browser_kind = WEBDOC_BROWSER_VARIABLE;
    ep.browser_ref = "chrome";
    CHECK_EQ(webdoc_edit(path, &ep), WEBDOC_OK);

    CHECK_EQ(webdoc_load(path, &doc), WEBDOC_OK);
    CHECK(doc.browser_kind == WEBDOC_BROWSER_VARIABLE);
    CHECK(strcmp(doc.browser_ref, "chrome") == 0);
    webdoc_reset(&doc);

    /* literal path variant */
    memset(&ep, 0, sizeof(ep));
    ep.locked = WEBDOC_EDIT_KEEP;
    ep.set_browser = 1;
    ep.browser_kind = WEBDOC_BROWSER_PATH;
    ep.browser_ref = "relative/chrome";
    CHECK_EQ(webdoc_edit(path, &ep), WEBDOC_ERR_INVALID_BROWSER);

    CHECK_EQ(webdoc_delete(path), WEBDOC_OK);
    CHECK_EQ(webdoc_delete(path), WEBDOC_ERR_NOT_FOUND);
    free(path);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_hostile_files(void)
{
    webdoc_t doc;

    write_file(tpath("h1.web"), "not json at all");
    CHECK_EQ(webdoc_load(tpath("h1.web"), &doc), WEBDOC_ERR_INVALID_FILE);

    write_file(tpath("h2.web"), "{\"version\":99,\"url\":\"https://a.example/\"}");
    CHECK_EQ(webdoc_load(tpath("h2.web"), &doc), WEBDOC_ERR_UNSUPPORTED_VERSION);

    write_file(tpath("h3.web"), "{\"version\":1}");
    CHECK_EQ(webdoc_load(tpath("h3.web"), &doc), WEBDOC_ERR_INVALID_FILE);

    write_file(tpath("h4.web"), "{\"version\":1,\"url\":\"file:///etc/shadow\"}");
    CHECK_EQ(webdoc_load(tpath("h4.web"), &doc), WEBDOC_ERR_UNSUPPORTED_SCHEME);

    write_file(tpath("h5.web"),
               "{\"version\":1,\"url\":\"https://a.example/\",\"locked\":\"yes\"}");
    CHECK_EQ(webdoc_load(tpath("h5.web"), &doc), WEBDOC_ERR_INVALID_FILE);

    write_file(tpath("h6.web"),
               "{\"version\":1,\"url\":\"https://a.example/\","
               "\"browser\":\"/bin/sh\"}");
    CHECK_EQ(webdoc_load(tpath("h6.web"), &doc), WEBDOC_ERR_INVALID_BROWSER);

    /* Unknown members inside version 1 survive an edit. */
    write_file(tpath("h7.web"),
               "{\"version\":1,\"url\":\"https://a.example/\",\"note\":\"keep me\"}");
    {
        webdoc_edit_params ep;
        char *buf;
        FILE *f;
        char text[512] = {0};

        memset(&ep, 0, sizeof(ep));
        ep.locked = 1;
        CHECK_EQ(webdoc_edit(tpath("h7.web"), &ep), WEBDOC_OK);
        f = fopen(tpath("h7.web"), "r");
        CHECK(f != NULL);
        if (f) {
            buf = fgets(text, sizeof(text), f);
            (void)buf;
            if (fread(text + strlen(text), 1,
                      sizeof(text) - strlen(text) - 1, f) == 0)
                text[strlen(text)] = '\0';
            fclose(f);
        }
        CHECK(strstr(text, "keep me") != NULL);
    }
}

/* ----------------------------------------------------- locks & variables -- */

static void test_locks_and_vars(void)
{
    webdoc_create_params cp;
    char *value = NULL;
    int g = -1, l = -1;

    memset(&cp, 0, sizeof(cp));
    cp.url = "https://example.com/";
    CHECK_EQ(webdoc_create(tpath("lk.web"), &cp), WEBDOC_OK);

    CHECK_EQ(webdoc_lock(tpath("lk.web")), WEBDOC_OK);
    CHECK_EQ(webdoc_lock_state(tpath("lk.web"), &g, &l), WEBDOC_OK);
    CHECK(g == 0 && l == 1);

    /* Global unlock must not clear a per-document lock. */
    CHECK_EQ(webdoc_unlock(NULL), WEBDOC_OK);
    CHECK_EQ(webdoc_lock_state(tpath("lk.web"), &g, &l), WEBDOC_OK);
    CHECK(g == 0 && l == 1);

    /* Per-web document unlock must not clear the global lock. */
    CHECK_EQ(webdoc_lock(NULL), WEBDOC_OK);
    CHECK_EQ(webdoc_unlock(tpath("lk.web")), WEBDOC_OK);
    CHECK_EQ(webdoc_lock_state(tpath("lk.web"), &g, &l), WEBDOC_OK);
    CHECK(g == 1 && l == 0);

    /* Either scope denies opening. */
    CHECK_EQ(webdoc_open(tpath("lk.web")), WEBDOC_ERR_LOCKED);
    CHECK_EQ(webdoc_unlock(NULL), WEBDOC_OK);
    CHECK_EQ(webdoc_lock(tpath("lk.web")), WEBDOC_OK);
    CHECK_EQ(webdoc_open(tpath("lk.web")), WEBDOC_ERR_LOCKED);
    CHECK_EQ(webdoc_unlock(tpath("lk.web")), WEBDOC_OK);

    /* A copied web document keeps its own lock state. */
    CHECK_EQ(webdoc_lock(tpath("lk.web")), WEBDOC_OK);
    {
        char cmd[4096];
        webdoc_t doc;

        snprintf(cmd, sizeof(cmd), "%s/copy.web", tmpdir);
        {
            FILE *in = fopen(tpath("lk.web"), "r");
            FILE *out = fopen(cmd, "w");
            int ch;

            CHECK(in && out);
            if (in && out) {
                while ((ch = fgetc(in)) != EOF)
                    fputc(ch, out);
            }
            if (in) fclose(in);
            if (out) fclose(out);
        }
        CHECK_EQ(webdoc_load(cmd, &doc), WEBDOC_OK);
        CHECK(doc.locked == 1);
        webdoc_reset(&doc);
    }

    CHECK_EQ(webdoc_set_variable("chrome", "/usr/bin/chromium"), WEBDOC_OK);
    CHECK_EQ(webdoc_get_variable("chrome", &value), WEBDOC_OK);
    CHECK(value && strcmp(value, "/usr/bin/chromium") == 0);
    free(value);
    value = NULL;

    /* Variables are indirections: changing one changes every web document at once. */
    CHECK_EQ(webdoc_set_variable("chrome", "/usr/bin/google-chrome"), WEBDOC_OK);
    CHECK_EQ(webdoc_get_variable("chrome", &value), WEBDOC_OK);
    CHECK(value && strcmp(value, "/usr/bin/google-chrome") == 0);
    free(value);

    /* Variables are general name/value pairs: any sane text is accepted.
       Whether a value is usable as a browser is decided where it is used. */
    CHECK_EQ(webdoc_set_variable("editor", "nvim --clean"), WEBDOC_OK);
    CHECK_EQ(webdoc_set_variable("relative", "relative/path"), WEBDOC_OK);
    CHECK_EQ(webdoc_set_variable("bad name", "/usr/bin/x"), WEBDOC_ERR_INVALID_ARG);
    CHECK_EQ(webdoc_set_variable("nl", "/usr/bin/x\ny"), WEBDOC_ERR_INVALID_ARG);
    CHECK_EQ(webdoc_unset_variable("editor"), WEBDOC_OK);

    /* ... and a non-absolute value is rejected at open time, not stored. */
    {
        webdoc_edit_params ep;

        memset(&ep, 0, sizeof(ep));
        ep.locked = 0;
        ep.set_browser = 1;
        ep.browser_kind = WEBDOC_BROWSER_VARIABLE;
        ep.browser_ref = "relative";
        CHECK_EQ(webdoc_edit(tpath("lk.web"), &ep), WEBDOC_OK);
        CHECK_EQ(webdoc_open(tpath("lk.web")), WEBDOC_ERR_INVALID_BROWSER);
    }
    CHECK_EQ(webdoc_unset_variable("relative"), WEBDOC_OK);
    CHECK_EQ(webdoc_unset_variable("chrome"), WEBDOC_OK);
    CHECK_EQ(webdoc_unset_variable("chrome"), WEBDOC_ERR_VAR_NOT_FOUND);
    CHECK_EQ(webdoc_get_variable("chrome", &value), WEBDOC_ERR_VAR_NOT_FOUND);
}

/* ---------------------------------------------------------------- listing - */

static void test_listing(void)
{
    webdoc_list list;
    char dir[4096];
    webdoc_create_params cp;

    snprintf(dir, sizeof(dir), "%s/ls", tmpdir);
    CHECK(mkdir(dir, 0700) == 0);

    memset(&cp, 0, sizeof(cp));
    cp.url = "https://b.example/";
    {
        char p[8192];
        snprintf(p, sizeof(p), "%s/bbb.web", dir);
        CHECK_EQ(webdoc_create(p, &cp), WEBDOC_OK);
        snprintf(p, sizeof(p), "%s/aaa.web", dir);
        CHECK_EQ(webdoc_create(p, &cp), WEBDOC_OK);
        snprintf(p, sizeof(p), "%s/ignored.txt", dir);
        write_file(p, "not a link");
    }
    CHECK_EQ(webdoc_list_dir(dir, &list), WEBDOC_OK);
    CHECK(list.count == 2);
    CHECK(list.count == 2 && strcmp(list.items[0].name, "aaa") == 0);
    CHECK(list.count == 2 && strcmp(list.items[1].name, "bbb") == 0);
    webdoc_list_free(&list);

    CHECK_EQ(webdoc_list_dir(tpath("nope"), &list), WEBDOC_ERR_NOT_FOUND);
}

int main(void)
{
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("HOME", tmpdir, 1);
    setenv("XDG_CONFIG_HOME", tpath("cfg"), 1);
    setenv("XDG_STATE_HOME", tpath("state"), 1);

    test_url();
    test_resolve();
    test_json();
    test_lifecycle();
    test_hostile_files();
    test_locks_and_vars();
    test_listing();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        char cmd[4200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
        if (system(cmd) != 0)
            fprintf(stderr, "note: could not clean %s\n", tmpdir);
    } else {
        fprintf(stderr, "test data left in %s\n", tmpdir);
    }
    return failures ? 1 : 0;
}
