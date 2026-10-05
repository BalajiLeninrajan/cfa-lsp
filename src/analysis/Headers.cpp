#include "Headers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

#include "Lexer.hpp"
#include "TextScan.hpp"

namespace fs = std::filesystem;

namespace cfalsp {

namespace {

bool reserved( const std::string & s ) {
	static const std::unordered_set<std::string> words = {
		// C
		"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
		"float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
		"sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "bool",
		"true", "false", "typeof", "alignof", "asm", "static_assert",
		// Cforall
		"forall", "trait", "with", "coroutine", "monitor", "thread", "generator", "exception", "suspend", "resume",
		"choose", "fallthrough", "fallthru", "catchResume", "throwResume", "fixup", "waitfor", "waituntil", "when",
		"timeout", "mutex", "nomutex", "disable", "enable", "corun", "cofor", "vtable", "virtual", "otype", "dtype",
		"ftype", "ttype", "zero_t", "one_t", "basetypeof", "finally", "throw", "try", "catch", "recover",
	};
	return words.count( s ) > 0;
}

bool aggregateKeyword( const std::string & s ) {
	return s == "struct" || s == "union" || s == "enum" || s == "trait" || s == "coroutine" || s == "monitor" ||
		s == "thread" || s == "generator" || s == "exception";
}

// NAME in "#define NAME ...", or "".
std::string definedName( std::string_view d ) {
	size_t i = d.find_first_not_of( " \t", 1 );
	if ( i == std::string_view::npos || d.compare( i, 6, "define" ) != 0 ) return {};
	i += 6;
	size_t b = d.find_first_not_of( " \t", i );
	if ( b == std::string_view::npos || b == i ) return {};
	size_t e = b;
	while ( e < d.size() && text::isIdentChar( d[e] ) ) e += 1;
	return std::string( d.substr( b, e - b ) );
}

// What a brace opens. Declarations directly inside a transparent block are
// at file scope.
enum class Block { Transparent, Body, Aggregate, Enum };

} // namespace

std::vector<HeaderName> fileScopeNames( std::string_view src ) {
	LexOptions lo;
	lo.directives = true;
	const std::vector<Token> toks = lex( src, lo );
	const int n = int( toks.size() );
	std::vector<HeaderName> out;
	auto add = [&]( const std::string & name, bool type ) {
		if ( !text::isIdentifier( name ) || name[0] == '_' || name.find( '$' ) != std::string::npos || reserved( name ) ) return;
		out.push_back( { name, type } );
	};
	auto is = [&]( int i, const char * s ) { return i >= 0 && i < n && toks[i].kind != TokKind::Directive && toks[i].text == s; };
	auto nextOf = [&]( int i ) {
		i += 1;
		while ( i < n && toks[i].kind == TokKind::Directive ) i += 1;
		return i;
	};
	auto prevOf = [&]( int i ) {
		i -= 1;
		while ( i >= 0 && toks[i].kind == TokKind::Directive ) i -= 1;
		return i;
	};
	// The '(' matching the ')' at i, or -1.
	auto openOf = [&]( int i ) {
		int depth = 0;
		for ( int k = i; k >= 0; k -= 1 ) {
			if ( toks[k].kind != TokKind::Punct ) continue;
			if ( toks[k].text == ")" ) depth += 1;
			else if ( toks[k].text == "(" && --depth == 0 ) return k;
		}
		return -1;
	};
	// Past the ')' matching a '(' at i; i itself if there is no '(' there.
	auto skipParens = [&]( int i ) {
		if ( !is( i, "(" ) ) return i;
		int depth = 0;
		for ( int k = i; k < n; k += 1 ) {
			if ( toks[k].kind != TokKind::Punct ) continue;
			if ( toks[k].text == "(" ) depth += 1;
			else if ( toks[k].text == ")" && --depth == 0 ) return nextOf( k );
		}
		return n;
	};

	struct Level {
		Block kind;
		int parens;							// the enclosing level's parenthesis depth
	};
	std::vector<Level> blocks;
	int parens = 0;							// ( and [ depth in the current block
	bool typedefStmt = false, inInit = false;
	std::string aggregate;					// the aggregate keyword of this statement
	std::vector<bool> named( n, false );	// aggregate names, already handled
	auto reset = [&] {
		typedefStmt = false;
		inInit = false;
		aggregate.clear();
	};
	auto declLevel = [&] {
		return std::all_of( blocks.begin(), blocks.end(), []( const Level & b ) { return b.kind == Block::Transparent; } );
	};
	auto enumLevel = [&] {
		return !blocks.empty() && blocks.back().kind == Block::Enum &&
			std::all_of( blocks.begin(), blocks.end() - 1, []( const Level & b ) { return b.kind == Block::Transparent; } );
	};

	for ( int i = 0; i < n; i += 1 ) {
		const Token & t = toks[i];
		const std::string & s = t.text;
		if ( t.kind == TokKind::Directive ) {
			add( definedName( s ), false );
			continue;
		}
		bool decl = declLevel();
		if ( t.kind == TokKind::Punct ) {
			if ( s == "{" ) {
				Block kind = Block::Body;
				if ( decl && parens == 0 ) {
					int p = prevOf( i );
					if ( is( p, ")" ) && is( prevOf( openOf( p ) ), "forall" ) ) kind = Block::Transparent;		// forall( T ) {
					else if ( p >= 0 && toks[p].kind == TokKind::String && is( prevOf( p ), "extern" ) ) kind = Block::Transparent;	// extern "C" {
					else if ( aggregate == "enum" ) kind = Block::Enum;
					else if ( !aggregate.empty() ) kind = Block::Aggregate;
				}
				blocks.push_back( { kind, parens } );
				parens = 0;
			} else if ( s == "}" ) {
				if ( !blocks.empty() ) {
					Level b = blocks.back();
					blocks.pop_back();
					parens = b.parens;
					// The end of a function body or a block of declarations ends the statement.
					if ( declLevel() && parens == 0 && ( b.kind == Block::Transparent || ( b.kind == Block::Body && !inInit ) ) ) reset();
				}
			} else if ( decl || enumLevel() ) {
				if ( s == "(" || s == "[" || s == "@[" ) parens += 1;
				else if ( s == ")" || s == "]" ) parens = std::max( 0, parens - 1 );
				else if ( decl && parens == 0 && s == ";" ) reset();
				else if ( decl && parens == 0 && s == "=" ) inInit = true;
				else if ( decl && parens == 0 && s == "," ) inInit = false;
			}
			continue;
		}
		if ( t.kind != TokKind::Identifier ) continue;

		if ( enumLevel() ) {
			int p = prevOf( i );
			if ( parens == 0 && ( is( p, "{" ) || is( p, "," ) ) ) add( s, false );		// an enumerator
			continue;
		}
		if ( !decl || parens > 0 ) continue;
		if ( s == "typedef" ) {
			typedefStmt = true;
			continue;
		}
		if ( aggregateKeyword( s ) ) {
			// struct S {, enum( int ) E {, trait sized( T ) {: a definition. struct S * p; is a use.
			aggregate = s;
			int k = skipParens( nextOf( i ) );
			if ( k < n && toks[k].kind == TokKind::Identifier && !reserved( toks[k].text ) ) {
				if ( is( skipParens( nextOf( k ) ), "{" ) ) add( toks[k].text, true );
				named[k] = true;
			}
			continue;
		}
		if ( named[i] || reserved( s ) || inInit ) continue;
		int k = nextOf( i );
		if ( is( k, "(" ) ) add( s, false );												// a function
		else if ( is( k, ";" ) || is( k, "," ) || is( k, "=" ) || is( k, "[" ) ) add( s, typedefStmt );	// a variable or typedef
	}
	return out;
}

HeaderIndex HeaderIndex::scan( const std::string & includeDir ) {
	HeaderIndex index;
	if ( includeDir.empty() ) return index;
	std::set<std::string> seen;
	for ( const char * sub : { "", "/concurrency", "/collections" } ) {
		std::vector<fs::path> files;
		std::error_code ec;
		for ( fs::directory_iterator it( includeDir + sub, ec ), end; !ec && it != end; it.increment( ec ) ) {
			if ( it->path().extension() == ".hfa" ) files.push_back( it->path() );
		}
		std::sort( files.begin(), files.end() );
		for ( const fs::path & f : files ) {
			std::string name = f.filename().string();
			if ( !seen.insert( name ).second ) continue;
			std::ifstream in( f, std::ios::binary );
			if ( !in ) continue;
			std::ostringstream ss;
			ss << in.rdbuf();
			index.add( name, ss.str() );
		}
	}
	return index;
}

void HeaderIndex::add( const std::string & header, std::string_view text ) {
	for ( const HeaderName & n : fileScopeNames( text ) ) {
		auto & entries = names[n.name];
		auto it = std::find_if( entries.begin(), entries.end(), [&]( const Entry & e ) { return e.header == header; } );
		if ( it == entries.end() ) entries.push_back( { header, n.type } );
		else it->type = it->type || n.type;
	}
}

std::vector<std::string> HeaderIndex::headersFor( const std::string & name, bool typesOnly ) const {
	std::vector<std::string> out;
	auto it = names.find( name );
	if ( it == names.end() ) return out;
	for ( const Entry & e : it->second ) {
		if ( !typesOnly || e.type ) out.push_back( e.header );
	}
	std::sort( out.begin(), out.end() );
	return out;
}

} // namespace cfalsp
