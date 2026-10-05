// LSP sessions against an in-process Server, with the fake cfa/cfa-cpp/gcc
// (tests/server/fake/fake_cfa.cpp) standing in for the toolchain.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <poll.h>
#include <future>
#include <sstream>
#include <string_view>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"
#include "server/Checker.hpp"
#include "server/Server.hpp"
#include "server/Uri.hpp"

using namespace cfalsp;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string envOr( const char * name, const std::string & dflt = "" ) {
	const char * v = std::getenv( name );
	return v && *v ? v : dflt;
}

std::string fakeCfa() {
	std::string p = envOr( "CFA_LSP_FAKE_CFA" );
	return ! p.empty() && access( p.c_str(), X_OK ) == 0 ? p : "";
}

std::string fixtures() { return envOr( "CFA_LSP_FIXTURES", "tests/fixtures" ); }

std::string readAll( const std::string & p ) {
	std::ifstream in( p, std::ios::binary );
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// The real Analysis has a non-empty token legend; the stub has none.
bool realAnalysis() { return ! Analysis::tokenTypes().empty(); }

class Session {
  public:
	explicit Session( bool start = true ) {
		REQUIRE( pipe2( toServer, O_CLOEXEC ) == 0 );
		REQUIRE( pipe2( fromServer, O_CLOEXEC ) == 0 );
		server = std::make_unique<Server>( toServer[0], fromServer[1], "" );
		if ( start ) run();
	}
	void run() { thread = std::thread( [this] { exitCode = server->run(); } ); }
	Server & srv() { return *server; }
	~Session() {
		if ( toServer[1] >= 0 ) close( toServer[1] );
		if ( thread.joinable() ) thread.join();
		server.reset();
		close( toServer[0] );
		close( fromServer[0] );
		close( fromServer[1] );
	}

	void send( const json & msg ) { sendAll( { msg } ); }

	// In one write, so the server reads them all at once.
	void sendAll( const std::vector<json> & msgs ) {
		std::string f;
		for ( const json & msg : msgs ) {
			std::string body = msg.dump();
			f += "Content-Length: " + std::to_string( body.size() ) + "\r\n\r\n" + body;
		}
		REQUIRE( write( toServer[1], f.data(), f.size() ) == (ssize_t)f.size() );
	}

	void notify( const std::string & method, json params ) {
		send( { { "jsonrpc", "2.0" }, { "method", method }, { "params", std::move( params ) } } );
	}

	// Sends a request and waits for its response; notifications that
	// arrive meanwhile are queued.
	json request( const std::string & method, json params = json::object() ) {
		int id = nextId++;
		send( { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move( params ) } } );
		return response( id );
	}

	// Waits for the response to request `id`.
	json response( const json & id ) {
		for ( auto it = queue.begin(); it != queue.end(); ++it ) {
			if ( it->contains( "id" ) && ( *it )["id"] == id && ! it->contains( "method" ) ) {
				json m = *it;
				queue.erase( it );
				return m;
			}
		}
		for ( ;; ) {
			auto m = readMessage( std::chrono::seconds( 10 ) );
			REQUIRE_MESSAGE( m, "no response to request " << id );
			if ( m->contains( "id" ) && ( *m )["id"] == id && ! m->contains( "method" ) ) return *m;
			queue.push_back( *m );
		}
	}

	// Waits for a request from the server.
	std::optional<json> serverRequest( const std::string & method, std::chrono::milliseconds timeout = std::chrono::seconds( 10 ) ) {
		auto deadline = std::chrono::steady_clock::now() + timeout;
		for ( ;; ) {
			for ( auto it = queue.begin(); it != queue.end(); ++it ) {
				if ( it->value( "method", "" ) == method && it->contains( "id" ) ) {
					json m = *it;
					queue.erase( it );
					return m;
				}
			}
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
			if ( left.count() <= 0 ) return std::nullopt;
			auto m = readMessage( left );
			if ( ! m ) return std::nullopt;
			queue.push_back( *m );
		}
	}

	// Waits for a notification matching `pred`.
	std::optional<json> waitFor( const std::string & method, std::function<bool( const json & )> pred,
								 std::chrono::milliseconds timeout = std::chrono::seconds( 10 ) ) {
		auto deadline = std::chrono::steady_clock::now() + timeout;
		for ( ;; ) {
			for ( auto it = queue.begin(); it != queue.end(); ++it ) {
				if ( it->value( "method", "" ) == method && pred( ( *it )["params"] ) ) {
					json m = *it;
					queue.erase( it );
					return m["params"];
				}
			}
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
			if ( left.count() <= 0 ) return std::nullopt;
			auto m = readMessage( left );
			if ( ! m ) return std::nullopt;
			queue.push_back( *m );
		}
	}

	std::optional<json> diagnosticsFor( const std::string & uri, std::function<bool( const json & )> pred,
										std::chrono::milliseconds timeout = std::chrono::seconds( 10 ) ) {
		return waitFor( "textDocument/publishDiagnostics",
						[&]( const json & p ) { return p["uri"] == uri && pred( p["diagnostics"] ); }, timeout );
	}

	json initialize( json initOptions, json capabilities = json::object() ) {
		json r = request( "initialize", { { "processId", nullptr }, { "rootUri", nullptr },
										  { "capabilities", capabilities }, { "initializationOptions", initOptions } } );
		notify( "initialized", json::object() );
		return r;
	}

	int finish() {
		close( toServer[1] );
		toServer[1] = -1;
		thread.join();
		return exitCode;
	}

	std::deque<json> queue;
	int exitCode = -1;

  private:
	std::optional<json> readMessage( std::chrono::milliseconds timeout ) {
		auto deadline = std::chrono::steady_clock::now() + timeout;
		for ( ;; ) {
			if ( auto body = parser.next() ) return json::parse( *body );
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() );
			if ( left.count() <= 0 ) return std::nullopt;
			pollfd p{ fromServer[0], POLLIN, 0 };
			if ( poll( &p, 1, (int)left.count() ) <= 0 ) continue;
			char buf[65536];
			ssize_t n = read( fromServer[0], buf, sizeof( buf ) );
			if ( n <= 0 ) return std::nullopt;
			parser.feed( buf, (size_t)n );
		}
	}

	int toServer[2], fromServer[2];
	std::unique_ptr<Server> server;
	std::thread thread;
	FrameParser parser;
	int nextId = 1;
};

