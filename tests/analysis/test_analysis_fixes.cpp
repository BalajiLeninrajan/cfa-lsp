// Analysis behaviour on small hand-built dumps: fallbacks that must not fire
// (comments, dead macros), diagnostics, member completion on text the
// translator hasn't seen, operators, and queries across analyses.
#include <doctest/doctest.h>

#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"

using namespace cfalsp;
using json = nlohmann::json;

namespace {

// Builds a dump whose columns are already original columns (SourceMap::identity()).
struct Dump {
	json decls = json::array(), refs = json::array(), exprs = json::array(), scopes = json::array(),
		 diagnostics = json::array();

	static json range( int line, int col, int endLine, int endCol ) {
		return { { "line", line }, { "col", col }, { "endLine", endLine }, { "endCol", endCol } };
	}

	// `line` is 1-based. Returns the id.
	int decl( const std::string & name, const std::string & kind, const std::string & file, int line, int col,
			  json extra = json::object() ) {
		int id = int( decls.size() );
		json d = { { "id", id }, { "name", name }, { "kind", kind }, { "file", file },
				   { "nameRange", range( line, col, line, col + int( name.size() ) ) }, { "type", "" },
				   { "signature", "" }, { "parent", nullptr }, { "typeDecl", nullptr }, { "body", nullptr },
				   { "generated", false }, { "local", false } };
		json whole = range( line, col, line, col + int( name.size() ) );
		for ( auto & [k, v] : whole.items() ) d[k] = v;
		for ( auto & [k, v] : extra.items() ) d[k] = v;
		decls.push_back( d );
		return id;
	}

	void ref( const std::string & file, int line, int col, int len, int decl, const char * role ) {
		json r = range( line, col, line, col + len );
		r["file"] = file;
		r["decl"] = decl;
		r["role"] = role;
		refs.push_back( r );
	}

	void expr( const std::string & file, int line, int col, int len, const std::string & type, json typeDecl = nullptr ) {
		json e = range( line, col, line, col + len );
		e["file"] = file;
		e["type"] = type;
		e["typeDecl"] = typeDecl;
		exprs.push_back( e );
	}

	std::shared_ptr<const Analysis> load( const std::map<std::string, std::string> & sources ) const {
		json dump = { { "format", 1 }, { "complete", true }, { "diagnostics", diagnostics }, { "decls", decls },
					  { "refs", refs }, { "exprs", exprs }, { "scopes", scopes } };
		auto reader = [sources]( const std::string & path ) -> std::optional<std::string> {
			auto it = sources.find( path );
			if ( it == sources.end() ) return std::nullopt;
			return it->second;
		};
		return Analysis::load( dump, SourceMap::identity(), reader );
	}
};

std::set<std::string> labels( const std::vector<CompletionItem> & items ) {
	std::set<std::string> out;
	for ( const auto & i : items ) out.insert( i.label );
	return out;
}

bool contains( const std::string & s, const std::string & part ) { return s.find( part ) != std::string::npos; }

const char * lib = "/opt/cfa/include/cfa/lib.hfa";

} // namespace

