// Analysis features that read the source rather than the dump: macros,
// #include lines, uses inside macro expansions and libcfa's internal names.
#include <doctest/doctest.h>

#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"

using namespace cfalsp;
using json = nlohmann::json;

namespace {

const std::map<std::string, std::string> sources = {
	{ "/m/macros.h", "#define TWICE( v ) ((v) * two)\n#define  MAGIC \\\n\t42\n" },
	{ "/m/main.cfa", "#include \"macros.h\"\nint two = 2;\nint x = TWICE(4) + MAGIC;\n" },
	{ "/opt/cfa/include/cfa/lib.hfa", "int threads;\nint thread$;\n" },
};

// What `cfa -E` makes of main.cfa.
const char * preprocessed =
	"# 1 \"/m/main.cfa\"\n"
	"# 1 \"/m/macros.h\" 1\n"
	"# 2 \"/m/main.cfa\" 2\n"
	"int two = 2;\n"
	"int x = ((4) * two) + 42;\n"
	"# 1 \"/opt/cfa/include/cfa/lib.hfa\" 1\n"
	"int threads;\n"
	"int thread$;\n";

std::optional<std::string> reader( const std::string & path ) {
	auto it = sources.find( path );
	if ( it == sources.end() ) return std::nullopt;
	return it->second;
}

json decl( int id, const std::string & name, const std::string & file, int line, int col ) {
	json r = { { "line", line }, { "col", col }, { "endLine", line }, { "endCol", col + int( name.size() ) } };
	json d = { { "id", id }, { "name", name }, { "kind", "variable" }, { "file", file }, { "nameRange", r },
			   { "type", "int" }, { "signature", "int " + name }, { "parent", nullptr }, { "typeDecl", nullptr },
			   { "generated", false }, { "local", false } };
	for ( auto & [k, v] : r.items() ) d[k] = v;
	return d;
}

std::shared_ptr<const Analysis> load() {
	json dump = { { "format", 1 }, { "complete", true }, { "diagnostics", json::array() },
				  { "decls", { decl( 1, "two", "/m/main.cfa", 2, 4 ), decl( 2, "x", "/m/main.cfa", 3, 4 ),
							   decl( 3, "threads", "/opt/cfa/include/cfa/lib.hfa", 1, 4 ),
							   decl( 4, "thread$", "/opt/cfa/include/cfa/lib.hfa", 2, 4 ) } },
				  // `two` from TWICE's body: the source there spells TWICE(4), not two.
				  { "refs", { { { "file", "/m/main.cfa" }, { "line", 3 }, { "col", 15 }, { "endLine", 3 }, { "endCol", 18 },
								{ "decl", 1 }, { "role", "read" } } } },
				  { "exprs", json::array() }, { "scopes", json::array() } };
	SourceMap map( preprocessed, reader );
	return Analysis::load( dump, map, reader );
}

} // namespace

TEST_SUITE( "analysis" ) {

TEST_CASE( "macros: hover and definition read #define lines, even from headers with no declarations" ) {
	auto a = load();
	auto h = a->hover( "/m/main.cfa", { 2, 9 } );
	REQUIRE( h );
	CHECK( h->markdown.find( "#define TWICE( v ) ((v) * two)" ) != std::string::npos );
	CHECK( h->markdown.find( "macro" ) != std::string::npos );
	CHECK( h->markdown.find( "macros.h:1" ) != std::string::npos );
	CHECK( h->range == Range{ { 2, 8 }, { 2, 13 } } );
	auto d = a->definition( "/m/main.cfa", { 2, 9 } );
	REQUIRE( d.size() == 1 );
	CHECK( d[0].file == "/m/macros.h" );
	CHECK( d[0].range.start == Loc{ 0, 8 } );

	// A continued definition is shown whole.
	h = a->hover( "/m/main.cfa", { 2, 20 } );
	REQUIRE( h );
	CHECK( h->markdown.find( "#define  MAGIC \\\n\t42" ) != std::string::npos );
}

TEST_CASE( "macros: cpp's output says which branch of an #if it kept" ) {
	// FROM_CMDLINE comes from a -D flag, which the source doesn't show.
	std::map<std::string, std::string> src = {
		{ "/e/conf.h", "#define CONF 1\n" },
		{ "/e/main.cfa", "#include \"conf.h\"\n"		// 0
						 "#ifndef FROM_CMDLINE\n"
						 "int dead_code;\n"
						 "#define CHOSEN 2\n"
						 "#else\n"
						 "int live_code;\n"			// 5
						 "#define CHOSEN 1\n"
						 "#endif\n"
						 "#ifdef OUTER\n"
						 "#ifdef INNER\n"
						 "int inner_code;\n"			// 10
						 "#endif\n"
						 "#define IN_OUTER 1\n"		// no code of its own: nothing to see
						 "#endif\n"
						 "int x = CHOSEN + IN_OUTER;\n" },
	};
	const char * pp =
		"# 1 \"/e/main.cfa\"\n"
		"# 1 \"/e/conf.h\" 1\n"
		"# 2 \"/e/main.cfa\" 2\n"
		"# 6 \"/e/main.cfa\"\n"
		"int live_code;\n"
		"# 15 \"/e/main.cfa\"\n"
		"int x = 1 + 1;\n";
	auto read = [src]( const std::string & path ) -> std::optional<std::string> {
		auto it = src.find( path );
		if ( it == src.end() ) return std::nullopt;
		return it->second;
	};
	json dump = { { "format", 1 }, { "complete", true }, { "diagnostics", json::array() },
				  { "decls", { decl( 1, "x", "/e/main.cfa", 15, 4 ) } },
				  { "refs", json::array() }, { "exprs", json::array() }, { "scopes", json::array() } };
	SourceMap map( pp, read );
	auto a = Analysis::load( dump, map, read );
	auto h = a->hover( "/e/main.cfa", { 14, 9 } );
	REQUIRE( h );
	CHECK( h->markdown.find( "#define CHOSEN 1" ) != std::string::npos );
	h = a->hover( "/e/main.cfa", { 14, 18 } );
	REQUIRE( h );
	CHECK( h->markdown.find( "#define IN_OUTER 1" ) != std::string::npos );
}

TEST_CASE( "a use that only the macro body spells is not a ref" ) {
	auto a = load();
	CHECK( a->references( "/m/main.cfa", { 1, 4 }, false ).empty() );
	for ( const auto & t : a->semanticTokens( "/m/main.cfa" ) ) CHECK( ( t.start.line != 2 || t.start.col < 8 ) );
}

TEST_CASE( "definition on an #include line opens the file" ) {
	auto a = load();
	auto d = a->definition( "/m/main.cfa", { 0, 12 } );
	REQUIRE( d.size() == 1 );
	CHECK( d[0].file == "/m/macros.h" );
	CHECK( d[0].range.start == Loc{ 0, 0 } );
}

TEST_CASE( "completion hides libcfa's internal $ names unless asked" ) {
	auto a = load();
	std::set<std::string> got;
	for ( const auto & c : a->completion( "/m/main.cfa", { 2, 0 }, "thr" ) ) got.insert( c.label );
	CHECK( got.count( "threads" ) );
	CHECK_FALSE( got.count( "thread$" ) );
	got.clear();
	for ( const auto & c : a->completion( "/m/main.cfa", { 2, 0 }, "thread$" ) ) got.insert( c.label );
	CHECK( got.count( "thread$" ) );
}

} // TEST_SUITE