json pos( int line, int ch ) { return { { "line", line }, { "character", ch } }; }

json fakeOptions( json extra = json::object() ) {
	json o = { { "cfa", fakeCfa() }, { "translator", fakeCfa() }, { "cc", fakeCfa() }, { "debounceMs", 20 },
			   { "preludeDir", "/nonexistent-prelude" }, { "flags", { "-Wall" } } };
	for ( auto & [k, v] : extra.items() ) o[k] = v;
	return o;
}

// Points the fake at the canned files and keeps temp dirs in a scratch dir.
struct FakeEnv {
	TempDir scratch;
	std::string oldTmp = envOr( "TMPDIR" );
	FakeEnv() {
		setenv( "FAKE_CFA_DIR", ( fixtures() + "/server" ).c_str(), 1 );
		setenv( "FAKE_CFA_LOG", ( scratch.path() + "/log" ).c_str(), 1 );
		setenv( "FAKE_CFA_PIDS", ( scratch.path() + "/pids" ).c_str(), 1 );
		fs::create_directories( scratch.path() + "/tmp" );
		setenv( "TMPDIR", ( scratch.path() + "/tmp" ).c_str(), 1 );
	}
	~FakeEnv() {
		if ( oldTmp.empty() ) unsetenv( "TMPDIR" );
		else setenv( "TMPDIR", oldTmp.c_str(), 1 );
	}
	int tempDirsLeft() const {
		int n = 0;
		for ( auto & e : fs::directory_iterator( scratch.path() + "/tmp" ) ) {
			(void)e;
			n += 1;
		}
		return n;
	}
};

bool processGone( pid_t pid ) {
	std::ifstream stat( "/proc/" + std::to_string( pid ) + "/stat" );
	if ( ! stat ) return true;
	std::string s;
	std::getline( stat, s );
	size_t rp = s.rfind( ')' );
	return rp != std::string::npos && rp + 2 < s.size() && s[rp + 2] == 'Z';	// zombie of someone else's
}

} // namespace

