CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=
CFLAGS ?= -O2 -g
CPPFLAGS += -D_FORTIFY_SOURCE=2
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow
LDFLAGS ?=
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 openssl 2>/dev/null || echo '-lsqlite3 -lcrypto')
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 openssl 2>/dev/null)

PROGRAM = recode_all_music_in_folder
SOURCE = src/recode_all_music_in_folder.c

.PHONY: all clean install uninstall test
all: $(PROGRAM)

$(PROGRAM): $(SOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(PROGRAM)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROGRAM) $(DESTDIR)$(BINDIR)/$(PROGRAM)
	install -d $(DESTDIR)$(DATADIR)/bash-completion/completions
	install -m 644 completions/recode_all_music_in_folder.bash $(DESTDIR)$(DATADIR)/bash-completion/completions/$(PROGRAM)
	install -d $(DESTDIR)$(DATADIR)/zsh/site-functions
	install -m 644 completions/_recode_all_music_in_folder $(DESTDIR)$(DATADIR)/zsh/site-functions/_$(PROGRAM)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROGRAM)
	rm -f $(DESTDIR)$(DATADIR)/bash-completion/completions/$(PROGRAM)
	rm -f $(DESTDIR)$(DATADIR)/zsh/site-functions/_$(PROGRAM)

clean:
	rm -f $(PROGRAM)

# Safe tests use synthetic data in temporary directories; no real audio is changed.
test: $(PROGRAM)
	./tests/smoke.sh
	./tests/metadata.sh
	./tests/escape-navigation.sh
	python3 ./tests/escape-terminal.py
	./tests/windows-batch.sh
	./tests/macos-state-path.sh
