#include "TextScan.hpp"

#include <algorithm>
#include <cctype>

namespace cfalsp::text {

namespace {

bool isSpace( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim( std::string_view s ) {
	while ( !s.empty() && isSpace( s.front() ) ) s.remove_prefix( 1 );
	while ( !s.empty() && isSpace( s.back() ) ) s.remove_suffix( 1 );
	return s;
}

std::string stripSpaces( std::string_view s ) {
	std::string out;
	for ( char c : s ) if ( !isSpace( c ) ) out += c;
	return out;
}

// Lines made only of comment punctuation, like "////////" or " * ----".
bool isRule( std::string_view s ) {
	s = trim( s );
	return std::all_of( s.begin(), s.end(), []( char c ) { return std::string_view( "/*-=#~_+ \t" ).find( c ) != std::string_view::npos; } );
}

// Joins comment lines, dropping rules and blank lines at either end.
std::string joinDoc( const std::vector<std::string> & lines ) {
	std::vector<std::string> kept;
	for ( const auto & l : lines ) {
		if ( isRule( l ) && !trim( l ).empty() ) continue;
		kept.push_back( l );
	}
	while ( !kept.empty() && trim( kept.front() ).empty() ) kept.erase( kept.begin() );
	while ( !kept.empty() && trim( kept.back() ).empty() ) kept.pop_back();
	std::string out;
	for ( size_t i = 0; i < kept.size(); i += 1 ) {
		if ( i ) out += '\n';
		std::string_view l = kept[i];
		while ( !l.empty() && isSpace( l.back() ) ) l.remove_suffix( 1 );
		out += l;
	}
	return out;
}

// "// text", "/// text", "//! text", "//< text" -> "text".
std::string lineCommentBody( std::string_view s ) {
	s.remove_prefix( 2 );
	if ( !s.empty() && ( s.front() == '/' || s.front() == '!' ) ) s.remove_prefix( 1 );
	if ( !s.empty() && s.front() == '<' ) s.remove_prefix( 1 );
	if ( !s.empty() && s.front() == ' ' ) s.remove_prefix( 1 );
	return std::string( s );
}

// The inside of a /* */ comment, one entry per line, with leading " * " removed.
std::vector<std::string> blockCommentBody( std::string_view s ) {
	s.remove_prefix( 2 );
	if ( s.size() >= 2 && s.substr( s.size() - 2 ) == "*/" ) s.remove_suffix( 2 );
	if ( !s.empty() && ( s.front() == '*' || s.front() == '!' ) ) s.remove_prefix( 1 );
	if ( !s.empty() && s.front() == '<' ) s.remove_prefix( 1 );
	std::vector<std::string> out;
	bool starred = false;
	size_t p = 0;
	while ( true ) {
		size_t nl = s.find( '\n', p );
		std::string_view l = s.substr( p, nl == std::string_view::npos ? std::string_view::npos : nl - p );
		std::string_view t = l;
		while ( !t.empty() && isSpace( t.front() ) ) t.remove_prefix( 1 );
		if ( !out.empty() || !trim( l ).empty() ) {
			if ( !t.empty() && t.front() == '*' && !( t.size() > 1 && t[1] == '/' ) ) {
				starred = true;
				t.remove_prefix( 1 );
				if ( !t.empty() && t.front() == ' ' ) t.remove_prefix( 1 );
				out.emplace_back( t );
			} else if ( out.empty() ) {
				out.emplace_back( t );
			} else {
				out.emplace_back( l );
			}
		}
		if ( nl == std::string_view::npos ) break;
		p = nl + 1;
	}
	// Without leading stars, remove the common indentation of continuation lines.
	size_t common = std::string::npos;
	if ( starred ) return out;
	for ( size_t i = 1; i < out.size(); i += 1 ) {
		if ( trim( out[i] ).empty() ) continue;
		size_t ind = out[i].find_first_not_of( " \t" );
		common = std::min( common, ind );
	}
	if ( common != std::string::npos && common > 0 ) {
		for ( size_t i = 1; i < out.size(); i += 1 ) {
			if ( out[i].size() >= common ) out[i] = out[i].substr( common );
		}
	}
	return out;
}

} // namespace

bool isIdentStart( char c ) { return std::isalpha( (unsigned char)c ) || c == '_' || c == '$'; }
bool isIdentChar( char c ) { return std::isalnum( (unsigned char)c ) || c == '_' || c == '$'; }

bool isIdentifier( std::string_view s ) {
	if ( s.empty() || !isIdentStart( s[0] ) ) return false;
	return std::all_of( s.begin(), s.end(), isIdentChar );
}

FileText::FileText( std::string c ) : contents( std::move( c ) ) {
	starts.push_back( 0 );
	for ( size_t i = 0; i < contents.size(); i += 1 ) {
		if ( contents[i] == '\n' && i + 1 <= contents.size() ) starts.push_back( i + 1 );
	}
	// A trailing newline doesn't start another line.
	if ( starts.size() > 1 && starts.back() == contents.size() ) starts.pop_back();
}

std::string_view FileText::line( int n ) const {
	if ( n < 0 || n >= lineCount() ) return {};
	size_t b = starts[n];
	size_t e = n + 1 < lineCount() ? starts[n + 1] - 1 : contents.size();
	if ( e > b && contents[e - 1] == '\r' ) e -= 1;
	return std::string_view( contents ).substr( b, e - b );
}

std::string FileText::slice( Range r ) const {
	std::string out;
	for ( int l = std::max( r.start.line, 0 ); l <= r.end.line && l < lineCount(); l += 1 ) {
		std::string_view t = line( l );
		size_t b = l == r.start.line ? size_t( std::max( r.start.col, 0 ) ) : 0;
		size_t e = l == r.end.line ? size_t( std::max( r.end.col, 0 ) ) : t.size();
		b = std::min( b, t.size() );
		e = std::min( std::max( e, b ), t.size() );
		if ( l != r.start.line ) out += '\n';
		out += t.substr( b, e - b );
	}
	return out;
}

std::string docAbove( const FileText & f, int line ) {
	int l = line - 1;
	std::vector<std::string> lines;
	while ( l >= 0 ) {
		std::string_view t = trim( f.line( l ) );
		if ( t.size() < 2 || t.substr( 0, 2 ) != "//" ) break;
		lines.push_back( lineCommentBody( t ) );
		l -= 1;
	}
	if ( !lines.empty() ) {
		std::reverse( lines.begin(), lines.end() );
		return joinDoc( lines );
	}
	if ( l < 0 ) return {};
	std::string_view last = trim( f.line( l ) );
	if ( last.size() < 2 || last.substr( last.size() - 2 ) != "*/" ) return {};
	// Walk up to the line that opens the block.
	for ( int s = l; s >= 0 && l - s < 500; s -= 1 ) {
		std::string_view t = f.line( s );
		size_t open = s == l ? t.rfind( "/*", t.size() - 2 ) : t.rfind( "/*" );
		if ( s != l && t.find( "*/" ) != std::string_view::npos ) return {};	// another comment ends first
		if ( open == std::string_view::npos ) continue;
		if ( !trim( t.substr( 0, open ) ).empty() ) return {};	// code before the comment
		std::string block = f.slice( { { s, int( open ) }, { l, int( f.line( l ).size() ) } } );
		block = std::string( trim( block ) );
		return joinDoc( blockCommentBody( block ) );
	}
	return {};
}

std::string docTrailing( const FileText & f, Loc pos ) {
	std::string_view t = f.line( pos.line );
	char quote = 0;
	int depth = 0;
	bool ended = false;					// passed a ';' or ',' that ends this declarator
	for ( size_t i = std::max( pos.col, 0 ); i < t.size(); i += 1 ) {
		char c = t[i];
		if ( quote ) {
			if ( c == '\\' ) i += 1;
			else if ( c == quote ) quote = 0;
			continue;
		}
		if ( c == '"' || c == '\'' ) { quote = c; continue; }
		if ( c == '(' || c == '[' || c == '{' ) depth += 1;
		else if ( c == ')' || c == ']' || c == '}' ) depth -= 1;
		else if ( ( c == ';' || c == ',' ) && depth <= 0 ) ended = true;
		else if ( ended && isIdentStart( c ) ) return {};	// another declaration owns the comment
		if ( c == '/' && i + 1 < t.size() && t[i + 1] == '/' ) {
			return joinDoc( { lineCommentBody( trim( t.substr( i ) ) ) } );
		}
		if ( c == '/' && i + 1 < t.size() && t[i + 1] == '*' ) {
			size_t e = t.find( "*/", i + 2 );
			if ( e == std::string_view::npos ) return {};
			return joinDoc( blockCommentBody( t.substr( i, e + 2 - i ) ) );
		}
	}
	return {};
}

std::optional<std::pair<int, int>> identifierAt( std::string_view line, int col ) {
	if ( col < 0 || size_t( col ) > line.size() ) return std::nullopt;
	size_t c = col;
	if ( !( c < line.size() && isIdentChar( line[c] ) ) ) {
		if ( c == 0 || !isIdentChar( line[c - 1] ) ) return std::nullopt;
	}
	size_t b = c, e = c;
	while ( b > 0 && isIdentChar( line[b - 1] ) ) b -= 1;
	while ( e < line.size() && isIdentChar( line[e] ) ) e += 1;
	if ( !isIdentStart( line[b] ) ) return std::nullopt;
	return std::pair<int, int>( int( b ), int( e ) );
}

std::vector<std::pair<int, int>> paramSpans( std::string_view sig, std::string_view name ) {
	// Find the '(' that opens the parameter list: the first `name(` outside a
	// forall( ... ) group, at any depth so `void (*signal( int ))( int )` works.
	size_t open = std::string_view::npos, lastTop = std::string_view::npos;
	int depth = 0;
	for ( size_t i = 0; i < sig.size() && open == std::string_view::npos; i += 1 ) {
		char c = sig[i];
		bool wordStart = i == 0 || !isIdentChar( sig[i - 1] );
		if ( wordStart && sig.compare( i, 6, "forall" ) == 0 && ( i + 6 >= sig.size() || !isIdentChar( sig[i + 6] ) ) ) {
			size_t j = sig.find( '(', i );
			int d = 0;
			for ( ; j < sig.size(); j += 1 ) {
				if ( sig[j] == '(' ) d += 1;
				else if ( sig[j] == ')' && --d == 0 ) break;
			}
			if ( j >= sig.size() ) break;
			i = j;
			continue;
		}
		if ( !name.empty() && sig.compare( i, name.size(), name ) == 0 ) {
			bool boundary = !isIdentifier( name ) ||
				( wordStart && ( i + name.size() >= sig.size() || !isIdentChar( sig[i + name.size()] ) ) );
			size_t j = i + name.size();
			while ( j < sig.size() && isSpace( sig[j] ) ) j += 1;
			if ( boundary && j < sig.size() && sig[j] == '(' ) open = j;
		}
		if ( c == '(' || c == '[' || c == '{' ) {
			if ( depth == 0 && c == '(' ) lastTop = i;
			depth += 1;
		} else if ( c == ')' || c == ']' || c == '}' ) {
			depth = std::max( depth - 1, 0 );
		}
	}
	if ( open == std::string_view::npos ) open = lastTop;
	if ( open == std::string_view::npos ) return {};

	std::vector<std::pair<int, int>> spans;
	size_t start = open + 1;
	depth = 0;
	auto add = [&]( size_t b, size_t e ) {
		while ( b < e && isSpace( sig[b] ) ) b += 1;
		while ( e > b && isSpace( sig[e - 1] ) ) e -= 1;
		if ( e > b ) spans.emplace_back( int( b ), int( e ) );
	};
	for ( size_t i = open + 1; i < sig.size(); i += 1 ) {
		char c = sig[i];
		if ( c == '(' || c == '[' || c == '{' ) depth += 1;
		else if ( c == ')' || c == ']' || c == '}' ) {
			if ( depth == 0 ) {
				add( start, i );
				break;
			}
			depth -= 1;
		} else if ( c == ',' && depth == 0 ) {
			add( start, i );
			start = i + 1;
		}
	}
	if ( spans.size() == 1 && trim( sig.substr( spans[0].first, spans[0].second - spans[0].first ) ) == "void" ) spans.clear();
	return spans;
}

std::vector<std::string> identChain( std::string_view expr ) {
	std::string s = stripSpaces( expr );
	std::vector<std::string> out;
	size_t p = 0;
	while ( p <= s.size() ) {
		size_t dot = s.find( '.', p ), arrow = s.find( "->", p );
		size_t e = std::min( dot, arrow );
		std::string part = s.substr( p, e == std::string::npos ? std::string::npos : e - p );
		if ( !isIdentifier( part ) ) return {};
		out.push_back( part );
		if ( e == std::string::npos ) break;
		p = e + ( e == arrow ? 2 : 1 );
	}
	return out;
}

std::vector<std::string> withClauseAtEnd( std::string_view text ) {
	size_t e = text.size();
	while ( e > 0 && isSpace( text[e - 1] ) ) e -= 1;
	if ( e > 0 && text[e - 1] == '{' ) {
		e -= 1;
		while ( e > 0 && isSpace( text[e - 1] ) ) e -= 1;
	}
	if ( e == 0 || text[e - 1] != ')' ) return {};
	size_t close = e - 1, j = close;
	int depth = 0;
	while ( true ) {
		char c = text[j];
		if ( c == ')' ) depth += 1;
		else if ( c == '(' && --depth == 0 ) break;
		if ( j == 0 ) return {};
		j -= 1;
	}
	size_t open = j, k = open;
	while ( k > 0 && isSpace( text[k - 1] ) ) k -= 1;
	size_t we = k;
	while ( k > 0 && isIdentChar( text[k - 1] ) ) k -= 1;
	if ( text.substr( k, we - k ) != "with" ) return {};

	std::vector<std::string> out;
	size_t start = open + 1;
	depth = 0;
	for ( size_t i = open + 1; i <= close; i += 1 ) {
		char c = text[i];
		if ( i == close || ( c == ',' && depth == 0 ) ) {
			std::string part = stripSpaces( text.substr( start, i - start ) );
			if ( !part.empty() ) out.push_back( part );
			start = i + 1;
		} else if ( c == '(' || c == '[' ) depth += 1;
		else if ( c == ')' || c == ']' ) depth -= 1;
	}
	return out;
}

std::optional<CallSite> callBefore( std::string_view t ) {
	struct Open { char c; size_t pos; int commas; };
	std::vector<Open> stack;
	bool lineStart = true;
	for ( size_t i = 0; i < t.size(); i += 1 ) {
		char c = t[i];
		if ( c == '\n' ) { lineStart = true; continue; }
		if ( isSpace( c ) ) continue;
		if ( lineStart && c == '#' ) {
			// Preprocessor line, with backslash continuations.
			while ( i < t.size() && !( t[i] == '\n' && t[i - 1] != '\\' ) ) i += 1;
			if ( i >= t.size() ) return std::nullopt;
			lineStart = true;							// the loop steps over the '\n'
			continue;
		}
		lineStart = false;
		if ( c == '/' && i + 1 < t.size() && t[i + 1] == '/' ) {
			size_t e = t.find( '\n', i );
			if ( e == std::string_view::npos ) return std::nullopt;
			i = e - 1;
			continue;
		}
		if ( c == '/' && i + 1 < t.size() && t[i + 1] == '*' ) {
			size_t e = t.find( "*/", i + 2 );
			if ( e == std::string_view::npos ) return std::nullopt;
			i = e + 1;
			continue;
		}
		if ( c == '"' || c == '\'' ) {
			size_t j = i + 1;
			while ( j < t.size() && t[j] != c && t[j] != '\n' ) j += t[j] == '\\' ? 2 : 1;
			i = std::min( j, t.size() );
			continue;
		}
		switch ( c ) {
		  case '(': case '[': case '{':
			stack.push_back( { c, i, 0 } );
			break;
		  case ')': case ']': case '}': {
			char want = c == ')' ? '(' : c == ']' ? '[' : '{';
			auto it = std::find_if( stack.rbegin(), stack.rend(), [&]( const Open & o ) { return o.c == want; } );
			if ( it != stack.rend() ) stack.erase( std::next( it ).base(), stack.end() );
			break;
		  }
		  case ',':
			if ( !stack.empty() ) stack.back().commas += 1;
			break;
		}
	}

	const Open * call = nullptr;
	for ( auto it = stack.rbegin(); it != stack.rend(); ++it ) {
		if ( it->c == '{' ) return std::nullopt;
		if ( it->c == '(' ) { call = &*it; break; }
	}
	if ( !call ) return std::nullopt;

	size_t e = call->pos;
	while ( e > 0 && isSpace( t[e - 1] ) ) e -= 1;
	size_t b = e;
	if ( b > 0 && isIdentChar( t[b - 1] ) ) {
		while ( b > 0 && isIdentChar( t[b - 1] ) ) b -= 1;
		if ( !isIdentStart( t[b] ) ) return std::nullopt;
		// `?`-suffixed postfix operators such as ?`s are rare; plain identifiers only.
	} else {
		static constexpr std::string_view opChars = "?+-*/%<>=!&|^~[]{}()";
		while ( b > 0 && opChars.find( t[b - 1] ) != std::string_view::npos ) b -= 1;
		if ( std::string_view( t.substr( b, e - b ) ).find( '?' ) == std::string_view::npos ) return std::nullopt;
	}
	if ( b == e ) return std::nullopt;
	return CallSite{ std::string( t.substr( b, e - b ) ), e, call->commas };
}

CompletionContext completionContext( std::string_view lb ) {
	CompletionContext ctx;
	size_t first = lb.find_first_not_of( " \t" );
	if ( first != std::string_view::npos && lb[first] == '#' ) return ctx;

	// No completion inside comments or literals.
	bool inBlock = false;
	char quote = 0;
	for ( size_t i = 0; i < lb.size(); i += 1 ) {
		char c = lb[i], n = i + 1 < lb.size() ? lb[i + 1] : '\0';
		if ( inBlock ) {
			if ( c == '*' && n == '/' ) { inBlock = false; i += 1; }
			continue;
		}
		if ( quote ) {
			if ( c == '\\' ) i += 1;
			else if ( c == quote ) quote = 0;
			continue;
		}
		if ( c == '/' && n == '/' ) return ctx;
		if ( c == '/' && n == '*' ) { inBlock = true; i += 1; continue; }
		if ( c == '"' || c == '\'' ) quote = c;
	}
	if ( inBlock || quote ) return ctx;

	size_t p = lb.size();
	while ( p > 0 && isIdentChar( lb[p - 1] ) ) p -= 1;
	ctx.prefix = std::string( lb.substr( p ) );
	if ( !ctx.prefix.empty() && !isIdentStart( ctx.prefix[0] ) ) return ctx;	// a number

	size_t q = p;
	while ( q > 0 && isSpace( lb[q - 1] ) ) q -= 1;
	size_t op = std::string_view::npos;
	if ( q >= 1 && lb[q - 1] == '.' && !( q >= 2 && lb[q - 2] == '.' ) ) op = q - 1;
	else if ( q >= 2 && lb[q - 1] == '>' && lb[q - 2] == '-' ) op = q - 2;
	if ( op == std::string_view::npos ) {
		ctx.kind = CompletionContext::Ident;
		return ctx;
	}

	// Parse the operand backwards: a chain of names joined by . or ->, each
	// optionally followed by (...) or [...].
	size_t k = op;
	auto skipSpace = [&] { while ( k > 0 && isSpace( lb[k - 1] ) ) k -= 1; };
	skipSpace();
	size_t operandEnd = k;
	std::vector<std::string> names;
	bool plain = true;
	while ( true ) {
		skipSpace();
		bool group = false;
		while ( k > 0 && ( lb[k - 1] == ')' || lb[k - 1] == ']' ) ) {
			int depth = 0;
			size_t j = k;
			while ( j > 0 ) {
				char c = lb[--j];
				if ( c == ')' || c == ']' ) depth += 1;
				else if ( ( c == '(' || c == '[' ) && --depth == 0 ) break;
			}
			if ( depth != 0 ) return ctx;
			k = j;
			group = true;
			skipSpace();
		}
		size_t e = k;
		while ( k > 0 && isIdentChar( lb[k - 1] ) ) k -= 1;
		if ( k == e ) {
			if ( !group ) return ctx;
			plain = false;									// e.g. (*p).
			break;
		}
		if ( !isIdentStart( lb[k] ) ) return ctx;			// 1.
		names.emplace_back( lb.substr( k, e - k ) );
		size_t save = k;
		skipSpace();
		if ( k >= 1 && lb[k - 1] == '.' && !( k >= 2 && lb[k - 2] == '.' ) ) { k -= 1; continue; }
		if ( k >= 2 && lb[k - 1] == '>' && lb[k - 2] == '-' ) { k -= 2; continue; }
		k = save;
		break;
	}
	ctx.kind = CompletionContext::Member;
	if ( plain ) ctx.chain.assign( names.rbegin(), names.rend() );
	ctx.operand = stripSpaces( lb.substr( k, operandEnd - k ) );
	ctx.operandStart = int( k );
	ctx.operandEnd = int( operandEnd );
	return ctx;
}

} // namespace cfalsp::text
