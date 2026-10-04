#include <doctest/doctest.h>

#include "server/Text.hpp"
#include "server/Uri.hpp"

using namespace cfalsp;

// "aé😀b": a (1 byte, 1 unit), é (2 bytes, 1 unit), 😀 (4 bytes, 2 units), b
static const std::string mixed = "a\xC3\xA9\xF0\x9F\x98\x80" "b";

TEST_CASE( "UTF-16 columns to bytes" ) {
	CHECK( toByteCol( mixed, 0, Encoding::Utf16 ) == 0 );
	CHECK( toByteCol( mixed, 1, Encoding::Utf16 ) == 1 );
	CHECK( toByteCol( mixed, 2, Encoding::Utf16 ) == 3 );
	CHECK( toByteCol( mixed, 3, Encoding::Utf16 ) == 3 );	// between surrogate halves: rounds down
	CHECK( toByteCol( mixed, 4, Encoding::Utf16 ) == 7 );
	CHECK( toByteCol( mixed, 5, Encoding::Utf16 ) == 8 );
	CHECK( toByteCol( mixed, 99, Encoding::Utf16 ) == 8 );	// clamps
	CHECK( toByteCol( mixed, -1, Encoding::Utf16 ) == 0 );
}

TEST_CASE( "bytes to UTF-16 columns" ) {
	CHECK( fromByteCol( mixed, 0, Encoding::Utf16 ) == 0 );
	CHECK( fromByteCol( mixed, 1, Encoding::Utf16 ) == 1 );
	CHECK( fromByteCol( mixed, 2, Encoding::Utf16 ) == 1 );	// inside é
	CHECK( fromByteCol( mixed, 3, Encoding::Utf16 ) == 2 );
	CHECK( fromByteCol( mixed, 5, Encoding::Utf16 ) == 2 );	// inside the emoji
	CHECK( fromByteCol( mixed, 7, Encoding::Utf16 ) == 4 );
	CHECK( fromByteCol( mixed, 8, Encoding::Utf16 ) == 5 );
	CHECK( fromByteCol( mixed, 100, Encoding::Utf16 ) == 5 );
}

TEST_CASE( "UTF-8 columns are bytes, rounded to character starts" ) {
	CHECK( toByteCol( mixed, 3, Encoding::Utf8 ) == 3 );
	CHECK( toByteCol( mixed, 2, Encoding::Utf8 ) == 1 );
	CHECK( toByteCol( mixed, 5, Encoding::Utf8 ) == 3 );
	CHECK( fromByteCol( mixed, 7, Encoding::Utf8 ) == 7 );
	CHECK( fromByteCol( mixed, 2, Encoding::Utf8 ) == 1 );
}

TEST_CASE( "invalid UTF-8 counts one unit per byte" ) {
	std::string bad = "a\xFF\xC3z";
	CHECK( fromByteCol( bad, 4, Encoding::Utf16 ) == 4 );
	CHECK( toByteCol( bad, 3, Encoding::Utf16 ) == 3 );
}

TEST_CASE( "round trip on every character boundary" ) {
	std::string s = "x\xF0\x9F\x98\x80\xF0\x9F\x98\x80y\xE2\x82\xAC";
	for ( int b : { 0, 1, 5, 9, 10, 13 } ) {
		int u = fromByteCol( s, b, Encoding::Utf16 );
		CHECK( toByteCol( s, u, Encoding::Utf16 ) == b );
	}
}

TEST_CASE( "Text lines, offsets and LSP positions" ) {
	Text t( "ab\r\nc" + mixed + "\n\nlast" );
	CHECK( t.lineCount() == 4 );
	CHECK( t.line( 0 ) == "ab" );					// '\r' excluded
	CHECK( t.line( 1 ) == "c" + mixed );
	CHECK( t.line( 2 ) == "" );
	CHECK( t.line( 3 ) == "last" );
	CHECK( t.line( 4 ) == "" );
	CHECK( t.offset( { 1, 0 } ) == 4 );
	CHECK( t.loc( 4 ) == Loc{ 1, 0 } );
	CHECK( t.clamp( { 0, 99 } ) == Loc{ 0, 3 } );	// may address the '\r'
	CHECK( t.clamp( { 9, 0 } ) == Loc{ 3, 4 } );
	CHECK( t.fromLsp( 1, 3, Encoding::Utf16 ) == Loc{ 1, 4 } );	// after "c", "a", "é"
	CHECK( t.fromLsp( 1, 4, Encoding::Utf16 ) == Loc{ 1, 4 } );	// inside the emoji
	CHECK( t.fromLsp( 1, 5, Encoding::Utf16 ) == Loc{ 1, 8 } );
	CHECK( t.toLspCol( { 1, 8 }, Encoding::Utf16 ) == 5 );
	CHECK( t.toLspCol( { 1, 8 }, Encoding::Utf8 ) == 8 );
	CHECK( t.fromLsp( 7, 3, Encoding::Utf16 ) == Loc{ 3, 4 } );
}

TEST_CASE( "Text replace" ) {
	Text t( "hello\nworld\n" );
	auto [s, e] = t.replace( { 0, 5 }, { 1, 0 }, ", " );
	CHECK( s == Loc{ 0, 5 } );
	CHECK( e == Loc{ 1, 0 } );
	CHECK( t.str() == "hello, world\n" );
	t.replace( { 0, 99 }, { 0, 99 }, "!" );			// clamps to end of line
	CHECK( t.str() == "hello, world!\n" );
	CHECK( t.lineRange( 0 ) == Range{ { 0, 0 }, { 0, 13 } } );
	Text u( "\t  x = 1;  " );
	CHECK( u.lineRange( 0 ) == Range{ { 0, 3 }, { 0, 11 } } );
}

TEST_CASE( "file URIs" ) {
	CHECK( pathToUri( "/home/me/a b/c#d%.cfa" ) == "file:///home/me/a%20b/c%23d%25.cfa" );
	CHECK( pathToUri( "/x/\xC3\xA9.hfa" ) == "file:///x/%C3%A9.hfa" );
	CHECK( uriToPath( "file:///home/me/a%20b/c%23d%25.cfa" ) == std::optional<std::string>( "/home/me/a b/c#d%.cfa" ) );
	CHECK( uriToPath( "file:///x/%c3%a9.hfa" ) == std::optional<std::string>( "/x/\xC3\xA9.hfa" ) );
	CHECK( uriToPath( "file://localhost/etc/x" ) == std::optional<std::string>( "/etc/x" ) );
	CHECK( uriToPath( "FILE:///a" ) == std::optional<std::string>( "/a" ) );
	CHECK( uriToPath( "file:///a/b%3Ac" ) == std::optional<std::string>( "/a/b:c" ) );
	CHECK( ! uriToPath( "untitled:Untitled-1" ) );
	CHECK( ! uriToPath( "file://otherhost/a" ) );
	for ( std::string p : { "/a/b.cfa", "/weird ~!@$&'()*+,;=/x", "/q/%41" } ) {
		CHECK( uriToPath( pathToUri( p ) ) == std::optional<std::string>( p ) );
	}
}
