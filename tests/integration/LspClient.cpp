#include "LspClient.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "server/Process.hpp"

extern char ** environ;

namespace cfalsp::test {

std::string envOr( const char * name, const std::string & dflt ) {
	const char * v = std::getenv( name );
	return v && *v ? v : dflt;
}

std::string readAll( const std::string & path ) {
	std::ifstream in( path, std::ios::binary );
	if ( ! in ) throw std::runtime_error( "cannot read " + path );
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static std::string executable( const char * var ) {
	std::string p = envOr( var );
	return ! p.empty() && access( p.c_str(), X_OK ) == 0 ? p : "";
}

std::string serverBinary() { return executable( "CFA_LSP_SERVER" ); }
std::string translatorBinary() { return executable( "CFA_LSP_TRANSLATOR" ); }
bool haveCfa() { return ! findInPath( "cfa" ).empty(); }

json lspPos( int line, int character ) { return { { "line", line }, { "character", character } }; }

json posOf( const std::string & text, const std::string & needle, int offset, int nth ) {
	size_t at = text.find( needle );
	for ( int i = 0; i < nth && at != std::string::npos; i += 1 ) at = text.find( needle, at + 1 );
	if ( at == std::string::npos ) throw std::runtime_error( "fixture text has no '" + needle + "'" );
	at += offset;
	int line = 0;
	size_t bol = 0;
	for ( size_t i = 0; i < at; i += 1 ) {
		if ( text[i] == '\n' ) {
			line += 1;
			bol = i + 1;
		}
	}
	return lspPos( line, int( at - bol ) );
}

LspClient::LspClient( const std::string & logPath ) {
	std::string bin = serverBinary();
	if ( bin.empty() ) throw std::runtime_error( "CFA_LSP_SERVER is not set to a built server" );
	int in[2], out[2];
	if ( pipe2( in, O_CLOEXEC ) != 0 || pipe2( out, O_CLOEXEC ) != 0 ) throw std::runtime_error( "pipe failed" );
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init( &fa );
	posix_spawn_file_actions_adddup2( &fa, in[0], 0 );
	posix_spawn_file_actions_adddup2( &fa, out[1], 1 );
	if ( ! logPath.empty() ) {
		posix_spawn_file_actions_addopen( &fa, 2, logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644 );
	}
	std::vector<char *> argv{ const_cast<char *>( bin.c_str() ), nullptr };
	int rc = posix_spawn( &pid, bin.c_str(), &fa, nullptr, argv.data(), environ );
	posix_spawn_file_actions_destroy( &fa );
	close( in[0] );
	close( out[1] );
	if ( rc != 0 ) {
		close( in[1] );
		close( out[0] );
		throw std::runtime_error( "cannot start " + bin );
	}
	toServer = in[1];
	fromServer = out[0];
	signal( SIGPIPE, SIG_IGN );
}

LspClient::~LspClient() {
	if ( toServer >= 0 ) close( toServer );
	if ( pid > 0 ) {
		// Give it a moment to exit on end of input, then make sure.
		for ( int i = 0; i < 100; i += 1 ) {
			if ( waitpid( pid, nullptr, WNOHANG ) == pid ) {
				pid = -1;
				break;
			}
			usleep( 20000 );
		}
		if ( pid > 0 ) {
			kill( pid, SIGKILL );
			waitpid( pid, nullptr, 0 );
		}
	}
	if ( fromServer >= 0 ) close( fromServer );
}

void LspClient::send( const json & msg ) {
	std::string f = frame( msg.dump() );
	size_t done = 0;
	while ( done < f.size() ) {
		ssize_t n = write( toServer, f.data() + done, f.size() - done );
		if ( n < 0 && errno == EINTR ) continue;
		if ( n <= 0 ) throw std::runtime_error( "server closed its input" );
		done += size_t( n );
	}
}

std::optional<json> LspClient::readMessage( std::chrono::milliseconds timeout ) {
	auto deadline = std::chrono::steady_clock::now() + timeout;
	for ( ;; ) {
		if ( auto body = parser.next() ) return json::parse( *body );
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
		if ( left.count() <= 0 ) return std::nullopt;
		pollfd p{ fromServer, POLLIN, 0 };
		int r = poll( &p, 1, int( left.count() ) );
		if ( r < 0 && errno == EINTR ) continue;
		if ( r <= 0 ) continue;
		char buf[65536];
		ssize_t n = read( fromServer, buf, sizeof( buf ) );
		if ( n <= 0 ) return std::nullopt;
		parser.feed( buf, size_t( n ) );
	}
}

json LspClient::initialize( json initOptions, const std::string & rootPath ) {
	json caps = { { "general", { { "positionEncodings", { "utf-16" } } } },
				  { "textDocument", { { "documentSymbol", { { "hierarchicalDocumentSymbolSupport", true } } } } } };
	json params = { { "processId", nullptr }, { "capabilities", caps }, { "initializationOptions", initOptions } };
	params["rootUri"] = rootPath.empty() ? json( nullptr ) : json( "file://" + rootPath );
	json r = request( "initialize", params );
	notify( "initialized", json::object() );
	return r;
}

void LspClient::notify( const std::string & method, json params ) {
	send( { { "jsonrpc", "2.0" }, { "method", method }, { "params", std::move( params ) } } );
}

json LspClient::request( const std::string & method, json params, std::chrono::milliseconds timeout ) {
	int id = nextId++;
	send( { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move( params ) } } );
	auto deadline = std::chrono::steady_clock::now() + timeout;
	for ( ;; ) {
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
		auto m = readMessage( std::max( left, 0ms ) );
		if ( ! m ) throw std::runtime_error( "no response to " + method );
		if ( m->contains( "id" ) && ( *m )["id"] == id && ! m->contains( "method" ) ) return *m;
		queue.push_back( std::move( *m ) );
	}
}

std::optional<json> LspClient::waitFor( const std::string & method, std::function<bool( const json & )> pred,
										std::chrono::milliseconds timeout ) {
	auto deadline = std::chrono::steady_clock::now() + timeout;
	for ( ;; ) {
		for ( auto it = queue.begin(); it != queue.end(); ++it ) {
			if ( it->value( "method", "" ) == method && pred( ( *it )["params"] ) ) {
				json p = ( *it )["params"];
				queue.erase( it );
				return p;
			}
		}
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
		if ( left.count() <= 0 ) return std::nullopt;
		auto m = readMessage( left );
		if ( ! m ) return std::nullopt;
		queue.push_back( std::move( *m ) );
	}
}

std::optional<json> LspClient::diagnostics( const std::string & uri, int minVersion, std::chrono::milliseconds timeout ) {
	return waitFor( "textDocument/publishDiagnostics",
					[&]( const json & p ) { return p["uri"] == uri && p.value( "version", -1 ) >= minVersion; }, timeout );
}

void LspClient::dropDiagnostics( const std::string & uri ) {
	std::erase_if( queue, [&]( const json & m ) {
		return m.value( "method", "" ) == "textDocument/publishDiagnostics" && m["params"]["uri"] == uri;
	} );
}

void LspClient::open( const std::string & uri, const std::string & text ) {
	notify( "textDocument/didOpen",
			{ { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", text } } } } );
}

void LspClient::change( const std::string & uri, int version, json start, json end, const std::string & text ) {
	notify( "textDocument/didChange",
			{ { "textDocument", { { "uri", uri }, { "version", version } } },
			  { "contentChanges", { { { "range", { { "start", start }, { "end", end } } }, { "text", text } } } } } );
}

int LspClient::shutdown() {
	request( "shutdown", nullptr );
	notify( "exit", nullptr );
	close( toServer );
	toServer = -1;
	int status = 0;
	if ( waitpid( pid, &status, 0 ) != pid ) return -1;
	pid = -1;
	return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
}

} // namespace cfalsp::test

namespace cfalsp::test {

int LspClient::sendSignal( int sig ) {
	if ( pid <= 0 ) return -1;
	kill( pid, sig );
	int status = 0;
	if ( waitpid( pid, &status, 0 ) != pid ) return -1;
	pid = -1;
	return status;
}

} // namespace cfalsp::test
