#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "server/Checker.hpp"
#include "server/Flags.hpp"
#include "server/Toolchain.hpp"

using namespace cfalsp;
namespace fs = std::filesystem;

using V = std::vector<std::string>;

static bool contains( const V & v, const V & seq ) {
	return std::search( v.begin(), v.end(), seq.begin(), seq.end() ) != v.end();
}

TEST_CASE( "cfa_flags.txt parsing" ) {
	CHECK( parseFlagsFile( "-Wall\n# comment\n\n  -Wextra  \n-I ../inc\n\t# indented comment\n-DX=1\r\n" ) ==
		   V{ "-Wall", "-Wextra", "-I", "../inc", "-DX=1" } );
	CHECK( parseFlagsFile( "" ).empty() );
}

TEST_CASE( "nearest cfa_flags.txt" ) {
	TempDir tmp;
	fs::create_directories( tmp.path() + "/a/b/c" );
	std::ofstream( tmp.path() + "/a/cfa_flags.txt" ) << "-Wall\n";
	CHECK( findFlagsFile( tmp.path() + "/a/b/c" ) == std::optional<std::string>( tmp.path() + "/a/cfa_flags.txt" ) );
	CHECK( findFlagsFile( tmp.path() + "/a" ) == std::optional<std::string>( tmp.path() + "/a/cfa_flags.txt" ) );
	std::ofstream( tmp.path() + "/a/b/cfa_flags.txt" ) << "-w\n";
	CHECK( findFlagsFile( tmp.path() + "/a/b/c" ) == std::optional<std::string>( tmp.path() + "/a/b/cfa_flags.txt" ) );
}

TEST_CASE( "flags are split by stage like the cfa driver does" ) {
	FlagSet f = classifyFlags( { "-Wall", "-Wextra", "-Werror", "-Wno-self-assign", "-Wself-assign", "-DX=1", "-D", "Y",
								 "-UZ", "-I", "inc", "-I/abs", "-iquote", "q", "-isystem", "sys", "-include", "pre.h",
								 "-std=gnu17", "-O2", "-fno-common", "-g", "-c", "-o", "out.o", "main.cfa", "-nodebug",
								 "-m32", "-lm", "-Wl,--as-needed", "-fdiagnostics-color=always" },
							   "/proj" );
	CHECK( contains( f.cpp, { "-DX=1", "-D", "Y", "-UZ", "-I", "/proj/inc", "-I", "/abs", "-Wp,-iquote/proj/q", "-isystem",
							  "/proj/sys", "-include", "/proj/pre.h" } ) );
	CHECK( contains( f.cpp, { "-std=gnu17" } ) );
	CHECK( contains( f.cpp, { "-nodebug" } ) );
	CHECK( contains( f.cpp, { "-m32" } ) );
	CHECK( std::find( f.cpp.begin(), f.cpp.end(), "-iquote" ) == f.cpp.end() );
	CHECK( classifyFlags( { "-iquoteq" }, "/proj" ).cpp == V{ "-Wp,-iquote/proj/q" } );
	// -Wp, would split the directory at the comma.
	CHECK( classifyFlags( { "-iquote", "a,b" }, "/proj" ).cpp == V{ "-I", "/proj/a,b" } );
	CHECK( std::find( f.cpp.begin(), f.cpp.end(), "main.cfa" ) == f.cpp.end() );
	CHECK( std::find( f.cpp.begin(), f.cpp.end(), "out.o" ) == f.cpp.end() );
	CHECK( f.translator == V{ "-Wall", "-Werror", "-Wno-self-assign", "-Wself-assign" } );
	CHECK( f.backend == V{ "-Wall", "-Wextra", "-Werror", "-Wno-self-assign", "-Wself-assign", "-std=gnu17", "-O2",
						   "-fno-common", "-m32" } );
	CHECK( ! f.debug );
	CHECK( f.m32 );
	CHECK( ! f.nolib );
}

