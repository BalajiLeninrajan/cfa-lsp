# cfa-lsp: a Cforall language server built on a fork of the CFA translator.
#
#   make              server + forked translator
#   make server       just build/cfa-lsp
#   make translator   just the forked cfa-cpp (translator.mk)
#   make test         unit and integration tests
#   make install      PREFIX=~/.local (uninstall removes it again)
#   make compile_commands.json
#
# BUILD can be overridden (make BUILD=build/foo) so several builds can run in
# one tree without sharing object files.

CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -MMD -MP
CPPFLAGS += -Isrc -Isrc/analysis -Ithird_party
LDFLAGS  ?=
LDLIBS   += -pthread

BUILD   ?= build
PREFIX  ?= $(HOME)/.local

ANALYSIS_SRCS := $(wildcard src/analysis/*.cpp)
SERVER_SRCS   := $(filter-out src/server/main.cpp,$(wildcard src/server/*.cpp))
MAIN_SRC      := $(wildcard src/server/main.cpp)
TEST_SRCS     := $(wildcard tests/*.cpp tests/*/*.cpp)

obj = $(patsubst %.cpp,$(BUILD)/obj/%.o,$(1))

ANALYSIS_OBJS := $(call obj,$(ANALYSIS_SRCS))
SERVER_OBJS   := $(call obj,$(SERVER_SRCS))
MAIN_OBJ      := $(call obj,$(MAIN_SRC))
TEST_OBJS     := $(call obj,$(TEST_SRCS))

SERVER_BIN := $(BUILD)/cfa-lsp
TEST_BIN   := $(BUILD)/cfa-lsp-tests

.PHONY: all server test install uninstall clean
all: server translator

server: $(SERVER_BIN)

$(SERVER_BIN): $(MAIN_OBJ) $(SERVER_OBJS) $(ANALYSIS_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_BIN): $(TEST_OBJS) $(SERVER_OBJS) $(ANALYSIS_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# Stand-in for cfa, cfa-cpp and gcc in the server tests.
FAKE_CFA := $(BUILD)/cfa-lsp-fake-cfa

$(FAKE_CFA): tests/server/fake/fake_cfa.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out -MMD -MP,$(CXXFLAGS)) -o $@ $<

# Tests find the binaries and fixtures through these variables. Tests that
# need the translator skip themselves if it isn't built, so `make test` works
# before `make translator` (which takes a while the first time). Pass doctest
# options with ARGS, e.g. ARGS='-tc="*hover*"'.
test: $(TEST_BIN) $(FAKE_CFA) $(SERVER_BIN)
	CFA_LSP_SERVER=$(abspath $(SERVER_BIN)) CFA_LSP_TRANSLATOR=$(abspath $(TRANSLATOR_BIN)) \
	CFA_LSP_FAKE_CFA=$(abspath $(FAKE_CFA)) \
	CFA_LSP_FIXTURES=$(abspath tests/fixtures) $(TEST_BIN) $(ARGS)

# Stripped: the translator is built with -g and is about 400 MB otherwise.
install: all
	install -Dm755 -s $(SERVER_BIN) $(PREFIX)/bin/cfa-lsp
	install -Dm755 -s $(TRANSLATOR_BIN) $(PREFIX)/libexec/cfa-lsp/cfa-cpp

uninstall:
	rm -f $(PREFIX)/bin/cfa-lsp $(PREFIX)/libexec/cfa-lsp/cfa-cpp
	-rmdir $(PREFIX)/libexec/cfa-lsp

# One entry per server/analysis/test source, for clangd.
compile_commands.json: Makefile $(MAIN_SRC) $(SERVER_SRCS) $(ANALYSIS_SRCS) $(TEST_SRCS)
	@{ echo '['; sep=''; for f in $(MAIN_SRC) $(SERVER_SRCS) $(ANALYSIS_SRCS) $(TEST_SRCS); do \
	    printf '%s{"directory":"%s","file":"%s","command":"%s %s %s -c %s"}\n' \
	      "$$sep" "$(CURDIR)" "$$f" "$(CXX)" "$(CPPFLAGS)" "$(CXXFLAGS)" "$$f"; sep=','; \
	  done; echo ']'; } > $@

clean:
	rm -rf $(BUILD)/obj $(SERVER_BIN) $(TEST_BIN) $(FAKE_CFA)

# Defines `translator` and TRANSLATOR_BIN (the forked cfa-cpp).
include translator.mk

-include $(ANALYSIS_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJS:.o=.d)