TEST_CASE( "lifecycle" ) {
	Session s;
	json r = s.request( "textDocument/hover", { { "textDocument", { { "uri", "file:///x.cfa" } } }, { "position", pos( 0, 0 ) } } );
	CHECK( r["error"]["code"] == -32002 );

	r = s.initialize( { { "cfa", "/nonexistent/cfa" }, { "translator", "/nonexistent/cfa-cpp" } },
					  { { "general", { { "positionEncodings", { "utf-16", "utf-8" } } } } } );
	json caps = r["result"]["capabilities"];
	CHECK( caps["positionEncoding"] == "utf-8" );
	CHECK( caps["textDocumentSync"]["change"] == 2 );
	CHECK( caps["textDocumentSync"]["openClose"] == true );
	CHECK( caps["hoverProvider"] == true );
	CHECK( caps["definitionProvider"] == true );
	CHECK( caps["declarationProvider"] == true );
	CHECK( caps["referencesProvider"] == true );
	CHECK( caps["documentSymbolProvider"] == true );
	CHECK( caps["completionProvider"]["triggerCharacters"] == json{ ".", ">" } );
	CHECK( caps["signatureHelpProvider"]["triggerCharacters"] == json{ "(", "," } );
	CHECK( caps["semanticTokensProvider"]["legend"]["tokenTypes"] == json( Analysis::tokenTypes() ) );
	CHECK( caps["semanticTokensProvider"]["full"] == true );
	CHECK( caps["documentHighlightProvider"] == true );
	CHECK( caps["inlayHintProvider"] == true );
	CHECK( caps["renameProvider"] == true );				// the client didn't announce prepareSupport
	CHECK( caps["workspaceSymbolProvider"] == true );

	auto msg = s.waitFor( "window/showMessage", []( const json & ) { return true; } );
	REQUIRE( msg );
	CHECK( ( *msg )["message"].get<std::string>().find( "cfa" ) != std::string::npos );

	CHECK( s.request( "initialize", json::object() )["error"]["code"] == -32600 );
	CHECK( s.request( "textDocument/frobnicate", json::object() )["error"]["code"] == -32601 );
	s.notify( "$/cancelRequest", { { "id", 99 } } );
	s.notify( "$/somethingElse", json::object() );

	// No analysis yet: empty answers, not errors.
	json td = { { "uri", "file:///nowhere/x.cfa" } };
	CHECK( s.request( "textDocument/hover", { { "textDocument", td }, { "position", pos( 0, 0 ) } } )["result"].is_null() );
	CHECK( s.request( "textDocument/definition", { { "textDocument", td }, { "position", pos( 0, 0 ) } } )["result"] == json::array() );
	CHECK( s.request( "textDocument/documentSymbol", { { "textDocument", td } } )["result"] == json::array() );
	CHECK( s.request( "textDocument/semanticTokens/full", { { "textDocument", td } } )["result"]["data"] == json::array() );
	CHECK( s.request( "textDocument/completion", { { "textDocument", td }, { "position", pos( 0, 0 ) } } )["result"]["items"] == json::array() );
	CHECK( s.request( "textDocument/documentHighlight", { { "textDocument", td }, { "position", pos( 0, 0 ) } } )["result"] == json::array() );
	CHECK( s.request( "textDocument/inlayHint", { { "textDocument", td }, { "range", { { "start", pos( 0, 0 ) }, { "end", pos( 9, 0 ) } } } } )["result"] == json::array() );
	CHECK( s.request( "textDocument/prepareRename", { { "textDocument", td }, { "position", pos( 0, 0 ) } } )["result"].is_null() );
	CHECK( s.request( "textDocument/rename", { { "textDocument", td }, { "position", pos( 0, 0 ) }, { "newName", "y" } } )["error"]["code"] == -32803 );
	CHECK( s.request( "textDocument/rename", { { "textDocument", td }, { "position", pos( 0, 0 ) }, { "newName", "for" } } )["error"]["code"] == -32602 );
	CHECK( s.request( "textDocument/rename", { { "textDocument", td }, { "position", pos( 0, 0 ) }, { "newName", "1x" } } )["error"]["code"] == -32602 );
	CHECK( s.request( "workspace/symbol", { { "query", "" } } )["result"] == json::array() );
	CHECK( s.request( "textDocument/switchSourceHeader", td )["result"].is_null() );

	CHECK( s.request( "shutdown" )["result"].is_null() );
	CHECK( s.request( "textDocument/hover", json::object() )["error"]["code"] == -32600 );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
}

TEST_CASE( "switchSourceHeader pairs .cfa and .hfa files with the same stem" ) {
	Session s;
	s.initialize( { { "cfa", "/nonexistent/cfa" } } );
	std::string dir = fs::canonical( fixtures() + "/server" ).string();
	auto other = [&]( const std::string & path ) {
		return s.request( "textDocument/switchSourceHeader", { { "uri", pathToUri( path ) } } )["result"];
	};
	CHECK( other( dir + "/hello.cfa" ) == pathToUri( dir + "/hello.hfa" ) );
	CHECK( other( dir + "/hello.hfa" ) == pathToUri( dir + "/hello.cfa" ) );
	CHECK( other( dir + "/gcc.err" ).is_null() );
	CHECK( other( dir + "/missing.cfa" ).is_null() );
	// A header in another directory counts when it is open.
	fs::path tmp = fs::temp_directory_path() / ( "cfa-lsp-switch-" + std::to_string( getpid() ) );
	fs::create_directories( tmp / "include" );
	std::string header = ( tmp / "include" / "lone.hfa" ).string();
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", pathToUri( header ) }, { "languageId", "cfa" }, { "version", 1 }, { "text", "int x;\n" } } } } );
	CHECK( other( ( tmp / "lone.cfa" ).string() ) == pathToUri( header ) );
	fs::remove_all( tmp );
	CHECK( s.request( "shutdown" )["result"].is_null() );
}

