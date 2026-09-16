/* SPDX-License-Identifier: MIT
 *
 * web - command line frontend for libwebdoc.
 *
 * The CLI contains no document logic: it parses argv, calls libwebdoc, and
 * turns webdoc_err values into user facing messages and exit codes.
 *
 * Output discipline:
 *   stdout - results the user asked for (a document, a listing, variables)
 *   stderr - errors and, under --verbose, progress reporting
 * so that `web show x | ...` and `web variables | ...` stay clean whether or
 * not --verbose is in effect.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "webdoc.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>

#ifndef WEBDOC_VERSION
#define WEBDOC_VERSION "1.0.0"
#endif

#define EXIT_FAIL    1
#define EXIT_USAGE   2
#define EXIT_LOCKED  3

static const char *prog = "web";

static void usage(FILE *out)
{
    fprintf(out,
"Usage: web <command> [options]\n"
"\n"
"Commands:\n"
"  create <name> <url> [options]   create <name>.web\n"
"  edit   <name> [url] [options]   modify an existing document\n"
"  delete <name>                   remove a document\n"
"  open   <name>                   open a document in a browser\n"
"  show   <name>                   print one document\n"
"  list   [directory]              list the documents in a directory\n"
"  lock   [name]                   lock one document, or all (no argument)\n"
"  unlock [name]                   unlock one document, or all\n"
"  set    <variable> <value>       define a variable\n"
"  unset  <variable>               remove a variable\n"
"  variables                       list the defined variables\n"
"  config                          show the global configuration\n"
"\n"
"Options:\n"
"  --browser <variable>      open with the browser named by this variable\n"
"  --browser-path <path>     open with this exact browser executable\n"
"  --default-browser         (edit) go back to the system default browser\n"
"  --locked                  (create) create the document already locked\n"
"  --lock, --unlock          (edit) change the lock state\n"
"  --path                    treat the argument as a literal filesystem path\n"
"  -v, --verbose             report each step on standard error\n"
"  -h, --help                show this help\n"
"  -V, --version             show version\n"
"\n"
"A reference without a .web suffix gets one; with --path the argument is\n"
"used exactly as given.\n"
"\n"
"Examples:\n"
"  web set chrome /usr/bin/google-chrome\n"
"  web create docs https://docs.example.com/ --browser chrome\n"
"  web open docs\n");
}

static int fail(webdoc_err rc, const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", prog, what, webdoc_strerror(rc));
    return rc == WEBDOC_ERR_LOCKED ? EXIT_LOCKED : EXIT_FAIL;
}

/* ------------------------------------------------------------- listing ---- */

static void mode_string(mode_t m, char *buf)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    buf[0] = S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-');
    for (i = 0; i < 9; i++)
        buf[i + 1] = (m & (1 << (8 - i))) ? rwx[i] : '-';
    buf[10] = '\0';
}

/* The storage tag ("variable:chrome") is a file format detail; the user
   wrote "chrome" and that is what is printed back. */
static const char *browser_display(const char *browser)
{
    if (!browser)
        return NULL;
    if (strncmp(browser, "variable:", 9) == 0)
        return browser + 9;
    if (strncmp(browser, "path:", 5) == 0)
        return browser + 5;
    return browser;
}

static void print_list(const webdoc_list *list, int global_locked)
{
    char modes[11];
    char timebuf[32];
    size_t i;

    if (global_locked)
        printf("global lock: active\n");

    for (i = 0; i < list->count; i++) {
        const webdoc_entry *e = &list->items[i];
        struct passwd *pw = getpwuid(e->uid);
        struct group  *gr = getgrgid(e->gid);
        struct tm tm;

        mode_string(e->mode, modes);
        if (localtime_r(&e->mtime, &tm))
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", &tm);
        else
            snprintf(timebuf, sizeof(timebuf), "?");

        printf("%s %-8s %-8s %6lld %s %-20s ",
               modes,
               pw ? pw->pw_name : "?",
               gr ? gr->gr_name : "?",
               (long long)e->size, timebuf, e->name);

        if (e->status != WEBDOC_OK) {
            printf("<%s>", webdoc_strerror(e->status));
        } else {
            const char *browser = browser_display(e->browser);

            printf("%s%s%s%s", e->url,
                   e->locked ? "  [locked]" : "",
                   browser ? "  " : "",
                   browser ? browser : "");
        }
        putchar('\n');
    }
}

