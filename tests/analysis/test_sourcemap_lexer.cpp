#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "Lexer.hpp"

using namespace cfalsp;

namespace {

std::vector<std::string> texts( std::string_view src, const LexOptions & opts = {} ) {
	std::vector<std::string> out;
	for ( const Token & t : lex( src, opts ) ) out.push_back( t.text );
	return out;
}

using V = std::vector<std::string>;

} // namespace

TEST_CASE( "lexer: identifiers, numbers and literals" ) {
	CHECK( texts( "int $x_1 = 0x1fUL + 1.5e-3f + .5 + 1e+10 + 0x1p-3 + 1_000;" ) ==
		   V{ "int", "$x_1", "=", "0x1fUL", "+", "1.5e-3f", "+", ".5", "+", "1e+10", "+", "0x1p-3", "+", "1_000", ";" } );
	CHECK( texts( R"(u8"a b" u"c" U'd' L"e\"f" 'x' '\'' "g\\")" ) ==
		   V{ R"(u8"a b")", R"(u"c")", "U'd'", R"(L"e\"f")", "'x'", R"('\'')", R"("g\\")" } );
	auto ts = lex( "u8\"s\" L'c' x" );
	REQUIRE( ts.size() == 3 );
	CHECK( ts[0].kind == TokKind::String );
	CHECK( ts[1].kind == TokKind::Char );
	CHECK( ts[2].kind == TokKind::Identifier );
	// Unterminated literals stop at the end of the line.
	CHECK( texts( "\"abc\nx 'y\nz" ) == V{ "\"abc", "x", "'y", "z" } );
}

TEST_CASE( "lexer: punctuators by longest match" ) {
	CHECK( texts( "a->b ... <<= >>= ++ -- && || ## != == <= >=" ) ==
		   V{ "a", "->", "b", "...", "<<=", ">>=", "++", "--", "&&", "||", "##", "!=", "==", "<=", ">=" } );
	CHECK( texts( "x @= y; 0 ~ 10; 1 -~= 5; a ~= b; s | t; z \\ 2; @[x]" ) ==
		   V{ "x", "@=", "y", ";", "0", "~", "10", ";", "1", "-~=", "5", ";", "a", "~=", "b", ";",
			  "s", "|", "t", ";", "z", "\\", "2", ";", "@[", "x", "]" } );
	LexOptions c;
	c.cfa = false;
	CHECK( texts( "x @= y -~ z", c ) == V{ "x", "@", "=", "y", "-", "~", "z" } );
}

TEST_CASE( "lexer: CFA operator names" ) {
	CHECK( texts( "void ?{}( S & ); void ^?{}( S & ); S ?+?( S, S ); int ?[?]( A, int ); ?++ ++? -? ?<<=? ?()" ) ==
		   V{ "void", "?{}", "(", "S", "&", ")", ";", "void", "^?{}", "(", "S", "&", ")", ";", "S", "?+?", "(", "S", ",",
			  "S", ")", ";", "int", "?[?]", "(", "A", ",", "int", ")", ";", "?++", "++?", "-?", "?<<=?", "?()" } );
	auto ts = lex( "?{} ?`ms ``x" );
	REQUIRE( ts.size() == 3 );
	CHECK( ts[0].kind == TokKind::Identifier );
	CHECK( ts[1].text == "?`ms" );
	CHECK( ts[2].text == "``x" );
	// A plain conditional is not an operator name.
	CHECK( texts( "a ? b : c" ) == V{ "a", "?", "b", ":", "c" } );
}

TEST_CASE( "lexer: comments, directives and splices" ) {
	std::string src =
		"#include <stdio.h>\n"
		"#define F(x) \\\n"
		"\t((x) + 1) /* c\n"
		"   still comment */ int after_define;\n"
		"  # if 0 // indented\n"
		"int a; // line comment \\\n"
		"continued\n"
		"/* lead */ # pragma once\n"
		"int /* mid */ b = 'c'; /* multi\n"
		"line */ int lo\\\n"
		"ng = 1;\n";
	CHECK( texts( src ) == V{ "int", "a", ";", "int", "b", "=", "'c'", ";", "int", "long", "=", "1", ";" } );

	auto ts = lex( src );
	const Token & lng = ts[9];
	CHECK( lng.text == "long" );
	CHECK( lng.line == 9 );
	CHECK( lng.col == 12 );
	CHECK( lng.endLine == 10 );
	CHECK( lng.endCol == 2 );
	CHECK( std::string_view( src ).substr( lng.offset, lng.endOffset - lng.offset ) == "lo\\\nng" );

	LexOptions o;
	o.comments = true;
	o.directives = true;
	auto all = lex( src, o );
	REQUIRE( all.size() >= 4 );
	CHECK( all[0].kind == TokKind::Directive );
	CHECK( all[0].text == "#include <stdio.h>" );
	CHECK( all[1].kind == TokKind::Directive );
	CHECK( all[1].line == 1 );
	// The block comment keeps the directive going, so after_define is part of it.
	CHECK( all[1].endLine == 3 );
	CHECK( all[1].text.find( "after_define" ) != std::string::npos );
	CHECK( all[2].kind == TokKind::Directive );
	CHECK( all[2].line == 4 );
	CHECK( all[3].text == "int" );
	REQUIRE( all.size() >= 7 );
	CHECK( all[6].kind == TokKind::Comment );
	CHECK( all[6].text == "// line comment continued" );
}

TEST_CASE( "lexer: positions" ) {
	auto ts = lex( "\tint  x;\r\n  y" );
	REQUIRE( ts.size() == 4 );
	CHECK( ts[0].line == 0 );
	CHECK( ts[0].col == 1 );
	CHECK( ts[0].endCol == 4 );
	CHECK( ts[1].col == 6 );
	CHECK( ts[3].line == 1 );
	CHECK( ts[3].col == 2 );
	CHECK( ts[3].offset == 12 );
	CHECK( lex( "" ).empty() );
	CHECK( lex( "\\" ).size() == 1 );
	CHECK( texts( "a\\\n" ) == V{ "a" } );
	CHECK( texts( "/* open" ).empty() );
}
