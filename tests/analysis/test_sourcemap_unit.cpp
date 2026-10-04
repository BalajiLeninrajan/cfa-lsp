// SourceMap on hand-written preprocessed text; no cfa needed.
#include <doctest/doctest.h>

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "SourceMap.hpp"

using namespace cfalsp;

namespace cfalsp {
[[maybe_unused]] static std::ostream & operator<<( std::ostream & os, const Loc & l ) { return os << l.line << ":" << l.col; }
[[maybe_unused]] static std::ostream & operator<<( std::ostream & os, const Range & r ) { return os << r.start << "-" << r.end; }
} // namespace cfalsp

namespace {

struct Files {
	std::map<std::string, std::string> text;
	std::shared_ptr<std::multiset<std::string>> reads = std::make_shared<std::multiset<std::string>>();

	SourceMap::Reader reader() const {
		auto t = text;
		auto r = reads;
		return [t, r]( const std::string & path ) -> std::optional<std::string> {
			r->insert( path );
			auto it = t.find( path );
			if ( it == t.end() ) return std::nullopt;
			return it->second;
		};
	}
};

Loc L( int line, int col ) { return { line, col }; }

} // namespace

TEST_CASE( "sourcemap: identity" ) {
	SourceMap m = SourceMap::identity();
	CHECK( m.map( "x", 3, 7 ) == L( 2, 7 ) );
	CHECK( m.map( "x", 0, -4 ) == L( 0, 0 ) );
	CHECK( m.mapRange( "x", 2, 5, 2, 9 ) == Range{ L( 1, 5 ), L( 1, 9 ) } );
	CHECK( m.mapRange( "x", 2, 5, 1, 0 ).end == L( 1, 5 ) );
}

TEST_CASE( "sourcemap: whitespace, comments and line markers" ) {
	Files f;
	f.text["/r/main.cfa"] =
		"#include \"h.hfa\"\n"
		"/* lead */\tint   x =\t N;  // trailing\n"
		"int z = x;\n";
	f.text["/r/h.hfa"] = "#define N 10\nint  y;\n";
	std::string prep =
		"# 0 \"/tmp/t.cfa\"\n"
		"# 0 \"<built-in>\"\n"
		"# 1 \"/tmp/t.cfa\"\n"
		"# 1 \"/r/main.cfa\"\n"
		"# 1 \"/r/h.hfa\" 1\n"
		"\n"
		"int y;\n"
		"# 2 \"/r/main.cfa\" 2\n"
		"           int x = 10;\n"
		"int z = x;\n";
	SourceMap m( prep, f.reader() );

	// main.cfa line 2: "           int x = 10;"
	CHECK( m.map( "/r/main.cfa", 2, 11 ) == L( 1, 11 ) );		// int
	CHECK( m.map( "/r/main.cfa", 2, 14, true ) == L( 1, 14 ) );
	CHECK( m.map( "/r/main.cfa", 2, 15 ) == L( 1, 17 ) );		// x
	CHECK( m.map( "/r/main.cfa", 2, 16, true ) == L( 1, 18 ) );
	CHECK( m.map( "/r/main.cfa", 2, 17 ) == L( 1, 19 ) );		// =
	CHECK( m.map( "/r/main.cfa", 2, 19 ) == L( 1, 22 ) );		// 10 -> N
	CHECK( m.map( "/r/main.cfa", 2, 21, true ) == L( 1, 23 ) );
	CHECK( m.map( "/r/main.cfa", 2, 20 ) == L( 1, 22 ) );		// mid-token in a macro: start
	CHECK( m.map( "/r/main.cfa", 2, 20, true ) == L( 1, 23 ) );	// and end of the invocation
	CHECK( m.map( "/r/main.cfa", 2, 12 ) == L( 1, 12 ) );		// mid-token, exact: keeps offset
	CHECK( m.map( "/r/main.cfa", 2, 21 ) == L( 1, 23 ) );		// ;
	CHECK( m.mapRange( "/r/main.cfa", 2, 11, 2, 22 ) == Range{ L( 1, 11 ), L( 1, 24 ) } );
	// Whitespace and the padding before the first token.
	CHECK( m.map( "/r/main.cfa", 2, 0 ) == L( 1, 11 ) );
	CHECK( m.map( "/r/main.cfa", 2, 0, true ) == L( 1, 11 ) );
	CHECK( m.map( "/r/main.cfa", 2, 14 ) == L( 1, 17 ) );		// space before x -> start of x
	CHECK( m.map( "/r/main.cfa", 2, 15, true ) == L( 1, 14 ) );	// end right after int
	// Past the end of the line.
	CHECK( m.map( "/r/main.cfa", 2, 400 ) == L( 1, 24 ) );
	CHECK( m.map( "/r/main.cfa", 3, 4 ) == L( 2, 4 ) );

	CHECK( m.map( "/r/h.hfa", 2, 4 ) == L( 1, 5 ) );			// y
	CHECK( m.map( "/r/h.hfa", 2, 0 ) == L( 1, 0 ) );

	// Lines without tokens and lines past the end clamp.
	CHECK( m.map( "/r/main.cfa", 1, 50 ) == L( 0, 16 ) );
	CHECK( m.map( "/r/main.cfa", 99, 3 ) == L( 3, 0 ) );
	CHECK( m.map( "/r/main.cfa", -5, -5 ) == L( 0, 0 ) );
	CHECK( m.map( "/r/main.cfa", 2, -5 ) == L( 1, 11 ) );
	// Unknown files fall back to identity.
	CHECK( m.map( "/r/other.cfa", 4, 2 ) == L( 3, 2 ) );
	// An empty range stays empty.
	CHECK( m.mapRange( "/r/main.cfa", 2, 15, 2, 15 ) == Range{ L( 1, 17 ), L( 1, 17 ) } );
}

