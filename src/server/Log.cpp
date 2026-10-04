#include "Log.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace cfalsp::log {

namespace {
std::atomic<Level> current{ Level::Off };
std::mutex mtx;
}

void init() {
	const char * env = std::getenv( "CFA_LSP_LOG" );
	if ( ! env || ! *env ) { current = Level::Off; return; }
	std::string v( env );
	for ( auto & c : v ) c = (char)std::tolower( (unsigned char)c );
	if ( v == "off" || v == "0" || v == "none" ) current = Level::Off;
	else if ( v == "error" ) current = Level::Error;
	else if ( v == "warn" || v == "warning" ) current = Level::Warn;
	else if ( v == "debug" || v == "trace" ) current = Level::Debug;
	else current = Level::Info;
}

void setLevel( Level l ) { current = l; }

bool enabled( Level l ) {
	Level c = current;
	return c != Level::Off && l <= c;
}

void write( Level l, const std::string & msg ) {
	static const char * names[] = { "", "ERROR", "WARN", "INFO", "DEBUG" };
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t( now );
	int ms = (int)( std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ).count() % 1000 );
	std::tm tm;
	localtime_r( &t, &tm );
	char stamp[32];
	std::strftime( stamp, sizeof( stamp ), "%H:%M:%S", &tm );
	std::lock_guard<std::mutex> lock( mtx );
	std::fprintf( stderr, "[%s.%03d %s] %s\n", stamp, ms, names[(int)l], msg.c_str() );
	std::fflush( stderr );
}

} // namespace cfalsp::log
