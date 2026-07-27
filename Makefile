CC       ?= clang
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude
LDFLAGS  ?=

PKG_CONFIG ?= pkg-config
ZLIB_CFLAGS   := $(shell $(PKG_CONFIG) --cflags zlib)
ZLIB_LIBS     := $(shell $(PKG_CONFIG) --libs zlib)
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs openssl)

CFLAGS  += $(ZLIB_CFLAGS) $(OPENSSL_CFLAGS)
LDFLAGS += $(ZLIB_LIBS) $(OPENSSL_LIBS)

BUILD_DIR := build
BIN       := $(BUILD_DIR)/sg

SRCS := $(shell find src -name '*.c')
OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TEST_SRCS := $(shell find tests -name '*.c')
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

LIB_OBJS := $(filter-out $(BUILD_DIR)/cli/main.o,$(OBJS))

.PHONY: all clean test debug sanitize

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
sanitize: clean test

clean:
	rm -rf $(BUILD_DIR)
