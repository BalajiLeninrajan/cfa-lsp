#include "SourceMap.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Lexer.hpp"

// How the alignment works
//
// The preprocessed text is split into lines and the GNU line markers give
// each line its (file, original line). For one file, the preprocessed lines
// are cut into segments where the original line number keeps increasing
// (a header included twice gives two segments). A marker that repeats the
// current line number and switches between tokens from a system header and
// from the file (flag 3) continues that line on a new preprocessed line; see
// SubLines.
//
// Within a segment, cpp prints every token on the line it came from, except
// that a macro expansion is printed on the line of the macro name, even when
// the invocation's arguments continue on later lines. So a preprocessed token
// on line L comes from an original token on line L or later, and the two
// streams can be cut into small groups: a group starts at a preprocessed line
// and takes original lines until the parentheses opened in it are closed.
//
// Each group is aligned with a small edit-distance table whose moves are:
// match equal tokens (free), treat an original identifier (plus its
// parenthesised arguments, if any) as a macro invocation that produced any
// number of preprocessed tokens, or skip a token on either side (dead #if
// code, _Pragma, anything unexplained). Tokens produced by a macro map to the
// invocation, and identifiers copied from its arguments map to the argument.
// To tell the two apart, the expansion is redone from the #defines in effect
// where cpp printed it (see Expander), which also says which tokens came from
// a macro's body. When the redo doesn't give the tokens the table assigned to
// the invocation, it is tried on what follows too and decides itself where
// the invocation ends (see Aligner::macro). When it can't be redone, an
// identifier that the invocation also has is taken to be that occurrence.
//
// The #defines in effect come from cpp itself. The check runs `cfa -E -dD`,
// which prints each #define and #undef cpp obeyed where it read it, the
// predefined ones and those from -D first. So the definition in effect at a
// preprocessed line is the last one printed above it (see Impl::index). The
// translator reads the same text with those lines emptied (see
// SourceMap::withoutDefinitions), so the line numbers agree.
//
// A #line directive in the original makes the markers give line numbers that
// differ from the real ones. The file's own #line directives say how the two
// relate, and the preprocessed lines are moved back to their real lines before
// they are cut into segments (see remapLines).
//
// The translator can also give the line in the preprocessed text (pline),
// which names one preprocessed line exactly: the right piece of a split line
// and the right copy of a header included twice.

namespace cfalsp {

namespace {

constexpr int SkipCost = 12;
constexpr int MacroCost = 4;		// plus one per original token inside the invocation
constexpr int UnbalancedCost = 8;	// a macro expansion with unbalanced parentheses
constexpr size_t MaxGroupTokens = 4000;
constexpr int MaxGroupLines = 400;
constexpr size_t MaxTableCells = 1000000;	// about 21 MB of tables

struct Mapped {
	int col = 0, endCol = 0;		// in the preprocessed line
	Loc start, end;					// original span
	Loc outerStart, outerEnd;		// the whole macro invocation for expanded tokens
	int origTok = -1;				// original token index for exact matches
	bool body = false;				// made by a macro's body, not copied from the source (see Expander)
};

// The tokens of one original line, split by preprocessed line. There is more
// than one when cpp restarts the line with a marker in the middle (it does
// this around tokens that come from a system-header macro, as in assert()).
// Each preprocessed line has its own columns.
using SubLines = std::vector<std::vector<Mapped>>;

struct Segment {
	int first = 0;					// 0-based original line of lines[0]
	std::vector<SubLines> lines;
};

struct PrepLine {
	int line;						// 0-based original line
	int presumed;					// the line the markers give (differs after a #line)
	size_t off, len;				// in the preprocessed text
	bool cont;						// continues the previous entry's original line
	int pline;						// 1-based line in the preprocessed text
	int seg = -1, sub = 0;			// where buildFile put it: segment, and piece of its line
};

// A macro definition that cpp printed, for redoing an expansion.
struct Define {
	bool function = false;
	std::vector<std::string> params;	// __VA_ARGS__ for "..."
	bool variadic = false;			// the last parameter takes the remaining arguments
	std::vector<Token> body;
};

// A #line N ["file"] (or # N "file") in an original file.
struct LineDirective {
	int at;							// 0-based line of the directive
	int number;						// the 1-based line it gives the next line
	std::optional<std::string> file;
};

// From original line `actual` on, line actual + k is presumed line
// presumed + k, for lines up to the next region. `ours` is false while a
// #line names another file.
struct Region {
	int actual, presumed;
	bool ours;
};

struct FileTable {
	std::vector<PrepLine> prep;
	std::once_flag once;
	bool ok = false;
	std::string text;
	std::vector<int> lineStarts;
	std::vector<Token> toks;
	std::vector<Segment> segs;
	std::vector<Region> regions;	// empty without #line directives

	// The 0-based real line of a 0-based presumed line, from the first region
	// that has it.
	int actualLine( int presumed ) const {
		for ( size_t k = 0; k < regions.size(); ++k ) {
			if ( inRegion( k, presumed ) ) return regions[k].actual + ( presumed - regions[k].presumed );
		}
		return presumed;
	}
	bool inRegion( size_t k, int presumed ) const {
		const Region & g = regions[k];
		if ( !g.ours || presumed < g.presumed ) return false;
		long actual = (long)g.actual + ( presumed - g.presumed );
		return k + 1 == regions.size() || actual < regions[k + 1].actual;
	}

	int lineCount() const { return (int)lineStarts.size(); }
	int lineLength( int line ) const {
		int b = lineStarts[line];
		int e = line + 1 < lineCount() ? lineStarts[line + 1] - 1 : (int)text.size();
		while ( e > b && ( text[e - 1] == '\r' || text[e - 1] == '\n' ) ) --e;
		return e - b;
	}
	Loc clamp( Loc l ) const {
		if ( l.line < 0 ) return { 0, 0 };
		if ( l.line >= lineCount() ) return { lineCount() - 1, lineLength( lineCount() - 1 ) };
		return { l.line, std::clamp( l.col, 0, lineLength( l.line ) ) };
	}
};

struct PTok {
	Token tok;						// col/endCol are offsets in the preprocessed line
	int line;						// 0-based original line
	int sub;						// index into the line's SubLines
	int pline;						// 1-based line in the preprocessed text
};

Loc startOf( const Token & t ) { return { t.line, t.col }; }
Loc endOf( const Token & t ) { return { t.endLine, t.endCol }; }

// The definition macro `name` had when cpp printed preprocessed line `pline`,
// or null when it had none.
using DefineLookup = std::function<const Define *( const std::string & name, int pline )>;

// Tokenizes a macro body or a pasted token. A "#" first on the line would
// start a directive, so the text is lexed after a token that is then dropped.
std::vector<Token> lexBody( std::string_view text ) {
	std::vector<Token> ts = lex( "0 " + std::string( text ) );
	if ( !ts.empty() ) ts.erase( ts.begin() );
	return ts;
}

// Builtin macros whose value cpp makes up. In a redone expansion each one
// matches whatever single token cpp printed for it.
bool madeUpByCpp( const std::string & name ) {
	static const char * const names[] = { "__LINE__", "__FILE__", "__BASE_FILE__", "__FILE_NAME__", "__COUNTER__",
										  "__INCLUDE_LEVEL__", "__DATE__", "__TIME__", "__TIMESTAMP__" };
	for ( const char * n : names ) {
		if ( name == n ) return true;
	}
	return false;
}

// Where a token of a redone expansion came from, as original tokens [b, e).
// A token copied from the source (an argument, or the invocation itself) is
// that one token. One that a macro's body made (`body`) has the invocation
// that expanded it, or b = -1 when the source doesn't spell that invocation
// whole; the outermost invocation then stands for it.
struct Origin {
	int b = -1, e = -1;
	bool body = false;
};

// The smallest run of original tokens holding both, or none.
Origin span( const Origin & x, const Origin & y ) {
	if ( x.b < 0 || y.b < 0 ) return {};
	return { std::min( x.b, y.b ), std::max( x.e, y.e ), false };
}

// A macro's ids, sorted: the macros a token may no longer expand (C's rule
// that a macro is not expanded again inside its own expansion).
using HideSet = std::vector<int>;

HideSet unite( const HideSet & x, const HideSet & y ) {
	HideSet out;
	std::set_union( x.begin(), x.end(), y.begin(), y.end(), std::back_inserter( out ) );
	return out;
}

HideSet intersect( const HideSet & x, const HideSet & y ) {
	HideSet out;
	std::set_intersection( x.begin(), x.end(), y.begin(), y.end(), std::back_inserter( out ) );
	return out;
}

// A token while redoing an expansion.
struct XTok {
	std::string text;
	TokKind kind = TokKind::Other;
	bool space = false;				// whitespace before it, which # turns into one space
	bool any = false;				// made up by cpp (__LINE__): matches any one token
	bool mark = false;				// a placemarker: an empty argument next to ##, or an empty __VA_OPT__
	Origin from;
	HideSet hide;
};

// Redoes cpp's expansion of some original tokens, as C11 6.10.3 says (Prosser's
// algorithm, with hide sets), plus __VA_OPT__ and GNU's ", ## __VA_ARGS__".
// Arguments are expanded before they are substituted, except next to # and
// ##, and the result is scanned again together with the tokens after it.
// Every token carries where it came from, and the result has to be exactly
// what cpp printed, so a case the redo gets wrong fails instead of mapping
// tokens to the wrong place.
class Expander {
  public:
	// Takes the definitions in effect where cpp printed P[pb], so pb < P.size().
	Expander( const std::vector<Token> & O, const std::vector<PTok> & P, size_t pb, size_t pe, const DefineLookup & defines )
		: O( O ), P( P ), pb( pb ), pe( pe ), line( P[pb].pline ), defines( defines ) {}

