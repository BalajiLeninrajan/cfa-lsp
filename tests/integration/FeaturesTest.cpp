// End-to-end tests for document highlight, inlay hints, rename,
// switchSourceHeader and workspace symbols: the built server, the forked
// translator and cfa on tests/fixtures/project. Skipped when any of them is
// missing.
#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

#include "LspClient.hpp"

using namespace cfalsp::test;
namespace fs = std::filesystem;

namespace {

bool ready() {
	if ( serverBinary().empty() || translatorBinary().empty() || ! haveCfa() ) {
		MESSAGE( "needs the built server and translator (CFA_LSP_SERVER, CFA_LSP_TRANSLATOR) and cfa on PATH; skipping" );
		return false;
	}
	return true;
}

std::string projectDir() {
	return fs::canonical( envOr( "CFA_LSP_FIXTURES", "tests/fixtures" ) + "/project" ).string();
}

std::string uriOf( const std::string & path ) { return "file://" + path; }

json td( const std::string & uri ) { return { { "uri", uri } }; }
json at( const std::string & uri, json pos ) { return { { "textDocument", td( uri ) }, { "position", pos } }; }

bool contains( const std::string & s, const std::string & part ) { return s.find( part ) != std::string::npos; }

// A server with one document open and checked.
struct Opened {
	LspClient c{ envOr( "CFA_LSP_TEST_LOG" ) };
	std::string uri, text;
	json init;
	Opened( const std::string & path, std::string contents, int debounceMs = 50 ) : uri( uriOf( path ) ), text( std::move( contents ) ) {
		init = c.initialize( { { "debounceMs", debounceMs }, { "backend", false } }, projectDir() );
		c.open( uri, text );
		REQUIRE( c.diagnostics( uri, 1 ) );
	}
	json pos( const std::string & needle, int offset = 0, int nth = 0 ) const { return posOf( text, needle, offset, nth ); }
};

// Inlay hints by position: (line, character) -> (label, kind).
std::map<std::pair<int, int>, std::pair<std::string, int>> hints( const json & result ) {
	std::map<std::pair<int, int>, std::pair<std::string, int>> out;
	for ( const auto & h : result ) {
		out[{ h["position"]["line"].get<int>(), h["position"]["character"].get<int>() }] = { h["label"].get<std::string>(), h["kind"].get<int>() };
	}
	return out;
}

std::pair<int, int> key( const json & pos ) { return { pos["line"].get<int>(), pos["character"].get<int>() }; }

json wholeFile() { return { { "start", lspPos( 0, 0 ) }, { "end", lspPos( 10000, 0 ) } }; }

json renameParams( const std::string & uri, json pos, const std::string & newName ) {
	json p = at( uri, pos );
	p["newName"] = newName;
	return p;
}

} // namespace

