# Read after the configured cforall/src Makefile (in build/cforall/src) by
# `make compile_commands.json`; see TRANSLATOR_COMPILE_COMMANDS in
# translator.mk. Prints a compile_commands.json entry for each source of
# cfa-cpp, the demangler and BasicTypes-gen (which writes AST/BasicKind.hpp),
# one per line and each starting with a comma, using that Makefile's own
# compile command. The bison and flex output stands in for parser.yy and
# lex.ll.

cfa_lsp_srcs := $(sort $(patsubst %.yy,%.cc,$(patsubst %.ll,%.cc,$(filter %.cpp %.yy %.ll, \
  $(___driver_cfa_cpp_SOURCES) $(___driver_demangler_SOURCES) BasicTypes-gen.cpp))))

.PHONY: cfa-lsp-compile-commands
cfa-lsp-compile-commands:
	@:$(foreach f,$(cfa_lsp_srcs),$(info ,{"directory":"$(CURDIR)","file":"$(srcdir)/$(f)","command":"$(CXXCOMPILE) -c $(srcdir)/$(f)"}))
