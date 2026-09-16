# webdoc

webdoc turns a web address into an ordinary file. You get a small file called
`something.web` that you can put in a folder, rename, copy, back up, email or
keep in git — and double-click, or run `web open`, to open it in a browser.

Think of it as a bookmark that lives in your filesystem instead of inside one
browser's profile.

---

## Install

### From a `.deb`

```console
$ sudo apt install ./webdoc_1.0.0-1_amd64.deb
```

### From source

```console
$ make
$ sudo make install
```

`make install` also registers `.web` as a file type with your desktop, so file
managers show web documents with their own icon and open them with `web`
rather than a text editor. Log out and back in if your file manager was
already running.

To check that it worked:

```console
$ web --version
web (webdoc) 1.0.0
```

---

## Your first document

```console
$ web create postgres https://www.postgresql.org/
```

That creates `postgres.web` in the current directory. Nothing is printed —
like most Unix tools, `web` stays quiet when it succeeds. Add `-v` if you want
it to say what it did.

Open it:

```console
$ web open postgres
```

Your default browser opens the page. You can also double-click `postgres.web`
in your file manager.

Look at it:

```console
$ web show postgres
path:    postgres.web
version: 1
url:     https://www.postgresql.org/
browser: system default
locked:  no
```

And because it's just a file, all of this works:

```console
$ mkdir ~/Bookmarks && mv postgres.web ~/Bookmarks/
$ cp ~/Bookmarks/postgres.web ~/Desktop/
$ web list ~/Bookmarks
```

---

## Everyday commands

| What you want | Command |
|---|---|
| Make a document | `web create <name> <url>` |
| Open it | `web open <name>` |
| See what's in it | `web show <name>` |
| List a folder | `web list [folder]` |
| Change the URL | `web edit <name> <new-url>` |
| Delete it | `web delete <name>` |
| Stop it opening | `web lock <name>` |
| Allow it again | `web unlock <name>` |
| See your variables | `web variables` |
| See your configuration | `web config` |

You can write the name with or without `.web` — `web open postgres` and
`web open postgres.web` are the same thing, and you never end up with
`postgres.web.web`. Paths work too: `web create ~/Bookmarks/docs <url>`.

`show` and `list` answer different questions. `show` prints one document;
`list` enumerates a directory. `web show` on a folder is an error, and tells
you to use `web list`.

---

## Choosing a browser

By default a document opens in your system's default browser, and `web` never
changes that setting.

If you want a particular document to always open somewhere specific, first
give that browser a name:

```console
$ web set chrome /usr/bin/google-chrome
```

Then use the name:

```console
$ web create work https://mail.example.com/ --browser chrome
$ web show work
browser: chrome (/usr/bin/google-chrome)
```

The point of naming it is that the name is what gets stored. Switch browsers
later and every document that says `chrome` follows along:

```console
$ web set chrome /usr/bin/chromium      # every "chrome" document now uses this
```

If you'd rather pin one exact program and never have it move, use a path
instead of a name:

```console
$ web edit work --browser-path /opt/google/chrome/chrome
```

And to go back to your system default:

```console
$ web edit work --default-browser
```

### Variables aren't only for browsers

`web set` and `web unset` manage plain name/value pairs in your
configuration. Browser selection is just the first thing that uses them, so
the same mechanism is there for whatever later versions need.

```console
$ web variables
chrome   /usr/bin/google-chrome
editor   /usr/bin/nano
```

---

## Locking

Locking means "don't open this right now". There are two switches, and they
work independently.

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

Two things worth knowing:

- A document's own lock is stored inside the file, so if you copy or send a
  locked document, it arrives locked.
- `web unlock` with no name lifts the global lock only. A document you locked
  individually stays locked until you unlock it by name. That's deliberate — a
  blanket unlock shouldn't quietly undo a decision you made about one specific
  document.

Locking is a convenience, not a security feature: anyone who can read the file
can see the URL.

---

## Verbose mode

