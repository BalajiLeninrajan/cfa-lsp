#include "SourceMap.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
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
// current line number continues that line on a new preprocessed line; see
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
// invocation; identifiers in the expansion that also appear in the invocation
// (usually arguments) map to that occurrence instead.

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
	size_t off, len;				// in the preprocessed text
	bool cont;						// continues the previous entry's original line
};

struct FileTable {
	std::vector<PrepLine> prep;
	std::once_flag once;
	bool ok = false;
	std::string text;
	std::vector<int> lineStarts;
	std::vector<Token> toks;
	std::vector<Segment> segs;

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
};

Loc startOf( const Token & t ) { return { t.line, t.col }; }
Loc endOf( const Token & t ) { return { t.endLine, t.endCol }; }

class Aligner {
  public:
	Aligner( const FileTable & ft, const std::vector<PTok> & P, std::vector<Mapped> & res )
		: ft( ft ), O( ft.toks ), P( P ), res( res ) {}

	void group( size_t pb, size_t pe, size_t ob, size_t oe );

  private:
	const FileTable & ft;
	const std::vector<Token> & O;
	const std::vector<PTok> & P;
	std::vector<Mapped> & res;

	// Aligns P[pb, pe) with O[ob, oe). With `commit`, stops at the first exact match at or after O[commit]
	// and returns where it stopped; otherwise returns { pe, oe }.
	std::pair<size_t, size_t> table( size_t pb, size_t pe, size_t ob, size_t oe, size_t commit = SIZE_MAX );
	void alignLong( size_t pb, size_t pe, size_t ob, size_t oe, size_t gb, size_t ge );
	void exact( size_t p, size_t o );
	void macro( size_t pb, size_t pe, size_t ob, size_t oe );
	void unmatched( size_t p, size_t o, size_t ob, size_t oe );
};

void Aligner::exact( size_t p, size_t o ) {
	Mapped & m = res[p];
	m.start = m.outerStart = startOf( O[o] );
	m.end = m.outerEnd = endOf( O[o] );
	m.origTok = (int)o;
}

