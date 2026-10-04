#pragma once

#include <string>
#include <vector>

#include "Flags.hpp"

namespace cfalsp {

// Where the programs the checker runs live.
struct Toolchain {
	std::string cfa;						// the cfa driver; "" if not found
	std::string translator;					// our cfa-cpp with --lsp; "" if not found
	std::string cc = "gcc";					// backend C compiler
	std::string preludeDir;					// override; "" means derive from cfa
	std::string cfaPrefix;					// install prefix of cfa (realpath(cfa)/../..), "" if unknown

	// The prelude directory the driver would use for these flags.
	std::string preludeFor( const FlagSet & flags ) const;

	// True if `path` belongs to the compiler or the system (libcfa headers,
	// the prelude, /usr/...), not to the user's project.
	bool isSystemPath( const std::string & path ) const;
};

struct ToolchainOptions {
	std::string cfa;						// initializationOptions.cfa
	std::string translator;					// initializationOptions.translator
	std::string preludeDir;					// initializationOptions.preludeDir
	std::string cc;							// initializationOptions.cc
	std::string exeDir;						// directory of the running server binary
};

// cfa: option, else PATH. translator: option, else $CFA_LSP_TRANSLATOR,
// else <exeDir>/../libexec/cfa-lsp/cfa-cpp, else the development build
// (build/cforall/driver/cfa-cpp next to or above <exeDir>).
Toolchain discoverToolchain( const ToolchainOptions & opts );

// Directory holding the running executable.
std::string executableDir();

// realpath(cfa) is <prefix>/bin/cfa; the prelude is
// <prefix>/lib/cfa/<arch>-<debug|nodebug|nolib>, as in cforall/driver/cfa.cc.
std::string derivePreludeDir( const std::string & cfaPrefix, bool debug, bool nolib, bool m32 );

} // namespace cfalsp
