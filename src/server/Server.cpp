#include "Server.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <tuple>

#include "Flags.hpp"
#include "Log.hpp"
#include "TextScan.hpp"
#include "Uri.hpp"

namespace fs = std::filesystem;

namespace cfalsp {

namespace {

enum ErrorCode {
	ParseError = -32700,
	InvalidRequest = -32600,
	MethodNotFound = -32601,
	InvalidParams = -32602,
	InternalError = -32603,
	ServerNotInitialized = -32002,
	RequestFailed = -32803,
	RequestCancelled = -32800,
};

struct LspError {
	int code;
	std::string message;
};

std::optional<std::string> readFile( const std::string & path ) {
	std::ifstream in( path, std::ios::binary );
	if ( ! in ) return std::nullopt;
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool isHeader( const std::string & path ) {
	for ( const char * ext : { ".hfa", ".h", ".ifa" } ) {
		std::string e( ext );
		if ( path.size() > e.size() && path.compare( path.size() - e.size(), e.size(), e ) == 0 ) return true;
	}
	return false;
}

const nlohmann::json & member( const nlohmann::json & j, const char * key ) {
	static const nlohmann::json null;
	if ( ! j.is_object() ) return null;
	auto it = j.find( key );
	return it == j.end() ? null : *it;
}

int intOr( const nlohmann::json & j, int dflt ) { return j.is_number_integer() ? j.get<int>() : dflt; }

std::string methodOf( const nlohmann::json & msg ) {
	const nlohmann::json & m = member( msg, "method" );
	return m.is_string() ? m.get<std::string>() : std::string();
}

// Adds the diagnostics in `more` that `into` doesn't have on the same line.
void mergeDiags( std::vector<Diag> & into, const std::vector<Diag> & more ) {
	std::set<std::tuple<std::string, int, std::string>> seen;
	for ( const auto & d : into ) seen.insert( { d.file, d.range.start.line, d.message } );
	for ( const auto & d : more ) {
		if ( seen.insert( { d.file, d.range.start.line, d.message } ).second ) into.push_back( d );
	}
}

// A full-text change as a few small edits, so the snapshot mapping stays
// useful for clients that only send whole buffers: one edit per changed run
// of lines, last first, so each edit's positions are in the text before it.
// Is a name at `r` in `text` still a whole name? A range mapped from the
// snapshot can be part of a longer one if the user typed onto its end.
bool wholeName( const Text & text, Range r ) {
	std::string_view line = text.line( r.start.line );
	if ( r.start.line != r.end.line || r.start.col >= (int)line.size() || ! text::isIdentChar( line[r.start.col] ) ) return true;
	bool left = r.start.col == 0 || ! text::isIdentChar( line[r.start.col - 1] );
	bool right = r.end.col >= (int)line.size() || ! text::isIdentChar( line[r.end.col] );
	return left && right;
}

// Does `name` have the characters of `query` in order, ignoring case?
bool fuzzyMatch( std::string_view name, std::string_view query ) {
	size_t k = 0;
	for ( char c : name ) {
		if ( k < query.size() && std::tolower( (unsigned char)c ) == std::tolower( (unsigned char)query[k] ) ) k += 1;
	}
	return k == query.size();
}

std::vector<std::pair<Range, std::string>> diffEdits( const Text & oldText, const std::string & newText ) {
	const std::string & a = oldText.str();
	auto lines = []( const std::string & s ) {
		std::vector<size_t> starts{ 0 };
		for ( size_t i = 0; i < s.size(); i += 1 ) if ( s[i] == '\n' ) starts.push_back( i + 1 );
		return starts;
	};
	std::vector<size_t> la = lines( a ), lb = lines( newText );
	auto lineOf = []( const std::string & s, const std::vector<size_t> & st, size_t i ) {
		size_t e = i + 1 < st.size() ? st[i + 1] : s.size();
		return std::string_view( s ).substr( st[i], e - st[i] );
	};
	size_t na = la.size(), nb = lb.size();
	size_t pre = 0;
	while ( pre < na && pre < nb && lineOf( a, la, pre ) == lineOf( newText, lb, pre ) ) pre += 1;
	size_t suf = 0;
	while ( suf < na - pre && suf < nb - pre && lineOf( a, la, na - 1 - suf ) == lineOf( newText, lb, nb - 1 - suf ) ) suf += 1;

	// Changed line runs [a0, a1) -> [b0, b1), from a line LCS of the middle when it is small enough.
	struct Hunk { size_t a0, a1, b0, b1; };
	std::vector<Hunk> hunks;
	size_t ma = na - pre - suf, mb = nb - pre - suf;
	if ( ma > 0 && mb > 0 && ma * mb <= 4000000 ) {
		std::vector<uint32_t> len( ( ma + 1 ) * ( mb + 1 ), 0 );
		auto at = [mb]( size_t i, size_t j ) { return i * ( mb + 1 ) + j; };
		for ( size_t i = ma; i-- > 0; ) {
			for ( size_t j = mb; j-- > 0; ) {
				len[at( i, j )] = lineOf( a, la, pre + i ) == lineOf( newText, lb, pre + j )
					? len[at( i + 1, j + 1 )] + 1 : std::max( len[at( i + 1, j )], len[at( i, j + 1 )] );
			}
		}
		size_t i = 0, j = 0, hi = 0, hj = 0;
		while ( i < ma || j < mb ) {
			if ( i < ma && j < mb && lineOf( a, la, pre + i ) == lineOf( newText, lb, pre + j ) ) {
				if ( hi < i || hj < j ) hunks.push_back( { pre + hi, pre + i, pre + hj, pre + j } );
				i += 1, j += 1;
				hi = i, hj = j;
			} else if ( j < mb && ( i == ma || len[at( i, j + 1 )] >= len[at( i + 1, j )] ) ) {
				j += 1;
			} else {
				i += 1;
			}
		}
		if ( hi < i || hj < j ) hunks.push_back( { pre + hi, pre + i, pre + hj, pre + j } );
	} else if ( ma > 0 || mb > 0 ) {
		hunks.push_back( { pre, na - suf, pre, nb - suf } );
	}

	std::vector<std::pair<Range, std::string>> out;
	for ( auto h = hunks.rbegin(); h != hunks.rend(); ++h ) {
		size_t ab = la[h->a0], ae = h->a1 < na ? la[h->a1] : a.size();
		size_t bb = lb[h->b0], be = h->b1 < nb ? lb[h->b1] : newText.size();
		// Trim the hunk to the characters that differ.
		while ( ab < ae && bb < be && a[ab] == newText[bb] ) ab += 1, bb += 1;
		while ( ab < ae && bb < be && a[ae - 1] == newText[be - 1] ) ae -= 1, be -= 1;
		out.push_back( { Range{ oldText.loc( ab ), oldText.loc( ae ) }, newText.substr( bb, be - bb ) } );
	}
	return out;
}

} // namespace

Server::Server( int inFd, int outFd, std::string exeDir ) : reader( inFd ), writer( outFd ), exeDir( std::move( exeDir ) ) {}

Server::~Server() { stopWorker(); }

EditList Server::Document::editsSince( uint64_t s ) const {
	EditList out;
	for ( const auto & [seq, e] : log ) {
		if ( seq > s ) out.push_back( e );
	}
	return out;
}

EditList Server::Document::editsBetween( uint64_t from, uint64_t to ) const {
	EditList out;
	for ( const auto & [seq, e] : log ) {
		if ( seq > from && seq <= to ) out.push_back( e );
	}
	return out;
}

Server::Incoming Server::parse( const std::string & body ) {
	Incoming in;
	try {
		in.msg = json::parse( body );
	} catch ( const std::exception & e ) {
		in.error = e.what();
	}
	return in;
}

int Server::run() {
	for ( ;; ) {
		if ( inbox.empty() ) {
			auto body = reader.read();
			if ( ! body ) {
				log::info( "end of input" );
				stopWorker();
				return phase == Phase::ShuttingDown ? 0 : 1;
			}
			inbox.push_back( parse( *body ) );
		}
		Incoming in = std::move( inbox.front() );
		inbox.pop_front();
		if ( ! in.error.empty() ) {
			send( { { "jsonrpc", "2.0" }, { "id", nullptr },
					{ "error", { { "code", ParseError }, { "message", in.error } } } } );
			continue;
		}
		const json & msg = in.msg;
		if ( methodOf( msg ) == "exit" ) {
			log::info( "exit" );
			stopWorker();
			return phase == Phase::ShuttingDown ? 0 : 1;
		}
		// Requests are answered in order, so a request can wait behind a slow
		// one. Its cancel may have arrived meanwhile.
		if ( msg.is_object() && msg.contains( "id" ) && msg.contains( "method" ) ) {
			readAhead( msg["id"] );
			auto c = cancelled.find( msg["id"].dump() );
			if ( c != cancelled.end() ) {
				cancelled.erase( c );
				send( { { "jsonrpc", "2.0" }, { "id", msg["id"] },
						{ "error", { { "code", RequestCancelled }, { "message", "request cancelled" } } } } );
				continue;
			}
		}
		handle( msg );
	}
}

void Server::readAhead( const json & current ) {
	while ( auto body = reader.tryRead() ) {
		Incoming in = parse( *body );
		if ( in.error.empty() && methodOf( in.msg ) == "$/cancelRequest" ) {
			const json & id = member( member( in.msg, "params" ), "id" );
			bool known = id == current;
			for ( const Incoming & q : inbox ) {
				known = known || ( q.msg.is_object() && q.msg.contains( "method" ) && member( q.msg, "id" ) == id );
			}
			// A cancel for a request already answered needs nothing.
			if ( known && ! id.is_null() ) cancelled.insert( id.dump() );
			continue;
		}
		inbox.push_back( std::move( in ) );
	}
}

void Server::send( const json & msg ) {
	std::string s = msg.dump( -1, ' ', false, json::error_handler_t::replace );
	log::debug( "--> ", s.size() > 2000 ? s.substr( 0, 2000 ) + "..." : s );
	writer.write( s );
}

void Server::notify( const std::string & method, json params ) {
	send( { { "jsonrpc", "2.0" }, { "method", method }, { "params", std::move( params ) } } );
}

void Server::handle( const json & msg ) {
	if ( ! msg.is_object() ) {
		send( { { "jsonrpc", "2.0" }, { "id", nullptr },
				{ "error", { { "code", InvalidRequest }, { "message", "not a JSON object" } } } } );
		return;
	}
	auto mit = msg.find( "method" );
	bool hasId = msg.contains( "id" );
	if ( mit == msg.end() || ! mit->is_string() ) {
		if ( ! hasId ) {
			send( { { "jsonrpc", "2.0" }, { "id", nullptr },
					{ "error", { { "code", InvalidRequest }, { "message", "missing method" } } } } );
		} else {
			response( msg );						// to something we sent
		}
		return;
	}
	const std::string method = *mit;
	const json & params = member( msg, "params" );
	log::debug( "<-- ", method, hasId ? " (request)" : "" );

	if ( ! hasId ) {
		try {
			notification( method, params );
		} catch ( const std::exception & e ) {
			log::error( method, ": ", e.what() );
		}
		return;
	}

	const json id = msg["id"];
	json reply = { { "jsonrpc", "2.0" }, { "id", id } };
	try {
		reply["result"] = request( method, params );
	} catch ( const LspError & e ) {
		reply["error"] = { { "code", e.code }, { "message", e.message } };
	} catch ( const std::exception & e ) {
		log::error( method, ": ", e.what() );
		reply["error"] = { { "code", InternalError }, { "message", e.what() } };
	}
	send( reply );
}

nlohmann::json Server::request( const std::string & method, const json & params ) {
	if ( method == "initialize" ) {
		if ( phase != Phase::Uninitialized ) throw LspError{ InvalidRequest, "already initialized" };
		return initialize( params );
	}
	if ( phase == Phase::Uninitialized ) throw LspError{ ServerNotInitialized, "server not initialized" };
	if ( phase == Phase::ShuttingDown ) throw LspError{ InvalidRequest, "server is shutting down" };
	if ( method == "shutdown" ) {
		phase = Phase::ShuttingDown;
		stopWorker();
		return nullptr;
	}

	std::lock_guard<std::mutex> lock( mtx );
	diskCache.clear();
	if ( method == "textDocument/hover" ) return hover( params );
	if ( method == "textDocument/definition" || method == "textDocument/declaration" ) return definition( params );
	if ( method == "textDocument/references" ) return references( params );
	if ( method == "textDocument/documentSymbol" ) return documentSymbol( params );
	if ( method == "textDocument/completion" ) return completion( params );
	if ( method == "textDocument/signatureHelp" ) return signatureHelp( params );
	if ( method == "textDocument/semanticTokens/full" ) return semanticTokens( params );
	if ( method == "textDocument/documentHighlight" ) return documentHighlight( params );
	if ( method == "textDocument/inlayHint" ) return inlayHint( params );
	if ( method == "textDocument/prepareRename" ) return prepareRename( params );
	if ( method == "textDocument/rename" ) return rename( params );
	if ( method == "textDocument/switchSourceHeader" ) return switchSourceHeader( params );
	if ( method == "workspace/symbol" ) return workspaceSymbol( params );
	throw LspError{ MethodNotFound, "unhandled method " + method };
}

void Server::notification( const std::string & method, const json & params ) {
	if ( phase == Phase::Uninitialized ) return;		// dropped, per spec
	if ( method == "initialized" ) {
		warnMissing();
		if ( configurationRegistration ) {
			sendRequest( "client/registerCapability",
						 { { "registrations", { { { "id", "cfa-lsp-configuration" }, { "method", "workspace/didChangeConfiguration" } } } } } );
		}
		if ( configurationPull ) requestConfiguration();
		return;
	}
	if ( phase == Phase::ShuttingDown ) return;
	if ( method == "textDocument/didOpen" ) didOpen( params );
	else if ( method == "textDocument/didChange" ) didChange( params );
	else if ( method == "textDocument/didClose" ) didClose( params );
	else if ( method == "textDocument/didSave" ) didSave( params );
	else if ( method == "workspace/didChangeConfiguration" ) didChangeConfiguration( params );
	// $/cancelRequest is handled when reading ahead (see run()); by the time
	// one gets here, its request has been answered.
}

int Server::sendRequest( const std::string & method, json params ) {
	int id = nextRequestId++;
	send( { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move( params ) } } );
	return id;
}

void Server::response( const json & msg ) {
	const json & id = member( msg, "id" );
	if ( ! id.is_number_integer() || configRequests.erase( id.get<int>() ) == 0 ) return;
	// After shutdown the worker is gone; there is nothing to re-check.
	if ( phase != Phase::Running ) return;
	const json & result = member( msg, "result" );
	if ( result.is_array() && ! result.empty() ) applySettings( result[0] );
}

nlohmann::json Server::initialize( const json & params ) {
	const json & caps = member( params, "capabilities" );
	const json & encs = member( member( caps, "general" ), "positionEncodings" );
	enc = Encoding::Utf16;
	if ( encs.is_array() ) {
		for ( const auto & e : encs ) {
			if ( e == "utf-8" ) enc = Encoding::Utf8;
		}
	}
	hierarchicalSymbols = member( member( member( caps, "textDocument" ), "documentSymbol" ), "hierarchicalDocumentSymbolSupport" ) == true;
	prepareRenameSupport = member( member( member( caps, "textDocument" ), "rename" ), "prepareSupport" ) == true;
	inlayHintRefresh = member( member( member( caps, "workspace" ), "inlayHint" ), "refreshSupport" ) == true;
	const json & ws = member( caps, "workspace" );
	configurationPull = member( ws, "configuration" ) == true;
	configurationRegistration = member( member( ws, "didChangeConfiguration" ), "dynamicRegistration" ) == true;

	const json & root = member( params, "rootUri" );
	if ( root.is_string() ) {
		if ( auto p = uriToPath( root.get<std::string>() ) ) rootPath = *p;
	} else if ( member( params, "rootPath" ).is_string() ) {
		rootPath = member( params, "rootPath" ).get<std::string>();
	}

	const json & io = member( params, "initializationOptions" );
	initOptions = io.is_object() ? io : json::object();
	log::info( "encoding: ", enc == Encoding::Utf8 ? "utf-8" : "utf-16" );
	applyOptions( initOptions );
	phase = Phase::Running;
	startWorker();

	json tokenTypes = Analysis::tokenTypes();
	json tokenModifiers = Analysis::tokenModifiers();
	json capabilities = {
		{ "positionEncoding", enc == Encoding::Utf8 ? "utf-8" : "utf-16" },
		{ "textDocumentSync", { { "openClose", true }, { "change", 2 }, { "save", { { "includeText", false } } } } },
		{ "hoverProvider", true },
		{ "definitionProvider", true },
		{ "declarationProvider", true },
		{ "referencesProvider", true },
		{ "documentSymbolProvider", true },
		{ "completionProvider", { { "triggerCharacters", { ".", ">" } }, { "resolveProvider", false } } },
		{ "signatureHelpProvider", { { "triggerCharacters", { "(", "," } } } },
		{ "semanticTokensProvider", { { "legend", { { "tokenTypes", tokenTypes }, { "tokenModifiers", tokenModifiers } } },
									  { "full", true } } },
		{ "documentHighlightProvider", true },
		{ "inlayHintProvider", true },
		{ "renameProvider", prepareRenameSupport ? json{ { "prepareProvider", true } } : json( true ) },
		{ "workspaceSymbolProvider", true },
	};
	return { { "capabilities", capabilities }, { "serverInfo", { { "name", "cfa-lsp" }, { "version", "0.1.0" } } } };
}

// ---------------------------------------------------------------- configuration

void Server::applyOptions( const json & io ) {
	ToolchainOptions to;
	to.exeDir = exeDir;
	auto str = [&]( const char * k ) { return member( io, k ).is_string() ? member( io, k ).get<std::string>() : std::string(); };
	to.cfa = str( "cfa" );
	to.translator = str( "translator" );
	to.preludeDir = str( "preludeDir" );
	to.cc = str( "cc" );
	Options o;
	o.debounceMs = std::max( 0, intOr( member( io, "debounceMs" ), 500 ) );
	o.timeoutMs = std::max( 1, intOr( member( io, "timeoutMs" ), 120000 ) );
	if ( member( io, "backend" ).is_boolean() ) o.backend = member( io, "backend" ).get<bool>();
	if ( member( io, "stopAfterResolve" ).is_boolean() ) o.stopAfterResolve = member( io, "stopAfterResolve" ).get<bool>();
	const json & fl = member( io, "flags" );
	if ( fl.is_array() ) {
		std::vector<std::string> v;
		for ( const auto & f : fl ) {
			if ( f.is_string() ) v.push_back( f );
		}
		o.flags = v;
	} else if ( fl.is_string() ) {
		o.flags = parseFlagsFile( fl.get<std::string>() );
	}

	Toolchain tc = discoverToolchain( to );
	log::info( "cfa: ", tc.cfa.empty() ? "(none)" : tc.cfa, "; translator: ", tc.translator.empty() ? "(none)" : tc.translator );
	auto chk = std::make_shared<const Checker>( tc );
	std::lock_guard<std::mutex> lock( mtx );
	opts = o;
	checker = chk;
	effective = io;
}

void Server::didChangeConfiguration( const json & params ) {
	const json & s = member( params, "settings" );
	if ( s.is_object() && ! s.empty() ) applySettings( s );
	else if ( configurationPull ) requestConfiguration();	// the pull model sends no settings
}

void Server::requestConfiguration() {
	configRequests.insert( sendRequest( "workspace/configuration", { { "items", { { { "section", "cfa-lsp" } } } } } ) );
}

void Server::applySettings( const json & s ) {
	// Settings pushed by the client are usually keyed by server name; a
	// workspace/configuration answer is our section already.
	const json * section = &s;
	for ( const char * k : { "cfa-lsp" } ) {
		if ( member( s, k ).is_object() ) {
			section = &member( s, k );
			break;
		}
	}
	settings = section->is_object() ? *section : json::object();
	// Our settings override initializationOptions key by key; null removes
	// one. Other keys are someone else's.
	json eff = initOptions;
	for ( const char * k : { "cfa", "translator", "preludeDir", "flags", "backend", "stopAfterResolve", "cc", "debounceMs", "timeoutMs" } ) {
		auto it = settings.find( k );
		if ( it == settings.end() ) continue;
		if ( it->is_null() ) eff.erase( k );
		else eff[k] = *it;
	}
	if ( eff == effective ) return;
	log::info( "configuration changed: ", eff.dump() );
	applyOptions( eff );
	{
		std::lock_guard<std::mutex> lock( mtx );
		if ( inflight ) inflight->token->cancel();
		for ( auto & [path, doc] : docs ) schedule( path, 0 );
	}
	warnMissing();
}

void Server::warnMissing() {
	const Toolchain & tc = checker->toolchain();
	std::string missing;
	if ( tc.cfa.empty() ) missing = "the cfa compiler (not on PATH; set initializationOptions.cfa)";
	else if ( tc.translator.empty() ) missing = "the cfa-lsp translator (set initializationOptions.translator or CFA_LSP_TRANSLATOR); only compiler errors will be shown";
	if ( missing == warnedMissing ) return;
	warnedMissing = missing;
	if ( ! missing.empty() ) notify( "window/showMessage", { { "type", 2 }, { "message", "cfa-lsp: cannot find " + missing + "." } } );
}

// ---------------------------------------------------------------- documents

void Server::didOpen( const json & params ) {
	const json & td = member( params, "textDocument" );
	if ( ! member( td, "uri" ).is_string() || ! member( td, "text" ).is_string() ) return;
	std::string uri = td["uri"];
	auto path = uriToPath( uri );
	if ( ! path ) {
		log::warn( "ignoring non-file document ", uri );
		return;
	}
	std::string norm = fs::path( *path ).lexically_normal().string();
	std::lock_guard<std::mutex> lock( mtx );
	Document & d = docs[norm];
	d = Document();
	d.uri = uri;
	d.path = norm;
	d.version = intOr( member( td, "version" ), 0 );
	d.text = Text( td["text"].get<std::string>() );
	if ( readFile( norm ) == d.text.str() ) d.diskSeq = 0;
	schedule( d.path, 0 );
}

void Server::didChange( const json & params ) {
	std::lock_guard<std::mutex> lock( mtx );
	Document * d = docFor( params );
	if ( ! d ) return;
	d->version = intOr( member( member( params, "textDocument" ), "version" ), d->version );
	const json & changes = member( params, "contentChanges" );
	if ( ! changes.is_array() ) return;
	for ( const auto & ch : changes ) {
		if ( ! member( ch, "text" ).is_string() ) continue;
		const std::string & text = ch["text"].get_ref<const std::string &>();
		std::vector<std::pair<Range, std::string>> edits;
		const json & range = member( ch, "range" );
		if ( range.is_object() ) {
			const json & s = member( range, "start" ), & e = member( range, "end" );
			Loc a = d->text.fromLsp( intOr( member( s, "line" ), 0 ), intOr( member( s, "character" ), 0 ), enc );
			Loc b = d->text.fromLsp( intOr( member( e, "line" ), 0 ), intOr( member( e, "character" ), 0 ), enc );
			edits.push_back( { Range{ a, b }, text } );
		} else {
			edits = diffEdits( d->text, text );
		}
		for ( const auto & [r, ins] : edits ) {
			auto [s, e] = d->text.replace( r.start, r.end, ins );
			d->seq += 1;
			d->log.push_back( { d->seq, makeEdit( s, e, ins ) } );
		}
	}
	// The check in flight keeps running and its results are mapped through
	// these edits. Cancelling it would starve diagnostics while someone types
	// with pauses shorter than the debounce plus the check time. A check that
	// has run more than twice as long as the last one is probably stuck on
	// text this edit may have fixed, such as an expression the resolver takes
	// minutes on, so that one is cancelled. Raising lastCheck to the time it
	// ran means a check that got slower for good still finishes after a few
	// edits.
	if ( inflight && inflight->path == d->path && ! inflight->token->cancelled() && d->lastCheck > Clock::duration::zero() ) {
		Clock::duration ran = Clock::now() - inflight->started;
		if ( ran > 2 * d->lastCheck ) {
			inflight->token->cancel();
			d->lastCheck = ran;
		}
	}
	schedule( d->path, opts.debounceMs );
	trimLog( *d );
}

void Server::didClose( const json & params ) {
	std::vector<json> out;
	std::unique_lock<std::mutex> lock( mtx );
	Document * d = docFor( params );
	if ( ! d ) return;
	std::string path = d->path;
	pending.erase( path );
	if ( inflight && inflight->path == path ) inflight->token->cancel();
	diskCache.clear();
	storeDiags( path, 0, {}, out );
	docs.erase( path );
	std::lock_guard<std::mutex> pub( publishMtx );
	lock.unlock();
	for ( auto & p : out ) notify( "textDocument/publishDiagnostics", std::move( p ) );
	if ( testHook ) testHook( "closed" );
}

void Server::didSave( const json & params ) {
	std::lock_guard<std::mutex> lock( mtx );
	Document * d = docFor( params );
	if ( ! d ) return;
	d->diskSeq = d->seq;
	// Skip if the check in flight already covers this text. Otherwise save
	// always re-checks (after the check in flight): headers on disk may have
	// changed.
	bool covered = inflight && inflight->path == d->path && inflight->seq == d->seq;
	if ( ! covered ) schedule( d->path, 0 );
	if ( isHeader( d->path ) ) {
		for ( auto & [p, other] : docs ) {
			if ( p != d->path ) schedule( p, opts.debounceMs );
		}
	}
}

void Server::schedule( const std::string & path, int delayMs ) {
	auto due = Clock::now() + std::chrono::milliseconds( delayMs );
	auto it = pending.find( path );
	if ( it == pending.end() || delayMs > 0 || due < it->second ) pending[path] = due;
	cv.notify_all();
}

void Server::trimLog( Document & doc ) {
	uint64_t keep = doc.analysis ? doc.analysisSeq : doc.seq;
	auto ds = diagStore.find( doc.path );
	if ( ds != diagStore.end() ) {
		auto own = ds->second.find( doc.path );
		if ( own != ds->second.end() ) keep = std::min( keep, own->second.seq );
	}
	if ( inflight && inflight->path == doc.path ) keep = std::min( keep, inflight->seq );
	if ( doc.diskSeq ) keep = std::min( keep, *doc.diskSeq );
	if ( ! doc.backDiags.empty() ) keep = std::min( keep, doc.backSeq );
	auto it = std::find_if( doc.log.begin(), doc.log.end(), [&]( const auto & e ) { return e.first > keep; } );
	doc.log.erase( doc.log.begin(), it );
}

Server::Document * Server::docFor( const json & params ) {
	const json & uri = member( member( params, "textDocument" ), "uri" );
	if ( ! uri.is_string() ) return nullptr;
	auto path = uriToPath( uri.get<std::string>() );
	if ( ! path ) return nullptr;
	auto it = docs.find( fs::path( *path ).lexically_normal().string() );
	return it == docs.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- conversions

nlohmann::json Server::lspPos( const Text & text, Loc l ) const {
	Loc c = text.clamp( l );
	return { { "line", c.line }, { "character", text.toLspCol( c, enc ) } };
}

nlohmann::json Server::lspRange( const Text & text, Range r ) const {
	return { { "start", lspPos( text, r.start ) }, { "end", lspPos( text, r.end ) } };
}

const Text & Server::textOf( const std::string & path ) {
	auto d = docs.find( path );
	if ( d != docs.end() ) return d->second.text;
	auto it = diskCache.find( path );
	if ( it == diskCache.end() ) it = diskCache.emplace( path, Text( readFile( path ).value_or( "" ) ) ).first;
	return it->second;
}

const Text & Server::diskText( const std::string & path ) {
	auto it = diskCache.find( path );
	if ( it == diskCache.end() ) it = diskCache.emplace( path, Text( readFile( path ).value_or( "" ) ) ).first;
	return it->second;
}

std::optional<std::pair<const Text *, Range>> Server::fromDisk( const std::string & path, Range r, bool exact ) {
	auto d = docs.find( path );
	if ( d == docs.end() ) return std::pair{ &diskText( path ), r };
	const Document & doc = d->second;
	if ( ! doc.diskSeq ) return std::pair{ &diskText( path ), r };
	EditList edits = doc.editsSince( *doc.diskSeq );
	if ( ! exact ) return std::pair{ &doc.text, toCurrentClamped( edits, r ) };
	auto m = toCurrentExact( edits, r );
	if ( ! m ) return std::nullopt;
	return std::pair{ &doc.text, *m };
}

bool Server::sameIdentifier( const Document & d, Loc cur, const EditList & edits ) const {
	if ( edits.empty() ) return true;
	auto id = text::identifierAt( d.text.line( cur.line ), cur.col );
	if ( ! id ) return true;
	Loc a{ cur.line, id->first }, b{ cur.line, id->second };
	MappedLoc ma = toSnapshot( edits, a ), mb = toSnapshot( edits, b );
	if ( ! ma.exact || ! mb.exact ) return false;
	auto back = toCurrentExact( edits, { ma.loc, mb.loc } );
	return back && back->start == a && back->end == b;
}

std::string Server::uriOf( const std::string & path ) const {
	auto d = docs.find( path );
	return d != docs.end() ? d->second.uri : pathToUri( path );
}

std::optional<nlohmann::json> Server::lspLocation( const Document & from, const Location & loc, bool exact ) {
	std::string file = fs::path( loc.file ).lexically_normal().string();
	if ( file == from.path ) {
		EditList edits = from.editsSince( from.analysisSeq );
		Range r;
		if ( exact ) {
			auto m = toCurrentExact( edits, loc.range );
			if ( ! m || ( ! edits.empty() && ! wholeName( from.text, *m ) ) ) return std::nullopt;
			r = *m;
		} else {
			r = toCurrentClamped( edits, loc.range );
		}
		return json{ { "uri", from.uri }, { "range", lspRange( from.text, r ) } };
	}
	if ( file.empty() || file[0] != '/' ) return std::nullopt;
	// Other files were read from disk by the check.
	auto m = fromDisk( file, loc.range, exact );
	if ( ! m ) return std::nullopt;
	return json{ { "uri", uriOf( file ) }, { "range", lspRange( *m->first, m->second ) } };
}

// ---------------------------------------------------------------- queries

nlohmann::json Server::hover( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return nullptr;
	const json & p = member( params, "position" );
	Loc cur = d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	EditList edits = d->editsSince( d->analysisSeq );
	MappedLoc m = toSnapshot( edits, cur );
	if ( m.exact && sameIdentifier( *d, cur, edits ) ) {
		if ( auto h = d->analysis->hover( d->path, m.loc ) ) {
			json out = { { "contents", { { "kind", "markdown" }, { "value", h->markdown } } } };
			if ( auto r = toCurrentExact( edits, h->range ) ) {
				if ( r->start != r->end ) out["range"] = lspRange( d->text, *r );
			}
			return out;
		}
	}
	// A name the snapshot doesn't have: declared since, or in code that didn't parse.
	auto h = d->analysis->hoverInText( d->text.str(), d->text.offset( cur ) );
	if ( ! h ) return nullptr;
	return { { "contents", { { "kind", "markdown" }, { "value", h->markdown } } }, { "range", lspRange( d->text, h->range ) } };
}

nlohmann::json Server::definition( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return json::array();
	const json & p = member( params, "position" );
	Loc cur = d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	EditList edits = d->editsSince( d->analysisSeq );
	MappedLoc m = toSnapshot( edits, cur );
	json out = json::array();
	if ( m.exact && sameIdentifier( *d, cur, edits ) ) {
		// A prototype whose body is in another open document (a header and its .cfa): go to the body.
		std::vector<Location> decls = d->analysis->declarationsAt( d->path, m.loc );
		if ( ! decls.empty() && ! d->analysis->definitionOf( decls ) ) {
			for ( auto & [path, other] : docs ) {
				if ( &other == d || ! other.analysis ) continue;
				if ( auto def = other.analysis->definitionOf( decls ) ) {
					if ( auto j = lspLocation( other, *def, true ) ) return json::array( { *j } );
				}
			}
		}
		for ( const Location & l : d->analysis->definition( d->path, m.loc ) ) {
			if ( auto j = lspLocation( *d, l, true ) ) out.push_back( *j );
		}
		if ( ! out.empty() ) return out;
	}
	// A name the snapshot doesn't have: declared since, or in code that didn't parse.
	if ( auto r = d->analysis->declarationInText( d->text.str(), d->text.offset( cur ) ) ) {
		out.push_back( json{ { "uri", d->uri }, { "range", lspRange( d->text, *r ) } } );
	}
	return out;
}

nlohmann::json Server::references( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return json::array();
	const json & p = member( params, "position" );
	Loc cur = d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	EditList edits = d->editsSince( d->analysisSeq );
	MappedLoc m = toSnapshot( edits, cur );
	if ( ! m.exact || ! sameIdentifier( *d, cur, edits ) ) return json::array();
	bool incl = member( member( params, "context" ), "includeDeclaration" ) == true;
	json out = json::array();
	std::set<std::string> seen;
	auto add = [&]( const Document & from, const Location & l ) {
		auto j = lspLocation( from, l, true );
		if ( j && seen.insert( j->dump() ).second ) out.push_back( *j );
	};
	for ( const Location & l : d->analysis->references( d->path, m.loc, incl ) ) add( *d, l );
	// Each open document's analysis has the uses in that document.
	std::vector<Location> decls = d->analysis->declarationsAt( d->path, m.loc );
	if ( ! decls.empty() ) {
		for ( auto & [path, other] : docs ) {
			if ( &other == d || ! other.analysis ) continue;
			for ( const Location & l : other.analysis->referencesTo( decls, incl ) ) add( other, l );
		}
	}
	return out;
}

nlohmann::json Server::documentSymbol( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return json::array();
	EditList edits = d->editsSince( d->analysisSeq );
	std::function<json( const Symbol & )> conv = [&]( const Symbol & s ) -> json {
		Range r = toCurrentClamped( edits, s.range );
		Range sel = toCurrentExact( edits, s.selectionRange ).value_or( Range{ r.start, r.start } );
		if ( sel.start < r.start || r.end < sel.end ) sel = { r.start, r.start };
		json j = { { "name", s.name.empty() ? std::string( "?" ) : s.name }, { "kind", s.kind },
				   { "range", lspRange( d->text, r ) }, { "selectionRange", lspRange( d->text, sel ) } };
		if ( ! s.detail.empty() ) j["detail"] = s.detail;
		json kids = json::array();
		for ( const auto & c : s.children ) kids.push_back( conv( c ) );
		if ( ! kids.empty() ) j["children"] = kids;
		return j;
	};
	json out = json::array();
	std::vector<Symbol> syms = d->analysis->documentSymbols( d->path );
	if ( hierarchicalSymbols ) {
		for ( const auto & s : syms ) out.push_back( conv( s ) );
		return out;
	}
	std::function<void( const Symbol &, const std::string & )> flat = [&]( const Symbol & s, const std::string & container ) {
		Range r = toCurrentClamped( edits, s.range );
		json j = { { "name", s.name }, { "kind", s.kind },
				   { "location", { { "uri", d->uri }, { "range", lspRange( d->text, r ) } } } };
		if ( ! container.empty() ) j["containerName"] = container;
		out.push_back( j );
		for ( const auto & c : s.children ) flat( c, s.name );
	};
	for ( const auto & s : syms ) flat( s, "" );
	return out;
}

nlohmann::json Server::completion( const json & params ) {
	json empty = { { "isIncomplete", false }, { "items", json::array() } };
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return empty;
	const json & p = member( params, "position" );
	Loc cur = d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	MappedLoc m = toSnapshot( d->editsSince( d->analysisSeq ), cur );
	std::string lineBefore( d->text.line( cur.line ).substr( 0, cur.col ) );
	json items = json::array();
	// '>' is a trigger character for ->; in a comparison or a shift it isn't a member access.
	const json & context = member( params, "context" );
	if ( member( context, "triggerCharacter" ) == ">" && ! ( lineBefore.size() >= 2 && lineBefore.ends_with( "->" ) ) ) return empty;
	std::string textBefore = d->text.str().substr( 0, d->text.offset( cur ) );
	for ( const CompletionItem & c : d->analysis->completion( d->path, m.loc, lineBefore, textBefore ) ) {
		json j = { { "label", c.label }, { "kind", c.kind } };
		if ( ! c.detail.empty() ) j["detail"] = c.detail;
		if ( ! c.documentation.empty() ) j["documentation"] = { { "kind", "markdown" }, { "value", c.documentation } };
		if ( ! c.insertText.empty() ) j["insertText"] = c.insertText;
		if ( ! c.sortText.empty() ) j["sortText"] = c.sortText;
		items.push_back( std::move( j ) );
	}
	return { { "isIncomplete", false }, { "items", items } };
}

nlohmann::json Server::signatureHelp( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return nullptr;
	const json & p = member( params, "position" );
	Loc cur = d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	MappedLoc m = toSnapshot( d->editsSince( d->analysisSeq ), cur );
	std::string textBefore = d->text.str().substr( 0, d->text.offset( cur ) );
	auto sh = d->analysis->signatureHelp( d->path, m.loc, textBefore );
	if ( ! sh || sh->signatures.empty() ) return nullptr;
	json sigs = json::array();
	for ( const SignatureInfo & s : sh->signatures ) {
		json ps = json::array();
		for ( auto [b, e] : s.params ) {
			int lb = fromByteCol( s.label, b, enc ), le = fromByteCol( s.label, e, enc );
			ps.push_back( { { "label", { lb, le } } } );
		}
		json j = { { "label", s.label }, { "parameters", ps } };
		if ( ! s.documentation.empty() ) j["documentation"] = { { "kind", "markdown" }, { "value", s.documentation } };
		sigs.push_back( std::move( j ) );
	}
	return { { "signatures", sigs }, { "activeSignature", sh->activeSignature }, { "activeParameter", sh->activeParameter } };
}

nlohmann::json Server::semanticTokens( const json & params ) {
	json data = json::array();
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return { { "data", data } };
	EditList edits = d->editsSince( d->analysisSeq );
	struct Tok { int line, col, len, type, mods; };
	std::vector<Tok> toks;
	for ( const SemanticToken & t : d->analysis->semanticTokens( d->path ) ) {
		if ( t.length <= 0 ) continue;
		Range r{ t.start, { t.start.line, t.start.col + t.length } };
		auto m = toCurrentExact( edits, r );
		if ( ! m || m->start.line != m->end.line || ( ! edits.empty() && ! wholeName( d->text, *m ) ) ) continue;
		int c0 = d->text.toLspCol( m->start, enc ), c1 = d->text.toLspCol( m->end, enc );
		if ( c1 <= c0 ) continue;
		toks.push_back( { m->start.line, c0, c1 - c0, t.type, t.modifiers } );
	}
	std::sort( toks.begin(), toks.end(), []( const Tok & a, const Tok & b ) {
		return a.line != b.line ? a.line < b.line : a.col < b.col;
	} );
	int pl = 0, pc = 0;
	int lastEnd = -1, lastLine = -1;
	for ( const Tok & t : toks ) {
		if ( t.line == lastLine && t.col < lastEnd ) continue;	// overlapping tokens aren't allowed
		int dl = t.line - pl;
		int dc = dl == 0 ? t.col - pc : t.col;
		data.insert( data.end(), { dl, dc, t.len, t.type, t.mods } );
		pl = t.line;
		pc = t.col;
		lastLine = t.line;
		lastEnd = t.col + t.len;
	}
	return { { "data", data } };
}

std::optional<Loc> Server::cursorInSnapshot( const Document & d, const json & params ) const {
	const json & p = member( params, "position" );
	Loc cur = d.text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	EditList edits = d.editsSince( d.analysisSeq );
	MappedLoc m = toSnapshot( edits, cur );
	if ( ! m.exact || ! sameIdentifier( d, cur, edits ) ) return std::nullopt;
	return m.loc;
}

nlohmann::json Server::documentHighlight( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return json::array();
	auto pos = cursorInSnapshot( *d, params );
	if ( ! pos ) return json::array();
	json out = json::array();
	for ( const DocumentHighlight & h : d->analysis->documentHighlights( d->path, *pos ) ) {
		if ( auto j = lspLocation( *d, Location{ d->path, h.range }, true ) ) {
			out.push_back( { { "range", ( *j )["range"] }, { "kind", h.kind } } );
		}
	}
	return out;
}

nlohmann::json Server::inlayHint( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return json::array();
	const json & r = member( params, "range" );
	const json & s = member( r, "start" ), & e = member( r, "end" );
	Loc a = d->text.fromLsp( intOr( member( s, "line" ), 0 ), intOr( member( s, "character" ), 0 ), enc );
	Loc b = d->text.fromLsp( intOr( member( e, "line" ), INT_MAX ), intOr( member( e, "character" ), 0 ), enc );
	EditList edits = d->editsSince( d->analysisSeq );
	Range snap{ toSnapshot( edits, a ).loc, toSnapshot( edits, b ).loc };
	json out = json::array();
	for ( const InlayHint & h : d->analysis->inlayHints( d->path, snap ) ) {
		Loc p = h.pos;
		if ( ! edits.empty() ) {
			// The hints of a call edited since the check may be wrong.
			if ( ! toCurrentExact( edits, h.span ) ) continue;
			auto m = toCurrentExact( edits, Range{ h.pos, h.pos } );
			if ( ! m ) continue;
			p = m->start;
		}
		if ( p < a || b < p ) continue;
		out.push_back( { { "position", lspPos( d->text, p ) }, { "label", h.label }, { "kind", h.kind },
						 { "paddingLeft", false }, { "paddingRight", h.kind == 2 } } );
	}
	return out;
}

std::optional<RenamePlan> Server::renameAt( const Document & d, const json & params ) {
	auto pos = cursorInSnapshot( d, params );
	if ( ! pos ) return std::nullopt;
	auto plan = d.analysis->rename( d.path, *pos );
	if ( ! plan ) return std::nullopt;
	if ( ! plan->error.empty() ) throw LspError{ RequestFailed, plan->error };
	EditList edits = d.editsSince( d.analysisSeq );
	auto current = [&]( Range r ) {
		auto m = toCurrentExact( edits, r );
		bool spelled = false;
		if ( m && m->start.line == m->end.line && m->start.col <= m->end.col ) {
			std::string_view line = d.text.line( m->start.line );
			spelled = size_t( m->end.col ) <= line.size() && line.substr( m->start.col, m->end.col - m->start.col ) == plan->name;
		}
		if ( ! spelled || ( ! edits.empty() && ! wholeName( d.text, *m ) ) ) {
			throw LspError{ RequestFailed, "an occurrence of `" + plan->name +
											   "` changed since the file was last checked; try again after the next check" };
		}
		return *m;
	};
	plan->range = current( plan->range );
	for ( Range & r : plan->sites ) r = current( r );
	return plan;
}

nlohmann::json Server::prepareRename( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return nullptr;
	auto plan = renameAt( *d, params );
	if ( ! plan ) return nullptr;
	return { { "range", lspRange( d->text, plan->range ) }, { "placeholder", plan->name } };
}

nlohmann::json Server::rename( const json & params ) {
	const json & nn = member( params, "newName" );
	if ( ! nn.is_string() ) throw LspError{ InvalidParams, "missing newName" };
	std::string name = nn.get<std::string>();
	if ( ! text::isIdentifier( name ) || Analysis::isKeyword( name ) ) throw LspError{ InvalidParams, "`" + name + "` is not a valid identifier" };
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) throw LspError{ RequestFailed, "the file has not been checked yet" };
	auto plan = renameAt( *d, params );
	if ( ! plan ) throw LspError{ RequestFailed, "there is no symbol to rename here" };
	json edits = json::array();
	for ( const Range & r : plan->sites ) edits.push_back( { { "range", lspRange( d->text, r ) }, { "newText", name } } );
	json changes = json::object();
	changes[d->uri] = edits;
	return { { "changes", changes } };
}

// clangd's extension: the header of a source file, or the other way round, by stem.
nlohmann::json Server::switchSourceHeader( const json & params ) {
	const json & uri = member( params, "uri" );
	if ( ! uri.is_string() ) throw LspError{ InvalidParams, "missing uri" };
	auto path = uriToPath( uri.get<std::string>() );
	if ( ! path ) return nullptr;
	fs::path p = fs::path( *path ).lexically_normal();
	std::string other = p.extension() == ".cfa" ? ".hfa" : p.extension() == ".hfa" ? ".cfa" : "";
	if ( other.empty() ) return nullptr;
	fs::path next = p;
	next.replace_extension( other );
	std::error_code ec;
	if ( docs.count( next.string() ) || fs::is_regular_file( next, ec ) ) return uriOf( next.string() );
	// An open document elsewhere with the same stem, such as include/x.hfa for src/x.cfa.
	for ( const auto & [q, doc] : docs ) {
		fs::path qp( q );
		if ( qp.stem() == p.stem() && qp.extension() == other ) return doc.uri;
	}
	return nullptr;
}

// The symbols of the open documents and of the project headers they include.
nlohmann::json Server::workspaceSymbol( const json & params ) {
	const json & q = member( params, "query" );
	std::string query = q.is_string() ? q.get<std::string>() : std::string();
	// A file that is open and checked gets its symbols from its own analysis.
	std::set<std::string> owned;
	for ( const auto & [path, doc] : docs ) {
		if ( doc.analysis ) owned.insert( path );
	}
	json out = json::array();
	std::set<std::string> seen;
	for ( const auto & entry : docs ) {
		const std::string & path = entry.first;
		const Document & doc = entry.second;
		if ( ! doc.analysis ) continue;
		for ( const std::string & file : doc.analysis->projectFiles() ) {
			std::string norm = fs::path( file ).lexically_normal().string();
			if ( norm != path && owned.count( norm ) ) continue;
			std::function<void( const Symbol &, const std::string & )> add = [&]( const Symbol & s, const std::string & container ) {
				if ( out.size() >= 1000 ) return;
				if ( fuzzyMatch( s.name, query ) ) {
					if ( auto loc = lspLocation( doc, Location{ file, s.selectionRange }, false ) ) {
						json j = { { "name", s.name }, { "kind", s.kind }, { "location", *loc } };
						if ( ! container.empty() ) j["containerName"] = container;
						if ( seen.insert( j.dump() ).second ) out.push_back( std::move( j ) );
					}
				}
				for ( const auto & c : s.children ) add( c, s.name );
			};
			for ( const Symbol & s : doc.analysis->documentSymbols( file ) ) add( s, "" );
		}
	}
	return out;
}

// ---------------------------------------------------------------- diagnostics

nlohmann::json Server::diagJson( const Diag & d, const std::string & target, const std::string & source, uint64_t seq ) {
	const Text * text = &textOf( target );
	Range r = d.range;
	auto dit = docs.find( target );
	if ( target == source && dit != docs.end() ) {
		r = toCurrentClamped( dit->second.editsSince( seq ), r );
	} else if ( dit != docs.end() ) {
		// Another document's check read this file from disk.
		auto m = fromDisk( target, r, false );
		text = m->first;
		r = m->second;
	}
	if ( d.wholeLine ) {
		r = text->lineRange( text->clamp( r.start ).line );
	} else if ( r.start == r.end ) {
		Range lr = text->lineRange( text->clamp( r.start ).line );
		if ( r.start.col < lr.end.col ) r.end = lr.end;
	}
	json j = { { "range", lspRange( *text, r ) }, { "severity", d.severity }, { "source", d.source }, { "message", d.message } };
	if ( ! d.code.empty() ) j["code"] = d.code;
	if ( ! d.related.empty() ) {
		json rel = json::array();
		for ( const auto & x : d.related ) {
			const Text & rt = textOf( x.file );
			Range rr = x.range;
			if ( rr.start == rr.end ) rr = rt.lineRange( rt.clamp( rr.start ).line );
			rel.push_back( { { "location", { { "uri", uriOf( x.file ) }, { "range", lspRange( rt, rr ) } } },
							 { "message", x.message } } );
		}
		j["relatedInformation"] = rel;
	}
	return j;
}

nlohmann::json Server::publishFor( const std::string & target ) {
	json list = json::array();
	auto it = diagStore.find( target );
	if ( it != diagStore.end() ) {
		for ( const auto & [source, set] : it->second ) {
			for ( const Diag & d : set.diags ) list.push_back( diagJson( d, target, source, set.seq ) );
		}
	}
	json p = { { "uri", uriOf( target ) }, { "diagnostics", list } };
	auto d = docs.find( target );
	if ( d != docs.end() ) p["version"] = d->second.version;
	return p;
}

void Server::storeDiags( const std::string & source, uint64_t seq, const std::vector<Diag> & diags, std::vector<json> & out ) {
	std::map<std::string, std::vector<Diag>> byTarget;
	for ( const Diag & d : diags ) byTarget[d.file].push_back( d );
	byTarget[source];							// always republish the source itself
	std::set<std::string> targets;
	for ( auto & [t, set] : diagStore ) {
		if ( set.count( source ) ) targets.insert( t );
	}
	for ( auto & [t, list] : byTarget ) {
		targets.insert( t );
		if ( list.empty() ) diagStore[t].erase( source );
		else diagStore[t][source] = DiagSet{ seq, std::move( list ) };
	}
	for ( const auto & t : targets ) {
		if ( ! byTarget.count( t ) ) diagStore[t].erase( source );
		out.push_back( publishFor( t ) );
		if ( diagStore[t].empty() ) diagStore.erase( t );
	}
}

// ---------------------------------------------------------------- checking

CheckRequest Server::makeRequest( const Document & doc ) const {
	CheckRequest req;
	req.path = doc.path;
	req.text = doc.text.str();
	req.backend = opts.backend;
	req.stopAfterResolve = opts.stopAfterResolve;
	req.timeout = std::chrono::milliseconds( opts.timeoutMs );
	std::string dir = fs::path( doc.path ).parent_path().string();
	if ( opts.flags ) {
		req.flags = *opts.flags;
		req.flagsBase = rootPath.empty() ? dir : rootPath;
	} else if ( auto f = findFlagsFile( dir ) ) {
		req.flags = parseFlagsFile( readFile( *f ).value_or( "" ) );
		req.flagsBase = fs::path( *f ).parent_path().string();
	} else {
		req.flags = defaultFlags();
		req.flagsBase = dir;
	}
	return req;
}

void Server::startWorker() {
	std::lock_guard<std::mutex> lock( mtx );
	if ( worker.joinable() ) return;
	stopping = false;
	worker = std::thread( [this] { workerLoop(); } );
}

void Server::terminate() {
	stopWorker();
}

void Server::stopWorker() {
	{
		std::lock_guard<std::mutex> lock( mtx );
		stopping = true;
		if ( inflight ) inflight->token->cancel();
		cv.notify_all();
	}
	if ( worker.joinable() ) worker.join();
}

void Server::rebaseBackend( Document & doc, uint64_t seq ) {
	if ( ! doc.backDiags.empty() && doc.backSeq != seq ) {
		EditList edits = doc.editsBetween( doc.backSeq, seq );
		std::vector<Diag> kept;
		for ( Diag & d : doc.backDiags ) {
			if ( d.file == doc.path ) {
				// Backend diagnostics cover whole lines; drop the ones whose
				// line has been edited.
				int l = d.range.start.line;
				auto m = toCurrentExact( edits, Range{ { l, 0 }, { l + 1, 0 } } );
				if ( ! m || m->start.col != 0 || m->end != Loc{ m->start.line + 1, 0 } ) continue;
				d.range = { m->start, m->start };
			}
			kept.push_back( std::move( d ) );
		}
		doc.backDiags = std::move( kept );
	}
	doc.backSeq = seq;
}

void Server::workerLoop() {
	std::unique_lock<std::mutex> lk( mtx );
	for ( ;; ) {
		if ( stopping ) return;
		if ( pending.empty() ) {
			cv.wait( lk );
			continue;
		}
		auto next = std::min_element( pending.begin(), pending.end(),
									  []( const auto & a, const auto & b ) { return a.second < b.second; } );
		if ( next->second > Clock::now() ) {
			cv.wait_until( lk, next->second );
			continue;
		}
		std::string path = next->first;
		pending.erase( next );
		auto dit = docs.find( path );
		if ( dit == docs.end() ) continue;
		CheckRequest req = makeRequest( dit->second );
		uint64_t seq = dit->second.seq;
		std::shared_ptr<const Checker> chk = checker;
		auto token = std::make_shared<CancelToken>();
		inflight = InFlight{ path, seq, token, Clock::now() };
		lk.unlock();

		log::info( "checking ", path );
		auto t0 = Clock::now();
		FrontResult fr = chk->front( req, *token );
		auto ms = []( auto d ) { return std::chrono::duration_cast<std::chrono::milliseconds>( d ).count(); };
		log::info( "front end for ", path, ": status ", (int)fr.status, ", ", fr.diags.size(), " diagnostics, ", ms( Clock::now() - t0 ), " ms" );

		std::vector<json> out;
		lk.lock();
		dit = docs.find( path );
		if ( token->cancelled() || fr.status == FrontResult::Cancelled || dit == docs.end() || stopping ) {
			inflight.reset();
			continue;
		}
		Document & doc = dit->second;
		std::optional<json> refresh;
		if ( fr.analysis && ( fr.usable || ! doc.analysis ) ) {
			doc.analysis = fr.analysis;
			doc.analysisSeq = seq;
			// Clients ask for inlay hints when the text changes, not when a
			// check finishes, so the hints of a file just opened would stay empty.
			if ( inlayHintRefresh ) {
				refresh = json{ { "jsonrpc", "2.0" }, { "id", "cfa-lsp-refresh-" + std::to_string( ++refreshRequests ) },
								{ "method", "workspace/inlayHint/refresh" } };
			}
		}
		// The last backend warnings stay until the backend runs again, which
		// it can't while the translator reports errors that stop it before
		// code generation. With stopAfterResolve it never runs, so nothing
		// is carried.
		bool runBack = fr.backendReady && req.backend;
		bool carry = req.backend && ! req.stopAfterResolve && fr.status != FrontResult::Fallback &&
					 fr.status != FrontResult::NoCfa;
		std::vector<Diag> diags = fr.diags;
		if ( carry ) {
			rebaseBackend( doc, seq );
			mergeDiags( diags, doc.backDiags );
		} else {
			doc.backDiags.clear();
		}
		bool carried = ! doc.backDiags.empty();
		diskCache.clear();
		storeDiags( path, seq, diags, out );
		trimLog( doc );
		{
			std::lock_guard<std::mutex> pub( publishMtx );
			lk.unlock();
			if ( refresh ) send( *refresh );
			if ( testHook ) testHook( "publish" );
			for ( auto & p : out ) notify( "textDocument/publishDiagnostics", std::move( p ) );
		}
		out.clear();

		if ( runBack ) {
			auto t1 = Clock::now();
			std::vector<Diag> back = chk->back( req, fr, *token );
			log::info( "backend for ", path, ": ", back.size(), " diagnostics, ", ms( Clock::now() - t1 ), " ms" );
			lk.lock();
			dit = docs.find( path );
			if ( ! token->cancelled() && dit != docs.end() && ! stopping ) {
				dit->second.backDiags = back;
				dit->second.backSeq = seq;
				// The first publish had the carried warnings; replace them.
				if ( ! back.empty() || carried ) {
					std::vector<Diag> merged = fr.diags;
					mergeDiags( merged, back );
					diskCache.clear();
					storeDiags( path, seq, merged, out );
				}
			}
			{
				std::lock_guard<std::mutex> pub( publishMtx );
				lk.unlock();
				for ( auto & p : out ) notify( "textDocument/publishDiagnostics", std::move( p ) );
			}
			out.clear();
		}
		fr = FrontResult();					// removes the temp dir
		lk.lock();
		Clock::duration took = Clock::now() - inflight->started;
		inflight.reset();
		if ( auto it = docs.find( path ); it != docs.end() ) {
			if ( ! token->cancelled() ) it->second.lastCheck = took;
			trimLog( it->second );
		}
	}
}

} // namespace cfalsp