	// Expands O[ob, oe) and compares the result with P[pb, pe). On a match,
	// from[i] is where P[pb + i] came from.
	bool run( size_t ob, size_t oe, std::vector<Origin> & from );

	// Expands the macro use that starts at O[ob], with as many of the tokens
	// before O[oe] as it takes, and compares the result with the start of
	// P[pb, pe). On a match, from is as above (empty when the use gives
	// nothing) and `stop` is the use's end in O. False when the result isn't
	// what cpp printed or O[ob] isn't a macro.
	bool prefix( size_t ob, size_t oe, std::vector<Origin> & from, size_t & stop );

  private:
	static constexpr size_t MaxWork = 200000;		// tokens scanned
	static constexpr size_t MaxTokens = 100000;		// in an expanded argument
	static constexpr int MaxDepth = 100;			// arguments within arguments

	using Args = std::vector<std::vector<XTok>>;
	using Expanded = std::vector<std::optional<std::vector<XTok>>>;

	const std::vector<Token> & O;
	const std::vector<PTok> & P;
	const size_t pb, pe;
	const int line;
	const DefineLookup & defines;
	std::unordered_map<std::string, int> ids;
	size_t work = 0;
	// For prefix(): the use starts at O[first], and the scan stops at the
	// first token after it that comes untouched from the source, at O[stopAt].
	bool prefixMode = false;
	size_t first = 0, stopAt = SIZE_MAX;

