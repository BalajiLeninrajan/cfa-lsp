// End-to-end tests of the background index: the built server, the
// translator and cfa on a copy of tests/fixtures/workspace, where only some
// files are open. Skipped when any of them is missing.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "LspClient.hpp"
#include "server/Checker.hpp"

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

json td( const std::string & uri ) { return { { "uri", uri } }; }
json at( const std::string & uri, json pos ) { return { { "textDocument", td( uri ) }, { "position", pos } }; }

std::string hoverText( const json & h ) { return h.is_null() ? "" : h["contents"]["value"].get<std::string>(); }
bool contains( const std::string & s, const std::string & part ) { return s.find( part ) != std::string::npos; }

// Calls `pred` until it holds or `limit` passes: the index fills in the
// background, one file at a time.
bool eventually( const std::function<bool()> & pred, std::chrono::seconds limit = std::chrono::seconds( 180 ) ) {
	auto deadline = std::chrono::steady_clock::now() + limit;
	for ( ;; ) {
		if ( pred() ) return true;
		if ( std::chrono::steady_clock::now() > deadline ) return false;
		std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
	}
}

// A copy of the fixture (the tests change files on disk) with one server
// whose workspace root is the copy, and main.cfa open.
struct Workspace {
	cfalsp::TempDir tmp;
	std::string dir;
	std::unique_ptr<LspClient> client;

	Workspace() {
		dir = fs::canonical( tmp.path() ).string() + "/ws";
		fs::copy( envOr( "CFA_LSP_FIXTURES", "tests/fixtures" ) + "/workspace", dir, fs::copy_options::recursive );
		client = std::make_unique<LspClient>( envOr( "CFA_LSP_TEST_LOG" ) );
		client->initialize( { { "debounceMs", 50 }, { "backend", false } }, dir );
		client->open( uri( "main.cfa" ), text( "main.cfa" ) );
		REQUIRE( client->diagnostics( uri( "main.cfa" ), 1 ) );
	}
	~Workspace() { client.reset(); }

	std::string path( const std::string & name ) const { return dir + "/" + name; }
	std::string uri( const std::string & name ) const { return "file://" + path( name ); }
	std::string text( const std::string & name ) const { return readAll( path( name ) ); }
	json pos( const std::string & name, const std::string & needle, int offset = 0, int nth = 0 ) const {
		return posOf( text( name ), needle, offset, nth );
	}

	json references( const std::string & name, json p, bool incl ) {
		return client->result( "textDocument/references",
							   { { "textDocument", td( uri( name ) ) }, { "position", p }, { "context", { { "includeDeclaration", incl } } } } );
	}
};

Workspace & workspace() {
	static std::unique_ptr<Workspace> w;
	if ( ! w ) w = std::make_unique<Workspace>();
	return *w;
}

using Spot = std::pair<std::string, std::string>;	// uri, start position as JSON

Spot spot( const std::string & uri, const json & pos ) { return { uri, pos.dump() }; }

std::set<Spot> spots( const json & locations ) {
	std::set<Spot> out;
	for ( const auto & l : locations ) out.insert( spot( l["uri"].get<std::string>(), l["range"]["start"] ) );
	return out;
}

} // namespace

