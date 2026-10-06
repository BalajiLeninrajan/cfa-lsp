#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>

#include "TextScan.hpp"
#include "fixture.hpp"

using namespace cfalsp;
using fixture::cfa;
using fixture::hfa;

namespace {

bool contains( const std::string & s, const std::string & part ) { return s.find( part ) != std::string::npos; }

const CompletionItem * findItem( const std::vector<CompletionItem> & items, const std::string & label ) {
	auto it = std::find_if( items.begin(), items.end(), [&]( const CompletionItem & c ) { return c.label == label; } );
	return it == items.end() ? nullptr : &*it;
}

int countItems( const std::vector<CompletionItem> & items, const std::string & label ) {
	return int( std::count_if( items.begin(), items.end(), [&]( const CompletionItem & c ) { return c.label == label; } ) );
}

Range rng( int l1, int c1, int l2, int c2 ) { return { { l1, c1 }, { l2, c2 } }; }

const SemanticToken * tokenAt( const std::vector<SemanticToken> & toks, int line, int col ) {
	for ( const auto & t : toks ) if ( t.start.line == line && t.start.col == col ) return &t;
	return nullptr;
}

int typeIndex( const std::string & name ) {
	const auto & types = Analysis::tokenTypes();
	return int( std::find( types.begin(), types.end(), name ) - types.begin() );
}

int modBit( const std::string & name ) {
	const auto & mods = Analysis::tokenModifiers();
	return 1 << int( std::find( mods.begin(), mods.end(), name ) - mods.begin() );
}

} // namespace

