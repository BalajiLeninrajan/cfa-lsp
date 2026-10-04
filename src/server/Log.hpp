#pragma once

#include <sstream>
#include <string>

namespace cfalsp::log {

// Logging goes to stderr only, and only when CFA_LSP_LOG is set:
// CFA_LSP_LOG=error|warn|info|debug (any other non-empty value means info).
enum class Level { Off, Error, Warn, Info, Debug };

void init();							// reads CFA_LSP_LOG
void setLevel( Level );
bool enabled( Level );
void write( Level, const std::string & msg );

template<typename... Args>
void print( Level lvl, const Args &... args ) {
	if ( ! enabled( lvl ) ) return;
	std::ostringstream os;
	( os << ... << args );
	write( lvl, os.str() );
}

template<typename... Args> void error( const Args &... args ) { print( Level::Error, args... ); }
template<typename... Args> void warn( const Args &... args ) { print( Level::Warn, args... ); }
template<typename... Args> void info( const Args &... args ) { print( Level::Info, args... ); }
template<typename... Args> void debug( const Args &... args ) { print( Level::Debug, args... ); }

} // namespace cfalsp::log
