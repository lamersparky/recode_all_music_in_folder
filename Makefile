CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=
CFLAGS ?= -O2 -g
CPPFLAGS += -D_FORTIFY_SOURCE=2
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 openssl 2>/dev/null || echo '-lsqlite3 -lcrypto')
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 openssl 2>/dev/null)

PROGRAM = recode_all_music_in_folder
SOURCE = src/recode_all_music_in_folder.c

.PHONY: all clean install uninstall test
all: $(PROGRAM)

$(PROGRAM): $(SOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $< $(LDLIBS)

install: $(PROGRAM)
	install -Dm755 $(PROGRAM) $(DESTDIR)$(BINDIR)/$(PROGRAM)
	install -Dm644 completions/recode_all_music_in_folder.bash $(DESTDIR)$(DATADIR)/bash-completion/completions/$(PROGRAM)
	install -Dm644 completions/_recode_all_music_in_folder $(DESTDIR)$(DATADIR)/zsh/site-functions/_$(PROGRAM)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROGRAM)
	rm -f $(DESTDIR)$(DATADIR)/bash-completion/completions/$(PROGRAM)
	rm -f $(DESTDIR)$(DATADIR)/zsh/site-functions/_$(PROGRAM)

clean:
	rm -f $(PROGRAM)

# Safe smoke test: no music is modified and the test state lives in a temporary directory.
test: $(PROGRAM)
	./tests/smoke.sh
	./tests/metadata.sh
	./tests/escape-navigation.sh
	python3 ./tests/escape-terminal.py
