#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Types.hpp"

namespace cfalsp {

// How LSP columns are counted on the wire.
enum class Encoding { Utf8, Utf16 };

// Converts an LSP column on one line (no line terminator) to a byte offset.
// Columns past the end clamp to the line length; a column that falls inside
// a character (e.g. between the halves of a surrogate pair) rounds down to
// the start of that character.
int toByteCol( std::string_view line, int col, Encoding enc );

// The reverse: a byte offset to an LSP column. A byte offset inside a UTF-8
// sequence rounds down to the start of the character. Invalid bytes count as
// one unit each.
int fromByteCol( std::string_view line, int byteCol, Encoding enc );

// A buffer with a line index. Lines end at '\n'; a trailing '\r' is not part
// of the line text, but Loc columns are plain byte offsets from the line
// start, so they can address it.
class Text {
  public:
	Text() : Text( std::string() ) {}
	explicit Text( std::string s );

	const std::string & str() const { return text; }
	int lineCount() const { return (int)starts.size(); }
	std::string_view line( int n ) const;	// "" for lines out of range

	Loc clamp( Loc l ) const;				// into the text, column within its line
	size_t offset( Loc l ) const;			// clamped
	Loc loc( size_t offset ) const;			// clamped

	Loc fromLsp( int line, int character, Encoding enc ) const;
	int toLspCol( Loc l, Encoding enc ) const;	// column of a clamped `l`

	// Replaces [start, end) (clamped, swapped if reversed) with `newText`.
	// Returns the clamped start and end actually used.
	std::pair<Loc, Loc> replace( Loc start, Loc end, std::string_view newText );

	// First non-blank column and end column of a line, for whole-line ranges.
	Range lineRange( int line ) const;

  private:
	void index();
	std::string text;
	std::vector<size_t> starts;				// byte offset of each line start
};

} // namespace cfalsp