TEST_SUITE( "analysis" ) {

// The fixtures are hand-written; make sure every range spells what it claims.
TEST_CASE( "fixture dumps are consistent with their sources" ) {
	for ( const char * name : { "shapes", "broken" } ) {
		auto j = fixture::dump( name );
		std::map<long long, std::string> names;
		for ( const auto & d : j["decls"] ) {
			names[d["id"].get<long long>()] = d["name"];
			if ( d["line"].get<int>() <= 0 || d.value( "generated", false ) ) continue;
			auto src = fixture::read( d["file"] );
			if ( !src ) continue;
			text::FileText ft( *src );
			const auto & nr = d["nameRange"];
			Range r = rng( nr["line"].get<int>() - 1, nr["col"], nr["endLine"].get<int>() - 1, nr["endCol"] );
			INFO( std::string( name ) << " decl " << d["id"] );
			CHECK( ft.slice( r ) == d["name"].get<std::string>() );
		}
		for ( const auto & ref : j["refs"] ) {
			text::FileText ft( fixture::read( ref["file"] ).value() );
			Range r = rng( ref["line"].get<int>() - 1, ref["col"], ref["endLine"].get<int>() - 1, ref["endCol"] );
			INFO( std::string( name ) << " ref at " << ref["line"] << ":" << ref["col"] );
			CHECK( ft.slice( r ) == names[ref["decl"].get<long long>()] );
		}
		for ( const auto & e : j["exprs"] ) {
			text::FileText ft( fixture::read( e["file"] ).value() );
			Range r = rng( e["line"].get<int>() - 1, e["col"], e["endLine"].get<int>() - 1, e["endCol"] );
			std::string s = ft.slice( r );
			INFO( std::string( name ) << " expr at " << e["line"] << ":" << e["col"] << " = " << s );
			CHECK( !s.empty() );
			CHECK( std::count( s.begin(), s.end(), '(' ) == std::count( s.begin(), s.end(), ')' ) );
		}
	}
}

TEST_CASE( "load reports completeness and diagnostics" ) {
	auto a = fixture::load( "shapes" );
	CHECK( a->complete() );
	CHECK( a->diagnostics().empty() );

	auto b = fixture::load( "broken" );
	CHECK_FALSE( b->complete() );
	auto ds = b->diagnostics();
	REQUIRE( ds.size() == 4 );
	CHECK( ds[0].loc.file == fixture::broken );
	CHECK( ds[0].loc.range == rng( 4, 9, 4, 21 ) );
	CHECK( ds[0].severity == 1 );
	CHECK( ds[0].message.starts_with( "No alternatives for expression area( b, 1 )" ) );
	CHECK( contains( ds[0].message, "double area( double r )" ) );
	CHECK( ds[1].severity == 2 );
	CHECK( ds[2].severity == 3 );
	CHECK( ds[2].loc.file == hfa );
	CHECK( ds[3].loc.range == rng( 0, 0, 0, 0 ) );
	CHECK( ds[3].message == "translation stopped after errors" );
}

TEST_CASE( "load rejects malformed dumps" ) {
	CHECK_THROWS_AS( fixture::loadText( "[]" ), std::runtime_error );
	CHECK_THROWS_AS( fixture::loadText( "{}" ), std::runtime_error );
	CHECK_THROWS_AS( fixture::loadText( R"J({"format": 2})J" ), std::runtime_error );
	CHECK_THROWS_AS( fixture::loadText( R"J({"format": 1, "decls": {}})J" ), std::runtime_error );
	CHECK_NOTHROW( fixture::loadText( R"J({"format": 1})J" ) );
}

TEST_CASE( "hover on a call shows the chosen overload" ) {
	auto a = fixture::load( "shapes" );
	auto h = a->hover( cfa, { 32, 16 } );		// area( b )
	REQUIRE( h );
	CHECK( h->range == rng( 32, 15, 32, 19 ) );
	CHECK( contains( h->markdown, "```cfa\nint area( Box & b )\n```" ) );
	CHECK( contains( h->markdown, "function" ) );
	CHECK( contains( h->markdown, "`shapes.hfa:17`" ) );
	CHECK( contains( h->markdown, "1 other overload" ) );
	CHECK_FALSE( contains( h->markdown, "other overloads" ) );
	CHECK( contains( h->markdown, "Area of a box." ) );

	h = a->hover( cfa, { 37, 9 } );				// area( 2.0 )
	REQUIRE( h );
	CHECK( contains( h->markdown, "double area( double r )" ) );
	CHECK( contains( h->markdown, "`shapes.cfa:9`" ) );
	CHECK( contains( h->markdown, "Area of a circle with radius r." ) );
}

TEST_CASE( "hover finds the doc comment on the other declaration of the same function" ) {
	auto a = fixture::load( "shapes" );
	auto h = a->hover( hfa, { 17, 8 } );		// prototype of area( double ), documented at the definition
	REQUIRE( h );
	CHECK( contains( h->markdown, "Area of a circle with radius r." ) );
	h = a->hover( cfa, { 3, 5 } );				// definition of area( Box & ), documented at the prototype
	REQUIRE( h );
	CHECK( contains( h->markdown, "Area of a box." ) );
}

TEST_CASE( "hover right after an identifier counts as on it" ) {
	auto a = fixture::load( "shapes" );
	auto h = a->hover( cfa, { 35, 14 } );		// end of `inner`
	REQUIRE( h );
	CHECK( h->range == rng( 35, 9, 35, 14 ) );
	CHECK( contains( h->markdown, "int inner" ) );
	CHECK( contains( h->markdown, "local variable" ) );
	CHECK_FALSE( contains( h->markdown, "overload" ) );
}

TEST_CASE( "hover on fields, with, library and declarations" ) {
	auto a = fixture::load( "shapes" );

	auto h = a->hover( cfa, { 35, 20 } );		// b.hi
	REQUIRE( h );
	CHECK( contains( h->markdown, "Point hi" ) );
	CHECK( contains( h->markdown, "field of `Box`" ) );
	CHECK( contains( h->markdown, "`shapes.hfa:11`" ) );

	h = a->hover( cfa, { 35, 22 } );			// .x
	REQUIRE( h );
	CHECK( contains( h->markdown, "horizontal offset" ) );

	h = a->hover( cfa, { 4, 10 } );				// hi through with( b )
	REQUIRE( h );
	CHECK( contains( h->markdown, "field of `Box` (through `with`)" ) );

	h = a->hover( cfa, { 35, 3 } );				// sout
	REQUIRE( h );
	CHECK( contains( h->markdown, "extern ofstream & sout" ) );
	CHECK( contains( h->markdown, "global variable" ) );
	CHECK( contains( h->markdown, "`fstream.hfa:8`" ) );
	CHECK( contains( h->markdown, "Standard output stream." ) );

	h = a->hover( cfa, { 12, 12 } );			// coroutine Walker's own name
	REQUIRE( h );
	CHECK( contains( h->markdown, "coroutine Walker" ) );
	CHECK( h->range == rng( 12, 10, 12, 16 ) );

	h = a->hover( cfa, { 17, 6 } );				// main( Walker & ), overloaded with main()
	REQUIRE( h );
	CHECK( contains( h->markdown, "1 other overload" ) );

	h = a->hover( cfa, { 26, 9 } );				// area inside twice is the trait assertion
	REQUIRE( h );
	CHECK( contains( h->markdown, "int area( T & )" ) );
	CHECK( contains( h->markdown, "assertion of `Shape`" ) );

	h = a->hover( cfa, { 13, 6 } );				// field with a trailing comment
	REQUIRE( h );
	CHECK( contains( h->markdown, "box being walked" ) );

	h = a->hover( hfa, { 3, 8 } );				// struct Point, not its generated constructor
	REQUIRE( h );
	CHECK( contains( h->markdown, "struct Point" ) );
	CHECK( contains( h->markdown, "A point on the integer grid." ) );
	CHECK_FALSE( contains( h->markdown, "?{}" ) );

	h = a->hover( hfa, { 9, 8 } );				// block comment
	REQUIRE( h );
	CHECK( contains( h->markdown, "An axis-aligned box." ) );

	h = a->hover( hfa, { 10, 8 } );				// `lo` in `Point lo, hi;` has no doc of its own
	REQUIRE( h );
	CHECK_FALSE( contains( h->markdown, "offset" ) );
}

TEST_CASE( "hover on an expression without a ref shows its type" ) {
	auto a = fixture::load( "shapes" );
	auto h = a->hover( cfa, { 32, 20 } );		// inside area( b ), between ( and b
	REQUIRE( h );
	CHECK( h->markdown == "```cfa\nint\n```" );
	CHECK( h->range == rng( 32, 15, 32, 24 ) );
}

TEST_CASE( "hover misses" ) {
	auto a = fixture::load( "shapes" );
	CHECK_FALSE( a->hover( cfa, { 30, 0 } ) );			// leading tab
	CHECK_FALSE( a->hover( "/nowhere.cfa", { 0, 0 } ) );
	CHECK_FALSE( a->hover( cfa, { 500, 3 } ) );
	CHECK_FALSE( a->hover( cfa, { -1, -1 } ) );
}

TEST_CASE( "hover on unresolved code falls back to name lookup" ) {
	auto a = fixture::load( "broken" );
	auto h = a->hover( fixture::broken, { 4, 10 } );	// area( b, 1 ) failed to resolve
	REQUIRE( h );
	CHECK( contains( h->markdown, "int area( Box & b )" ) );
	CHECK( contains( h->markdown, "Not resolved" ) );
	CHECK( contains( h->markdown, "2 declarations" ) );
	CHECK( h->range == rng( 4, 9, 4, 13 ) );
}

TEST_CASE( "definition goes to the chosen declaration's body" ) {
	auto a = fixture::load( "shapes" );
	auto d = a->definition( cfa, { 32, 16 } );			// call bound to the prototype in the header
	REQUIRE( d.size() == 1 );
	CHECK( d[0].file == cfa );
	CHECK( d[0].range == rng( 3, 4, 3, 8 ) );

	d = a->definition( cfa, { 37, 22 } );				// n
	REQUIRE( d.size() == 1 );
	CHECK( d[0].range == rng( 32, 11, 32, 12 ) );

	d = a->definition( cfa, { 35, 33 } );				// w.box.lo
	REQUIRE( d.size() == 1 );
	CHECK( d[0].file == hfa );
	CHECK( d[0].range == rng( 10, 7, 10, 9 ) );

	d = a->definition( cfa, { 35, 4 } );				// sout, in libcfa
	REQUIRE( d.size() == 1 );
	CHECK( d[0].file == fixture::libcfa );

	d = a->definition( cfa, { 12, 11 } );				// a declaration's own name
	REQUIRE( d.size() == 1 );
	CHECK( d[0].range == rng( 12, 10, 12, 16 ) );

	CHECK( a->definition( cfa, { 30, 0 } ).empty() );
	CHECK( a->definition( "/nowhere.cfa", { 1, 1 } ).empty() );
}

TEST_CASE( "definition without a ref lists every visible declaration" ) {
	auto a = fixture::load( "broken" );
	auto d = a->definition( fixture::broken, { 4, 11 } );	// area
	REQUIRE( d.size() == 2 );
	CHECK( d[0] == Location{ hfa, rng( 16, 4, 16, 8 ) } );
	CHECK( d[1] == Location{ hfa, rng( 17, 7, 17, 11 ) } );

	d = a->definition( fixture::broken, { 4, 15 } );		// b, a local found without scopes
	REQUIRE( d.size() == 1 );
	CHECK( d[0] == Location{ fixture::broken, rng( 3, 5, 3, 6 ) } );

	d = a->definition( fixture::broken, { 5, 9 } );		// right after `a`
	REQUIRE( d.size() == 1 );
	CHECK( d[0].range == rng( 4, 5, 4, 6 ) );

	CHECK( a->definition( fixture::broken, { 5, 3 } ).empty() );	// `return`
}

TEST_CASE( "definition fallback puts locals before globals" ) {
	// A local `area` shadows the global functions.
	auto a = fixture::loadText( R"J({"format": 1, "complete": false,
		"decls": [
			{"id": 1, "name": "area", "kind": "function", "file": "/fixture/shapes/shapes.hfa", "line": 17, "col": 0, "endLine": 17, "endCol": 19,
			 "nameRange": {"line": 17, "col": 4, "endLine": 17, "endCol": 8}, "type": "int (Box &)", "signature": "int area( Box & b )"},
			{"id": 2, "name": "main", "kind": "function", "file": "/fixture/broken/broken.cfa", "line": 3, "col": 0, "endLine": 7, "endCol": 1,
			 "nameRange": {"line": 3, "col": 4, "endLine": 3, "endCol": 8}, "body": {"line": 3, "col": 11, "endLine": 7, "endCol": 1}},
			{"id": 3, "name": "area", "kind": "variable", "file": "/fixture/broken/broken.cfa", "line": 4, "col": 1, "endLine": 4, "endCol": 6,
			 "nameRange": {"line": 4, "col": 5, "endLine": 4, "endCol": 6}, "parent": 2, "local": true}
		]})J" );
	auto d = a->definition( fixture::broken, { 4, 10 } );
	REQUIRE( d.size() == 2 );
	CHECK( d[0].file == fixture::broken );
	CHECK( d[1].file == hfa );
}

