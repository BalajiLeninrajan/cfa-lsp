// The built server, killed by a termination signal while a check runs, must
// not leave the check's processes or its temp directory behind. Uses the
// fake toolchain (tests/server/fake/fake_cfa.cpp).
#include <doctest/doctest.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "LspClient.hpp"
#include "server/Checker.hpp"

using namespace cfalsp::test;
namespace fs = std::filesystem;

namespace {

bool gone( pid_t pid ) {
	std::ifstream stat( "/proc/" + std::to_string( pid ) + "/stat" );
	if ( ! stat ) return true;
	std::string s;
	std::getline( stat, s );
	size_t rp = s.rfind( ')' );
	return rp != std::string::npos && rp + 2 < s.size() && s[rp + 2] == 'Z';
}

void stopsCleanly( int sig ) {
	std::string fake = envOr( "CFA_LSP_FAKE_CFA" );
	if ( serverBinary().empty() || fake.empty() ) {
		MESSAGE( "needs CFA_LSP_SERVER and CFA_LSP_FAKE_CFA; skipping" );
		return;
	}
	std::string fixtures = envOr( "CFA_LSP_FIXTURES", "tests/fixtures" );
	cfalsp::TempDir scratch;
	std::string tmp = scratch.path() + "/tmp", pids = scratch.path() + "/pids";
	fs::create_directories( tmp );
	std::string oldTmp = envOr( "TMPDIR" );
	setenv( "TMPDIR", tmp.c_str(), 1 );
	setenv( "FAKE_CFA_DIR", ( fixtures + "/server" ).c_str(), 1 );
	setenv( "FAKE_CFA_PIDS", pids.c_str(), 1 );
	pid_t child = 0, grandchild = 0;
	int status = -1;
	{
		LspClient c;
		c.initialize( { { "cfa", fake }, { "translator", fake }, { "cc", fake }, { "debounceMs", 0 },
						{ "preludeDir", "/nonexistent-prelude" }, { "flags", { "-Wall" } } } );
		std::string path = fs::canonical( fixtures + "/server/hello.cfa" ).string();
		c.open( "file://" + path, "// FAKE_SLOW\n" + readAll( path ) );
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
		while ( ! fs::exists( pids ) && std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
		REQUIRE( fs::exists( pids ) );
		std::ifstream( pids ) >> child >> grandchild;
		status = c.sendSignal( sig );
	}
	if ( oldTmp.empty() ) unsetenv( "TMPDIR" );
	else setenv( "TMPDIR", oldTmp.c_str(), 1 );
	unsetenv( "FAKE_CFA_PIDS" );

	CHECK( WIFSIGNALED( status ) );
	if ( WIFSIGNALED( status ) ) CHECK( WTERMSIG( status ) == sig );
	REQUIRE( child > 0 );
	REQUIRE( grandchild > 0 );
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( ! ( gone( child ) && gone( grandchild ) ) && std::chrono::steady_clock::now() < deadline ) {
		std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
	}
	CHECK( gone( child ) );
	CHECK( gone( grandchild ) );
	if ( ! gone( grandchild ) ) kill( grandchild, SIGKILL );
	if ( ! gone( child ) ) kill( child, SIGKILL );
	CHECK( fs::is_empty( tmp ) );
}

} // namespace

TEST_SUITE( "integration" ) {

TEST_CASE( "SIGTERM while checking: no processes or temp directory left behind" ) { stopsCleanly( SIGTERM ); }
TEST_CASE( "SIGHUP while checking: no processes or temp directory left behind" ) { stopsCleanly( SIGHUP ); }

TEST_CASE( "stale temp directories of a killed server are removed at startup" ) {
	cfalsp::TempDir scratch;
	std::string oldTmp = envOr( "TMPDIR" );
	setenv( "TMPDIR", scratch.path().c_str(), 1 );
	std::string stale = scratch.path() + "/cfa-lsp-AAAAAA", fresh = scratch.path() + "/cfa-lsp-BBBBBB";
	std::string other = scratch.path() + "/cfa-lsp-translator-CCCCCC";
	for ( const auto & d : { stale, fresh, other } ) {
		fs::create_directories( d );
		std::ofstream( d + "/in.cfa" ) << "int x;\n";
	}
	fs::last_write_time( stale, fs::file_time_type::clock::now() - std::chrono::hours( 2 ) );
	fs::last_write_time( other, fs::file_time_type::clock::now() - std::chrono::hours( 2 ) );
	cfalsp::removeStaleTempDirs();
	if ( oldTmp.empty() ) unsetenv( "TMPDIR" );
	else setenv( "TMPDIR", oldTmp.c_str(), 1 );
	CHECK_FALSE( fs::exists( stale ) );
	CHECK( fs::exists( fresh ) );
	CHECK( fs::exists( other ) );
}

} // TEST_SUITE
