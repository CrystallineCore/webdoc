#!/bin/sh
# SPDX-License-Identifier: MIT
#
# End-to-end tests for the web CLI.
#
# Usage:
#   sh tests/test_cli.sh [path-to-web]
#
# The binary may be given explicitly.  With no argument it is looked for
# relative to this script, so the suite works whether it is run from the
# source root (`make check`, `sh tests/test_cli.sh`) or from inside tests/
# (`./test_cli.sh`), and the current directory does not matter.
set -u

# Directory holding this script, resolved to an absolute path.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
    WEB=$1
    case "$WEB" in
        /*) ;;
        *) WEB="$PWD/$WEB" ;;
    esac
    if [ ! -x "$WEB" ] || [ ! -f "$WEB" ]; then
        printf '%s: not an executable file\n' "$WEB" >&2
        exit 1
    fi
else
    WEB=
    for candidate in "$script_dir/../web" "$script_dir/web" "$PWD/web"; do
        if [ -x "$candidate" ] && [ -f "$candidate" ]; then
            WEB=$candidate
            break
        fi
    done
    if [ -z "$WEB" ]; then
        cat >&2 <<'USAGE'
test_cli.sh: the web binary was not found.

Build it first, from the source root:

    make web

then run the suite with `make check`, or point this script at the binary:

    sh tests/test_cli.sh /path/to/web
USAGE
        exit 1
    fi
fi

# Normalise to an absolute path: the tests chdir into a temporary directory.
WEB=$(CDPATH= cd -- "$(dirname -- "$WEB")" && pwd)/$(basename -- "$WEB") || exit 1

TMP=$(mktemp -d /tmp/web-cli-XXXXXX) || exit 1
export HOME="$TMP"
export XDG_CONFIG_HOME="$TMP/cfg"
export XDG_STATE_HOME="$TMP/state"
cd "$TMP" || exit 1

pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); printf 'FAIL: %s\n' "$1" >&2; }

# expect_status <expected> <description> <command...>
expect_status() {
    want=$1; desc=$2; shift 2
    "$@" >out.txt 2>err.txt
    got=$?
    if [ "$got" = "$want" ]; then ok; else
        bad "$desc (exit $got, wanted $want)"
        sed 's/^/    /' err.txt >&2
    fi
}

expect_file() {
    if [ -f "$1" ]; then ok; else bad "expected file $1"; fi
}

expect_no_file() {
    if [ -f "$1" ]; then bad "unexpected file $1"; else ok; fi
}

expect_grep() {
    if grep -q "$1" "$2"; then ok; else
        bad "expected /$1/ in $2"
        sed 's/^/    /' "$2" >&2
    fi
}

# --- create ---------------------------------------------------------------
expect_status 0 "create" "$WEB" create postgres https://www.postgresql.org/
expect_file postgres.web
expect_grep '"url": "https://www.postgresql.org/"' postgres.web

expect_status 1 "create must not overwrite" \
    "$WEB" create postgres https://example.com/
expect_grep 'already exists' err.txt

# .web suffix is never doubled
expect_status 0 "create with .web suffix" \
    "$WEB" create kernel.web https://kernel.org/
expect_file kernel.web
expect_no_file kernel.web.web

# paths
mkdir -p Docs
expect_status 0 "create in subdirectory" \
    "$WEB" create Docs/debian https://www.debian.org/
expect_file Docs/debian.web

# rejected schemes and hostile URLs
expect_status 1 "reject ftp" "$WEB" create bad ftp://example.com/
expect_no_file bad.web
expect_status 1 "reject javascript" "$WEB" create bad2 'javascript:alert(1)'
expect_status 1 "reject whitespace in URL" "$WEB" create bad3 'https://a b.com/'
expect_status 1 "reject control chars" \
    "$WEB" create bad4 "$(printf 'https://a.example/\nevil')"

# --- show -----------------------------------------------------------------
expect_status 0 "show link" "$WEB" show postgres
expect_grep 'url:     https://www.postgresql.org/' out.txt
expect_status 0 "show link by filename" "$WEB" show postgres.web
# show refuses directories; list is the directory command
expect_status 2 "show refuses a directory" "$WEB" show .
expect_grep 'web list' err.txt
expect_status 2 "show refuses a directory via --path" "$WEB" show --path "$TMP"
expect_status 0 "list current directory" "$WEB" list
expect_grep 'postgres' out.txt
expect_status 0 "list explicit directory" "$WEB" list .
expect_grep 'postgres' out.txt
expect_status 0 "list subdirectory" "$WEB" list Docs
expect_grep 'debian' out.txt
expect_status 2 "list refuses a single document" "$WEB" list postgres.web
expect_status 1 "list missing directory" "$WEB" list nosuchdir
expect_status 1 "show missing" "$WEB" show nosuchdoc

# malformed files are reported, not crashed on
printf 'this is not json' > broken.web
expect_status 1 "show malformed" "$WEB" show broken
expect_status 0 "list tolerates malformed entries" "$WEB" list
expect_grep 'broken' out.txt

# non-regular files are refused rather than hung on
mkfifo fifo.web 2>/dev/null && expect_status 1 "refuse FIFO" "$WEB" show fifo
rm -f fifo.web

# --- edit -----------------------------------------------------------------
expect_status 0 "edit url" "$WEB" edit postgres https://www.postgresql.org/docs/
expect_grep 'docs' postgres.web
expect_status 1 "edit rejects bad url" "$WEB" edit postgres ftp://x.example/
expect_status 2 "edit with nothing to do" "$WEB" edit postgres

# --- variables and browsers ----------------------------------------------
mkdir -p bin
cat > bin/fakebrowser <<'EOF'
#!/bin/sh
printf '%s|%s\n' "$#" "$1" >> "$HOME/launched.txt"
EOF
chmod 755 bin/fakebrowser

expect_status 0 "set variable" "$WEB" set fake "$TMP/bin/fakebrowser"
expect_grep '"fake"' "$XDG_CONFIG_HOME/webdoc/config.json"
expect_grep '"variables"' "$XDG_CONFIG_HOME/webdoc/config.json"
expect_status 0 "variables are not browser-specific" "$WEB" set editor 'nvim --clean'
expect_status 1 "set rejects bad name" "$WEB" set 'bad name' /usr/bin/x

expect_status 0 "attach variable" "$WEB" edit postgres --browser fake
expect_grep '"browser": "variable:fake"' postgres.web
# the user never sees the storage tag
expect_status 0 "show hides storage tags" "$WEB" show postgres
expect_grep 'browser: fake' out.txt
if grep -q 'variable:' out.txt; then bad "show leaked the storage tag"; else ok; fi

expect_status 0 "open via variable" "$WEB" open postgres
sleep 0.3
expect_grep '^1|https://www.postgresql.org/docs/$' launched.txt

# the URL reaches the browser as exactly one argv element, unexpanded
expect_status 0 "create URL with shell metacharacters" \
    "$WEB" create tricky 'https://example.com/?a=$(id)&b=%60id%60;rm'
expect_status 0 "attach browser" "$WEB" edit tricky --browser fake
expect_status 0 "open tricky" "$WEB" open tricky
sleep 0.3
expect_grep '1|https://example.com/?a=\$(id)&b=%60id%60;rm' launched.txt

# literal browser path
expect_status 0 "literal browser path" \
    "$WEB" edit tricky --browser-path "$TMP/bin/fakebrowser"
expect_grep "\"browser\": \"path:$TMP/bin/fakebrowser\"" tricky.web
expect_status 1 "reject relative browser path" \
    "$WEB" edit tricky --browser-path bin/fakebrowser

# a world-writable browser is refused
chmod 777 bin/fakebrowser
expect_status 1 "refuse world-writable browser" "$WEB" open tricky
chmod 755 bin/fakebrowser

# undefined variable
expect_status 0 "attach undefined variable" "$WEB" edit tricky --browser ghost
expect_status 1 "open with undefined variable" "$WEB" open tricky
expect_grep 'variable' err.txt

expect_status 0 "unset variable" "$WEB" unset fake
expect_status 1 "unset twice" "$WEB" unset fake

# --- locking --------------------------------------------------------------
expect_status 0 "set variable again" "$WEB" set fake "$TMP/bin/fakebrowser"
expect_status 0 "attach variable" "$WEB" edit postgres --browser fake

expect_status 0 "lock one link" "$WEB" lock postgres
expect_grep '"locked": true' postgres.web
expect_status 3 "locked link refuses to open" "$WEB" open postgres

# a copy carries its lock with it
cp postgres.web copy.web
expect_status 3 "copied link stays locked" "$WEB" open copy

# global unlock does not clear a per-document lock
expect_status 0 "global unlock" "$WEB" unlock
expect_status 3 "still locked locally" "$WEB" open postgres

expect_status 0 "unlock one link" "$WEB" unlock postgres
expect_status 0 "opens again" "$WEB" open postgres

# global lock covers unlocked documents
expect_status 0 "global lock" "$WEB" lock
expect_status 3 "global lock blocks open" "$WEB" open postgres
# per-document unlock does not clear the global lock
expect_status 0 "unlock one link under global lock" "$WEB" unlock postgres
expect_status 3 "global lock still blocks" "$WEB" open postgres
expect_status 0 "global unlock" "$WEB" unlock
expect_status 0 "opens again" "$WEB" open postgres

# --- symlinks -------------------------------------------------------------
expect_status 0 "create symlink target" "$WEB" create real https://a.example/
ln -sf real.web alias.web
expect_status 0 "edit through symlink" "$WEB" edit alias https://b.example/
if [ -L alias.web ]; then ok; else bad "edit replaced the symlink itself"; fi
expect_grep 'b.example' real.web

# --- --path ---------------------------------------------------------------
expect_status 0 "show --path" "$WEB" show --path "$TMP/postgres.web"
expect_status 1 "--path does not add a suffix" \
    "$WEB" show --path "$TMP/postgres"

# --- output discipline and browser detachment ----------------------------
# A browser that chatters and lingers must never reach the caller's terminal,
# with or without -v.  This is the regression test for a verbose open that
# held the terminal open until interrupted.
cat > bin/chatty <<'CHATEOF'
#!/bin/sh
echo "Opening in existing browser session."
echo "warning: something" >&2
sleep 5
CHATEOF
chmod 755 bin/chatty
expect_status 0 "set chatty browser" "$WEB" set fake "$TMP/bin/chatty"

# quiet by default
"$WEB" open postgres >quiet-out.txt 2>quiet-err.txt
sleep 1
if [ -s quiet-out.txt ] || [ -s quiet-err.txt ]; then
    bad "browser output leaked into the caller's terminal"
    cat quiet-out.txt quiet-err.txt >&2
else ok; fi

# `web open` returns immediately; it does not wait for the browser to exit
before=$(date +%s)
"$WEB" open postgres >/dev/null 2>&1
after=$(date +%s)
if [ "$((after - before))" -lt 3 ]; then ok; else
    bad "open blocked for the lifetime of the browser"
fi

# verbose reports webdoc's own steps on stderr, and still no browser output
"$WEB" open postgres -v >verbose-out.txt 2>verbose-err.txt
sleep 1
if [ -s verbose-out.txt ]; then
    bad "verbose wrote to stdout"
    cat verbose-out.txt >&2
else ok; fi
expect_grep 'opened' verbose-err.txt
expect_grep 'lock check passed' verbose-err.txt
expect_grep 'detached from this terminal' verbose-err.txt
if grep -q 'Opening in existing browser session' verbose-err.txt; then
    bad "verbose let the browser write to the terminal"
else ok; fi
if grep -q 'warning: something' verbose-err.txt; then
    bad "verbose let the browser write to the terminal"
else ok; fi

# -v before the subcommand works too
"$WEB" -v open postgres >/dev/null 2>global-v.txt
sleep 1
expect_grep 'opened' global-v.txt

expect_status 0 "restore browser" "$WEB" set fake "$TMP/bin/fakebrowser"

# --- variables and config ------------------------------------------------
expect_status 0 "variables" "$WEB" variables
expect_grep 'fake' out.txt
expect_grep 'editor' out.txt
# key/value pairs only: no JSON, no storage tags
if grep -q '[{}"]' out.txt; then bad "variables leaked the storage format"; else ok; fi

expect_status 0 "config" "$WEB" config
expect_grep 'global lock' out.txt
expect_grep 'config file' out.txt
expect_grep 'variables' out.txt
expect_status 2 "config takes no arguments" "$WEB" config extra

# --- delete ---------------------------------------------------------------
expect_status 0 "delete" "$WEB" delete copy
expect_no_file copy.web
expect_status 1 "delete missing" "$WEB" delete copy
expect_status 1 "refuse to delete a directory" "$WEB" delete --path "$TMP/Docs"

# --- usage ----------------------------------------------------------------
expect_status 2 "unknown command" "$WEB" frobnicate
expect_status 0 "help" "$WEB" --help
expect_status 0 "version" "$WEB" --version

printf '%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
    rm -rf "$TMP"
    exit 0
fi
printf 'test data left in %s\n' "$TMP" >&2
exit 1