TEST_CASE( "references" ) {
	auto a = fixture::load( "shapes" );
	auto r = a->references( cfa, { 4, 12 }, false );		// field x
	REQUIRE( r.size() == 3 );
	CHECK( r[0] == Location{ cfa, rng( 4, 12, 4, 13 ) } );
	CHECK( r[1] == Location{ cfa, rng( 4, 19, 4, 20 ) } );
	CHECK( r[2] == Location{ cfa, rng( 35, 22, 35, 23 ) } );

	r = a->references( cfa, { 4, 12 }, true );
	REQUIRE( r.size() == 4 );
	CHECK( r[3] == Location{ hfa, rng( 4, 5, 4, 6 ) } );

	// The prototype and the definition are one function.
	r = a->references( cfa, { 3, 5 }, true );
	REQUIRE( r.size() == 3 );
	CHECK( r[0] == Location{ cfa, rng( 3, 4, 3, 8 ) } );
	CHECK( r[1] == Location{ cfa, rng( 32, 15, 32, 19 ) } );
	CHECK( r[2] == Location{ hfa, rng( 16, 4, 16, 8 ) } );

	// ... but not the other overload, nor the trait assertion.
	r = a->references( cfa, { 37, 9 }, false );
	REQUIRE( r.size() == 1 );
	CHECK( r[0].range == rng( 37, 8, 37, 12 ) );

	r = a->references( cfa, { 32, 11 }, true );		// const int n
	CHECK( r.size() == 3 );

	CHECK( a->references( cfa, { 30, 0 }, true ).empty() );
	CHECK( a->references( "/nowhere.cfa", { 0, 0 }, true ).empty() );
}

TEST_CASE( "unit index: entities, uses and symbols for other files" ) {
	auto a = fixture::load( "shapes" );
	UnitIndex u = a->unitIndex();
	auto entity = [&]( const std::string & name, const Location & decl ) {
		for ( int i = 0; i < int( u.entities.size() ); i += 1 ) {
			const auto & ds = u.entities[i].declarations;
			if ( u.entities[i].name == name && std::find( ds.begin(), ds.end(), decl ) != ds.end() ) return i;
		}
		return -1;
	};
	auto refAt = [&]( Range r ) -> const UnitIndex::Ref * {
		for ( const auto & x : u.refs ) {
			if ( x.loc == Location{ cfa, r } ) return &x;
		}
		return nullptr;
	};

	// The prototype and the definition are one entity, with the body.
	int box = entity( "area", { hfa, rng( 16, 4, 16, 8 ) } );
	REQUIRE( box >= 0 );
	CHECK( entity( "area", { cfa, rng( 3, 4, 3, 8 ) } ) == box );
	CHECK( u.entities[box].declarations.size() == 2 );
	REQUIRE( u.entities[box].definition );
	CHECK( *u.entities[box].definition == Location{ cfa, rng( 3, 4, 3, 8 ) } );
	CHECK( u.entities[box].definitionRange.start <= Loc{ 3, 4 } );
	CHECK( u.entities[box].function );
	CHECK( ! u.entities[box].library );
	CHECK( u.entities[box].kind == 12 );
	CHECK( u.entities[box].detail == "int area( Box & b )" );
	int mainFn = entity( "main", { cfa, rng( 29, 4, 29, 8 ) } );
	int twice = entity( "twice", { cfa, rng( 25, 4, 25, 9 ) } );
	REQUIRE( mainFn >= 0 );
	REQUIRE( twice >= 0 );

	// Calls know the function they are in.
	const UnitIndex::Ref * call = refAt( rng( 32, 15, 32, 19 ) );
	REQUIRE( call );
	CHECK( call->entity == box );
	CHECK( call->call );
	CHECK( call->caller == mainFn );
	call = refAt( rng( 26, 8, 26, 12 ) );			// the trait's area, inside twice
	REQUIRE( call );
	CHECK( call->caller == twice );
	CHECK( u.entities[call->entity].name == "area" );
	CHECK( call->entity != box );

	// sout is used here but declared only in libcfa.
	const UnitIndex::Ref * sout = refAt( rng( 35, 2, 35, 6 ) );
	REQUIRE( sout );
	CHECK( u.entities[sout->entity].library );
	CHECK( ! sout->call );

	// Locals are left out.
	CHECK( ! refAt( rng( 32, 21, 32, 22 ) ) );		// b
	CHECK( ! refAt( rng( 34, 14, 34, 15 ) ) );		// n
	CHECK( entity( "inner", { cfa, rng( 34, 6, 34, 11 ) } ) < 0 );

	// Symbols come from the project files, not libcfa.
	bool sawTwice = false, sawField = false;
	for ( const auto & s : u.symbols ) {
		CHECK( s.loc.file != fixture::libcfa );
		if ( s.name == "twice" && s.loc == Location{ cfa, rng( 25, 4, 25, 9 ) } ) sawTwice = true;
		if ( s.name == "x" && s.container == "Point" && s.loc == Location{ hfa, rng( 4, 5, 4, 6 ) } ) sawField = true;
	}
	CHECK( sawTwice );
	CHECK( sawField );

	// Every spelling of area is a use or a declaration in the dump.
	for ( const auto & l : u.loose ) CHECK( l.name != "area" );
}

