#include <doctest/doctest.h>

#include "TextScan.hpp"

using namespace cfalsp;
using namespace cfalsp::text;

namespace {

std::vector<std::string> spans( const std::string & sig, const std::string & name ) {
	std::vector<std::string> out;
	for ( auto [b, e] : paramSpans( sig, name ) ) out.push_back( sig.substr( b, e - b ) );
	return out;
}

} // namespace

TEST_SUITE( "analysis/text" ) {

TEST_CASE( "FileText lines and slices" ) {
	FileText f( "ab\r\ncd\n\nef" );
	CHECK( f.lineCount() == 4 );
	CHECK( f.line( 0 ) == "ab" );
	CHECK( f.line( 2 ) == "" );
	CHECK( f.line( 3 ) == "ef" );
	CHECK( f.line( 9 ) == "" );
	CHECK( f.slice( { { 0, 1 }, { 1, 1 } } ) == "b\nc" );
	CHECK( f.slice( { { 3, 0 }, { 3, 50 } } ) == "ef" );
	CHECK( FileText( "x\n" ).lineCount() == 1 );
}

TEST_CASE( "doc comments above a declaration" ) {
	FileText f(
		"int a;\n"						// 0
		"// First line.\n"				// 1
		"///  Second line.\n"			// 2
		"int b;\n"						// 3
		"\n"							// 4
		"/**\n"							// 5
		" * Block doc.\n"				// 6
		" *   indented\n"				// 7
		" */\n"							// 8
		"int c;\n"						// 9
		"/* one line */\n"				// 10
		"int d;\n"						// 11
		"int e; /* trailing */\n"		// 12
		"int f;\n"						// 13
		"////////////\n"				// 14
		"// Banner //\n"				// 15
		"////////////\n"				// 16
		"int g;\n"						// 17
		"// separated\n"				// 18
		"\n"							// 19
		"int h;\n" );					// 20
	CHECK( docAbove( f, 0 ) == "" );
	CHECK( docAbove( f, 3 ) == "First line.\n Second line." );
	CHECK( docAbove( f, 9 ) == "Block doc.\n  indented" );
	CHECK( docAbove( f, 11 ) == "one line" );
	CHECK( docAbove( f, 13 ) == "" );			// the comment belongs to e
	CHECK( docAbove( f, 17 ) == "Banner //" );
	CHECK( docAbove( f, 20 ) == "" );
}

TEST_CASE( "trailing doc comments" ) {
	FileText f(
		"\tint x;\t// x doc\n"
		"\tint a, b; // shared\n"
		"\tint c; int d; // d only\n"
		"\tchar * s = \"//\"; // real\n"
		"\tint e; /*< block */\n"
		"\tint f( int, int ); // after a call\n" );
	CHECK( docTrailing( f, { 0, 6 } ) == "x doc" );
	CHECK( docTrailing( f, { 1, 6 } ) == "" );			// b follows a
	CHECK( docTrailing( f, { 1, 9 } ) == "shared" );
	CHECK( docTrailing( f, { 2, 6 } ) == "" );
	CHECK( docTrailing( f, { 3, 9 } ) == "real" );
	CHECK( docTrailing( f, { 4, 6 } ) == "block" );
	CHECK( docTrailing( f, { 5, 5 } ) == "after a call" );
}

TEST_CASE( "identifier under the cursor" ) {
	CHECK( identifierAt( "  foo.bar", 2 ) == std::pair( 2, 5 ) );
	CHECK( identifierAt( "  foo.bar", 5 ) == std::pair( 2, 5 ) );	// right after
	CHECK( identifierAt( "  foo.bar", 6 ) == std::pair( 6, 9 ) );
	CHECK( identifierAt( "  foo.bar", 9 ) == std::pair( 6, 9 ) );
	CHECK_FALSE( identifierAt( "  foo.bar", 1 ) );
	CHECK_FALSE( identifierAt( "x = 12", 5 ) );
	CHECK_FALSE( identifierAt( "abc", 7 ) );
	CHECK_FALSE( identifierAt( "abc", -1 ) );
}

TEST_CASE( "parameter spans" ) {
	CHECK( spans( "forall( T ) void push( Stack(T) & s, T x )", "push" ) == std::vector<std::string>{ "Stack(T) & s", "T x" } );
	CHECK( spans( "int main()", "main" ).empty() );
	CHECK( spans( "int f( void )", "f" ).empty() );
	CHECK( spans( "void ?{}( Point & )", "?{}" ) == std::vector<std::string>{ "Point &" } );
	CHECK( spans( "extern int printf( const char * __format, ... )", "printf" ) == std::vector<std::string>{ "const char * __format", "..." } );
	CHECK( spans( "void (*signal( int sig, void (*f)( int ) ))( int )", "signal" ) == std::vector<std::string>{ "int sig", "void (*f)( int )" } );
	CHECK( spans( "forall( T | { void f( T ); } ) void f( T x, T y )", "f" ) == std::vector<std::string>{ "T x", "T y" } );
	CHECK( spans( "void push_back( T x )", "push" ) == std::vector<std::string>{ "T x" } );		// falls back to the last list
	CHECK( spans( "int x", "x" ).empty() );
}

TEST_CASE( "identifier chains and with clauses" ) {
	CHECK( identChain( "a . b->c" ) == std::vector<std::string>{ "a", "b", "c" } );
	CHECK( identChain( "a" ) == std::vector<std::string>{ "a" } );
	CHECK( identChain( "f(x).b" ).empty() );
	CHECK( identChain( "" ).empty() );

	CHECK( withClauseAtEnd( ") with( c ) " ) == std::vector<std::string>{ "c" } );
	CHECK( withClauseAtEnd( "with ( a, b.c ) {" ) == std::vector<std::string>{ "a", "b.c" } );
	CHECK( withClauseAtEnd( "\n\twith( s )\n\t" ) == std::vector<std::string>{ "s" } );
	CHECK( withClauseAtEnd( "for ( i; 4 ) " ).empty() );
	CHECK( withClauseAtEnd( "notwith( x )" ).empty() );
	CHECK( withClauseAtEnd( "x;" ).empty() );
	CHECK( withClauseAtEnd( ")" ).empty() );
}

TEST_CASE( "call sites" ) {
	auto c = callBefore( "f( a, g( b ), " );
	REQUIRE( c );
	CHECK( c->name == "f" );
	CHECK( c->nameEnd == 1 );
	CHECK( c->argIndex == 2 );

	c = callBefore( "x = foo  ( \"a,b\", ',', [1,2], {3,4}, " );
	REQUIRE( c );
	CHECK( c->name == "foo" );
	CHECK( c->argIndex == 4 );

	c = callBefore( "#define F(x, y\nint main() { bar( " );
	REQUIRE( c );
	CHECK( c->name == "bar" );

	c = callBefore( "sout | ?|?( sout, " );
	REQUIRE( c );
	CHECK( c->name == "?|?" );
	CHECK( c->argIndex == 1 );

	c = callBefore( "a[ f( 1, " );
	REQUIRE( c );
	CHECK( c->name == "f" );

	c = callBefore( "f( a[ 1, " );
	REQUIRE( c );
	CHECK( c->name == "f" );
	CHECK( c->argIndex == 0 );

	CHECK_FALSE( callBefore( "f( a ) " ) );
	CHECK_FALSE( callBefore( "f( { " ) );
	CHECK_FALSE( callBefore( "f( /* " ) );
	CHECK_FALSE( callBefore( "( 1, " ) );
	CHECK_FALSE( callBefore( "1( " ) );
}

TEST_CASE( "completion contexts" ) {
	auto c = completionContext( "\tfoo" );
	CHECK( c.kind == CompletionContext::Ident );
	CHECK( c.prefix == "foo" );

	c = completionContext( "" );
	CHECK( c.kind == CompletionContext::Ident );
	CHECK( c.prefix == "" );

	c = completionContext( "\tx = a.b->c.de" );
	CHECK( c.kind == CompletionContext::Member );
	CHECK( c.prefix == "de" );
	CHECK( c.chain == std::vector<std::string>{ "a", "b", "c" } );
	CHECK( c.operand == "a.b->c" );
	CHECK( c.operandStart == 5 );
	CHECK( c.operandEnd == 11 );

	c = completionContext( "\tf( x ).g[ 2 ]." );
	CHECK( c.kind == CompletionContext::Member );
	CHECK( c.chain == std::vector<std::string>{ "f", "g" } );
	CHECK( c.operand == "f(x).g[2]" );

	c = completionContext( "\t( *p )." );
	CHECK( c.kind == CompletionContext::Member );
	CHECK( c.chain.empty() );
	CHECK( c.operand == "(*p)" );

	CHECK( completionContext( "\tx = 1." ).kind == CompletionContext::None );
	CHECK( completionContext( "\tx = 1.5" ).kind == CompletionContext::None );
	CHECK( completionContext( "\tf( ... " ).kind == CompletionContext::Ident );
	CHECK( completionContext( "\t\"a.b" ).kind == CompletionContext::None );
	CHECK( completionContext( "\t'.'" ).kind == CompletionContext::Ident );
	CHECK( completionContext( "\t// x." ).kind == CompletionContext::None );
	CHECK( completionContext( "  #if X" ).kind == CompletionContext::None );
	CHECK( completionContext( "\t) ." ).kind == CompletionContext::None );
}

TEST_CASE( "declarations found in text" ) {
	std::string text = "void f( Pair( int ) & p, Node ** q ) {\n\tstruct S s = { 0 };\n\tif ( c ) s = t;\n\treturn s;\n}\n";
	auto typeOf = [&]( const TextDeclaration & d ) { return text.substr( d.typeStart, d.typeEnd - d.typeStart ); };
	auto nameOf = [&]( const TextDeclaration & d ) { return text.substr( d.nameStart, d.nameEnd - d.nameStart ); };

	auto ds = declarationsOf( text, "p" );
	REQUIRE( ds.size() == 1 );
	CHECK( ds[0].typeName == "Pair" );
	CHECK( typeOf( ds[0] ) == "Pair( int ) &" );
	CHECK( nameOf( ds[0] ) == "p" );
	CHECK( ds[0].pointers == 0 );
	ds = declarationsOf( text, "q" );
	REQUIRE( ds.size() == 1 );
	CHECK( ds[0].typeName == "Node" );
	CHECK( ds[0].pointers == 2 );
	// `if ( c ) s = t;` and `return s;` are not declarations.
	ds = declarationsOf( text, "s" );
	REQUIRE( ds.size() == 1 );
	CHECK( typeOf( ds[0] ) == "struct S" );
	// Only before `end`.
	CHECK( declarationsOf( text, "s", text.find( "s =" ) ).empty() );

	auto n = declarationsAt( text, text.find( "return s" ) + 8 );
	REQUIRE( n );
	CHECK( n->name == "s" );
	CHECK( n->start == text.find( "return s" ) + 7 );
	REQUIRE( n->declarations.size() == 1 );
	CHECK( n->declarations[0].nameStart == text.find( "s =" ) );
	CHECK_FALSE( declarationsAt( text, text.find( "0 }" ) ) );			// a number
	CHECK_FALSE( declarationsAt( "// Rect r;\n", 8 ) );					// a comment
	CHECK_FALSE( declarationsAt( "f( \"r\" );", 4 ) );					// a string
}

} // TEST_SUITE