/* Expands a leading "~/" so `web list '~/Web'` works even when the shell did
   not expand it (quoted argument). */
static void expand_tilde(const char *in, char *out, size_t outlen)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home && home[0] == '/')
        snprintf(out, outlen, "%s%s", home, in + 1);
    else
        snprintf(out, outlen, "%s", in);
}

/* ------------------------------------------------------------- options ---- */

enum {
    OPT_BROWSER = 1000,
    OPT_BROWSER_PATH,
    OPT_DEFAULT_BROWSER,
    OPT_LOCKED,
    OPT_PATH,
    OPT_LOCK,
    OPT_UNLOCK
};

static struct option long_opts[] = {
    { "browser",         required_argument, NULL, OPT_BROWSER },
    { "browser-path",    required_argument, NULL, OPT_BROWSER_PATH },
    { "default-browser", no_argument,       NULL, OPT_DEFAULT_BROWSER },
    { "locked",          no_argument,       NULL, OPT_LOCKED },
    { "path",            no_argument,       NULL, OPT_PATH },
    { "verbose",         no_argument,       NULL, 'v' },
    { "lock",            no_argument,       NULL, OPT_LOCK },
    { "unlock",          no_argument,       NULL, OPT_UNLOCK },
    { "help",            no_argument,       NULL, 'h' },
    { "version",         no_argument,       NULL, 'V' },
    { NULL, 0, NULL, 0 }
};

typedef struct {
    const char *browser;
    const char *browser_path;
    int         default_browser;
    int         locked_flag;
    int         lock_flag;
    int         unlock_flag;
    int         explicit_path;
    int         verbose;
} options;

/* Set by a --verbose given before the subcommand, e.g. `web -v open docs`. */
static int global_verbose;

static void apply_verbose(const options *o)
{
    if (o->verbose || global_verbose) {
        webdoc_set_report_stream(stderr);
        webdoc_set_verbose(1);
    }
}

/* Parses options for a subcommand; leaves positional arguments in argv. */
static int parse_opts(int argc, char **argv, options *o)
{
    int c;

    memset(o, 0, sizeof(*o));
    optind = 1;
    while ((c = getopt_long(argc, argv, "hVv", long_opts, NULL)) != -1) {
        switch (c) {
        case OPT_BROWSER:         o->browser = optarg; break;
        case OPT_BROWSER_PATH:    o->browser_path = optarg; break;
        case OPT_DEFAULT_BROWSER: o->default_browser = 1; break;
        case OPT_LOCKED:          o->locked_flag = 1; break;
        case OPT_PATH:            o->explicit_path = 1; break;
        case OPT_LOCK:            o->lock_flag = 1; break;
        case OPT_UNLOCK:          o->unlock_flag = 1; break;
        case 'v': o->verbose = 1; break;
        case 'h': usage(stdout); exit(0);
        case 'V': printf("web (webdoc) %s\n", WEBDOC_VERSION); exit(0);
        default:
            usage(stderr);
            return -1;
        }
    }
    if (o->browser && o->browser_path) {
        fprintf(stderr, "%s: --browser and --browser-path are mutually exclusive\n", prog);
        return -1;
    }
    if (o->default_browser && (o->browser || o->browser_path)) {
        fprintf(stderr, "%s: --default-browser conflicts with --browser/--browser-path\n", prog);
        return -1;
    }
    if (o->lock_flag && o->unlock_flag) {
        fprintf(stderr, "%s: --lock and --unlock are mutually exclusive\n", prog);
        return -1;
    }
    apply_verbose(o);
    return 0;
}

/* Resolves a user reference and reports the resolution under --verbose. */
static webdoc_err resolve(const char *ref, int explicit_path,
                          char *out, size_t outlen)
{
    webdoc_err rc = webdoc_resolve_name(ref, explicit_path, out, outlen);

    if (rc == WEBDOC_OK && webdoc_verbose() && strcmp(ref, out) != 0)
        fprintf(stderr, "%s: '%s' refers to %s\n", prog, ref, out);
    return rc;
}

/* ------------------------------------------------------------- commands --- */

