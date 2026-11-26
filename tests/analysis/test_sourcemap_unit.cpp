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
	f.text["/r/m.c"] = "#include \"t.h\"\n#define T 1000\n#include \"t.h\"\n";
	f.text["/r/t.h"] = "int  v = T;\n";
	std::string prep =
		"# 1 \"/r/m.c\"\n"				// 1
		"# 1 \"/r/t.h\" 1\n"			// 2
		"int v = T;\n"					// 3
		"# 2 \"/r/m.c\" 2\n"			// 4
		"\n"							// 5
		"# 1 \"/r/t.h\" 1\n"			// 6
		"int v = 1000;\n"				// 7
		"# 4 \"/r/m.c\" 2\n";			// 8
	SourceMap m( prep, f.reader() );
	const std::string H = "/r/t.h";
	// Without pline, the first copy is used: column 12 is past its end.
	CHECK( m.map( H, 1, 4 ) == L( 0, 5 ) );
	CHECK( m.map( H, 1, 8 ) == L( 0, 9 ) );
	CHECK( m.map( H, 1, 12 ) == L( 0, 11 ) );
	// pline picks the copy.
	CHECK( m.map( H, { 1, 8, 3 } ) == L( 0, 9 ) );
	CHECK( m.map( H, { 1, 9, 3 } ) == L( 0, 10 ) );				// ; in the first copy
	CHECK( m.map( H, { 1, 8, 7 } ) == L( 0, 9 ) );				// 1000 -> T
	CHECK( m.map( H, { 1, 12, 7 } ) == L( 0, 10 ) );				// ; in the second copy
	CHECK( m.mapRange( H, { 1, 4, 7 }, { 1, 13, 7 } ) == Range{ L( 0, 5 ), L( 0, 11 ) } );
	// A pline that is not a line of this file with this number is ignored.
	CHECK( m.map( H, { 1, 12, 4 } ) == L( 0, 11 ) );
	CHECK( m.map( H, { 2, 12, 7 } ) == L( 1, 0 ) );
	CHECK( m.map( H, { 1, 12, 99 } ) == L( 0, 11 ) );
}

// A #line in the source makes the markers give other line numbers; the
// file's own directives say which real lines they are.
TEST_CASE( "sourcemap: #line in the source" ) {
	Files f;
	f.text["/r/l.cfa"] =
		"int a;\n"						// 0
		"#line 100\n"					// 1
		"int   b;\n"					// 2
		"int c;\n"						// 3
		"#line 4\n"						// 4
		"int  d;\n"						// 5
		"# 4 \"/r/l.cfa\"\n"				// 6
		"int  e;\n";						// 7
	std::string prep =
		"# 1 \"/r/l.cfa\"\n"				// 1
		"int a;\n"						// 2
		"# 100 \"/r/l.cfa\"\n"			// 3
		"int b;\n"						// 4
		"int c;\n"						// 5
		"# 4 \"/r/l.cfa\"\n"				// 6
		"int d;\n"						// 7
		"# 4 \"/r/l.cfa\"\n"				// 8
		"int e;\n";						// 9
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/l.cfa";
	CHECK( m.map( F, 1, 4 ) == L( 0, 4 ) );
	CHECK( m.map( F, 100, 4 ) == L( 2, 6 ) );
	CHECK( m.map( F, 101, 4 ) == L( 3, 4 ) );
	CHECK( m.mapRange( F, 100, 4, 100, 5 ) == Range{ L( 2, 6 ), L( 2, 7 ) } );
	CHECK( m.mapRange( F, 100, 0, 101, 6 ) == Range{ L( 2, 0 ), L( 3, 6 ) } );
	// d and e are both on line 4 as far as the markers go: the first one wins
	// without pline. Repeating the number is not a split line, so e is not a
	// piece of d's line.
	CHECK( m.map( F, 4, 4 ) == L( 5, 5 ) );
	CHECK( m.map( F, { 4, 4, 7 } ) == L( 5, 5 ) );
	CHECK( m.map( F, { 4, 4, 9 } ) == L( 7, 5 ) );
	CHECK( m.map( F, { 4, 6, 9 }, true ) == L( 7, 7 ) );
	// The lines that left text are the real ones, not the numbers the
	// markers give.
	CHECK( m.outputLines( F ) == ( std::vector<int>{ 0, 2, 3, 5, 7 } ) );
}