TEST_CASE( "commands passed to each stage" ) {
	Toolchain tc;
	tc.cfa = "/opt/cfa/bin/cfa";
	tc.translator = "/x/cfa-cpp";
	tc.preludeDir = "/p/x64-debug";
	Checker ch( tc );
	CheckRequest req;
	req.path = "/home/u/proj/main.cfa";
	FlagSet f = classifyFlags( { "-Wall", "-Wextra", "-DN=3" }, "/home/u/proj" );

	V cpp = ch.cppCommand( req, f, "/tmp/t/in.cfa" );
	CHECK( cpp.front() == "/opt/cfa/bin/cfa" );
	CHECK( contains( cpp, { "-E" } ) );
	CHECK( contains( cpp, { "-DN=3" } ) );
	// The file's directory is a quote directory, ahead of the user's flags.
	auto quote = std::find( cpp.begin(), cpp.end(), "-Wp,-iquote/home/u/proj" );
	CHECK( quote < std::find( cpp.begin(), cpp.end(), "-DN=3" ) );
	CHECK( std::find( cpp.begin(), cpp.end(), "-I" ) == cpp.end() );
	CHECK( cpp.back() == "/tmp/t/in.cfa" );
	req.path = "/home/u/a,b/main.cfa";
	CHECK( contains( ch.cppCommand( req, f, "/tmp/t/in.cfa" ), { "-I", "/home/u/a,b" } ) );
	req.path = "/home/u/proj/main.cfa";

	V tr = ch.translatorCommand( req, f, "/tmp/t/in.i", "/tmp/t/out.json", "/tmp/t/out.c" );
	CHECK( tr == V{ "/x/cfa-cpp", "--lsp", "/tmp/t/out.json", "--lsp-focus", "/home/u/proj/main.cfa", "--lsp-c-out",
					"/tmp/t/out.c", "-Wall", "--prelude-dir=/p/x64-debug", "-L", "--colors=never", "--lsp-skip-bodies",
					"/p/x64-debug", "--lsp-skip-bodies", "/usr", "--lsp-skip-bodies", "/lib", "--lsp-skip-bodies", "/lib64",
					"--lsp-skip-bodies", "/opt", "/tmp/t/in.i" } );
	V noC = ch.translatorCommand( req, f, "/tmp/t/in.i", "/tmp/t/out.json", "" );
	CHECK( std::find( noC.begin(), noC.end(), "--lsp-c-out" ) == noC.end() );
	req.skipSystemBodies = false;
	V keep = ch.translatorCommand( req, f, "/tmp/t/in.i", "/tmp/t/out.json", "" );
	CHECK( std::find( keep.begin(), keep.end(), "--lsp-skip-bodies" ) == keep.end() );
	req.skipSystemBodies = true;

	V be = ch.backendCommand( f, "/tmp/t/out.c" );
	CHECK( be.front() == "gcc" );
	CHECK( contains( be, { "-fsyntax-only" } ) );
	CHECK( contains( be, { "-x", "cpp-output" } ) );
	CHECK( contains( be, { "-Wall", "-Wextra" } ) );
	CHECK( contains( be, { "-std=gnu11" } ) );
	CHECK( std::find( be.begin(), be.end(), "-DN=3" ) == be.end() );
	CHECK( be.back() == "/tmp/t/out.c" );

	FlagSet g = classifyFlags( { "-std=c2x" }, "/" );
	V be2 = ch.backendCommand( g, "x.c" );
	CHECK( std::find( be2.begin(), be2.end(), "-std=gnu11" ) == be2.end() );
}

