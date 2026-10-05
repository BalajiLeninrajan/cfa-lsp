#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Headers.hpp"

using namespace cfalsp;

namespace {

std::string libcfa() {
	const char * env = std::getenv( "CFA_LSP_FIXTURES" );
	return std::string( env && *env ? env : "tests/fixtures" ) + "/libcfa";
}

std::string slurp( const std::string & p ) {
	std::ifstream in( p, std::ios::binary );
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

using V = std::vector<std::string>;

} // namespace

TEST_SUITE( "analysis" ) {

TEST_CASE( "headers: the names a header declares at file scope" ) {
	std::map<std::string, bool> got;		// name -> is a type
	for ( const HeaderName & n : fileScopeNames( slurp( libcfa() + "/fstream.hfa" ) ) ) got[n.name] = got[n.name] || n.type;
	std::map<std::string, bool> want = {
		{ "ofstream", true }, { "persist", false }, { "fresh", false }, { "fileno_of", false }, { "sout", false },
		{ "serr", false }, { "verbosity", false }, { "quiet", false }, { "handle_t", true }, { "fsize", true },
		{ "FMode", true }, { "ReadMode", false }, { "WriteMode", false }, { "AppendMode", false }, { "Sep", true },
		{ "Space", false }, { "Tab", false }, { "open", false }, { "NL", false }, { "FS_MAX", false },
	};
	CHECK( got == want );
}

TEST_CASE( "headers: the index names headers the way they are included" ) {
	HeaderIndex idx = HeaderIndex::scan( libcfa() );
	REQUIRE( !idx.empty() );
	CHECK( idx.headersFor( "sout" ) == V{ "fstream.hfa" } );
	CHECK( idx.headersFor( "sout", true ) == V{} );
	CHECK( idx.headersFor( "ofstream", true ) == V{ "fstream.hfa" } );
	// From the concurrency/ and collections/ subdirectories, by file name.
	CHECK( idx.headersFor( "join" ) == V{ "thread.hfa" } );
	CHECK( idx.headersFor( "worker_base", true ) == V{ "thread.hfa" } );
	CHECK( idx.headersFor( "string", true ) == V{ "string.hfa" } );
	CHECK( idx.headersFor( "size" ) == V{ "string.hfa" } );
	// collections/fstream.hfa is hidden by fstream.hfa; bits/ isn't searched.
	CHECK( idx.headersFor( "shadowed" ) == V{} );
	CHECK( idx.headersFor( "internal_only" ) == V{} );
	CHECK( idx.headersFor( "nosuch" ) == V{} );

	CHECK( HeaderIndex::scan( "/nonexistent/include/cfa" ).empty() );
	CHECK( HeaderIndex::scan( "" ).empty() );

	HeaderIndex two;
	two.add( "b.hfa", "int f;" );
	two.add( "a.hfa", "void f( void ); typedef int g;" );
	two.add( "a.hfa", "struct g { int x; };" );
	CHECK( two.headersFor( "f" ) == V{ "a.hfa", "b.hfa" } );
	CHECK( two.headersFor( "g", true ) == V{ "a.hfa" } );
}

} // TEST_SUITE