// cpp ignores a #line inside a comment, so the lines after it keep their numbers.
TEST_CASE( "sourcemap: #line in a comment" ) {
	Files f;
	f.text["/r/c.cfa"] =
		"/*\n"
		"#line 2\n"
		"*/\n"
		"int   b;\n"
		"\n"
		"int  c;\n";
	std::string prep =
		"# 1 \"/r/c.cfa\"\n"
		"\n"
		"\n"
		"\n"
		"int b;\n"
		"\n"
		"int c;\n";
	SourceMap m( prep, f.reader() );
	CHECK( m.map( "/r/c.cfa", 4, 4 ) == L( 3, 6 ) );
	CHECK( m.map( "/r/c.cfa", 6, 4 ) == L( 5, 5 ) );
}

// An identifier in a macro's body that is also an argument: the expansion is
// redone from the #define, so the body's tmp maps to the whole invocation and
// the argument's to the argument.
TEST_CASE( "sourcemap: macro body identifier that is also an argument" ) {
	Files f;
	f.text["/r/swap.h"] = "#define SWAP( a, b ) \\\n\t{ int tmp = a; a = b; b = tmp; }\n";
	f.text["/r/w.cfa"] =
		"#include \"swap.h\"\n"
		"void f( int x, int tmp ) {\n"
		"\tSWAP( x, tmp );\n"
		"\tSWAP( tmp, x );\n"
		"}\n";
	std::string prep =
		"# 1 \"/r/w.cfa\"\n"
		"# 1 \"/r/swap.h\" 1\n"
		"# 2 \"/r/w.cfa\" 2\n"
		"void f( int x, int tmp ) {\n"
		" { int tmp = x; x = tmp; tmp = tmp; };\n"
		" { int tmp = tmp; tmp = x; x = tmp; };\n"
		"}\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/w.cfa";
	// " { int tmp = x; x = tmp; tmp = tmp; };"
	//   0123456789012345678901234567890123456
	Loc swap = L( 2, 1 ), x = L( 2, 7 ), tmp = L( 2, 10 );
	CHECK( m.map( F, 3, 7 ) == swap );						// int tmp: the body's
	CHECK( m.map( F, 3, 10, true ) == L( 2, 15 ) );			// ... ends with the invocation
	CHECK( m.inMacroBody( F, { 3, 7 } ) );
	CHECK( m.map( F, 3, 13 ) == x );
	CHECK_FALSE( m.inMacroBody( F, { 3, 13 } ) );
	CHECK( m.map( F, 3, 16 ) == x );
	CHECK( m.map( F, 3, 20 ) == tmp );						// a = b: the argument
	CHECK_FALSE( m.inMacroBody( F, { 3, 20 } ) );
	CHECK( m.map( F, 3, 25 ) == tmp );
	CHECK( m.map( F, 3, 31 ) == swap );						// b = tmp: the body's
	CHECK( m.inMacroBody( F, { 3, 31 } ) );
	CHECK( m.inMacroBody( F, { 3, 11 } ) );					// = from the body
	CHECK( m.map( F, 3, 37 ) == L( 2, 15 ) );				// ;
	CHECK_FALSE( m.inMacroBody( F, { 3, 37 } ) );
	// " { int tmp = tmp; tmp = x; x = tmp; };"
	CHECK( m.map( F, 4, 7 ) == L( 3, 1 ) );
	CHECK( m.map( F, 4, 13 ) == L( 3, 7 ) );				// = a: the argument tmp
	CHECK( m.map( F, 4, 18 ) == L( 3, 7 ) );
	CHECK( m.map( F, 4, 24 ) == L( 3, 12 ) );				// x
	CHECK( m.map( F, 4, 27 ) == L( 3, 12 ) );
	CHECK( m.map( F, 4, 31 ) == L( 3, 1 ) );				// = tmp: the body's
	CHECK( m.inMacroBody( F, { 4, 31 } ) );

	// Without the definition the argument is the only guess.
	Files g = f;
	g.text["/r/swap.h"] = "";
	SourceMap n( prep, g.reader() );
	CHECK( n.map( F, 3, 7 ) == tmp );
	CHECK_FALSE( n.inMacroBody( F, { 3, 7 } ) );
}

