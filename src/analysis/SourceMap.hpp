#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
	// A translator position: `line` is 1-based, `col` a 0-based byte offset
	// into the preprocessed line, and `pline` the 1-based line in the
	// preprocessed text (the dump's optional pline), or 0 if unknown. With
	// pline, the position names one preprocessed line even when cpp split the
	// original line or the file was included twice.
	struct Point {
		int line = 0, col = 0, pline = 0;
	};

	// Returns the current text of a file: the analysed buffer for the main
	// file, disk contents for headers. nullopt if unreadable.
	using Reader = std::function<std::optional<std::string>( const std::string & path )>;

	// `preprocessed` is `cfa -E -dD` output. The translator read its
	// withoutDefinitions(), which has the same lines. Its #define and #undef
	// lines say which macros were defined where. Plain `cfa -E` output works
	// too, but then no macro expansion is redone (see inMacroBody).
	SourceMap( std::string preprocessed, Reader read );

	// `cfa -E -dD` output with each #define and #undef line that cpp printed
	// left empty, for the translator. Every line keeps its number.
	static std::string withoutDefinitions( std::string_view preprocessed );
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
	Loc map( const std::string & file, Point p, bool isEnd = false ) const;

	Range mapRange( const std::string & file, int line, int col, int endLine, int endCol ) const;
	Range mapRange( const std::string & file, Point start, Point end ) const;

	// Whether the token at `p` came from the body of a macro rather than from
	// the source or a macro argument, so the source has no token for it, only
	// the invocation. Known only when SourceMap could redo the expansion from
	// the definitions cpp printed and got what cpp got; false otherwise.
	bool inMacroBody( const std::string & file, Point p ) const;

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