TEST_CASE( "document symbols" ) {
	auto a = fixture::load( "shapes" );
	auto syms = a->documentSymbols( hfa );
	std::vector<std::string> names;
	for ( const auto & s : syms ) names.push_back( s.name );
	CHECK( names == std::vector<std::string>{ "Point", "Box", "area", "area", "Shape", "Colour" } );
	REQUIRE( syms.size() == 6 );
	CHECK( syms[0].kind == 23 );
	REQUIRE( syms[0].children.size() == 2 );
	CHECK( syms[0].children[0].name == "x" );
	CHECK( syms[0].children[0].kind == 8 );
	CHECK( syms[0].children[0].detail == "int" );
	CHECK( syms[0].range == rng( 3, 0, 6, 2 ) );
	CHECK( syms[0].selectionRange == rng( 3, 7, 3, 12 ) );
	CHECK( syms[2].kind == 12 );
	CHECK( syms[2].detail == "int (Box &)" );
	CHECK( syms[4].kind == 11 );
	REQUIRE( syms[4].children.size() == 1 );			// the assertion, not the type parameter
	CHECK( syms[4].children[0].name == "area" );
	CHECK( syms[5].kind == 10 );
	REQUIRE( syms[5].children.size() == 3 );
	CHECK( syms[5].children[0].kind == 22 );

	syms = a->documentSymbols( cfa );
	names.clear();
	for ( const auto & s : syms ) names.push_back( s.name );
	CHECK( names == std::vector<std::string>{ "area", "area", "Walker", "main", "twice", "main" } );
	REQUIRE( syms.size() == 6 );
	CHECK( syms[2].kind == 5 );
	CHECK( syms[2].detail == "coroutine" );
	REQUIRE( syms[2].children.size() == 2 );			// no generated __cor
	CHECK( syms[2].children[1].name == "steps" );
	CHECK( syms[4].selectionRange == rng( 25, 4, 25, 9 ) );
	CHECK( syms[4].range == rng( 24, 0, 27, 1 ) );
	for ( const auto & s : syms ) CHECK( s.children.size() <= 2 );	// no locals or parameters

	CHECK( a->documentSymbols( "/nowhere.cfa" ).empty() );
}

TEST_CASE( "document symbols name anonymous aggregates" ) {
	auto a = fixture::loadText( R"J({"format": 1, "decls": [
		{"id": 1, "name": "", "kind": "enum", "file": "/x.cfa", "line": 1, "col": 0, "endLine": 1, "endCol": 20,
		 "nameRange": {"line": 1, "col": 0, "endLine": 1, "endCol": 4}},
		{"id": 2, "name": "LIMIT", "kind": "enumerator", "file": "/x.cfa", "line": 1, "col": 7, "endLine": 1, "endCol": 17,
		 "nameRange": {"line": 1, "col": 7, "endLine": 1, "endCol": 12}, "parent": 1}
	]})J" );
	auto syms = a->documentSymbols( "/x.cfa" );
	REQUIRE( syms.size() == 1 );
	CHECK( syms[0].name == "(anonymous enum)" );
	REQUIRE( syms[0].children.size() == 1 );
	CHECK( syms[0].children[0].name == "LIMIT" );
}

TEST_CASE( "member completion uses the resolved expression type" ) {
	auto a = fixture::load( "shapes" );
	// The user retypes `b.hi.` where the snapshot has `b.hi.x`.
	auto items = a->completion( cfa, { 35, 22 }, "\t\tsout | inner | b.hi." );
	REQUIRE( items.size() == 2 );
	CHECK( findItem( items, "x" ) );
	CHECK( findItem( items, "y" ) );
	CHECK( findItem( items, "x" )->kind == 5 );
	CHECK( findItem( items, "x" )->detail == "int" );
	CHECK( findItem( items, "x" )->documentation == "horizontal offset" );

	items = a->completion( cfa, { 35, 23 }, "\t\tsout | inner | b.hi.x" );
	REQUIRE( items.size() == 1 );
	CHECK( items[0].label == "x" );

	// `sout | inner | b.hi.x` also ends there; only the operand's own expression counts.
	items = a->completion( cfa, { 35, 24 }, "\t\tsout | inner | b.hi.x." );
	CHECK( items.empty() );

	// Expressions ending at the same place but spelled differently don't count.
	items = a->completion( cfa, { 35, 19 }, "\t\tsout | inner | q." );
	CHECK( items.empty() );

	// Coroutine fields, without the generated ones.
	items = a->completion( cfa, { 19, 4 }, "\t\tw." );
	REQUIRE( items.size() == 2 );
	CHECK( findItem( items, "box" ) );
	CHECK( findItem( items, "steps" ) );

	// A library variable.
	items = a->completion( cfa, { 35, 7 }, "\t\tsout." );
	REQUIRE( items.size() == 1 );
	CHECK( items[0].label == "file" );
}

TEST_CASE( "member completion falls back to names" ) {
	auto a = fixture::load( "shapes" );
	// New code on a line the snapshot doesn't have.
	auto items = a->completion( cfa, { 37, 1 }, "\tw.box." );
	REQUIRE( items.size() == 2 );
	CHECK( findItem( items, "lo" ) );
	CHECK( findItem( items, "hi" ) );

	items = a->completion( cfa, { 37, 1 }, "\tw -> box .lo.  " );
	CHECK( items.size() == 2 );

	items = a->completion( cfa, { 37, 1 }, "\tw.box.lo.y" );
	REQUIRE( items.size() == 1 );
	CHECK( items[0].label == "y" );

	// Through a `with` clause: `lo` is a field of the parameter b.
	items = a->completion( cfa, { 4, 9 }, "\treturn lo." );
	CHECK( items.size() == 2 );

	// A parameter.
	items = a->completion( cfa, { 19, 4 }, "\tw.box." );
	CHECK( items.size() == 2 );

	// Out of scope: `w` in main( Walker & ) isn't visible in area.
	items = a->completion( cfa, { 4, 9 }, "\tw.box." );
	CHECK( items.empty() );

	auto b = fixture::load( "broken" );
	items = b->completion( fixture::broken, { 5, 1 }, "\tb.hi." );
	CHECK( items.size() == 2 );
}