// The #define in effect is the last one cpp read before the use, in any file
// of the include graph: here a header included by a header, an #undef and a
// new definition, and two definitions in the branches of an #if.
TEST_CASE( "sourcemap: macro definitions from the whole include graph, in order" ) {
	Files f;
	f.text["/r/inner.h"] = "#define SWAP( a, b ) { int tmp = a; a = b; b = tmp; }\n";
	f.text["/r/outer.h"] = "#include \"inner.h\"\n#define F( a ) a + tmp\n";
	f.text["/r/g.cfa"] =
		"#include \"outer.h\"\n"						// 0
		"int x = F( tmp );\n"
		"#undef F\n"
		"#define F( a ) tmp + a\n"
		"int y = F( tmp );\n"
		"#ifndef NOPE\n"								// 5
		"#define G( a ) tmp - a\n"
		"#else\n"
		"#define G( a ) a + tmp\n"
		"#endif\n"
		"int z = G( tmp );\n"							// 10
		"void f( int x, int tmp ) { SWAP( x, tmp ); }\n";
	std::string prep =
		"# 1 \"/r/g.cfa\"\n"
		"# 1 \"/r/outer.h\" 1\n"
		"# 1 \"/r/inner.h\" 1\n"
		"# 2 \"/r/outer.h\" 2\n"
		"# 2 \"/r/g.cfa\" 2\n"
		"int x = tmp + tmp;\n"
		"\n"
		"\n"
		"int y = tmp + tmp;\n"
		"\n\n\n\n\n"
		"int z = tmp - tmp;\n"
		"void f( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; }; }\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/g.cfa";
	// "int x = tmp + tmp;" from outer.h's F: the argument, then the body.
	CHECK( m.map( F, 2, 8 ) == L( 1, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 2, 8 } ) );
	CHECK( m.map( F, 2, 14 ) == L( 1, 8 ) );
	CHECK( m.map( F, 2, 17, true ) == L( 1, 16 ) );
	CHECK( m.inMacroBody( F, { 2, 14 } ) );
	// After #undef and the new #define: the body, then the argument.
	CHECK( m.map( F, 5, 8 ) == L( 4, 8 ) );
	CHECK( m.inMacroBody( F, { 5, 8 } ) );
	CHECK( m.map( F, 5, 14 ) == L( 4, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 5, 14 } ) );
	// The later G, in the #else, would give "tmp + tmp", so the first one is used.
	CHECK( m.map( F, 11, 8 ) == L( 10, 8 ) );
	CHECK( m.inMacroBody( F, { 11, 8 } ) );
	CHECK( m.map( F, 11, 14 ) == L( 10, 11 ) );
	// SWAP from inner.h, which g.cfa doesn't include itself.
	// "void f( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; }; }"
	CHECK( m.map( F, 12, 33 ) == L( 11, 27 ) );
	CHECK( m.inMacroBody( F, { 12, 33 } ) );
	CHECK( m.map( F, 12, 46 ) == L( 11, 36 ) );
	CHECK( m.map( F, 12, 51 ) == L( 11, 36 ) );
	CHECK( m.map( F, 12, 57 ) == L( 11, 27 ) );
	CHECK( m.inMacroBody( F, { 12, 57 } ) );
	CHECK( m.map( F, 12, 19 ) == L( 11, 19 ) );				// the parameter, outside the macro
}

