# Security model

## Trust boundaries

Everything below is untrusted input, including files the user owns: a `.web`
file may have arrived by email, in a tarball, in a git repository or on a
shared filesystem.

* URLs
* `.web` files and every JSON member inside them
* browser paths and browser variable values
* filesystem paths given on the command line
* directory contents when listing

The only trusted input is the code itself. `web` is not installed setuid and
grants no privilege the invoking user does not already have; the threat model
is *a hostile file or path tricking the user's own session into doing
something they did not ask for*.

## Command and shell injection

There is no shell anywhere in libwebdoc. No `system()`, no `popen()`, no
`execl("/bin/sh", ...)`, no command string is ever constructed.

The browser is launched with `posix_spawn()` (or `posix_spawnp()` for
`xdg-open`) and an explicit `argv` array:

```
argv[0] = executable
argv[1] = url
argv[2] = NULL
```

The URL is one argv element. It cannot be word-split, globbed, or interpreted
as a redirection, substitution or metacharacter, because no parser that
understands those things is in the path. The URL validation below is defence
in depth on top of that, not the only defence.

Directory listing uses `opendir()`/`readdir()`/`stat()` and formats its own
output; it never shells out to `ls` and never expands a glob, so a file named
`--help.web` or `$(id).web` is just a filename.

## URL policy

Accepted: `http://` and `https://`, case-insensitively, up to 2048 bytes.

Rejected:

* every other scheme, including `file:`, `javascript:` and `data:`, which are
  the schemes that turn "open a bookmark" into local file disclosure or script
  execution in the browser;
* bytes outside `0x21` to `0x7E`: control characters (notably `\n` and `\r`,
  which smuggle extra lines into logs and `.desktop` files), spaces, and raw
  non-ASCII, which must be percent-encoded or IDN-encoded by the caller;
* characters RFC 3986 excludes from URIs anyway: `" < > \ ^ `` { | }`;
* an empty or over-long host;
* userinfo (`user:password@host`): credentials in a file that gets copied
  around are a secret-leak hazard, and `https://example.com@evil.example/` is
  a classic phishing shape.

Validation happens on creation, on every load, and again before opening.

## Executing a browser

A browser path comes from an untrusted file or from configuration, and it is
about to be executed, so before spawning:

* the path must be absolute, so there is no `PATH` search for an
  attacker-chosen name, and no `..` components;
* it must be a regular file (not a directory, FIFO or device);
* it must be executable by the current user;
* it must not be world-writable, and must not sit in a world-writable
  directory that lacks the sticky bit. Either would let any local user
  replace the binary we are about to run.

If a document specifies no browser, `xdg-open` is used and the desktop's own
default applies. `web` never modifies the user's default browser.

## The launched browser and the caller's terminal

The browser is started in a new session (`setsid()`) with all three of its
standard streams redirected to `/dev/null`, and is reparented to init by a
double fork. This is unconditional: no verbose mode, debug flag or
environment variable attaches it to the caller's terminal.

That matters beyond tidiness. A long-lived process that keeps a terminal it
was not meant to keep can write to that terminal after the user has moved on,
interleaving its own text with the shell's prompt and with whatever the user
is typing, and can read the user's keystrokes out from under the shell. The
result is a session that looks hung and input that lands somewhere the user
did not intend. Detaching removes the whole class of problem rather than
managing it.

`web open` waits only for the short-lived intermediate child, never for the
browser, so it returns immediately and leaves no zombie behind.

## Filesystem safety

* **Atomic replacement.** Updates write a temporary file in the same
  directory, `fsync()`, `rename()`, then `fsync()` the directory. An
  interrupted write leaves the previous version intact.
* **Exclusive creation.** `create` uses `O_CREAT | O_EXCL | O_NOFOLLOW`, which
  is a single atomic operation: there is no check-then-create window, and a
  pre-planted symlink at the target causes failure rather than a write through
  it.
* **Symlinks.** Reading follows symlinks, which is the behaviour users expect
  from a normal file. Writing resolves the path with `realpath()` first and
  replaces the *target*, so an edit neither destroys the symlink nor lands
  somewhere the resolved path did not point. `delete` uses `lstat()` and
  removes the name it was given.
* **Non-regular files.** Loading a document requires `S_ISREG`. A `.web` that
  is really a FIFO cannot block the process forever, and a directory or device
  node is rejected outright.
* **TOCTOU.** Nothing assumes a path is unchanged between operations:
  decisions are made on file descriptors and on atomic syscalls rather than on
  the result of an earlier `stat()`.
* **Concurrency.** Two concurrent `web edit` runs are last-writer-wins at the
  granularity of the whole file, and a reader always sees one complete
  version. There is no lock file and no daemon; a document is small enough
  that whole-file replacement is the simplest correct answer.

## Resource limits

* `.web` and configuration files: 64 KiB maximum.
* JSON: nesting depth 32, 1024 members or elements per container, 16 KiB per
  string; duplicate object keys are rejected (they are a classic
  parser-differential trick), as is trailing content after the document.
* URLs: 2048 bytes. Variable names: 64 bytes. Paths: 4096 bytes.

Malformed input yields a specific error code, never a crash: the parser is
bounds-checked throughout and every allocation failure is propagated.

## Locking

The lock is a *user intent* mechanism, meaning "do not open this right now",
and not a security boundary. It is checked in exactly one place, `webdoc_open()`, which is
the single pipeline used by the CLI and by the desktop MIME handler alike, so
there is no second way to open a document that skips the check. Anyone who can
read the `.web` file can of course read the URL and open it by other means;
that is expected and is not what locking defends against.

Locking is implemented as a stored boolean, not with signals, `flock()`,
interrupts, or a state machine.

## What is deliberately out of scope

Icon fetching, remote metadata, HTTP requests, redirect following and image
parsing are **not** implemented. They are named here because they are the
natural next features, and each brings its own class of problems (SSRF,
decompression bombs, malicious image parsers, unbounded downloads). webdoc
makes no network requests of its own at all.
