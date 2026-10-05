#include <doctest/doctest.h>

#include "server/Checker.hpp"
#include "server/CompilerOutput.hpp"

using namespace cfalsp;

TEST_CASE( "demangling CFA names" ) {
	CHECK( demangle( "_X1ui_2" ) == "u" );
	CHECK( demangle( "_X6unusedi_2" ) == "unused" );
	CHECK( demangle( "_X4mainFi___1" ) == "main" );
	CHECK( demangle( "plain" ) == "plain" );
	CHECK( demangle( "_X99a" ) == "_X99a" );
	CHECK( demangleMessage( "unused variable '_X6unusedi_2'" ) == "unused variable 'unused'" );
	CHECK( demangleMessage( "unused variable \xE2\x80\x98_X1yi_2\xE2\x80\x99 here" ) == "unused variable 'y' here" );
	CHECK( demangleMessage( "no names here" ) == "no names here" );
}

TEST_CASE( "parsing compiler output" ) {
	std::string out =
		"CFA Version 1.0.0 (debug)\n"
		"/p/a.cfa: In function '_X1fFi_i__1':\n"
		"/p/a.cfa:4:41: warning: division by zero [-Wdiv-by-zero]\n"
		"/p/a.cfa:3:23: warning: unused variable '_X6unusedi_2' [-Wunused-variable]\n"
		"/p/a.cfa:7: error: no column here\n"
		"/p/a.cfa:9:1 error: No alternatives for expression Name: sepDisable\n"
		"  Name: sepDisable\n"
		"  with junk that doesn't matter\n"
		"  Alternatives are:\n"
		"In file included from /p/a.cfa:1:\n"
		"/p/b.hfa:2:10: fatal error: c.hfa: No such file or directory\n"
		"compilation terminated.\n"
		"cc1: fatal error: stdbool.h: No such file or directory\n"
		"/p/a.cfa:4:41: note: something helpful\n";
	auto d = parseCompilerOutput( out );
	REQUIRE( d.size() == 7 );
	CHECK( d[0].file == "/p/a.cfa" );
	CHECK( d[0].line == 4 );
	CHECK( d[0].col == 41 );
	CHECK( d[0].severity == "warning" );
	CHECK( d[0].message == "division by zero" );
	CHECK( d[0].option == "-Wdiv-by-zero" );
	CHECK( d[1].message == "unused variable 'unused'" );
	CHECK( d[2].line == 7 );
	CHECK( d[2].col == 0 );
	CHECK( d[3].severity == "error" );
	CHECK( d[3].col == 0 );
	CHECK( d[3].message == "No alternatives for expression Name: sepDisable" );
	CHECK( d[3].detail == std::vector<std::string>{ "Name: sepDisable", "Alternatives are" } );
	CHECK( d[4].file == "/p/b.hfa" );
	CHECK( d[4].severity == "error" );
	CHECK( d[5].file.empty() );
	CHECK( d[5].message == "cc1: stdbool.h: No such file or directory" );
	CHECK( d[6].severity == "note" );
}

TEST_CASE( "line markers" ) {
	CHECK( parseLineMarker( "# 12 \"/a/b.cfa\" 1 3" ) == std::make_optional( std::make_pair( 12, std::string( "/a/b.cfa" ) ) ) );
	CHECK( parseLineMarker( "#line 3 \"x\"" ) == std::make_optional( std::make_pair( 3, std::string( "x" ) ) ) );
	CHECK( parseLineMarker( "# 1 \"we\\\"ird\\\\name\"" ) == std::make_optional( std::make_pair( 1, std::string( "we\"ird\\name" ) ) ) );
	CHECK( ! parseLineMarker( "#include <x>" ) );
	CHECK( ! parseLineMarker( "# define X" ) );

	LineMarkerMap m( "int a;\n# 10 \"/p/a.cfa\"\nint b;\nint c;\n# 3 \"/p/h.hfa\" 1\nint d;\n" );
	CHECK( ! m.lookup( 1 ) );
	CHECK( ! m.lookup( 2 ) );
	CHECK( m.lookup( 3 ) == std::make_optional( std::make_pair( std::string( "/p/a.cfa" ), 10 ) ) );
	CHECK( m.lookup( 4 ) == std::make_optional( std::make_pair( std::string( "/p/a.cfa" ), 11 ) ) );
	CHECK( m.lookup( 6 ) == std::make_optional( std::make_pair( std::string( "/p/h.hfa" ), 3 ) ) );
	CHECK( ! m.lookup( 99 ) );
}

