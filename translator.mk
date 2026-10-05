# Builds the forked CFA translator (cfa-cpp) from the cforall/ submodule.
#
# The cforall tree is configured once, out of tree, in TRANSLATOR_DIR. That
# directory does not depend on $(BUILD), so every build in this tree shares
# one translator. Only cforall/src is built; libcfa and the driver are not.
# A lock serialises everything here that writes to the source tree or to
# TRANSLATOR_DIR, so concurrent runs don't step on each other.
#
# Before the first configure, the generated autotools files and
# AST/BasicKind.hpp that the submodule tracks are touched so make never tries
# to regenerate them from a checkout with arbitrary mtimes.
#
# The submodule also tracks the bison and flex output (parser.cc, parser.hh,
# lex.cc). git gives every file it checks out the current time, so on a fresh
# clone parser.yy can be newer than parser.cc. When make finds an output older
# than its grammar, the grammar and the output are compared with the
# submodule's commit. If none of them differ, the output is up to date and is
# only touched. Otherwise it is regenerated in the source tree through
# automake's ylwrap, the way the cforall build does it. So bison and flex are
# needed only after an edit to parser.yy or lex.ll (or outside a git
# checkout).
#
# configure is given YACC and LEX so that it doesn't look for bison and flex,
# and the cforall build never runs them. (configure gives up on flex when
# libfl is missing anyway, so that build could not regenerate lex.cc.)
#
# `make parser` regenerates all three outputs, whatever their mtimes. CI uses
# it to check that the committed output matches the grammar.

TRANSLATOR_DIR  := build/cforall
TRANSLATOR_BIN  := $(TRANSLATOR_DIR)/driver/cfa-cpp
TRANSLATOR_JOBS ?= 8

CFORALL_SRC := $(CURDIR)/cforall
CFA_PARSER  := cforall/src/Parser

# Runs the rest of the line under the translator lock.
TRANSLATOR_LOCK = mkdir -p $(TRANSLATOR_DIR) && unset MAKEFLAGS MFLAGS MAKEOVERRIDES && \
  flock $(TRANSLATOR_DIR)/.lock

# The commands that write the tracked bison and flex output, run from
# cforall/src. Commit what they produce.
YACC_CMD := $(SHELL) ../automake/ylwrap Parser/parser.yy y.tab.c Parser/parser.cc \
  y.tab.h Parser/parser.hh y.output Parser/parser.output -- bison -y -d -t -v -Wno-yacc; \
  rm -f Parser/parser.output; touch Parser/parser.cc Parser/parser.hh
LEX_CMD := $(SHELL) ../automake/ylwrap Parser/lex.ll lex.yy.c Parser/lex.cc -- flex

# $(call regen,GRAMMAR OUTPUTS,COMMAND): recipe for bison or flex output that
# make found older than its grammar (paths relative to cforall/src). git
# fails outside a checkout, which counts as a difference.
regen = $(TRANSLATOR_LOCK) sh -c '\
  set -e; cd $(CFORALL_SRC)/src; \
  if git diff --quiet HEAD -- $(1) 2>/dev/null; then \
    touch $(wordlist 2,$(words $(1)),$(1)); \
  else \
    echo "regenerating $(wordlist 2,$(words $(1)),$(1))"; \
    $(2); \
  fi'

.PHONY: translator parser
translator: $(CFA_PARSER)/parser.cc $(CFA_PARSER)/parser.hh $(CFA_PARSER)/lex.cc
	@$(TRANSLATOR_LOCK) sh -c '\
	  set -e; \
	  if [ ! -f $(TRANSLATOR_DIR)/config.status ]; then \
	    echo "configuring cforall in $(TRANSLATOR_DIR)"; \
	    cd $(CFORALL_SRC); \
	    touch aclocal.m4; \
	    touch configure src/config.h.in Makefile.in */Makefile.in */*/Makefile.in tools/prettyprinter/Makefile.in; \
	    touch src/AST/BasicKind.hpp; \
	    cd $(CURDIR)/$(TRANSLATOR_DIR); \
	    $(CFORALL_SRC)/configure --quiet --disable-gprofiler YACC="bison -y" LEX=: \
	      >configure.log 2>&1 || { cat configure.log >&2; exit 1; }; \
	  fi; \
	  $(MAKE) --no-print-directory -s -C $(CURDIR)/$(TRANSLATOR_DIR)/src -j$(TRANSLATOR_JOBS) ../driver/cfa-cpp'

$(CFA_PARSER)/parser.cc $(CFA_PARSER)/parser.hh &: $(CFA_PARSER)/parser.yy
	@$(call regen,Parser/parser.yy Parser/parser.cc Parser/parser.hh,$(YACC_CMD))

$(CFA_PARSER)/lex.cc: $(CFA_PARSER)/lex.ll
	@$(call regen,Parser/lex.ll Parser/lex.cc,$(LEX_CMD))

parser:
	@$(TRANSLATOR_LOCK) sh -c 'set -e; cd $(CFORALL_SRC)/src; $(YACC_CMD); $(LEX_CMD)'

# compile_commands.json entries for cforall/src, one per line, each starting
# with a comma. The configured Makefile expands its own compile command and
# source list (compile-commands.mk), so the entries match the real build. The
# grep drops anything else that make prints, such as config.status output if
# the Makefile is out of date. There are none until `make translator` has
# configured TRANSLATOR_DIR.
TRANSLATOR_MAKEFILE := $(wildcard $(TRANSLATOR_DIR)/src/Makefile)
TRANSLATOR_COMPILE_COMMANDS = \
  if [ -f $(TRANSLATOR_DIR)/src/Makefile ]; then \
    (unset MAKEFLAGS MFLAGS MAKEOVERRIDES; \
     $(MAKE) --no-print-directory -s -C $(TRANSLATOR_DIR)/src \
       -f Makefile -f $(CURDIR)/compile-commands.mk cfa-lsp-compile-commands) \
      | grep '^,{"directory"'; \
  else \
    echo "no compile commands for cforall/src until make translator configures $(TRANSLATOR_DIR)" >&2; \
  fi