TEST_CASE( "identifier completion: locals, globals, keywords" ) {
	auto a = fixture::load( "shapes" );
	auto items = a->completion( cfa, { 35, 2 }, "\t\t" );
	auto inner = findItem( items, "inner" );
	REQUIRE( inner );
	CHECK( inner->sortText == "0inner" );
	CHECK( inner->kind == 6 );
	CHECK( findItem( items, "n" ) );
	CHECK( findItem( items, "b" ) );
	CHECK( findItem( items, "w" ) );
	CHECK_FALSE( findItem( items, "i" ) );				// another function's local
	CHECK_FALSE( findItem( items, "s" ) );
	CHECK_FALSE( findItem( items, "x" ) );				// fields aren't plain names

	auto area = findItem( items, "area" );
	REQUIRE( area );
	CHECK( countItems( items, "area" ) == 1 );
	CHECK( area->sortText == "1area" );
	CHECK( area->kind == 3 );
	CHECK( contains( area->detail, "(+1 overload)" ) );

	auto point = findItem( items, "Point" );
	REQUIRE( point );
	CHECK( point->sortText == "2Point" );
	CHECK( point->kind == 22 );
	CHECK( findItem( items, "Walker" )->kind == 7 );
	CHECK( findItem( items, "Shape" )->kind == 8 );
	CHECK( findItem( items, "Red" )->kind == 20 );

	auto sout = findItem( items, "sout" );
	REQUIRE( sout );
	CHECK( sout->sortText == "3sout" );
	CHECK( findItem( items, "printf" ) );
	CHECK( findItem( items, "sized" ) );
	CHECK( countItems( items, "open" ) == 1 );
	CHECK( contains( findItem( items, "open" )->detail, "(+1 overload)" ) );

	CHECK_FALSE( findItem( items, "?+?" ) );
	CHECK_FALSE( findItem( items, "?|?" ) );
	CHECK_FALSE( findItem( items, "?{}" ) );
	CHECK_FALSE( findItem( items, "__cfa_flags" ) );
	CHECK_FALSE( findItem( items, "__builtin_expect" ) );
	CHECK_FALSE( findItem( items, "__cor" ) );

	auto kw = findItem( items, "forall" );
	REQUIRE( kw );
	CHECK( kw->sortText == "4forall" );
	CHECK( kw->kind == 14 );
	CHECK( findItem( items, "catchResume" ) );
	CHECK( findItem( items, "while" ) );

	// Sort order: locals < project < libcfa < keywords.
	CHECK( inner->sortText < area->sortText );
	CHECK( area->sortText < point->sortText );
	CHECK( point->sortText < sout->sortText );
	CHECK( sout->sortText < kw->sortText );
}

TEST_CASE( "identifier completion filters by prefix" ) {
	auto a = fixture::load( "shapes" );
	auto items = a->completion( cfa, { 32, 17 }, "\tconst int n = ar" );
	REQUIRE( items.size() == 1 );
	CHECK( items[0].label == "area" );
	CHECK( items[0].documentation == "Area of a box." );	// the first focus-file definition, documented at its prototype

	items = a->completion( cfa, { 32, 17 }, "\tconst int n = w" );
	CHECK( findItem( items, "w" ) );						// declared earlier in main
	CHECK( findItem( items, "while" ) );
	CHECK( findItem( items, "Walker" ) );					// case-insensitive
	CHECK_FALSE( findItem( items, "inner" ) );

	// Locals declared after the cursor aren't offered.
	items = a->completion( cfa, { 30, 1 }, "\tn" );
	CHECK_FALSE( findItem( items, "n" ) );

	items = a->completion( cfa, { 35, 2 }, "\t\t__" );
	CHECK( findItem( items, "__cfa_flags" ) );
	CHECK( findItem( items, "__builtin_expect" ) );
}

TEST_CASE( "identifier completion sees parameters, type parameters and with fields" ) {
	auto a = fixture::load( "shapes" );
	auto items = a->completion( cfa, { 26, 1 }, "\t" );
	CHECK( findItem( items, "s" ) );
	REQUIRE( findItem( items, "T" ) );
	CHECK( findItem( items, "T" )->kind == 25 );

	items = a->completion( cfa, { 4, 9 }, "\treturn (" );
	REQUIRE( findItem( items, "b" ) );
	auto lo = findItem( items, "lo" );
	REQUIRE( lo );
	CHECK( lo->sortText == "0lo" );
	CHECK( contains( lo->detail, "with Box" ) );
	CHECK( findItem( items, "hi" ) );

	items = a->completion( cfa, { 19, 2 }, "\t\t" );
	CHECK( findItem( items, "i" ) );
	CHECK( findItem( items, "w" ) );
	CHECK_FALSE( findItem( items, "steps" ) );			// no with clause in this main
}

TEST_CASE( "no completion in comments, strings or numbers" ) {
	auto a = fixture::load( "shapes" );
	CHECK( a->completion( cfa, { 35, 6 }, "\t\t// ar" ).empty() );
	CHECK( a->completion( cfa, { 35, 6 }, "\t\tprintf( \"ar" ).empty() );
	CHECK( a->completion( cfa, { 35, 6 }, "\t\tx = 1." ).empty() );
	CHECK( a->completion( cfa, { 35, 6 }, "\t\tx = 1.5" ).empty() );
	CHECK( a->completion( cfa, { 35, 6 }, "#include <fs" ).empty() );
	CHECK( a->completion( cfa, { 35, 6 }, "\t/* ar" ).empty() );
	CHECK_FALSE( a->completion( cfa, { 35, 6 }, "\t/* x */ ar" ).empty() );
}

TEST_CASE( "completion in a file not in the dump still offers globals" ) {
	auto a = fixture::load( "shapes" );
	auto items = a->completion( "/elsewhere/new.cfa", { 0, 2 }, "ar" );
	REQUIRE( findItem( items, "area" ) );
	CHECK( a->completion( "/elsewhere/new.cfa", { 0, 2 }, "b.x." ).empty() );
}

