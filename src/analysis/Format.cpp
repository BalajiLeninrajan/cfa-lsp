#include "Format.hpp"

#include <algorithm>
#include <vector>

#include "Lexer.hpp"

namespace cfalsp {

namespace {

struct Line {
	size_t begin = 0, end = 0;		// the text without the line terminator
	size_t next = 0;				// start of the next line
};

std::vector<Line> splitLines( std::string_view text ) {
	std::vector<Line> out;
	size_t b = 0;
	for ( ;; ) {
		size_t nl = text.find( '\n', b );
		if ( nl == std::string_view::npos ) {
			out.push_back( { b, text.size(), text.size() } );
			return out;
		}
		size_t e = nl > b && text[nl - 1] == '\r' ? nl - 1 : nl;
		out.push_back( { b, e, nl + 1 } );
		b = nl + 1;
	}
}

int widthOf( std::string_view ws, int tab ) {
	int w = 0;
	for ( char c : ws ) w = c == '\t' ? w + tab - w % tab : w + 1;
	return w;
}

std::string indentation( int width, const FormatOptions & o ) {
	if ( o.insertSpaces ) return std::string( width, ' ' );
	return std::string( width / o.tabSize, '\t' ) + std::string( width % o.tabSize, ' ' );
}

bool isKeyword( const Token & t, std::initializer_list<const char *> words ) {
	if ( t.kind != TokKind::Identifier ) return false;
	for ( const char * w : words ) if ( t.text == w ) return true;
	return false;
}

// An open bracket. Braces (and file scope) also hold the state of the
// statements directly inside them.
struct Open {
	Open( char c = 0 ) : c( c ) {}		// resize needs a default
	char c;							// '{', '(', '[', or 'R' for file scope
	bool caseBody = false;			// after a case label, statements are one level in
	std::string bodies;				// bodies without braces pending, innermost last: 'i' for
									// an if, 'o' for while, for, with, else and do
	char header = 0;				// the '(' of an if ('i') or a while, for or with ('o') header
	char owner = 0;					// a '{' that is a body: the kind of the body
	bool stmt = false;				// a '{' that starts a statement
};

// What the last token (comments and directives aside) did.
enum class Prev {
	Terminator,						// ended a statement or opened or closed a block: ; { } case x:
	HeaderEnd,						// ended an if/while/for/with header, or was else or do
	Other,
};

class Formatter {
  public:
	Formatter( std::string_view text, const FormatOptions & opts, int firstLine, int lastLine )
		: text( text ), opts( opts ), lines( splitLines( text ) ), firstLine( firstLine ) {
		this->opts.tabSize = std::clamp( opts.tabSize, 1, 32 );
		this->lastLine = std::min( lastLine, int( lines.size() ) - 1 );
		LexOptions lo;
		lo.comments = true;
		lo.directives = true;
		toks = lex( text, lo );
	}

	std::string run();

  private:
	std::string_view text;
	FormatOptions opts;
	std::vector<Line> lines;
	int firstLine, lastLine;
	std::vector<Token> toks;

	std::vector<int> first, cover, last;	// per line: first token starting on it, token covering
											// its start, last token touching it (-1: none)
	std::vector<Open> stack{ { 'R' } };
	Prev prev = Prev::Terminator;
	bool bodyStart = false;			// the last token ended a header, or was else or do
	bool caseLabel = false;			// in a case label, before its ':'
	std::string popped;				// the bodies the last token completed, for an else after it
	size_t caseDepth = 0;
	const Token * prevTok = nullptr;