static int cmd_create(int argc, char **argv)
{
    options o;
    webdoc_create_params p;
    char path[WEBDOC_MAX_PATH];
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 2) {
        fprintf(stderr, "%s: usage: web create <name> <url> [options]\n", prog);
        return EXIT_USAGE;
    }

    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);

    memset(&p, 0, sizeof(p));
    p.url = argv[optind + 1];
    p.locked = o.locked_flag;
    if (o.browser) {
        p.browser_kind = WEBDOC_BROWSER_VARIABLE;
        p.browser_ref = o.browser;
    } else if (o.browser_path) {
        p.browser_kind = WEBDOC_BROWSER_PATH;
        p.browser_ref = o.browser_path;
    }

    rc = webdoc_create(path, &p);
    if (rc == WEBDOC_ERR_EXISTS) {
        fprintf(stderr, "%s: %s already exists; use `web edit` to modify it\n",
                prog, path);
        return EXIT_FAIL;
    }
    if (rc != WEBDOC_OK)
        return fail(rc, path);

    if (o.browser) {
        char *value = NULL;

        if (webdoc_get_variable(o.browser, &value) != WEBDOC_OK)
            fprintf(stderr,
                    "%s: '%s' is not defined yet; define it with "
                    "`web set %s /path/to/browser`\n",
                    prog, o.browser, o.browser);
        free(value);
    }
    if (o.locked_flag && webdoc_verbose())
        fprintf(stderr, "%s: created in the locked state\n", prog);
    return 0;
}

static int cmd_edit(int argc, char **argv)
{
    options o;
    webdoc_edit_params p;
    char path[WEBDOC_MAX_PATH];
    int npos;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    npos = argc - optind;
    if (npos < 1 || npos > 2) {
        fprintf(stderr, "%s: usage: web edit <name> [url] [options]\n", prog);
        return EXIT_USAGE;
    }

    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);

    memset(&p, 0, sizeof(p));
    p.locked = WEBDOC_EDIT_KEEP;
    if (npos == 2)
        p.url = argv[optind + 1];
    if (o.browser) {
        p.set_browser = 1;
        p.browser_kind = WEBDOC_BROWSER_VARIABLE;
        p.browser_ref = o.browser;
    } else if (o.browser_path) {
        p.set_browser = 1;
        p.browser_kind = WEBDOC_BROWSER_PATH;
        p.browser_ref = o.browser_path;
    } else if (o.default_browser) {
        p.set_browser = 1;
        p.browser_kind = WEBDOC_BROWSER_DEFAULT;
    }
    if (o.lock_flag)
        p.locked = 1;
    else if (o.unlock_flag)
        p.locked = 0;

    if (!p.url && !p.set_browser && p.locked == WEBDOC_EDIT_KEEP) {
        fprintf(stderr, "%s: nothing to change\n", prog);
        return EXIT_USAGE;
    }

    rc = webdoc_edit(path, &p);
    if (rc != WEBDOC_OK)
        return fail(rc, path);
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    options o;
    char path[WEBDOC_MAX_PATH];
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 1) {
        fprintf(stderr, "%s: usage: web delete <name>\n", prog);
        return EXIT_USAGE;
    }
    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);
    rc = webdoc_delete(path);
    if (rc != WEBDOC_OK)
        return fail(rc, path);
    return 0;
}

static int cmd_open(int argc, char **argv)
{
    options o;
    char path[WEBDOC_MAX_PATH];
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 1) {
        fprintf(stderr, "%s: usage: web open <name>\n", prog);
        return EXIT_USAGE;
    }
    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);

    rc = webdoc_open(path);
    if (rc == WEBDOC_ERR_LOCKED) {
        int global_locked = 0, doc_locked = 0;

        (void)webdoc_lock_state(path, &global_locked, &doc_locked);
        fprintf(stderr, "%s: %s is locked (%s%s%s); unlock it with `web unlock%s`\n",
                prog, path,
                global_locked ? "global lock" : "",
                (global_locked && doc_locked) ? " and " : "",
                doc_locked ? "this document" : "",
                doc_locked ? " <name>" : "");
        return EXIT_LOCKED;
    }
    if (rc != WEBDOC_OK)
        return fail(rc, path);
    if (o.verbose || global_verbose)
        fprintf(stderr, "%s: opened %s\n", prog, path);
    return 0;
}

/*
 * `show` prints one document and nothing else.  A directory is refused: the
 * two questions "what is in this document" and "what documents are here"
 * have different answers and different output shapes, so they are different
 * commands.
 */
