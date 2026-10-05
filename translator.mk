# Builds the forked CFA translator (cfa-cpp) from the cforall/ submodule.
#
# The cforall tree is configured once, out of tree, in TRANSLATOR_DIR. That
# directory does not depend on $(BUILD), so every build in this tree shares
# one translator. Only cforall/src is built; libcfa and the driver are not.
# A lock serialises concurrent `make translator` runs on the shared directory.
#
# Before the first configure, the generated files that the submodule tracks
# (autotools output, the bison/flex output, AST/BasicKind.hpp) are touched so
# make never tries to regenerate them from a checkout with arbitrary mtimes.
#
# The submodule tracks the bison and flex output, so after an edit to
# parser.yy or lex.ll they are regenerated in the source tree (the same way
# the cforall build does it, through automake's ylwrap) before the translator
# is built. configure gives up on flex when libfl is missing, so the cforall
# build itself could not regenerate lex.cc.

TRANSLATOR_DIR  := build/cforall
TRANSLATOR_BIN  := $(TRANSLATOR_DIR)/driver/cfa-cpp
TRANSLATOR_JOBS ?= 8

CFORALL_SRC := $(CURDIR)/cforall
CFA_PARSER  := cforall/src/Parser

.PHONY: translator
translator: $(CFA_PARSER)/parser.cc $(CFA_PARSER)/parser.hh $(CFA_PARSER)/lex.cc
	@mkdir -p $(TRANSLATOR_DIR)
	@unset MAKEFLAGS MFLAGS MAKEOVERRIDES; flock $(TRANSLATOR_DIR)/.lock sh -c '\
	  set -e; \
	  if [ ! -f $(TRANSLATOR_DIR)/config.status ]; then \
	    echo "configuring cforall in $(TRANSLATOR_DIR)"; \
	    cd $(CFORALL_SRC); \
	    touch aclocal.m4; \
	    touch configure src/config.h.in Makefile.in */Makefile.in */*/Makefile.in tools/prettyprinter/Makefile.in; \
	    touch src/AST/BasicKind.hpp src/Parser/lex.cc src/Parser/parser.cc src/Parser/parser.hh; \
	    cd $(CURDIR)/$(TRANSLATOR_DIR); \
	    $(CFORALL_SRC)/configure --quiet --disable-gprofiler >configure.log 2>&1 \
	      || { cat configure.log >&2; exit 1; }; \
	  fi; \
	  $(MAKE) --no-print-directory -s -C $(CURDIR)/$(TRANSLATOR_DIR)/src -j$(TRANSLATOR_JOBS) ../driver/cfa-cpp'

$(CFA_PARSER)/parser.cc $(CFA_PARSER)/parser.hh &: $(CFA_PARSER)/parser.yy
	cd cforall/src && $(SHELL) ../automake/ylwrap Parser/parser.yy y.tab.c Parser/parser.cc \
	  y.tab.h Parser/parser.hh y.output Parser/parser.output -- bison -y -d -t -v -Wno-yacc
	rm -f $(CFA_PARSER)/parser.output
	touch $(CFA_PARSER)/parser.cc $(CFA_PARSER)/parser.hh

$(CFA_PARSER)/lex.cc: $(CFA_PARSER)/lex.ll
	cd cforall/src && $(SHELL) ../automake/ylwrap Parser/lex.ll lex.yy.c Parser/lex.cc -- flex