	bool inRange( int l ) const { return l >= firstLine && l <= lastLine; }
	static bool braceLike( const Open & o ) { return o.c == '{' || o.c == 'R'; }
	size_t nearestBrace() const {	// stack[0] is file scope, so this always finds one
		size_t j = stack.size() - 1;
		while ( !braceLike( stack[j] ) ) j -= 1;
		return j;
	}
	// The indentation level of statements directly inside stack[upto].
	int level( size_t upto ) const {
		int lv = 0;
		for ( size_t i = 0; i <= upto; i += 1 ) {
			const Open & o = stack[i];
			if ( o.c == '{' ) lv += 1;
			if ( braceLike( o ) ) lv += ( o.caseBody ? 1 : 0 ) + int( o.bodies.size() );
		}
		return lv;
	}
	void index();
	void process( const Token & t );
	bool canTrim( int l, std::string_view content ) const;
};

void Formatter::index() {
	int n = int( lines.size() );
	first.assign( n, -1 );
	cover.assign( n, -1 );
	last.assign( n, -1 );
	for ( int i = 0; i < int( toks.size() ); i += 1 ) {
		const Token & t = toks[i];
		if ( t.line < 0 || t.line >= n ) continue;
		if ( first[t.line] < 0 ) first[t.line] = i;
		int endLine = std::min( t.endLine, n - 1 );
		if ( endLine > t.line && t.endLine == endLine && t.endCol == 0 ) endLine -= 1;
		for ( int l = t.line; l <= endLine; l += 1 ) {
			if ( l > t.line ) cover[l] = i;
			last[l] = i;
		}
	}
}

void Formatter::process( const Token & t ) {
	if ( t.kind == TokKind::Comment || t.kind == TokKind::Directive ) return;
	const std::string & s = t.text;
	bool punct = t.kind == TokKind::Punct;
	bool atStatement = braceLike( stack.back() ) && prev != Prev::Other;
	bool wasBodyStart = bodyStart;
	bool elseBefore = prevTok && isKeyword( *prevTok, { "else" } ) && prevTok->endLine == t.line;
	bodyStart = false;
	std::string done = std::move( popped );		// what the token before completed
	popped.clear();
	Prev next = Prev::Other;

	if ( punct && s == "{" ) {
		Open o{ '{' };
		o.stmt = atStatement;
		if ( wasBodyStart && !stack.back().bodies.empty() ) {	// the body has braces
			o.owner = stack.back().bodies.back();
			stack.back().bodies.pop_back();
		}
		stack.push_back( o );
		next = Prev::Terminator;
	} else if ( punct && s == "}" ) {
		size_t j = nearestBrace();
		if ( j > 0 ) {
			Open closed = stack[j];
			stack.resize( j );
			if ( closed.owner || closed.stmt ) {
				// The block ends its statement, and so the bodies around it,
				// unless an else follows.
				Open & up = stack[nearestBrace()];
				popped = up.bodies;
				if ( closed.owner ) popped += closed.owner;
				up.bodies.clear();
			}
		}
		next = Prev::Terminator;
	} else if ( punct && ( s == "(" || s == "[" || s == "@[" ) ) {
		Open o{ s == "(" ? '(' : '[' };
		if ( s == "(" && braceLike( stack.back() ) && prevTok && isKeyword( *prevTok, { "if", "while", "for", "with" } ) ) {
			o.header = prevTok->text == "if" ? 'i' : 'o';
		}
		stack.push_back( o );
	} else if ( punct && ( s == ")" || s == "]" ) ) {
		char want = s == ")" ? '(' : '[';
		size_t j = stack.size() - 1;
		while ( j > 0 && stack[j].c != want && !braceLike( stack[j] ) ) j -= 1;
		if ( stack[j].c == want ) {
			char header = stack[j].header;
			stack.resize( j );
			if ( header && braceLike( stack.back() ) ) {
				stack.back().bodies += header;
				bodyStart = true;
				next = Prev::HeaderEnd;
			}
		}
	} else if ( punct && s == ";" ) {
		if ( braceLike( stack.back() ) ) {
			popped = std::move( stack.back().bodies );
			stack.back().bodies.clear();
			next = Prev::Terminator;
		}
	} else if ( punct && s == ":" && caseLabel && stack.size() == caseDepth ) {
		caseLabel = false;
		next = Prev::Terminator;
	} else if ( atStatement && isKeyword( t, { "case", "default" } ) ) {
		stack.back().caseBody = true;
		caseLabel = true;
		caseDepth = stack.size();
	} else if ( braceLike( stack.back() ) && isKeyword( t, { "else", "do" } ) ) {
		// An else belongs to the innermost if the last statement completed
		// that has no else yet; the bodies around that if are still open.
		size_t k = s == "else" ? done.rfind( 'i' ) : std::string::npos;
		if ( k != std::string::npos ) stack.back().bodies = done.substr( 0, k );
		stack.back().bodies += 'o';
		bodyStart = true;
		next = Prev::HeaderEnd;
	} else if ( elseBefore && wasBodyStart && isKeyword( t, { "if" } ) && !stack.back().bodies.empty() ) {
		stack.back().bodies.pop_back();	// else if on one line: the if's body takes the else's place
	}
	prev = next;
	prevTok = &t;
}

// Trailing whitespace can go unless it is inside a token other than a
// comment or directive, or removing it would turn a backslash into a line
// splice.
bool Formatter::canTrim( int l, std::string_view content ) const {
	size_t k = content.find_last_not_of( " \t" );
	if ( k == std::string_view::npos ) return last[l] < 0;
	if ( content[k] == '\\' ) return false;
	if ( last[l] < 0 ) return true;
	const Token & t = toks[last[l]];
	if ( t.endLine > l ) return false;
	if ( t.endCol > int( k + 1 ) ) return t.kind == TokKind::Comment || t.kind == TokKind::Directive;
	return true;
}

std::string Formatter::run() {
	index();
	const int n = int( lines.size() );
	std::vector<int> delta( n, 0 );
	int lastDelta = 0;
	bool anchorInRange = false;
	std::string out;
	out.reserve( text.size() + text.size() / 8 );

	for ( int l = 0; l < n; l += 1 ) {
		const Line & ln = lines[l];
		std::string_view content = text.substr( ln.begin, ln.end - ln.begin );
		size_t lead = std::min( content.find_first_not_of( " \t" ), content.size() );
		int oldWidth = widthOf( content.substr( 0, lead ), opts.tabSize );
		int newWidth = -1;			// -1: leave the indentation alone

		if ( cover[l] >= 0 ) {
			// Inside a block comment that starts its line: move with that line.
			// Not after a line splice, where the whitespace is inside the
			// comment's logical line.
			const Token & c = toks[cover[l]];
			std::string_view above = text.substr( lines[l - 1].begin, lines[l - 1].end - lines[l - 1].begin );
			if ( c.kind == TokKind::Comment && c.text.starts_with( "/*" ) && first[c.line] == cover[l] && inRange( c.line ) &&
				 !above.ends_with( '\\' ) ) {
				newWidth = std::max( 0, oldWidth + delta[c.line] );
			}
		} else if ( first[l] >= 0 && toks[first[l]].kind != TokKind::Directive ) {
			const Token & t = toks[first[l]];
			bool anchor = false;
			int lv = 0;
			if ( t.kind == TokKind::Punct && t.text == "}" && nearestBrace() > 0 ) {
				anchor = true;
				lv = level( nearestBrace() - 1 );
			} else if ( braceLike( stack.back() ) && prev != Prev::Other ) {
				anchor = true;
				const Open & top = stack.back();
				lv = level( stack.size() - 1 );
				if ( top.caseBody && isKeyword( t, { "case", "default" } ) ) lv -= 1;
				if ( t.kind == TokKind::Punct && t.text == "{" && bodyStart && !top.bodies.empty() ) lv -= 1;
				if ( isKeyword( t, { "else" } ) ) {		// at its if, inside the bodies still open around it
					size_t k = popped.rfind( 'i' );
					if ( k != std::string::npos ) lv += int( k );
				}
			}
			if ( anchor ) {
				newWidth = lv * opts.tabSize;
				lastDelta = newWidth - oldWidth;
				anchorInRange = inRange( l );
			} else {
				newWidth = std::max( 0, oldWidth + ( anchorInRange ? lastDelta : 0 ) );
			}
		}
		if ( newWidth >= 0 ) delta[l] = newWidth - oldWidth;

		for ( int i = first[l]; i >= 0 && i < int( toks.size() ) && toks[i].line == l; i += 1 ) process( toks[i] );

		if ( !inRange( l ) ) {
			out.append( text.substr( ln.begin, ln.next - ln.begin ) );
			continue;
		}
		std::string line;
		if ( newWidth >= 0 && lead < content.size() ) line = indentation( newWidth, opts ) + std::string( content.substr( lead ) );
		else line = std::string( content );
		if ( opts.trimTrailingWhitespace && canTrim( l, content ) ) {
			size_t k = line.find_last_not_of( " \t" );
			line.resize( k == std::string::npos ? 0 : k + 1 );
		}
		out += line;
		out.append( text.substr( ln.end, ln.next - ln.end ) );
	}

	if ( lastLine == n - 1 ) {
		if ( opts.trimFinalNewlines ) {
			// Keep one line terminator after the last line.
			for ( ;; ) {
				size_t len = out.size();
				if ( len == 0 || out[len - 1] != '\n' ) break;
				size_t end = len - 1;
				if ( end > 0 && out[end - 1] == '\r' ) end -= 1;
				if ( end == 0 || out[end - 1] != '\n' ) break;
				out.resize( end );
			}
		}
		// Not after a backslash, which the newline would turn into a splice.
		if ( opts.insertFinalNewline && !out.empty() && out.back() != '\n' && out.back() != '\\' ) {
			size_t nl = text.find( '\n' );
			out += nl != std::string_view::npos && nl > 0 && text[nl - 1] == '\r' ? "\r\n" : "\n";
		}
	}
	return out;
}

} // namespace

std::string formatText( std::string_view text, const FormatOptions & opts, int firstLine, int lastLine ) {
	return Formatter( text, opts, firstLine, lastLine ).run();
}

} // namespace cfalsp
