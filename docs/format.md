# The `.web` file format, version 1

A web document is a single file. Everything that persistently belongs to a
document lives inside that file; nothing about an individual document is
recorded anywhere else.

## Encoding

* UTF-8 encoded JSON, one top-level object.
* No byte-order mark.
* The file is written with a trailing newline.
* Readers must reject files larger than 64 KiB.

JSON was chosen over a custom or binary format because a `.web` file is a
tiny, long-lived, user-visible filesystem object: interoperability, a
well-defined grammar, human readability and the availability of parsers in
every language matter far more than the handful of bytes a denser encoding
would save.

## Members

| Member    | Type    | Required | Meaning                                    |
|-----------|---------|----------|--------------------------------------------|
| `version` | integer | yes      | Format version. Currently `1`.              |
| `url`     | string  | yes      | The URL. `http://` or `https://` only.      |
| `browser` | string  | no       | Browser selection (see below).              |
| `locked`  | boolean | no       | Per-document lock state. Absent means unlocked. |

Optional members with no value are omitted rather than written as `null`,
`""` or `false`. The smallest valid document is:

```json
{
  "version": 1,
  "url": "https://www.postgresql.org/"
}
```

A document's *name* is its filename minus the `.web` suffix. The name is
deliberately **not** duplicated inside the file: a document that is renamed,
moved or copied would otherwise carry a stale name around, and the two copies
would disagree about which one is authoritative.

## The `browser` member

The value is a tagged string with exactly one of two prefixes:

* `variable:NAME`, an indirection through a variable defined in the global
  configuration. `NAME` matches `[A-Za-z0-9_-]{1,64}`. Variables are a
  general name/value facility; a variable used here must resolve to an
  absolute path to an executable, which is checked at open time.
* `path:/absolute/path`, one specific executable, stored literally.

```json
{
  "version": 1,
  "url": "https://www.postgresql.org/",
  "browser": "variable:chrome",
  "locked": true
}
```

These tags are a storage detail. The CLI neither asks for them nor prints
them: you write `--browser chrome` and `web show` reports `browser: chrome`.

The variable form stores the *indirection*, never the resolved executable, so
`web set chrome /usr/bin/chromium` immediately changes every document that
references `variable:chrome`. Resolution happens at open time, on every open.

The tag prefix is what distinguishes the two forms. Shell quoting is not used
for this: quotes are consumed by the shell before `web` ever sees `argv`, so
they carry no information the program could act on. The CLI selects the form
with explicit options, `--browser` and `--browser-path`.

An untagged value is invalid and the file is rejected.

## Versioning and extensibility

* A reader that understands version *N* must reject a file whose `version` is
  greater than *N* (`WEBDOC_ERR_UNSUPPORTED_VERSION`) rather than guess.
* Within a version, unknown members are ignored on read and **preserved** on
  write, so a document edited by an older `web` does not silently lose data
  that a newer one added.
* Adding an optional member is a compatible change and does not require a
  version bump. Changing the meaning or type of an existing member, or adding
  a required member, does.

## Writing

Every update is written by creating a temporary file in the same directory,
`fsync()`ing it, and `rename()`ing it over the target, followed by an
`fsync()` of the directory. A reader therefore sees either the old file or
the new one, never a half-written one, even across a crash.

Creation uses `O_CREAT | O_EXCL | O_NOFOLLOW`: `web create` never overwrites
an existing file, and there is no window between a check and the create in
which a symlink could be planted.

## Validity

A file is a valid version 1 document if and only if:

* it parses as a single JSON object with no trailing content and no duplicate
  member names;
* `version` is the integer 1;
* `url` is a string accepted by the URL rules in `docs/security.md`;
* `browser`, if present, is a string with a valid `variable:` or `path:`
  tag;
* `locked`, if present, is a boolean.

Anything else is `WEBDOC_ERR_INVALID_FILE` (or the more specific
`WEBDOC_ERR_INVALID_URL`, `WEBDOC_ERR_INVALID_BROWSER`,
`WEBDOC_ERR_UNSUPPORTED_VERSION`).