TEST_CASE( "prepareRename when the client supports it; inlay hint refresh after a check" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	json open = { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", readAll( path ) } } } };
	auto refreshes = []( const Session & s ) {
		int n = 0;
		for ( const json & m : s.queue ) {
			if ( m.value( "method", "" ) == "workspace/inlayHint/refresh" ) n += 1;
		}
		return n;
	};
	{
		Session s;
		json caps = { { "textDocument", { { "rename", { { "prepareSupport", true } } } } },
					  { "workspace", { { "inlayHint", { { "refreshSupport", true } } } } } };
		json r = s.initialize( fakeOptions( { { "backend", false } } ), caps );
		CHECK( r["result"]["capabilities"]["renameProvider"] == json{ { "prepareProvider", true } } );
		s.notify( "textDocument/didOpen", open );
		REQUIRE( s.diagnosticsFor( uri, []( const json & ) { return true; } ) );
		// Sent before the diagnostics, so it is queued by now.
		CHECK( refreshes( s ) == 1 );
		for ( const json & m : s.queue ) {
			if ( m.value( "method", "" ) == "workspace/inlayHint/refresh" ) {
				CHECK( m.contains( "id" ) );
			}
		}
		// The client's answer is not a request.
		s.send( { { "jsonrpc", "2.0" }, { "id", "cfa-lsp-refresh-1" }, { "result", nullptr } } );
		CHECK( s.request( "shutdown" )["result"].is_null() );
	}
	{
		Session s;
		s.initialize( fakeOptions( { { "backend", false } } ) );
		s.notify( "textDocument/didOpen", open );
		REQUIRE( s.diagnosticsFor( uri, []( const json & ) { return true; } ) );
		CHECK( s.request( "shutdown" )["result"].is_null() );
		CHECK( refreshes( s ) == 0 );
	}
}

TEST_CASE( "exit without shutdown, and garbage input" ) {
	Session s;
	s.send( "not json" );
	s.initialize( { { "cfa", "/nonexistent/cfa" } } );
	CHECK( s.request( "textDocument/hover", json::object() )["result"].is_null() );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 1 );
}

TEST_CASE( "end of input without shutdown exits with 1" ) {
	Session s;
	s.initialize( { { "cfa", "/nonexistent/cfa" } } );
	CHECK( s.finish() == 1 );
}

