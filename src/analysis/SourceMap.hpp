#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Types.hpp"

namespace cfalsp {

// Maps translator coordinates (see docs/dump-format.md) back to original
// source positions.
//
// The translator reads `cfa -E` output, where line markers keep line numbers
// right but whitespace is collapsed and macros are expanded, so its columns
// are offsets into the preprocessed line. SourceMap re-tokenizes the
// preprocessed line and the original line(s) and aligns the two token
// sequences. Tokens that came from a macro expansion map to the span of the
// macro invocation in the original text.
class SourceMap {
  public:
	// Returns the current text of a file: the analysed buffer for the main
	// file, disk contents for headers. nullopt if unreadable.
	using Reader = std::function<std::optional<std::string>( const std::string & path )>;

	// `preprocessed` is the exact text the translator read.
	SourceMap( std::string preprocessed, Reader read );
	~SourceMap();
	SourceMap( SourceMap && ) noexcept;
	SourceMap & operator=( SourceMap && ) noexcept;

	// A map that changes nothing except converting the 1-based line to
	// 0-based. For tests whose dumps already use original columns.
	static SourceMap identity();

	// `line` is 1-based, `col` a 0-based byte offset into the preprocessed
	// line. `isEnd` says `col` is an exclusive end, so it maps to the end of
	// the preceding token rather than the start of the next one. Positions
	// that can't be mapped clamp to the nearest mappable position on the line.
	Loc map( const std::string & file, int line, int col, bool isEnd = false ) const;

	Range mapRange( const std::string & file, int line, int col, int endLine, int endCol ) const;

	// Every file named in a line marker (empty for identity()).
	std::vector<std::string> files() const;

	// The 0-based lines of `file` that left text in the preprocessed output,
	// sorted. nullopt if no line marker names the file, and for identity().
	std::optional<std::vector<int>> outputLines( const std::string & file ) const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;				// null for identity()
};

} // namespace cfalsp
