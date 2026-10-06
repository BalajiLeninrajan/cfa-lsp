// cfa-lsp: a language server for Cforall. Speaks LSP on stdin/stdout; logs
// to stderr when CFA_LSP_LOG is set.
#include <pthread.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "Checker.hpp"
#include "Log.hpp"
#include "Server.hpp"
#include "Toolchain.hpp"
#include "Version.hpp"

int main( int argc, char * argv[] ) {
	for ( int i = 1; i < argc; i += 1 ) {
		std::string a = argv[i];
		if ( a == "--version" ) {
			std::printf( "cfa-lsp %s\n", cfalsp::version );
			return 0;
		}
		if ( a == "--help" || a == "-h" ) {
			std::printf( "usage: cfa-lsp [--stdio]\n"
						 "Cforall language server; speaks LSP over stdin/stdout.\n"
						 "Set CFA_LSP_LOG=error|warn|info|debug to log to stderr.\n" );
			return 0;
		}
		// --stdio and anything else editors pass is accepted and ignored.
	}
	std::signal( SIGPIPE, SIG_IGN );
	cfalsp::log::init();

	// Termination signals are taken by one thread, which stops the check in
	// flight before the process ends. Otherwise its processes, which run in
	// their own process group, would outlive the server, and so would its
	// temp directory with a copy of the buffer. Blocked here, before any
	// other thread starts, so every thread inherits the mask; children get an
	// empty one (see runProcess).
	sigset_t stop;
	sigemptyset( &stop );
	for ( int sig : { SIGTERM, SIGHUP, SIGINT } ) sigaddset( &stop, sig );
	pthread_sigmask( SIG_BLOCK, &stop, nullptr );

	cfalsp::removeStaleTempDirs();
	cfalsp::Server server( 0, 1, cfalsp::executableDir() );
	std::thread( [&server, stop] {
		int sig = 0;
		while ( sigwait( &stop, &sig ) != 0 ) {}
		cfalsp::log::info( "signal ", sig, ": stopping" );
		server.terminate();
		// Die of the signal, so the parent sees how the server ended.
		std::signal( sig, SIG_DFL );
		sigset_t one;
		sigemptyset( &one );
		sigaddset( &one, sig );
		pthread_sigmask( SIG_UNBLOCK, &one, nullptr );
		raise( sig );
	} ).detach();
	return server.run();
}
