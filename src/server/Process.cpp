#include "Process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "Log.hpp"

extern char ** environ;

namespace cfalsp {

std::string RunResult::describe() const {
	switch ( status ) {
	  case Exited: return "exited with status " + std::to_string( code );
	  case Signaled: return std::string( "killed by signal " ) + strsignal( code );
	  case TimedOut: return "timed out";
	  case Cancelled: return "cancelled";
	  case SpawnFailed: return "could not start: " + error;
	}
	return "?";
}

std::string findInPath( const std::string & name ) {
	if ( name.find( '/' ) != std::string::npos ) {
		return access( name.c_str(), X_OK ) == 0 ? name : "";
	}
	const char * path = std::getenv( "PATH" );
	if ( ! path ) return "";
	std::string p( path );
	size_t start = 0;
	while ( start <= p.size() ) {
		size_t end = p.find( ':', start );
		if ( end == std::string::npos ) end = p.size();
		std::string dir = p.substr( start, end - start );
		if ( dir.empty() ) dir = ".";
		std::string cand = dir + "/" + name;
		struct stat st;
		if ( stat( cand.c_str(), &st ) == 0 && S_ISREG( st.st_mode ) && access( cand.c_str(), X_OK ) == 0 ) return cand;
		start = end + 1;
	}
	return "";
}

RunResult runProcess( const std::vector<std::string> & argv, const RunOptions & opts, const CancelToken * cancel ) {
	RunResult res;
	if ( argv.empty() ) {
		res.error = "empty command";
		return res;
	}
	std::string exe = findInPath( argv[0] );
	if ( exe.empty() ) {
		res.error = argv[0] + ": not found";
		return res;
	}

	// Build everything before spawning; nothing is allocated in the child.
	std::vector<char *> args;
	for ( const auto & a : argv ) args.push_back( const_cast<char *>( a.c_str() ) );
	args.push_back( nullptr );

	std::map<std::string, std::string> envMap;
	for ( char ** e = environ; e && *e; e += 1 ) {
		std::string kv( *e );
		size_t eq = kv.find( '=' );
		if ( eq == std::string::npos ) continue;
		envMap[kv.substr( 0, eq )] = kv.substr( eq + 1 );
	}
	for ( const auto & [k, v] : opts.env ) envMap[k] = v;
	std::vector<std::string> envStore;
	for ( const auto & [k, v] : envMap ) envStore.push_back( k + "=" + v );
	std::vector<char *> envp;
	for ( auto & s : envStore ) envp.push_back( s.data() );
	envp.push_back( nullptr );

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init( &fa );
	posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
	posix_spawn_file_actions_addopen( &fa, 1, opts.stdoutPath.empty() ? "/dev/null" : opts.stdoutPath.c_str(),
									  O_WRONLY | O_CREAT | O_TRUNC, 0644 );
	posix_spawn_file_actions_addopen( &fa, 2, opts.stderrPath.empty() ? "/dev/null" : opts.stderrPath.c_str(),
									  O_WRONLY | O_CREAT | O_TRUNC, 0644 );
	posix_spawn_file_actions_addclosefrom_np( &fa, 3 );
	if ( ! opts.cwd.empty() ) posix_spawn_file_actions_addchdir_np( &fa, opts.cwd.c_str() );

	posix_spawnattr_t attr;
	posix_spawnattr_init( &attr );
	posix_spawnattr_setpgroup( &attr, 0 );
	sigset_t none, all;
	sigemptyset( &none );
	sigemptyset( &all );
	for ( int sig : { SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD } ) sigaddset( &all, sig );
	posix_spawnattr_setsigmask( &attr, &none );
	posix_spawnattr_setsigdefault( &attr, &all );
	posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF );

	pid_t pid = -1;
	int rc = posix_spawn( &pid, exe.c_str(), &fa, &attr, args.data(), envp.data() );
	posix_spawn_file_actions_destroy( &fa );
	posix_spawnattr_destroy( &attr );
	if ( rc != 0 ) {
		res.error = argv[0] + ": " + std::strerror( rc );
		return res;
	}
	log::debug( "spawned pid ", pid, ": ", [&] {
		std::string s;
		for ( const auto & a : argv ) s += ( s.empty() ? "" : " " ) + a;
		return s;
	}() );

	// The leader is only reaped (waitpid) after the group has been killed:
	// while it is a zombie its pid, which is the group id, can't be reused,
	// so kill( -pid ) can't hit an unrelated process.
	auto exited = [&]() {
		siginfo_t info{};
		for ( ;; ) {
			int rc = waitid( P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT );
			if ( rc == 0 ) return info.si_pid == pid;
			if ( errno != EINTR ) return true;
		}
	};
	auto reap = [&]() {
		int status = 0;
		while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR ) {}
		return status;
	};

	auto startTime = std::chrono::steady_clock::now();
	auto delay = std::chrono::milliseconds( 1 );
	for ( ;; ) {
		if ( exited() ) {
			kill( -pid, SIGKILL );				// strays left in the group
			int status = reap();
			if ( WIFEXITED( status ) ) {
				res.status = RunResult::Exited;
				res.code = WEXITSTATUS( status );
			} else {
				res.status = RunResult::Signaled;
				res.code = WIFSIGNALED( status ) ? WTERMSIG( status ) : 0;
			}
			break;
		}
		bool cancelled = cancel && cancel->cancelled();
		bool timedOut = opts.timeout.count() > 0 && std::chrono::steady_clock::now() - startTime > opts.timeout;
		if ( cancelled || timedOut ) {
			// SIGTERM first so the cfa driver and gcc delete their temp
			// files (cc1 keeps one in /tmp regardless of TMPDIR), then
			// SIGKILL whatever is left.
			kill( -pid, SIGTERM );
			auto termAt = std::chrono::steady_clock::now();
			while ( ! exited() && std::chrono::steady_clock::now() - termAt < std::chrono::milliseconds( 300 ) ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			kill( -pid, SIGKILL );
			reap();
			res.status = cancelled ? RunResult::Cancelled : RunResult::TimedOut;
			log::debug( "killed pid ", pid, cancelled ? " (cancelled)" : " (timed out)" );
			break;
		}
		std::this_thread::sleep_for( delay );
		if ( delay < std::chrono::milliseconds( 20 ) ) delay *= 2;
	}
	return res;
}

} // namespace cfalsp