TEST_SUITE( "integration" ) {

TEST_CASE( "index: references reach files that aren't open" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	std::set<Spot> want = {
		spot( w.uri( "main.cfa" ), w.pos( "main.cfa", "area( r )" ) ),
		spot( w.uri( "util.cfa" ), w.pos( "util.cfa", "area( r )" ) ),
		spot( w.uri( "shapes.cfa" ), w.pos( "shapes.cfa", "area( Rect r ) {" ) ),
		spot( w.uri( "shapes.cfa" ), w.pos( "shapes.cfa", "area( r )" ) ),
		spot( w.uri( "shapes.hfa" ), w.pos( "shapes.hfa", "area( Rect r );" ) ),
	};
	json got;
	CHECK( eventually( [&] {
		got = w.references( "main.cfa", w.pos( "main.cfa", "area( r )" ), true );
		return spots( got ) == want;
	} ) );
	CHECK( spots( got ) == want );
	CHECK( got.size() == want.size() );

	// Uses inside a header that isn't open: the index checks headers as focus files.
	json rect = w.references( "main.cfa", w.pos( "main.cfa", "Rect r =" ), false );
	std::set<Spot> s = spots( rect );
	CHECK( s.count( spot( w.uri( "shapes.hfa" ), w.pos( "shapes.hfa", "Rect r );" ) ) ) );
	CHECK( s.count( spot( w.uri( "shapes.hfa" ), w.pos( "shapes.hfa", "Rect r );", 0, 1 ) ) ) );
	CHECK( s.count( spot( w.uri( "shapes.cfa" ), w.pos( "shapes.cfa", "Rect r ) {" ) ) ) );
	CHECK( s.count( spot( w.uri( "util.cfa" ), w.pos( "util.cfa", "Rect r ) {" ) ) ) );
	CHECK( s.count( spot( w.uri( "main.cfa" ), w.pos( "main.cfa", "Rect r =" ) ) ) );
}

TEST_CASE( "index: definition goes to a body in a file that isn't open" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	json d;
	CHECK( eventually( [&] {
		d = w.client->result( "textDocument/definition", at( w.uri( "main.cfa" ), w.pos( "main.cfa", "describe( r )" ) ) );
		return d.size() == 1 && d[0]["uri"] == w.uri( "shapes.cfa" );
	} ) );
	REQUIRE( d.size() == 1 );
	CHECK( d[0]["range"]["start"] == w.pos( "shapes.cfa", "describe( Rect r ) {" ) );
}

TEST_CASE( "index: workspace symbols" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	json r;
	CHECK( eventually( [&] {
		r = w.client->result( "workspace/symbol", { { "query", "twiceAr" } } );
		for ( const auto & s : r ) {
			if ( s["location"]["uri"] == w.uri( "util.cfa" ) ) return true;
		}
		return false;
	} ) );
	bool def = false, proto = false;
	for ( const auto & s : r ) {
		CHECK( s["name"] == "twiceArea" );
		CHECK( s["kind"] == 12 );
		if ( s["location"]["uri"] == w.uri( "util.cfa" ) && s["location"]["range"]["start"] == w.pos( "util.cfa", "twiceArea" ) ) def = true;
		if ( s["location"]["uri"] == w.uri( "main.cfa" ) && s["location"]["range"]["start"] == w.pos( "main.cfa", "twiceArea" ) ) proto = true;
	}
	CHECK( def );
	CHECK( proto );

	// Fields have their aggregate as the container; an empty query lists everything.
	r = w.client->result( "workspace/symbol", { { "query", "" } } );
	bool field = false;
	for ( const auto & s : r ) {
		if ( s["name"] == "w" && s.value( "containerName", "" ) == "Rect" ) field = true;
	}
	CHECK( field );
}

TEST_CASE( "index: rename across files" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	json p = w.pos( "main.cfa", "area( r )", 1 );
	json edit;
	auto count = [&]() {
		size_t n = 0;
		for ( const auto & [uri, list] : edit["changes"].items() ) n += list.size();
		return n;
	};
	CHECK( eventually( [&] {
		json params = at( w.uri( "main.cfa" ), p );
		params["newName"] = "surface";
		edit = w.client->result( "textDocument/rename", params );
		return edit.is_object() && count() == 5;
	} ) );
	REQUIRE( edit.is_object() );
	std::set<Spot> got;
	for ( const auto & [uri, list] : edit["changes"].items() ) {
		for ( const auto & e : list ) {
			CHECK( e["newText"] == "surface" );
			// Every range covers exactly the old name.
			json s = e["range"]["start"], end = e["range"]["end"];
			CHECK( s["line"] == end["line"] );
			CHECK( end["character"].get<int>() - s["character"].get<int>() == 4 );
			got.insert( spot( uri, s ) );
		}
	}
	std::set<Spot> want = {
		spot( w.uri( "main.cfa" ), w.pos( "main.cfa", "area( r )" ) ),
		spot( w.uri( "util.cfa" ), w.pos( "util.cfa", "area( r )" ) ),
		spot( w.uri( "shapes.cfa" ), w.pos( "shapes.cfa", "area( Rect r ) {" ) ),
		spot( w.uri( "shapes.cfa" ), w.pos( "shapes.cfa", "area( r )" ) ),
		spot( w.uri( "shapes.hfa" ), w.pos( "shapes.hfa", "area( Rect r );" ) ),
	};
	CHECK( got == want );

	json prep = w.client->result( "textDocument/prepareRename", at( w.uri( "main.cfa" ), p ) );
	REQUIRE( ! prep.is_null() );
	CHECK( prep["placeholder"] == "area" );
	CHECK( prep["range"]["start"] == w.pos( "main.cfa", "area( r )" ) );

	// Bad names are refused, and there is nothing to rename on punctuation.
	json bad = at( w.uri( "main.cfa" ), p );
	bad["newName"] = "2much";
	CHECK( w.client->request( "textDocument/rename", bad ).contains( "error" ) );
	CHECK( w.client->result( "textDocument/prepareRename", at( w.uri( "main.cfa" ), w.pos( "main.cfa", "{ 2" ) ) ).is_null() );

	// twiceArea is declared in main.cfa and again in util.cfa, with no shared
	// header. The index can't link the two, so renaming only main.cfa's would
	// break the program: refused.
	json twice = at( w.uri( "main.cfa" ), w.pos( "main.cfa", "twiceArea( r )" ) );
	twice["newName"] = "doubleArea";
	json err = w.client->request( "textDocument/rename", twice )["error"];
	CHECK( err["code"] == -32803 );
	CHECK( contains( err.value( "message", "" ), "util.cfa" ) );
}

TEST_CASE( "index: call hierarchy" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	// Callers in three files, two of them not open. Once the index has the
	// body, the item points at it.
	json area;
	std::map<std::string, json> callers;
	CHECK( eventually( [&] {
		json items = w.client->result( "textDocument/prepareCallHierarchy", at( w.uri( "main.cfa" ), w.pos( "main.cfa", "area( r )" ) ) );
		if ( ! items.is_array() || items.size() != 1 ) return false;
		area = items[0];
		callers.clear();
		for ( const auto & c : w.client->result( "callHierarchy/incomingCalls", { { "item", area } } ) ) {
			callers[c["from"]["name"].get<std::string>()] = c;
		}
		return callers.size() == 3 && area["uri"] == w.uri( "shapes.cfa" );
	} ) );
	CHECK( area["name"] == "area" );
	CHECK( area["kind"] == 12 );
	CHECK( area["uri"] == w.uri( "shapes.cfa" ) );
	CHECK( area["selectionRange"]["start"] == w.pos( "shapes.cfa", "area( Rect r ) {" ) );
	CHECK( area["detail"] == "double area( Rect r )" );
	REQUIRE( callers.count( "twiceArea" ) );
	CHECK( callers["twiceArea"]["from"]["uri"] == w.uri( "util.cfa" ) );
	CHECK( callers["twiceArea"]["from"]["selectionRange"]["start"] == w.pos( "util.cfa", "twiceArea" ) );
	CHECK( callers["twiceArea"]["fromRanges"] == json::array( { { { "start", w.pos( "util.cfa", "area( r )" ) },
																   { "end", w.pos( "util.cfa", "area( r )", 4 ) } } } ) );
	REQUIRE( callers.count( "describe" ) );
	CHECK( callers["describe"]["from"]["uri"] == w.uri( "shapes.cfa" ) );
	REQUIRE( callers.count( "main" ) );
	CHECK( callers["main"]["from"]["uri"] == w.uri( "main.cfa" ) );

	// What describe calls: area, but not libcfa's operator for sout | x.
	json out = w.client->result( "callHierarchy/outgoingCalls", { { "item", callers["describe"]["from"] } } );
	REQUIRE( out.size() == 1 );
	CHECK( out[0]["to"]["name"] == "area" );
	CHECK( out[0]["fromRanges"][0]["start"] == w.pos( "shapes.cfa", "area( r )" ) );

	// What main calls, from the open file.
	json items = w.client->result( "textDocument/prepareCallHierarchy", at( w.uri( "main.cfa" ), w.pos( "main.cfa", "main()" ) ) );
	REQUIRE( items.size() == 1 );
	std::set<std::string> callees;
	for ( const auto & c : w.client->result( "callHierarchy/outgoingCalls", { { "item", items[0] } } ) ) {
		callees.insert( c["to"]["name"].get<std::string>() );
	}
	CHECK( callees == std::set<std::string>{ "area", "describe", "twiceArea" } );
}

TEST_CASE( "index: files changed on disk are checked again" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	// A new file, and another call in a file that isn't open.
	std::ofstream( w.path( "extra.cfa" ) ) << "#include \"shapes.hfa\"\n\ndouble extra( Rect r ) { return area( r ); }\n";
	std::string util = w.text( "util.cfa" ) + "\ndouble thrice( Rect r ) { return 3 * area( r ); }\n";
	std::ofstream( w.path( "util.cfa" ) ) << util;
	w.client->notify( "workspace/didChangeWatchedFiles",
					  { { "changes", { { { "uri", w.uri( "extra.cfa" ) }, { "type", 1 } }, { { "uri", w.uri( "util.cfa" ) }, { "type", 2 } } } } } );
	std::set<Spot> got;
	Spot extra = spot( w.uri( "extra.cfa" ), w.pos( "extra.cfa", "area( r )" ) );
	Spot thrice = spot( w.uri( "util.cfa" ), posOf( util, "area( r )", 0, 1 ) );
	CHECK( eventually( [&] {
		got = spots( w.references( "main.cfa", w.pos( "main.cfa", "area( r )" ), false ) );
		return got.count( extra ) && got.count( thrice );
	} ) );
	CHECK( got.count( extra ) );
	CHECK( got.count( thrice ) );

	// A deleted file drops out.
	fs::remove( w.path( "extra.cfa" ) );
	w.client->notify( "workspace/didChangeWatchedFiles", { { "changes", { { { "uri", w.uri( "extra.cfa" ) }, { "type", 3 } } } } } );
	CHECK( eventually( [&] {
		return ! spots( w.references( "main.cfa", w.pos( "main.cfa", "area( r )" ), false ) ).count( extra );
	} ) );
}

TEST_CASE( "index: rename refuses uses the index can't see" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	json params = at( w.uri( "main.cfa" ), w.pos( "main.cfa", "area( r )", 1 ) );
	params["newName"] = "surface";
	auto refusal = [&]( const std::string & part ) {
		json err;
		bool ok = eventually( [&] {
			err = w.client->request( "textDocument/rename", params )["error"];
			return err.is_object() && contains( err.value( "message", "" ), part );
		} );
		if ( ! ok ) MESSAGE( "last answer: " << err.dump() );
		return ok;
	};
	auto changed = [&]( const std::vector<std::pair<std::string, int>> & files ) {
		json list = json::array();
		for ( const auto & [name, type] : files ) list.push_back( { { "uri", w.uri( name ) }, { "type", type } } );
		w.client->notify( "workspace/didChangeWatchedFiles", { { "changes", list } } );
	};

	// A file that fails to preprocess leaves no table, but it calls area.
	std::ofstream( w.path( "broken.cfa" ) ) << "#include \"missing.hfa\"\n#include \"shapes.hfa\"\n\ndouble b( Rect r ) { return area( r ); }\n";
	changed( { { "broken.cfa", 1 } } );
	CHECK( refusal( "broken.cfa" ) );

	// A macro body in a file that isn't open names area.
	fs::remove( w.path( "broken.cfa" ) );
	std::ofstream( w.path( "macro.cfa" ) ) << "#include \"shapes.hfa\"\n\n#define AREA2( r ) ( 2 * area( r ) )\ndouble width( Rect r ) { return r.w; }\n";
	changed( { { "broken.cfa", 3 }, { "macro.cfa", 1 } } );
	CHECK( refusal( "macro on line 3 of macro.cfa" ) );

	// Without them the rename goes through again.
	fs::remove( w.path( "macro.cfa" ) );
	changed( { { "macro.cfa", 3 } } );
	CHECK( eventually( [&] {
		json r = w.client->request( "textDocument/rename", params );
		return r.contains( "result" ) && r["result"].is_object();
	} ) );
}

TEST_CASE( "index: checks read open headers' unsaved buffers" ) {
	if ( ! ready() ) return;
	Workspace & w = workspace();
	LspClient & c = *w.client;
	std::string header = w.text( "shapes.hfa" );
	c.open( w.uri( "shapes.hfa" ), header );
	REQUIRE( c.diagnostics( w.uri( "shapes.hfa" ), 1 ) );
	// Declare a function in the header without saving it.
	std::string decl = "double perimeter( Rect r );\n";
	json end = posOf( header + "@", "@" );
	c.change( w.uri( "shapes.hfa" ), 2, end, end, decl );
	std::string edited = header + decl;

	std::string scratch = "#include \"shapes.hfa\"\n\nint main() {\n\tRect r = { 1, 2 };\n\treturn perimeter( r ) > 0;\n}\n";
	std::string uri = w.uri( "scratch_unsaved.cfa" );
	c.open( uri, scratch );
	auto d = c.diagnostics( uri, 1 );
	REQUIRE( d );
	CHECK( ( *d )["diagnostics"] == json::array() );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( scratch, "perimeter" ) ) ) ), "double perimeter( Rect r )" ) );
	json def = c.result( "textDocument/definition", at( uri, posOf( scratch, "perimeter" ) ) );
	REQUIRE( def.size() == 1 );
	CHECK( def[0]["uri"] == w.uri( "shapes.hfa" ) );
	CHECK( def[0]["range"]["start"] == posOf( edited, "perimeter" ) );

	// Taking the declaration out again re-checks the file that includes the header.
	c.dropDiagnostics( uri );
	json from = posOf( edited, "double perimeter" ), to = posOf( edited + "@", "@" );
	c.change( w.uri( "shapes.hfa" ), 3, from, to, "" );
	auto again = c.waitFor( "textDocument/publishDiagnostics",
							[&]( const json & p ) { return p["uri"] == uri && ! p["diagnostics"].empty(); }, std::chrono::seconds( 120 ) );
	REQUIRE( again );
	CHECK( contains( ( *again )["diagnostics"][0]["message"].get<std::string>(), "perimeter" ) );

	c.notify( "textDocument/didClose", { { "textDocument", td( uri ) } } );
	c.notify( "textDocument/didClose", { { "textDocument", td( w.uri( "shapes.hfa" ) ) } } );
}

} // TEST_SUITE