TEST_CASE( "checks publish translator, preprocessor and backend diagnostics" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string header = fs::canonical( fixtures() + "/server/hello.hfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path );

	{
		Session s;
		s.initialize( fakeOptions() );
		s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", text } } } } );

		// The backend warning comes back through out.c's line markers,
		// demangled, once, on the whole of line 4.
		auto d = s.diagnosticsFor( uri, []( const json & ds ) { return ! ds.empty(); } );
		REQUIRE( d );
		json ds = ( *d )["diagnostics"];
		REQUIRE( ds.size() == 1 );
		CHECK( ds[0]["message"] == "unused variable 'unused'" );
		CHECK( ds[0]["source"] == "gcc" );
		CHECK( ds[0]["code"] == "-Wunused-variable" );
		CHECK( ds[0]["severity"] == 2 );
		CHECK( ds[0]["range"] == json{ { "start", pos( 3, 1 ) }, { "end", pos( 3, 12 ) } } );
		CHECK( ( *d )["version"] == 1 );

		// The translator got what the driver would pass, plus the LSP flags.
		std::string log = readAll( env.scratch.path() + "/log" );
		CHECK( log.find( "-E " ) != std::string::npos );
		CHECK( log.find( "--lsp " ) != std::string::npos );
		CHECK( log.find( "--lsp-focus " + path ) != std::string::npos );
		CHECK( log.find( "--prelude-dir=/nonexistent-prelude" ) != std::string::npos );
		CHECK( log.find( "-Wp,-iquote" + fs::path( path ).parent_path().string() + " " ) != std::string::npos );
		CHECK( log.find( "-fsyntax-only" ) != std::string::npos );

		if ( realAnalysis() ) {
			json td = { { "uri", uri } };
			// hover on the call to twice (line 9, col 9)
			json h = s.request( "textDocument/hover", { { "textDocument", td }, { "position", pos( 8, 10 ) } } )["result"];
			CHECK( ! h.is_null() );
			if ( ! h.is_null() ) CHECK( h["contents"]["value"].get<std::string>().find( "twice" ) != std::string::npos );
			json def = s.request( "textDocument/definition", { { "textDocument", td }, { "position", pos( 8, 10 ) } } )["result"];
			CHECK( def.is_array() );
			CHECK( ! def.empty() );
			json toks = s.request( "textDocument/semanticTokens/full", { { "textDocument", td } } )["result"]["data"];
			CHECK( toks.size() % 5 == 0 );
			CHECK( ! toks.empty() );
			json syms = s.request( "textDocument/documentSymbol", { { "textDocument", td } } )["result"];
			CHECK( ! syms.empty() );
		}

		// Errors in an included project header: published on the header,
		// and summarised on the #include line of the main file. The errors
		// stop translation before code generation, so the backend can't run;
		// its warning from the last check stays.
		std::string headerErr = text + "// FAKE_HEADER_ERROR\n";
		s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 2 } } },
											  { "contentChanges", { { { "text", headerErr } } } } } );
		if ( realAnalysis() ) {
			auto h = s.diagnosticsFor( pathToUri( header ), []( const json & ds ) { return ds.size() == 1; } );
			REQUIRE( h );
			CHECK( ( *h )["diagnostics"][0]["message"] == "fake error in header" );
			// An error in a system header goes on that header too; its warning doesn't.
			auto sys = s.diagnosticsFor( "file:///nonexistent-prelude/sys.hfa", []( const json & ds ) { return ! ds.empty(); } );
			REQUIRE( sys );
			REQUIRE( ( *sys )["diagnostics"].size() == 1 );
			CHECK( ( *sys )["diagnostics"][0]["message"] == "fake error in a system header" );
			d = s.diagnosticsFor( uri, []( const json & ds ) { return ds.size() == 3; } );
			REQUIRE( d );
			bool summary = false, carried = false;
			for ( const auto & x : ( *d )["diagnostics"] ) {
				if ( x["message"].get<std::string>().find( "1 error in included file hello.hfa" ) != std::string::npos ) {
					summary = true;
					CHECK( x["range"]["start"]["line"] == 0 );
					CHECK( x["relatedInformation"][0]["location"]["uri"] == pathToUri( header ) );
				}
				if ( x["source"] == "gcc" ) {
					carried = true;
					CHECK( x["range"]["start"] == pos( 3, 1 ) );
				}
			}
			CHECK( summary );
			CHECK( carried );
			// That check came back incomplete with no declarations, so the
			// previous analysis still answers queries.
			json hv = s.request( "textDocument/hover", { { "textDocument", { { "uri", uri } } }, { "position", pos( 8, 10 ) } } )["result"];
			CHECK( ! hv.is_null() );

			// Insert a line at the top: results move down by one without
			// waiting for a check.
			s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 3 } } },
					  { "contentChanges", { { { "range", { { "start", pos( 0, 0 ) }, { "end", pos( 0, 0 ) } } }, { "text", "// new\n" } } } } } );
			json refs = s.request( "textDocument/references", { { "textDocument", { { "uri", uri } } }, { "position", pos( 10, 8 ) },
																{ "context", { { "includeDeclaration", true } } } } )["result"];
			bool sawUse = false;
			for ( const auto & r : refs ) {
				if ( r["range"]["start"] == pos( 10, 8 ) ) sawUse = true;
			}
			CHECK( sawUse );
		} else {
			d = s.diagnosticsFor( uri, []( const json & ds ) { return ds.empty(); } );
			CHECK( d );
		}

		// A preprocessor failure is reported on its line, and the old
		// analysis survives. With the real analysis the text is now
		// "// new\n" + text + "// FAKE_HEADER_ERROR\n"; without it, text + the comment.
		// The backend warning moved down with its line.
		int endLine = realAnalysis() ? 13 : 12;
		s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 4 } } },
				  { "contentChanges", { { { "range", { { "start", pos( endLine, 0 ) }, { "end", pos( endLine, 0 ) } } }, { "text", "FAKE_CPP_ERROR\n" } } } } } );
		d = s.diagnosticsFor( uri, []( const json & ds ) {
			return ! ds.empty() && ds[0]["message"].get<std::string>().find( "FAKE_CPP_ERROR" ) != std::string::npos;
		} );
		REQUIRE( d );
		CHECK( ( *d )["diagnostics"][0]["range"]["start"]["line"] == endLine );
		CHECK( ( *d )["diagnostics"][0]["severity"] == 1 );
		CHECK( ( *d )["version"] == 4 );
		if ( realAnalysis() ) {
			REQUIRE( ( *d )["diagnostics"].size() == 2 );
			CHECK( ( *d )["diagnostics"][1]["source"] == "gcc" );
			CHECK( ( *d )["diagnostics"][1]["range"]["start"] == pos( 4, 1 ) );
		}
		if ( realAnalysis() ) {
			// the call to twice, one line lower than in the snapshot
			json hv = s.request( "textDocument/hover", { { "textDocument", { { "uri", uri } } }, { "position", pos( 9, 10 ) } } )["result"];
			CHECK( ! hv.is_null() );
		}

		// Closing clears the document's diagnostics.
		s.notify( "textDocument/didClose", { { "textDocument", { { "uri", uri } } } } );
		CHECK( s.diagnosticsFor( uri, []( const json & ds ) { return ds.empty(); } ) );
		s.request( "shutdown" );
		s.notify( "exit", nullptr );
		CHECK( s.finish() == 0 );
	}
	CHECK( env.tempDirsLeft() == 0 );
}