TEST_CASE( "signature help lists overloads, the resolved one first" ) {
	auto a = fixture::load( "shapes" );
	std::string before = fixture::textBefore( cfa, 32, 21 );	// "const int n = area( "
	REQUIRE( before.ends_with( "area( " ) );
	auto h = a->signatureHelp( cfa, { 32, 21 }, before );
	REQUIRE( h );
	REQUIRE( h->signatures.size() == 2 );
	CHECK( h->signatures[0].label == "int area( Box & b )" );
	REQUIRE( h->signatures[0].params.size() == 1 );
	auto [b, e] = h->signatures[0].params[0];
	CHECK( h->signatures[0].label.substr( b, e - b ) == "Box & b" );
	CHECK( h->signatures[0].documentation == "Area of a box." );
	CHECK( h->signatures[1].label == "double area( double r )" );
	CHECK( h->activeSignature == 0 );
	CHECK( h->activeParameter == 0 );

	// The call at line 38 resolved to the other overload.
	before = fixture::textBefore( cfa, 37, 14 );
	REQUIRE( before.ends_with( "area( " ) );
	h = a->signatureHelp( cfa, { 37, 14 }, before );
	REQUIRE( h );
	CHECK( h->signatures[0].label == "double area( double r )" );
}

TEST_CASE( "signature help finds the argument index" ) {
	auto a = fixture::load( "shapes" );
	auto h = a->signatureHelp( cfa, { 37, 1 }, "int main() {\n\topen( sout, \"x,(\", " );
	REQUIRE( h );
	REQUIRE( h->signatures.size() == 2 );
	CHECK( h->activeParameter == 2 );
	CHECK( h->signatures[h->activeSignature].label == "void open( ofstream & os, const char * name, const char * mode )" );
	CHECK( h->signatures[0].params.size() == 3 );

	h = a->signatureHelp( cfa, { 37, 1 }, "\tsout | area( open( 1, 2 ), " );
	REQUIRE( h );
	CHECK( h->signatures[0].label.find( "area" ) != std::string::npos );
	CHECK( h->activeParameter == 1 );

	h = a->signatureHelp( cfa, { 37, 1 }, "\tarea( /* , ( */ x[1, 2], '(', " );
	REQUIRE( h );
	CHECK( h->activeParameter == 2 );

	h = a->signatureHelp( cfa, { 37, 1 }, "\tarea( b // , (\n\t\t, " );
	REQUIRE( h );
	CHECK( h->activeParameter == 1 );

	h = a->signatureHelp( cfa, { 37, 1 }, "\tsout | ?|?( sout, " );
	REQUIRE( h );
	CHECK( h->signatures.size() == 2 );
	CHECK( h->activeParameter == 1 );
	CHECK( h->signatures[0].params.size() == 2 );
}