TEST_SUITE( "integration" ) {

TEST_CASE( "the server advertises highlight, inlay hints, rename and workspace symbols; document highlight" ) {
	if ( ! ready() ) return;
	Opened o( projectDir() + "/geometry.cfa", readAll( projectDir() + "/geometry.cfa" ) );
	json caps = o.init["result"]["capabilities"];
	CHECK( caps["documentHighlightProvider"] == true );
	CHECK( caps["inlayHintProvider"] == true );
	CHECK( caps["renameProvider"] == true );
	CHECK( caps["workspaceSymbolProvider"] == true );

	json hl = o.c.result( "textDocument/documentHighlight", at( o.uri, o.pos( "box = {" ) ) );
	std::map<std::pair<int, int>, int> got;
	for ( const auto & h : hl ) got[key( h["range"]["start"] )] = h["kind"];
	std::map<std::pair<int, int>, int> want = {
		{ key( o.pos( "box = {" ) ), 1 },
		{ key( o.pos( "&box;", 1 ) ), 2 },
		{ key( o.pos( "area( box )", 6 ) ), 2 },
		{ key( o.pos( "box, 1" ) ), 2 },
	};
	CHECK( got == want );
	for ( const auto & h : hl ) {
		CHECK( h["range"]["end"]["character"].get<int>() - h["range"]["start"]["character"].get<int>() == 3 );
	}

	// One overload of area: its definition here and its call, not area( Circle ) or the header prototype.
	hl = o.c.result( "textDocument/documentHighlight", at( o.uri, o.pos( "area( box )" ) ) );
	got.clear();
	for ( const auto & h : hl ) got[key( h["range"]["start"] )] = h["kind"];
	want = { { key( o.pos( "area( Rect r ) {" ) ), 1 }, { key( o.pos( "area( box )" ) ), 2 } };
	CHECK( got == want );

	CHECK( o.c.result( "textDocument/documentHighlight", at( o.uri, o.pos( "return 3.14159", 2 ) ) ) == json::array() );
	CHECK( o.c.shutdown() == 0 );
}

TEST_CASE( "inlay hints show parameter names and the types of polymorphic calls" ) {
	if ( ! ready() ) return;
	Opened o( projectDir() + "/geometry.cfa", readAll( projectDir() + "/geometry.cfa" ) );
	auto h = hints( o.c.result( "textDocument/inlayHint", { { "textDocument", td( o.uri ) }, { "range", wholeFile() } } ) );
	auto label = [&]( json pos ) {
		auto it = h.find( key( pos ) );
		return it == h.end() ? std::pair<std::string, int>{ "", 0 } : it->second;
	};
	CHECK( label( o.pos( "3, 7" ) ) == std::pair<std::string, int>{ "a:", 2 } );
	CHECK( label( o.pos( "7 )" ) ) == std::pair<std::string, int>{ "b:", 2 } );
	CHECK( label( o.pos( "area( box )", 6 ) ) == std::pair<std::string, int>{ "r:", 2 } );
	CHECK( label( o.pos( "area( wheel )", 6 ) ) == std::pair<std::string, int>{ "c:", 2 } );
	CHECK( label( o.pos( "a1 + a2" ) ) == std::pair<std::string, int>{ "amount:", 2 } );
	CHECK( label( o.pos( "&box, 1" ) ) == std::pair<std::string, int>{ "shapes:", 2 } );
	CHECK( label( o.pos( "&box, 1", 6 ) ) == std::pair<std::string, int>{ "n:", 2 } );
	CHECK( label( o.pos( "fib )" ) ) == std::pair<std::string, int>{ "f:", 2 } );
	// biggest( T, T ) returns T: say what T is at each call.
	auto t = label( o.pos( "biggest( 3, 7 )", 15 ) );
	CHECK( t.second == 1 );
	CHECK( t.first.starts_with( ": " ) );
	CHECK( contains( t.first, "int" ) );
	t = label( o.pos( "biggest( 2.5, 1.5 )", 19 ) );
	CHECK( t.second == 1 );
	CHECK( t.first == ": double" );
	// Declarations get no hints.
	CHECK( label( o.pos( "double amount )" ) ).first.empty() );
	CHECK( o.c.shutdown() == 0 );
}

TEST_CASE( "inlay hints skip arguments named like the parameter, span lines and follow edits" ) {
	if ( ! ready() ) return;
	std::string text =
		"int add( int x, int y ) { return x + y; }\n"
		"forall( T ) T first( T a, T b ) { return a; }\n"
		"int main() {\n"
		"\tint x = 1, y = 2;\n"
		"\tint z = add( x, 3 );\n"
		"\tz = add( 4,\n"
		"\t         y );\n"
		"\tdouble d = first( 1.5, 2.5 );\n"
		"\treturn z + (int)d;\n"
		"}\n";
	// No check after the edits below: hints come from the first snapshot.
	Opened o( projectDir() + "/scratch_hints.cfa", text, 600000 );
	json all = o.c.result( "textDocument/inlayHint", { { "textDocument", td( o.uri ) }, { "range", wholeFile() } } );
	auto h = hints( all );
	std::map<std::pair<int, int>, std::pair<std::string, int>> want = {
		{ key( o.pos( "3 );" ) ), { "y:", 2 } },
		{ key( o.pos( "4,\n" ) ), { "x:", 2 } },
		{ key( o.pos( "1.5, 2.5" ) ), { "a:", 2 } },
		{ key( o.pos( "2.5 )" ) ), { "b:", 2 } },
		{ key( o.pos( "first( 1.5, 2.5 )", 17 ) ), { ": double", 1 } },
	};
	CHECK( h == want );
	for ( const auto & x : all ) {
		CHECK( x["paddingRight"] == ( x["kind"] == 2 ) );
	}

	// Only the hints inside the requested range.
	json line = { { "start", lspPos( o.pos( "double d" )["line"], 0 ) }, { "end", lspPos( o.pos( "double d" )["line"].get<int>() + 1, 0 ) } };
	CHECK( o.c.result( "textDocument/inlayHint", { { "textDocument", td( o.uri ) }, { "range", line } } ).size() == 3 );

	// An edited call loses its hints; the others shift with the text.
	json p = o.pos( "3 );" );
	o.c.change( o.uri, 2, p, p, "1 + " );
	std::string edited = text;
	edited.insert( edited.find( "3 );" ), "1 + " );
	o.c.change( o.uri, 3, lspPos( 0, 0 ), lspPos( 0, 0 ), "// top\n" );
	edited = "// top\n" + edited;
	h = hints( o.c.result( "textDocument/inlayHint", { { "textDocument", td( o.uri ) }, { "range", wholeFile() } } ) );
	CHECK( h.size() == 4 );
	CHECK_FALSE( h.count( key( posOf( edited, "1 + 3 );" ) ) ) );
	CHECK( h[key( posOf( edited, "4,\n" ) )] == std::pair<std::string, int>{ "x:", 2 } );
	CHECK( h[key( posOf( edited, "first( 1.5, 2.5 )", 17 ) )] == std::pair<std::string, int>{ ": double", 1 } );
	CHECK( o.c.shutdown() == 0 );
}

TEST_CASE( "rename within one file, and refusals" ) {
	if ( ! ready() ) return;
	Opened o( projectDir() + "/geometry.cfa", readAll( projectDir() + "/geometry.cfa" ) );

	json r = o.c.result( "textDocument/prepareRename", at( o.uri, o.pos( "box, 1", 1 ) ) );
	REQUIRE( r.is_object() );
	CHECK( r["placeholder"] == "box" );
	CHECK( r["range"]["start"] == o.pos( "box, 1" ) );
	CHECK( r["range"]["end"] == o.pos( "box, 1", 3 ) );

	// A local: the declaration and every use.
	r = o.c.result( "textDocument/rename", renameParams( o.uri, o.pos( "box = {" ), "crate" ) );
	REQUIRE( r["changes"].size() == 1 );
	json edits = r["changes"][o.uri];
	std::set<std::pair<int, int>> starts;
	for ( const auto & e : edits ) {
		CHECK( e["newText"] == "crate" );
		CHECK( e["range"]["end"]["character"].get<int>() - e["range"]["start"]["character"].get<int>() == 3 );
		starts.insert( key( e["range"]["start"] ) );
	}
	CHECK( starts == std::set<std::pair<int, int>>{ key( o.pos( "box = {" ) ), key( o.pos( "&box;", 1 ) ),
													 key( o.pos( "area( box )", 6 ) ), key( o.pos( "box, 1" ) ) } );

	// A global function declared only in this file.
	r = o.c.result( "textDocument/rename", renameParams( o.uri, o.pos( "check( a1" ), "verify" ) );
	starts.clear();
	for ( const auto & e : r["changes"][o.uri] ) starts.insert( key( e["range"]["start"] ) );
	CHECK( starts == std::set<std::pair<int, int>>{ key( o.pos( "check( double" ) ), key( o.pos( "check( a1" ) ) } );

	// Declared in the header (area, the field centre) or in libcfa (sout): refused, saying where.
	json err = o.c.request( "textDocument/rename", renameParams( o.uri, o.pos( "area( box )" ), "size" ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "geometry.hfa" ) );
	CHECK( contains( err["message"], "across files" ) );
	err = o.c.request( "textDocument/prepareRename", at( o.uri, o.pos( "centre.y" ) ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "geometry.hfa" ) );
	err = o.c.request( "textDocument/prepareRename", at( o.uri, o.pos( "sout | biggest" ) ) )["error"];
	CHECK( contains( err["message"], "fstream.hfa" ) );

	// Not an identifier.
	CHECK( o.c.request( "textDocument/rename", renameParams( o.uri, o.pos( "box = {" ), "while" ) )["error"]["code"] == -32602 );
	CHECK( o.c.request( "textDocument/rename", renameParams( o.uri, o.pos( "box = {" ), "a b" ) )["error"]["code"] == -32602 );
	// Nothing there.
	CHECK( o.c.result( "textDocument/prepareRename", at( o.uri, o.pos( "return 3.14159", 2 ) ) ).is_null() );
	CHECK( o.c.shutdown() == 0 );
}

TEST_CASE( "rename refuses names used in macros or where the dump has no ref, and globals of a header" ) {
	if ( ! ready() ) return;
	std::string text =
		"int n = 3;\n"
		"#define TWICE (n * 2)\n"
		"#define SQ( n ) ((n) * (n))\n"
		"enum { SIZE = 4 };\n"
		"int main() {\n"
		"\tint m = TWICE + SQ( 2 );\n"
		"\tint arr[SIZE];\n"
		"\tint hidden = SIZE;\n"
		"#if 0\n"
		"\thidden += 1;\n"
		"#endif\n"
		"\tarr[0] = hidden;\n"
		"\treturn m + n + arr[0];\n"
		"}\n";
	Opened o( projectDir() + "/scratch_rename.cfa", text );
	json err = o.c.request( "textDocument/rename", renameParams( o.uri, o.pos( "n = 3" ), "count" ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "TWICE" ) );
	// SQ's parameter n is not a use of the global, and m isn't in any macro.
	json r = o.c.result( "textDocument/rename", renameParams( o.uri, o.pos( "m = TWICE" ), "k" ) );
	CHECK( r["changes"][o.uri].size() == 2 );

	// Spellings the translator has no ref for: dead code, an array dimension.
	err = o.c.request( "textDocument/rename", renameParams( o.uri, o.pos( "hidden = SIZE" ), "shown" ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "line 10" ) );
	err = o.c.request( "textDocument/prepareRename", at( o.uri, o.pos( "SIZE;" ) ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "line 7" ) );

	// In a header, only local names.
	std::string header = projectDir() + "/geometry.hfa";
	std::string htext = readAll( header );
	o.c.open( uriOf( header ), htext );
	REQUIRE( o.c.diagnostics( uriOf( header ), 1 ) );
	err = o.c.request( "textDocument/rename", renameParams( uriOf( header ), posOf( htext, "Rect {" ), "Box" ) )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err["message"], "header" ) );
	r = o.c.result( "textDocument/rename", renameParams( uriOf( header ), posOf( htext, "r );" ), "rect" ) );
	REQUIRE( r["changes"][uriOf( header )].size() == 1 );
	CHECK( r["changes"][uriOf( header )][0]["range"]["start"] == posOf( htext, "r );" ) );
	CHECK( o.c.shutdown() == 0 );
}