TEST_CASE( "sourcemap: lazy reads" ) {
	Files f;
	f.text["/r/a.cfa"] = "int a;\n";
	f.text["/r/b.hfa"] = "int b;\n";
	std::string prep = "# 1 \"/r/a.cfa\"\nint a;\n# 1 \"/r/b.hfa\" 1 3 4\nint b;\n# 2 \"/r/a.cfa\" 2\n";
	SourceMap m( prep, f.reader() );
	CHECK( f.reads->empty() );
	CHECK( m.map( "/r/a.cfa", 1, 4 ) == L( 0, 4 ) );
	CHECK( m.map( "/r/a.cfa", 1, 5, true ) == L( 0, 5 ) );
	CHECK( f.reads->count( "/r/a.cfa" ) == 1 );
	CHECK( f.reads->count( "/r/b.hfa" ) == 0 );
	CHECK( m.map( "/r/b.hfa", 1, 4 ) == L( 0, 4 ) );
	CHECK( f.reads->count( "/r/b.hfa" ) == 1 );
	CHECK( f.reads->size() == 2 );
}

TEST_CASE( "sourcemap: unreadable file falls back to identity" ) {
	Files f;
	SourceMap m( "# 1 \"/r/gone.cfa\"\nint  a;\n", f.reader() );
	CHECK( m.map( "/r/gone.cfa", 1, 5 ) == L( 0, 5 ) );
	SourceMap n( "# 1 \"/r/gone.cfa\"\nint  a;\n", nullptr );
	CHECK( n.map( "/r/gone.cfa", 1, 5 ) == L( 0, 5 ) );
	SourceMap t( "# 1 \"/r/x.cfa\"\nint a;\n", []( const std::string & ) -> std::optional<std::string> {
		throw std::runtime_error( "boom" );
	} );
	CHECK( t.map( "/r/x.cfa", 1, 4 ) == L( 0, 4 ) );
}

TEST_CASE( "sourcemap: pragmas, #line, escaped names" ) {
	Files f;
	f.text["/r/p.c"] = "#pragma GCC diagnostic push\nint   a;\n#pragma weak foo\n\n\nint\tb;\n";
	f.text["/r/we\"ird\\.c"] = "int  q;\n";
	std::string prep =
		"# 1 \"/r/p.c\"\n"
		"#pragma GCC diagnostic push\n"
		"int a;\n"
		"#pragma weak foo\n"
		"#line 6 \"/r/p.c\"\n"
		"int b;\n"
		"# 1 \"/r/we\\\"ird\\\\.c\"\n"
		"int q;\n";
	SourceMap m( prep, f.reader() );
	CHECK( m.map( "/r/p.c", 2, 4 ) == L( 1, 6 ) );
	CHECK( m.map( "/r/p.c", 6, 4 ) == L( 5, 4 ) );
	CHECK( m.map( "/r/we\"ird\\.c", 1, 4 ) == L( 0, 5 ) );
}