void Aligner::macro( size_t pb, size_t pe, size_t ob, size_t oe ) {
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
			macro( pb + i, pb + i2, ob + j, ob + e );
			i = i2, j = e;
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

void alignSegment( FileTable & ft, std::string_view prep, size_t b, size_t e ) {
	Segment seg;
	seg.first = ft.prep[b].line;
	seg.lines.resize( ft.prep[e - 1].line - seg.first + 1 );

	std::vector<PTok> P;
	int sub = 0;
	for ( size_t k = b; k < e; ++k ) {
		const PrepLine & pl = ft.prep[k];
		sub = pl.cont ? sub + 1 : 0;
		for ( Token & t : lex( prep.substr( pl.off, pl.len ) ) ) P.push_back( { std::move( t ), pl.line, sub } );
	}
	std::vector<Mapped> res( P.size() );
	for ( size_t p = 0; p < P.size(); ++p ) {
		res[p].col = P[p].tok.col;
		res[p].endCol = P[p].tok.endCol;
	}

	const std::vector<Token> & O = ft.toks;
	Aligner al( ft, P, res );
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

void buildFile( FileTable & ft, const std::string & name, std::string_view prep, const SourceMap::Reader & read ) {
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
	for ( size_t b = 0; b < ft.prep.size(); ) {
		size_t e = b + 1;
		while ( e < ft.prep.size() && ( ft.prep[e].line > ft.prep[e - 1].line || ft.prep[e].cont ) ) ++e;
		alignSegment( ft, prep, b, e );
		b = e;
	}
	ft.ok = true;
}

// Parses `# N "file" flags` or `#line N "file"`. Returns false for any other
// directive (#pragma, #ident, ...).
bool parseMarker( std::string_view l, int & line, std::optional<std::string> & file ) {
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
		if ( m.col < e && m.endCol > b && ( m.origTok >= 0 || m.start != m.outerStart ) ) return true;
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

	void index();
	const FileTable * table( const std::string & file );

	// Finds the table and the line's tokens; subs is null when the line has
	// none (or the file is unknown), and the caller falls back.
	struct Line {
		const FileTable * ft = nullptr;
		const SubLines * subs = nullptr;
	};
	Line line( const std::string & file, int line1 );
	Loc fallback( const Line & l, int line1, int col ) const {
		Loc f{ std::max( 0, line1 - 1 ), std::max( 0, col ) };
		return l.ft && l.ft->ok ? l.ft->clamp( f ) : f;
	}
	Loc map( const std::string & file, int line1, int col, bool isEnd ) {
		Line l = line( file, line1 );
		if ( !l.subs ) return fallback( l, line1, col );
		return mapIn( *l.ft, pickSubLine( *l.subs, col, isEnd ), std::max( 0, col ), isEnd, false );
	}
	Range mapRange( const std::string & file, int line1, int col, int endLine1, int endCol );
};

void SourceMap::Impl::index() {
	FileTable * cur = nullptr;
	int next = 0;
	// The file of the last code line, while only markers have followed it.
	FileTable * last = nullptr;
	for ( size_t pos = 0; pos < prep.size(); ) {
		size_t nl = prep.find( '\n', pos );
		if ( nl == std::string::npos ) nl = prep.size();
		std::string_view l( prep.data() + pos, nl - pos );
		if ( !l.empty() && l[0] == '#' ) {
			int n;
			std::optional<std::string> f;
			if ( parseMarker( l, n, f ) ) {
				if ( f ) {
					auto & slot = files[*f];
					if ( !slot ) slot = std::make_unique<FileTable>();
					cur = slot.get();
				}
				next = n - 1;
			} else {
				++next;
				last = nullptr;
			}
		} else {
			if ( cur && next >= 0 && l.find_first_not_of( " \t\r\f\v" ) != std::string_view::npos ) {
				bool cont = last == cur && cur->prep.back().line == next;
				cur->prep.push_back( { next, pos, l.size(), cont } );
				last = cur;
			} else {
				last = nullptr;
			}
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
	std::call_once( ft->once, [&] { buildFile( *ft, file, prep, read ); } );
	return ft;
}


SourceMap::Impl::Line SourceMap::Impl::line( const std::string & file, int line1 ) {
	Line l;
	l.ft = table( file );
	if ( !l.ft || !l.ft->ok ) return l;
	for ( const Segment & seg : l.ft->segs ) {
		int k = line1 - 1 - seg.first;
		if ( k >= 0 && k < (int)seg.lines.size() && !seg.lines[k].empty() ) {
			l.subs = &seg.lines[k];
			break;
		}
	}
	return l;
}

Range SourceMap::Impl::mapRange( const std::string & file, int line1, int col, int endLine1, int endCol ) {
	Line a = line( file, line1 ), b = line1 == endLine1 ? a : line( file, endLine1 );
	const std::vector<Mapped> * sa = nullptr, * sb = nullptr;
	if ( a.subs && line1 == endLine1 && endCol >= col ) {
		sa = sb = &pickSubLine( *a.subs, col, false, endCol );
	} else {
		if ( a.subs ) sa = &pickSubLine( *a.subs, col, false );
		if ( b.subs ) sb = &pickSubLine( *b.subs, endCol, true );
	}
	auto start = [&]( bool outer ) { return sa ? mapIn( *a.ft, *sa, std::max( 0, col ), false, outer ) : fallback( a, line1, col ); };
	auto end = [&]( bool outer ) { return sb ? mapIn( *b.ft, *sb, std::max( 0, endCol ), true, outer ) : fallback( b, endLine1, endCol ); };
	Loc s = start( false );
	if ( endLine1 == line1 && endCol == col ) return { s, s };
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

Loc SourceMap::map( const std::string & file, int line, int col, bool isEnd ) const {
	if ( !impl ) return { std::max( 0, line - 1 ), std::max( 0, col ) };
	return impl->map( file, line, col, isEnd );
}

Range SourceMap::mapRange( const std::string & file, int line, int col, int endLine, int endCol ) const {
	if ( !impl ) {
		Loc s{ std::max( 0, line - 1 ), std::max( 0, col ) }, e{ std::max( 0, endLine - 1 ), std::max( 0, endCol ) };
		return { s, std::max( s, e ) };
	}
	return impl->mapRange( file, line, col, endLine, endCol );
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
	std::lock_guard<std::mutex> lock( impl->mu );
	if ( !impl->indexed ) {
		impl->index();
		impl->indexed = true;
	}
	auto it = impl->files.find( file );
	if ( it == impl->files.end() ) return std::nullopt;
	std::vector<int> out;
	for ( const PrepLine & p : it->second->prep ) out.push_back( p.line );
	std::sort( out.begin(), out.end() );
	out.erase( std::unique( out.begin(), out.end() ), out.end() );
	return out;
}

} // namespace cfalsp
