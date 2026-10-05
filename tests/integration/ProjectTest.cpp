// End-to-end tests: the built cfa-lsp binary, the forked translator and the
// installed cfa on tests/fixtures/project. Skipped when any of them is
// missing.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
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

std::string hoverText( const json & h ) { return h.is_null() ? "" : h["contents"]["value"].get<std::string>(); }

bool contains( const std::string & s, const std::string & part ) { return s.find( part ) != std::string::npos; }

std::set<std::string> labels( const json & completion ) {
	std::set<std::string> out;
	for ( const auto & i : completion["items"] ) out.insert( i["label"].get<std::string>() );
	return out;
}

// One server with geometry.cfa open, shared by the tests that only query it.
struct Project {
	std::string dir = projectDir();
	std::string path = dir + "/geometry.cfa";
	std::string uri = uriOf( path );
	std::string text = readAll( path );
	std::string header = dir + "/geometry.hfa";
	std::string headerText = readAll( header );
	std::unique_ptr<LspClient> client;
	json init;
	json firstDiagnostics;

	Project() {
		client = std::make_unique<LspClient>( envOr( "CFA_LSP_TEST_LOG" ) );
		init = client->initialize( { { "debounceMs", 50 }, { "backend", false } }, dir );
		client->open( uri, text );
		auto d = client->diagnostics( uri, 1 );
		REQUIRE( d );
		firstDiagnostics = ( *d )["diagnostics"];
	}
	json pos( const std::string & needle, int offset = 0, int nth = 0 ) const { return posOf( text, needle, offset, nth ); }
};

Project & project() {
	static std::unique_ptr<Project> p;
	if ( ! p ) p = std::make_unique<Project>();
	return *p;
}

// A second document in the project directory that exists only in the
// editor: the tests that edit use it so the shared one stays intact.
struct Scratch {
	LspClient & c;
	std::string uri;
	std::string text;
	int version = 1;
	Scratch( LspClient & c, const std::string & name, std::string text ) : c( c ), uri( uriOf( projectDir() + "/" + name ) ), text( std::move( text ) ) {
		c.open( uri, this->text );
		auto d = c.diagnostics( uri, 1 );
		REQUIRE( d );
	}
	~Scratch() { c.notify( "textDocument/didClose", { { "textDocument", td( uri ) } } ); }
	// Inserts `ins` at the `nth` occurrence of `needle` plus `offset`.
	void insert( const std::string & needle, int offset, const std::string & ins, int nth = 0 ) {
		json p = posOf( text, needle, offset, nth );
		size_t at = text.find( needle );
		for ( int i = 0; i < nth; i += 1 ) at = text.find( needle, at + 1 );
		text.insert( at + offset, ins );
		version += 1;
		c.change( uri, version, p, p, ins );
	}
	json pos( const std::string & needle, int offset = 0, int nth = 0 ) const { return posOf( text, needle, offset, nth ); }
};

} // namespace

