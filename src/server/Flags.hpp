#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cfalsp {

// The user's compile flags, split by the stage that needs them, the way the
// cfa driver splits them.
struct FlagSet {
	std::vector<std::string> cpp;			// for `cfa -E`: -D -U -I -isystem -include -std -W... -nodebug ...
	std::vector<std::string> translator;	// for cfa-cpp: -Wall -Werror -w and CFA's own -W<name>
	std::vector<std::string> backend;		// for gcc on the generated C: -W... -w -std -m32/-m64 -f... -O...
	bool debug = true;						// false with -nodebug
	bool nolib = false;
	bool m32 = false;
};

// Default when no flags are configured anywhere.
std::vector<std::string> defaultFlags();

// Parses cfa_flags.txt: blank lines and lines starting with '#' are
// skipped; other lines are split on whitespace, so "-I ../include" works.
std::vector<std::string> parseFlagsFile( const std::string & content );

// Walks up from `dir` and returns the nearest cfa_flags.txt.
std::optional<std::string> findFlagsFile( const std::string & dir );

// Relative paths in -I and similar flags resolve against `baseDir`.
// `-iquote DIR` becomes `-I DIR`: the cfa driver's cc1 wrapper does not know
// -iquote takes an argument and would treat DIR as the input file.
FlagSet classifyFlags( const std::vector<std::string> & flags, const std::string & baseDir );

// True for the warning names cfa-cpp itself understands (-W<name>).
bool isTranslatorWarning( const std::string & name );

} // namespace cfalsp