TEST_CASE( "switchSourceHeader goes between geometry.cfa and geometry.hfa" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 50 }, { "backend", false } }, projectDir() );
	std::string dir = projectDir();
	CHECK( c.result( "textDocument/switchSourceHeader", td( uriOf( dir + "/geometry.cfa" ) ) ) == uriOf( dir + "/geometry.hfa" ) );
	CHECK( c.result( "textDocument/switchSourceHeader", td( uriOf( dir + "/geometry.hfa" ) ) ) == uriOf( dir + "/geometry.cfa" ) );
	CHECK( c.result( "textDocument/switchSourceHeader", td( uriOf( dir + "/broken.cfa" ) ) ).is_null() );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "workspace symbols cover the open documents and the project headers they include" ) {
	if ( ! ready() ) return;
	Opened o( projectDir() + "/geometry.cfa", readAll( projectDir() + "/geometry.cfa" ) );
	std::string header = uriOf( projectDir() + "/geometry.hfa" );
	std::string htext = readAll( projectDir() + "/geometry.hfa" );
	json all = o.c.result( "workspace/symbol", { { "query", "" } } );
	std::multiset<std::pair<std::string, std::string>> got;		// name, uri
	for ( const auto & s : all ) got.insert( { s["name"].get<std::string>(), s["location"]["uri"].get<std::string>() } );
	CHECK( got.count( { "area", o.uri } ) == 2 );
	CHECK( got.count( { "check", o.uri } ) == 1 );
	CHECK( got.count( { "Rect", header } ) == 1 );
	CHECK( got.count( { "area", header } ) >= 2 );
	CHECK( got.count( { "box", o.uri } ) == 0 );			// locals are left out
	for ( const auto & s : all ) {
		CHECK( s["name"] != "sout" );						// so is libcfa
		if ( s["name"] == "Rect" ) {
			CHECK( s["kind"] == 23 );						// Struct
			CHECK( s["location"]["range"]["start"] == posOf( htext, "Rect {" ) );
		}
		if ( s["name"] == "radius" ) {
			CHECK( s["containerName"] == "Circle" );
		}
	}

	// The query matches its characters in order, ignoring case.
	std::set<std::string> names;
	for ( const auto & s : o.c.result( "workspace/symbol", { { "query", "BGST" } } ) ) names.insert( s["name"].get<std::string>() );
	CHECK( names == std::set<std::string>{ "biggest" } );
	CHECK( o.c.result( "workspace/symbol", { { "query", "zzqq" } } ) == json::array() );
	CHECK( o.c.shutdown() == 0 );
}

} // TEST_SUITE
