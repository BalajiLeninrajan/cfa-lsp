// SourceMap against real `cfa -E` output. Every test skips itself when cfa
// is not on PATH.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "Lexer.hpp"
#include "SourceMap.hpp"

using namespace cfalsp;

namespace cfalsp {
[[maybe_unused]] static std::ostream & operator<<( std::ostream & os, const Loc & l ) { return os << l.line << ":" << l.col; }
[[maybe_unused]] static std::ostream & operator<<( std::ostream & os, const Range & r ) { return os << r.start << "-" << r.end; }
} // namespace cfalsp
namespace fs = std::filesystem;

namespace {

bool haveCfa() {
	static const bool have = std::system( "command -v cfa >/dev/null 2>&1" ) == 0;
	return have;
}

std::string slurp( const fs::path & p ) {
	std::ifstream in( p, std::ios::binary );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void spit( const fs::path & p, const std::string & text ) {
	fs::create_directories( p.parent_path() );
	std::ofstream( p, std::ios::binary ) << text;
}

struct TempDir {
	fs::path path;
	TempDir() {
		std::string tmpl = ( fs::temp_directory_path() / "cfa-lsp-sourcemap-XXXXXX" ).string();
		if ( mkdtemp( tmpl.data() ) ) path = tmpl;
	}
	~TempDir() {
		std::error_code ec;
		if ( !path.empty() ) fs::remove_all( path, ec );
	}
};

// Preprocesses `text` as the server does: a copy in another directory whose
// first line is a marker for the real path, with -I for the real directory.
std::optional<std::string> preprocess( const fs::path & real, const std::string & text, const fs::path & work ) {
	fs::path buf = work / "buffer" / "buffer.cfa";
	spit( buf, "# 1 \"" + real.string() + "\"\n" + text );
	fs::path out = work / "buffer" / "out.i", err = work / "buffer" / "err.txt";
	std::string cmd = "cfa -E -I'" + real.parent_path().string() + "' '" + buf.string() + "' > '" + out.string() +
					  "' 2> '" + err.string() + "'";
	if ( std::system( cmd.c_str() ) != 0 ) {
		FAIL_CHECK( "cfa -E failed: " << slurp( err ) );
		return std::nullopt;
	}
	return slurp( out );
}

SourceMap::Reader diskReader( std::shared_ptr<std::set<std::string>> reads = nullptr ) {
	return [reads]( const std::string & path ) -> std::optional<std::string> {
		if ( reads ) reads->insert( path );
		std::ifstream in( path, std::ios::binary );
		if ( !in ) return std::nullopt;
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	};
}

// The first preprocessed line for each 1-based line of `file`, found from the
// line markers independently of SourceMap.
std::map<int, std::string> prepLines( const std::string & prep, const std::string & file ) {
	std::map<int, std::string> out;
	std::istringstream in( prep );
	std::string l, cur;
	int next = 0;
	while ( std::getline( in, l ) ) {
		if ( !l.empty() && l[0] == '#' ) {
			std::istringstream ls( l.substr( 1 ) );
			int n;
			std::string name;
			if ( ls >> n ) {
				if ( ls >> name && name.size() >= 2 ) cur = name.substr( 1, name.size() - 2 );
				next = n;
			} else {
				++next;
			}
			continue;
		}
		if ( cur == file && l.find_first_not_of( ' ' ) != std::string::npos ) out.emplace( next, l );
		++next;
	}
	return out;
}

std::string markerEndingWith( const std::string & prep, const std::string & suffix ) {
	std::istringstream in( prep );
	std::string l;
	while ( std::getline( in, l ) ) {
		size_t q = l.rfind( suffix + "\"" );
		if ( l.size() > 2 && l[0] == '#' && q != std::string::npos ) {
			size_t b = l.find( '"' );
			return l.substr( b + 1, q + suffix.size() - b - 1 );
		}
	}
	return "";
}

// For every original line whose tokens are the same as its preprocessed
// line's, checks each token's start, end and an offset inside it. Returns the
// 1-based lines whose tokens differ (macro uses and their continuation lines).
std::set<int> checkTokens( const SourceMap & m, const std::string & file, const std::string & orig,
						   const std::string & prep, int * checked = nullptr ) {
	std::map<int, std::vector<Token>> byLine;
	for ( Token & t : lex( orig ) ) byLine[t.line + 1].push_back( std::move( t ) );
	std::set<int> differ;
	int n = 0;
	for ( auto & [line, text] : prepLines( prep, file ) ) {
		std::vector<Token> p = lex( text );
		const std::vector<Token> & o = byLine[line];
		bool same = p.size() == o.size();
		for ( size_t k = 0; same && k < p.size(); ++k ) same = p[k].text == o[k].text;
		if ( !same ) {
			differ.insert( line );
			continue;
		}
		for ( size_t k = 0; k < p.size(); ++k ) {
			CAPTURE( file );
			CAPTURE( line );
			CAPTURE( p[k].text );
			CHECK( m.map( file, line, p[k].col ) == Loc{ o[k].line, o[k].col } );
			CHECK( m.map( file, line, p[k].endCol, true ) == Loc{ o[k].endLine, o[k].endCol } );
			if ( p[k].endCol - p[k].col > 1 && o[k].line == o[k].endLine ) {
				CHECK( m.map( file, line, p[k].col + 1 ) == Loc{ o[k].line, o[k].col + 1 } );
			}
			++n;
		}
	}
	if ( checked ) *checked = n;
	return differ;
}

bool wordChar( char c ) { return std::isalnum( (unsigned char)c ) || c == '_'; }

// The nth occurrence of needle that does not start or end inside a word.
size_t findWord( const std::string & s, const std::string & needle, int nth ) {
	for ( size_t p = s.find( needle ); p != std::string::npos; p = s.find( needle, p + 1 ) ) {
		if ( wordChar( needle.front() ) && p > 0 && wordChar( s[p - 1] ) ) continue;
		size_t e = p + needle.size();
		if ( wordChar( needle.back() ) && e < s.size() && wordChar( s[e] ) ) continue;
		if ( nth-- == 0 ) return p;
	}
	return std::string::npos;
}

// 0-based position of the nth occurrence of `needle` on 1-based `line`.
Loc origAt( const std::string & text, int line, const std::string & needle, int nth = 0 ) {
	std::istringstream in( text );
	std::string l;
	for ( int i = 1; std::getline( in, l ); ++i ) {
		if ( i != line ) continue;
		size_t p = findWord( l, needle, nth );
		REQUIRE( p != std::string::npos );
		return { line - 1, (int)p };
	}
	FAIL( "no line " << line );
	return {};
}

int prepCol( const std::map<int, std::string> & lines, int line, const std::string & needle, int nth = 0 ) {
	auto it = lines.find( line );
	REQUIRE( it != lines.end() );
	size_t p = findWord( it->second, needle, nth );
	REQUIRE( p != std::string::npos );
	return (int)p;
}

Loc plus( Loc l, int n ) { return { l.line, l.col + n }; }

struct Case {
	TempDir dir;
	fs::path real;
	std::string text, prep;
	bool ok = false;

	Case( const std::string & name, const std::string & src ) : text( src ) {
		if ( !haveCfa() || dir.path.empty() ) return;
		real = dir.path / "src" / name;
		spit( real, text );
		auto p = preprocess( real, text, dir.path );
		if ( !p ) return;
		prep = *p;
		ok = true;
	}
};

} // namespace

TEST_CASE( "sourcemap/cfa: whitespace, comments, strings, splices" ) {
	if ( !haveCfa() ) { MESSAGE( "cfa not on PATH; skipping" ); return; }
	Case c( "ws.cfa",
			"int\ta1 =   1;\t\t// trailing comment\n"
			"/* lead */ int a2 /* mid */ = 2;\n"
			"    \t  int a3 = 3; /* block\n"
			"   spanning */ int a4 = 4;\n"
			"char * s1 = \"x  /* not a comment */  y\";\n"
			"char * s2 = \"tab\\there // nope\";\n"
			"char c1 = '\"';   char c2 = '\\'';\n"
			"int a5 = 1 +\n"
			"\t2 +\n"
			"\t\t3;\n"
			"int lo\\\n"
			"ng1 = 5;\n"
			"const char * s3 = \"abc\\\n"
			"def\";\n"
			"  int a6 = sizeof( u8\"x\" ) + sizeof( L'y' ) + 0x1fUL + (int)1.5e-3f;\n"
			"// int dead;\n"
			"/*\n"
			"int dead2;\n"
			"*/\n"
			"int\t\t\t\t\ta7\t=\t7;\n" );
	REQUIRE( c.ok );
	SourceMap m( c.prep, diskReader() );
	int n = 0;
	// The ";" after the spliced string has no space before it, so cpp prints it
	// on line 13 with the string.
	CHECK( checkTokens( m, c.real.string(), c.text, c.prep, &n ) == std::set<int>{ 13 } );
	CHECK( n > 80 );
	auto P = prepLines( c.prep, c.real.string() );
	CHECK( m.map( c.real.string(), 13, prepCol( P, 13, "\"abcdef\"" ) ) == origAt( c.text, 13, "\"abc" ) );
	CHECK( m.map( c.real.string(), 13, prepCol( P, 13, "\"abcdef\"" ) + 8, true ) == Loc{ 13, 4 } );
	CHECK( m.map( c.real.string(), 13, prepCol( P, 13, ";" ) ) == Loc{ 13, 4 } );
	CHECK( m.map( c.real.string(), 13, prepCol( P, 13, "\"abcdef\"" ) + 5 ) == Loc{ 13, 1 } );
}

TEST_CASE( "sourcemap/cfa: macros" ) {
	if ( !haveCfa() ) { MESSAGE( "cfa not on PATH; skipping" ); return; }
	Case c( "macros.cfa",
			"#define N    10\n"						// 1
			"#define ADD(a, b) \\\n"				// 2
			"\t((a) + (b))\n"						// 3
			"#define TWICE(x) ADD(x, x)\n"			// 4
			"#define EMPTY\n"						// 5
			"#define CALL(f, x) f(x)\n"				// 6
			"int sq( int v ) { return v * v; }\n"	// 7
			"int m1 = N;\n"							// 8
			"int m2 = ADD( m1, 2 ) + TWICE( m1 );\n"	// 9
			"int m3 = ADD( m1,\n"					// 10
			"\t\tm2 ) + m1;\n"						// 11
			"EMPTY int m4 = CALL( sq, m3 );\n"		// 12
			"int m5 = TWICE\n"						// 13
			"\t( 4 ) + 1;\n"						// 14
			"int m6 = sq( N ) + sq( m5 );\n"			// 15
			"#if 0\n"								// 16
			"int dead = ADD( 1,\n"					// 17
			"#else\n"								// 18
			"int live = ADD( 1, 2 );\n"				// 19
			"#endif\n" );							// 20
	REQUIRE( c.ok );
	const std::string F = c.real.string();
	SourceMap m( c.prep, diskReader() );
	CHECK( checkTokens( m, F, c.text, c.prep ) == std::set<int>{ 8, 9, 10, 11, 12, 13, 14, 15, 19 } );
	auto P = prepLines( c.prep, F );
	const std::string & T = c.text;

	// int m1 = N;
	CHECK( m.map( F, 8, prepCol( P, 8, "m1" ) ) == origAt( T, 8, "m1" ) );
	CHECK( m.map( F, 8, prepCol( P, 8, "10" ) ) == origAt( T, 8, "N" ) );
	CHECK( m.map( F, 8, prepCol( P, 8, "10" ) + 2, true ) == plus( origAt( T, 8, "N" ), 1 ) );
	CHECK( m.map( F, 8, prepCol( P, 8, ";" ) ) == origAt( T, 8, ";" ) );

	// int m2 = ((m1) + (2)) + ((m1) + (m1));
	Loc add = origAt( T, 9, "ADD" ), addEnd = plus( origAt( T, 9, ") +" ), 1 );
	Loc twice = origAt( T, 9, "TWICE" ), twiceEnd = plus( origAt( T, 9, ");" ), 1 );
	CHECK( m.map( F, 9, prepCol( P, 9, "((" ) ) == add );
	CHECK( m.map( F, 9, prepCol( P, 9, "m1" ) ) == origAt( T, 9, "m1" ) );	// argument
	CHECK( m.map( F, 9, prepCol( P, 9, "2" ) ) == add );
	CHECK( m.map( F, 9, prepCol( P, 9, "2" ) + 1, true ) == addEnd );
	CHECK( m.map( F, 9, prepCol( P, 9, "+", 1 ) ) == origAt( T, 9, "+" ) );	// between the two uses
	CHECK( m.map( F, 9, prepCol( P, 9, "((", 1 ) ) == twice );
	CHECK( m.map( F, 9, prepCol( P, 9, "m1", 1 ) ) == origAt( T, 9, "m1", 1 ) );
	CHECK( m.map( F, 9, prepCol( P, 9, "m1", 2 ) ) == origAt( T, 9, "m1", 1 ) );
	CHECK( m.map( F, 9, prepCol( P, 9, ";" ) - 1, true ) == twiceEnd );
	CHECK( m.mapRange( F, 9, prepCol( P, 9, "((" ), 9, prepCol( P, 9, ";" ) ) == Range{ add, twiceEnd } );

	// A use spanning lines 10-11 comes out on line 10; "+ m1;" stays on 11.
	Loc add3 = origAt( T, 10, "ADD" ), add3End = plus( origAt( T, 11, ")" ), 1 );
	CHECK( m.map( F, 10, prepCol( P, 10, "((" ) ) == add3 );
	CHECK( m.map( F, 10, prepCol( P, 10, "m2" ) ) == origAt( T, 11, "m2" ) );
	CHECK( m.map( F, 10, prepCol( P, 10, "m2" ) + 2, true ) == plus( origAt( T, 11, "m2" ), 2 ) );
	CHECK( m.map( F, 10, (int)P[10].size(), true ) == add3End );
	CHECK( m.map( F, 11, prepCol( P, 11, "+" ) ) == origAt( T, 11, "+" ) );
	CHECK( m.map( F, 11, prepCol( P, 11, "m1" ) ) == origAt( T, 11, "m1" ) );

	// EMPTY int m4 = sq(m3);
	CHECK( m.map( F, 12, prepCol( P, 12, "int" ) ) == origAt( T, 12, "int" ) );
	CHECK( m.map( F, 12, prepCol( P, 12, "sq" ) ) == origAt( T, 12, "sq" ) );
	CHECK( m.map( F, 12, prepCol( P, 12, "m3" ) ) == origAt( T, 12, "m3" ) );
	CHECK( m.map( F, 12, prepCol( P, 12, "(" ) ) == origAt( T, 12, "CALL" ) );

	// TWICE on line 13 with its arguments on line 14.
	CHECK( m.map( F, 13, prepCol( P, 13, "4" ) ) == origAt( T, 13, "TWICE" ) );
	CHECK( m.map( F, 13, prepCol( P, 13, "4" ) + 1, true ) == plus( origAt( T, 14, ")" ), 1 ) );
	CHECK( m.map( F, 14, prepCol( P, 14, "+" ) ) == origAt( T, 14, "+" ) );
	CHECK( m.map( F, 14, prepCol( P, 14, "1" ) ) == origAt( T, 14, "1" ) );

	// int m6 = sq( 10 ) + sq( m5 );
	CHECK( m.map( F, 15, prepCol( P, 15, "10" ) ) == origAt( T, 15, "N" ) );
	CHECK( m.map( F, 15, prepCol( P, 15, "sq", 1 ) ) == origAt( T, 15, "sq", 1 ) );
	CHECK( m.map( F, 15, prepCol( P, 15, "m5" ) ) == origAt( T, 15, "m5" ) );

	// Dead #if code before a live line.
	CHECK( m.map( F, 19, prepCol( P, 19, "live" ) ) == origAt( T, 19, "live" ) );
	CHECK( m.map( F, 19, prepCol( P, 19, ";" ) ) == origAt( T, 19, ";" ) );
}

TEST_CASE( "sourcemap/cfa: CFA syntax" ) {
	if ( !haveCfa() ) { MESSAGE( "cfa not on PATH; skipping" ); return; }
	Case c( "cfa.cfa",
			"#include <fstream.hfa>\n"
			"struct S { int i; };\n"
			"void ?{}( S & s ) { s.i = 0; }\n"
			"void ?{}( S & s, int v ) { s.i = v; }\n"
			"void ^?{}( S & s ) {}\n"
			"S ?+?( S a, S b ) { S r; r.i = a.i + b.i; return r; }\n"
			"forall( T | { T ?+?( T, T ); } )\n"
			"T twice( T a ) { return a + a; }\n"
			"void use( S & s ) with( s ) {\n"
			"\ti += 1;\n"
			"\tfor ( k; 3 ) { i += k; }\n"
			"\tfor ( j;   0 ~ 10 ~ 2 ) { i -= j; }\n"
			"\tint & r = i;\n"
			"\t&r = &s.i;\n"
			"\tS t @= { 4 };\n"
			"\t[ int, int ] tup = [ 1, 2 ];\n"
			"\tsout | i | t.i | nl;\n"
			"}\n" );
	REQUIRE( c.ok );
	SourceMap m( c.prep, diskReader() );
	int n = 0;
	auto differ = checkTokens( m, c.real.string(), c.text, c.prep, &n );
	for ( int l : differ ) MESSAGE( "differs: line " << l );
	CHECK( differ.empty() );
	CHECK( n > 150 );
}

TEST_CASE( "sourcemap/cfa: local include and libcfa headers" ) {
	if ( !haveCfa() ) { MESSAGE( "cfa not on PATH; skipping" ); return; }
	const char * env = std::getenv( "CFA_LSP_FIXTURES" );
	fs::path dir = fs::absolute( fs::path( env ? env : "tests/fixtures" ) / "sourcemap" );
	fs::path real = dir / "include.cfa";
	REQUIRE( fs::exists( real ) );
	TempDir work;
	REQUIRE( !work.path.empty() );
	std::string text = slurp( real );
	auto prep = preprocess( real, text, work.path );
	REQUIRE( prep );

	std::string local = markerEndingWith( *prep, "/local.hfa" );
	std::string fstream = markerEndingWith( *prep, "/fstream.hfa" );
	REQUIRE( !local.empty() );
	REQUIRE( !fstream.empty() );

	auto reads = std::make_shared<std::set<std::string>>();
	auto t0 = std::chrono::steady_clock::now();
	SourceMap m( *prep, diskReader( reads ) );
	CHECK( checkTokens( m, real.string(), text, *prep ) == std::set<int>{ 5 } );
	CHECK( checkTokens( m, local, slurp( local ), *prep ) == std::set<int>{ 11 } );
	int n = 0;
	checkTokens( m, fstream, slurp( fstream ), *prep, &n );
	CHECK( n > 500 );
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
	MESSAGE( "preprocessed lines: " << std::count( prep->begin(), prep->end(), '\n' ) << ", " << n
			 << " fstream.hfa tokens checked, " << ms << " ms" );
	CHECK( ms < 2000 );
	// Only the three queried files were read.
	CHECK( *reads == std::set<std::string>{ real.string(), local, fstream } );

	auto P = prepLines( *prep, local );
	Loc scale = origAt( slurp( local ), 11, "SCALE" );
	CHECK( m.map( local, 11, prepCol( P, 11, "( (" ) ) == scale );
	CHECK( m.map( local, 11, prepCol( P, 11, "p.x" ) ) == origAt( slurp( local ), 11, "p.x" ) );
	CHECK( m.map( local, 11, prepCol( P, 11, "3" ) ) == scale );
	CHECK( m.map( local, 11, prepCol( P, 11, "p.y" ) ) == origAt( slurp( local ), 11, "p.y" ) );
}

namespace {

// Every preprocessed line for 1-based `line` of `file`, in order.
std::vector<std::string> allPrepLines( const std::string & prep, const std::string & file, int line ) {
	std::vector<std::string> out;
	std::istringstream in( prep );
	std::string l, cur;
	int next = 0;
	while ( std::getline( in, l ) ) {
		if ( !l.empty() && l[0] == '#' ) {
			std::istringstream ls( l.substr( 1 ) );
			int n;
			std::string name;
			if ( ls >> n ) {
				if ( ls >> name && name.size() >= 2 ) cur = name.substr( 1, name.size() - 2 );
				next = n;
			} else {
				++next;
			}
			continue;
		}
		if ( cur == file && next == line ) out.push_back( l );
		++next;
	}
	return out;
}

} // namespace

TEST_CASE( "sourcemap/cfa: assert and ctype macros split lines" ) {
	if ( !haveCfa() ) { MESSAGE( "cfa not on PATH; skipping" ); return; }
	Case c( "split.cfa",
			"#include <assert.h>\n"
			"#include <ctype.h>\n"
			"int f( int ch ) {\n"
			"\tassert( ch > 0 );\n"
			"\tif ( ! isdigit( ch ) || ch == '0' ) return 1;\n"
			"\treturn 0;\n"
			"}\n" );
	REQUIRE( c.ok );
	const std::string F = c.real.string();
	const std::string & T = c.text;
	SourceMap m( c.prep, diskReader() );

	auto subs4 = allPrepLines( c.prep, F, 4 ), subs5 = allPrepLines( c.prep, F, 5 );
	REQUIRE( subs4.size() > 1 );		// if cpp stops splitting, this test needs a new example
	REQUIRE( subs5.size() > 1 );

	int col = (int)findWord( subs4.front(), "ch", 0 );
	CHECK( m.mapRange( F, 4, col, 4, col + 2 ) == Range{ origAt( T, 4, "ch" ), plus( origAt( T, 4, "ch" ), 2 ) } );
	col = (int)subs4.back().find( ';' );
	CHECK( m.map( F, 4, col ) == origAt( T, 4, ";" ) );

	bool found = false;
	for ( const std::string & s : subs5 ) {
		size_t b = s.find_first_not_of( ' ' );
		if ( b == std::string::npos || s.substr( b, 2 ) != "ch" || s.find_first_not_of( ' ', b + 2 ) != std::string::npos ) continue;
		found = true;
		CHECK( m.mapRange( F, 5, (int)b, 5, (int)b + 2 ) == Range{ origAt( T, 5, "ch" ), plus( origAt( T, 5, "ch" ), 2 ) } );
	}
	CHECK( found );
	const std::string & last = subs5.back();
	CHECK( m.map( F, 5, (int)last.find( "||" ) ) == origAt( T, 5, "||" ) );
	col = (int)findWord( last, "ch", 0 );
	CHECK( m.mapRange( F, 5, col, 5, col + 2 ) == Range{ origAt( T, 5, "ch", 1 ), plus( origAt( T, 5, "ch", 1 ), 2 ) } );
	CHECK( m.map( F, 6, (int)findWord( allPrepLines( c.prep, F, 6 ).front(), "return", 0 ) ) == origAt( T, 6, "return" ) );
}
