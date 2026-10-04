#include "Text.hpp"

#include <algorithm>

namespace cfalsp {

namespace {

// Length in bytes of the UTF-8 sequence at s[i], or 1 for an invalid byte.
int seqLen( std::string_view s, size_t i ) {
	unsigned char c = (unsigned char)s[i];
	int n;
	if ( c < 0x80 ) return 1;
	else if ( ( c & 0xE0 ) == 0xC0 ) n = 2;
	else if ( ( c & 0xF0 ) == 0xE0 ) n = 3;
	else if ( ( c & 0xF8 ) == 0xF0 ) n = 4;
	else return 1;
	if ( i + n > s.size() ) return 1;
	for ( int k = 1; k < n; k += 1 ) {
		if ( ( (unsigned char)s[i + k] & 0xC0 ) != 0x80 ) return 1;
	}
	return n;
}

// UTF-16 code units for a sequence of `len` bytes.
int units( int len, Encoding enc ) {
	if ( enc == Encoding::Utf8 ) return len;
	return len == 4 ? 2 : 1;
}

} // namespace

int toByteCol( std::string_view line, int col, Encoding enc ) {
	if ( col <= 0 ) return 0;
	size_t i = 0;
	int u = 0;
	while ( i < line.size() && u < col ) {
		int n = seqLen( line, i );
		int w = units( n, enc );
		if ( u + w > col ) break;				// col falls inside this character
		u += w;
		i += n;
	}
	return (int)i;
}

int fromByteCol( std::string_view line, int byteCol, Encoding enc ) {
	if ( byteCol <= 0 ) return 0;
	size_t limit = std::min( (size_t)byteCol, line.size() );
	size_t i = 0;
	int u = 0;
	while ( i < limit ) {
		int n = seqLen( line, i );
		if ( i + n > limit ) break;				// offset inside this character
		u += units( n, enc );
		i += n;
	}
	if ( enc == Encoding::Utf8 ) return (int)i;
	return u;
}

Text::Text( std::string s ) : text( std::move( s ) ) { index(); }

void Text::index() {
	starts.clear();
	starts.push_back( 0 );
	for ( size_t i = 0; i < text.size(); i += 1 ) {
		if ( text[i] == '\n' ) starts.push_back( i + 1 );
	}
}

std::string_view Text::line( int n ) const {
	if ( n < 0 || n >= lineCount() ) return {};
	size_t b = starts[n];
	size_t e = n + 1 < lineCount() ? starts[n + 1] - 1 : text.size();
	if ( e > b && text[e - 1] == '\r' ) e -= 1;
	return std::string_view( text ).substr( b, e - b );
}

Loc Text::clamp( Loc l ) const {
	if ( l.line < 0 ) return { 0, 0 };
	if ( l.line >= lineCount() ) {
		int last = lineCount() - 1;
		return { last, (int)( text.size() - starts[last] ) };
	}
	size_t b = starts[l.line];
	size_t e = l.line + 1 < lineCount() ? starts[l.line + 1] - 1 : text.size();
	int len = (int)( e - b );
	return { l.line, std::clamp( l.col, 0, len ) };
}

size_t Text::offset( Loc l ) const {
	Loc c = clamp( l );
	return starts[c.line] + c.col;
}

Loc Text::loc( size_t off ) const {
	off = std::min( off, text.size() );
	auto it = std::upper_bound( starts.begin(), starts.end(), off );
	int line = (int)( it - starts.begin() ) - 1;
	return { line, (int)( off - starts[line] ) };
}

Loc Text::fromLsp( int ln, int character, Encoding enc ) const {
	if ( ln < 0 ) return { 0, 0 };
	if ( ln >= lineCount() ) return clamp( { ln, 0 } );
	return { ln, toByteCol( line( ln ), character, enc ) };
}

int Text::toLspCol( Loc l, Encoding enc ) const {
	Loc c = clamp( l );
	std::string_view s = line( c.line );
	if ( c.col > (int)s.size() ) {
		// Pointing at a '\r' before '\n'; count it as one unit.
		return fromByteCol( s, (int)s.size(), enc ) + ( c.col - (int)s.size() );
	}
	return fromByteCol( s, c.col, enc );
}

std::pair<Loc, Loc> Text::replace( Loc start, Loc end, std::string_view newText ) {
	size_t a = offset( start ), b = offset( end );
	if ( b < a ) std::swap( a, b );
	Loc s = loc( a ), e = loc( b );
	text.replace( a, b - a, newText );
	index();
	return { s, e };
}

Range Text::lineRange( int ln ) const {
	std::string_view s = line( ln );
	int b = 0;
	while ( b < (int)s.size() && ( s[b] == ' ' || s[b] == '\t' ) ) b += 1;
	int e = (int)s.size();
	if ( b == e ) b = 0;
	return { { ln, b }, { ln, e } };
}

} // namespace cfalsp
