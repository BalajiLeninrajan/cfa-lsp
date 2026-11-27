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
		"#define N 10\n"
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
		"#define T 1000\n"				// 5
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
		"#define SWAP(a,b) { int tmp = a; a = b; b = tmp; }\n"
		"\n"
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

	// Without the definitions cpp printed, the argument is the only guess.
	SourceMap n( SourceMap::translatorInput( prep ), f.reader() );
	CHECK( n.map( F, 3, 7 ) == tmp );
	CHECK_FALSE( n.inMacroBody( F, { 3, 7 } ) );
}

// `cfa -E -dD` prints each #define and #undef where cpp read it, so the
// definition in effect is the last one above the use, from any file: here a
// header included by a header, an #undef and a new definition, and the live
// branch of an #if.
TEST_CASE( "sourcemap: macro definitions cpp printed, in order" ) {
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
		"#define SWAP(a,b) { int tmp = a; a = b; b = tmp; }\n"
		"# 2 \"/r/outer.h\" 2\n"
		"#define F(a) a + tmp\n"
		"# 2 \"/r/g.cfa\" 2\n"
		"int x = tmp + tmp;\n"
		"#undef F\n"
		"#define F(a) tmp + a\n"
		"int y = tmp + tmp;\n"
		"\n"
		"#define G(a) tmp - a\n"
		"\n\n\n"
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
	// cpp left out the G in the #else.
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
	// Only the file asked about is read: the definitions come from cpp.
	CHECK( *f.reads == std::multiset<std::string>{ F } );
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
		"#define SHOW(v) show( #v, v, tmp )\n"
		"#define FIELD(n) n ## _len + n + len\n"
		"#define LOG(f,...) out( f, ## __VA_ARGS__ ) + out\n"
		"#define N tmp\n"
		"#define GLUE(a) a ## 1 + a + tmp\n"
		"#define STR(x) #x\n"
		"#define XSTR(x) STR( x )\n"
		"#define AT(s) at( s, __LINE__, __FILE__ ) + at\n"
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
		"#define TMP tmp\n"
		"#define INNER(a) ( a + TMP )\n"
		"#define OUTER(a) INNER( a ) * tmp\n"
		"#define foo foo + tmp\n"
		"#define A B + a\n"
		"#define B A + b\n"
		"#define CALLER G\n"
		"#define G(a) a + tmp\n"
		"#define ID(a) a\n"
		"#define SWAP(a,b) { int tmp = a; a = b; b = tmp; }\n"
		"#define WRAP(a) a * tmp\n"
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
	// A macro is not expanded again inside its own expansion. The name it
	// leaves is the one the source spells there.
	CHECK( m.map( F, 13, 9 ) == L( 12, 9 ) );
	CHECK_FALSE( m.inMacroBody( F, { 13, 9 } ) );
	CHECK( m.inMacroBody( F, { 13, 15 } ) );
	CHECK_FALSE( m.inMacroBody( F, { 14, 9 } ) );
	CHECK( m.inMacroBody( F, { 14, 13 } ) );
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

// cpp starts with a "# 0" marker for the main file and its predefined macros
// before it reads any of the file, so the main file's #defines are printed
// where cpp read them: one after a use isn't in effect there. One in a dead
// #if branch isn't printed at all, nor is the #undef next to it.
TEST_CASE( "sourcemap: a #define after the use, and ones in a dead #if branch" ) {
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
		"#define __STDC__ 1\n"
		"# 0 \"<command-line>\"\n"
		"# 1 \"/r/late.cfa\"\n"
		"#define SWAP(a,b) { int tmp = a; a = b; b = tmp; check( tmp ); }\n"
		"void check( int );\n"
		"void f( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp; check( tmp ); }; }\n"
		"#define check(v) ((void)0)\n"
		"#define N 10\n"
		"\n\n\n\n\n"
		"#define S(a,b) { int tmp = a; a = b; b = tmp + N; }\n"
		"#define T(a) a + tmp EXTRA\n"
		"void h( int x, int tmp ) { { int tmp = x; x = tmp; tmp = tmp + 10; }; }\n"
		"int t = tmp + tmp EXTRA;\n"
		"# 1 \"/r/guard.h\" 1\n"
		"\n"
		"#define GUARD_H \n"
		"#define G(a) a - tmp\n"
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
	// "tmp + 10": the N before the #ifdef BIG.
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
		"#define SW(a,b) { int tmp = a; a = b; b = tmp; }\n"
		"#define PICK(n) SW\n"
		"#define TWO(a) a + tmp\n"
		"#define LOG(f,...) out( f, ## __VA_ARGS__ ) + tmp\n"
		"#define LOG1(...) out( 1, ## __VA_ARGS__ ) + tmp\n"
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

// With two definitions of TWO, CHECK and OFF in the source, cpp printed only
// the live one. A shorter or empty body can't match the start of a longer
// one, and an empty OFF can't take the tokens of the check( y ) after it.
TEST_CASE( "sourcemap: macros with a definition in each #if branch" ) {
	Files f;
	f.text["/r/alt.cfa"] =
		"#define LONG\n"									// 0
		"#ifdef LONG\n"
		"#define TWO( a ) a + tmp + tmp\n"
		"#else\n"
		"#define TWO( a ) a + tmp\n"
		"#endif\n"											// 5
		"#ifndef NDEBUG\n"
		"#define CHECK( x ) check( x );\n"
		"#else\n"
		"#define CHECK( x )\n"
		"#endif\n"											// 10
		"#define OFF_ON\n"
		"#ifndef OFF_ON\n"
		"#define OFF( x ) check( x );\n"
		"#else\n"
		"#define OFF( x )\n"								// 15
		"#endif\n"
		"int u = TWO( tmp ) TWO( y );\n"
		"void f( int y ) { CHECK( y ) CHECK( y ) }\n"
		"void g( int y ) { OFF( y ) check( y ); }\n";
	std::string prep =
		"# 1 \"/r/alt.cfa\"\n"
		"#define LONG \n"
		"\n"
		"#define TWO(a) a + tmp + tmp\n"
		"\n\n\n\n"
		"#define CHECK(x) check( x );\n"
		"\n\n\n"
		"#define OFF_ON \n"
		"\n\n\n"
		"#define OFF(x) \n"
		"\n"
		"int u = tmp + tmp + tmp y + tmp + tmp;\n"
		"void f( int y ) { check( y ); check( y ); }\n"
		"void g( int y ) { check( y ); }\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/alt.cfa";
	// The first TWO gave "tmp + tmp + tmp", not "tmp + tmp".
	CHECK( m.map( F, 18, 8 ) == L( 17, 13 ) );
	CHECK_FALSE( m.inMacroBody( F, { 18, 8 } ) );
	CHECK( m.map( F, 18, 14 ) == L( 17, 8 ) );
	CHECK( m.inMacroBody( F, { 18, 14 } ) );
	CHECK( m.map( F, 18, 20 ) == L( 17, 8 ) );
	CHECK( m.inMacroBody( F, { 18, 20 } ) );
	CHECK( m.map( F, 18, 24 ) == L( 17, 24 ) );
	CHECK_FALSE( m.inMacroBody( F, { 18, 24 } ) );
	CHECK( m.map( F, 18, 28 ) == L( 17, 19 ) );
	CHECK( m.inMacroBody( F, { 18, 28 } ) );
	CHECK( m.map( F, 18, 34 ) == L( 17, 19 ) );
	// Each CHECK gave "check( y );".
	CHECK( m.map( F, 19, 18 ) == L( 18, 18 ) );
	CHECK( m.inMacroBody( F, { 19, 18 } ) );
	CHECK( m.map( F, 19, 25 ) == L( 18, 25 ) );
	CHECK_FALSE( m.inMacroBody( F, { 19, 25 } ) );
	CHECK( m.map( F, 19, 30 ) == L( 18, 29 ) );
	CHECK( m.inMacroBody( F, { 19, 30 } ) );
	CHECK( m.map( F, 19, 37 ) == L( 18, 36 ) );
	CHECK_FALSE( m.inMacroBody( F, { 19, 37 } ) );
	// OFF gave nothing; "check( y );" is the source's.
	CHECK( m.map( F, 20, 18 ) == L( 19, 27 ) );
	CHECK_FALSE( m.inMacroBody( F, { 20, 18 } ) );
	CHECK( m.map( F, 20, 25 ) == L( 19, 34 ) );
}

// A macro that expands to its own name, as glibc's stdin, stdout and stderr
// do, leaves the name the source spells in an argument. So does one whose
// body calls the function of the same name, as glibc's tolower( c ) does
// with optimization on.
TEST_CASE( "sourcemap: a macro that expands to its own name" ) {
	Files f;
	f.text["/r/self.cfa"] =
		"#define stdout stdout\n"
		"#define OUT stdout\n"
		"#define CHECK( e ) if ( ( e ) != 0 ) abort()\n"
		"void f() { CHECK( fflush( stdout ) ); }\n"
		"void g() { CHECK( fflush( OUT ) ); }\n"
		"#define body( c, f ) ( { int r; if ( c < 0 ) r = c; else r = f ( c ); r; } )\n"	// 5
		"#define lower( c ) body( c, lower )\n"
		"int d = lower( x );\n";
	std::string prep =
		"# 1 \"/r/self.cfa\"\n"
		"#define stdout stdout\n"
		"#define OUT stdout\n"
		"#define CHECK(e) if ( ( e ) != 0 ) abort()\n"
		"void f() { if ( ( fflush( stdout ) ) != 0 ) abort(); }\n"
		"void g() { if ( ( fflush( stdout ) ) != 0 ) abort(); }\n"
		"#define body(c,f) ( { int r; if ( c < 0 ) r = c; else r = f ( c ); r; } )\n"
		"#define lower(c) body( c, lower )\n"
		"int d = ( { int r; if ( x < 0 ) r = x; else r = lower ( x ); r; } );\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/self.cfa";
	CHECK( m.map( F, 4, 18 ) == L( 3, 18 ) );
	CHECK_FALSE( m.inMacroBody( F, { 4, 18 } ) );
	CHECK( m.map( F, 4, 26 ) == L( 3, 26 ) );
	CHECK_FALSE( m.inMacroBody( F, { 4, 26 } ) );
	CHECK( m.map( F, 4, 44 ) == L( 3, 11 ) );
	CHECK( m.inMacroBody( F, { 4, 44 } ) );
	// Here stdout is OUT's body.
	CHECK( m.map( F, 5, 26 ) == L( 4, 26 ) );
	CHECK( m.inMacroBody( F, { 5, 26 } ) );
	// The lower that body calls is the source's, and only its name.
	CHECK( m.mapRange( F, 8, 48, 8, 53 ) == Range{ L( 7, 8 ), L( 7, 13 ) } );
	CHECK_FALSE( m.inMacroBody( F, { 8, 48 } ) );
	CHECK( m.map( F, 8, 56 ) == L( 7, 15 ) );
	CHECK_FALSE( m.inMacroBody( F, { 8, 56 } ) );
	CHECK( m.inMacroBody( F, { 8, 16 } ) );
}

// An empty use gives nothing even when the source's tokens after it are what
// a longer body would give, also at the end of a line. And A B gives x from
// A and y from B.
TEST_CASE( "sourcemap: a use next to the source's own tokens" ) {
	Files f;
	f.text["/r/own.cfa"] =
		"#define OFF_ON\n"									// 0
		"#ifndef OFF_ON\n"
		"#define OFF( x ) check( x );\n"
		"#else\n"
		"#define OFF( x )\n"
		"#endif\n"											// 5
		"#ifndef OFF_ON\n"
		"#define A x B\n"
		"#else\n"
		"#define A x\n"
		"#endif\n"											// 10
		"#define B y\n"
		"void check( int );\n"
		"void g( int y ) {\n"
		"\tOFF( y ) check( y );\n"
		"}\n"												// 15
		"A B\n"
		"A B ;\n";
	std::string prep =
		"# 1 \"/r/own.cfa\"\n"
		"#define OFF_ON \n"
		"\n\n\n"
		"#define OFF(x) \n"
		"\n\n\n\n"
		"#define A x\n"
		"\n"
		"#define B y\n"
		"void check( int );\n"
		"void g( int y ) {\n"
		" check( y );\n"
		"}\n"
		"x y\n"
		"x y ;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/own.cfa";
	CHECK( m.map( F, 15, 1 ) == L( 14, 10 ) );
	CHECK_FALSE( m.inMacroBody( F, { 15, 1 } ) );
	CHECK( m.map( F, 15, 8 ) == L( 14, 17 ) );
	// A gave x and B gave y.
	CHECK( m.map( F, 17, 0 ) == L( 16, 0 ) );
	CHECK( m.inMacroBody( F, { 17, 0 } ) );
	CHECK( m.map( F, 17, 2 ) == L( 16, 2 ) );
	CHECK( m.inMacroBody( F, { 17, 2 } ) );
	CHECK( m.map( F, 18, 2 ) == L( 17, 2 ) );
	CHECK( m.inMacroBody( F, { 18, 2 } ) );
	CHECK( m.map( F, 18, 4 ) == L( 17, 4 ) );
	CHECK_FALSE( m.inMacroBody( F, { 18, 4 } ) );
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
		"#define V(f,...) out( f __VA_OPT__(,) __VA_ARGS__ ) + tmp\n"
		"#define V2(a,...) a ## __VA_OPT__( x ## __VA_ARGS__ y ) ## a\n"
		"#define VA(...) __VA_OPT__( tmp ) + tmp\n"
		"#define E \n"
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

// cpp's predefined macros are printed before the file, so a body that uses
// __INT_MAX__ is redone.
TEST_CASE( "sourcemap: a body that uses a predefined macro" ) {
	Files f;
	f.text["/r/pre.cfa"] =
		"#define BIG( a ) a + tmp + __INT_MAX__\n"
		"int w = BIG( tmp );\n";
	std::string prep =
		"# 0 \"/r/pre.cfa\"\n"
		"# 0 \"<built-in>\"\n"
		"#define __INT_MAX__ 0x7fffffff\n"
		"# 0 \"<command-line>\"\n"
		"# 1 \"/r/pre.cfa\"\n"
		"#define BIG(a) a + tmp + __INT_MAX__\n"
		"int w = tmp + tmp + 0x7fffffff;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/pre.cfa";
	CHECK( m.map( F, 2, 8 ) == L( 1, 13 ) );
	CHECK_FALSE( m.inMacroBody( F, { 2, 8 } ) );
	CHECK( m.map( F, 2, 14 ) == L( 1, 8 ) );
	CHECK( m.inMacroBody( F, { 2, 14 } ) );
	CHECK( m.inMacroBody( F, { 2, 20 } ) );
}

// What can't be redone maps as before: an identifier of the expansion that
// the invocation also has is taken to be that one.
TEST_CASE( "sourcemap: macros that can't be redone" ) {
	Files f;
	f.text["/r/no.cfa"] =
		"#define V( f, ... ) out( f __VA_OPT__(,) __VA_ARGS__ ) + tmp\n"
		"#define E\n"
		"int v = V( tmp, E );\n"
		"#define N tmp\n"
		"#define S( a ) a + N\n"
		"#pragma push_macro( \"N\" )\n"						// 5
		"#undef N\n"
		"#pragma pop_macro( \"N\" )\n"
		"int x = S( tmp );\n";
	std::string prep =
		"# 1 \"/r/no.cfa\"\n"
		"#define V(f,...) out( f __VA_OPT__(,) __VA_ARGS__ ) + tmp\n"
		"#define E \n"
		"int v = out( tmp ) + tmp;\n"
		"#define N tmp\n"
		"#define S(a) a + N\n"
		"       \n"
		"#undef N\n"
		"       \n"
		"int x = tmp + tmp;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/no.cfa";
	// gcc 16 leaves out __VA_OPT__ when the variable arguments expand to
	// nothing, and not every version does, so this isn't redone.
	CHECK( m.map( F, 3, 13 ) == L( 2, 11 ) );
	CHECK( m.map( F, 3, 21 ) == L( 2, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 3, 21 } ) );
	// pop_macro brings N back without a #define for -dD to print, so N looks
	// undefined, the redo gives "tmp + N", and both tmps map to the argument.
	CHECK( m.map( F, 9, 8 ) == L( 8, 11 ) );
	CHECK( m.map( F, 9, 14 ) == L( 8, 11 ) );
	CHECK_FALSE( m.inMacroBody( F, { 9, 14 } ) );
}

// The translator reads the -dD output with the #define and #undef lines
// emptied, so every other line keeps its number.
TEST_CASE( "sourcemap: translatorInput" ) {
	CHECK( SourceMap::translatorInput( "# 1 \"/r/a.cfa\"\n#define X 1\nint x;\n#undef X\n#pragma once\n#define\n" ) ==
		   "# 1 \"/r/a.cfa\"\n\nint x;\n\n#pragma once\n#define\n" );
	CHECK( SourceMap::translatorInput( " #define X 1\n#undef X" ) == " #define X 1\n" );
	CHECK( SourceMap::translatorInput( "" ).empty() );
	// A #define after a line, then a marker for a later line, splits nothing.
	CHECK( SourceMap::translatorInput( "# 1 \"/r/a.cfa\"\nint a;\n#define Q 1\n# 9 \"/r/a.cfa\"\nint b;\n" ) ==
		   "# 1 \"/r/a.cfa\"\nint a;\n\n# 9 \"/r/a.cfa\"\nint b;\n" );
	// Two directives in one macro's arguments split the line twice, and the
	// three pieces are joined into one.
	const std::string mark = "# 1 \"/r/t.cfa\"\n";
	CHECK( SourceMap::translatorInput( mark + " x = w + y+w +\n#define S 1\n#define T 1\n" + mark + "                     y\n\n#undef S\n" +
									   mark + "    +w\n    ;\n" ) ==
		   mark + mark + mark + mark + mark + mark + mark + mark + mark + " x = w + y+w + y +w\n    ;\n" );
}

// cpp prints a #define or #undef inside a macro's arguments as soon as it
// reads it: the tokens before the macro, the directive, a marker repeating
// their line, and the rest of the line. The translator gets the line joined
// again (see SourceMap::translatorInput), and so does SourceMap.
TEST_CASE( "sourcemap: a #define inside a macro's arguments" ) {
	Files f;
	f.text["/r/s.cfa"] =
		"#define F(a,b) a+b\n"
		"int y, w;\n"
		"int g() {\n"
		"\tint x = w + F( y,\n"
		"#define Q 2\n"												// 5
		"\tw );\n"
		"\treturn x;\n"
		"}\n";
	std::string prep =
		"# 1 \"/r/s.cfa\"\n"
		"#define F(a,b) a+b\n"
		"int y, w;\n"
		"int g() {\n"
		" int x = w +\n"
		"#define Q 2\n"
		"# 4 \"/r/s.cfa\"\n"
		"             y+w\n"
		"\n"
		"    ;\n"
		" return x;\n"
		"}\n";
	CHECK( SourceMap::translatorInput( prep ) ==
		   "# 1 \"/r/s.cfa\"\n"
		   "\n"
		   "int y, w;\n"
		   "int g() {\n"
		   "# 4 \"/r/s.cfa\"\n"
		   "# 4 \"/r/s.cfa\"\n"
		   "# 4 \"/r/s.cfa\"\n"
		   " int x = w + y+w\n"
		   "\n"
		   "    ;\n"
		   " return x;\n"
		   "}\n" );
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/s.cfa";
	// " int x = w + y+w" at pline 8
	//   0123456789012345
	CHECK( m.map( F, { 4, 9, 8 } ) == L( 3, 9 ) );				// w
	CHECK( m.map( F, { 4, 11, 8 } ) == L( 3, 11 ) );			// +
	CHECK( m.map( F, { 4, 13, 8 } ) == L( 3, 16 ) );			// y, the argument
	CHECK( m.map( F, 4, 13 ) == L( 3, 16 ) );
	CHECK_FALSE( m.inMacroBody( F, { 4, 13, 8 } ) );
	CHECK( m.map( F, { 4, 14, 8 } ) == L( 3, 13 ) );			// the body's +
	CHECK( m.inMacroBody( F, { 4, 14, 8 } ) );
	CHECK( m.map( F, { 4, 15, 8 } ) == L( 5, 1 ) );				// w, the argument on line 6
	CHECK( m.map( F, { 6, 4, 10 } ) == L( 5, 4 ) );				// ;
	CHECK( m.map( F, { 7, 1, 11 } ) == L( 6, 1 ) );				// return
}

// The tokens before the macro were printed with the definitions above the
// directive, and the macro's expansion with the directive's.
TEST_CASE( "sourcemap: definitions printed in the middle of a line" ) {
	Files f;
	f.text["/r/n.cfa"] =
		"#define N 1\n"
		"#define F( a ) a\n"
		"int x = N + F( N\n"
		"#undef N\n"
		"#define N 2\n"												// 5
		" );\n";
	std::string prep =
		"# 1 \"/r/n.cfa\"\n"
		"#define N 1\n"
		"#define F(a) a\n"
		"int x = 1 +\n"
		"#undef N\n"
		"#define N 2\n"
		"# 3 \"/r/n.cfa\"\n"
		"            2\n"
		"\n"
		"\n"
		"  ;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/n.cfa";
	// "int x = 1 + 2" at pline 8
	//  0123456789012
	CHECK( m.map( F, { 3, 8, 8 } ) == L( 2, 8 ) );				// 1: N
	CHECK( m.inMacroBody( F, { 3, 8, 8 } ) );					// redone with N as 1
	CHECK( m.map( F, { 3, 10, 8 } ) == L( 2, 10 ) );			// +
	CHECK( m.map( F, { 3, 12, 8 } ) == L( 2, 12 ) );			// 2: F( N )
	CHECK( m.inMacroBody( F, { 3, 12, 8 } ) );					// redone with N as 2
	CHECK( m.map( F, { 6, 2, 11 } ) == L( 5, 2 ) );				// ;
}

// cpp looks a macro's name up before it reads the arguments, so a use that
// redefines its own macro inside them takes the old body. Both definitions
// give "x = tmp", the first use with tmp from the old body and the second
// with tmp from the argument. cpp prints the definitions before the first
// use, which starts its line, and in the middle of the second, whose line is
// joined.
TEST_CASE( "sourcemap: a use that redefines its own macro in its arguments" ) {
	Files f;
	f.text["/r/j.cfa"] =
		"#define S(a, b) a = tmp\n"
		"int tmp, x;\n"
		"void f() {\n"
		"\tS( x,\n"
		"#undef S\n"												// 5
		"#define S(a, b) a = b\n"
		"\ttmp );\n"
		"\tx = x + S( x,\n"
		"#undef S\n"
		"#define S(a, b) a = tmp\n"									// 10
		"\ttmp );\n"
		"}\n";
	std::string prep =
		"# 1 \"/r/j.cfa\"\n"
		"#define S(a,b) a = tmp\n"
		"int tmp, x;\n"
		"void f() {\n"
		"\n"
		"#undef S\n"
		"#define S(a,b) a = b\n"
		"# 4 \"/r/j.cfa\"\n"
		" x = tmp\n"
		"\n"
		"\n"
		"      ;\n"
		" x = x +\n"
		"#undef S\n"
		"#define S(a,b) a = tmp\n"
		"# 8 \"/r/j.cfa\"\n"
		"         x = tmp\n"
		"\n"
		"\n"
		"      ;\n"
		"}\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/j.cfa";
	// " x = tmp" at pline 9
	//  012345
	CHECK( m.map( F, { 4, 1, 9 } ) == L( 3, 4 ) );				// x, the argument
	CHECK_FALSE( m.inMacroBody( F, { 4, 1, 9 } ) );
	CHECK( m.map( F, { 4, 5, 9 } ) == L( 3, 1 ) );				// tmp from the old body: the use
	CHECK( m.inMacroBody( F, { 4, 5, 9 } ) );
	// " x = x + x = tmp" at pline 17
	//  01234567890123
	CHECK( m.map( F, { 8, 5, 17 } ) == L( 7, 5 ) );				// x before the use
	CHECK( m.map( F, { 8, 9, 17 } ) == L( 7, 12 ) );			// x, the argument
	CHECK( m.map( F, { 8, 13, 17 } ) == L( 10, 1 ) );			// tmp from the argument b
	CHECK_FALSE( m.inMacroBody( F, { 8, 13, 17 } ) );
}

// A #line back to an earlier line gives the same marker after a #define as a
// directive inside a use's arguments, but those definitions came before the
// use. The markers put them on lines 11 to 13, below the use's line 1, and
// the #line 10 above them says they are really on lines 4 to 6.
TEST_CASE( "sourcemap: definitions above a #line that goes back" ) {
	Files f;
	f.text["/r/l.cfa"] =
		"int y;\n"
		"#line 10\n"
		"int z;\n"
		"#define F(a) a + 1\n"
		"#undef F\n"
		"#define F(a) 2 + a\n"
		"#line 1\n"
		"int x = F( x );\n";
	std::string prep =
		"# 1 \"/r/l.cfa\"\n"
		"int y;\n"
		"# 10 \"/r/l.cfa\"\n"
		"int z;\n"
		"#define F(a) a + 1\n"
		"#undef F\n"
		"#define F(a) 2 + a\n"
		"# 1 \"/r/l.cfa\"\n"
		"int x = 2 + x;\n";
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/l.cfa";
	// "int x = 2 + x;" at pline 9
	//  012345678901
	CHECK( m.map( F, { 1, 8, 9 } ) == L( 7, 8 ) );				// 2: the use
	CHECK( m.inMacroBody( F, { 1, 8, 9 } ) );					// redone with the last F
	CHECK( m.map( F, { 1, 12, 9 } ) == L( 7, 11 ) );			// x, the argument
	CHECK_FALSE( m.inMacroBody( F, { 1, 12, 9 } ) );
}

// When the marker after the directive switches flag 3, plain cpp splits the
// line there too. Only the lines between the pieces change, so the second
// piece continues the line.
TEST_CASE( "sourcemap: a #define inside a system-header macro's arguments" ) {
	Files f;
	f.text["/r/c.cfa"] =
		"#include <hd.h>\n"
		"int w, y;\n"
		"int z = w + isd( y\n"
		"#define R 2\n"
		" );\n";
	std::string prep =
		"# 1 \"/r/c.cfa\"\n"
		"# 1 \"/r/hd.h\" 1 3\n"
		"#define isd(c) ( tab[c] & 8 )\n"
		"# 2 \"/r/c.cfa\" 2\n"
		"int w, y;\n"
		"int z = w +\n"
		"#define R 2\n"
		"# 3 \"/r/c.cfa\" 3\n"
		"            ( tab[\n"
		"# 3 \"/r/c.cfa\"\n"
		"           y\n"
		"# 3 \"/r/c.cfa\" 3\n"
		"           ] & 8 )\n"
		"\n"
		"  \n"
		"# 5 \"/r/c.cfa\"\n"
		" ;\n";
	// What plain cpp prints, but for the emptied #define and the marker for
	// the line after "int z = w + ".
	CHECK( SourceMap::translatorInput( prep ) ==
		   "# 1 \"/r/c.cfa\"\n"
		   "# 1 \"/r/hd.h\" 1 3\n"
		   "\n"
		   "# 2 \"/r/c.cfa\" 2\n"
		   "int w, y;\n"
		   "int z = w + \n"
		   "# 4 \"/r/c.cfa\"\n"
		   "# 3 \"/r/c.cfa\" 3\n"
		   "           ( tab[\n"
		   "# 3 \"/r/c.cfa\"\n"
		   "           y\n"
		   "# 3 \"/r/c.cfa\" 3\n"
		   "           ] & 8 )\n"
		   "\n"
		   "  \n"
		   "# 5 \"/r/c.cfa\"\n"
		   " ;\n" );
	SourceMap m( prep, f.reader() );
	const std::string F = "/r/c.cfa";
	CHECK( m.map( F, { 3, 10, 6 } ) == L( 2, 10 ) );			// +
	CHECK( m.map( F, { 3, 11, 11 } ) == L( 2, 17 ) );			// y, the argument
	CHECK( m.map( F, { 3, 11, 9 } ) == L( 2, 12 ) );			// ( from the body: isd
	CHECK( m.inMacroBody( F, { 3, 13, 9 } ) );					// tab
	CHECK( m.map( F, { 5, 1, 17 } ) == L( 4, 2 ) );				// ;
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
		"#define ADD(a,b) ((a) + (b))\n"
		"#define EMPTY \n"
		"#define SWAP(a,b) b a\n"
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
	SourceMap m2( "# 1 \"/r/mac.cfa\"\n#define X(n) ((n)*2)\n" + p2 + "\n", g.reader() );
	CHECK( m2.mapRange( "/r/mac.cfa", 2, p2Col, 2, p2Col + 6 ) == Range{ L( 1, o2Col ), L( 1, o2Col + 6 ) } );
	// A token from a macro maps to its invocation.
	int x = int( p2.find( "((399)" ) );
	int xo = int( o2.find( "X(399)" ) ) - 21;
	CHECK( m2.mapRange( "/r/mac.cfa", 2, x, 2, x + 1 ) == Range{ L( 1, xo ), L( 1, xo + 6 ) } );
}