	const Define * lookup( const std::string & name ) { return defines( name, line ); }
	int idOf( const std::string & name );
	bool source( size_t ob, size_t oe, std::vector<XTok> & out );
	bool emit( XTok t, std::vector<XTok> & out, bool top );
	bool expand( std::vector<XTok> ts, std::vector<XTok> & out, bool top, int depth );
	bool subst( const Define & d, const Args & args, bool omitted, const HideSet & hs, const Origin & inv,
				std::vector<XTok> & os, int depth );
	bool replace( const Define & d, const Args & args, bool omitted, const Origin & inv, size_t bb, size_t be,
				  Expanded & expanded, std::vector<XTok> & os, int depth );
};

int Expander::idOf( const std::string & name ) {
	return ids.try_emplace( name, (int)ids.size() ).first->second;
}

// Expands O[ob, oe) into `out`, which cpp's output has to start with.
bool Expander::source( size_t ob, size_t oe, std::vector<XTok> & out ) {
	std::vector<XTok> in;
	for ( size_t o = ob; o < oe; ++o ) {
		XTok t;
		t.text = O[o].text;
		t.kind = O[o].kind;
		t.space = o > ob && O[o - 1].endOffset < O[o].offset;
		t.from = { (int)o, (int)o + 1, false };
		in.push_back( std::move( t ) );
	}
	work = 0;
	stopAt = SIZE_MAX;
	return expand( std::move( in ), out, true, 0 );
}

std::vector<Origin> origins( const std::vector<XTok> & out ) {
	std::vector<Origin> from;
	for ( const XTok & t : out ) from.push_back( t.from );
	return from;
}

bool Expander::run( size_t ob, size_t oe, std::vector<Origin> & from ) {
	prefixMode = false;
	std::vector<XTok> out;
	if ( !source( ob, oe, out ) || out.size() != pe - pb ) return false;
	from = origins( out );
	return true;
}

bool Expander::prefix( size_t ob, size_t oe, std::vector<Origin> & from, size_t & stop ) {
	prefixMode = true;
	first = ob;
	std::vector<XTok> out;
	if ( !source( ob, oe, out ) ) return false;
	from = origins( out );
	stop = std::min( stopAt, oe );
	return true;
}

// Appends a finished token. At the top, it has to be the next one cpp printed.
bool Expander::emit( XTok t, std::vector<XTok> & out, bool top ) {
	if ( top ) {
		size_t p = pb + out.size();
		if ( p >= pe || ( !t.any && P[p].tok.text != t.text ) ) return false;
	} else if ( out.size() >= MaxTokens ) {
		return false;
	}
	out.push_back( std::move( t ) );
	return true;
}

bool Expander::expand( std::vector<XTok> ts, std::vector<XTok> & out, bool top, int depth ) {
	if ( depth > MaxDepth ) return false;
	std::reverse( ts.begin(), ts.end() );			// a stack: the next token is at the back
	auto isPunct = []( const XTok & t, const char * p ) { return t.kind == TokKind::Punct && t.text == p; };
	// A token as the source has it, not produced or copied by an expansion.
	auto untouched = []( const XTok & t ) { return !t.from.body && t.hide.empty() && t.from.b >= 0 && t.from.e == t.from.b + 1; };
	while ( !ts.empty() ) {
		if ( ++work > MaxWork ) return false;
		XTok t = std::move( ts.back() );
		ts.pop_back();
		if ( top && prefixMode && untouched( t ) && (size_t)t.from.b > first ) {
			stopAt = t.from.b;						// the use is over
			return true;
		}
		if ( t.kind != TokKind::Identifier || t.any ) {
			if ( !emit( std::move( t ), out, top ) ) return false;
			continue;
		}
		const int id = idOf( t.text );
		const Define * d = std::binary_search( t.hide.begin(), t.hide.end(), id ) ? nullptr : lookup( t.text );
		if ( !d && madeUpByCpp( t.text ) && !std::binary_search( t.hide.begin(), t.hide.end(), id ) ) {
			t.any = true;
			t.from.body = true;
		}
		if ( !d || ( d->function && ( ts.empty() || !isPunct( ts.back(), "(" ) ) ) ) {
			if ( top && prefixMode && untouched( t ) ) return false;	// the name isn't a macro use
			if ( !emit( std::move( t ), out, top ) ) return false;
			continue;
		}

		HideSet hs = t.hide;
		Origin inv = t.from;
		Args args;
		bool omitted = false;						// no variable arguments at all
		if ( d->function ) {
			ts.pop_back();							// (
			const size_t n = d->params.size();
			args.emplace_back();
			int nest = 0;
			bool closed = false;
			while ( !ts.empty() ) {
				XTok a = std::move( ts.back() );
				ts.pop_back();
				if ( isPunct( a, "(" ) ) {
					++nest;
				} else if ( isPunct( a, ")" ) ) {
					if ( nest == 0 ) {
						hs = intersect( t.hide, a.hide );
						inv = span( t.from, a.from );
						closed = true;
						break;
					}
					--nest;
				} else if ( isPunct( a, "," ) && nest == 0 && !( d->variadic && args.size() == n ) ) {
					args.emplace_back();
					continue;
				}
				args.back().push_back( std::move( a ) );
			}
			if ( !closed ) return false;			// cpp: unterminated argument list
			if ( n == 0 ) {
				if ( args.size() != 1 || !args[0].empty() ) return false;
				args.clear();
			} else if ( args.size() + 1 == n && d->variadic ) {
				args.emplace_back();
				omitted = true;
			} else if ( args.size() != n ) {
				return false;
			}
			// GNU mode counts one empty argument as none when the variable
			// arguments are the only parameter.
			if ( d->variadic && n == 1 && args[0].empty() ) omitted = true;
		}
		hs = unite( hs, { id } );
		inv.body = true;
		if ( inv.b < 0 ) inv.e = -1;
		std::vector<XTok> os;
		if ( !subst( *d, args, omitted, hs, inv, os, depth ) ) return false;
		if ( !os.empty() ) os.front().space = t.space;
		for ( size_t k = os.size(); k-- > 0; ) ts.push_back( std::move( os[k] ) );
	}
	return true;
}

// The string literal # makes of an argument: its spelling with each run of
// whitespace between tokens as one space, and " and \ escaped in literals.
XTok stringize( const std::vector<XTok> & arg, const Origin & inv ) {
	XTok s;
	s.kind = TokKind::String;
	s.from = inv;
	s.text = "\"";
	bool first = true;
	for ( const XTok & a : arg ) {
		if ( a.mark ) continue;
		if ( a.any ) s.any = true;
		if ( !first && a.space ) s.text += ' ';
		first = false;
		bool literal = a.kind == TokKind::String || a.kind == TokKind::Char;
		for ( char c : a.text ) {
			if ( literal && ( c == '"' || c == '\\' ) ) s.text += '\\';
			s.text += c;
		}
	}
	s.text += '"';
	return s;
}

// a ## b. False when the two don't make one token, which cpp rejects.
bool paste( XTok & lhs, const XTok & rhs, const Origin & inv ) {
	if ( rhs.mark ) return true;
	if ( lhs.mark ) {
		bool space = lhs.space;
		lhs = rhs;
		lhs.space = space;
		return true;
	}
	std::string text = lhs.text + rhs.text;
	std::vector<Token> ts = lexBody( text );
	if ( ts.size() != 1 || ts[0].text != text ) return false;
	lhs.text = std::move( text );
	lhs.kind = ts[0].kind;
	lhs.any = false;
	lhs.from = inv;
	lhs.hide = intersect( lhs.hide, rhs.hide );
	return true;
}

// The body of `d` with its parameters replaced, as `os`. `omitted`: the use
// gave no variable arguments.
bool Expander::subst( const Define & d, const Args & args, bool omitted, const HideSet & hs, const Origin & inv,
					  std::vector<XTok> & os, int depth ) {
	Expanded expanded( d.params.size() );
	if ( !replace( d, args, omitted, inv, 0, d.body.size(), expanded, os, depth ) ) return false;
	size_t kept = 0;
	for ( size_t i = 0; i < os.size(); ++i ) {
		if ( os[i].mark ) continue;
		os[i].hide = unite( os[i].hide, hs );
		if ( kept != i ) os[kept] = std::move( os[i] );
		++kept;
	}
	os.resize( kept );
	return true;
}

// Body tokens [bb, be) of `d` with the parameters replaced, appended to `os`,
// placemarkers included. `expanded` holds each argument once it is expanded.
bool Expander::replace( const Define & d, const Args & args, bool omitted, const Origin & inv, size_t bb, size_t be,
						Expanded & expanded, std::vector<XTok> & os, int depth ) {
	const std::vector<Token> & B = d.body;
	const size_t n = d.params.size();
	auto isPunct = [&]( size_t i, const char * p ) { return i < be && B[i].kind == TokKind::Punct && B[i].text == p; };
	auto param = [&]( size_t i ) -> size_t {
		if ( !d.function || i >= be || B[i].kind != TokKind::Identifier ) return n;
		return std::find( d.params.begin(), d.params.end(), B[i].text ) - d.params.begin();
	};
	auto isVaOpt = [&]( size_t i ) {
		return d.variadic && i < be && B[i].kind == TokKind::Identifier && B[i].text == "__VA_OPT__" && isPunct( i + 1, "(" );
	};
	auto spaced = [&]( size_t i ) { return i > 0 && B[i - 1].endOffset < B[i].offset; };
	auto fromBody = [&]( size_t i ) {
		XTok t;
		t.text = B[i].text;
		t.kind = B[i].kind;
		t.space = spaced( i );
		t.from = inv;
		return t;
	};
	auto mark = [&] {
		XTok m;
		m.mark = true;
		m.from = inv;
		return m;
	};
	// An argument's first token is spaced as the parameter was.
	auto append = [&]( const std::vector<XTok> & a, size_t i ) {
		size_t at = os.size();
		os.insert( os.end(), a.begin(), a.end() );
		if ( at < os.size() ) os[at].space = spaced( i );
	};
	auto expandedArg = [&]( size_t k ) -> const std::vector<XTok> * {
		if ( !expanded[k] ) {
			std::vector<XTok> e;
			if ( !expand( args[k], e, false, depth + 1 ) ) return nullptr;
			expanded[k] = std::move( e );
		}
		return &*expanded[k];
	};
	// __VA_OPT__( ... ) at B[i]: its tokens when there are variable
	// arguments, else a placemarker, as `opt`; `close` gets its ).
	auto vaOpt = [&]( size_t i, size_t & close, std::vector<XTok> & opt ) {
		int nest = 0;
		for ( close = i + 2; close < be; ++close ) {
			if ( isPunct( close, "(" ) ) ++nest;
			else if ( isPunct( close, ")" ) && nest-- == 0 ) break;
		}
		if ( close == be ) return false;
		const size_t k = n - 1;
		if ( !omitted && !args[k].empty() ) {
			// gcc 16 asks whether the expanded argument is empty, and not
			// every version does. Where that and the argument as written
			// differ, the redo doesn't guess.
			const std::vector<XTok> * e = expandedArg( k );
			if ( !e || e->empty() ) return false;
			if ( !replace( d, args, omitted, inv, i + 2, close, expanded, opt, depth ) ) return false;
			if ( !opt.empty() ) opt.front().space = spaced( i );
		}
		if ( opt.empty() ) opt.push_back( mark() );
		return true;
	};
	for ( size_t i = bb; i < be; ++i ) {
		if ( d.function && isPunct( i, "#" ) ) {
			size_t k = param( i + 1 );
			if ( k == n ) return false;				// cpp rejects such a #define, and # __VA_OPT__ isn't redone
			os.push_back( stringize( args[k], inv ) );
			os.back().space = spaced( i );
			++i;
			continue;
		}
		if ( isPunct( i, "##" ) ) {
			if ( os.empty() || i + 1 == be ) return false;
			if ( isVaOpt( ++i ) ) {
				std::vector<XTok> opt;
				size_t close;
				if ( !vaOpt( i, close, opt ) || !paste( os.back(), opt.front(), inv ) ) return false;
				os.insert( os.end(), opt.begin() + 1, opt.end() );
				i = close;
				continue;
			}
			size_t k = param( i );
			if ( k == n ) {
				if ( !paste( os.back(), fromBody( i ), inv ) ) return false;
			} else if ( d.variadic && k + 1 == n && !os.back().mark && os.back().kind == TokKind::Punct && os.back().text == "," ) {
				// GNU: the comma goes when the variable arguments are left out,
				// and stays when they are there but empty.
				if ( omitted ) os.pop_back();
				else append( args[k], i );
			} else if ( !args[k].empty() ) {
				if ( !paste( os.back(), args[k].front(), inv ) ) return false;
				os.insert( os.end(), args[k].begin() + 1, args[k].end() );
			}
			continue;
		}
		if ( isVaOpt( i ) ) {
			std::vector<XTok> opt;
			size_t close;
			if ( !vaOpt( i, close, opt ) ) return false;
			os.insert( os.end(), opt.begin(), opt.end() );
			i = close;
			continue;
		}
		size_t k = param( i );
		if ( k == n ) {
			os.push_back( fromBody( i ) );
		} else if ( isPunct( i + 1, "##" ) ) {
			if ( args[k].empty() ) os.push_back( mark() );
			else append( args[k], i );
		} else {
			const std::vector<XTok> * e = expandedArg( k );
			if ( !e ) return false;
			append( *e, i );
		}
	}
	return true;
}

class Aligner {
  public:
	Aligner( const FileTable & ft, const std::vector<PTok> & P, std::vector<Mapped> & res, const DefineLookup & defines )
		: ft( ft ), O( ft.toks ), P( P ), res( res ), defines( defines ) {}

