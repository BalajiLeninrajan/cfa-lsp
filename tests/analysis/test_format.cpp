#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Format.hpp"
#include "Lexer.hpp"

using namespace cfalsp;
namespace fs = std::filesystem;

namespace {

FormatOptions tabs() { return {}; }

FormatOptions spaces( int n = 4 ) {
	FormatOptions o;
	o.tabSize = n;
	o.insertSpaces = true;
	return o;
}

std::string fixtures() {
	const char * env = std::getenv( "CFA_LSP_FIXTURES" );
	return env && *env ? env : "tests/fixtures";
}

std::string slurp( const fs::path & p ) {
	std::ifstream in( p, std::ios::binary );
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// Each line of a comment or directive without its leading and trailing
// whitespace: the formatter may move a block comment's lines and trim the
// end of a line comment, and nothing else.
std::string trimLines( const std::string & s ) {
	std::string out;
	std::istringstream in( s );
	std::string line;
	while ( std::getline( in, line ) ) {
		size_t b = line.find_first_not_of( " \t\r" ), e = line.find_last_not_of( " \t\r" );
		out += b == std::string::npos ? "" : line.substr( b, e - b + 1 );
		out += '\n';
	}
	return out;
}

struct Tok {
	TokKind kind;
	std::string text;
	bool operator==( const Tok & ) const = default;
};

std::vector<Tok> tokens( const std::string & s, bool everything ) {
	LexOptions lo;
	lo.comments = everything;
	lo.directives = everything;
	std::vector<Tok> out;
	for ( const Token & t : lex( s, lo ) ) {
		bool loose = t.kind == TokKind::Comment || t.kind == TokKind::Directive;
		out.push_back( { t.kind, loose ? trimLines( t.text ) : t.text } );
	}
	return out;
}

std::string withoutSpace( const std::string & s ) {
	std::string out;
	for ( char c : s ) if ( c != ' ' && c != '\t' && c != '\r' && c != '\n' ) out += c;
	return out;
}

// The formatter's promise: the same tokens (code tokens exactly), only
// whitespace changed, and a second run changes nothing.
void checkSafe( const std::string & name, const std::string & src, const FormatOptions & o ) {
	INFO( name );
	std::string out = formatText( src, o );
	CHECK( tokens( out, false ) == tokens( src, false ) );
	CHECK( tokens( out, true ) == tokens( src, true ) );
	CHECK( withoutSpace( out ) == withoutSpace( src ) );
	CHECK( formatText( out, o ) == out );
}

} // namespace

TEST_SUITE( "analysis" ) {

TEST_CASE( "format: lines are indented by brace depth" ) {
	std::string src =
		"int main() {\n"
		"int x = 1;\n"
		"  if ( x ) {\n"
		"        x += 1;\n"
		"    }\n"
		"return x;\n"
		"}\n";
	CHECK( formatText( src, tabs() ) ==
		   "int main() {\n"
		   "\tint x = 1;\n"
		   "\tif ( x ) {\n"
		   "\t\tx += 1;\n"
		   "\t}\n"
		   "\treturn x;\n"
		   "}\n" );
	CHECK( formatText( src, spaces( 2 ) ) ==
		   "int main() {\n"
		   "  int x = 1;\n"
		   "  if ( x ) {\n"
		   "    x += 1;\n"
		   "  }\n"
		   "  return x;\n"
		   "}\n" );
}

TEST_CASE( "format: case labels and the statements under them" ) {
	std::string src =
		"void f( int x ) {\n"
		"switch ( x ) {\n"
		"case 1:\n"
		"foo();\n"
		"break;\n"
		"default:\n"
		"bar();\n"
		"}\n"
		"}\n";
	CHECK( formatText( src, spaces() ) ==
		   "void f( int x ) {\n"
		   "    switch ( x ) {\n"
		   "        case 1:\n"
		   "            foo();\n"
		   "            break;\n"
		   "        default:\n"
		   "            bar();\n"
		   "    }\n"
		   "}\n" );
}

TEST_CASE( "format: bodies without braces are one level in" ) {
	std::string src =
		"int g( int x ) {\n"
		"if ( x > 0 )\n"
		"return 1;\n"
		"else if ( x < 0 )\n"
		"return -1;\n"
		"else\n"
		"return 0;\n"
		"for ( i; 10 )\n"
		"if ( i )\n"
		"x += i;\n"
		"while ( x )\n"
		"{\n"
		"x -= 1;\n"
		"}\n"
		"}\n";
	CHECK( formatText( src, spaces() ) ==
		   "int g( int x ) {\n"
		   "    if ( x > 0 )\n"
		   "        return 1;\n"
		   "    else if ( x < 0 )\n"
		   "        return -1;\n"
		   "    else\n"
		   "        return 0;\n"
		   "    for ( i; 10 )\n"
		   "        if ( i )\n"
		   "            x += i;\n"
		   "    while ( x )\n"
		   "    {\n"
		   "        x -= 1;\n"
		   "    }\n"
		   "}\n" );
}

TEST_CASE( "format: an else lines up with its if inside another body" ) {
	std::string src =
		"void h( int n ) {\n"
		"for ( i; n )\n"
		"if ( i ) {\n"
		"a();\n"
		"} else {\n"
		"b();\n"
		"}\n"
		"for ( i; n )\n"
		"if ( i )\n"
		"a();\n"
		"else\n"
		"b();\n"
		"if ( n )\n"
		"if ( n > 1 )\n"
		"a();\n"
		"else\n"
		"b();\n"
		"else\n"
		"c();\n"
		"d();\n"
		"}\n";
	CHECK( formatText( src, spaces() ) ==
		   "void h( int n ) {\n"
		   "    for ( i; n )\n"
		   "        if ( i ) {\n"
		   "            a();\n"
		   "        } else {\n"
		   "            b();\n"
		   "        }\n"
		   "    for ( i; n )\n"
		   "        if ( i )\n"
		   "            a();\n"
		   "        else\n"
		   "            b();\n"
		   "    if ( n )\n"
		   "        if ( n > 1 )\n"
		   "            a();\n"
		   "        else\n"
		   "            b();\n"
		   "    else\n"
		   "        c();\n"
		   "    d();\n"
		   "}\n" );
}

TEST_CASE( "format: continuation lines keep their alignment" ) {
	std::string src =
		"int main() {\n"
		"        int total = add( 1,\n"
		"                         2 );\n"
		"        sout | total\n"
		"             | nl;\n"
		"}\n";
	CHECK( formatText( src, spaces() ) ==
		   "int main() {\n"
		   "    int total = add( 1,\n"
		   "                     2 );\n"
		   "    sout | total\n"
		   "         | nl;\n"
		   "}\n" );
	// With tabs, the width is kept: tabs first, then spaces.
	CHECK( formatText( src, tabs() ) ==
		   "int main() {\n"
		   "\tint total = add( 1,\n"
		   "\t\t\t\t\t 2 );\n"
		   "\tsout | total\n"
		   "\t\t | nl;\n"
		   "}\n" );
}

TEST_CASE( "format: CFA declarations come through unchanged apart from whitespace" ) {
	std::string src =
		"forall( T )\n"
		"struct Pair {\n"
		"T first, second;\n"
		"};\n"
		"void ?{}( Pair(int) & p ) with( p ) {\n"
		"first = 0; second = 0;\n"
		"}\n"
		"void ^?{}( Pair(int) & p ) {}\n"
		"int ?`len( Pair(int) p ) { return 2; }\n";
	CHECK( formatText( src, tabs() ) ==
		   "forall( T )\n"
		   "struct Pair {\n"
		   "\tT first, second;\n"
		   "};\n"
		   "void ?{}( Pair( int ) & p ) with( p ) {\n"
		   "\tfirst = 0; second = 0;\n"
		   "}\n"
		   "void ^?{}( Pair( int ) & p ) {}\n"
		   "int ?`len( Pair( int ) p ) { return 2; }\n" );
}

TEST_CASE( "format: spacing within a line" ) {
	std::string src =
		"int main() {\n"
		"for  (   i;   10     )        {\n"
		"f(a ,b);   g( );\n"
		"if(x){ y; }\n"
		"}\n"
		"while (x)\t  x -= 1 ;\n"
		"for (;;) {}\n"
		"sout  |  f (x)  |  nl;\n"
		"}\n"
		"int z = ({ 1; });  // kept\n"
		"#define M(a)   ( a ,b )\n";
	CHECK( formatText( src, tabs() ) ==
		   "int main() {\n"
		   "\tfor ( i; 10 ) {\n"
		   "\t\tf( a, b ); g();\n"
		   "\t\tif ( x ) { y; }\n"
		   "\t}\n"
		   "\twhile ( x ) x -= 1;\n"
		   "\tfor ( ;; ) {}\n"
		   "\tsout | f ( x ) | nl;\n"
		   "}\n"
		   "int z = ({ 1; });  // kept\n"
		   "#define M(a)   ( a ,b )\n" );
}

TEST_CASE( "format: comments move with their line, directives stay" ) {
	std::string src =
		"struct S {\n"
		"        /**\n"
		"         * doc\n"
		"         */\n"
		"        int x;   \n"
		"        int y; // trailing   \n"
		"};\n"
		"  #define X 1\n"
		"#define M( x ) \\\n"
		"        do { x; } while ( 0 )\n"
		"int f() {\n"
		"#ifdef A\n"
		"return 1;\n"
		"#endif\n"
		"}\n";
	CHECK( formatText( src, spaces() ) ==
		   "struct S {\n"
		   "    /**\n"
		   "     * doc\n"
		   "     */\n"
		   "    int x;\n"
		   "    int y; // trailing\n"
		   "};\n"
		   "  #define X 1\n"
		   "#define M( x ) \\\n"
		   "        do { x; } while ( 0 )\n"
		   "int f() {\n"
		   "#ifdef A\n"
		   "    return 1;\n"
		   "#endif\n"
		   "}\n" );
}

TEST_CASE( "format: whitespace inside tokens and before a backslash stays" ) {
	// An unterminated string ends at the line end, so its trailing spaces are
	// part of it; trimming the spaces after a backslash would make a splice.
	std::string src = "a = \"abc   \nb = c \\   \nd;\n";
	CHECK( formatText( src, tabs() ) == src );
	// Line endings are kept.
	CHECK( formatText( "int main() {\r\nreturn 0;   \r\n}\r\n", tabs() ) == "int main() {\r\n\treturn 0;\r\n}\r\n" );
	// Blank lines lose their whitespace unless trimming is off.
	CHECK( formatText( "int x;\n   \nint y;\n", tabs() ) == "int x;\n\nint y;\n" );
	FormatOptions keep = tabs();
	keep.trimTrailingWhitespace = false;
	CHECK( formatText( "int x;   \n   \nint y;\n", keep ) == "int x;   \n   \nint y;\n" );
}

TEST_CASE( "format: a range changes only its lines" ) {
	std::string src = "int main() {\nint a;\nint b;\nint c;\n}\n";
	CHECK( formatText( src, tabs(), 2, 2 ) == "int main() {\nint a;\n\tint b;\nint c;\n}\n" );
	CHECK( formatText( src, tabs(), 1, 3 ) == "int main() {\n\tint a;\n\tint b;\n\tint c;\n}\n" );
	// A continuation line moves only with its statement's first line.
	std::string cont = "int main() {\n        f( 1,\n           2 );\n}\n";
	CHECK( formatText( cont, spaces(), 2, 2 ) == cont );
	CHECK( formatText( cont, spaces(), 1, 2 ) == "int main() {\n    f( 1,\n       2 );\n}\n" );
}

TEST_CASE( "format: final newlines" ) {
	FormatOptions trim = tabs();
	trim.trimFinalNewlines = true;
	CHECK( formatText( "int x;\n\n\n", trim ) == "int x;\n" );
	CHECK( formatText( "int x;\r\n\r\n", trim ) == "int x;\r\n" );
	CHECK( formatText( "int x;", trim ) == "int x;" );
	FormatOptions insert = tabs();
	insert.insertFinalNewline = true;
	CHECK( formatText( "int x;", insert ) == "int x;\n" );
	CHECK( formatText( "int x;\r\nint y;", insert ) == "int x;\r\nint y;\r\n" );
	CHECK( formatText( "int x;\n", insert ) == "int x;\n" );
	CHECK( formatText( "a \\", insert ) == "a \\" );			// a newline would make a splice
	// Only when formatting reaches the last line.
	CHECK( formatText( "int x;\nint y;", insert, 0, 0 ) == "int x;\nint y;" );
}

TEST_CASE( "format: never changes a token, on every fixture" ) {
	std::vector<fs::path> files;
	for ( const auto & e : fs::recursive_directory_iterator( fixtures() ) ) {
		std::string ext = e.path().extension().string();
		if ( e.is_regular_file() && ( ext == ".cfa" || ext == ".hfa" ) ) files.push_back( e.path() );
	}
	REQUIRE( files.size() > 20 );
	FormatOptions both = spaces( 2 );
	both.insertFinalNewline = true;
	both.trimFinalNewlines = true;
	for ( const auto & f : files ) {
		std::string src = slurp( f );
		checkSafe( f.string() + " (tabs)", src, tabs() );
		checkSafe( f.string() + " (spaces)", src, both );
	}

	// And on text written to trip it up.
	std::string tricky =
		"#define LONG( x ) \\\n"
		"    ( (x) + \\\n"
		"      1 )\n"
		"   /* a block\n"
		"      comment */ int a = 1; /* another\n"
		"   one */\n"
		"char * s = \"open   \n"
		"int b = 2 \\   \n"
		"  ;\n"
		"int sp\\\n"
		"lice = 3;\n"
		"  // line comment \\\n"
		"     continued\n"
		"void f() { if ( a ) { b; } else\n"
		"c; switch ( a ) { case 1: { d; } default: e; } }\n"
		"int g() {\n"
		"\treturn ({ int t = 1;\n"
		"\t\tt; });\n"
		"}\n"
		"c ?( ) : d; e ?(\t) : f;\n"
		"} } ) ] unbalanced {\n"
		"x;\n"
		"/* unterminated\n"
		"   comment";
	checkSafe( "tricky (tabs)", tricky, tabs() );
	checkSafe( "tricky (spaces)", tricky, both );
	checkSafe( "tricky (crlf)", [&] {
		std::string crlf;
		for ( char c : tricky ) crlf += c == '\n' ? std::string( "\r\n" ) : std::string( 1, c );
		return crlf;
	}(), tabs() );
}

} // TEST_SUITE
