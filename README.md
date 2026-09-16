# webdoc

`webdoc` makes a web address into an ordinary file.

A web document is a small file named `something.web`. It holds a URL, an
optional choice of browser, and an optional lock. Because it is a plain file,
it can be placed in any directory, renamed, copied, moved, archived, mailed,
committed to version control, and removed with the tools already on the
system. Opening it, either by running `web open` or by double-clicking it in a
file manager, launches it in a browser.

The practical difference from a browser bookmark is ownership. A bookmark
lives inside one browser profile and is reachable only through that browser's
interface. A web document lives in the filesystem, so it can be organised
alongside the project it belongs to and handled by every tool that understands
files.

The package provides two things:

* `web`, the command line interface, together with a manual page and desktop
  integration that registers `.web` with the system so file managers open it
  correctly.
* `libwebdoc`, the C library that implements the format and every operation on
  it. The command line interface is a thin front end over this library and
  contains no logic of its own.

Requirements: a C11 compiler and a C library. There are no other dependencies,
no network access, no background service, and no database.

---

## Table of contents

* [Installation](#installation)
* [Getting started](#getting-started)
* [Concepts](#concepts)
* [Command reference](#command-reference)
* [Global options](#global-options)
* [Exit status](#exit-status)
* [Verbose output](#verbose-output)
* [Choosing a browser](#choosing-a-browser)
* [Variables](#variables)
* [Locking](#locking)
* [Files and configuration](#files-and-configuration)
* [The file format](#the-file-format)
* [Library reference](#library-reference)
* [Building from source](#building-from-source)
* [Packaging](#packaging)
* [Troubleshooting](#troubleshooting)
* [Security](#security)
* [Licence](#licence)

---

## Installation

### From an apt repository

```console
$ sudo apt update
$ sudo apt install webdoc
```

### From a downloaded package

```console
$ sudo apt install ./webdoc_1.0.0-1_amd64.deb
```

### From source

```console
$ make
$ sudo make install
```

`make install` also registers `.web` as a file type with the desktop, so file
managers display web documents with their own icon and open them with `web`
rather than with a text editor. If the file manager was already running, log
out and back in so that it rereads the MIME database.

Confirm the installation:

```console
$ web --version
web (webdoc) 1.0.0
```

The manual page is installed as `man web`.

---

## Getting started

Create a document:

```console
$ web create postgres https://www.postgresql.org/
```

This writes `postgres.web` into the current directory. Nothing is printed. As
with most Unix tools, `web` is silent on success; pass `-v` when a description
of each step is wanted.

Open it:

```console
$ web open postgres
```

The system's default browser opens the page. Double-clicking `postgres.web` in
a file manager does exactly the same thing, through the same code path.

Inspect it:

```console
$ web show postgres
path:    postgres.web
version: 1
url:     https://www.postgresql.org/
browser: system default
locked:  no
```

Because it is a file, ordinary file handling applies:

```console
$ mkdir ~/Bookmarks
$ mv postgres.web ~/Bookmarks/
$ cp ~/Bookmarks/postgres.web ~/Desktop/
$ web list ~/Bookmarks
```

---

## Concepts

**Document.** One `.web` file. It holds everything that persistently belongs
to it: the URL, the browser selection, and its own lock state. Nothing about
an individual document is recorded anywhere else, so copying a document copies
all of it and deleting one leaves nothing behind to clean up.

**Name.** A document is referred to by its filename without the `.web` suffix.
`web open docs` and `web open docs.web` refer to the same file, and a suffix
is never doubled into `docs.web.web`. Relative and absolute paths work as
well, so `web create ~/Bookmarks/docs <url>` is valid. Passing `--path`
disables this handling and uses the argument exactly as written.

**Variable.** A name and value pair stored in the global configuration.
Variables are a general facility; selecting a browser is the first use of
them, not the only possible one. A document that names a variable stores the
name, not the value, so redefining the variable changes every document that
refers to it.

**Lock.** A stored instruction not to open something at the moment. There are
two independent locks: one belonging to each document and stored inside it,
and one global lock stored in the state file. Either one refuses an open, and
neither clears the other.

---

## Command reference

General form:

```
web [global options] <command> [arguments] [options]
```

### `web create <name> <url> [options]`

Creates `<name>.web`.

The URL must use the `http://` or `https://` scheme. Creation never overwrites
anything: if the target already exists the command fails and suggests
`web edit` instead. Accepted options are `--browser`, `--browser-path`,
`--locked` and `--path`.

```console
$ web create docs https://docs.example.com/
$ web create work https://mail.example.com/ --browser chrome
$ web create vault https://vault.example.com/ --locked
```

### `web edit <name> [url] [options]`

Modifies an existing document. Any combination of the URL, the browser and the
lock state may be changed in one invocation. Fields that are not mentioned are
left untouched, including any fields written by a future version of the tool.
At least one change must be requested, otherwise the command reports that
there is nothing to do.

```console
$ web edit docs https://docs.example.com/v2/
$ web edit docs --browser firefox
$ web edit docs --default-browser
$ web edit docs --lock
```

### `web delete <name>`

Removes the document. Directories are refused. When the name given is a
symbolic link, the link itself is removed rather than its target, which is
what naming a link ordinarily means.

### `web open <name>`

Opens the document in a browser.

The sequence is: read the file, validate the URL, check the global lock, check
the document's own lock, resolve the browser, validate the browser
executable, then launch it. The command is refused with exit status 3 if
either lock is in effect.

The browser is launched fully detached. It runs in a new session with all
three of its standard streams redirected to `/dev/null`, and it is reparented
to `init`, so `web open` returns immediately instead of waiting for the
browser to exit. This behaviour is unconditional and is not changed by
`--verbose`. See [Verbose output](#verbose-output).

### `web show <name>`

Prints one document: its path, format version, URL, browser and lock state.

A directory is refused, with a message pointing at `web list`. The two
questions "what is inside this document" and "which documents are here" have
different answers and different output shapes, so they are separate commands.

```console
$ web show work
path:    work.web
version: 1
url:     https://mail.example.com/
browser: chrome (/usr/bin/google-chrome)
locked:  no
```

The `browser` line shows the variable name followed by its current value in
parentheses. When a document pins an executable directly, only the path is
shown. When no browser is set, the line reads `system default`.

### `web list [directory]`

Lists the `.web` files in a directory, defaulting to the current one, in the
manner of `ls -l`. Columns are permissions, owner, group, size, modification
time, name, and then the URL, followed by `[locked]` and the browser where
those apply. When the global lock is active, a line saying so precedes the
listing.

```console
$ web list ~/Bookmarks
-rw-r--r-- alice    alice       102 2026-09-14 11:20 docs         https://docs.example.com/  chrome
-rw-r--r-- alice    alice        84 2026-09-14 11:21 postgres     https://www.postgresql.org/
```

A document that cannot be parsed is listed with the reason in angle brackets
rather than omitted, so a damaged file is visible instead of silently missing.
Hidden files are skipped, as with `ls`. The listing is produced with
directory and file system calls, never by invoking `ls` and never by expanding
a glob, so a file with an awkward name is only ever a filename.

### `web lock [name]` and `web unlock [name]`

With a name, sets or clears that document's own lock, which is stored inside
the file and therefore travels with copies of it.

With no name, sets or clears the global lock, which applies to every document.

The two scopes are independent. A global unlock does not unlock a document
that was locked individually, and unlocking one document does not disable the
global lock.

### `web set <variable> <value>` and `web unset <variable>`

Defines or removes a variable. Names may contain letters, digits, underscores
and hyphens, up to 64 characters. Values are arbitrary text without control
characters.

```console
$ web set chrome /usr/bin/google-chrome
$ web unset chrome
```

When the value looks like an absolute path and is not currently executable, a
note is printed. This is advisory only; the value is still stored, because a
variable is not required to be a browser.

### `web variables`

Prints the defined variables as aligned name and value pairs, one per line,
with no quoting or formatting from the underlying storage. When no variables
are defined, a single line says so.

```console
$ web variables
chrome   /usr/bin/google-chrome
firefox  /usr/bin/firefox
```

### `web config`

Prints the global configuration state.

```console
$ web config
version:        1.0.0
format:         1
extension:      .web
config file:    /home/alice/.config/webdoc/config.json
state file:     /home/alice/.local/state/webdoc/state.json
global lock:    inactive
variables:      2
default browser: system (via xdg-open)
```

Nothing about individual documents appears here, because nothing about them is
stored globally. The filesystem is the only record of which documents exist.

### `web help` and `web version`

Equivalent to `--help` and `--version`.

---

## Global options

| Option | Applies to | Meaning |
|---|---|---|
| `--browser <variable>` | `create`, `edit` | Open with the browser named by this variable. |
| `--browser-path <path>` | `create`, `edit` | Open with this exact executable, stored literally. |
| `--default-browser` | `edit` | Clear the browser field and use the desktop default. |
| `--locked` | `create` | Create the document already locked. |
| `--lock`, `--unlock` | `edit` | Set or clear the document's own lock. |
| `--path` | most | Use the argument as a literal path: no `.web` suffix is added and no `~` expansion is done. |
| `-v`, `--verbose` | all | Report each step on standard error. |
| `-h`, `--help` | all | Print usage. |
| `-V`, `--version` | all | Print the version. |

`--browser` and `--browser-path` are mutually exclusive, as are `--lock` and
`--unlock`, and `--default-browser` conflicts with both browser options.

`-v`, `-h` and `-V` may be given before the command or after it, so
`web -v open docs` and `web open docs -v` behave identically.

---

## Exit status

| Status | Meaning |
|---|---|
| `0` | Success. |
| `1` | The operation failed, for example the file was missing, malformed, or unreadable. |
| `2` | Usage error, for example an unknown command, a wrong argument count, or a directory passed to `show`. |
| `3` | Refused because a lock is in effect. |

Status 3 is distinct so that a script can tell "this is locked, which is a
decision someone made" apart from "this went wrong".

---

## Verbose output

`-v` makes `web` describe each step it performs.

```console
$ web -v open docs
web: 'docs' refers to docs.web
web: read docs.web
web: address https://docs.example.com/
web: lock check passed
web: browser: chrome (/usr/bin/google-chrome)
web: browser checks passed
web: browser started and detached from this terminal
web: opened docs.web
```

Three properties of this output are worth knowing.

**It goes to standard error.** Results go to standard output and diagnostics
go to standard error, so `web show x | ...`, `web list | ...` and
`web variables | ...` produce the same clean stream whether or not `-v` is in
effect.

**It describes the operation, not the implementation.** Verbose mode reports
names, addresses, browsers and locks. It is a record of what the tool did, not
a debugging trace, so temporary filenames, file descriptors, internal field
encodings and function names do not appear.

**It never includes the browser's own output.** The browser is always launched
detached, in every mode. Its messages cannot reach the terminal, and it cannot
read from the terminal either. This is deliberate: a browser that kept the
caller's terminal would print its startup messages into the next shell prompt
and could consume keystrokes intended for the shell, leaving the session
looking as though it had hung. To see a browser's own diagnostics, run that
browser directly.

---

## Choosing a browser

By default a document opens in the system's default browser through
`xdg-open`. `web` never changes that system setting.

To make a document always open somewhere specific, first give the browser a
name:

```console
$ web set chrome /usr/bin/google-chrome
```

Then refer to it by that name:

```console
$ web create work https://mail.example.com/ --browser chrome
```

What is stored in the document is the name, not the path. The path is resolved
every time the document is opened, so switching browsers later updates every
document that refers to the name:

```console
$ web set chrome /usr/bin/chromium
```

To pin one exact program instead, so that it cannot move, store the path
directly:

```console
$ web edit work --browser-path /opt/google/chrome/chrome
```

To return to the system default:

```console
$ web edit work --default-browser
```

Before launching anything, the executable is checked. It must be an absolute
path with no `..` components, a regular file, executable by the current user,
not world-writable, and not sitting in a world-writable directory that lacks
the sticky bit. Anything else is refused as an invalid browser configuration.

---

## Variables

`web set` and `web unset` manage plain name and value pairs in the
configuration file. Selecting a browser is simply the first thing that uses
them, so the same mechanism is available for whatever later versions need.

```console
$ web set chrome /usr/bin/google-chrome
$ web set firefox /usr/bin/firefox
$ web variables
chrome   /usr/bin/google-chrome
firefox  /usr/bin/firefox
```

A variable used as a browser must resolve to an absolute path to an
executable. That requirement is enforced at the point of use, when a document
is opened, not when the variable is defined.

---

## Locking

Locking records the instruction "do not open this at the moment". There are
two switches and they operate independently.

Lock one document:

```console
$ web lock work
$ web open work
web: work.web is locked (this document); unlock it with `web unlock <name>`
```

Lock everything at once:

```console
$ web lock        # no name: applies to every document
$ web unlock      # back to normal
```

Two points follow from where each lock is stored.

* A document's own lock lives inside the file, so a locked document that is
  copied or sent arrives locked.
* `web unlock` with no name lifts the global lock only. A document locked
  individually stays locked until it is unlocked by name. This is deliberate:
  a blanket unlock should not quietly undo a decision made about one specific
  document.

Locking is a convenience, not a security boundary. Anyone who can read the
file can read the URL and open it by other means. The lock is checked in one
place, the single open pipeline shared by the command line interface and the
desktop handler, so there is no second route that skips the check.

---

## Files and configuration

```text
./anything.web                        documents, in any directory
~/.config/webdoc/config.json          variables
~/.local/state/webdoc/state.json      the global lock
```

Both locations honour `XDG_CONFIG_HOME` and `XDG_STATE_HOME` and are reported
by `web config`. Both files are created with mode 0600 inside directories
created with mode 0700.

The split reflects what the data is. Variables are configuration that a user
may reasonably edit by hand or keep in a dotfiles repository. The global lock
is runtime state that is not hand-authored.

Neither file lists individual documents. There is no database, no index and no
background process. Deleting a `.web` file is complete in itself, and one
restored from a backup works immediately.

---

## The file format

A `.web` file is a single UTF-8 JSON object with a trailing newline. Readers
reject files larger than 64 KiB.

```json
{
  "version": 1,
  "url": "https://docs.example.com/",
  "browser": "variable:chrome",
  "locked": true
}
```

| Member | Type | Required | Meaning |
|---|---|---|---|
| `version` | integer | yes | Format version. Currently `1`. |
| `url` | string | yes | The address. `http://` or `https://` only. |
| `browser` | string | no | Browser selection, tagged `variable:NAME` or `path:/absolute/path`. |
| `locked` | boolean | no | The document's own lock. Absent means unlocked. |

Optional members with no value are omitted rather than written as `null`, `""`
or `false`, so the smallest valid document contains only `version` and `url`.

The tags on the `browser` value are a storage detail. The command line
interface neither asks for them nor prints them: `--browser chrome` is what is
typed and `browser: chrome` is what is reported.

Writes are atomic. Every update is written to a temporary file in the same
directory, flushed, then renamed over the target, followed by a flush of the
directory. A reader sees either the old file or the new one, never a partial
one, even across a crash. Creation uses an exclusive, symlink-refusing open,
so `web create` cannot overwrite an existing file and cannot be redirected
through a symbolic link planted at the target.

Unknown members are ignored on read and preserved on write, so a document
edited by an older version of the tool does not silently lose data added by a
newer one. A reader that understands version 1 refuses a file declaring a
higher version rather than guessing.

[`docs/format.md`](docs/format.md) is the complete specification.

---

## Library reference

`libwebdoc` implements the format and every operation on it. The command line
interface parses arguments, calls the library, and turns results into messages
and exit codes; it holds no logic of its own. Any other front end therefore
gets identical behaviour, including both lock checks.

The Debian package links the library statically into the `web` binary and does
not ship a shared library or a development package. To build against
`libwebdoc`, build from source and run `sudo make install`, which installs
`webdoc.h`, `libwebdoc.a` and `libwebdoc.so`.

```console
$ cc example.c -lwebdoc -o example
```

### Conventions

Every function returns `webdoc_err`, where `WEBDOC_OK` is zero and any other
value is a failure. Pass a code to `webdoc_strerror()` for a short
non-localised description. Malformed input always produces a specific error
code, never a crash.

Structures are owned by the caller. `webdoc_t` is zero-initialised with
`webdoc_init()` and released with `webdoc_reset()`; a value returned through a
`char **` argument is released with `free()`; a list is released with its own
`_free` function. Every `_free` and `_reset` function accepts an already
cleared structure, so cleanup paths do not need to track how far they got.

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `WEBDOC_FILE_VERSION` | `1` | Format version written by this release. |
| `WEBDOC_EXT` | `".web"` | Filename suffix. |
| `WEBDOC_MAX_FILE_SIZE` | 64 KiB | Largest accepted document or configuration file. |
| `WEBDOC_MAX_URL_LEN` | `2048` | Longest accepted URL. |
| `WEBDOC_MAX_VAR_NAME_LEN` | `64` | Longest accepted variable name. |
| `WEBDOC_MAX_PATH` | `4096` | Path buffer size used throughout the API. |

### Error codes

`WEBDOC_OK`, `WEBDOC_ERR_INVALID_ARG`, `WEBDOC_ERR_INVALID_URL`,
`WEBDOC_ERR_UNSUPPORTED_SCHEME`, `WEBDOC_ERR_INVALID_FILE`,
`WEBDOC_ERR_UNSUPPORTED_VERSION`, `WEBDOC_ERR_NOT_FOUND`,
`WEBDOC_ERR_EXISTS`, `WEBDOC_ERR_NOT_A_DOCUMENT`, `WEBDOC_ERR_IS_DIRECTORY`,
`WEBDOC_ERR_LOCKED`, `WEBDOC_ERR_INVALID_BROWSER`,
`WEBDOC_ERR_VAR_NOT_FOUND`, `WEBDOC_ERR_PERMISSION`, `WEBDOC_ERR_TOO_LARGE`,
`WEBDOC_ERR_IO`, `WEBDOC_ERR_NOMEM`, `WEBDOC_ERR_SPAWN`,
`WEBDOC_ERR_INTERNAL`.

```c
const char *webdoc_strerror(webdoc_err err);
```

### The document structure

```c
typedef enum {
    WEBDOC_BROWSER_DEFAULT = 0,  /* no browser field: desktop default */
    WEBDOC_BROWSER_VARIABLE,     /* "variable:NAME"                   */
    WEBDOC_BROWSER_PATH          /* "path:/absolute/executable"       */
} webdoc_browser_kind;

typedef struct {
    int                  version;
    char                *url;
    webdoc_browser_kind  browser_kind;
    char                *browser_ref;   /* variable name or literal path */
    int                  locked;
    char                *extra_json;    /* preserved unknown members     */
} webdoc_t;

void webdoc_init(webdoc_t *doc);
void webdoc_reset(webdoc_t *doc);
const char *webdoc_browser_string(const webdoc_t *doc, char *buf, size_t buflen);
```

`extra_json` holds members the current version does not understand. It is
carried through an edit unchanged and should be treated as opaque.

`webdoc_browser_string()` renders the tagged form, such as `variable:chrome`,
into a caller-supplied buffer. It returns `NULL` for
`WEBDOC_BROWSER_DEFAULT`. Because the tag is a storage detail, a user-facing
front end should strip it before display.

### Reading and writing

```c
webdoc_err webdoc_load(const char *path, webdoc_t *out);
webdoc_err webdoc_save(const char *path, const webdoc_t *doc);
```

`webdoc_load()` validates as it reads: the file must be a regular file, parse
as a single JSON object, declare a supported version, and carry a URL that
passes the URL policy. It fails with `WEBDOC_ERR_NOT_A_DOCUMENT` for anything
that is not a regular file, which is what refuses a FIFO before it can block
the process.

`webdoc_save()` replaces the file atomically and preserves its existing
permissions. If the path is a symbolic link, the target is replaced rather
than the link.

### Creating, editing and deleting

```c
typedef struct {
    const char          *url;          /* required                     */
    webdoc_browser_kind  browser_kind;
    const char          *browser_ref;
    int                  locked;
} webdoc_create_params;

webdoc_err webdoc_create(const char *path, const webdoc_create_params *params);

#define WEBDOC_EDIT_KEEP (-1)

typedef struct {
    const char          *url;          /* NULL: keep                   */
    int                  set_browser;  /* 0: keep, 1: apply kind/ref   */
    webdoc_browser_kind  browser_kind;
    const char          *browser_ref;
    int                  locked;       /* WEBDOC_EDIT_KEEP, 0 or 1     */
} webdoc_edit_params;

webdoc_err webdoc_edit(const char *path, const webdoc_edit_params *params);
webdoc_err webdoc_delete(const char *path);
```

Zero the parameter structure before filling it in, and set
`locked = WEBDOC_EDIT_KEEP` for an edit that should not touch the lock.
`webdoc_create()` returns `WEBDOC_ERR_EXISTS` rather than overwriting.

### Name resolution

```c
webdoc_err webdoc_resolve_name(const char *reference, int explicit_path,
                               char *out, size_t outlen);
```

With `explicit_path` zero, this applies the suffix and `~` handling described
under [Concepts](#concepts), producing a path ending in exactly one `.web`.
With `explicit_path` non-zero the reference is copied verbatim. This is the
one place that policy lives, so every front end resolves names identically.

### Opening

```c
webdoc_err webdoc_open(const char *path);
```

Runs the whole pipeline: load, validate, check both locks, resolve the
browser, validate the executable, launch it detached. Returns
`WEBDOC_ERR_LOCKED` if either lock is in effect and `WEBDOC_ERR_SPAWN` if the
browser could not be started. It returns as soon as the browser has been
launched and never waits for it to exit.

### Locking

```c
webdoc_err webdoc_lock(const char *path);    /* NULL: the global lock */
webdoc_err webdoc_unlock(const char *path);
webdoc_err webdoc_lock_state(const char *path, int *global_locked,
                             int *doc_locked);
```

Pass `NULL` as the path to operate on the global lock. Either output pointer
of `webdoc_lock_state()` may be `NULL` when that part of the answer is not
wanted; passing a `NULL` path reports the global state only.

### Variables

```c
webdoc_err webdoc_set_variable(const char *name, const char *value);
webdoc_err webdoc_get_variable(const char *name, char **value_out);
webdoc_err webdoc_unset_variable(const char *name);

typedef struct {
    char **names;
    char **values;
    size_t count;
} webdoc_variable_list;

webdoc_err webdoc_list_variables(webdoc_variable_list *out);
void       webdoc_variable_list_free(webdoc_variable_list *list);
```

`webdoc_get_variable()` returns a string the caller must `free()`, and
`WEBDOC_ERR_VAR_NOT_FOUND` when the name is not defined. The list arrays are
parallel: `names[i]` corresponds to `values[i]`.

### Showing and listing

```c
webdoc_err webdoc_show(const char *path, FILE *out);

typedef struct {
    char       *name;     /* filename without the .web suffix           */
    char       *path;
    char       *url;      /* NULL when the entry could not be parsed    */
    char       *browser;  /* tagged form, or NULL                       */
    int         locked;
    mode_t      mode;
    off_t       size;
    time_t      mtime;
    uid_t       uid;
    gid_t       gid;
    webdoc_err  status;   /* WEBDOC_OK, or why this entry failed        */
} webdoc_entry;

typedef struct {
    webdoc_entry *items;
    size_t        count;
} webdoc_list;

webdoc_err webdoc_list_dir(const char *dir, webdoc_list *out);
void       webdoc_list_free(webdoc_list *list);
```

`webdoc_show()` refuses a directory with `WEBDOC_ERR_IS_DIRECTORY`.

`webdoc_list_dir()` reports a failed entry through that entry's `status` field
rather than failing the whole call, so one damaged file does not hide the rest
of a directory. Entries are sorted by name.

### Configuration paths

```c
webdoc_err webdoc_config_file(char *out, size_t outlen);
webdoc_err webdoc_state_file(char *out, size_t outlen);
```

Report the absolute paths of the configuration and state files, honouring the
relevant XDG environment variables.

### Validation

```c
webdoc_err webdoc_url_validate(const char *url);
webdoc_err webdoc_variable_name_validate(const char *name);
webdoc_err webdoc_variable_value_validate(const char *value);
```

These are exposed so that a front end can check input before acting on it and
report the same errors the library would. They are applied internally
regardless.

### Verbose reporting

```c
void webdoc_set_verbose(int enabled);
int  webdoc_verbose(void);
void webdoc_set_report_stream(FILE *stream);
```

Reporting is disabled by default. When enabled, the library writes one short
line per step to the report stream, which defaults to `stderr`. These settings
are process-wide.

### A complete example

```c
#include <webdoc.h>
#include <stdio.h>

int main(void)
{
    webdoc_create_params params = {0};
    webdoc_t doc;
    char path[WEBDOC_MAX_PATH];
    webdoc_err rc;

    rc = webdoc_resolve_name("docs", 0, path, sizeof(path));
    if (rc != WEBDOC_OK) {
        fprintf(stderr, "resolve: %s\n", webdoc_strerror(rc));
        return 1;
    }

    params.url = "https://docs.example.com/";
    params.browser_kind = WEBDOC_BROWSER_VARIABLE;
    params.browser_ref = "chrome";

    rc = webdoc_create(path, &params);
    if (rc != WEBDOC_OK && rc != WEBDOC_ERR_EXISTS) {
        fprintf(stderr, "create: %s\n", webdoc_strerror(rc));
        return 1;
    }

    rc = webdoc_load(path, &doc);
    if (rc != WEBDOC_OK) {
        fprintf(stderr, "load: %s\n", webdoc_strerror(rc));
        return 1;
    }
    printf("%s -> %s\n", path, doc.url);
    webdoc_reset(&doc);

    rc = webdoc_open(path);
    if (rc != WEBDOC_OK) {
        fprintf(stderr, "open: %s\n", webdoc_strerror(rc));
        return 1;
    }
    return 0;
}
```

---

## Building from source

```console
$ make          # libwebdoc.a, libwebdoc.so and web
$ make check    # unit tests, end-to-end tests, and validation of the
                # shipped MIME and desktop files
$ sudo make install
```

Useful targets and variables:

| Target | Effect |
|---|---|
| `all` | Static library, shared library and the `web` binary. |
| `check` | Runs every test. |
| `install-bin` | Installs the binary, manual page and desktop integration only. This is what the Debian package uses. |
| `install` | `install-bin` plus the header and both libraries. |
| `uninstall` | Removes everything `install` placed. |
| `clean` | Removes build products. |

`PREFIX` defaults to `/usr/local`; `DESTDIR` stages an installation without
touching the live system, and suppresses the MIME, desktop and icon cache
refresh, which is correct during package building.

Source layout:

```text
include/webdoc.h   public libwebdoc API
src/json.c         vendored strict JSON reader and writer
src/util.c         errno mapping, bounded reads, atomic writes, XDG paths
src/report.c       verbose progress reporting
src/validate.c     URL and variable policy, error strings
src/config.c       variables, global lock state, configuration paths
src/webfile.c      load, save, create, edit, delete, name resolution, locking
src/browser.c      browser resolution, executable validation, detached spawn
src/open.c         the single open pipeline
src/show.c         show and directory listing
src/web.c          command line front end, argument parsing and messages only
tests/             unit tests and end-to-end tests
data/              MIME package, desktop entry, icon
docs/              format specification, security model, manual page
debian/            Debian packaging
```

---

## Packaging

`dpkg-buildpackage -us -uc -b` produces a single `webdoc` binary package
containing the binary, the manual page, the MIME registration, the desktop
entry and the icon.

`libwebdoc` is linked statically into `web`, so the package ships no shared
library. That means no development package and no ABI to keep stable while the
format is at version 1. No maintainer scripts are required either: dpkg
triggers refresh the MIME, desktop and icon caches.

To serve the package from an apt repository:

```console
$ mkdir -p repo/pool
$ cp webdoc_1.0.0-1_amd64.deb repo/pool/
$ cd repo
$ dpkg-scanpackages --multiversion pool /dev/null > Packages
$ gzip -k Packages
$ apt-ftparchive release . > Release
$ gpg --clearsign -o InRelease Release
```

---

## Troubleshooting

**A `.web` file opens in a text editor.** The file type is not registered.
Install the package or run `sudo make install`, then log out and back in so
the file manager rereads the MIME database. To confirm the registration:

```console
$ gio info yourfile.web | grep content-type
  standard::content-type: application/x-webdoc
```

**`web open` reports an invalid browser configuration.** The executable failed
one of the checks described under [Choosing a browser](#choosing-a-browser).
The most common causes are a relative path, a path that no longer exists, and
a world-writable executable or directory. Check what the document resolves to
with `web show <name>`.

**`web open` exits with status 3.** A lock is in effect. The message says
whether it is the global lock, the document's own lock, or both. Use
`web unlock` or `web unlock <name>` accordingly.

**Nothing is printed on success.** That is the intended behaviour. Use `-v`.

**`web show` on a folder is an error.** Use `web list` for directories.

**A URL is rejected.** Only `http://` and `https://` are accepted, up to 2048
bytes. Spaces, control characters, raw non-ASCII bytes and embedded
credentials are refused. Percent-encode anything outside plain ASCII.

---

## Security

`web` is not installed setuid and grants no privilege the invoking user does
not already have. The threat considered is a hostile file or path causing a
user's own session to do something unintended.

In summary:

* No shell is used anywhere. The browser is launched with an explicit argument
  vector, so a URL cannot be word-split, globbed or read as a metacharacter.
* URLs are restricted to `http://` and `https://`, bounded in length, and free
  of control characters, whitespace, raw non-ASCII bytes and embedded
  credentials.
* A browser executable must be an absolute path to a regular, executable, not
  world-writable file in a directory that is not world-writable without the
  sticky bit.
* The browser is always launched in its own session with its standard streams
  detached, so it can neither write to the caller's terminal nor read from it.
* Writes are atomic; creation is exclusive and refuses symbolic links;
  non-regular files are rejected before they can block the process.
* Documents and configuration files are limited to 64 KiB, and the JSON parser
  bounds nesting depth, member counts and string lengths, and rejects
  duplicate keys and trailing content.

[`docs/security.md`](docs/security.md) gives the full model and its reasoning.
Please report security issues privately to the maintainer address in
[`debian/control`](debian/control) rather than in a public tracker.

---

## Further reading

* [`docs/format.md`](docs/format.md), the file format and its versioning rules
* [`docs/security.md`](docs/security.md), the threat model and mitigations
* [`docs/web.1`](docs/web.1), the manual page, also available as `man web`

---

## Licence

MIT. See [LICENSE](LICENSE).