TEST_SUITE( "analysis" ) {

TEST_CASE( "fallbacks: no answer for a word in a comment or a string" ) {
	std::map<std::string, std::string> src = {
		{ "/c/main.cfa", "int resumeAt;\n"
						 "int x = 1; // resumeAt here\n"
						 "/* resumeAt\n   resumeAt */\n"
						 "const char * s = \"resumeAt\";\n"
						 "int y = resumeAt;\n" },
	};
	Dump d;
	d.decl( "resumeAt", "variable", "/c/main.cfa", 1, 4, { { "type", "int" }, { "signature", "int resumeAt" } } );
	auto a = d.load( src );
	CHECK_FALSE( a->hover( "/c/main.cfa", { 1, 15 } ) );
	CHECK( a->definition( "/c/main.cfa", { 1, 15 } ).empty() );
	CHECK_FALSE( a->hover( "/c/main.cfa", { 3, 5 } ) );			// second line of a block comment
	CHECK_FALSE( a->hover( "/c/main.cfa", { 4, 20 } ) );
	// Outside comments the name fallback still answers.
	auto h = a->hover( "/c/main.cfa", { 5, 9 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "int resumeAt" ) );
}

TEST_CASE( "macros: only definitions cpp kept and that are not #undef'd" ) {
	std::map<std::string, std::string> src = {
		{ "/m/coll.h", "#ifdef __cforall\n"
					   "forall( T & )\n"
					   "#else\n"
					   "#define T void\n"
					   "#endif\n"
					   "struct small;\n"
					   "#undef T\n"
					   "#define LIVE 1\n"
					   "#ifndef __cforall\n"
					   "#define DEAD 2\n"
					   "#endif\n"
					   "#define GONE 3\n"
					   "#undef GONE\n" },
		{ "/m/main.cfa", "#include \"coll.h\"\n"
						 "int a = LIVE + T + DEAD + GONE;\n"
						 "#define LOCAL 4\n"
						 "int b = LOCAL;\n"
						 "#undef LOCAL\n"
						 "int c = LOCAL;\n" },
	};
	Dump d;
	d.decl( "a", "variable", "/m/main.cfa", 2, 4 );
	d.decl( "small", "struct", "/m/coll.h", 6, 7 );
	auto a = d.load( src );
	auto h = a->hover( "/m/main.cfa", { 1, 9 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "#define LIVE 1" ) );
	CHECK_FALSE( a->hover( "/m/main.cfa", { 1, 15 } ) );		// T: in the #else of #ifdef __cforall, and #undef'd
	CHECK_FALSE( a->hover( "/m/main.cfa", { 1, 19 } ) );		// DEAD
	CHECK_FALSE( a->hover( "/m/main.cfa", { 1, 26 } ) );		// GONE
	CHECK( a->hover( "/m/main.cfa", { 3, 9 } ) );				// LOCAL while defined
	CHECK_FALSE( a->hover( "/m/main.cfa", { 5, 9 } ) );		// and after its #undef
}

TEST_CASE( "type parameters answer for their name inside the declaration, assertions included" ) {
	std::map<std::string, std::string> src = {
		{ "/t/main.cfa", "#define T void\n"
						 "forall( T | { int ?<?( T, T ); } )\n"
						 "T biggest( T a, T b ) { return a; }\n" },
	};
	Dump d;
	int fn = d.decl( "biggest", "function", "/t/main.cfa", 3, 2,
					 { { "signature", "forall( T ) T biggest( T a, T b )" }, { "body", Dump::range( 3, 22, 3, 35 ) } } );
	d.decls[fn]["line"] = 2;
	d.decls[fn]["col"] = 0;
	d.decls[fn]["endLine"] = 3;
	d.decls[fn]["endCol"] = 35;
	d.decl( "T", "typeParam", "/t/main.cfa", 2, 8, { { "parent", fn }, { "local", true }, { "signature", "T" } } );
	auto a = d.load( src );
	auto h = a->hover( "/t/main.cfa", { 1, 23 } );			// T in ?<?( T, T )
	REQUIRE( h );
	CHECK( contains( h->markdown, "type parameter" ) );
	CHECK_FALSE( contains( h->markdown, "#define" ) );
	auto def = a->definition( "/t/main.cfa", { 1, 23 } );
	REQUIRE( def.size() == 1 );
	CHECK( def[0].range.start == Loc{ 1, 8 } );
}

TEST_CASE( "diagnostics: the first line names the problem and the resolver dump is condensed" ) {
	Dump d;
	auto diag = [&]( const std::string & message, const std::string & detail ) {
		json j = Dump::range( 1, 0, 1, 1 );
		j["file"] = "/d/main.cfa";
		j["severity"] = "error";
		j["message"] = message;
		if ( ! detail.empty() ) j["detail"] = detail;
		d.diagnostics.push_back( j );
	};
	diag( "No alternatives for expression Name: nosuch", "" );
	diag( "No alternatives for expression Untyped Member Expression, with field: ",
		  "  Name: third... from aggregate:\n  Name: pi" );
	diag( "Invalid application of existing declaration(s) in expression Applying untyped:",
		  "  Name: area\n...to:\n"
		  "  Constant Expression (\"rect\": array of char with dimension of Constant Expression (5: unsigned long int)\n"
		  "  ... with resolved type:\n    unsigned long int)\n"
		  "  ... with resolved type:\n    array of char with dimension of Constant Expression (5: unsigned long int)\n"
		  "    ... with resolved type:\n      unsigned long int\n"
		  "  Constant Expression (3: signed int)\n  ... with resolved type:\n    signed int" );
	diag( "No alternatives for expression area( b, 1 )", "Alternatives are:\n  int area( Box & b )\n  double area( double r )" );
	auto ds = d.load( {} )->diagnostics();
	REQUIRE( ds.size() == 4 );
	CHECK( ds[0].message == "use of undeclared identifier `nosuch`" );
	CHECK( ds[1].message == "no field `third` in `pi`" );
	CHECK( ds[2].message == "no overload of `area` accepts the arguments (char [5], int)" );
	CHECK( ds[3].message == "No alternatives for expression area( b, 1 )\n\nAlternatives are:\nint area( Box & b )\ndouble area( double r )" );
}

TEST_CASE( "member completion: declarations typed since the snapshot, (*p) and pointer levels" ) {
	std::map<std::string, std::string> src = {
		{ "/p/main.cfa", "struct Point { double x, y; };\n"
						 "struct Circle { Point centre; double radius; };\n"
						 "typedef struct { int lo, hi; } Span;\n"
						 "int main() {\n"
						 "Circle wheel; Circle * pc; Circle ** ppc;\n"
						 "}\n" },
	};
	Dump d;
	const std::string f = "/p/main.cfa";
	int point = d.decl( "Point", "struct", f, 1, 7 );
	d.decl( "x", "field", f, 1, 22, { { "parent", point }, { "type", "double" } } );
	d.decl( "y", "field", f, 1, 25, { { "parent", point }, { "type", "double" } } );
	int circle = d.decl( "Circle", "struct", f, 2, 7 );
	d.decl( "centre", "field", f, 2, 22, { { "parent", circle }, { "type", "Point" }, { "typeDecl", point } } );
	d.decl( "radius", "field", f, 2, 37, { { "parent", circle }, { "type", "double" } } );
	int span = d.decl( "__anonymous_Span", "struct", f, 3, 8, { { "generated", true } } );
	d.decl( "lo", "field", f, 3, 21, { { "parent", span }, { "type", "int" } } );
	d.decl( "hi", "field", f, 3, 25, { { "parent", span }, { "type", "int" } } );
	d.decl( "Span", "typedef", f, 3, 31, { { "typeDecl", span } } );
	int main = d.decl( "main", "function", f, 4, 4, { { "body", Dump::range( 4, 11, 6, 1 ) } } );
	d.decl( "wheel", "variable", f, 5, 7, { { "type", "Circle" }, { "typeDecl", circle }, { "local", true }, { "parent", main } } );
	d.decl( "pc", "variable", f, 5, 23, { { "type", "Circle *" }, { "typeDecl", circle }, { "local", true }, { "parent", main } } );
	d.decl( "ppc", "variable", f, 5, 37, { { "type", "Circle **" }, { "typeDecl", circle }, { "local", true }, { "parent", main } } );
	auto a = d.load( src );
	Loc pos{ 5, 0 };									// the closing brace: new lines go before it
	std::string before = src[f].substr( 0, src[f].rfind( '}' ) );
	auto complete = [&]( const std::string & typed ) {
		std::string text = before + typed;
		std::string line = text.substr( text.rfind( '\n' ) + 1 );
		return labels( a->completion( f, pos, line, text ) );
	};
	using S = std::set<std::string>;
	CHECK( complete( "Circle & cr = wheel;\ncr." ) == S{ "centre", "radius" } );
	CHECK( complete( "Circle & cr = wheel;\ncr.centre." ) == S{ "x", "y" } );
	CHECK( complete( "Span sp;\nsp." ) == S{ "lo", "hi" } );
	CHECK( complete( "Circle * np;\nnp->" ) == S{ "centre", "radius" } );
	CHECK( complete( "(*pc)." ) == S{ "centre", "radius" } );
	CHECK( complete( "(*ppc)->" ) == S{ "centre", "radius" } );
	CHECK( complete( "pc->" ) == S{ "centre", "radius" } );
	CHECK( complete( "ppc->" ).empty() );				// Circle **: -> needs one level
	CHECK( complete( "pc." ).empty() );
	CHECK( complete( "wheel." ) == S{ "centre", "radius" } );
}

TEST_CASE( "completion and signature help: macros, and no libcfa internal overloads" ) {
	std::map<std::string, std::string> src = {
		{ "/s/main.cfa", "#define SQUARE( x ) ((x) * (x))\n"
						 "#define SIZE 10\n"
						 "int main() {\n"
						 "}\n" },
		{ lib, "#define _INTERNAL 1\n#define LIBMAC 2\nvoid resume( coroutine$ * dst );\nint resume( int & c );\n" },
	};
	Dump d;
	d.decl( "main", "function", "/s/main.cfa", 3, 4, { { "body", Dump::range( 3, 11, 4, 1 ) } } );
	d.decl( "resume", "function", lib, 3, 5, { { "signature", "void resume( coroutine$ *dst )" } } );
	d.decl( "resume", "function", lib, 4, 4, { { "signature", "int resume( int &c )" }, { "type", "int (int &)" } } );
	auto a = d.load( src );
	auto items = labels( a->completion( "/s/main.cfa", { 3, 0 }, "SQ" ) );
	CHECK( items.count( "SQUARE" ) );
	items = labels( a->completion( "/s/main.cfa", { 3, 0 }, "LIB" ) );
	CHECK( items.count( "LIBMAC" ) );
	CHECK_FALSE( labels( a->completion( "/s/main.cfa", { 3, 0 }, "IN" ) ).count( "_INTERNAL" ) );

	auto sh = a->signatureHelp( "/s/main.cfa", { 3, 0 }, src["/s/main.cfa"].substr( 0, 70 ) + "int z = SQUARE( 3" );
	REQUIRE( sh );
	REQUIRE( sh->signatures.size() == 1 );
	CHECK( sh->signatures[0].label == "SQUARE( x )" );
	CHECK( sh->signatures[0].params.size() == 1 );

	sh = a->signatureHelp( "/s/main.cfa", { 3, 0 }, "int main() {\nresume( " );
	REQUIRE( sh );
	REQUIRE( sh->signatures.size() == 1 );
	CHECK( sh->signatures[0].label == "int resume( int &c )" );
}

TEST_CASE( "operators, postfix calls and the outline" ) {
	std::map<std::string, std::string> src = {
		{ "/o/main.cfa", "struct Vec { int x; };\n"
						 "Vec ?+?( Vec a, Vec b );\n"
						 "int ?`len( Vec v );\n"
						 "Vec s = v + w; int n = v`len;\n"
						 "MAKE_VT( Vec );\n" },
	};
	Dump d;
	const std::string f = "/o/main.cfa";
	int vec = d.decl( "Vec", "struct", f, 1, 7 );
	d.decl( "x", "field", f, 1, 17, { { "parent", vec } } );
	int plus = d.decl( "?+?", "function", f, 2, 4, { { "signature", "Vec ?+?( Vec a, Vec b )" } } );
	int len = d.decl( "?`len", "function", f, 3, 4, { { "signature", "int ?`len( Vec v )" } } );
	d.decl( "Vec_vt", "variable", f, 5, 0 );				// made up by a macro; the source says MAKE_VT
	d.ref( f, 4, 10, 1, plus, "call" );
	d.ref( f, 4, 25, 3, len, "call" );
	auto a = d.load( src );
	auto h = a->hover( f, { 3, 10 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "Vec ?+?( Vec a, Vec b )" ) );
	CHECK( h->range == Range{ { 3, 10 }, { 3, 11 } } );
	auto refs = a->references( f, { 1, 5 }, false );
	REQUIRE( refs.size() == 1 );
	CHECK( refs[0].range.start == Loc{ 3, 10 } );
	h = a->hover( f, { 3, 26 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "int ?`len( Vec v )" ) );
	// Operators are not coloured as functions; postfix names are.
	bool plusToken = false, lenToken = false;
	for ( const auto & t : a->semanticTokens( f ) ) {
		if ( t.start == Loc{ 3, 10 } ) plusToken = true;
		if ( t.start == Loc{ 3, 25 } ) lenToken = true;
	}
	CHECK_FALSE( plusToken );
	CHECK( lenToken );
	std::set<std::string> outline;
	for ( const auto & s : a->documentSymbols( f ) ) outline.insert( s.name );
	CHECK( outline.count( "Vec" ) );
	CHECK_FALSE( outline.count( "Vec_vt" ) );
}

TEST_CASE( "a field of a generic aggregate shows its type in the instance" ) {
	std::map<std::string, std::string> src = {
		{ "/g/main.cfa", "forall( T ) struct Pair { T first; };\nint z = pi.first;\n" },
	};
	Dump d;
	const std::string f = "/g/main.cfa";
	int pair = d.decl( "Pair", "struct", f, 1, 19 );
	int first = d.decl( "first", "field", f, 1, 28, { { "parent", pair }, { "type", "T" }, { "signature", "T first" } } );
	d.ref( f, 2, 11, 5, first, "member" );
	d.expr( f, 2, 11, 5, "int" );
	auto h = d.load( src )->hover( f, { 1, 12 } );
	REQUIRE( h );
	CHECK( contains( h->markdown, "T first" ) );
	CHECK( contains( h->markdown, "Type here: `int`" ) );
}

TEST_CASE( "queries across analyses: references and the body of a prototype" ) {
	std::map<std::string, std::string> src = {
		{ "/x/geo.hfa", "double area( int r );\n" },
		{ "/x/geo.cfa", "#include \"geo.hfa\"\ndouble area( int r ) { return r; }\ndouble a = area( 1 );\n" },
	};
	// The header's own analysis: the prototype and nothing else.
	Dump h;
	h.decl( "area", "function", "/x/geo.hfa", 1, 7, { { "type", "double (int)" } } );
	auto ha = h.load( src );
	// The .cfa's: the prototype, the definition and a call.
	Dump c;
	int proto = c.decl( "area", "function", "/x/geo.hfa", 1, 7, { { "type", "double (int)" } } );
	c.decl( "area", "function", "/x/geo.cfa", 2, 7, { { "type", "double (int)" }, { "body", Dump::range( 2, 21, 2, 34 ) } } );
	c.ref( "/x/geo.cfa", 3, 11, 4, proto, "call" );
	auto ca = c.load( src );

	auto decls = ha->declarationsAt( "/x/geo.hfa", { 0, 8 } );
	REQUIRE( decls.size() == 1 );
	CHECK_FALSE( ha->definitionOf( decls ) );
	auto def = ca->definitionOf( decls );
	REQUIRE( def );
	CHECK( def->file == "/x/geo.cfa" );
	CHECK( def->range.start == Loc{ 1, 7 } );
	auto refs = ca->referencesTo( decls, false );
	REQUIRE( refs.size() == 1 );
	CHECK( refs[0].file == "/x/geo.cfa" );
	CHECK( refs[0].range.start == Loc{ 2, 11 } );
	CHECK( ca->referencesTo( decls, true ).size() == 3 );
}

} // TEST_SUITE
