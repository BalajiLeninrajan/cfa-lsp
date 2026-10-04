#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cfalsp {

// One diagnostic from gcc, cpp or the cfa driver, as printed.
struct RawDiag {
	std::string file;						// as printed; empty for tool-level messages
	int line = 0;							// 1-based; 0 if none
	int col = 0;							// 1-based; 0 if none
	std::string severity;					// "error", "warning" or "note"
	std::string message;					// demangled
	std::vector<std::string> detail;		// condensed continuation lines (cfa resolver dumps)
	std::string option;						// e.g. "-Wunused-variable", without brackets
};

// Parses compiler stderr (gcc with -fdiagnostics-plain-output, the cfa
// driver, cfa-cpp's resolver messages).
std::vector<RawDiag> parseCompilerOutput( const std::string & text );

// `_X1ui_2` -> `u`; anything else unchanged.
std::string demangle( const std::string & name );

// Replaces quoted mangled names ('_X6unusedi_2') in a message.
std::string demangleMessage( const std::string & msg );

// Parses `# N "file" flags` / `#line N "file"`. Returns (N, file).
std::optional<std::pair<int, std::string>> parseLineMarker( const std::string & line );

// For each line of a file with line markers (preprocessor or cfa-cpp
// output), the (file, line) it came from.
class LineMarkerMap {
  public:
	explicit LineMarkerMap( const std::string & text );
	// `line` is 1-based in the marked-up text. nullopt before any marker or
	// for a marker line itself.
	std::optional<std::pair<std::string, int>> lookup( int line ) const;

  private:
	struct Entry { std::string file; int line; };
	std::vector<std::optional<Entry>> lines;	// index 0 = line 1
};

// Where each file was first included, from preprocessor line markers:
// file -> (including file, 1-based line of the #include).
using IncludeSites = std::map<std::string, std::pair<std::string, int>>;
IncludeSites findIncludeSites( const std::string & preprocessed );

// Follows the chain of include sites from `file` up to `mainFile` and
// returns the 1-based line in `mainFile` that (indirectly) includes it.
std::optional<int> includeLineInMain( const IncludeSites & sites, const std::string & file,
									  const std::string & mainFile );

} // namespace cfalsp