TEST_SUITE( "integration" ) {

TEST_CASE( "a clean file has no diagnostics and the server advertises its features" ) {
	if ( ! ready() ) return;
	Project & p = project();
	CHECK( p.firstDiagnostics == json::array() );
	json caps = p.init["result"]["capabilities"];
	CHECK( caps["hoverProvider"] == true );
	CHECK( caps["completionProvider"]["triggerCharacters"].size() == 2 );
}

TEST_CASE( "diagnostics in a broken file have the right ranges and don't cascade" ) {
	if ( ! ready() ) return;
	std::string path = projectDir() + "/broken.cfa";
	std::string text = readAll( path );
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 0 }, { "backend", false } } );
	c.open( uriOf( path ), text );
	auto d = c.diagnostics( uriOf( path ), 1 );
	REQUIRE( d );
	json ds = ( *d )["diagnostics"];
	REQUIRE( ds.size() == 2 );
	// double bad = area( "rect" );  -- the whole call
	CHECK( ds[0]["range"]["start"] == posOf( text, "area( \"rect\" )" ) );
	CHECK( ds[0]["range"]["end"] == posOf( text, "area( \"rect\" )", 14 ) );
	CHECK( ds[0]["severity"] == 1 );
	// The first line names the problem; the resolver dump is condensed.
	CHECK( ds[0]["message"].get<std::string>().starts_with( "no overload of `area` accepts the arguments (char [5])" ) );
	// sout | d | nosuch;  -- just the name
	CHECK( ds[1]["range"]["start"] == posOf( text, "nosuch" ) );
	CHECK( ds[1]["range"]["end"] == posOf( text, "nosuch", 6 ) );
	CHECK( ds[1]["message"] == "use of undeclared identifier `nosuch`" );

	// The statements around the errors were still resolved.
	json h = c.result( "textDocument/hover", at( uriOf( path ), posOf( text, "area( r )" ) ) );
	CHECK( contains( hoverText( h ), "double area( Rect r )" ) );
	h = c.result( "textDocument/hover", at( uriOf( path ), posOf( text, "bad;" ) ) );
	CHECK( contains( hoverText( h ), "double bad" ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "a header opened on its own is checked without the #pragma once warning" ) {
	if ( ! ready() ) return;
	Project & p = project();
	std::string uri = uriOf( p.header );
	p.client->open( uri, p.headerText );
	auto d = p.client->diagnostics( uri, 1 );
	REQUIRE( d );
	CHECK( ( *d )["diagnostics"] == json::array() );
	json h = p.client->result( "textDocument/hover", at( uri, posOf( p.headerText, "Point lo" ) ) );
	CHECK( contains( hoverText( h ), "struct Point" ) );
	p.client->notify( "textDocument/didClose", { { "textDocument", td( uri ) } } );
}

TEST_CASE( "hover shows the overload the translator chose" ) {
	if ( ! ready() ) return;
	Project & p = project();
	auto hover = [&]( const std::string & needle, int nth = 0 ) {
		return hoverText( p.client->result( "textDocument/hover", at( p.uri, p.pos( needle, 0, nth ) ) ) );
	};
	std::string rect = hover( "area( box )" );
	CHECK( contains( rect, "double area( Rect r )" ) );
	CHECK( contains( rect, "1 other overload" ) );
	CHECK( contains( rect, "Area of a rectangle." ) );		// doc comment from the header prototype
	CHECK( contains( hover( "area( wheel )" ), "double area( Circle c )" ) );
	// Overloaded on the return type only.
	CHECK( contains( hover( "unit();" ), "int unit()" ) );
	CHECK_FALSE( contains( hover( "unit();" ), "double unit()" ) );
	CHECK( contains( hover( "unit();", 1 ), "double unit()" ) );
	// Polymorphic and library functions, members, locals.
	CHECK( contains( hover( "biggest( 3" ), "forall( T ) T biggest( T a, T b )" ) );
	CHECK( contains( hover( "sout | biggest" ), "ofstream" ) );
	CHECK( contains( hover( "radius );" ), "double radius" ) );		// through with( c )
	CHECK( contains( hover( "fn = a" ), "int fn" ) );
	CHECK( contains( hover( "amount;" ), "double amount" ) );
	CHECK( contains( hover( "check( double" ), "Throws when the amount is too large." ) );
	// Inside the trait-constrained function, area is the trait's assertion.
	CHECK( contains( hover( "area( shapes[i] )" ), "double area( S )" ) );
	// Macros, which the translator never sees.
	CHECK( contains( hover( "SQ( radius )" ), "#define SQ( x ) ((x) * (x))" ) );
	CHECK( contains( hover( "ORIGIN, 0" ), "#define ORIGIN 0" ) );
}

TEST_CASE( "definition jumps to the chosen declaration" ) {
	if ( ! ready() ) return;
	Project & p = project();
	auto def = [&]( const std::string & needle, int offset = 0, int nth = 0 ) {
		json r = p.client->result( "textDocument/definition", at( p.uri, p.pos( needle, offset, nth ) ) );
		REQUIRE( r.is_array() );
		REQUIRE( r.size() == 1 );
		return r[0];
	};
	// The call goes to the definition with a body in this file, not the prototype.
	json d = def( "area( box )" );
	CHECK( d["uri"] == p.uri );
	CHECK( d["range"]["start"] == p.pos( "area( Rect r ) {", 0 ) );
	d = def( "area( wheel )" );
	CHECK( d["range"]["start"] == p.pos( "area( Circle c ) with" ) );
	d = def( "unit();", 0, 1 );
	CHECK( d["range"]["start"] == p.pos( "unit() { return 1.0" ) );

	// Into the project header.
	d = def( "Rect box" );
	CHECK( d["uri"] == uriOf( p.header ) );
	CHECK( d["range"]["start"] == posOf( p.headerText, "Rect {", 0 ) );
	d = def( "hi.x |", 0 );
	CHECK( d["uri"] == uriOf( p.header ) );
	CHECK( d["range"]["start"] == posOf( p.headerText, "hi;" ) );

	// Into libcfa.
	d = def( "sout | biggest" );
	std::string uri = d["uri"];
	CHECK( uri.size() > 11 );
	CHECK( uri.compare( uri.size() - 11, 11, "fstream.hfa" ) == 0 );
	std::string lib = readAll( uri.substr( 7 ) );
	json start = d["range"]["start"];
	int line = start["line"];
	size_t bol = 0;
	for ( int i = 0; i < line; i += 1 ) bol = lib.find( '\n', bol ) + 1;
	CHECK( lib.compare( bol + start["character"].get<int>(), 4, "sout" ) == 0 );

	// A local.
	d = def( "box, 1" );
	CHECK( d["range"]["start"] == p.pos( "box = {" ) );

	// Include directives.
	d = def( "#include \"geometry.hfa\"", 12 );
	CHECK( d["uri"] == uriOf( p.header ) );
	d = def( "#include <fstream.hfa>", 12 );
	CHECK( d["uri"].get<std::string>().ends_with( "/include/cfa/fstream.hfa" ) );

	// A macro.
	d = def( "SQ( radius )" );
	CHECK( d["uri"] == uriOf( p.header ) );
	CHECK( d["range"]["start"] == posOf( p.headerText, "SQ( x )" ) );
}

TEST_CASE( "references" ) {
	if ( ! ready() ) return;
	Project & p = project();
	json r = p.client->result( "textDocument/references",
							   { { "textDocument", td( p.uri ) }, { "position", p.pos( "box = {" ) },
								 { "context", { { "includeDeclaration", true } } } } );
	std::set<int> lines;
	for ( const auto & l : r ) {
		CHECK( l["uri"] == p.uri );
		lines.insert( l["range"]["start"]["line"].get<int>() );
	}
	std::set<int> want = { p.pos( "box = {" )["line"], p.pos( "&box;" )["line"], p.pos( "area( box )" )["line"],
						   p.pos( "box, 1" )["line"] };
	CHECK( lines == want );
	CHECK( r.size() == 4 );

	// area(Rect) across the header prototype and the definition, not area(Circle).
	r = p.client->result( "textDocument/references",
						  { { "textDocument", td( p.uri ) }, { "position", p.pos( "area( box )" ) },
							{ "context", { { "includeDeclaration", true } } } } );
	bool sawHeader = false, sawCircle = false;
	for ( const auto & l : r ) {
		if ( l["uri"] == uriOf( p.header ) ) sawHeader = true;
		if ( l["uri"] == p.uri && l["range"]["start"] == p.pos( "area( wheel )" ) ) sawCircle = true;
	}
	CHECK( sawHeader );
	CHECK_FALSE( sawCircle );
}

TEST_CASE( "document symbols" ) {
	if ( ! ready() ) return;
	Project & p = project();
	json syms = p.client->result( "textDocument/documentSymbol", { { "textDocument", td( p.uri ) } } );
	std::multiset<std::string> names;
	for ( const auto & s : syms ) names.insert( s["name"].get<std::string>() );
	CHECK( names.count( "area" ) == 2 );
	CHECK( names.count( "unit" ) == 2 );
	CHECK( names.count( "main" ) == 2 );
	CHECK( names.count( "check" ) == 1 );
	CHECK( names.count( "tooBig_vt" ) == 1 );
	CHECK( names.count( "box" ) == 0 );						// locals are left out
	for ( const auto & s : syms ) {
		if ( s["name"] == "check" ) {
			CHECK( s["kind"] == 12 );						// Function
			CHECK( s["selectionRange"]["start"] == p.pos( "check( double" ) );
		}
	}
}

TEST_CASE( "semantic tokens" ) {
	if ( ! ready() ) return;
	Project & p = project();
	json legend = p.init["result"]["capabilities"]["semanticTokensProvider"]["legend"];
	json data = p.client->result( "textDocument/semanticTokens/full", { { "textDocument", td( p.uri ) } } )["data"];
	REQUIRE( data.size() % 5 == 0 );
	struct Tok { int line, col, len; std::string type; int mods; };
	std::vector<Tok> toks;
	int line = 0, col = 0;
	for ( size_t i = 0; i < data.size(); i += 5 ) {
		int dl = data[i], dc = data[i + 1];
		line += dl;
		col = dl ? dc : col + dc;
		toks.push_back( { line, col, data[i + 2], legend["tokenTypes"][data[i + 3].get<int>()], data[i + 4] } );
	}
	auto find = [&]( json pos ) -> const Tok * {
		for ( const Tok & t : toks ) {
			if ( t.line == pos["line"] && t.col == pos["character"] ) return &t;
		}
		return nullptr;
	};
	int declaration = 1, defaultLibrary = 4;
	const Tok * t = find( p.pos( "area( box )" ) );
	REQUIRE( t );
	CHECK( t->type == "function" );
	CHECK( t->len == 4 );
	t = find( p.pos( "Rect * pbox" ) );
	REQUIRE( t );
	CHECK( t->type == "struct" );
	t = find( p.pos( "pbox = &box" ) );
	REQUIRE( t );
	CHECK( t->type == "variable" );
	CHECK( ( t->mods & declaration ) );
	t = find( p.pos( "sout | u" ) );
	REQUIRE( t );
	CHECK( ( t->mods & defaultLibrary ) );
	t = find( p.pos( "centre.y" ) );
	REQUIRE( t );
	CHECK( t->type == "property" );
	t = find( p.pos( "shapes[i]" ) );
	REQUIRE( t );
	CHECK( t->type == "parameter" );
	// Macros aren't coloured as the code they expand to.
	CHECK_FALSE( find( p.pos( "SQ( radius )" ) ) );
	CHECK_FALSE( find( p.pos( "ORIGIN, 0" ) ) );
	// Tokens are ordered and don't overlap.
	for ( size_t i = 1; i < toks.size(); i += 1 ) {
		CHECK( ( toks[i - 1].line < toks[i].line || toks[i - 1].col + toks[i - 1].len <= toks[i].col ) );
	}
}

TEST_CASE( "member completion keeps working while the file doesn't parse" ) {
	if ( ! ready() ) return;
	Project & p = project();
	Scratch s( *p.client, "scratch_members.cfa", p.text );
	// Half a statement: the file no longer parses.
	s.insert( "\tsout | u | du;", 0, "\twheel." );
	auto d = p.client->diagnostics( s.uri, s.version );
	REQUIRE( d );
	bool syntaxError = false;
	for ( const auto & x : ( *d )["diagnostics"] ) syntaxError = syntaxError || x["severity"] == 1;
	CHECK( syntaxError );

	json r = p.client->result( "textDocument/completion", at( s.uri, s.pos( "wheel.\tsout", 6 ) ) );
	std::set<std::string> got = labels( r );
	CHECK( got.count( "centre" ) );
	CHECK( got.count( "radius" ) );
	CHECK_FALSE( got.count( "lo" ) );

	// A chain through a pointer.
	s.insert( "wheel.\tsout", 6, "\n\tpbox->hi." );
	r = p.client->result( "textDocument/completion", at( s.uri, s.pos( "pbox->hi.\tsout", 9 ) ) );
	got = labels( r );
	CHECK( got.count( "x" ) );
	CHECK( got.count( "y" ) );
	CHECK( got.size() == 2 );

	s.insert( "pbox->hi.\tsout", 9, "\n\tpbox->" );
	r = p.client->result( "textDocument/completion", at( s.uri, s.pos( "pbox->\tsout", 6 ) ) );
	got = labels( r );
	CHECK( got.count( "lo" ) );
	CHECK( got.count( "hi" ) );

	// Identifier completion: locals before globals, keywords last.
	s.insert( "pbox->\tsout", 6, "\n\twh" );
	r = p.client->result( "textDocument/completion", at( s.uri, s.pos( "\twh\tsout", 3 ) ) );
	got = labels( r );
	CHECK( got.count( "wheel" ) );
	CHECK( got.count( "while" ) );
}

TEST_CASE( "signature help" ) {
	if ( ! ready() ) return;
	Project & p = project();
	Scratch s( *p.client, "scratch_signature.cfa", p.text );
	s.insert( "\tsout | u | du;", 0, "\tbiggest( 1, " );
	json r = p.client->result( "textDocument/signatureHelp", at( s.uri, s.pos( "biggest( 1, ", 12 ) ) );
	REQUIRE( ! r.is_null() );
	CHECK( contains( r["signatures"][0]["label"], "biggest( T a, T b )" ) );
	CHECK( r["activeParameter"] == 1 );
	json param = r["signatures"][0]["parameters"][1]["label"];
	std::string label = r["signatures"][0]["label"];
	CHECK( label.substr( param[0].get<int>(), param[1].get<int>() - param[0].get<int>() ) == "T b" );

	s.insert( "biggest( 1, ", 12, "2 );\n\tarea( " );
	r = p.client->result( "textDocument/signatureHelp", at( s.uri, s.pos( "\tarea( ", 7 ) ) );
	REQUIRE( ! r.is_null() );
	std::set<std::string> sigs;
	for ( const auto & x : r["signatures"] ) sigs.insert( x["label"].get<std::string>() );
	CHECK( sigs.count( "double area( Rect r )" ) );
	CHECK( sigs.count( "double area( Circle c )" ) );
	CHECK( r["activeParameter"] == 0 );
}

TEST_CASE( "positions follow edits made after the last check" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	// A long debounce: no check runs after the edits, so every answer comes
	// from the first snapshot mapped through the edits.
	c.initialize( { { "debounceMs", 600000 }, { "backend", false } } );
	std::string path = projectDir() + "/geometry.cfa";
	std::string text = readAll( path );
	std::string uri = uriOf( projectDir() + "/scratch_edits.cfa" );
	// The first check runs right away; only later edits are debounced.
	c.open( uri, text );
	REQUIRE( c.diagnostics( uri, 1 ) );

	std::string edited = "// two new lines\n// at the top\n" + text;
	c.change( uri, 2, lspPos( 0, 0 ), lspPos( 0, 0 ), "// two new lines\n// at the top\n" );
	// And some text inserted earlier on the same line as the target.
	json linePos = posOf( edited, "double a1 = area( box )" );
	c.change( uri, 3, linePos, linePos, "  " );
	edited.insert( edited.find( "double a1 = area( box )" ), "  " );

	json h = c.result( "textDocument/hover", at( uri, posOf( edited, "area( box )" ) ) );
	CHECK( contains( hoverText( h ), "double area( Rect r )" ) );
	json d = c.result( "textDocument/definition", at( uri, posOf( edited, "pbox->hi", 6 ) ) );
	REQUIRE( d.size() == 1 );
	CHECK( d[0]["uri"] == uriOf( projectDir() + "/geometry.hfa" ) );
	d = c.result( "textDocument/definition", at( uri, posOf( edited, "box, 1" ) ) );
	REQUIRE( d.size() == 1 );
	CHECK( d[0]["uri"] == uri );
	CHECK( d[0]["range"]["start"] == posOf( edited, "box = {" ) );
	json refs = c.result( "textDocument/references", { { "textDocument", td( uri ) }, { "position", posOf( edited, "box = {" ) },
													   { "context", { { "includeDeclaration", false } } } } );
	bool sawShifted = false;
	for ( const auto & r : refs ) {
		if ( r["range"]["start"] == posOf( edited, "box )" ) ) sawShifted = true;
	}
	CHECK( sawShifted );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "answers for text typed since the last check" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 600000 }, { "backend", false } } );
	std::string text = readAll( projectDir() + "/geometry.cfa" );
	std::string uri = uriOf( projectDir() + "/scratch_typed.cfa" );
	c.open( uri, text );
	REQUIRE( c.diagnostics( uri, 1 ) );
	int version = 1;
	auto insert = [&]( const std::string & needle, int offset, const std::string & ins ) {
		json p = posOf( text, needle, offset );
		text.insert( text.find( needle ) + offset, ins );
		c.change( uri, ++version, p, p, ins );
	};

	// Typing onto the end of a name makes a different name: no answer for the old one.
	insert( "area( box )", 4, "X" );
	CHECK( c.result( "textDocument/hover", at( uri, posOf( text, "areaX( box )", 1 ) ) ).is_null() );
	CHECK( c.result( "textDocument/definition", at( uri, posOf( text, "areaX( box )", 1 ) ) ) == json::array() );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "area( wheel )" ) ) ) ), "double area( Circle c )" ) );

	// '>' in a comparison is not a member access.
	insert( "\tsout | u | du;", 0, "\tif ( a1 >" );
	json params = at( uri, posOf( text, "a1 >\tsout", 4 ) );
	params["context"] = { { "triggerKind", 2 }, { "triggerCharacter", ">" } };
	CHECK( c.result( "textDocument/completion", params )["items"] == json::array() );

	// A local declared since the check, and a dereferenced pointer.
	insert( "a1 >\tsout", 4, " 0 ) {}\n\tCircle & cr = wheel;\n\tcr." );
	CHECK( labels( c.result( "textDocument/completion", at( uri, posOf( text, "cr.\tsout", 3 ) ) ) ) == std::set<std::string>{ "centre", "radius" } );
	// Hover and definition find that declaration in the text too.
	std::string h = hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "cr.\tsout" ) ) ) );
	CHECK( contains( h, "Circle & cr" ) );
	CHECK( contains( h, "from the text" ) );
	json def = c.result( "textDocument/definition", at( uri, posOf( text, "cr.\tsout", 1 ) ) );
	REQUIRE( def.size() == 1 );
	CHECK( def[0]["uri"] == uri );
	CHECK( def[0]["range"]["start"] == posOf( text, "cr = wheel" ) );
	CHECK( def[0]["range"]["end"] == posOf( text, "cr = wheel", 2 ) );
	insert( "cr.\tsout", 3, "centre;\n\t(*pbox)." );
	CHECK( labels( c.result( "textDocument/completion", at( uri, posOf( text, "(*pbox).\tsout", 8 ) ) ) ) == std::set<std::string>{ "lo", "hi" } );

	// A client that sends whole buffers: edits far apart don't hide what is between them.
	std::string whole = "// top\n" + text + "// bottom\n";
	c.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", ++version } } },
										  { "contentChanges", { { { "text", whole } } } } } );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( whole, "area( wheel )" ) ) ) ), "double area( Circle c )" ) );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( whole, "biggest( 3" ) ) ) ), "biggest" ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "a check of a file with a syntax error still covers the code that parses" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 50 }, { "backend", false } } );
	std::string text = readAll( projectDir() + "/geometry.cfa" );
	std::string uri = uriOf( projectDir() + "/scratch_recovery.cfa" );
	c.open( uri, text );
	REQUIRE( c.diagnostics( uri, 1 ) );
	// A half-typed statement, and code after it that no earlier check has seen.
	std::string ins = "\tint half = ;\n\tdouble fresh = a1 * 2;\n\tsout | fresh;\n";
	json p = posOf( text, "\tsout | u | du;" );
	text.insert( text.find( "\tsout | u | du;" ), ins );
	c.change( uri, 2, p, p, ins );
	auto d = c.diagnostics( uri, 2 );
	REQUIRE( d );
	// Only the syntax error: nothing about the code the parser skipped.
	REQUIRE( ( *d )["diagnostics"].size() == 1 );
	CHECK( ( *d )["diagnostics"][0]["range"]["start"]["line"] == posOf( text, "int half" )["line"] );
	CHECK( contains( ( *d )["diagnostics"][0]["message"].get<std::string>(), "syntax error" ) );
	// The new code was translated: hover comes from the translator, not the text.
	std::string h = hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "fresh;" ) ) ) );
	CHECK( contains( h, "double fresh" ) );
	CHECK_FALSE( contains( h, "from the text" ) );
	json def = c.result( "textDocument/definition", at( uri, posOf( text, "a1 * 2" ) ) );
	REQUIRE( def.size() == 1 );
	CHECK( def[0]["range"]["start"] == posOf( text, "a1 = area" ) );

	// A block left open: the `if` takes main's closing brace. The translator closes main at the end of the file
	// and reports main's '{', and main is still translated.
	ins = "\tif ( fresh > 0 ) {\n";
	p = posOf( text, "\tsout | u | du;" );
	text.insert( text.find( "\tsout | u | du;" ), ins );
	c.change( uri, 3, p, p, ins );
	d = c.diagnostics( uri, 3 );
	REQUIRE( d );
	std::set<int> lines;
	for ( const json & x : ( *d )["diagnostics"] ) lines.insert( x["range"]["start"]["line"].get<int>() );
	CHECK( lines == std::set<int>{ posOf( text, "int half" )["line"].get<int>(), posOf( text, "int main" )["line"].get<int>() } );
	h = hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "du;" ) ) ) );
	CHECK( contains( h, "double du" ) );
	CHECK_FALSE( contains( h, "from the text" ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "a header edited but not saved: locations in it follow the buffer" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 600000 }, { "backend", false } } );
	std::string header = projectDir() + "/geometry.hfa", source = projectDir() + "/geometry.cfa";
	std::string htext = readAll( header ), text = readAll( source );
	c.open( uriOf( header ), htext );
	REQUIRE( c.diagnostics( uriOf( header ), 1 ) );
	c.change( uriOf( header ), 2, lspPos( 0, 0 ), lspPos( 0, 0 ), "// one\n// two\n// three\n" );
	std::string edited = "// one\n// two\n// three\n" + htext;
	c.open( uriOf( source ), text );
	REQUIRE( c.diagnostics( uriOf( source ), 1 ) );
	json d = c.result( "textDocument/definition", at( uriOf( source ), posOf( text, "Rect box" ) ) );
	REQUIRE( d.size() == 1 );
	CHECK( d[0]["uri"] == uriOf( header ) );
	CHECK( d[0]["range"]["start"] == posOf( edited, "Rect {" ) );
	CHECK( d[0]["range"]["end"] == posOf( edited, "Rect {", 4 ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "a header and its source both open: references and definition use both analyses" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 50 }, { "backend", false } } );
	std::string header = projectDir() + "/geometry.hfa", source = projectDir() + "/geometry.cfa";
	std::string htext = readAll( header ), text = readAll( source );
	c.open( uriOf( header ), htext );
	REQUIRE( c.diagnostics( uriOf( header ), 1 ) );
	c.open( uriOf( source ), text );
	REQUIRE( c.diagnostics( uriOf( source ), 1 ) );

	json r = c.result( "textDocument/references", { { "textDocument", td( uriOf( header ) ) }, { "position", posOf( htext, "area( Rect r );" ) },
													 { "context", { { "includeDeclaration", true } } } } );
	std::set<std::pair<std::string, int>> got;
	for ( const auto & l : r ) got.insert( { l["uri"].get<std::string>(), l["range"]["start"]["line"].get<int>() } );
	CHECK( got.count( { uriOf( header ), posOf( htext, "area( Rect r );" )["line"].get<int>() } ) );
	CHECK( got.count( { uriOf( source ), posOf( text, "area( Rect r ) {" )["line"].get<int>() } ) );
	CHECK( got.count( { uriOf( source ), posOf( text, "area( box )" )["line"].get<int>() } ) );
	CHECK_FALSE( got.count( { uriOf( source ), posOf( text, "area( Circle c )" )["line"].get<int>() } ) );

	// From the source, the use of Rect inside the header counts too.
	r = c.result( "textDocument/references", { { "textDocument", td( uriOf( source ) ) }, { "position", posOf( text, "Rect box" ) },
												{ "context", { { "includeDeclaration", false } } } } );
	bool inHeader = false;
	for ( const auto & l : r ) {
		if ( l["uri"] == uriOf( header ) && l["range"]["start"] == posOf( htext, "Rect r );" ) ) inHeader = true;
	}
	CHECK( inHeader );

	json d = c.result( "textDocument/definition", at( uriOf( header ), posOf( htext, "area( Rect r );" ) ) );
	REQUIRE( d.size() == 1 );
	CHECK( d[0]["uri"] == uriOf( source ) );
	CHECK( d[0]["range"]["start"] == posOf( text, "area( Rect r ) {" ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "operators, postfix calls and default arguments" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 50 }, { "backend", false } } );
	std::string text =
		"struct Vec { int x; };\n"
		"Vec ?+?( Vec a, Vec b ) { return a; }\n"
		"int ?`len( Vec v ) { return v.x; }\n"
		"void put( int a, int b = 4 ) {}\n"
		"int main() {\n"
		"\tVec v = { 1 }, w = { 2 };\n"
		"\tVec s = v + w;\n"
		"\tput( v`len );\n"
		"\treturn s.x;\n"
		"}\n";
	std::string uri = uriOf( projectDir() + "/scratch_ops.cfa" );
	c.open( uri, text );
	REQUIRE( c.diagnostics( uri, 1 ) );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "+ w" ) ) ) ), "Vec ?+?( Vec a, Vec b )" ) );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "len )" ) ) ) ), "int ?`len( Vec v )" ) );
	CHECK( contains( hoverText( c.result( "textDocument/hover", at( uri, posOf( text, "put( v" ) ) ) ), "void put( int a, int b = 4 )" ) );
	json r = c.result( "textDocument/references", { { "textDocument", td( uri ) }, { "position", posOf( text, "?+?" ) },
													 { "context", { { "includeDeclaration", false } } } } );
	REQUIRE( r.size() == 1 );
	CHECK( r[0]["range"]["start"] == posOf( text, "+ w" ) );
	CHECK( c.shutdown() == 0 );
}