TEST_CASE( "a newer edit cancels the running check and kills its processes" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path );

	Session s;
	s.initialize( fakeOptions( { { "backend", false } } ) );
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 },
															{ "text", "// FAKE_SLOW\n" + text } } } } );
	std::string pidFile = env.scratch.path() + "/pids";
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
	while ( ! fs::exists( pidFile ) && std::chrono::steady_clock::now() < deadline ) {
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
	}
	REQUIRE( fs::exists( pidFile ) );
	pid_t child = 0, grandchild = 0;
	std::ifstream( pidFile ) >> child >> grandchild;
	REQUIRE( child > 0 );
	REQUIRE( grandchild > 0 );

	// Requests are answered while the check runs.
	auto t0 = std::chrono::steady_clock::now();
	s.request( "textDocument/hover", { { "textDocument", { { "uri", uri } } }, { "position", pos( 0, 0 ) } } );
	CHECK( std::chrono::steady_clock::now() - t0 < std::chrono::seconds( 1 ) );

	s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 2 } } },
										  { "contentChanges", { { { "range", { { "start", pos( 0, 0 ) }, { "end", pos( 1, 0 ) } } }, { "text", "" } } } } } );
	// The replacement check finishes quickly and publishes.
	auto d = s.diagnosticsFor( uri, []( const json & ) { return true; }, std::chrono::seconds( 10 ) );
	REQUIRE( d );
	CHECK( ( *d )["version"] == 2 );
	CHECK( processGone( child ) );
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( ! processGone( grandchild ) && std::chrono::steady_clock::now() < deadline ) {
		std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
	}
	CHECK( processGone( grandchild ) );
	if ( ! processGone( grandchild ) ) kill( grandchild, SIGKILL );
	s.request( "shutdown" );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
	CHECK( env.tempDirsLeft() == 0 );
}

TEST_CASE( "stopAfterResolve passes the flag and skips the backend" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	Session s;
	s.initialize( fakeOptions( { { "stopAfterResolve", true } } ) );
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 },
															{ "text", readAll( path ) } } } } );
	// No backend, so no gcc warning.
	auto d = s.diagnosticsFor( uri, []( const json & ) { return true; } );
	REQUIRE( d );
	CHECK( ( *d )["diagnostics"] == json::array() );
	s.request( "shutdown" );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
	std::string log = readAll( env.scratch.path() + "/log" );
	CHECK( log.find( "--lsp-stop-after-resolve" ) != std::string::npos );
	CHECK( log.find( "--lsp-c-out" ) == std::string::npos );
	CHECK( log.find( "-fsyntax-only" ) == std::string::npos );
}

TEST_CASE( "UTF-16 positions in edits" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path );
	Session s;
	s.initialize( fakeOptions( { { "backend", true } } ) );
	// Put an emoji (two UTF-16 units, four bytes) before "int unused;" and
	// check the backend diagnostic's range is reported in UTF-16 units.
	std::string withEmoji = text;
	withEmoji.replace( withEmoji.find( "\tint unused;" ), 1, "\t\xF0\x9F\x98\x80" );
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", withEmoji } } } } );
	auto d = s.diagnosticsFor( uri, []( const json & ds ) { return ! ds.empty(); } );
	REQUIRE( d );
	CHECK( ( *d )["diagnostics"][0]["range"] == json{ { "start", pos( 3, 1 ) }, { "end", pos( 3, 14 ) } } );

	// An incremental edit in UTF-16 units: replace the emoji (columns 1..3)
	// with "x". The server must turn that into the right byte range.
	s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 2 } } },
										  { "contentChanges", { { { "range", { { "start", pos( 3, 1 ) }, { "end", pos( 3, 3 ) } } }, { "text", "x" } } } } } );
	d = s.diagnosticsFor( uri, []( const json & ds ) { return ! ds.empty(); } );
	REQUIRE( d );
	CHECK( ( *d )["version"] == 2 );
	CHECK( ( *d )["diagnostics"][0]["range"] == json{ { "start", pos( 3, 1 ) }, { "end", pos( 3, 13 ) } } );
	s.request( "shutdown" );
	s.notify( "exit", nullptr );
	s.finish();
}