TEST_CASE( "include sites from preprocessor output" ) {
	// Shape of `cfa -E` output: main includes lib (line 1) and a.hfa (line
	// 3); a.hfa includes b.hfa on its line 2.
	std::string pp =
		"# 0 \"/tmp/x/in.cfa\"\n"
		"# 1 \"/p/main.cfa\"\n"
		"# 1 \"/usr/include/lib.h\" 1 3\n"
		"int lib;\n"
		"# 2 \"/p/main.cfa\" 2\n"
		"\n"
		"# 1 \"/p/a.hfa\" 1\n"
		"int a;\n"
		"# 1 \"/p/b.hfa\" 1\n"
		"int b;\n"
		"# 3 \"/p/a.hfa\" 2\n"
		"# 4 \"/p/main.cfa\" 2\n"
		"# 1 \"/p/a.hfa\" 1\n"
		"# 5 \"/p/main.cfa\" 2\n";
	IncludeSites s = findIncludeSites( pp );
	CHECK( s["/usr/include/lib.h"] == std::make_pair( std::string( "/p/main.cfa" ), 1 ) );
	CHECK( s["/p/a.hfa"] == std::make_pair( std::string( "/p/main.cfa" ), 3 ) );	// first inclusion wins
	CHECK( s["/p/b.hfa"] == std::make_pair( std::string( "/p/a.hfa" ), 2 ) );
	CHECK( includeLineInMain( s, "/p/b.hfa", "/p/main.cfa" ) == std::optional<int>( 3 ) );
	CHECK( includeLineInMain( s, "/p/a.hfa", "/p/main.cfa" ) == std::optional<int>( 3 ) );
	CHECK( ! includeLineInMain( s, "/p/none.hfa", "/p/main.cfa" ) );
}

TEST_CASE( "compiler output to diagnostics" ) {
	Toolchain tc;
	tc.cfaPrefix = "/opt/cfa";
	std::string out =
		"/tmp/t/in.cfa:2:3: error: in the temp copy\n"
		"/p/main.cfa:4:1: warning: #pragma once in main file\n"
		"/opt/cfa/include/cfa/fstream.hfa:9:1: error: in libcfa\n"
		"/opt/cfa/include/cfa/fstream.hfa:12:1: warning: not the user's problem\n"
		"/opt/cfa/include/cfa/fstream.hfa:12:1: note: nor is this\n"
		"/p/inc.hfa:5:2: error: in a project header\n"
		"/p/inc.hfa:5:2: note: see here\n"
		"cc1: fatal error: no position\n"
		"/p/main.cfa:7:1 error: resolver says no\n";
	auto d = diagsFromOutput( out, "/p/main.cfa", "/tmp/t/in.cfa", "/p", tc, "cpp" );
	REQUIRE( d.size() == 5 );
	CHECK( d[0].file == "/p/main.cfa" );
	CHECK( d[0].range.start == Loc{ 1, 2 } );
	CHECK( ! d[0].wholeLine );
	// Errors in libcfa are kept, for the header; its warnings, and their notes, are not.
	CHECK( d[1].file == "/opt/cfa/include/cfa/fstream.hfa" );
	CHECK( d[1].range.start == Loc{ 8, 0 } );
	CHECK( d[1].related.empty() );
	CHECK( d[2].file == "/p/inc.hfa" );
	CHECK( d[2].related.size() == 1 );
	CHECK( d[3].file == "/p/main.cfa" );
	CHECK( d[3].wholeLine );
	CHECK( d[3].range.start.line == 0 );
	CHECK( d[4].wholeLine );
	CHECK( d[4].range.start.line == 6 );
	CHECK( d[4].source == "cpp" );
}
