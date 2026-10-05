# Read after the configured cforall/src Makefile (in build/cforall/src) by
# `make compile_commands.json`; see TRANSLATOR_COMPILE_COMMANDS in
# translator.mk. Prints a compile_commands.json entry for each source of
# cfa-cpp, libdemangle, the demangler and BasicTypes-gen (which writes
# AST/BasicKind.hpp), one per line and each starting with a comma, using that
# Makefile's own compile command. The bison and flex output stands in for
# parser.yy and lex.ll.

cfa_lsp_srcs := $(sort $(patsubst %.yy,%.cc,$(patsubst %.ll,%.cc,$(filter %.cpp %.yy %.ll, \
  $(___driver_cfa_cpp_SOURCES) $(libdemangle_a_SOURCES) $(___driver_demangler_SOURCES) \
  BasicTypes-gen.cpp))))

# LSP/module.mk compiles Lsp.cpp without -I$(top_builddir), whose "version"
# file would shadow <version>. Keep this in step with its rule there.
cfa_lsp_command = $(if $(filter LSP/Lsp.cpp,$(1)), \
  $(CXX) $(DEFS) -I. -I$(srcdir) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS), \
  $(CXXCOMPILE)) -c $(srcdir)/$(1)

.PHONY: cfa-lsp-compile-commands
cfa-lsp-compile-commands:
	@:$(foreach f,$(cfa_lsp_srcs),$(info ,{"directory":"$(CURDIR)","file":"$(srcdir)/$(f)","command":"$(strip $(call cfa_lsp_command,$(f)))"}))