TEST_CASE( "a publish built before didClose goes out before the close's empty one" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path ) + "// FAKE_HEADER_ERROR\n";	// a check with diagnostics
	// Hold the worker between building its publish and sending it, and close
	// the document meanwhile. The close's empty publish must still go last:
	// it waits for the worker's. If it didn't, it would be sent while the
	// worker waits here, and the worker's would follow it.
	std::promise<void> paused, closed;
	std::atomic<bool> firstPublish{ true }, firstClose{ true };
	std::shared_future<void> closedSent = closed.get_future().share();
	Session s( false );
	s.srv().setTestHook( [&]( const char * point ) {
		std::string_view p( point );
		if ( p == "publish" && firstPublish.exchange( false ) ) {
			paused.set_value();
			closedSent.wait_for( std::chrono::milliseconds( 500 ) );
		} else if ( p == "closed" && firstClose.exchange( false ) ) {
			closed.set_value();
		}
	} );
	s.run();
	s.initialize( fakeOptions( { { "debounceMs", 0 }, { "backend", false } } ) );
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", text } } } } );
	REQUIRE( paused.get_future().wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready );
	s.notify( "textDocument/didClose", { { "textDocument", { { "uri", uri } } } } );
	s.request( "shutdown" );
	std::vector<json> publishes;
	for ( const json & m : s.queue ) {
		if ( m.value( "method", "" ) == "textDocument/publishDiagnostics" && m["params"]["uri"] == uri ) publishes.push_back( m["params"] );
	}
	REQUIRE( publishes.size() == 2 );
	CHECK( ! publishes[0]["diagnostics"].empty() );
	CHECK( publishes[1]["diagnostics"] == json::array() );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
}

TEST_CASE( "a request cancelled before it is handled gets RequestCancelled" ) {
	Session s;
	s.initialize( { { "cfa", "/nonexistent/cfa" } } );
	json params = { { "textDocument", { { "uri", "file:///nowhere/x.cfa" } } }, { "position", pos( 0, 0 ) } };
	// All three arrive in one read, so the cancel is there before the
	// hover is answered.
	s.sendAll( { { { "jsonrpc", "2.0" }, { "id", 100 }, { "method", "textDocument/hover" }, { "params", params } },
				 { { "jsonrpc", "2.0" }, { "method", "$/cancelRequest" }, { "params", { { "id", 100 } } } },
				 { { "jsonrpc", "2.0" }, { "id", 101 }, { "method", "textDocument/hover" }, { "params", params } } } );
	json r = s.response( 100 );
	CHECK( r["error"]["code"] == -32800 );
	CHECK( ! r.contains( "result" ) );
	r = s.response( 101 );
	CHECK( r.contains( "result" ) );
	CHECK( r["result"].is_null() );
	// Cancelling something already answered changes nothing.
	s.notify( "$/cancelRequest", { { "id", 101 } } );
	CHECK( s.request( "textDocument/hover", params )["result"].is_null() );
	s.request( "shutdown" );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
}