TEST_CASE( "signature help outside calls" ) {
	auto a = fixture::load( "shapes" );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tx = 1;" ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tarea( b ) " ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tarea( (Box){ " ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tnothing( " ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tif ( " ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "\tarea( // " ) );
	CHECK_FALSE( a->signatureHelp( cfa, { 37, 1 }, "" ) );
	// Unknown file: still answers by name.
	CHECK( a->signatureHelp( "/elsewhere.cfa", { 0, 6 }, "area( " ) );
}

TEST_CASE( "semantic tokens" ) {
	auto a = fixture::load( "shapes" );
	auto toks = a->semanticTokens( cfa );
	REQUIRE_FALSE( toks.empty() );
	for ( size_t i = 0; i < toks.size(); i += 1 ) {
		CHECK( toks[i].length > 0 );
		if ( i == 0 ) continue;
		const auto & p = toks[i - 1], & t = toks[i];
		bool ordered = p.start < t.start;
		CHECK( ordered );
		if ( p.start.line == t.start.line ) CHECK( p.start.col + p.length <= t.start.col );
	}
	int decl = modBit( "declaration" ), ro = modBit( "readonly" ), lib = modBit( "defaultLibrary" );

	auto t = tokenAt( toks, 3, 4 );					// area definition
	REQUIRE( t );
	CHECK( t->type == typeIndex( "function" ) );
	CHECK( t->modifiers == decl );
	CHECK( t->length == 4 );

	t = tokenAt( toks, 3, 10 );						// Box
	REQUIRE( t );
	CHECK( t->type == typeIndex( "struct" ) );
	CHECK( t->modifiers == 0 );

	t = tokenAt( toks, 3, 16 );						// parameter b
	REQUIRE( t );
	CHECK( t->type == typeIndex( "parameter" ) );
	CHECK( t->modifiers == decl );

	t = tokenAt( toks, 4, 9 );						// hi through with
	REQUIRE( t );
	CHECK( t->type == typeIndex( "property" ) );

	t = tokenAt( toks, 12, 10 );					// coroutine Walker
	REQUIRE( t );
	CHECK( t->type == typeIndex( "class" ) );
	CHECK( t->modifiers == decl );

	t = tokenAt( toks, 24, 12 );					// trait Shape
	REQUIRE( t );
	CHECK( t->type == typeIndex( "interface" ) );

	t = tokenAt( toks, 24, 19 );					// T
	REQUIRE( t );
	CHECK( t->type == typeIndex( "typeParameter" ) );

	t = tokenAt( toks, 32, 11 );					// const int n
	REQUIRE( t );
	CHECK( t->type == typeIndex( "variable" ) );
	CHECK( t->modifiers == ( decl | ro ) );

	t = tokenAt( toks, 34, 14 );					// use of n
	REQUIRE( t );
	CHECK( t->modifiers == ro );

	t = tokenAt( toks, 35, 2 );						// sout
	REQUIRE( t );
	CHECK( t->type == typeIndex( "variable" ) );
	CHECK( t->modifiers == lib );

	toks = a->semanticTokens( hfa );
	t = tokenAt( toks, 23, 14 );					// Red
	REQUIRE( t );
	CHECK( t->type == typeIndex( "enumMember" ) );
	CHECK( t->modifiers == ( decl | ro ) );
	t = tokenAt( toks, 3, 7 );						// Point, not its generated constructor
	REQUIRE( t );
	CHECK( t->type == typeIndex( "struct" ) );

	toks = a->semanticTokens( fixture::libcfa );
	t = tokenAt( toks, 7, 18 );
	REQUIRE( t );
	CHECK( t->modifiers == ( decl | lib ) );

	CHECK( a->semanticTokens( "/nowhere.cfa" ).empty() );
}

TEST_CASE( "semantic tokens keep only the name inside a macro invocation" ) {
	// The ref spans `ExceptionDecl(TooLong)` as a macro expansion would map it.
	std::string src = "ExceptionDecl(TooLong);\nvoid f() { throw TooLong; }\n";
	auto a = fixture::loadText( R"J({"format": 1,
		"decls": [
			{"id": 1, "name": "TooLong", "kind": "exception", "file": "/m.cfa", "line": 1, "col": 0, "endLine": 1, "endCol": 22,
			 "nameRange": {"line": 1, "col": 0, "endLine": 1, "endCol": 22}},
			{"id": 2, "name": "Other", "kind": "struct", "file": "/m.cfa", "line": 2, "col": 0, "endLine": 2, "endCol": 4,
			 "nameRange": {"line": 2, "col": 0, "endLine": 2, "endCol": 4}}
		],
		"refs": [
			{"file": "/m.cfa", "line": 2, "col": 17, "endLine": 2, "endCol": 24, "decl": 1, "role": "read"}
		]})J",
		[&]( const std::string & p ) -> std::optional<std::string> { if ( p == "/m.cfa" ) return src; return std::nullopt; } );
	auto toks = a->semanticTokens( "/m.cfa" );
	REQUIRE( toks.size() == 2 );					// `Other` doesn't spell its name, so it's dropped
	CHECK( toks[0].start == Loc{ 0, 14 } );
	CHECK( toks[0].length == 7 );
	CHECK( toks[1].start == Loc{ 1, 17 } );
	CHECK( toks[1].type == typeIndex( "class" ) );
}

TEST_CASE( "legend" ) {
	CHECK( Analysis::tokenTypes().size() == 13 );
	CHECK( Analysis::tokenTypes().back() == "keyword" );
	CHECK( Analysis::tokenModifiers() == std::vector<std::string>{ "declaration", "readonly", "defaultLibrary" } );
}

TEST_CASE( "keyword tokens come from the text alone" ) {
	std::string src =
		"#include <fstream.hfa>\n"						// directives are left to the client
		"static void f( int x ) {\n"					// basic types too
		"\tfor () { suspend; }\n"
		"\ttry { resume( c ); } catch ( E * ) {}\n"	// resume is a function
		"\tenable_ehm(); // if while\n"				// so is enable_ehm; comments don't count
		"\treturn \"for\" ? x : 0;\n"
		"}\n";
	std::vector<std::pair<Loc, int>> got;
	for ( const SemanticToken & t : Analysis::keywordTokens( src ) ) {
		CHECK( t.type == typeIndex( "keyword" ) );
		CHECK( t.modifiers == 0 );
		got.push_back( { t.start, t.length } );
	}
	CHECK( got == std::vector<std::pair<Loc, int>>{
		{ { 1, 0 }, 6 },		// static
		{ { 2, 1 }, 3 },		// for
		{ { 2, 10 }, 7 },		// suspend
		{ { 3, 1 }, 3 },		// try
		{ { 3, 22 }, 5 },		// catch
		{ { 5, 1 }, 6 },		// return
	} );
}

TEST_CASE( "odd and missing data" ) {
	auto a = fixture::loadText( R"J({"format": 1, "complete": true,
		"decls": [
			{"id": 1, "name": "noname", "kind": "variable", "file": "/o.cfa", "line": 2, "col": 4, "endLine": 2, "endCol": 10},
			{"name": "noid", "kind": "variable", "file": "/o.cfa", "line": 3, "col": 0},
			{"id": 2, "name": "backwards", "kind": "function", "file": "/o.cfa", "line": 5, "col": 9, "endLine": 4, "endCol": 0,
			 "nameRange": {"line": 5, "col": 0, "endLine": 5, "endCol": 9}},
			{"id": 3, "name": "nowhere", "kind": "function"},
			{"id": 1, "name": "duplicate", "kind": "variable", "file": "/o.cfa", "line": 9, "col": 0},
			{"id": 4, "name": "weird", "kind": "mystery", "file": "/o.cfa", "line": 6, "col": 0, "endLine": 6, "endCol": 5,
			 "nameRange": {"line": 6, "col": 0, "endLine": 6, "endCol": 5}, "parent": 999, "typeDecl": "x"},
			"not an object"
		],
		"refs": [
			{"file": "/o.cfa", "line": 7, "col": 0, "endLine": 7, "endCol": 6, "decl": 1, "role": "read"},
			{"file": "/o.cfa", "line": 8, "col": 0, "endLine": 8, "endCol": 3, "decl": 12345, "role": "read"},
			{"file": "/o.cfa", "line": 0, "col": 0, "decl": 1},
			{"line": 7, "col": 0, "decl": 1}
		],
		"exprs": [ {"file": "/o.cfa", "line": 7, "col": 0, "endLine": 7, "endCol": 6} ],
		"scopes": [ {"file": "/o.cfa", "line": 1, "col": 0, "endLine": 20, "endCol": 0, "parent": 0, "decls": [1, 77]},
					{"file": "/o.cfa", "line": 1, "col": 0, "endLine": 20, "endCol": 0, "parent": 5} ]
	})J", nullptr );
	auto h = a->hover( "/o.cfa", { 6, 2 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "noname" ) );
	CHECK( a->definition( "/o.cfa", { 6, 2 } ).size() == 1 );
	CHECK( a->definition( "/o.cfa", { 6, 2 } )[0].range == rng( 1, 4, 1, 4 ) );	// no nameRange: the start
	CHECK( a->references( "/o.cfa", { 6, 2 }, true ).size() == 2 );

	h = a->hover( "/o.cfa", { 4, 3 } );					// end before start was clamped
	REQUIRE( h );
	CHECK( contains( h->markdown, "backwards" ) );
	CHECK_FALSE( a->hover( "/o.cfa", { 7, 1 } ) );		// ref to an unknown decl was dropped

	CHECK( a->documentSymbols( "/o.cfa" ).size() == 1 );	// "backwards"; "weird" has an unknown kind
	auto items = a->completion( "/o.cfa", { 10, 0 }, "" );
	CHECK( findItem( items, "nowhere" ) );
	CHECK_FALSE( findItem( items, "duplicate" ) );
	CHECK_NOTHROW( a->semanticTokens( "/o.cfa" ) );
	CHECK_NOTHROW( a->completion( "/o.cfa", { 10, 0 }, "noname." ) );
}

} // TEST_SUITE