TEST_CASE( "sourcemap: macro bodies with #, ## and GNU's , ## __VA_ARGS__" ) {
	Files f;
	f.text["/r/ops.cfa"] =
		"#define SHOW( v ) show( #v, v, tmp )\n"
		"#define FIELD( n ) n ## _len + n + len\n"
		"#define LOG( f, ... ) out( f, ## __VA_ARGS__ ) + out\n"
		"#define N tmp\n"
		"#define GLUE( a ) a ## 1 + a + tmp\n"
		"#define STR( x ) #x\n"
		"#define XSTR( x ) STR( x )\n"
		"#define AT( s ) at( s, __LINE__, __FILE__ ) + at\n"
		"SHOW( tmp ); SHOW( a  +  \"q\\n\" );\n"			// 8
		"FIELD( len );\n"
		"LOG( out ); LOG( out, 1, 2 );\n"					// 10
		"GLUE( N );\n"
		"STR( N ); XSTR( N ); XSTR( a  +b );\n"
		"AT( at );\n";
	std::string prep =
		"# 1 \"/r/ops.cfa\"\n"
		"# 9 \"/r/ops.cfa\"\n"
		"show( \"tmp\", tmp, tmp ); show( \"a + \\\"q\\\\n\\\"\", a + \"q\\n\", tmp );\n"
		"len_len + len + len;\n"
		"out( out ) + out; out( out, 1, 2 ) + out;\n"
		"N1 + tmp + tmp;\n"
		"\"N\"; \"tmp\"; \"a +b\";\n"
		"at( at, 14, \"/r/ops.cfa\" ) + at;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/ops.cfa";
	// show( "tmp", tmp, tmp ): # spells the argument; v is the argument and
	// the last tmp the body's.
	CHECK( m.map( F, 9, 6 ) == L( 8, 0 ) );
	CHECK( m.inMacroBody( F, { 9, 6 } ) );
	CHECK( m.map( F, 9, 13 ) == L( 8, 6 ) );
	CHECK_FALSE( m.inMacroBody( F, { 9, 13 } ) );
	CHECK( m.map( F, 9, 18 ) == L( 8, 0 ) );
	CHECK( m.inMacroBody( F, { 9, 18 } ) );
	// # keeps one space for each run of blanks and escapes the literal.
	CHECK( m.inMacroBody( F, { 9, 31 } ) );
	CHECK( m.map( F, 9, 47 ) == L( 8, 19 ) );					// a
	CHECK( m.map( F, 9, 58 ) == L( 8, 13 ) );
	// "len_len + len + len": the pasted name is the body's, like the last len.
	CHECK( m.map( F, 10, 0 ) == L( 9, 0 ) );
	CHECK( m.inMacroBody( F, { 10, 0 } ) );
	CHECK( m.map( F, 10, 10 ) == L( 9, 7 ) );
	CHECK_FALSE( m.inMacroBody( F, { 10, 10 } ) );
	CHECK( m.map( F, 10, 16 ) == L( 9, 0 ) );
	CHECK( m.inMacroBody( F, { 10, 16 } ) );
	// The comma goes without variable arguments and stays with them.
	CHECK( m.map( F, 11, 0 ) == L( 10, 0 ) );
	CHECK( m.map( F, 11, 5 ) == L( 10, 5 ) );
	CHECK_FALSE( m.inMacroBody( F, { 11, 5 } ) );
	CHECK( m.map( F, 11, 13 ) == L( 10, 0 ) );
	CHECK( m.inMacroBody( F, { 11, 13 } ) );
	CHECK( m.map( F, 11, 23 ) == L( 10, 17 ) );
	CHECK( m.map( F, 11, 37 ) == L( 10, 12 ) );
	CHECK( m.inMacroBody( F, { 11, 37 } ) );
	// "N1 + tmp + tmp": next to ## the argument N is not expanded; elsewhere
	// it is, so that tmp is N's body, written at N.
	CHECK( m.inMacroBody( F, { 12, 0 } ) );
	CHECK( m.mapRange( F, 12, 5, 12, 8 ) == Range{ L( 11, 6 ), L( 11, 7 ) } );
	CHECK( m.inMacroBody( F, { 12, 5 } ) );
	CHECK( m.map( F, 12, 11 ) == L( 11, 0 ) );
	CHECK( m.inMacroBody( F, { 12, 11 } ) );
	// "N"; "tmp"; "a +b";: XSTR expands its argument before STR spells it.
	CHECK( m.inMacroBody( F, { 13, 0 } ) );
	CHECK( m.inMacroBody( F, { 13, 5 } ) );
	CHECK( m.map( F, 13, 5 ) == L( 12, 10 ) );
	CHECK( m.inMacroBody( F, { 13, 12 } ) );
	// __LINE__ and __FILE__ match whatever cpp printed.
	CHECK( m.map( F, 14, 4 ) == L( 13, 4 ) );
	CHECK_FALSE( m.inMacroBody( F, { 14, 4 } ) );
	CHECK( m.inMacroBody( F, { 14, 8 } ) );
	CHECK( m.map( F, 14, 29 ) == L( 13, 0 ) );
	CHECK( m.inMacroBody( F, { 14, 29 } ) );
}