TEST_CASE( "sourcemap: header included twice" ) {
	Files f;
	f.text["/r/m.c"] = "#include \"t.h\"\n#define T 1\n#include \"t.h\"\n";
	f.text["/r/t.h"] = "int  v = T;\n";
	std::string prep =
		"# 1 \"/r/m.c\"\n"
		"# 1 \"/r/t.h\" 1\n"
		"int v = T;\n"
		"# 2 \"/r/m.c\" 2\n"
		"\n"
		"# 1 \"/r/t.h\" 1\n"
		"int v = 1;\n"
		"# 4 \"/r/m.c\" 2\n";
	SourceMap m( prep, f.reader() );
	// The first copy is used.
	CHECK( m.map( "/r/t.h", 1, 4 ) == L( 0, 5 ) );
	CHECK( m.map( "/r/t.h", 1, 8 ) == L( 0, 9 ) );
}

TEST_CASE( "sourcemap: macro expansions" ) {
	Files f;
	f.text["/r/m.cfa"] =
		"#define ADD(a, b) ((a) + (b))\n"
		"#define EMPTY\n"
		"#define SWAP(a, b) b a\n"
		"int r = ADD( x,\n"
		"   y ) + z;\n"
		"EMPTY int  w = f( 1 );\n"
		"int s = SWAP( p, q );\n";
	std::string prep =
		"# 1 \"/r/m.cfa\"\n"
		"\n\n\n"
		"int r = ((x) + (y))\n"
		"        + z;\n"
		"      int w = f( 1 );\n"
		"int s = q p;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/m.cfa";
	// "int r = ((x) + (y))": the expansion maps to ADD( x,\n   y )
	CHECK( m.map( F, 4, 8 ) == L( 3, 8 ) );					// ( -> start of ADD
	CHECK( m.map( F, 4, 10 ) == L( 3, 13 ) );				// x -> the argument x
	CHECK( m.map( F, 4, 11, true ) == L( 3, 14 ) );
	CHECK( m.map( F, 4, 16 ) == L( 4, 3 ) );				// y -> the argument y on the next line
	CHECK( m.map( F, 4, 13 ) == L( 3, 8 ) );				// + from the body -> start
	CHECK( m.map( F, 4, 14, true ) == L( 4, 6 ) );			// and end of the invocation
	CHECK( m.mapRange( F, 4, 8, 4, 19 ) == Range{ L( 3, 8 ), L( 4, 6 ) } );
	CHECK( m.map( F, 5, 8 ) == L( 4, 7 ) );					// + after the invocation
	CHECK( m.map( F, 5, 10 ) == L( 4, 9 ) );				// z
	// An empty macro before a declaration.
	CHECK( m.map( F, 6, 6 ) == L( 5, 6 ) );
	CHECK( m.map( F, 6, 10 ) == L( 5, 11 ) );
	CHECK( m.map( F, 6, 14 ) == L( 5, 15 ) );				// f
	// Arguments that come out in a different order still give an ordered range.
	CHECK( m.map( F, 7, 8 ) == L( 6, 17 ) );				// q
	CHECK( m.map( F, 7, 10 ) == L( 6, 14 ) );				// p
	Range sw = m.mapRange( F, 7, 8, 7, 11 );
	CHECK( sw == Range{ L( 6, 8 ), L( 6, 20 ) } );
}

TEST_CASE( "sourcemap: concurrent lookups" ) {
	Files f;
	std::string orig, prep = "# 1 \"/r/big.cfa\"\n";
	for ( int i = 0; i < 2000; ++i ) {
		orig += "int\t\tv" + std::to_string( i ) + "   =  " + std::to_string( i ) + ";\n";
		prep += "int v" + std::to_string( i ) + " = " + std::to_string( i ) + ";\n";
	}
	f.text["/r/big.cfa"] = orig;
	SourceMap m( prep, f.reader() );
	std::atomic<int> bad{ 0 };
	std::vector<std::thread> ts;
	for ( int t = 0; t < 8; ++t ) {
		ts.emplace_back( [&, t] {
			for ( int i = t; i < 2000; i += 8 ) {
				if ( m.map( "/r/big.cfa", i + 1, 4 ) != L( i, 5 ) ) ++bad;
			}
		} );
	}
	for ( auto & t : ts ) t.join();
	CHECK( bad == 0 );
	CHECK( f.reads->size() == 1 );
}

