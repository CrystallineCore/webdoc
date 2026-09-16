# webdoc - Makefile
# SPDX-License-Identifier: MIT

VERSION     := 1.0.0
SONAME_MAJ  := 1

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
LIBDIR      ?= $(PREFIX)/lib
INCLUDEDIR  ?= $(PREFIX)/include
DATADIR     ?= $(PREFIX)/share
MANDIR      ?= $(DATADIR)/man
DESTDIR     ?=

CC          ?= cc
CSTD        ?= -std=c11
WARN        := -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings \
               -Wmissing-prototypes -Wstrict-prototypes -Wformat=2
# Only set _FORTIFY_SOURCE when the caller has not: Debian's build flags
# already pass =3, and redefining it on the command line warns on every file.
FORTIFY     := $(if $(findstring _FORTIFY_SOURCE,$(CPPFLAGS) $(CFLAGS)),,-D_FORTIFY_SOURCE=2)
HARDEN      := $(FORTIFY) -fstack-protector-strong
CPPFLAGS    += -Iinclude -Isrc -DWEBDOC_VERSION=\"$(VERSION)\"
CFLAGS      ?= -O2 -g
CFLAGS      += $(CSTD) $(WARN) $(HARDEN)
LDFLAGS     += -Wl,-z,relro,-z,now

LIB_SRCS    := src/json.c src/util.c src/report.c src/validate.c src/config.c \
               src/webfile.c src/browser.c src/open.c src/show.c
LIB_OBJS    := $(LIB_SRCS:.c=.o)
LIB_PIC     := $(LIB_SRCS:.c=.lo)

STATIC_LIB  := libwebdoc.a
SHARED_LIB  := libwebdoc.so.$(VERSION)
SONAME      := libwebdoc.so.$(SONAME_MAJ)

BIN         := web
BIN_SRCS    := src/web.c

TEST_BIN    := tests/test_unit

.PHONY: all clean check check-data install install-bin uninstall install-data

all: $(STATIC_LIB) $(SHARED_LIB) $(BIN)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.lo: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_PIC)
	$(CC) -shared -Wl,-soname,$(SONAME) $(LDFLAGS) -o $@ $^
	ln -sf $(SHARED_LIB) $(SONAME)
	ln -sf $(SONAME) libwebdoc.so

# The CLI links the static library so the build tree is trivially runnable.
$(BIN): $(BIN_SRCS) $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(BIN_SRCS) $(STATIC_LIB)

$(TEST_BIN): tests/test_unit.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_unit.c $(STATIC_LIB)

check: $(TEST_BIN) $(BIN) check-data
	./$(TEST_BIN)
	sh tests/test_cli.sh ./$(BIN)

# The MIME package and desktop entry are shipped files that only fail at
# install time on a user's machine, so validate them here when the tools
# exist.  A malformed comment is enough to make update-mime-database skip
# the file entirely, leaving .web registered as nothing at all.
check-data:
	@if command -v xmllint >/dev/null 2>&1; then \
		xmllint --noout data/mime/packages/webdoc.xml && \
		echo "mime package: ok"; \
	else \
		python3 -c "import xml.dom.minidom as m; m.parse('data/mime/packages/webdoc.xml'); print('mime package: ok')"; \
	fi
	@if command -v desktop-file-validate >/dev/null 2>&1; then \
		desktop-file-validate data/applications/webdoc.desktop && \
		echo "desktop entry: ok"; \
	else \
		echo "desktop entry: skipped (desktop-file-validate not installed)"; \
	fi

# Everything a user needs: the CLI, the man page and the desktop files.
# This is what the Debian package ships.
install-bin: $(BIN) install-data
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

# Additionally installs the library and header, for building against libwebdoc.
install: all install-bin
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)
	install -m 0644 include/webdoc.h $(DESTDIR)$(INCLUDEDIR)/webdoc.h
	install -m 0644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/$(STATIC_LIB)
	install -m 0755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/$(SHARED_LIB)
	ln -sf $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libwebdoc.so

install-data:
	install -d $(DESTDIR)$(DATADIR)/mime/packages
	install -m 0644 data/mime/packages/webdoc.xml \
		$(DESTDIR)$(DATADIR)/mime/packages/webdoc.xml
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 0644 data/applications/webdoc.desktop \
		$(DESTDIR)$(DATADIR)/applications/webdoc.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps
	install -m 0644 data/icons/hicolor/scalable/apps/application-x-webdoc.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/application-x-webdoc.svg
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 0644 docs/web.1 $(DESTDIR)$(MANDIR)/man1/web.1
# Registering the file type is part of installing.  A staged install
# (DESTDIR set, i.e. package building) skips this: dpkg triggers refresh
# the caches themselves, and writing to the live system from a build is wrong.
ifeq ($(DESTDIR),)
	-update-mime-database $(DATADIR)/mime
	-update-desktop-database $(DATADIR)/applications
	-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) \
	      $(DESTDIR)$(INCLUDEDIR)/webdoc.h \
	      $(DESTDIR)$(LIBDIR)/$(STATIC_LIB) \
	      $(DESTDIR)$(LIBDIR)/$(SHARED_LIB) \
	      $(DESTDIR)$(LIBDIR)/$(SONAME) \
	      $(DESTDIR)$(LIBDIR)/libwebdoc.so \
	      $(DESTDIR)$(DATADIR)/mime/packages/webdoc.xml \
	      $(DESTDIR)$(DATADIR)/applications/webdoc.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/application-x-webdoc.svg \
	      $(DESTDIR)$(MANDIR)/man1/web.1

clean:
	rm -f $(LIB_OBJS) $(LIB_PIC) $(STATIC_LIB) $(SHARED_LIB) \
	      $(SONAME) libwebdoc.so $(BIN) $(TEST_BIN)
