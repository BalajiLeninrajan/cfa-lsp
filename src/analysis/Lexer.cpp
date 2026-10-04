#include "Lexer.hpp"

#include <algorithm>
#include <cstring>

namespace cfalsp {

namespace {

bool isIdStart( unsigned char c ) {
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' || c == '$' || c >= 0x80;
}

bool isDigit( unsigned char c ) { return c >= '0' && c <= '9'; }

bool isIdChar( unsigned char c ) { return isIdStart( c ) || isDigit( c ); }

// Longest first within each length class; lex() takes the longest match.
const char * const cPuncts[] = {
	"...", "<<=", ">>=",
	"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
	"*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##",
};

const char * const cfaPuncts[] = {
	"-~==", "-~!=",
	"+~=", "-~=", "~==", "~!=",
	"\\=", "@=", "@[", "+~", "~=", "-~",
};

const char singlePuncts[] = "[](){}.,:;!+-*/%^~&|<>=?#@`\\";

// Operators that can appear between the ?s of a CFA binary operator name.
const char * const binaryOverOps[] = {
	"<<=", ">>=", "==", "!=", "<<", ">>", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "\\=", "&=", "|=", "^=",
	"+", "-", "*", "/", "%", "\\", "^", "&", "|", "<", ">", "=",
};

class Lexer {
  public:
	Lexer( std::string_view text, const LexOptions & opts ) : raw( text ), opts( opts ) {
		bool spliced = false;
		for ( size_t i = 0; i + 1 < raw.size(); ++i ) {
			if ( raw[i] == '\\' && ( raw[i + 1] == '\n' || ( raw[i + 1] == '\r' && i + 2 < raw.size() && raw[i + 2] == '\n' ) ) ) {
				spliced = true;
				break;
			}
		}
		if ( spliced ) {
			owned.reserve( raw.size() );
			phys.reserve( raw.size() + 1 );
			for ( size_t i = 0; i < raw.size(); ) {
				if ( raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\n' ) { i += 2; continue; }
				if ( raw[i] == '\\' && i + 2 < raw.size() && raw[i + 1] == '\r' && raw[i + 2] == '\n' ) { i += 3; continue; }
				owned.push_back( raw[i] );
				phys.push_back( (int)i );
				++i;
			}
			phys.push_back( (int)raw.size() );
			s = owned;
		} else {
			s = raw;
		}
		lineStarts.push_back( 0 );
		for ( size_t i = 0; i < raw.size(); ++i ) {
			if ( raw[i] == '\n' ) lineStarts.push_back( (int)i + 1 );
		}
	}

	std::vector<Token> run();

  private:
	std::string_view raw;
	const LexOptions & opts;
	std::string owned;
	std::string_view s;				// logical text: raw with splices removed
	std::vector<int> phys;			// logical index -> raw offset; empty when there are no splices
	std::vector<int> lineStarts;
	size_t lineCursor = 0;
	std::vector<Token> out;

	char at( size_t i ) const { return i < s.size() ? s[i] : '\0'; }
	bool startsWith( size_t i, const char * p ) const {
		size_t len = std::strlen( p );
		return i + len <= s.size() && s.compare( i, len, p ) == 0;
	}
	int physOf( size_t i ) const { return phys.empty() ? (int)i : phys[i]; }

	// Tokens are emitted in order, so the line lookup moves a cursor forward
	// and only falls back to a search when it has to go back.
	void lineCol( int off, int & line, int & col ) {
		if ( lineCursor >= lineStarts.size() || lineStarts[lineCursor] > off ) {
			lineCursor = std::upper_bound( lineStarts.begin(), lineStarts.end(), off ) - lineStarts.begin() - 1;
		}
		while ( lineCursor + 1 < lineStarts.size() && lineStarts[lineCursor + 1] <= off ) ++lineCursor;
		line = (int)lineCursor;
		col = off - lineStarts[lineCursor];
	}

	void emit( TokKind kind, size_t b, size_t e ) {
		Token t;
		t.kind = kind;
		t.text = std::string( s.substr( b, e - b ) );
		t.offset = physOf( b );
		t.endOffset = e > b ? physOf( e - 1 ) + 1 : t.offset;
		lineCol( t.offset, t.line, t.col );
		lineCol( t.endOffset, t.endLine, t.endCol );
		lineCursor = std::min( lineCursor, lineStarts.size() - 1 );
		out.push_back( std::move( t ) );
	}

	size_t skipLiteral( size_t i ) const {	// i is at the opening quote
		char q = s[i++];
		while ( i < s.size() && s[i] != q && s[i] != '\n' ) {
			if ( s[i] == '\\' && i + 1 < s.size() && s[i + 1] != '\n' ) i += 2;
			else ++i;
		}
		if ( i < s.size() && s[i] == q ) ++i;
		return i;
	}

	size_t blockCommentEnd( size_t i ) const {	// i is at "/*"
		size_t e = s.find( "*/", i + 2 );
		return e == std::string_view::npos ? s.size() : e + 2;
	}

	size_t lineCommentEnd( size_t i ) const {
		size_t e = s.find( '\n', i );
		return e == std::string_view::npos ? s.size() : e;
	}

	size_t directiveEnd( size_t i ) const {
		while ( i < s.size() && s[i] != '\n' ) {
			if ( startsWith( i, "/*" ) ) i = blockCommentEnd( i );
			else if ( startsWith( i, "//" ) ) i = lineCommentEnd( i );
			else if ( s[i] == '"' || s[i] == '\'' ) i = skipLiteral( i );
			else ++i;
		}
		return i;
	}

	size_t cfaOperatorName( size_t i ) const;	// length of a CFA operator name at i, or 0
	size_t punctLength( size_t i ) const;
};

size_t Lexer::cfaOperatorName( size_t i ) const {
	size_t best = 0;
	char c = s[i];
	if ( c == '?' ) {
		for ( const char * op : binaryOverOps ) {
			size_t len = std::strlen( op );
			if ( startsWith( i + 1, op ) && at( i + 1 + len ) == '?' ) best = std::max( best, len + 2 );
		}
		for ( const char * op : { "++", "--", "()", "[?]", "{}" } ) {
			if ( startsWith( i + 1, op ) ) best = std::max( best, std::strlen( op ) + 1 );
		}
		if ( at( i + 1 ) == '`' && isIdStart( at( i + 2 ) ) ) {
			size_t j = i + 3;
			while ( j < s.size() && isIdChar( s[j] ) ) ++j;
			best = std::max( best, j - i );
		}
	} else if ( c == '^' && startsWith( i, "^?{}" ) ) {
		best = 4;
	} else if ( c == '`' && at( i + 1 ) == '`' && isIdStart( at( i + 2 ) ) ) {
		size_t j = i + 3;
		while ( j < s.size() && isIdChar( s[j] ) ) ++j;
		best = j - i;
	}
	// Unary operator names: ~? !? +? -? *? ++? --?
	if ( startsWith( i, "++?" ) || startsWith( i, "--?" ) ) best = std::max( best, (size_t)3 );
	else if ( std::strchr( "~!+-*", c ) && at( i + 1 ) == '?' ) best = std::max( best, (size_t)2 );
	return best;
}

size_t Lexer::punctLength( size_t i ) const {
	size_t best = 0;
	for ( const char * p : cPuncts ) {
		if ( startsWith( i, p ) ) best = std::max( best, std::strlen( p ) );
	}
	if ( opts.cfa ) {
		for ( const char * p : cfaPuncts ) {
			if ( startsWith( i, p ) ) best = std::max( best, std::strlen( p ) );
		}
	}
	if ( best == 0 && std::strchr( singlePuncts, s[i] ) ) best = 1;
	return best;
}

std::vector<Token> Lexer::run() {
	bool lineStart = true;		// no token yet on this logical line
	size_t i = 0;
	while ( i < s.size() ) {
		unsigned char c = s[i];
		if ( c == '\n' ) { lineStart = true; ++i; continue; }
		if ( c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' ) { ++i; continue; }
		if ( c == '/' && at( i + 1 ) == '/' ) {
			size_t e = lineCommentEnd( i );
			if ( opts.comments ) emit( TokKind::Comment, i, e );
			i = e;
			continue;
		}
		if ( c == '/' && at( i + 1 ) == '*' ) {
			size_t e = blockCommentEnd( i );
			if ( opts.comments ) emit( TokKind::Comment, i, e );
			i = e;
			continue;
		}
		if ( c == '#' && lineStart ) {
			size_t e = directiveEnd( i );
			if ( opts.directives ) emit( TokKind::Directive, i, e );
			i = e;
			continue;
		}
		lineStart = false;

		if ( isIdStart( c ) ) {
			size_t j = i + 1;
			while ( j < s.size() && isIdChar( s[j] ) ) ++j;
			std::string_view id = s.substr( i, j - i );
			char q = at( j );
			if ( ( q == '"' || q == '\'' ) && ( id == "u8" || id == "u" || id == "U" || id == "L" ) ) {
				size_t e = skipLiteral( j );
				emit( q == '"' ? TokKind::String : TokKind::Char, i, e );
				i = e;
			} else {
				emit( TokKind::Identifier, i, j );
				i = j;
			}
			continue;
		}
		if ( isDigit( c ) || ( c == '.' && isDigit( at( i + 1 ) ) ) ) {
			size_t j = i + 1;
			while ( j < s.size() ) {
				char d = s[j];
				if ( ( d == 'e' || d == 'E' || d == 'p' || d == 'P' ) && ( at( j + 1 ) == '+' || at( j + 1 ) == '-' ) ) j += 2;
				else if ( isIdChar( d ) || d == '.' ) ++j;
				else break;
			}
			emit( TokKind::Number, i, j );
			i = j;
			continue;
		}
		if ( c == '"' || c == '\'' ) {
			size_t e = skipLiteral( i );
			emit( c == '"' ? TokKind::String : TokKind::Char, i, e );
			i = e;
			continue;
		}
		size_t op = opts.cfa ? cfaOperatorName( i ) : 0;
		size_t p = punctLength( i );
		if ( op > p ) {
			emit( TokKind::Identifier, i, i + op );
			i += op;
		} else if ( p > 0 ) {
			emit( TokKind::Punct, i, i + p );
			i += p;
		} else {
			emit( TokKind::Other, i, i + 1 );
			++i;
		}
	}
	return std::move( out );
}

} // namespace

std::vector<Token> lex( std::string_view text, const LexOptions & opts ) {
	return Lexer( text, opts ).run();
}

} // namespace cfalsp