static int cmd_show(int argc, char **argv)
{
    options o;
    char raw[WEBDOC_MAX_PATH];
    char path[WEBDOC_MAX_PATH];
    struct stat st;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 1) {
        fprintf(stderr, "%s: usage: web show <name>\n", prog);
        return EXIT_USAGE;
    }

    if (o.explicit_path)
        snprintf(raw, sizeof(raw), "%s", argv[optind]);
    else
        expand_tilde(argv[optind], raw, sizeof(raw));

    if (stat(raw, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "%s: %s is a directory; use `web list %s` to list its documents\n",
                prog, argv[optind], argv[optind]);
        return EXIT_USAGE;
    }

    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);

    rc = webdoc_show(path, stdout);
    if (rc == WEBDOC_ERR_IS_DIRECTORY) {
        fprintf(stderr,
                "%s: %s is a directory; use `web list` to list its documents\n",
                prog, path);
        return EXIT_USAGE;
    }
    if (rc != WEBDOC_OK)
        return fail(rc, path);
    return 0;
}

/* `list` is the directory counterpart of `show`. */
static int cmd_list(int argc, char **argv)
{
    options o;
    char dir[WEBDOC_MAX_PATH];
    struct stat st;
    webdoc_list list;
    int global_locked = 0;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind > 1) {
        fprintf(stderr, "%s: usage: web list [directory]\n", prog);
        return EXIT_USAGE;
    }

    if (argc - optind == 0)
        snprintf(dir, sizeof(dir), ".");
    else if (o.explicit_path)
        snprintf(dir, sizeof(dir), "%s", argv[optind]);
    else
        expand_tilde(argv[optind], dir, sizeof(dir));

    if (stat(dir, &st) != 0)
        return fail(WEBDOC_ERR_NOT_FOUND, dir);
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: %s is not a directory; use `web show` for a "
                "single document\n", prog, dir);
        return EXIT_USAGE;
    }

    rc = webdoc_list_dir(dir, &list);
    if (rc != WEBDOC_OK)
        return fail(rc, dir);
    (void)webdoc_lock_state(NULL, &global_locked, NULL);
    print_list(&list, global_locked);
    webdoc_list_free(&list);
    return 0;
}

static int cmd_lock(int argc, char **argv, int lock)
{
    options o;
    char path[WEBDOC_MAX_PATH];
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;

    if (argc - optind == 0) {
        rc = lock ? webdoc_lock(NULL) : webdoc_unlock(NULL);
        if (rc != WEBDOC_OK)
            return fail(rc, "global lock");
        if (o.verbose || global_verbose)
            fprintf(stderr, "%s: all documents %s\n", prog,
                    lock ? "locked" : "unlocked");
        return 0;
    }
    if (argc - optind != 1) {
        fprintf(stderr, "%s: usage: web %s [name]\n", prog,
                lock ? "lock" : "unlock");
        return EXIT_USAGE;
    }
    rc = resolve(argv[optind], o.explicit_path, path, sizeof(path));
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);
    rc = lock ? webdoc_lock(path) : webdoc_unlock(path);
    if (rc != WEBDOC_OK)
        return fail(rc, path);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    options o;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 2) {
        fprintf(stderr, "%s: usage: web set <variable> <value>\n", prog);
        return EXIT_USAGE;
    }
    rc = webdoc_set_variable(argv[optind], argv[optind + 1]);
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);

    /* Variables are general name/value pairs.  Only mention executability
       when the value looks like a path, since that is the one use libwebdoc
       has for a variable today. */
    if (argv[optind + 1][0] == '/' && access(argv[optind + 1], X_OK) != 0)
        fprintf(stderr, "%s: note: %s is not currently executable\n",
                prog, argv[optind + 1]);
    return 0;
}

static int cmd_unset(int argc, char **argv)
{
    options o;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 1) {
        fprintf(stderr, "%s: usage: web unset <variable>\n", prog);
        return EXIT_USAGE;
    }
    rc = webdoc_unset_variable(argv[optind]);
    if (rc != WEBDOC_OK)
        return fail(rc, argv[optind]);
    return 0;
}

