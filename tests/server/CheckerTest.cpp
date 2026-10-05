// Checker runs against the installed cfa. These skip when cfa isn't on PATH.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

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
