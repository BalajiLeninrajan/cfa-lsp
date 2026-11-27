// Checker runs against the installed cfa. These skip when cfa isn't on PATH.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <vector>

#include "server/Checker.hpp"
#include "server/Toolchain.hpp"

using namespace cfalsp;
namespace fs = std::filesystem;

namespace {

std::string envOr( const char * name ) {
	const char * v = std::getenv( name );
	return v && *v ? v : "";
}

bool executable( const std::string & p ) { return ! p.empty() && access( p.c_str(), X_OK ) == 0; }

CheckRequest requestFor( const std::string & path, const std::string & text ) {
	CheckRequest req;
	req.path = path;
	req.text = text;
	req.flags = { "-Wall" };
	req.flagsBase = fs::path( path ).parent_path().string();
	req.timeout = std::chrono::seconds( 120 );
	return req;
}

} // namespace

TEST_CASE( "real cfa: preprocessor errors land on the real file" ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( findInPath( "cfa" ).empty() || ! executable( fake ) ) {
		MESSAGE( "needs cfa on PATH and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = fake;						// only the cpp stage matters here
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	std::ofstream( proj.path() + "/local.hfa" ) << "int local_thing;\n";

	FrontResult fr = ch.front( requestFor( main, "#include \"local.hfa\"\n\n#include \"missing.hfa\"\nint main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	REQUIRE( ! fr.diags.empty() );
	CHECK( fr.diags[0].file == main );
	CHECK( fr.diags[0].range.start.line == 2 );
	CHECK( fr.diags[0].severity == 1 );
	CHECK( fr.diags[0].message.find( "missing.hfa" ) != std::string::npos );
}

// The server takes the #line directives out of the buffer before cpp reads
// it, so cpp's messages give the real lines. A #line in a branch cpp skips
// doesn't matter either.
TEST_CASE( "real cfa: preprocessor diagnostics after a #line land on the real line" ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( findInPath( "cfa" ).empty() || ! executable( fake ) ) {
		MESSAGE( "needs cfa on PATH and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = fake;						// only the cpp stage matters here
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	auto lineOf = [&]( const FrontResult & fr, const std::string & text ) {
		int line = -1;
		for ( const Diag & d : fr.diags ) {
			INFO( d.file << ":" << d.range.start.line << ": " << d.message );
			if ( d.file == main && d.message.find( text ) != std::string::npos ) line = d.range.start.line;
		}
		return line;
	};

	FrontResult fr = ch.front( requestFor( main,
		"int a;\n"								// 0
		"#if 0\n"								// 1
		"#line 3\n"								// 2
		"#endif\n"								// 3
		"#warning before the directive\n"		// 4
		"#line 100\n"							// 5
		"int b;\n"								// 6
		"#warning after the directive\n"		// 7
		"#error still after it\n"				// 8
		"int main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, "before the directive" ) == 4 );
	CHECK( lineOf( fr, "after the directive" ) == 7 );
	CHECK( lineOf( fr, "still after it" ) == 8 );

	// cpp prints markers around the expansions of NULL and errno.
	fr = ch.front( requestFor( main,
		"#include <stdio.h>\n"					// 0
		"#include <errno.h>\n"					// 1
		"#if 0\n"								// 2
		"#line 7\n"								// 3
		"#endif\n"								// 4
		"int a;\n"								// 5
		"void f() { int * p = NULL;\n"			// 6
		"\terrno = 0;\n"						// 7
		"}\n"									// 8
		"#error after a system macro\n"			// 9
		"int main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, "after a system macro" ) == 9 );

	// Backwards, and in the GNU form naming another file.
	fr = ch.front( requestFor( main,
		"int a;\n"								// 0
		"#warning first\n"						// 1
		"#line 1\n"								// 2
		"#warning second\n"						// 3
		"# 1 \"other.cfa\"\n"					// 4
		"#warning third\n"						// 5
		"int main() {}\n" ), CancelToken() );
	CHECK( lineOf( fr, "#warning first" ) == 1 );
	CHECK( lineOf( fr, "#warning second" ) == 3 );
	CHECK( lineOf( fr, "#warning third" ) == 5 );

	// A #line that cpp reports as malformed stays, so the check shows cpp's
	// error as a build of the file would.
	fr = ch.front( requestFor( main,
		"int a;\n"								// 0
		"#line 10 foo.c\n"						// 1
		"int main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, "is not a valid filename" ) == 1 );
	fr = ch.front( requestFor( main,
		"int a;\n"								// 0
		"int b;\n"								// 1
		"#line abc\n"							// 2
		"int main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, "is not a positive integer" ) == 2 );
}

// gcc reads the translator's C, whose line markers come from the translator's
// positions.
TEST_CASE( "real cfa and translator: backend diagnostics after a #line land on the real line" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	// The 0-based line of gcc's "unused variable 'name'" in main.
	auto unusedLine = [&]( const std::string & text, const std::string & name ) {
		CheckRequest req = requestFor( main, text );
		FrontResult fr = ch.front( req, CancelToken() );
		REQUIRE( fr.status == FrontResult::Ok );
		REQUIRE( fr.backendReady );
		int line = -1;
		for ( const Diag & d : ch.back( req, fr, CancelToken() ) ) {
			INFO( d.file << ":" << d.range.start.line << ": " << d.message );
			if ( d.file == main && d.message.find( "unused variable '" + name + "'" ) != std::string::npos ) line = d.range.start.line;
		}
		return line;
	};
	const std::string text =
		"int main() {\n"						// 0
		"#ifdef NOT_DEFINED\n"					// 1
		"#line 3\n"								// 2
		"#endif\n"								// 3
		"\tint early;\n"						// 4
		"#line 100\n"							// 5
		"\tint unused;\n"						// 6
		"\treturn 0;\n"							// 7
		"}\n";
	CHECK( unusedLine( text, "early" ) == 4 );
	CHECK( unusedLine( text, "unused" ) == 6 );

	// Numbered backwards, the markers would give line 2 twice.
	const std::string back =
		"int a;\n"								// 0
		"int b;\n"								// 1
		"#line 1\n"								// 2
		"int main() {\n"						// 3
		"\tint unused;\n"						// 4
		"\treturn 0;\n"							// 5
		"}\n";
	CHECK( unusedLine( back, "unused" ) == 4 );
}

// The translator's positions, and so the references, are on the real lines.
TEST_CASE( "real cfa and translator: references after a #line are on the real line" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	const std::vector<std::string> lines = {
		"#include <stdio.h>",					// 0
		"#include <errno.h>",					// 1
		"int a;",								// 2
		"#if 0",								// 3
		"#line 8",								// 4
		"#endif",								// 5
		"int b() { int * p = NULL; return a; }",	// 6
		"#line 3",								// 7
		"int c() {",							// 8
		"\terrno = 0;",							// 9
		"\treturn a + b();",					// 10
		"}",									// 11
		"#line 1 \"other.cfa\"",				// 12
		"int d() { return c(); }",				// 13
		"#line 1",								// 14
		"int main() { return d(); }",			// 15
	};
	std::string text;
	for ( const auto & l : lines ) text += l + "\n";
	CheckRequest req = requestFor( main, text );
	req.backend = false;
	FrontResult fr = ch.front( req, CancelToken() );
	REQUIRE( fr.analysis );
	// The line of the declaration that `use`, on `line`, refers to.
	auto defLine = [&]( int line, const std::string & use ) {
		int col = (int)lines[line].find( use );
		REQUIRE( col >= 0 );
		auto def = fr.analysis->definition( main, { line, col } );
		REQUIRE( def.size() == 1 );
		CHECK( def[0].file == main );
		return def[0].range.start.line;
	};
	CHECK( defLine( 6, "a;" ) == 2 );
	CHECK( defLine( 10, "a +" ) == 2 );
	CHECK( defLine( 10, "b(" ) == 6 );
	CHECK( defLine( 13, "c(" ) == 8 );
	CHECK( defLine( 15, "d(" ) == 13 );
}

// An unsaved header that cpp finds on disk, not in its overlay, keeps its
// #line directives for cpp, so SourceMap maps them from the buffer as before.
TEST_CASE( "real cfa and translator: an unsaved header cpp reads from disk keeps its #line mapping" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa", a = proj.path() + "/a.h", b = proj.path() + "/b.h",
		c = proj.path() + "/c.h";
	const std::string bText = "int b1;\n#line 100\nint bv;\n";
	const std::string cText = "int c1;\n#line 50\nint cv;\n";
	// a.h includes b.h from its own directory, so cpp reads b.h from disk.
	// main.cfa's c.h comes from the overlay.
	std::ofstream( a ) << "#include \"b.h\"\n";
	std::ofstream( b ) << bText;
	std::ofstream( c ) << cText;
	const std::string use = "int f() { return bv + cv; }";
	CheckRequest req = requestFor( main, "#include \"a.h\"\n#include \"c.h\"\n" + use + "\n" );
	req.backend = false;
	req.overlays[b] = bText;
	req.overlays[c] = cText;
	FrontResult fr = ch.front( req, CancelToken() );
	REQUIRE( fr.analysis );
	auto def = fr.analysis->definition( main, { 2, (int)use.find( "bv" ) } );
	REQUIRE( def.size() == 1 );
	CHECK( def[0].file == b );
	CHECK( def[0].range.start.line == 2 );
	def = fr.analysis->definition( main, { 2, (int)use.find( "cv" ) } );
	REQUIRE( def.size() == 1 );
	CHECK( def[0].file == c );
	CHECK( def[0].range.start.line == 2 );
}

// A header with no include guard that cpp reads from its overlay and from
// disk keeps its #line directives in both, so SourceMap's one text for it
// maps both copies.
TEST_CASE( "real cfa and translator: an unsaved header read from its overlay and from disk keeps its #line mapping" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa", a = proj.path() + "/a.h", b = proj.path() + "/b.h";
	const std::string bText =
		"extern int b0;\n"						// 0
		"#line 100\n"							// 1
		"#ifndef ONCE\n"						// 2
		"#define ONCE\n"						// 3
		"extern int first;\n"					// 4
		"#else\n"								// 5
		"extern int second;\n"					// 6
		"#endif\n";
	// main.cfa gets b.h from the overlay, then a.h gets it from disk.
	std::ofstream( a ) << "#include \"b.h\"\n";
	std::ofstream( b ) << bText;
	const std::string use = "int f() { return first + second; }";
	CheckRequest req = requestFor( main, "#include \"b.h\"\n#include \"a.h\"\n" + use + "\n" );
	req.backend = false;
	req.overlays[b] = bText;
	FrontResult fr = ch.front( req, CancelToken() );
	REQUIRE( fr.analysis );
	auto def = fr.analysis->definition( main, { 2, (int)use.find( "first" ) } );
	REQUIRE( def.size() == 1 );
	CHECK( def[0].file == b );
	CHECK( def[0].range.start.line == 4 );
	def = fr.analysis->definition( main, { 2, (int)use.find( "second" ) } );
	REQUIRE( def.size() == 1 );
	CHECK( def[0].file == b );
	CHECK( def[0].range.start.line == 6 );
}

// The summary of an included file's errors goes on the real #include line.
// An unsaved header loses its #line directives too.
TEST_CASE( "real cfa: include summaries after a #line land on the real line" ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( findInPath( "cfa" ).empty() || ! executable( fake ) ) {
		MESSAGE( "needs cfa on PATH and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = fake;						// only the cpp stage matters here
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa", bad = proj.path() + "/bad.hfa";
	std::ofstream( bad ) << "#error boom\n";
	auto lineOf = []( const FrontResult & fr, const std::string & file, const std::string & text ) {
		int line = -1;
		for ( const Diag & d : fr.diags ) {
			INFO( d.file << ":" << d.range.start.line << ": " << d.message );
			if ( d.file == file && d.message.find( text ) != std::string::npos ) line = d.range.start.line;
		}
		return line;
	};

	FrontResult fr = ch.front( requestFor( main,
		"int a;\n"								// 0
		"#line 100\n"							// 1
		"#include \"bad.hfa\"\n"				// 2
		"int main() {}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, main, "in included file bad.hfa" ) == 2 );
	CHECK( lineOf( fr, bad, "boom" ) == 0 );

	CheckRequest req = requestFor( main,
		"int a;\n"								// 0
		"#line 1\n"								// 1
		"#warning second\n"						// 2
		"#include \"bad.hfa\"\n"				// 3
		"int main() {}\n" );
	req.overlays[bad] = "#line 40\nint x;\n#error unsaved\n";
	fr = ch.front( req, CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	CHECK( lineOf( fr, main, "#warning second" ) == 2 );
	CHECK( lineOf( fr, main, "in included file bad.hfa" ) == 3 );
	CHECK( lineOf( fr, bad, "unsaved" ) == 2 );
}

TEST_CASE( "real cfa: the file's directory is searched for quoted includes only" ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( findInPath( "cfa" ).empty() || ! executable( fake ) ) {
		MESSAGE( "needs cfa on PATH and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = fake;						// only the cpp stage matters here
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	// A project header with the name of a libcfa header.
	std::ofstream( proj.path() + "/string.hfa" ) << "#error the project's string.hfa\n";
	fs::create_directories( proj.path() + "/inc" );
	std::ofstream( proj.path() + "/inc/only_here.hfa" ) << "int only_here;\n";
	auto cppErrors = [&]( const CheckRequest & req ) {
		std::string out;
		for ( const auto & d : ch.front( req, CancelToken() ).diags ) {
			if ( d.source == "cpp" && d.severity == 1 ) out += d.message + "\n";
		}
		return out;
	};

	// <string.hfa> is libcfa's.
	CHECK( cppErrors( requestFor( main, "#include <string.hfa>\nint main() {}\n" ) ) == "" );
	// "string.hfa" is the one next to the file.
	CHECK( cppErrors( requestFor( main, "#include \"string.hfa\"\nint main() {}\n" ) ).find( "the project's string.hfa" ) != std::string::npos );
	// -iquote from the flags goes through the cfa driver too.
	CheckRequest req = requestFor( main, "#include \"only_here.hfa\"\nint main() { return only_here; }\n" );
	CHECK( cppErrors( req ).find( "only_here.hfa" ) != std::string::npos );
	req.flags = { "-iquote", "inc" };
	CHECK( cppErrors( req ) == "" );
}

TEST_CASE( "real cfa and translator: an error in a system header goes on the header" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	// stdio.h declares `typedef struct _IO_FILE FILE;`, which this breaks.
	CheckRequest req = requestFor( main, "#define FILE 1\n#include <stdio.h>\nint main() {}\n" );
	req.backend = false;
	FrontResult fr = ch.front( req, CancelToken() );
	bool onHeader = false, summary = false;
	for ( const auto & d : fr.diags ) {
		INFO( d.file << ":" << d.range.start.line << ": " << d.message );
		if ( d.file.starts_with( "/usr/include/" ) && d.severity == 1 ) onHeader = true;
		if ( d.file == main && d.range.start.line == 1 && d.message.find( "in included file" ) != std::string::npos ) summary = true;
	}
	CHECK( onHeader );
	CHECK( summary );
}

TEST_CASE( "real cfa: unsaved headers are read from their buffers" ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( findInPath( "cfa" ).empty() || ! executable( fake ) ) {
		MESSAGE( "needs cfa on PATH and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = fake;						// only the cpp stage matters here
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	fs::create_directories( proj.path() + "/inc" );
	fs::create_directories( proj.path() + "/q" );
	std::string main = proj.path() + "/main.cfa", local = proj.path() + "/local.hfa", other = proj.path() + "/inc/other.hfa",
				quoted = proj.path() + "/q/quoted.hfa";
	std::ofstream( local ) << "int local_thing;\n";
	std::ofstream( other ) << "int other_thing;\n";
	std::ofstream( quoted ) << "int quoted_thing;\n";
	CheckRequest req = requestFor( main, "#include \"local.hfa\"\n#include \"other.hfa\"\n#include \"quoted.hfa\"\nint main() {}\n" );
	req.flags = { "-I", "inc", "-iquote", "q" };

	// From disk the files are fine.
	FrontResult fr = ch.front( req, CancelToken() );
	CHECK( fr.status != FrontResult::PreprocessFailed );

	// The buffers are found first: next to the main file, through -I and
	// through -iquote. cpp's messages name the real files.
	req.overlays[local] = "int local_thing;\n#error local buffer\n";
	req.overlays[other] = "\n\n#error other buffer\n";
	req.overlays[quoted] = "#error quoted buffer\n";
	fr = ch.front( req, CancelToken() );
	CHECK( fr.status == FrontResult::PreprocessFailed );
	bool sawLocal = false, sawOther = false, sawQuoted = false;
	for ( const Diag & d : fr.diags ) {
		INFO( d.file << ":" << d.range.start.line << ": " << d.message );
		if ( d.file == local && d.range.start.line == 1 && d.message.find( "local buffer" ) != std::string::npos ) sawLocal = true;
		if ( d.file == other && d.range.start.line == 2 && d.message.find( "other buffer" ) != std::string::npos ) sawOther = true;
		if ( d.file == quoted && d.range.start.line == 0 && d.message.find( "quoted buffer" ) != std::string::npos ) sawQuoted = true;
		CHECK( d.file.find( "/overlay" ) == std::string::npos );
	}
	CHECK( sawLocal );
	CHECK( sawOther );
	CHECK( sawQuoted );
}

TEST_CASE( "real cfa without the translator: falls back to compiling" ) {
	if ( findInPath( "cfa" ).empty() ) {
		MESSAGE( "needs cfa on PATH; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = "/nonexistent/cfa-cpp";
	Toolchain tc = discoverToolchain( o );
	REQUIRE( tc.translator.empty() );
	Checker ch( tc );
	TempDir proj;
	std::string main = proj.path() + "/main.cfa";
	FrontResult fr = ch.front( requestFor( main, "int main() {\n\tint x = nosuchname;\n\treturn x;\n}\n" ), CancelToken() );
	CHECK( fr.status == FrontResult::Fallback );
	REQUIRE( ! fr.diags.empty() );
	CHECK( fr.diags[0].file == main );
	CHECK( fr.diags[0].range.start.line == 1 );
	CHECK( fr.diags[0].message.find( "nosuchname" ) != std::string::npos );

	// An error in an included project header shows on the header and as a
	// summary on the #include line.
	std::ofstream( proj.path() + "/bad.hfa" ) << "#pragma once\nstatic int bad = 1 + nosuchthing;\n";
	fr = ch.front( requestFor( main, "int x;\n#include \"bad.hfa\"\nint main() { return bad; }\n" ), CancelToken() );
	bool onHeader = false, summary = false;
	for ( const auto & d : fr.diags ) {
		if ( d.file == proj.path() + "/bad.hfa" && d.range.start.line == 1 ) onHeader = true;
		if ( d.file == main && d.range.start.line == 1 && d.message.find( "in included file bad.hfa" ) != std::string::npos ) summary = true;
	}
	CHECK( onHeader );
	CHECK( summary );
}

TEST_CASE( "real cfa and translator: the demo fixture loads" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	std::string fixtures = envOr( "CFA_LSP_FIXTURES" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) || fixtures.empty() ) {
		MESSAGE( "needs cfa, CFA_LSP_TRANSLATOR and CFA_LSP_FIXTURES; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	std::string path = fs::canonical( fixtures + "/demo/counter.cfa" ).string();
	std::ifstream in( path );
	std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
	CheckRequest req = requestFor( path, text );
	FrontResult fr = ch.front( req, CancelToken() );
	if ( fr.status == FrontResult::TranslatorFailed ) {
		std::string msg = fr.diags.empty() ? "" : fr.diags.back().message;
		if ( msg.find( "Unknown option --lsp" ) != std::string::npos ) {
			MESSAGE( "translator has no --lsp mode yet; skipping" );
			return;
		}
	}
	INFO( ( fr.diags.empty() ? std::string() : fr.diags[0].message ) );
	CHECK( fr.status == FrontResult::Ok );
	CHECK( fr.analysis );
	CHECK( fr.usable );
	for ( const auto & d : fr.diags ) CHECK_MESSAGE( d.severity != 1, d.message );
	std::vector<Diag> back = ch.back( req, fr, CancelToken() );
	for ( const auto & d : back ) CHECK_MESSAGE( d.severity != 1, d.message );
}

TEST_CASE( "cancellation stops a check promptly" ) {
	if ( findInPath( "cfa" ).empty() ) {
		MESSAGE( "needs cfa on PATH; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = "/nonexistent/cfa-cpp";		// fallback: one long cfa run
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	CancelToken tok;
	std::thread t( [&] {
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
		tok.cancel();
	} );
	auto t0 = std::chrono::steady_clock::now();
	FrontResult fr = ch.front( requestFor( proj.path() + "/m.cfa", "#include <fstream.hfa>\nint main() { sout | 1; }\n" ), tok );
	t.join();
	CHECK( fr.status == FrontResult::Cancelled );
	CHECK( std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 2 ) );
}

TEST_CASE( "real cfa and translator: declarations from the prelude's builtins point at builtins.cfa" ) {
	std::string translator = envOr( "CFA_LSP_TRANSLATOR" );
	if ( findInPath( "cfa" ).empty() || ! executable( translator ) ) {
		MESSAGE( "needs cfa and CFA_LSP_TRANSLATOR; skipping" );
		return;
	}
	ToolchainOptions o;
	o.translator = translator;
	Checker ch( discoverToolchain( o ) );
	TempDir proj;
	std::string main = proj.path() + "/gen.cfa";
	std::string text = "generator G { int n; };\nvoid main( G & g ) { suspend; }\nint main() { G g; resume( g ); }\n";
	CheckRequest req = requestFor( main, text );
	req.backend = false;
	FrontResult fr = ch.front( req, CancelToken() );
	REQUIRE( fr.analysis );
	// builtins.cfa names the files it was made from on the build machine. Where
	// they don't exist (an installed toolchain), definitions go to builtins.cfa;
	// where they do (CFA built from source on this machine), to the real file.
	auto def = fr.analysis->definition( main, { 2, 19 } );
	REQUIRE( def.size() == 1 );
	INFO( def[0].file );
	CHECK( ( def[0].file.ends_with( "/builtins.cfa" ) || std::filesystem::exists( def[0].file ) ) );
	std::ifstream in( def[0].file );
	std::string line;
	for ( int i = 0; i <= def[0].range.start.line && std::getline( in, line ); i += 1 ) {}
	CHECK( line.substr( def[0].range.start.col, 6 ) == "resume" );
}