/* Variables as aligned name/value pairs, one per line. */
static int cmd_variables(int argc, char **argv)
{
    options o;
    webdoc_variable_list list;
    size_t i, width = 0;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 0) {
        fprintf(stderr, "%s: usage: web variables\n", prog);
        return EXIT_USAGE;
    }

    rc = webdoc_list_variables(&list);
    if (rc != WEBDOC_OK)
        return fail(rc, "variables");

    if (list.count == 0) {
        printf("no variables defined\n");
        webdoc_variable_list_free(&list);
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        size_t len = strlen(list.names[i]);

        if (len > width)
            width = len;
    }
    for (i = 0; i < list.count; i++)
        printf("%-*s  %s\n", (int)width, list.names[i], list.values[i]);

    if (o.verbose || global_verbose)
        fprintf(stderr, "%s: %zu variable%s defined\n", prog, list.count,
                list.count == 1 ? "" : "s");
    webdoc_variable_list_free(&list);
    return 0;
}

/*
 * The global configuration state: where it is stored, whether the global
 * lock is on, and how many variables are defined.  Nothing about individual
 * documents appears here, because nothing about them is stored here.
 */
static int cmd_config(int argc, char **argv)
{
    options o;
    char config_file[WEBDOC_MAX_PATH];
    char state_file[WEBDOC_MAX_PATH];
    webdoc_variable_list list;
    int global_locked = 0;
    size_t count = 0;
    webdoc_err rc;

    if (parse_opts(argc, argv, &o) != 0)
        return EXIT_USAGE;
    if (argc - optind != 0) {
        fprintf(stderr, "%s: usage: web config\n", prog);
        return EXIT_USAGE;
    }

    rc = webdoc_config_file(config_file, sizeof(config_file));
    if (rc != WEBDOC_OK)
        return fail(rc, "configuration");
    rc = webdoc_state_file(state_file, sizeof(state_file));
    if (rc != WEBDOC_OK)
        return fail(rc, "state");

    (void)webdoc_lock_state(NULL, &global_locked, NULL);
    if (webdoc_list_variables(&list) == WEBDOC_OK) {
        count = list.count;
        webdoc_variable_list_free(&list);
    }

    printf("version:        %s\n", WEBDOC_VERSION);
    printf("format:         %d\n", WEBDOC_FILE_VERSION);
    printf("extension:      %s\n", WEBDOC_EXT);
    printf("config file:    %s\n", config_file);
    printf("state file:     %s\n", state_file);
    printf("global lock:    %s\n", global_locked ? "active" : "inactive");
    printf("variables:      %zu\n", count);
    printf("default browser: system (via xdg-open)\n");
    return 0;
}

/* ----------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    const char *cmd;

    /* Options accepted before the subcommand, so `web -v open docs` and
       `web open docs -v` behave identically. */
    while (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0) {
            global_verbose = 1;
            webdoc_set_report_stream(stderr);
            webdoc_set_verbose(1);
        } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
            printf("web (webdoc) %s\n", WEBDOC_VERSION);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, argv[1]);
            usage(stderr);
            return EXIT_USAGE;
        }
        argv[1] = argv[0];
        argc--;
        argv++;
    }

    if (argc < 2) {
        usage(stderr);
        return EXIT_USAGE;
    }
    cmd = argv[1];

    if (strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0) {
        printf("web (webdoc) %s\n", WEBDOC_VERSION);
        return 0;
    }

    /* Hand the subcommand its own argv so getopt_long indices stay simple. */
    argv[1] = argv[0];
    argc--;
    argv++;

    if (strcmp(cmd, "create") == 0)     return cmd_create(argc, argv);
    if (strcmp(cmd, "edit") == 0)       return cmd_edit(argc, argv);
    if (strcmp(cmd, "delete") == 0)     return cmd_delete(argc, argv);
    if (strcmp(cmd, "open") == 0)       return cmd_open(argc, argv);
    if (strcmp(cmd, "show") == 0)       return cmd_show(argc, argv);
    if (strcmp(cmd, "list") == 0)       return cmd_list(argc, argv);
    if (strcmp(cmd, "lock") == 0)       return cmd_lock(argc, argv, 1);
    if (strcmp(cmd, "unlock") == 0)     return cmd_lock(argc, argv, 0);
    if (strcmp(cmd, "set") == 0)        return cmd_set(argc, argv);
    if (strcmp(cmd, "unset") == 0)      return cmd_unset(argc, argv);
    if (strcmp(cmd, "variables") == 0)  return cmd_variables(argc, argv);
    if (strcmp(cmd, "config") == 0)     return cmd_config(argc, argv);

    fprintf(stderr, "%s: unknown command '%s'\n", prog, cmd);
    usage(stderr);
    return EXIT_USAGE;
}