TEST_CASE( "backend warnings with translator errors" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path );
	Session s;
	s.initialize( fakeOptions() );

	// Errors from a pass that only checks: there is C, and gcc runs on it.
	// Its warnings are shown, its errors (about code the translator
	// rejected) are not.
	s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 },
															{ "text", text + "// FAKE_CHECK_ERROR\n" } } } } );
	auto hasGcc = []( const json & ds ) {
		for ( const auto & d : ds ) if ( d["source"] == "gcc" ) return true;
		return false;
	};
	auto d = s.diagnosticsFor( uri, hasGcc );
	REQUIRE( d );
	bool translatorError = false;
	for ( const auto & x : ( *d )["diagnostics"] ) {
		if ( x["message"] == "fake error in main" ) translatorError = true;
		CHECK( x["message"] != "fake gcc error" );
		if ( x["source"] == "gcc" ) {
			CHECK( x["severity"] == 2 );
			CHECK( x["range"]["start"] == pos( 3, 1 ) );
		}
	}
	CHECK( translatorError );

	// Errors that stop translation: no C. The warning stays until the line
	// it is on is edited.
	s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 2 } } },
										  { "contentChanges", { { { "text", text + "// FAKE_HEADER_ERROR\n" } } } } } );
	auto version = [&]( int v ) {
		return s.waitFor( "textDocument/publishDiagnostics", [&]( const json & p ) { return p["uri"] == uri && p["version"] == v; } );
	};
	d = version( 2 );
	REQUIRE( d );
	CHECK( hasGcc( ( *d )["diagnostics"] ) );
	s.notify( "textDocument/didChange", { { "textDocument", { { "uri", uri }, { "version", 3 } } },
										  { "contentChanges", { { { "range", { { "start", pos( 3, 5 ) }, { "end", pos( 3, 11 ) } } }, { "text", "spare" } } } } } );
	d = version( 3 );
	REQUIRE( d );
	CHECK( ! ( *d )["diagnostics"].empty() );
	CHECK( ! hasGcc( ( *d )["diagnostics"] ) );
	s.request( "shutdown" );
	s.notify( "exit", nullptr );
	CHECK( s.finish() == 0 );
}

TEST_CASE( "workspace/didChangeConfiguration applies without a restart" ) {
	if ( fakeCfa().empty() ) {
		MESSAGE( "CFA_LSP_FAKE_CFA not set; skipping (run through make test)" );
		return;
	}
	FakeEnv env;
	std::string path = fs::canonical( fixtures() + "/server/hello.cfa" ).string();
	std::string uri = pathToUri( path );
	std::string text = readAll( path );
	auto hasGcc = []( const json & ds ) {
		for ( const auto & d : ds ) if ( d["source"] == "gcc" ) return true;
		return false;
	};

	SUBCASE( "pushed settings" ) {
		Session s;
		s.initialize( fakeOptions() );
		s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", text } } } } );
		REQUIRE( s.diagnosticsFor( uri, hasGcc ) );
		s.queue.clear();					// the check's first, empty, publish
		// Unrelated settings change nothing; ours turn the backend off and
		// re-check, and removing them brings back initializationOptions.
		s.notify( "workspace/didChangeConfiguration", { { "settings", { { "other-server", { { "backend", false } } } } } } );
		s.notify( "workspace/didChangeConfiguration", { { "settings", { { "cfa-lsp", { { "backend", false } } } } } } );
		REQUIRE( s.diagnosticsFor( uri, []( const json & ds ) { return ds.empty(); } ) );
		s.notify( "workspace/didChangeConfiguration", { { "settings", { { "cfa-lsp", { { "backend", nullptr } } } } } } );
		REQUIRE( s.diagnosticsFor( uri, hasGcc ) );
		s.request( "shutdown" );
		s.notify( "exit", nullptr );
		CHECK( s.finish() == 0 );
	}

	SUBCASE( "pulled with workspace/configuration" ) {
		Session s;
		s.initialize( fakeOptions(), { { "workspace", { { "configuration", true }, { "didChangeConfiguration", { { "dynamicRegistration", true } } } } } } );
		auto reg = s.serverRequest( "client/registerCapability" );
		REQUIRE( reg );
		CHECK( ( *reg )["params"]["registrations"][0]["method"] == "workspace/didChangeConfiguration" );
		s.send( { { "jsonrpc", "2.0" }, { "id", ( *reg )["id"] }, { "result", nullptr } } );
		// Asked once at startup; nothing configured yet.
		auto ask = s.serverRequest( "workspace/configuration" );
		REQUIRE( ask );
		CHECK( ( *ask )["params"]["items"][0]["section"] == "cfa-lsp" );
		s.send( { { "jsonrpc", "2.0" }, { "id", ( *ask )["id"] }, { "result", json::array( { nullptr } ) } } );
		s.notify( "textDocument/didOpen", { { "textDocument", { { "uri", uri }, { "languageId", "cfa" }, { "version", 1 }, { "text", text } } } } );
		REQUIRE( s.diagnosticsFor( uri, hasGcc ) );
		s.queue.clear();
		// A pull client's change notification has no settings: ask again.
		s.notify( "workspace/didChangeConfiguration", { { "settings", nullptr } } );
		ask = s.serverRequest( "workspace/configuration" );
		REQUIRE( ask );
		s.send( { { "jsonrpc", "2.0" }, { "id", ( *ask )["id"] }, { "result", json::array( { json{ { "backend", false } } } ) } } );
		REQUIRE( s.diagnosticsFor( uri, []( const json & ds ) { return ds.empty(); } ) );
		s.request( "shutdown" );
		s.notify( "exit", nullptr );
		CHECK( s.finish() == 0 );
	}
}