TEST_CASE( "sourcemap: nested macros, rescanning and macros in arguments" ) {
	Files f;
	f.text["/r/nest.cfa"] =
		"#define TMP tmp\n"
		"#define INNER( a ) ( a + TMP )\n"
		"#define OUTER( a ) INNER( a ) * tmp\n"
		"#define foo foo + tmp\n"
		"#define A B + a\n"
		"#define B A + b\n"							// 5
		"#define CALLER G\n"
		"#define G( a ) a + tmp\n"
		"#define ID( a ) a\n"
		"#define SWAP( a, b ) { int tmp = a; a = b; b = tmp; }\n"
		"#define WRAP( a ) a * tmp\n"					// 10
		"int x1 = OUTER( tmp );\n"
		"int x2 = foo;\n"
		"int x3 = A;\n"
		"int x4 = CALLER( tmp );\n"
		"void f( int x, int tmp ) { SWAP( x, ID( tmp ) ); }\n"	// 15
		"int x5 = WRAP( G( y ) );\n";
	std::string prep =
		"# 1 \"/r/nest.cfa\"\n"
		"# 12 \"/r/nest.cfa\"\n"
		"int x1 = ( tmp + tmp ) * tmp;\n"
		"int x2 = foo + tmp;\n"
		"int x3 = A + b + a;\n"
		"int x4 = tmp + tmp;\n"
		"void f( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; }; }\n"
		"int x5 = y + tmp * tmp;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/nest.cfa";
	// "int x1 = ( tmp + tmp ) * tmp": the argument, TMP's body, OUTER's body.
	CHECK( m.map( F, 12, 11 ) == L( 11, 16 ) );
	CHECK_FALSE( m.inMacroBody( F, { 12, 11 } ) );
	CHECK( m.map( F, 12, 17 ) == L( 11, 9 ) );
	CHECK( m.inMacroBody( F, { 12, 17 } ) );
	CHECK( m.map( F, 12, 25 ) == L( 11, 9 ) );
	CHECK( m.inMacroBody( F, { 12, 25 } ) );
	// A macro is not expanded again inside its own expansion.
	CHECK( m.map( F, 13, 9 ) == L( 12, 9 ) );
	CHECK( m.inMacroBody( F, { 13, 9 } ) );
	CHECK( m.inMacroBody( F, { 13, 15 } ) );
	CHECK( m.inMacroBody( F, { 14, 9 } ) );
	CHECK( m.inMacroBody( F, { 14, 17 } ) );
	// CALLER's body ends with G, which takes the ( tmp ) after CALLER.
	CHECK( m.map( F, 15, 9 ) == L( 14, 17 ) );
	CHECK_FALSE( m.inMacroBody( F, { 15, 9 } ) );
	CHECK( m.map( F, 15, 15 ) == L( 14, 9 ) );
	CHECK( m.inMacroBody( F, { 15, 15 } ) );
	// An argument with a macro in it is expanded before it is substituted.
	CHECK( m.map( F, 16, 33 ) == L( 15, 27 ) );
	CHECK( m.inMacroBody( F, { 16, 33 } ) );
	CHECK( m.map( F, 16, 46 ) == L( 15, 40 ) );
	CHECK_FALSE( m.inMacroBody( F, { 16, 46 } ) );
	CHECK( m.map( F, 16, 51 ) == L( 15, 40 ) );
	CHECK( m.map( F, 16, 57 ) == L( 15, 27 ) );
	// "int x5 = y + tmp * tmp": G's tmp maps to G( y ), WRAP's to all of it.
	CHECK( m.map( F, 17, 9 ) == L( 16, 18 ) );
	CHECK( m.mapRange( F, 17, 13, 17, 16 ) == Range{ L( 16, 15 ), L( 16, 21 ) } );
	CHECK( m.inMacroBody( F, { 17, 13 } ) );
	CHECK( m.mapRange( F, 17, 19, 17, 22 ) == Range{ L( 16, 9 ), L( 16, 23 ) } );
}