TEST_CASE( "code actions: did you mean, and the #include a libcfa name needs" ) {
	if ( ! ready() ) return;
	LspClient c( envOr( "CFA_LSP_TEST_LOG" ) );
	c.initialize( { { "debounceMs", 50 }, { "backend", false } } );
	auto actions = [&]( const std::string & uri, const json & diag ) {
		return c.result( "textDocument/codeAction", { { "textDocument", td( uri ) }, { "range", diag["range"] },
													  { "context", { { "diagnostics", json::array( { diag } ) } } } } );
	};
	auto titles = []( const json & as ) {
		std::set<std::string> out;
		for ( const auto & a : as ) out.insert( a["title"].get<std::string>() );
		return out;
	};

	// A typo, and sout without fstream.hfa.
	std::string text =
		"// Nothing included.\n"
		"int main() {\n"
		"\tint count = 1;\n"
		"\tcount = cuont + 1;\n"
		"\tsout | count;\n"
		"}\n";
	std::string uri = uriOf( projectDir() + "/scratch_actions.cfa" );
	c.open( uri, text );
	auto d = c.diagnostics( uri, 1 );
	REQUIRE( d );
	json typo, missing;
	for ( const auto & x : ( *d )["diagnostics"] ) {
		if ( x["message"] == "use of undeclared identifier `cuont`" ) typo = x;
		if ( x["message"] == "use of undeclared identifier `sout`" ) missing = x;
	}
	INFO( ( *d )["diagnostics"].dump() );
	REQUIRE( ! typo.is_null() );
	REQUIRE( ! missing.is_null() );

	json as = actions( uri, typo );
	REQUIRE( ! as.empty() );
	CHECK( as[0]["title"] == "Change `cuont` to `count`" );
	json e = as[0]["edit"]["changes"][uri][0];
	CHECK( e["range"]["start"] == posOf( text, "cuont" ) );
	CHECK( e["range"]["end"] == posOf( text, "cuont", 5 ) );
	CHECK( e["newText"] == "count" );

	as = actions( uri, missing );
	REQUIRE( ! as.empty() );
	CHECK( as[0]["title"] == "Add #include <fstream.hfa>" );
	e = as[0]["edit"]["changes"][uri][0];
	CHECK( e["range"]["start"] == lspPos( 1, 0 ) );		// after the leading comment
	CHECK( e["range"]["end"] == lspPos( 1, 0 ) );
	CHECK( e["newText"] == "#include <fstream.hfa>\n" );

	// A type without its header is a syntax error at the name after it.
	std::string stext =
		"#include <fstream.hfa>\n"
		"int main() {\n"
		"\tstring s = \"hi\";\n"
		"\tsout | s;\n"
		"}\n";
	std::string suri = uriOf( projectDir() + "/scratch_actions_type.cfa" );
	c.open( suri, stext );
	auto sd = c.diagnostics( suri, 1 );
	REQUIRE( sd );
	INFO( ( *sd )["diagnostics"].dump() );
	json syntax;
	for ( const auto & x : ( *sd )["diagnostics"] ) {
		if ( x["message"].get<std::string>().starts_with( "syntax error" ) ) syntax = x;
	}
	REQUIRE( ! syntax.is_null() );
	as = actions( suri, syntax );
	CHECK( titles( as ).count( "Add #include <string.hfa>" ) );
	for ( const auto & a : as ) {
		if ( a["title"] != "Add #include <string.hfa>" ) continue;
		CHECK( a["edit"]["changes"][suri][0]["range"]["start"] == lspPos( 1, 0 ) );		// after the #include
	}
	CHECK( c.shutdown() == 0 );
}

} // TEST_SUITE