TEST_SUITE( "analysis" ) {

// load() must convert translator columns (offsets into the preprocessed line)
// through the SourceMap.
TEST_CASE( "load maps preprocessed columns back to the source" ) {
	std::string src = "int    twice( int x ) {\n\treturn   x * 2;\n}\n";
	std::string pre = "# 1 \"/m/a.cfa\"\nint twice( int x ) {\n return x * 2;\n}\n";
	SourceMap::Reader reader = [&]( const std::string & p ) -> std::optional<std::string> {
		if ( p == "/m/a.cfa" ) return src;
		return std::nullopt;
	};
	SourceMap map( pre, reader );
	auto dump = nlohmann::json::parse( R"J({"format": 1, "complete": true,
		"decls": [
			{"id": 1, "name": "twice", "kind": "function", "file": "/m/a.cfa", "line": 1, "col": 0, "endLine": 3, "endCol": 1,
			 "nameRange": {"line": 1, "col": 4, "endLine": 1, "endCol": 9}, "type": "int (int)", "signature": "int twice( int x )",
			 "body": {"line": 1, "col": 19, "endLine": 3, "endCol": 1}},
			{"id": 2, "name": "x", "kind": "parameter", "file": "/m/a.cfa", "line": 1, "col": 11, "endLine": 1, "endCol": 16,
			 "nameRange": {"line": 1, "col": 15, "endLine": 1, "endCol": 16}, "type": "int", "parent": 1, "local": true}
		],
		"refs": [ {"file": "/m/a.cfa", "line": 2, "col": 8, "endLine": 2, "endCol": 9, "decl": 2, "role": "read"} ],
		"scopes": [ {"file": "/m/a.cfa", "line": 1, "col": 19, "endLine": 3, "endCol": 1, "parent": null, "decls": [2]} ]
	})J" );
	auto a = Analysis::load( dump, map, reader );
	auto d = a->definition( "/m/a.cfa", { 1, 10 } );		// the x in `return   x`
	REQUIRE( d.size() == 1 );
	CHECK( d[0].range == rng( 0, 18, 0, 19 ) );
	auto h = a->hover( "/m/a.cfa", { 0, 9 } );
	REQUIRE( h );
	CHECK( h->range == rng( 0, 7, 0, 12 ) );
	auto toks = a->semanticTokens( "/m/a.cfa" );
	REQUIRE( toks.size() == 3 );
	CHECK( toks[2].start == Loc{ 1, 10 } );
}

TEST_CASE( "queries stay fast on large dumps" ) {
	// 30000 library functions in 3000 overload sets, and a focus file with
	// 20000 refs, about what libcfa and the prelude contribute.
	nlohmann::json decls = nlohmann::json::array(), refs = nlohmann::json::array();
	const char * lib = "/opt/cfa/include/cfa/big.hfa";
	for ( int i = 0; i < 30000; i += 1 ) {
		std::string name = "fn" + std::to_string( i % 3000 );
		decls.push_back( { { "id", i + 1 }, { "name", name }, { "kind", "function" }, { "file", lib },
			{ "line", i + 1 }, { "col", 0 }, { "endLine", i + 1 }, { "endCol", 30 },
			{ "nameRange", { { "line", i + 1 }, { "col", 4 }, { "endLine", i + 1 }, { "endCol", 4 + int( name.size() ) } } },
			{ "type", "int (int)" }, { "signature", "int " + name + "( int x" + std::to_string( i ) + " )" } } );
	}
	for ( int i = 0; i < 20000; i += 1 ) {
		refs.push_back( { { "file", "/big/main.cfa" }, { "line", i / 10 + 1 }, { "col", ( i % 10 ) * 8 }, { "endLine", i / 10 + 1 },
			{ "endCol", ( i % 10 ) * 8 + 5 }, { "decl", i % 30000 + 1 }, { "role", "call" } } );
	}
	nlohmann::json dump = { { "format", 1 }, { "complete", true }, { "decls", decls }, { "refs", refs } };

	auto t0 = std::chrono::steady_clock::now();
	auto a = Analysis::load( dump, SourceMap::identity(), nullptr );
	auto t1 = std::chrono::steady_clock::now();
	int found = 0;
	for ( int i = 0; i < 2000; i += 1 ) {
		if ( a->hover( "/big/main.cfa", { i % 2000, 2 } ) ) found += 1;
		found += int( a->definition( "/big/main.cfa", { i % 2000, 10 } ).size() );
	}
	auto t2 = std::chrono::steady_clock::now();
	size_t items = 0;
	for ( int i = 0; i < 200; i += 1 ) items += a->completion( "/big/main.cfa", { 0, 0 }, "\tfn12" ).size();
	auto t3 = std::chrono::steady_clock::now();
	auto ms = []( auto d ) { return std::chrono::duration_cast<std::chrono::milliseconds>( d ).count(); };
	MESSAGE( "load " << ms( t1 - t0 ) << " ms, 4000 hover/definition " << ms( t2 - t1 ) << " ms, 200 completions "
			 << ms( t3 - t2 ) << " ms" );
	CHECK( found == 4000 );
	CHECK( items == 200 * 111 );						// fn12, fn120..fn129, fn1200..fn1299
	CHECK( ms( t2 - t1 ) < 2000 );
	CHECK( ms( t3 - t2 ) < 2000 );
}

TEST_CASE( "similar names for an undeclared identifier" ) {
	auto a = fixture::load( "shapes" );
	Loc inBlock{ 35, 8 };								// sout | inner | ...
	auto first = [&]( const std::string & name, Loc pos ) {
		auto v = a->similarNames( cfa, pos, name );
		return v.empty() ? std::string() : v[0];
	};
	CHECK( first( "innr", inBlock ) == "inner" );			// a dropped letter
	CHECK( first( "Inner", inBlock ) == "inner" );
	CHECK( first( "aera", inBlock ) == "area" );			// swapped letters
	CHECK( first( "Wlaker", inBlock ) == "Walker" );
	CHECK( first( "sotu", inBlock ) == "sout" );			// a library global
	CHECK( a->similarNames( cfa, inBlock, "aera", 1 ).size() == 1 );
	// One letter is not a typo of another, and far names don't count.
	CHECK( a->similarNames( cfa, inBlock, "x" ).empty() );
	CHECK( a->similarNames( cfa, inBlock, "zzzzzz" ).empty() );
	// Reserved library names only when the typo has the underscores too.
	CHECK( a->similarNames( cfa, inBlock, "cfa_flags" ).empty() );
	CHECK( first( "__cfa_flag", inBlock ) == "__cfa_flags" );
	// Locals of another function are out of scope.
	auto inArea = a->similarNames( cfa, { 4, 4 }, "innr" );
	CHECK( std::find( inArea.begin(), inArea.end(), "inner" ) == inArea.end() );
}

} // TEST_SUITE