// cpp starts with a "# 0" marker for the main file before it reads any of it,
// so a #define after the use isn't in effect there. A #define or #undef inside
// an #if may be in a dead branch: the definition before it, or none, is tried
// too. An include guard doesn't count as an #if.
TEST_CASE( "sourcemap: definitions cpp hadn't read yet, and ones in dead #if branches" ) {
	Files f;
	f.text["/r/guard.h"] = "#ifndef GUARD_H\n#define GUARD_H\n#define G( a ) a - tmp\n#endif\n";
	f.text["/r/late.cfa"] =
		"#define SWAP( a, b ) { int tmp = a; a = b; b = tmp; check( tmp ); }\n"	// 0
		"void check( int );\n"
		"void f( int x, int tmp ) { SWAP( x, tmp ); }\n"
		"#define check( v ) ((void)0)\n"
		"#define N 10\n"
		"#ifdef BIG\n"																// 5
		"#undef N\n"
		"#define N 100\n"
		"#define EXTRA + tmp\n"
		"#endif\n"
		"#define S( a, b ) { int tmp = a; a = b; b = tmp + N; }\n"				// 10
		"#define T( a ) a + tmp EXTRA\n"
		"void h( int x, int tmp ) { S( x, tmp ); }\n"
		"int t = T( tmp );\n"
		"#include \"guard.h\"\n"
		"int g = G( tmp );\n";														// 15
	std::string prep =
		"# 0 \"/r/late.cfa\"\n"
		"# 0 \"<built-in>\"\n"
		"# 0 \"<command-line>\"\n"
		"# 1 \"/r/late.cfa\"\n"
		"\n"
		"void check( int );\n"
		"void f( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; check( tmp ); }; }\n"
		"# 13 \"/r/late.cfa\"\n"
		"void h( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp + 10; }; }\n"
		"int t = tmp + tmp EXTRA;\n"
		"# 1 \"/r/guard.h\" 1\n"
		"# 16 \"/r/late.cfa\" 2\n"
		"int g = tmp - tmp;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/late.cfa";
	// "{ int tmp = x; x = tmp; tmp = tmp; check( tmp ); }": check isn't a
	// macro yet.
	CHECK( m.map( F, 3, 33 ) == L( 2, 27 ) );
	CHECK( m.inMacroBody( F, { 3, 33 } ) );
	CHECK( m.map( F, 3, 46 ) == L( 2, 36 ) );
	CHECK_FALSE( m.inMacroBody( F, { 3, 46 } ) );
	CHECK( m.map( F, 3, 69 ) == L( 2, 27 ) );
	CHECK( m.inMacroBody( F, { 3, 69 } ) );
	// "tmp + 10": the N before the #ifdef.
	CHECK( m.map( F, 13, 33 ) == L( 12, 27 ) );
	CHECK( m.inMacroBody( F, { 13, 33 } ) );
	CHECK( m.map( F, 13, 46 ) == L( 12, 33 ) );
	CHECK_FALSE( m.inMacroBody( F, { 13, 46 } ) );
	CHECK( m.inMacroBody( F, { 13, 63 } ) );
	// "tmp + tmp EXTRA": EXTRA was never defined.
	CHECK( m.map( F, 14, 8 ) == L( 13, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 14, 8 } ) );
	CHECK( m.map( F, 14, 14 ) == L( 13, 8 ) );
	CHECK( m.inMacroBody( F, { 14, 14 } ) );
	// G from the guarded header.
	CHECK( m.map( F, 16, 8 ) == L( 15, 11 ) );
	CHECK( m.map( F, 16, 14 ) == L( 15, 8 ) );
	CHECK( m.inMacroBody( F, { 16, 14 } ) );
}

