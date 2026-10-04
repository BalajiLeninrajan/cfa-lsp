// A test client that runs the built cfa-lsp binary as a child process and
// talks LSP to it over pipes.
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>

#include <nlohmann/json.hpp>

#include "server/Transport.hpp"

namespace cfalsp::test {

using json = nlohmann::json;
using namespace std::chrono_literals;

std::string envOr( const char * name, const std::string & dflt = "" );
std::string readAll( const std::string & path );

// The server binary from CFA_LSP_SERVER, or "" if it isn't built.
std::string serverBinary();
// The forked translator from CFA_LSP_TRANSLATOR, or "" if it isn't built.
std::string translatorBinary();
bool haveCfa();

json lspPos( int line, int character );

// Line and UTF-16 column of the `nth` occurrence of `needle` in `text`,
// plus `offset` bytes. All fixtures are ASCII, so bytes equal UTF-16 units.
json posOf( const std::string & text, const std::string & needle, int offset = 0, int nth = 0 );

class LspClient {
  public:
	// Starts the server. Its stderr goes to `logPath` if given.
	explicit LspClient( const std::string & logPath = "" );
	~LspClient();
	LspClient( const LspClient & ) = delete;
	LspClient & operator=( const LspClient & ) = delete;

	json initialize( json initOptions, const std::string & rootPath = "" );
	void notify( const std::string & method, json params );
	// Sends a request and returns its whole response (result or error).
	json request( const std::string & method, json params, std::chrono::milliseconds timeout = 30s );
	json result( const std::string & method, json params ) { return request( method, std::move( params ) )["result"]; }

	// Waits for a notification with `method` whose params satisfy `pred`,
	// discarding nothing: unmatched messages stay queued.
	std::optional<json> waitFor( const std::string & method, std::function<bool( const json & )> pred,
								 std::chrono::milliseconds timeout );
	// The next publishDiagnostics for `uri` with version >= `minVersion`.
	std::optional<json> diagnostics( const std::string & uri, int minVersion, std::chrono::milliseconds timeout = 120s );
	// Drops queued publishDiagnostics for `uri`.
	void dropDiagnostics( const std::string & uri );

	// didOpen; the version is 1.
	void open( const std::string & uri, const std::string & text );
	// An incremental didChange replacing [start, end) with `text`.
	void change( const std::string & uri, int version, json start, json end, const std::string & text );

	int shutdown();							// shutdown + exit; returns the exit status
	// Sends `sig` to the server and waits for it to end; returns the wait status.
	int sendSignal( int sig );
	pid_t serverPid() const { return pid; }

  private:
	std::optional<json> readMessage( std::chrono::milliseconds timeout );
	void send( const json & msg );

	pid_t pid = -1;
	int toServer = -1, fromServer = -1;
	FrameParser parser;
	std::deque<json> queue;
	int nextId = 1;
};

} // namespace cfalsp::test