`-v` makes `web` narrate what it is doing. It can go before or after the
command.

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

Two properties of this output matter:

- It goes to **standard error**, so `web show x | ...`, `web list | ...` and
  `web variables | ...` stay clean whether or not `-v` is in effect.
- It reports **what `web` did**, never the browser's own chatter. The browser
  is always launched fully detached — its output can't reach your terminal in
  either mode. If you need a browser's diagnostics, run that browser directly.

---

## Where things live

```text
./anything.web                        your documents — anywhere you like
~/.config/webdoc/config.json          your variables
~/.local/state/webdoc/state.json      the global lock
```

`web config` prints all of this, along with the format version and whether the
global lock is on.

There is no database, no index and no background process. If you delete a
`.web` file, it's gone; nothing else needs updating. If you restore one from a
backup, it works immediately.

---

## Troubleshooting

**A `.web` file opens in a text editor.** The file type isn't registered. Run
`sudo make install` (or install the package), then log out and back in. To
check:

```console
$ gio info yourfile.web | grep content-type
  standard::content-type: application/x-webdoc
```

**`web open` says the browser configuration is invalid.** `web` refuses to run
a browser that isn't an absolute path to a regular executable file, or that is
world-writable (anyone could have replaced it). Check what it resolves to with
`web show <name>`.

**Nothing is printed when I expected output.** That's normal. Use `-v`.

**The browser prints into my shell prompt.** It can't. The browser runs in its
own session with its standard streams closed off, in every mode. If you are
seeing this, you are not running `web`.

---

## For developers

The whole thing is a small C library (`libwebdoc`) plus a thin CLI. All
document logic lives in the library, including the single open pipeline that
both `web open` and the double-click handler go through, so lock checks can't
be skipped by opening a document a different way.

```text
include/webdoc.h   public libwebdoc API
src/json.c         vendored strict JSON reader/writer
src/util.c         errno mapping, bounded reads, atomic writes, XDG paths
src/report.c       verbose progress reporting
src/validate.c     URL and variable policy, error strings
src/config.c       variables, global lock state, configuration paths
src/webfile.c      load/save/create/edit/delete, name resolution, locking
src/browser.c      browser resolution, executable validation, detached spawn
src/open.c         the single open pipeline
src/show.c         show and directory listing
src/web.c          CLI frontend (argv parsing and messages only)
tests/             unit tests and end-to-end CLI tests
data/              MIME package, .desktop entry, icon
docs/              format specification, security model, man page
debian/            Debian packaging (one binary package)
```

```console
$ make          # libwebdoc.a, libwebdoc.so and web
$ make check    # unit tests, CLI tests, and validation of the shipped
                # MIME and desktop files
```

Only a C11 compiler and libc are needed — no external dependencies.

A `.web` file looks like this:

```json
{
  "version": 1,
  "url": "https://docs.example.com/",
  "browser": "variable:chrome"
}
```

- [`docs/format.md`](docs/format.md) — the format and its versioning rules
- [`docs/security.md`](docs/security.md) — threat model and mitigations
- [`docs/web.1`](docs/web.1) — the man page (`man web`)

### Packaging

`dpkg-buildpackage -us -uc -b` produces a single `webdoc` package containing
the binary, the man page, the MIME registration, the `.desktop` entry and the
icon. `libwebdoc` is linked statically, so there's no shared library, no `-dev`
package and no ABI to keep stable while the format is at version 1. No
maintainer scripts are needed: dpkg triggers refresh the MIME, desktop and icon
caches.

To serve it from an apt repository:

```console
$ mkdir -p repo/pool && cp webdoc_1.0.0-1_amd64.deb repo/pool/
$ cd repo && dpkg-scanpackages --multiversion pool /dev/null > Packages
$ gzip -k Packages
$ apt-ftparchive release . > Release
$ gpg --clearsign -o InRelease Release
```

## Licence

MIT. See [LICENSE](LICENSE).