// The alignment can give a macro use the wrong tokens: two uses side by side,
// a body that ends in a function-like macro name, which takes the ( ... )
// after the use, or a use whose arguments it matched one by one. The redo
// then says where the use ends.
TEST_CASE( "sourcemap: macro uses the alignment cuts in the wrong place" ) {
	Files f;
	f.text["/r/cut.cfa"] =
		"#define SW( a, b ) { int tmp = a; a = b; b = tmp; }\n"				// 0
		"#define PICK( n ) SW\n"
		"#define TWO( a ) a + tmp\n"
		"#define LOG( f, ... ) out( f, ## __VA_ARGS__ ) + tmp\n"
		"#define LOG1( ... ) out( 1, ## __VA_ARGS__ ) + tmp\n"
		"void k( int x, int tmp ) { PICK( 2 )( x, tmp ); }\n"					// 5
		"int u = TWO( tmp ) TWO( y );\n"
		"int g( int tmp ) { return LOG( tmp, ) + LOG1() + LOG1( 2 ); }\n";
	std::string prep =
		"# 1 \"/r/cut.cfa\"\n"
		"\n\n\n\n\n"
		"void k( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; }; }\n"
		"int u = tmp + tmp y + tmp;\n"
		"int g( int tmp ) { return out( tmp, ) + tmp + out( 1 ) + tmp + out( 1, 2 ) + tmp; }\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/cut.cfa";
	// PICK( 2 ) gives SW, which takes ( x, tmp ).
	CHECK( m.map( F, 6, 33 ) == L( 5, 27 ) );
	CHECK( m.inMacroBody( F, { 6, 33 } ) );
	CHECK( m.map( F, 6, 39 ) == L( 5, 38 ) );
	CHECK( m.map( F, 6, 46 ) == L( 5, 41 ) );
	CHECK_FALSE( m.inMacroBody( F, { 6, 46 } ) );
	CHECK( m.map( F, 6, 57 ) == L( 5, 27 ) );
	CHECK( m.inMacroBody( F, { 6, 57 } ) );
	// "tmp + tmp y + tmp": each TWO gave three tokens.
	CHECK( m.map( F, 7, 8 ) == L( 6, 13 ) );
	CHECK_FALSE( m.inMacroBody( F, { 7, 8 } ) );
	CHECK( m.map( F, 7, 14 ) == L( 6, 8 ) );
	CHECK( m.inMacroBody( F, { 7, 14 } ) );
	CHECK( m.map( F, 7, 18 ) == L( 6, 24 ) );
	CHECK( m.map( F, 7, 22 ) == L( 6, 19 ) );
	CHECK( m.inMacroBody( F, { 7, 22 } ) );
	// GNU keeps the comma before an empty variable argument, and drops it when
	// there is none, which an empty one is when it is the only parameter.
	CHECK( m.map( F, 8, 31 ) == L( 7, 31 ) );
	CHECK_FALSE( m.inMacroBody( F, { 8, 31 } ) );
	CHECK( m.map( F, 8, 40 ) == L( 7, 26 ) );
	CHECK( m.inMacroBody( F, { 8, 40 } ) );
	CHECK( m.map( F, 8, 57 ) == L( 7, 40 ) );
	CHECK( m.inMacroBody( F, { 8, 57 } ) );
	CHECK( m.map( F, 8, 77 ) == L( 7, 49 ) );
	CHECK( m.inMacroBody( F, { 8, 77 } ) );
}