	void group( size_t pb, size_t pe, size_t ob, size_t oe );

  private:
	const FileTable & ft;
	const std::vector<Token> & O;
	const std::vector<PTok> & P;
	std::vector<Mapped> & res;
	const DefineLookup & defines;

	// Aligns P[pb, pe) with O[ob, oe). With `commit`, stops at the first exact match at or after O[commit]
	// and returns where it stopped; otherwise returns { pe, oe }.
	std::pair<size_t, size_t> table( size_t pb, size_t pe, size_t ob, size_t oe, size_t commit = SIZE_MAX );
	void alignLong( size_t pb, size_t pe, size_t ob, size_t oe, size_t gb, size_t ge );
	void exact( size_t p, size_t o );
	std::pair<size_t, size_t> macro( size_t pb, size_t pe, size_t ob, size_t oe, size_t pmax, size_t omax );
	void redone( size_t pb, size_t pe, size_t ob, size_t oe, const std::vector<Origin> & from );
	void guess( size_t pb, size_t pe, size_t ob, size_t oe );
	void unmatched( size_t p, size_t o, size_t ob, size_t oe );
};

void Aligner::exact( size_t p, size_t o ) {
	Mapped & m = res[p];
	m.start = m.outerStart = startOf( O[o] );
	m.end = m.outerEnd = endOf( O[o] );
	m.origTok = (int)o;
}

// The macro use O[ob, oe) that the alignment says printed P[pb, pe). Its
// expansion is redone from the #defines in effect, so the tokens copied from
// the arguments can be told from those a macro's body made. The alignment can
// cut a use in the wrong place: two uses side by side, or a body that ends in
// a function-like macro name, which takes the ( ... ) after the use. So when
// the redo doesn't give exactly P[pb, pe), it is tried again with the tokens
// before O[omax] and P[pmax], and its expansion says where the use ends.
// Returns where the use ends in P and O.
std::pair<size_t, size_t> Aligner::macro( size_t pb, size_t pe, size_t ob, size_t oe, size_t pmax, size_t omax ) {
	std::vector<Origin> from;
	if ( !defines || O[ob].kind != TokKind::Identifier ) {
		guess( pb, pe, ob, oe );
		return { pe, oe };
	}
	if ( pb < pe && Expander( O, P, pb, pe, defines ).run( ob, oe, from ) ) {
		redone( pb, pe, ob, oe, from );
		return { pe, oe };
	}
	constexpr size_t MaxFollow = 64;				// tokens after O[oe] the use may take
	size_t stop = oe;
	if ( pb < pmax ) {
		if ( Expander( O, P, pb, pmax, defines ).prefix( ob, std::min( omax, oe + MaxFollow ), from, stop ) ) {
			redone( pb, pb + from.size(), ob, stop, from );
			return { pb + from.size(), stop };
		}
	}
	guess( pb, pe, ob, oe );
	return { pe, oe };
}

// P[pb, pe) from a redone expansion of O[ob, oe); from[i] is where P[pb + i] came from.
void Aligner::redone( size_t pb, size_t pe, size_t ob, size_t oe, const std::vector<Origin> & from ) {
	Loc invStart = startOf( O[ob] ), invEnd = endOf( O[oe - 1] );
	for ( size_t p = pb; p < pe; ++p ) {
		Mapped & m = res[p];
		m.start = m.outerStart = invStart;
		m.end = m.outerEnd = invEnd;
		const Origin & f = from[p - pb];
		// A macro whose body has its own name (glibc's #define stdout stdout,
		// or tolower( c ), whose body calls the function tolower) leaves the
		// name the source spells at the start of the use.
		const bool own = f.b >= 0 && O[f.b].text == P[p].tok.text;
		m.body = f.body && !own;
		// An identifier maps to the argument it was copied from, or for
		// one from a body, to the invocation in an argument that made it.
		if ( P[p].tok.kind == TokKind::Identifier && f.b >= 0 ) {
			m.start = startOf( O[f.b] );
			m.end = endOf( O[own ? f.b : f.e - 1] );
		}
	}
}

// Without a redo, an identifier that the invocation also has is taken to be
// that occurrence, trying them in order.
void Aligner::guess( size_t pb, size_t pe, size_t ob, size_t oe ) {
	Loc invStart = startOf( O[ob] ), invEnd = endOf( O[oe - 1] );
	size_t cursor = ob;
	for ( size_t p = pb; p < pe; ++p ) {
		Mapped & m = res[p];
		m.start = m.outerStart = invStart;
		m.end = m.outerEnd = invEnd;
		if ( P[p].tok.kind != TokKind::Identifier ) continue;
		for ( size_t n = 0; n < oe - ob; ++n ) {
			size_t o = ob + ( cursor - ob + n ) % ( oe - ob );
			if ( O[o].kind == TokKind::Identifier && O[o].text == P[p].tok.text ) {
				m.start = startOf( O[o] );
				m.end = endOf( O[o] );
				cursor = o + 1 < oe ? o + 1 : ob;
				break;
			}
		}
	}
}

// A preprocessed token with no original counterpart gets an empty span at the
// nearest original token.
void Aligner::unmatched( size_t p, size_t o, size_t ob, size_t oe ) {
	Loc at;
	if ( o < oe && O[o].line == P[p].line ) at = startOf( O[o] );
	else if ( o > ob ) at = endOf( O[o - 1] );
	else if ( o < oe ) at = startOf( O[o] );
	else at = ft.clamp( { P[p].line, P[p].tok.col } );
	Mapped & m = res[p];
	m.start = m.end = m.outerStart = m.outerEnd = at;
}

void Aligner::group( size_t pb, size_t pe, size_t ob, size_t oe ) {
	if ( pb == pe ) return;
	if ( ( pe - pb + 1 ) * ( oe - ob + 1 ) <= MaxTableCells ) {
		table( pb, pe, ob, oe );
		return;
	}
	// Too big for one table: align line by line.
	size_t o = ob;
	for ( size_t p = pb; p < pe; ) {
		int line = P[p].line;
		size_t q = p;
		while ( q < pe && P[q].line == line ) ++q;
		while ( o < oe && O[o].line < line ) ++o;
		size_t oq = o;
		while ( oq < oe && O[oq].line == line ) ++oq;
		alignLong( p, q, o, oq, ob, oe );
		p = q;
		o = oq;
	}
}

// One line that is too big for a table. Equal tokens at the start match
// (most of a long line is the same before and after cpp); the rest is aligned
// in windows of the original tokens, keeping the first half of each window's
// alignment, where the window's end can't distort it.
void Aligner::alignLong( size_t pb, size_t pe, size_t ob, size_t oe, size_t gb, size_t ge ) {
	while ( pb < pe && ob < oe && P[pb].tok.text == O[ob].text ) exact( pb++, ob++ );
	constexpr size_t Window = 256;
	while ( pb < pe ) {
		if ( ob == oe ) {
			for ( ; pb < pe; ++pb ) unmatched( pb, ob, gb, ge );
			return;
		}
		if ( ( pe - pb + 1 ) * ( oe - ob + 1 ) <= MaxTableCells ) {
			table( pb, pe, ob, oe );
			return;
		}
		size_t wo = std::min( oe - ob, Window );
		size_t wp = std::min( pe - pb, MaxTableCells / ( wo + 1 ) - 1 );
		auto [p, o] = table( pb, pb + wp, ob, ob + wo, ob + std::max<size_t>( wo / 2, 1 ) );
		pb = p;
		ob = o;
	}
}

// For each "(" among n tokens, the index of its matching ")", or -1.
template<typename Get>
std::vector<int> matchParens( size_t n, Get get ) {
	std::vector<int> match( n, -1 ), stack;
	for ( size_t k = 0; k < n; ++k ) {
		const Token & t = get( k );
		if ( t.kind != TokKind::Punct ) continue;
		if ( t.text == "(" ) stack.push_back( (int)k );
		else if ( t.text == ")" && !stack.empty() ) { match[stack.back()] = (int)k; stack.pop_back(); }
	}
	return match;
}

std::pair<size_t, size_t> Aligner::table( size_t pb, size_t pe, size_t ob, size_t oe, size_t commit ) {
	enum : uint8_t { End, Match, MacroName, MacroCall, SkipP, SkipO };
	const size_t nP = pe - pb, nO = oe - ob, W = nO + 1;

	// close[j]: for an original token followed by "(", the index of the
	// matching ")", else -1.
	std::vector<int> close( nO, -1 );
	{
		std::vector<int> m = matchParens( nO, [&]( size_t k ) -> const Token & { return O[ob + k]; } );
		for ( size_t j = 0; j + 1 < nO; ++j ) close[j] = m[j + 1];
	}
	// sib[i]: the next position after P[i] at the same paren depth, or -1 if
	// P[i] is a ")" (or an unmatched "("). Following sib from i visits every
	// i' such that P[i, i') has balanced parentheses.
	std::vector<int> sib( nP, -1 );
	{
		std::vector<int> m = matchParens( nP, [&]( size_t k ) -> const Token & { return P[pb + k].tok; } );
		for ( size_t i = 0; i < nP; ++i ) {
			const Token & t = P[pb + i].tok;
			if ( t.kind == TokKind::Punct && t.text == "(" ) sib[i] = m[i] >= 0 ? m[i] + 1 : -1;
			else if ( !( t.kind == TokKind::Punct && t.text == ")" ) ) sib[i] = (int)i + 1;
		}
	}

	// dp(i, j): cost of aligning P[i..] with O[j..].
	// any(i, j): min over i' >= i of dp(i', j); bal(i, j): the same over the
	// i' that leave P[i, i') balanced. A macro at O[j, e) costs
	// min( bal(i, e), any(i, e) + UnbalancedCost ). Ties pick the smallest i'.
	const size_t N = ( nP + 1 ) * W;
	std::vector<int> dp( N ), any( N ), anyAt( N ), bal( N ), balAt( N );
	std::vector<uint8_t> move( N );
	auto at = [W]( size_t i, size_t j ) { return i * W + j; };
	auto expansion = [&]( size_t i, size_t e, size_t & to ) {
		int b = bal[at( i, e )], u = any[at( i, e )] + UnbalancedCost;
		to = b <= u ? balAt[at( i, e )] : anyAt[at( i, e )];
		return std::min( b, u );
	};

	for ( size_t i = nP + 1; i-- > 0; ) {
		for ( size_t j = nO + 1; j-- > 0; ) {
			int cost = INT_MAX / 2;
			uint8_t mv = End;
			size_t to;
			if ( i == nP && j == nO ) cost = 0;
			if ( i < nP && j < nO && P[pb + i].tok.text == O[ob + j].text && dp[at( i + 1, j + 1 )] < cost ) {
				cost = dp[at( i + 1, j + 1 )];
				mv = Match;
			}
			if ( j < nO && O[ob + j].kind == TokKind::Identifier ) {
				int v = MacroCost + expansion( i, j + 1, to );
				if ( v < cost ) { cost = v; mv = MacroName; }
				if ( close[j] >= 0 ) {
					size_t e = close[j] + 1;
					v = MacroCost + (int)( e - j - 1 ) + expansion( i, e, to );
					if ( v < cost ) { cost = v; mv = MacroCall; }
				}
			}
			if ( i < nP && SkipCost + dp[at( i + 1, j )] < cost ) {
				cost = SkipCost + dp[at( i + 1, j )];
				mv = SkipP;
			}
			if ( j < nO && SkipCost + dp[at( i, j + 1 )] < cost ) {
				cost = SkipCost + dp[at( i, j + 1 )];
				mv = SkipO;
			}
			size_t c = at( i, j );
			dp[c] = cost;
			move[c] = mv;
			any[c] = cost, anyAt[c] = (int)i;
			if ( i < nP && any[at( i + 1, j )] < cost ) any[c] = any[at( i + 1, j )], anyAt[c] = anyAt[at( i + 1, j )];
			bal[c] = cost, balAt[c] = (int)i;
			if ( i < nP && sib[i] >= 0 && bal[at( sib[i], j )] < cost ) bal[c] = bal[at( sib[i], j )], balAt[c] = balAt[at( sib[i], j )];
		}
	}

	size_t i = 0, j = 0;
	while ( i < nP || j < nO ) {
		if ( ob + j >= commit && move[at( i, j )] == Match ) return { pb + i, ob + j };
		switch ( move[at( i, j )] ) {
		  case Match:
			exact( pb + i, ob + j );
			++i, ++j;
			break;
		  case MacroName:
		  case MacroCall: {
			size_t e = move[at( i, j )] == MacroName ? j + 1 : close[j] + 1, i2;
			expansion( i, e, i2 );
			auto [p, o] = macro( pb + i, pb + i2, ob + j, ob + e, pe, oe );
			i = p - pb, j = o - ob;
			break;
		  }
		  case SkipP:
			unmatched( pb + i, ob + j, ob, oe );
			++i;
			break;
		  case SkipO:
			++j;
			break;
		  default:
			return { pe, oe };	// unreachable: End only at (nP, nO)
		}
	}
	return { pe, oe };
}

void alignSegment( FileTable & ft, std::string_view prep, size_t b, size_t e, const DefineLookup & defines ) {
	Segment seg;
	seg.first = ft.prep[b].line;
	seg.lines.resize( ft.prep[e - 1].line - seg.first + 1 );

	std::vector<PTok> P;
	int sub = 0;
	for ( size_t k = b; k < e; ++k ) {
		PrepLine & pl = ft.prep[k];
		sub = pl.cont ? sub + 1 : 0;
		pl.seg = (int)ft.segs.size();
		pl.sub = sub;
		for ( Token & t : lex( prep.substr( pl.off, pl.len ) ) ) P.push_back( { std::move( t ), pl.line, sub, pl.pline } );
	}
	std::vector<Mapped> res( P.size() );
	for ( size_t p = 0; p < P.size(); ++p ) {
		res[p].col = P[p].tok.col;
		res[p].endCol = P[p].tok.endCol;
	}

	const std::vector<Token> & O = ft.toks;
	Aligner al( ft, P, res, defines );
	size_t j = std::lower_bound( O.begin(), O.end(), seg.first,
								 []( const Token & t, int line ) { return t.line < line; } ) - O.begin();
	for ( size_t i = 0; i < P.size(); ) {
		int first = P[i].line, last = first;
		while ( j < O.size() && O[j].line < first ) ++j;
		// Take original lines until the parentheses opened in them close, and
		// past a macro name that ends a line when "(" starts the next one.
		size_t k = j;
		int depth = 0;
		for ( ;; ) {
			while ( k < O.size() && O[k].line <= last && k - j < MaxGroupTokens ) {
				if ( O[k].kind == TokKind::Punct ) {
					if ( O[k].text == "(" ) ++depth;
					else if ( O[k].text == ")" && depth > 0 ) --depth;
				}
				last = std::max( last, O[k].endLine );
				++k;
			}
			if ( k >= O.size() || k - j >= MaxGroupTokens || last - first >= MaxGroupLines ) break;
			if ( depth > 0 || ( k > j && O[k - 1].kind == TokKind::Identifier && O[k].text == "(" ) ) {
				last = O[k].line;
				continue;
			}
			break;
		}
		size_t pe = i;
		while ( pe < P.size() && P[pe].line <= last ) ++pe;
		al.group( i, pe, j, k );
		i = pe;
		j = k;
	}

	for ( size_t p = 0; p < P.size(); ++p ) {
		SubLines & sl = seg.lines[P[p].line - seg.first];
		if ( (int)sl.size() <= P[p].sub ) sl.resize( P[p].sub + 1 );
		sl[P[p].sub].push_back( res[p] );
	}
	ft.segs.push_back( std::move( seg ) );
}

bool identStart( char c ) { return std::isalpha( (unsigned char)c ) || c == '_' || c == '$'; }
bool identChar( char c ) { return std::isalnum( (unsigned char)c ) || c == '_' || c == '$'; }
bool digit( char c ) { return c >= '0' && c <= '9'; }

// The #line directives of a file. The lexer finds them, so one inside a
// comment is skipped. Conditionals are not evaluated.
std::vector<LineDirective> scanLineDirectives( std::string_view text ) {
	std::vector<LineDirective> out;
	LexOptions opts;
	opts.directives = true;
	for ( const Token & t : lex( text, opts ) ) {
		if ( t.kind != TokKind::Directive ) continue;
		const std::string & l = t.text;
		size_t i = l.find( '#' ) + 1;
		auto blanks = [&] { while ( i < l.size() && ( l[i] == ' ' || l[i] == '\t' ) ) ++i; };
		blanks();
		if ( l.compare( i, 4, "line" ) == 0 && ( i + 4 == l.size() || !identChar( l[i + 4] ) ) ) {
			i += 4;
			blanks();
		}
		if ( i >= l.size() || !digit( l[i] ) ) continue;
		long n = 0;
		for ( ; i < l.size() && digit( l[i] ); ++i ) n = std::min( n * 10 + ( l[i] - '0' ), (long)INT_MAX );
		blanks();
		LineDirective d{ t.line, (int)n, std::nullopt };
		if ( i < l.size() && l[i] == '"' ) {
			std::string f;
			for ( ++i; i < l.size() && l[i] != '"'; ++i ) {
				if ( l[i] == '\\' && i + 1 < l.size() ) ++i;
				f.push_back( l[i] );
			}
			d.file = std::move( f );
		}
		out.push_back( std::move( d ) );
	}
	return out;
}

// Whether a line of `cfa -E -dD` output is a #define or #undef that cpp
// printed. cpp writes them in this one form, at the start of a line.
bool definitionLine( std::string_view l ) {
	return l.starts_with( "#define " ) || l.starts_with( "#undef " );
}

// Reads a definitionLine(): the macro's name, and its definition, or none for
// an #undef. A #define that doesn't parse also gives none, so the macro's uses
// aren't redone.
void parseDefinition( std::string_view l, std::string & name, std::optional<Define> & def ) {
	def.reset();
	const bool undef = l.starts_with( "#undef " );
	size_t i = undef ? 7 : 8;
	auto blanks = [&] { while ( i < l.size() && ( l[i] == ' ' || l[i] == '\t' ) ) ++i; };
	blanks();
	size_t nb = i;
	while ( i < l.size() && identChar( l[i] ) ) ++i;
	name = l.substr( nb, i - nb );
	if ( undef || name.empty() || !identStart( name[0] ) ) return;
	Define d;
	if ( i < l.size() && l[i] == '(' ) {
		d.function = true;
		++i;
		for ( ;; ) {
			blanks();
			if ( i >= l.size() ) return;
			if ( l[i] == ')' ) break;
			if ( l.compare( i, 3, "..." ) == 0 ) {
				d.params.push_back( "__VA_ARGS__" );
				d.variadic = true;
				i += 3;
			} else {
				size_t pb = i;
				while ( i < l.size() && identChar( l[i] ) ) ++i;
				if ( i == pb ) return;
				d.params.push_back( std::string( l.substr( pb, i - pb ) ) );
				blanks();
				if ( l.compare( i, 3, "..." ) == 0 ) {
					d.variadic = true;
					i += 3;
				}
			}
			blanks();
			if ( i < l.size() && l[i] == ',' && !d.variadic ) {
				++i;
				continue;
			}
			if ( i < l.size() && l[i] == ')' ) break;
			return;
		}
		++i;											// the )
	}
	d.body = lexBody( l.substr( std::min( i, l.size() ) ) );
	def = std::move( d );
}

// Moves the preprocessed lines back to their real lines when the file has
// #line directives, which make the markers give other numbers.
void remapLines( FileTable & ft, const std::string & name, const std::vector<LineDirective> & dirs ) {
	if ( dirs.empty() ) return;
	ft.regions = { { 0, 0, true } };
	for ( const LineDirective & d : dirs ) {
		// Without a file name, #line keeps the current one.
		ft.regions.push_back( { d.at + 1, d.number - 1, d.file ? *d.file == name : ft.regions.back().ours } );
	}
	// The preprocessed lines come in file order, so a line goes to the first
	// region from the current one on that puts it after the previous line. A
	// line number that two regions share is then taken from the right one.
	// Failing that (a header included again starts over), the first region
	// that has the line number.
	auto actual = [&]( size_t k, int presumed ) { return ft.regions[k].actual + ( presumed - ft.regions[k].presumed ); };
	size_t r = 0;
	int prev = -1;
	for ( PrepLine & pl : ft.prep ) {
		if ( pl.cont ) {
			pl.line = prev >= 0 ? prev : pl.line;
			continue;
		}
		size_t k = r;
		while ( k < ft.regions.size() && !( ft.inRegion( k, pl.presumed ) && actual( k, pl.presumed ) > prev ) ) ++k;
		if ( k == ft.regions.size() ) {
			for ( k = 0; k < ft.regions.size() && !ft.inRegion( k, pl.presumed ); ++k ) {}
		}
		if ( k < ft.regions.size() ) {
			r = k;
			pl.line = actual( k, pl.presumed );
		}
		prev = pl.line;
	}
}

void buildFile( FileTable & ft, const std::string & name, std::string_view prep, const SourceMap::Reader & read,
				const DefineLookup & defines ) {
	std::optional<std::string> text;
	if ( read ) {
		try { text = read( name ); } catch ( ... ) { text.reset(); }
	}
	if ( !text ) return;
	ft.text = std::move( *text );
	ft.lineStarts.push_back( 0 );
	for ( size_t i = 0; i < ft.text.size(); ++i ) {
		if ( ft.text[i] == '\n' ) ft.lineStarts.push_back( (int)i + 1 );
	}
	ft.toks = lex( ft.text );
	remapLines( ft, name, scanLineDirectives( ft.text ) );

	for ( size_t b = 0; b < ft.prep.size(); ) {
		size_t e = b + 1;
		while ( e < ft.prep.size() && ( ft.prep[e].line > ft.prep[e - 1].line || ft.prep[e].cont ) ) ++e;
		alignSegment( ft, prep, b, e, defines );
		b = e;
	}
	ft.ok = true;
}

// Parses `# N "file" flags` or `#line N "file"`. Returns false for any other
// directive (#pragma, #ident, ...). `flags` gets bit k - 1 for each flag k.
bool parseMarker( std::string_view l, int & line, std::optional<std::string> & file, int & flags ) {
	flags = 0;
	size_t p = 1;
	auto spaces = [&] { while ( p < l.size() && ( l[p] == ' ' || l[p] == '\t' ) ) ++p; };
	spaces();
	if ( l.substr( p, 4 ) == "line" ) { p += 4; spaces(); }
	if ( p >= l.size() || l[p] < '0' || l[p] > '9' ) return false;
	long n = 0;
	while ( p < l.size() && l[p] >= '0' && l[p] <= '9' ) {
		n = std::min( n * 10 + ( l[p] - '0' ), (long)INT_MAX );
		++p;
	}
	line = (int)n;
	spaces();
	if ( p < l.size() && l[p] == '"' ) {
		std::string f;
		for ( ++p; p < l.size() && l[p] != '"'; ++p ) {
			if ( l[p] == '\\' && p + 1 < l.size() ) {
				++p;
				if ( l[p] >= '0' && l[p] <= '7' ) {
					int v = 0;
					for ( int d = 0; d < 3 && p < l.size() && l[p] >= '0' && l[p] <= '7'; ++d, ++p ) v = v * 8 + ( l[p] - '0' );
					--p;
					f.push_back( (char)v );
					continue;
				}
			}
			f.push_back( l[p] );
		}
		file = std::move( f );
		for ( ++p; p < l.size(); ++p ) {
			if ( l[p] >= '1' && l[p] <= '4' && ( p + 1 == l.size() || l[p + 1] == ' ' || l[p + 1] == '\t' || l[p + 1] == '\r' ) ) {
				flags |= 1 << ( l[p] - '1' );
			}
		}
	}
	return true;
}

// The translator's column alone can't say which preprocessed line of a split
// original line it meant, so pick the likeliest. A sub-line scores 3 when a
// token starts (or for an end, ends) exactly at col, 2 when col is inside a
// token, 1 when col is within the sub-line's tokens. Ties go to a sub-line
// whose tokens in the range were written in the file rather than produced by
// a macro body, then to the first one.
int boundaryScore( const std::vector<Mapped> & ts, int col, bool isEnd ) {
	int score = 0;
	for ( const Mapped & m : ts ) {
		if ( ( isEnd ? m.endCol : m.col ) == col ) return 3;
		if ( m.col < col && col < m.endCol ) score = 2;
	}
	if ( score == 0 && !ts.empty() && ts.front().col <= col && col <= ts.back().endCol ) score = 1;
	return score;
}

bool spelled( const std::vector<Mapped> & ts, int b, int e ) {
	for ( const Mapped & m : ts ) {
		if ( m.col < e && m.endCol > b && ( m.origTok >= 0 || ( m.start != m.outerStart && !m.body ) ) ) return true;
	}
	return false;
}

// `end` < 0 picks for a single position `col`; otherwise for the range
// [col, end) on this line.
const std::vector<Mapped> & pickSubLine( const SubLines & subs, int col, bool isEnd, int end = -1 ) {
	if ( subs.size() == 1 ) return subs[0];
	size_t best = 0;
	int bestScore = -1;
	for ( size_t s = 0; s < subs.size(); ++s ) {
		int score;
		if ( end >= 0 ) score = 2 * ( boundaryScore( subs[s], col, false ) + boundaryScore( subs[s], end, true ) ) + spelled( subs[s], col, end );
		else score = 2 * boundaryScore( subs[s], col, isEnd ) + ( isEnd ? spelled( subs[s], col - 1, col ) : spelled( subs[s], col, col + 1 ) );
		if ( score > bestScore ) best = s, bestScore = score;
	}
	return subs[best];
}

Loc mapIn( const FileTable & ft, const std::vector<Mapped> & ts, int col, bool isEnd, bool outer ) {
	auto spanStart = [&]( const Mapped & m ) { return outer ? m.outerStart : m.start; };
	auto spanEnd = [&]( const Mapped & m ) { return outer ? m.outerEnd : m.end; };
	// Inside an exactly matched token, keep the offset within the token.
	auto inside = [&]( const Mapped & m, int off ) {
		const Token & t = ft.toks[m.origTok];
		if ( t.line == t.endLine ) return Loc{ t.line, std::min( t.col + off, t.endCol ) };
		Loc l = startOf( t );
		for ( int k = t.offset; k < t.endOffset && off > 0; ) {
			if ( ft.text[k] == '\\' && k + 1 < t.endOffset && ( ft.text[k + 1] == '\n' || ft.text[k + 1] == '\r' ) ) {
				k += ft.text[k + 1] == '\n' ? 2 : 3;
				l = { l.line + 1, 0 };
				continue;
			}
			++k, ++l.col, --off;
		}
		return l;
	};

	if ( !isEnd ) {
		auto it = std::upper_bound( ts.begin(), ts.end(), col, []( int c, const Mapped & m ) { return c < m.endCol; } );
		if ( it == ts.end() ) return spanEnd( ts.back() );
		if ( col <= it->col ) return spanStart( *it );
		if ( it->origTok >= 0 ) return inside( *it, col - it->col );
		return spanStart( *it );
	}
	// The last token starting before col.
	auto it = std::lower_bound( ts.begin(), ts.end(), col, []( const Mapped & m, int c ) { return m.col < c; } );
	if ( it == ts.begin() ) return spanStart( ts.front() );
	--it;
	if ( col >= it->endCol ) return spanEnd( *it );
	if ( it->origTok >= 0 ) return inside( *it, col - it->col );
	return spanEnd( *it );
}

} // namespace

struct SourceMap::Impl {
	std::string prep;
	Reader read;
	std::mutex mu;
	bool indexed = false;
	std::unordered_map<std::string, std::unique_ptr<FileTable>> files;
	// Every stored preprocessed line by its 1-based line in the text.
	struct PhysLine {
		int pline;
		FileTable * ft;
		int prep;						// index into ft->prep
	};
	std::vector<PhysLine> phys;