TEST_CASE( "overlay directories come right before the directories they shadow" ) {
	Toolchain tc;
	tc.cfa = "/opt/cfa/bin/cfa";
	tc.translator = "/x/cfa-cpp";
	Checker ch( tc );
	CheckRequest req;
	req.path = "/home/u/proj/src/main.cfa";
	req.extraFocus = { "/home/u/proj/inc/a.hfa", "/home/u/proj/src/main.cfa" };
	FlagSet f = classifyFlags( { "-I", "../inc", "-I/opt/other/", "-iquote", "../q" }, "/home/u/proj/src" );
	OverlayDirs ov = { { "/home/u/proj/src", "/tmp/t/overlay0" }, { "/home/u/proj/inc", "/tmp/t/overlay1" },
					   { "/home/u/proj/q", "/tmp/t/overlay2" } };

	V cpp = ch.cppCommand( req, f, "/tmp/t/in.cfa", ov );
	CHECK( contains( cpp, { "-Wp,-iquote/tmp/t/overlay0", "-Wp,-iquote/home/u/proj/src" } ) );
	CHECK( contains( cpp, { "-I", "/tmp/t/overlay1", "-I", "/home/u/proj/inc", "-I", "/opt/other/" } ) );
	CHECK( contains( cpp, { "-Wp,-iquote/tmp/t/overlay2", "-Wp,-iquote/home/u/proj/q" } ) );
	CHECK( std::count( cpp.begin(), cpp.end(), "-I" ) == 3 );
	CHECK( ch.cppCommand( req, f, "/tmp/t/in.cfa" ).size() == cpp.size() - 4 );

	// The main file is the first focus file, and only once.
	V tr = ch.translatorCommand( req, f, "/tmp/t/in.i", "/tmp/t/out.json", "" );
	CHECK( contains( tr, { "--lsp-focus", "/home/u/proj/src/main.cfa", "--lsp-focus", "/home/u/proj/inc/a.hfa", "-L" } ) );
	CHECK( std::count( tr.begin(), tr.end(), "--lsp-focus" ) == 2 );
}

TEST_CASE( "prelude directory follows the driver's layout" ) {
	TempDir tmp;
	fs::create_directories( tmp.path() + "/lib/cfa/x64-debug" );
	fs::create_directories( tmp.path() + "/lib/cfa/x64-nodebug" );
	CHECK( derivePreludeDir( tmp.path(), true, false, false ) == tmp.path() + "/lib/cfa/x64-debug" );
	CHECK( derivePreludeDir( tmp.path(), false, false, false ) == tmp.path() + "/lib/cfa/x64-nodebug" );
	// Missing configuration: the driver falls back to nolib, and so do we.
	fs::create_directories( tmp.path() + "/lib/cfa/x64-nolib" );
	fs::remove_all( tmp.path() + "/lib/cfa/x64-nodebug" );
	CHECK( derivePreludeDir( tmp.path(), false, false, false ) == tmp.path() + "/lib/cfa/x64-nolib" );
	CHECK( derivePreludeDir( "", true, false, false ) == "" );

	Toolchain tc;
	tc.cfaPrefix = tmp.path();
	FlagSet f = classifyFlags( { "-nodebug" }, "/" );
	CHECK( tc.preludeFor( f ) == tmp.path() + "/lib/cfa/x64-nolib" );
	tc.preludeDir = "/override";
	CHECK( tc.preludeFor( f ) == "/override" );
	CHECK( tc.isSystemPath( tmp.path() + "/include/cfa/fstream.hfa" ) );
	CHECK( tc.isSystemPath( "/usr/include/stdio.h" ) );
	CHECK( tc.isSystemPath( "extras.cfa" ) );
	CHECK( tc.isSystemPath( "<built-in>" ) );
	CHECK( ! tc.isSystemPath( "/home/u/proj/x.hfa" ) );
	CHECK( ! tc.isSystemPath( "/usrlocal/x.hfa" ) );
	// Only the prefix's installed headers and libraries: a dev build's prefix can hold the user's project.
	CHECK( ! tc.isSystemPath( tmp.path() + "/src/main.cfa" ) );
	CHECK( tc.systemDirs() == V{ tmp.path() + "/include", tmp.path() + "/lib", "/override", "/usr", "/lib", "/lib64", "/opt" } );
}