// cpp restarts a line with a marker around tokens from a system-header macro,
// so one original line has several preprocessed lines with their own columns.
TEST_CASE( "sourcemap: lines split by markers" ) {
	Files f;
	f.text["/r/s.c"] =
		"int f( int ch ) {\n"
		"\tassert( ch > 0 );\n"
		"\tif ( ! isdigit( ch ) || ch == '0' ) return 1;\n"
		"}\n";
	std::string prep =
		"# 1 \"/r/s.c\"\n"
		"int f( int ch ) {\n"
		" ((ch > 0) ? \n"
		"# 2 \"/r/s.c\" 3\n"
		"(void)\n"
		"# 2 \"/r/s.c\"\n"
		"(0) : __assert_fail(\"ch > 0\", \"/r/s.c\", 2, \n"
		"# 2 \"/r/s.c\" 3\n"
		"__extension__ __PRETTY_FUNCTION__\n"
		"# 2 \"/r/s.c\"\n"
		"));\n"
		" if ( ! \n"
		"# 3 \"/r/s.c\" 3\n"
		"       ((*__ctype_b_loc ())[(int) ((\n"
		"# 3 \"/r/s.c\"\n"
		"       ch\n"
		"# 3 \"/r/s.c\" 3\n"
		"       ))] & (unsigned short int) _ISdigit) \n"
		"# 3 \"/r/s.c\"\n"
		"                     || ch == '0' ) return 1;\n"
		"}\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/s.c";
	CHECK( m.mapRange( F, 2, 3, 2, 5 ) == Range{ L( 1, 9 ), L( 1, 11 ) } );		// ch in the first piece
	CHECK( m.map( F, 2, 2 ) == L( 1, 17 ) );									// ; in the last piece
	CHECK( m.map( F, 2, 0 ) == L( 1, 1 ) );
	CHECK( m.mapRange( F, 2, 1, 2, 2 ) == Range{ L( 1, 1 ), L( 1, 17 ) } );		// the whole assert
	CHECK( m.mapRange( F, 3, 7, 3, 9 ) == Range{ L( 2, 17 ), L( 2, 19 ) } );	// ch alone on its piece
	CHECK( m.map( F, 3, 21 ) == L( 2, 22 ) );									// ||
	CHECK( m.mapRange( F, 3, 24, 3, 26 ) == Range{ L( 2, 25 ), L( 2, 27 ) } );
	CHECK( m.map( F, 3, 1 ) == L( 2, 1 ) );										// if
	CHECK( m.map( F, 4, 0 ) == L( 3, 0 ) );
}

TEST_CASE( "sourcemap: lines too long for one alignment table" ) {
	// About 1200 tokens on one line, with no macros: only the spacing differs.
	std::string orig = "int t[] = {", prep = "int t[] = {";
	for ( int i = 1; i <= 600; i += 1 ) {
		orig += "  " + std::to_string( i ) + ",";
		prep += " " + std::to_string( i ) + ",";
	}
	int origCol = int( orig.size() ) + 2, prepCol = int( prep.size() ) + 1;
	orig += "  nosuch };\n";
	prep += " nosuch };\n";
	Files f;
	f.text["/r/long.cfa"] = orig;
	SourceMap m( "# 1 \"/r/long.cfa\"\n" + prep, f.reader() );
	CHECK( m.mapRange( "/r/long.cfa", 1, prepCol, 1, prepCol + 6 ) == Range{ L( 0, origCol ), L( 0, origCol + 6 ) } );

	// A macro call in every statement, so neither end of the line matches far.
	std::string o2 = "#define X(n) ((n)*2)\n", p2;
	int o2Col = 0, p2Col = 0;
	for ( int i = 0; i < 400; i += 1 ) {
		if ( i == 300 ) {
			o2Col = int( o2.size() ) - 21 + 7;					// after "#define ...\n" and "z  +=  "
			p2Col = int( p2.size() ) + 5;
			o2 += "z  +=  nosuch;  ";
			p2 += "z += nosuch; ";
			continue;
		}
		o2 += "z  +=  X(" + std::to_string( i ) + ");  ";
		p2 += "z += ((" + std::to_string( i ) + ")*2); ";
	}
	o2 += "\n";
	Files g;
	g.text["/r/mac.cfa"] = o2;
	SourceMap m2( "# 2 \"/r/mac.cfa\"\n" + p2 + "\n", g.reader() );
	CHECK( m2.mapRange( "/r/mac.cfa", 2, p2Col, 2, p2Col + 6 ) == Range{ L( 1, o2Col ), L( 1, o2Col + 6 ) } );
	// A token from a macro maps to its invocation.
	int x = int( p2.find( "((399)" ) );
	int xo = int( o2.find( "X(399)" ) ) - 21;
	CHECK( m2.mapRange( "/r/mac.cfa", 2, x, 2, x + 1 ) == Range{ L( 1, xo ), L( 1, xo + 6 ) } );
}