	// Every #define and #undef that cpp printed, by macro name, in order, with
	// its pline. nullopt for an #undef. Filled by index().
	std::unordered_map<std::string, std::vector<std::pair<int, std::optional<Define>>>> defines;

	void index();
	const FileTable * table( const std::string & file );
	const Define * definition( const std::string & name, int pline ) const;

	// Finds the table and the line's tokens; subs is null when the line has
	// none (or the file is unknown), and the caller falls back. With a known
	// pline, sub is the piece it names; otherwise the caller picks one.
	struct Line {
		const FileTable * ft = nullptr;
		const SubLines * subs = nullptr;
		int sub = -1;
		int line = -1;					// 0-based real line, when known
	};
	Line line( const std::string & file, int line1, int pline );
	Loc fallback( const Line & l, int line1, int col ) const {
		Loc f{ l.line >= 0 ? l.line : std::max( 0, line1 - 1 ), std::max( 0, col ) };
		return l.ft && l.ft->ok ? l.ft->clamp( f ) : f;
	}
	const std::vector<Mapped> & tokens( const Line & l, int col, bool isEnd ) const {
		return l.sub >= 0 ? ( *l.subs )[l.sub] : pickSubLine( *l.subs, col, isEnd );
	}
	Loc map( const std::string & file, Point p, bool isEnd ) {
		Line l = line( file, p.line, p.pline );
		if ( !l.subs ) return fallback( l, p.line, p.col );
		return mapIn( *l.ft, tokens( l, p.col, isEnd ), std::max( 0, p.col ), isEnd, false );
	}
	Range mapRange( const std::string & file, Point a, Point b );
	bool inMacroBody( const std::string & file, Point p ) {
		Line l = line( file, p.line, p.pline );
		if ( !l.subs ) return false;
		const std::vector<Mapped> & ts = tokens( l, p.col, false );
		auto it = std::upper_bound( ts.begin(), ts.end(), p.col, []( int c, const Mapped & m ) { return c < m.endCol; } );
		return it != ts.end() && it->col <= p.col && it->body;
	}
};

void SourceMap::Impl::index() {
	FileTable * cur = nullptr;
	int next = 0;
	// The file of the last code line, while only markers have followed it.
	FileTable * last = nullptr;
	// Whether the last marker, and the marker before the last code line, said
	// "system header" (flag 3).
	bool sys = false, lastSys = false;
	int pline = 1;
	for ( size_t pos = 0; pos < prep.size(); ++pline ) {
		size_t nl = prep.find( '\n', pos );
		if ( nl == std::string::npos ) nl = prep.size();
		std::string_view l( prep.data() + pos, nl - pos );
		if ( !l.empty() && l[0] == '#' ) {
			int n, flags;
			std::optional<std::string> f;
			if ( parseMarker( l, n, f, flags ) ) {
				if ( f ) {
					auto & slot = files[*f];
					if ( !slot ) slot = std::make_unique<FileTable>();
					cur = slot.get();
				}
				next = n - 1;
				sys = flags & 4;
			} else {
				// A #define or #undef from -dD takes the place of the directive's
				// own line, as a #pragma does.
				if ( definitionLine( l ) ) {
					std::string name;
					std::optional<Define> d;
					parseDefinition( l, name, d );
					if ( !name.empty() ) defines[name].push_back( { pline, std::move( d ) } );
				}
				++next;
				last = nullptr;
			}
		} else {
			if ( cur && next >= 0 && l.find_first_not_of( " \t\r\f\v" ) != std::string_view::npos ) {
				// cpp continues a line after a marker that repeats its number
				// when the tokens switch between system-header macros and the
				// file. A repeated number without that switch is a #line.
				bool cont = last == cur && cur->prep.back().presumed == next && sys != lastSys;
				cur->prep.push_back( { next, next, pos, l.size(), cont, pline } );
				phys.push_back( { pline, cur, (int)cur->prep.size() - 1 } );
				last = cur;
			} else {
				last = nullptr;
			}
			lastSys = sys;
			++next;
		}
		pos = nl + 1;
	}
}

const FileTable * SourceMap::Impl::table( const std::string & file ) {
	FileTable * ft;
	{
		std::lock_guard<std::mutex> lock( mu );
		if ( !indexed ) {
			index();
			indexed = true;
		}
		auto it = files.find( file );
		if ( it == files.end() ) return nullptr;
		ft = it->second.get();
	}
	std::call_once( ft->once, [&] {
		buildFile( *ft, file, prep, read, [this]( const std::string & name, int pline ) { return definition( name, pline ); } );
	} );
	return ft;
}

// The last definition of `name` printed above preprocessed line `pline`.
const Define * SourceMap::Impl::definition( const std::string & name, int pline ) const {
	auto it = defines.find( name );
	if ( it == defines.end() ) return nullptr;
	const auto & events = it->second;
	auto e = std::lower_bound( events.begin(), events.end(), pline, []( const auto & ev, int p ) { return ev.first < p; } );
	if ( e == events.begin() ) return nullptr;
	--e;
	return e->second ? &*e->second : nullptr;
}

SourceMap::Impl::Line SourceMap::Impl::line( const std::string & file, int line1, int pline ) {
	Line l;
	l.ft = table( file );
	if ( !l.ft || !l.ft->ok ) return l;
	// The preprocessed line that pline names, if it is a line of this file
	// with the same line number.
	if ( pline > 0 ) {
		auto it = std::lower_bound( phys.begin(), phys.end(), pline, []( const PhysLine & p, int n ) { return p.pline < n; } );
		if ( it != phys.end() && it->pline == pline && it->ft == l.ft ) {
			const PrepLine & pl = l.ft->prep[it->prep];
			if ( pl.presumed == line1 - 1 && pl.seg >= 0 ) {
				const Segment & seg = l.ft->segs[pl.seg];
				int k = pl.line - seg.first;
				if ( k >= 0 && k < (int)seg.lines.size() && pl.sub < (int)seg.lines[k].size() && !seg.lines[k][pl.sub].empty() ) {
					l.subs = &seg.lines[k];
					l.sub = pl.sub;
					l.line = pl.line;
					return l;
				}
			}
		}
	}
	if ( line1 < 1 ) return l;
	l.line = l.ft->actualLine( line1 - 1 );
	for ( const Segment & seg : l.ft->segs ) {
		int k = l.line - seg.first;
		if ( k >= 0 && k < (int)seg.lines.size() && !seg.lines[k].empty() ) {
			l.subs = &seg.lines[k];
			break;
		}
	}
	return l;
}

Range SourceMap::Impl::mapRange( const std::string & file, Point a, Point b ) {
	Line la = line( file, a.line, a.pline ), lb = a.line == b.line && a.pline == b.pline ? la : line( file, b.line, b.pline );
	const std::vector<Mapped> * sa = nullptr, * sb = nullptr;
	if ( la.subs && la.sub >= 0 ) sa = &( *la.subs )[la.sub];
	if ( lb.subs && lb.sub >= 0 ) sb = &( *lb.subs )[lb.sub];
	if ( !sa && !sb && la.subs && a.line == b.line && b.col >= a.col ) {
		sa = sb = &pickSubLine( *la.subs, a.col, false, b.col );
	} else {
		if ( !sa && la.subs ) sa = &pickSubLine( *la.subs, a.col, false );
		if ( !sb && lb.subs ) sb = &pickSubLine( *lb.subs, b.col, true );
	}
	auto start = [&]( bool outer ) { return sa ? mapIn( *la.ft, *sa, std::max( 0, a.col ), false, outer ) : fallback( la, a.line, a.col ); };
	auto end = [&]( bool outer ) { return sb ? mapIn( *lb.ft, *sb, std::max( 0, b.col ), true, outer ) : fallback( lb, b.line, b.col ); };
	Loc s = start( false );
	if ( b.line == a.line && b.col == a.col && b.pline == a.pline ) return { s, s };
	Loc e = end( false );
	if ( e < s ) {
		// Possible when macro arguments appear in the expansion out of order.
		s = start( true );
		e = end( true );
		if ( e < s ) e = s;
	}
	return { s, e };
}

SourceMap::SourceMap( std::string preprocessed, Reader read ) : impl( std::make_unique<Impl>() ) {
	impl->prep = std::move( preprocessed );
	impl->read = std::move( read );
}

SourceMap::~SourceMap() = default;
SourceMap::SourceMap( SourceMap && ) noexcept = default;
SourceMap & SourceMap::operator=( SourceMap && ) noexcept = default;

SourceMap SourceMap::identity() {
	SourceMap m( std::string(), nullptr );
	m.impl.reset();
	return m;
}

std::string SourceMap::withoutDefinitions( std::string_view preprocessed ) {
	std::string out;
	out.reserve( preprocessed.size() );
	for ( size_t pos = 0; pos < preprocessed.size(); ) {
		size_t nl = preprocessed.find( '\n', pos );
		if ( nl == std::string_view::npos ) nl = preprocessed.size();
		std::string_view l = preprocessed.substr( pos, nl - pos );
		if ( !definitionLine( l ) ) out += l;
		if ( nl < preprocessed.size() ) out += '\n';
		pos = nl + 1;
	}
	return out;
}

Loc SourceMap::map( const std::string & file, int line, int col, bool isEnd ) const {
	return map( file, Point{ line, col, 0 }, isEnd );
}

Loc SourceMap::map( const std::string & file, Point p, bool isEnd ) const {
	if ( !impl ) return { std::max( 0, p.line - 1 ), std::max( 0, p.col ) };
	return impl->map( file, p, isEnd );
}

Range SourceMap::mapRange( const std::string & file, int line, int col, int endLine, int endCol ) const {
	return mapRange( file, Point{ line, col, 0 }, Point{ endLine, endCol, 0 } );
}

Range SourceMap::mapRange( const std::string & file, Point start, Point end ) const {
	if ( !impl ) {
		Loc s{ std::max( 0, start.line - 1 ), std::max( 0, start.col ) }, e{ std::max( 0, end.line - 1 ), std::max( 0, end.col ) };
		return { s, std::max( s, e ) };
	}
	return impl->mapRange( file, start, end );
}

bool SourceMap::inMacroBody( const std::string & file, Point p ) const {
	return impl && impl->inMacroBody( file, p );
}

std::vector<std::string> SourceMap::files() const {
	if ( !impl ) return {};
	std::lock_guard<std::mutex> lock( impl->mu );
	if ( !impl->indexed ) {
		impl->index();
		impl->indexed = true;
	}
	std::vector<std::string> out;
	for ( const auto & [name, table] : impl->files ) out.push_back( name );
	return out;
}

std::optional<std::vector<int>> SourceMap::outputLines( const std::string & file ) const {
	if ( !impl ) return std::nullopt;
	// Building the table moves the lines after a #line back to their real
	// numbers; index() alone leaves the numbers the markers give.
	const FileTable * ft = impl->table( file );
	if ( !ft ) return std::nullopt;
	std::vector<int> out;
	for ( const PrepLine & p : ft->prep ) out.push_back( p.line );
	std::sort( out.begin(), out.end() );
	out.erase( std::unique( out.begin(), out.end() ), out.end() );
	return out;
}

} // namespace cfalsp
