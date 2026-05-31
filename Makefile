# aalloc — Makefile
#
# Targets:
#   all      Build static + shared libraries (default)
#   tests    Build and run tests
#   clean    Remove build artefacts
#
# Variables:
#   CC       Compiler (default: gcc)
#   DEBUG=1  Debug build with assertions

CC        ?= gcc
AR        ?= ar
BUILD     ?= build

WARN  := -Wall -Wextra -Wpedantic -Wshadow
STD   := -std=c11
INC   := -I$(CURDIR)/include
CFLAGS := $(STD) $(WARN) $(INC) $(CFLAGS_EXTRA)

ifeq ($(DEBUG),1)
  CFLAGS += -g3 -O0
else
  CFLAGS += -O2 -DNDEBUG
endif

LDFLAGS := -pthread

SRC  := src/aalloc.c
OBJ  := $(BUILD)/aalloc.o
LIB_A := $(BUILD)/libaalloc.a
LIB_SO := $(BUILD)/libaalloc.so

.PHONY: all tests clean

all: $(LIB_A) $(LIB_SO)

$(BUILD)/aalloc.o: $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(LIB_A): $(OBJ)
	$(AR) rcs $@ $^

$(LIB_SO): $(OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $^

# Tests
TEST_BIN := $(BUILD)/test_basic

tests: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_basic.c $(LIB_A) | $(BUILD)
	$(CC) $(CFLAGS) $< -L$(BUILD) -laalloc $(LDFLAGS) -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
