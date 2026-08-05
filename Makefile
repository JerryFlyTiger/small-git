CC       ?= clang
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude
LDFLAGS  ?=

# Feature-test macros. -std=c11 asks for strict ISO C, and on glibc that
# hides everything POSIX behind feature-test macros -- including strdup,
# strtok_r, mkstemp, getcwd and struct stat's st_mtim, all of which this
# code uses. Without this, a Linux build fails on the very #else branches
# written for it. Darwin exposes those by default and instead *hides*
# st_mtimespec once _POSIX_C_SOURCE is set, so the two platforms need
# opposite macros; picking the wrong one breaks the other platform.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
else
  CFLAGS += -D_POSIX_C_SOURCE=200809L
endif

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man
INSTALL ?= install

PKG_CONFIG ?= pkg-config
ZLIB_CFLAGS   := $(shell $(PKG_CONFIG) --cflags zlib)
ZLIB_LIBS     := $(shell $(PKG_CONFIG) --libs zlib)
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs openssl)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS  += $(ZLIB_CFLAGS) $(OPENSSL_CFLAGS) $(CURL_CFLAGS)
LDFLAGS += $(ZLIB_LIBS) $(OPENSSL_LIBS) $(CURL_LIBS)

BUILD_DIR := build
BIN       := $(BUILD_DIR)/sg

SRCS := $(shell find src -name '*.c')
OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TEST_SRCS := $(shell find tests -name '*.c')
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

LIB_OBJS := $(filter-out $(BUILD_DIR)/cli/main.o,$(OBJS))

.PHONY: all clean test debug sanitize release install uninstall

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t =="; $$t || exit 1; done

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean test $(BIN)

# Optimized build. Depends on clean because object files carry the flags they
# were compiled with and make only re-links when sources are newer -- without
# it, switching build modes silently reuses objects from the previous mode.
# The same applies in reverse: run `make clean` before going back to a plain
# `make` after a release or sanitize build.
release: CFLAGS += -O2 -DNDEBUG
release: clean $(BIN)

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(BIN) $(DESTDIR)$(BINDIR)/sg
	$(INSTALL) -d $(DESTDIR)$(MANDIR)/man1
	$(INSTALL) -m 644 docs/sg.1 $(DESTDIR)$(MANDIR)/man1/sg.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/sg
	rm -f $(DESTDIR)$(MANDIR)/man1/sg.1

clean:
	rm -rf $(BUILD_DIR)