TEST_CASE( "sourcemap: __VA_OPT__" ) {
	Files f;
	f.text["/r/va.cfa"] =
		"#define V( f, ... ) out( f __VA_OPT__(,) __VA_ARGS__ ) + tmp\n"
		"#define V2( a, ... ) a ## __VA_OPT__( x ## __VA_ARGS__ y ) ## a\n"
		"#define VA( ... ) __VA_OPT__( tmp ) + tmp\n"
		"#define E\n"
		"int h( int tmp ) { return V( tmp ) + V( tmp, 2 ) + VA( tmp ); }\n"		// 4
		"int q = V2( q ) + V2( q, 1 );\n";
	std::string prep =
		"# 1 \"/r/va.cfa\"\n"
		"\n\n\n\n"
		"int h( int tmp ) { return out( tmp ) + tmp + out( tmp , 2 ) + tmp + tmp + tmp; }\n"
		"int q = qq + qx1 yq;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/va.cfa";
	CHECK( m.map( F, 5, 31 ) == L( 4, 29 ) );
	CHECK_FALSE( m.inMacroBody( F, { 5, 31 } ) );
	CHECK( m.map( F, 5, 39 ) == L( 4, 26 ) );
	CHECK( m.inMacroBody( F, { 5, 39 } ) );
	CHECK( m.map( F, 5, 50 ) == L( 4, 40 ) );
	CHECK_FALSE( m.inMacroBody( F, { 5, 50 } ) );
	CHECK( m.map( F, 5, 62 ) == L( 4, 37 ) );
	CHECK( m.inMacroBody( F, { 5, 62 } ) );
	// Both of VA's tmps are its body's: __VA_ARGS__ isn't in it.
	CHECK( m.map( F, 5, 68 ) == L( 4, 51 ) );
	CHECK( m.inMacroBody( F, { 5, 68 } ) );
	CHECK( m.map( F, 5, 74 ) == L( 4, 51 ) );
	CHECK( m.inMacroBody( F, { 5, 74 } ) );
	// ## next to __VA_OPT__, with and without variable arguments.
	CHECK( m.map( F, 6, 8 ) == L( 5, 8 ) );
	CHECK( m.inMacroBody( F, { 6, 8 } ) );
	CHECK( m.map( F, 6, 13 ) == L( 5, 18 ) );
	CHECK( m.inMacroBody( F, { 6, 13 } ) );
	CHECK( m.inMacroBody( F, { 6, 17 } ) );
}

// What can't be redone maps as before: an identifier of the expansion that
// the invocation also has is taken to be that one.
TEST_CASE( "sourcemap: macros that can't be redone" ) {
	Files f;
	f.text["/r/no.cfa"] =
		"#define BIG( a ) a + tmp + __INT_MAX__\n"
		"#define V( f, ... ) out( f __VA_OPT__(,) __VA_ARGS__ ) + tmp\n"
		"#define E\n"
		"int w = BIG( tmp );\n"
		"int v = V( tmp, E );\n";
	std::string prep =
		"# 1 \"/r/no.cfa\"\n"
		"\n\n\n"
		"int w = tmp + tmp + 0x7fffffff;\n"
		"int v = out( tmp ) + tmp;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/no.cfa";
	// cpp's own predefined macros are in no file.
	CHECK( m.map( F, 4, 14 ) == L( 3, 13 ) );
	CHECK_FALSE( m.inMacroBody( F, { 4, 14 } ) );
	CHECK( m.map( F, 4, 20 ) == L( 3, 8 ) );
	// gcc 16 leaves out __VA_OPT__ when the variable arguments expand to
	// nothing, and not every version does, so this isn't redone.
	CHECK( m.map( F, 5, 13 ) == L( 4, 11 ) );
	CHECK( m.map( F, 5, 21 ) == L( 4, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 5, 21 } ) );
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

	// pline names the piece, so a column that several pieces have is exact.
	CHECK( m.map( F, { 2, 2, 11 } ) == L( 1, 17 ) );							// ; on "));"
	CHECK( m.map( F, { 2, 2, 7 } ) == L( 1, 1 ) );								// ) of "(0)", from assert's body
	CHECK( m.map( F, { 2, 0, 5 } ) == L( 1, 1 ) );
	CHECK( m.mapRange( F, { 2, 3, 3 }, { 2, 3, 11 } ) == Range{ L( 1, 9 ), L( 1, 18 ) } );	// across pieces
	CHECK( m.mapRange( F, { 3, 7, 16 }, { 3, 9, 16 } ) == Range{ L( 2, 17 ), L( 2, 19 ) } );
	CHECK( m.map( F, { 3, 7, 14 } ) == L( 2, 8 ) );								// ( from isdigit's body
	CHECK( m.map( F, { 3, 21, 20 } ) == L( 2, 22 ) );
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
