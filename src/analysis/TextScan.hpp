#pragma once

// Text helpers for Analysis: doc comments, identifiers under the cursor, and
// the small amount of lexical scanning that completion and signature help do
// on text the translator has not seen.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Types.hpp"

namespace cfalsp::text {

bool isIdentStart( char c );
bool isIdentChar( char c );
bool isIdentifier( std::string_view s );

// A file's contents split into lines.
class FileText {
  public:
	explicit FileText( std::string contents );
	int lineCount() const { return int( starts.size() ); }
	// Without the line terminator; empty if out of range.
	std::string_view line( int n ) const;
	// The text in `r`, lines joined with '\n'. Clamped to the file.
	std::string slice( Range r ) const;

  private:
	std::string contents;
	std::vector<size_t> starts;
};

// The comment that documents a declaration starting at `line`: a run of //
// comments, or one /* */ block, ending on the line right above it.
std::string docAbove( const FileText & f, int line );

// A comment after `pos` on its line (`int x; // doc`).
std::string docTrailing( const FileText & f, Loc pos );

// [start, end) of the identifier in `line` that contains `col` or ends at it.
std::optional<std::pair<int, int>> identifierAt( std::string_view line, int col );

// Byte spans of the parameters in a signature such as
// "forall( T ) void push( Stack(T) & s, T x )": the parenthesised list right
// after `name`, or the last top-level list if `name` isn't found.
std::vector<std::pair<int, int>> paramSpans( std::string_view signature, std::string_view name );

// "a.b->c" -> {"a", "b", "c"}; empty unless every part is an identifier.
std::vector<std::string> identChain( std::string_view expr );

// The expressions in a `with( ... )` clause that ends `text`, ignoring
// trailing whitespace and one `{`. Empty if `text` doesn't end with one.
std::vector<std::string> withClauseAtEnd( std::string_view text );

// The innermost unclosed call in `text` (code from the start of a file to the
// cursor).
struct CallSite {
	std::string name;						// identifier, or the run of operator characters before `(`
	size_t nameEnd = 0;						// offset just past `name` in the text
	int argIndex = 0;						// commas seen at the call's own depth
};
std::optional<CallSite> callBefore( std::string_view text );

// The argument list that starts at the first non-blank character at or after
// `from` in `f`, which must be `(`: each argument's range without surrounding
// blanks and comments, and the position just past the closing `)`. nullopt if
// there is no `(` there or the list doesn't close within 64 lines.
struct CallArguments {
	std::vector<Range> args;				// empty ranges for empty arguments (`f( a, )`)
	Loc end;
};
std::optional<CallArguments> callArguments( const FileText & f, Loc from );

// A declaration found in code the translator has not seen (`Rect r;`, `const Circle & c = w;`, a parameter
// `Pair( int ) p`), by the shape of its tokens alone: `a * b;` counts as a declaration of `b`, so callers check
// that `typeName` names a type. Offsets are into the scanned text.
struct TextDeclaration {
	std::string typeName;					// the identifier naming the type ("Pair")
	size_t typeStart = 0, typeEnd = 0;		// the type as written, with qualifiers and `*`s ("const Circle &")
	size_t nameStart = 0, nameEnd = 0;		// the declared name
	int pointers = 0;						// `*`s between the type and the name
};

// Declarations of `name` in `text` whose name starts before offset `end`, nearest first. Only the 32 KB of
// text before `end` are scanned.
std::vector<TextDeclaration> declarationsOf( std::string_view text, std::string_view name,
											 size_t end = std::string_view::npos );

// The identifier at `offset` in `text` (offset inside it or right after it) and the declarations of that name
// before it, the identifier itself included if it is being declared. nullopt if there is no identifier at
// `offset` or it is in a comment or a literal.
struct NameInText {
	std::string name;
	size_t start = 0, end = 0;
	std::vector<TextDeclaration> declarations;	// nearest first
};
std::optional<NameInText> declarationsAt( std::string_view text, size_t offset );

// What completion should offer, worked out from the text before the cursor on
// the cursor's line.
struct CompletionContext {
	enum Kind { None, Ident, Member } kind = None;
	std::string prefix;						// identifier characters right before the cursor
	// Member only: the operand before `.` or `->`.
	std::vector<std::string> chain;			// plain identifier chain, empty if the operand is more complex
	std::string operand;					// operand text without whitespace
	int operandStart = 0, operandEnd = 0;	// [start, end) in the line
};
CompletionContext completionContext( std::string_view lineBefore );

} // namespace cfalsp::text
