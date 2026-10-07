#include "Server.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#include "Flags.hpp"
#include "Format.hpp"
#include "Lexer.hpp"
#include "Log.hpp"
#include "TextScan.hpp"
#include "Uri.hpp"
#include "Version.hpp"

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

// Limits for the walk of the workspace root, so a root like $HOME doesn't
// turn into hours of background checks.
const size_t maxScanEntries = 50000;
const size_t maxIndexedFiles = 1000;

// Whether `name` is spelled as a whole identifier anywhere in `text`, in
// code, comments or strings alike.
bool spellsName( std::string_view text, const std::string & name ) {
	auto ident = []( char c ) { return std::isalnum( (unsigned char)c ) || c == '_'; };
	for ( size_t p = text.find( name ); p != std::string_view::npos; p = text.find( name, p + 1 ) ) {
		bool before = p > 0 && ident( text[p - 1] );
		bool after = p + name.size() < text.size() && ident( text[p + name.size()] );
		if ( ! before && ! after ) return true;
	}
	return false;
}

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

// The name in Analysis's summary of the resolver's "No alternatives for
// expression Name: x", which is also what an undeclared function call gives.
std::optional<std::string> undeclaredName( std::string_view message ) {
	const std::string_view prefix = "use of undeclared identifier `";
	if ( ! message.starts_with( prefix ) ) return std::nullopt;
	size_t end = message.find( '`', prefix.size() );
	if ( end == std::string_view::npos ) return std::nullopt;
	std::string name( message.substr( prefix.size(), end - prefix.size() ) );
	if ( ! text::isIdentifier( name ) ) return std::nullopt;
	return name;
}

// Where `name` is spelled in `r`, a diagnostic's range in the current text:
// the first identifier with that spelling inside it, else the first on the
// range's first line.
std::optional<Range> nameIn( const Text & text, Range r, const std::string & name ) {
	std::optional<Range> fallback;
	int last = std::min( r.end.line, r.start.line + 50 );
	for ( int l = r.start.line; l <= last; l += 1 ) {
		for ( const Token & t : lex( text.line( l ) ) ) {
			if ( t.kind != TokKind::Identifier || t.text != name || t.endLine != t.line ) continue;
			Range at{ { l, t.col }, { l, t.endCol } };
			if ( ! ( at.start < r.start ) && ! ( r.end < at.end ) ) return at;
			if ( ! fallback && l == r.start.line ) fallback = at;
		}
	}
	return fallback;
}

// The parser's name for "X" in `illegal syntax, adjacent identifiers "X" and
// "Y" ...` and `illegal syntax, identifier "X" cannot appear before a type`:
// what a type used without its header (`string s;` without string.hfa) gives.
std::optional<std::string> identifierBeforeIdentifier( std::string_view message ) {
	for ( std::string_view prefix : { "illegal syntax, adjacent identifiers \"", "illegal syntax, identifier \"" } ) {
		if ( ! message.starts_with( prefix ) ) continue;
		size_t end = message.find( '"', prefix.size() );
		if ( end == std::string_view::npos ) return std::nullopt;
		std::string name( message.substr( prefix.size(), end - prefix.size() ) );
		if ( text::isIdentifier( name ) ) return name;
	}
	return std::nullopt;
}

// The identifier right before an identifier at `at`, for a plain syntax error
// there.
std::optional<std::string> identifierBefore( const Text & text, Loc at ) {
	std::vector<Token> toks = lex( text.line( at.line ) );
	for ( size_t i = 1; i < toks.size(); i += 1 ) {
		if ( toks[i].col != at.col ) continue;
		if ( toks[i].kind == TokKind::Identifier && toks[i - 1].kind == TokKind::Identifier && text::isIdentifier( toks[i - 1].text ) ) {
			return toks[i - 1].text;
		}
		break;
	}
	return std::nullopt;
}

struct Includes {
	int line = 0;							// where a new #include goes
	std::set<std::string> headers;			// already included, as written and by file name
};

// A new #include goes after the last one before the first line of code, or
// on that line if there is none.
Includes includesOf( const Text & text ) {
	LexOptions lo;
	lo.comments = true;
	lo.directives = true;
	Includes out;
	int after = -1, code = -1;
	for ( const Token & t : lex( text.str(), lo ) ) {
		if ( t.kind == TokKind::Comment ) continue;
		if ( t.kind != TokKind::Directive ) {
			if ( code < 0 ) code = t.line;
			continue;
		}
		std::string_view d = t.text;
		size_t i = d.find_first_not_of( " \t", 1 );
		if ( i == std::string_view::npos || d.compare( i, 7, "include" ) != 0 ) continue;
		size_t open = d.find_first_of( "<\"", i + 7 );
		size_t close = open == std::string_view::npos ? open : d.find( d[open] == '<' ? '>' : '"', open + 1 );
		if ( close != std::string_view::npos ) {
			std::string name( d.substr( open + 1, close - open - 1 ) );
			out.headers.insert( name );
			out.headers.insert( name.substr( name.rfind( '/' ) + 1 ) );
		}
		if ( code < 0 ) after = t.endLine;
	}
	out.line = after >= 0 ? after + 1 : code >= 0 ? code : 0;
	return out;
}

nlohmann::json quickFix( const std::string & title, const nlohmann::json & diag, const std::string & uri, nlohmann::json edit ) {
	nlohmann::json changes = nlohmann::json::object();
	changes[uri] = nlohmann::json::array( { std::move( edit ) } );
	return { { "title", title }, { "kind", "quickfix" }, { "diagnostics", nlohmann::json::array( { diag } ) },
			 { "edit", { { "changes", changes } } } };
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
	if ( method == "textDocument/codeAction" ) return codeAction( params );
	if ( method == "textDocument/formatting" ) return formatting( params, false );
	if ( method == "textDocument/rangeFormatting" ) return formatting( params, true );
	if ( method == "textDocument/prepareCallHierarchy" ) return prepareCallHierarchy( params );
	if ( method == "callHierarchy/incomingCalls" ) return calls( params, true );
	if ( method == "callHierarchy/outgoingCalls" ) return calls( params, false );
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
		if ( watchFiles && indexing() ) {
			// Changes made outside the editor (git checkout, another editor)
			// reach the index through these.
			json watcher = { { "globPattern", "**/*.{cfa,hfa}" } };
			json reg = { { "id", "cfa-lsp-watch" }, { "method", "workspace/didChangeWatchedFiles" },
						 { "registerOptions", { { "watchers", json::array( { watcher } ) } } } };
			sendRequest( "client/registerCapability", { { "registrations", json::array( { reg } ) } } );
		}
		return;
	}
	if ( phase == Phase::ShuttingDown ) return;
	if ( method == "textDocument/didOpen" ) didOpen( params );
	else if ( method == "textDocument/didChange" ) didChange( params );
	else if ( method == "textDocument/didClose" ) didClose( params );
	else if ( method == "textDocument/didSave" ) didSave( params );
	else if ( method == "workspace/didChangeConfiguration" ) didChangeConfiguration( params );
	else if ( method == "workspace/didChangeWatchedFiles" ) {
		std::lock_guard<std::mutex> lock( mtx );
		indexRescan = true;
		cv.notify_all();
	}
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
	watchFiles = member( member( ws, "didChangeWatchedFiles" ), "dynamicRegistration" ) == true;

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
	{
		std::lock_guard<std::mutex> lock( mtx );
		indexRescan = true;
		cv.notify_all();
	}

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
		{ "codeActionProvider", { { "codeActionKinds", json::array( { "quickfix" } ) } } },
		{ "documentFormattingProvider", true },
		{ "documentRangeFormattingProvider", true },
		{ "callHierarchyProvider", true },
	};
	return { { "capabilities", capabilities }, { "serverInfo", { { "name", "cfa-lsp" }, { "version", cfalsp::version } } } };
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
	if ( member( io, "skipSystemBodies" ).is_boolean() ) o.skipSystemBodies = member( io, "skipSystemBodies" ).get<bool>();
	if ( member( io, "index" ).is_boolean() ) o.index = member( io, "index" ).get<bool>();
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
	headers.reset();						// cfa may have moved
	effective = io;
	// Other flags or tools give other results: check everything again.
	if ( ! opts.index ) index.clear();
	for ( auto & [path, e] : index ) {
		e.stale = true;
		e.gen += 1;
	}
	indexRescan = true;
	cv.notify_all();
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
	for ( const char * k : { "cfa", "translator", "preludeDir", "flags", "backend", "stopAfterResolve", "skipSystemBodies", "index", "cc",
							  "debounceMs", "timeoutMs" } ) {
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
	if ( inflight && ! inflight->index && inflight->path == d->path && ! inflight->token->cancelled() &&
		 d->lastCheck > Clock::duration::zero() ) {
		Clock::duration ran = Clock::now() - inflight->started;
		if ( ran > 2 * d->lastCheck ) {
			inflight->token->cancel();
			d->lastCheck = ran;
		}
	}
	schedule( d->path, opts.debounceMs );
	// Checks of the files that include it read the buffer.
	if ( isHeader( d->path ) ) scheduleDependents( d->path );
	trimLog( *d );
}

void Server::didClose( const json & params ) {
	std::vector<json> out;
	std::unique_lock<std::mutex> lock( mtx );
	Document * d = docFor( params );
	if ( ! d ) return;
	std::string path = d->path;
	bool unsaved = ! d->diskSeq || *d->diskSeq != d->seq;
	pending.erase( path );
	if ( inflight && ! inflight->index && inflight->path == path ) inflight->token->cancel();
	diskCache.clear();
	storeDiags( path, 0, {}, {}, out );
	docs.erase( path );
	// Files that include it read it from disk again.
	if ( unsaved && isHeader( path ) ) scheduleDependents( path );
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
	bool covered = inflight && ! inflight->index && inflight->path == d->path && inflight->seq == d->seq;
	if ( ! covered ) schedule( d->path, 0 );
	if ( isHeader( d->path ) ) {
		for ( auto & [p, other] : docs ) {
			if ( p != d->path ) schedule( p, opts.debounceMs );
		}
	}
	// Files whose text or headers changed on disk are indexed again.
	indexRescan = true;
	cv.notify_all();
}

void Server::schedule( const std::string & path, int delayMs ) {
	auto due = Clock::now() + std::chrono::milliseconds( delayMs );
	auto it = pending.find( path );
	if ( it == pending.end() || delayMs > 0 || due < it->second ) pending[path] = due;
	// Index checks give way to the user's files.
	if ( inflight && inflight->index ) inflight->token->cancel();
	cv.notify_all();
}

void Server::scheduleDependents( const std::string & header ) {
	for ( auto & [p, other] : docs ) {
		if ( p != header && std::find( other.includes.begin(), other.includes.end(), header ) != other.includes.end() ) {
			schedule( p, opts.debounceMs );
		}
	}
}

void Server::trimLog( Document & doc ) {
	uint64_t keep = doc.analysis ? doc.analysisSeq : doc.seq;
	auto ds = diagStore.find( doc.path );
	if ( ds != diagStore.end() ) {
		auto own = ds->second.find( doc.path );
		if ( own != ds->second.end() ) keep = std::min( keep, own->second.seq );
	}
	if ( inflight && ! inflight->index && inflight->path == doc.path ) keep = std::min( keep, inflight->seq );
	if ( doc.diskSeq ) keep = std::min( keep, *doc.diskSeq );
	// Checks of other documents that read this buffer.
	auto readAt = [&]( const SeqMap & reads ) {
		if ( auto r = reads.find( doc.path ); r != reads.end() ) keep = std::min( keep, r->second );
	};
	for ( const auto & [p, other] : docs ) {
		if ( other.analysis ) readAt( other.analysisReads );
	}
	for ( const auto & [target, bySource] : diagStore ) {
		for ( const auto & [source, set] : bySource ) readAt( set.reads );
	}
	if ( inflight && ! inflight->index ) readAt( inflight->reads );
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

const HeaderIndex & Server::headerIndex() {
	if ( ! headers ) {
		std::string prefix = checker ? checker->toolchain().cfaPrefix : std::string();
		headers = prefix.empty() ? HeaderIndex() : HeaderIndex::scan( prefix + "/include/cfa" );
	}
	return *headers;
}

std::string Server::uriOf( const std::string & path ) const {
	auto d = docs.find( path );
	return d != docs.end() ? d->second.uri : pathToUri( path );
}

Server::Snapshot Server::snapOf( const Document & d ) const { return { d.path, d.analysisSeq, d.analysisReads }; }

EditList Server::editsFor( const Snapshot & snap, const std::string & file, const Text *& text ) {
	auto d = docs.find( file );
	if ( d != docs.end() ) {
		const Document & doc = d->second;
		std::optional<uint64_t> since = doc.diskSeq;
		if ( file == snap.main ) since = snap.mainSeq;
		else if ( auto r = snap.reads.find( file ); r != snap.reads.end() ) since = r->second;
		if ( since ) {
			text = &doc.text;
			return doc.editsSince( *since );
		}
	}
	// Not open, or open with contents that differ from the disk in unknown ways.
	text = &diskText( file );
	return {};
}

std::optional<Server::Mapped> Server::toClient( const Snapshot & snap, const Location & loc, bool exact ) {
	std::string file = fs::path( loc.file ).lexically_normal().string();
	if ( file.empty() || file[0] != '/' ) return std::nullopt;
	const Text * text = nullptr;
	EditList edits = editsFor( snap, file, text );
	Range r;
	if ( exact ) {
		auto m = toCurrentExact( edits, loc.range );
		if ( ! m || ( ! edits.empty() && ! wholeName( *text, *m ) ) ) return std::nullopt;
		r = *m;
	} else {
		r = toCurrentClamped( edits, loc.range );
	}
	return Mapped{ text, { file, r } };
}

std::optional<nlohmann::json> Server::lspLocation( const Snapshot & snap, const Location & loc, bool exact ) {
	auto m = toClient( snap, loc, exact );
	if ( ! m ) return std::nullopt;
	return json{ { "uri", uriOf( m->loc.file ) }, { "range", lspRange( *m->text, m->loc.range ) } };
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
		// A prototype whose body is in another file (a header and its .cfa): go
		// to the body, from another open document or the index.
		std::vector<Location> decls = d->analysis->declarationsAt( d->path, m.loc );
		if ( ! decls.empty() && ! d->analysis->definitionOf( decls ) ) {
			for ( auto & [path, other] : docs ) {
				if ( &other == d || ! other.analysis ) continue;
				if ( auto def = other.analysis->definitionOf( decls ) ) {
					if ( auto j = lspLocation( other, *def, true ) ) return json::array( { *j } );
				}
			}
			if ( auto t = targetAt( *d, m.loc ) ) {
				std::vector<Source> srcs = sources();
				for ( const Match & mt : matchEntities( srcs, t->name, t->keys ) ) {
					const Source & s = srcs[mt.source];
					const auto & def = s.table->entities[mt.entity].definition;
					if ( ! def || hidden( s, def->file ) ) continue;
					if ( auto j = lspLocation( s.snap, *def, true ) ) return json::array( { *j } );
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
	// Each open document's table has the uses in that document, and the
	// index those in the files on disk.
	if ( auto t = targetAt( *d, m.loc ) ) {
		std::vector<Source> srcs = sources();
		for ( const Match & mt : matchEntities( srcs, t->name, t->keys ) ) {
			const Source & s = srcs[mt.source];
			for ( const auto & r : s.table->refs ) {
				if ( r.entity == mt.entity && ! hidden( s, r.loc.file ) ) {
					if ( auto j = lspLocation( s.snap, r.loc, true ); j && seen.insert( j->dump() ).second ) out.push_back( *j );
				}
			}
			if ( ! incl ) continue;
			for ( const Location & l : s.table->entities[mt.entity].declarations ) {
				if ( hidden( s, l.file ) ) continue;
				if ( auto j = lspLocation( s.snap, l, true ); j && seen.insert( j->dump() ).second ) out.push_back( *j );
			}
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
	if ( ! d ) return { { "data", data } };
	struct Tok { int line, col, len, type, mods; };
	std::vector<Tok> toks;
	// From the buffer as it is now, so they need no mapping.
	for ( const SemanticToken & t : Analysis::keywordTokens( d->text.str() ) ) {
		int c0 = d->text.toLspCol( t.start, enc ), c1 = d->text.toLspCol( { t.start.line, t.start.col + t.length }, enc );
		if ( c1 > c0 ) toks.push_back( { t.start.line, c0, c1 - c0, t.type, t.modifiers } );
	}
	std::vector<SemanticToken> named;
	EditList edits;
	if ( d->analysis ) {
		named = d->analysis->semanticTokens( d->path );
		edits = d->editsSince( d->analysisSeq );
	}
	for ( const SemanticToken & t : named ) {
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
	RenameSites sites;
	auto plan = renameAcross( *d, params, sites );
	if ( ! plan ) plan = renameAt( *d, params );
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
	RenameSites sites;
	if ( renameAcross( *d, params, sites ) ) {
		json changes = json::object();
		for ( const auto & [file, entry] : sites ) {
			json edits = json::array();
			for ( const Range & r : entry.second ) edits.push_back( { { "range", lspRange( *entry.first, r ) }, { "newText", name } } );
			changes[uriOf( file )] = edits;
		}
		return { { "changes", changes } };
	}
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

// The symbols of the open documents and of the project headers they include,
// then those of the other files in the index.
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
	// The files no open document includes, from the index.
	for ( const auto & [path, e] : index ) {
		if ( ! e.table ) continue;
		Source src{ e.table, Snapshot(), true };
		for ( const auto & sym : e.table->symbols ) {
			if ( out.size() >= 1000 ) break;
			if ( owned.count( fs::path( sym.loc.file ).lexically_normal().string() ) || ! fuzzyMatch( sym.name, query ) ) continue;
			if ( auto loc = lspLocation( src.snap, sym.loc, false ) ) {
				json j = { { "name", sym.name }, { "kind", sym.kind }, { "location", *loc } };
				if ( ! sym.container.empty() ) j["containerName"] = sym.container;
				if ( seen.insert( j.dump() ).second ) out.push_back( std::move( j ) );
			}
		}
	}
	return out;
}

nlohmann::json Server::codeAction( const json & params ) {
	json out = json::array();
	Document * d = docFor( params );
	if ( ! d ) return out;
	const json & context = member( params, "context" );
	const json & only = member( context, "only" );
	if ( only.is_array() && std::none_of( only.begin(), only.end(), []( const json & k ) { return k == "quickfix"; } ) ) return out;
	const json & diags = member( context, "diagnostics" );
	if ( ! diags.is_array() ) return out;
	auto pos = [&]( const json & p ) {
		return d->text.fromLsp( intOr( member( p, "line" ), 0 ), intOr( member( p, "character" ), 0 ), enc );
	};
	std::optional<Includes> includes;
	auto addIncludes = [&]( std::vector<std::string> found, const json & diag ) {
		if ( found.size() > 3 ) found.resize( 3 );
		for ( const std::string & h : found ) {
			if ( ! includes ) includes = includesOf( d->text );
			if ( includes->headers.count( h ) ) continue;
			Loc at{ includes->line, 0 };
			std::string ins = "#include <" + h + ">\n";
			if ( at.line >= d->text.lineCount() ) {				// after a last line with no newline
				at = d->text.loc( d->text.str().size() );
				ins = "\n#include <" + h + ">";
			}
			out.push_back( quickFix( "Add #include <" + h + ">", diag, d->uri,
									 { { "range", lspRange( d->text, { at, at } ) }, { "newText", ins } } ) );
		}
	};
	for ( const json & diag : diags ) {
		const json & msg = member( diag, "message" );
		const json & source = member( diag, "source" );
		if ( ! msg.is_string() || ( source.is_string() && source != "cfa" ) ) continue;
		std::string message = msg.get<std::string>();
		message = message.substr( 0, message.find( '\n' ) );
		const json & r = member( diag, "range" );
		Range range{ pos( member( r, "start" ) ), pos( member( r, "end" ) ) };
		if ( auto name = undeclaredName( message ) ) {
			auto at = nameIn( d->text, range, *name );
			if ( ! at ) continue;
			// An exact name from a header is a likelier fix than a near one.
			addIncludes( headerIndex().headersFor( *name ), diag );
			if ( ! d->analysis ) continue;
			Loc snap = toSnapshot( d->editsSince( d->analysisSeq ), at->start ).loc;
			for ( const std::string & s : d->analysis->similarNames( d->path, snap, *name ) ) {
				out.push_back( quickFix( "Change `" + *name + "` to `" + s + "`", diag, d->uri,
										 { { "range", lspRange( d->text, *at ) }, { "newText", s } } ) );
			}
		} else if ( auto type = identifierBeforeIdentifier( message ) ) {
			addIncludes( headerIndex().headersFor( *type, true ), diag );
		} else if ( message.starts_with( "syntax error" ) ) {
			if ( auto type = identifierBefore( d->text, range.start ) ) addIncludes( headerIndex().headersFor( *type, true ), diag );
		}
	}
	return out;
}

nlohmann::json Server::formatting( const json & params, bool range ) {
	Document * d = docFor( params );
	if ( ! d ) return nullptr;
	const json & o = member( params, "options" );
	auto flag = [&]( const char * key, bool dflt ) {
		const json & v = member( o, key );
		return v.is_boolean() ? v.get<bool>() : dflt;
	};
	FormatOptions fo;
	fo.tabSize = intOr( member( o, "tabSize" ), 4 );
	fo.insertSpaces = flag( "insertSpaces", true );
	fo.trimTrailingWhitespace = flag( "trimTrailingWhitespace", true );
	fo.insertFinalNewline = flag( "insertFinalNewline", false );
	fo.trimFinalNewlines = flag( "trimFinalNewlines", false );
	int first = 0, last = INT_MAX;
	if ( range ) {
		const json & r = member( params, "range" );
		const json & s = member( r, "start" ), & e = member( r, "end" );
		Loc a = d->text.fromLsp( intOr( member( s, "line" ), 0 ), intOr( member( s, "character" ), 0 ), enc );
		Loc b = d->text.fromLsp( intOr( member( e, "line" ), 0 ), intOr( member( e, "character" ), 0 ), enc );
		first = a.line;
		last = b.line > a.line && b.col == 0 ? b.line - 1 : b.line;	// a range ending at a line start leaves that line alone
	}
	std::string formatted = formatText( d->text.str(), fo, first, last );
	json edits = json::array();
	for ( const auto & [r, s] : diffEdits( d->text, formatted ) ) {
		edits.push_back( { { "range", lspRange( d->text, r ) }, { "newText", s } } );
	}
	return edits;
}

// ---------------------------------------------------------------- cross-file queries

std::vector<Server::Source> Server::sources() {
	std::vector<Source> out;
	for ( const auto & [path, d] : docs ) {
		if ( d.table ) out.push_back( { d.table, snapOf( d ), false } );
	}
	for ( const auto & [path, e] : index ) {
		if ( e.table ) out.push_back( { e.table, Snapshot(), true } );
	}
	return out;
}

// The index read files from disk; an open document's own check knows its
// current contents better.
bool Server::hidden( const Source & s, const std::string & file ) const {
	if ( ! s.index ) return false;
	auto d = docs.find( fs::path( file ).lexically_normal().string() );
	return d != docs.end() && d->second.analysis;
}

std::optional<Server::Target> Server::targetAt( const Document & d, Loc pos ) {
	if ( ! d.analysis || ! d.table ) return std::nullopt;
	std::vector<Location> decls = d.analysis->declarationsAt( d.path, pos );
	if ( decls.empty() ) return std::nullopt;
	for ( const auto & e : d.table->entities ) {
		bool same = false;
		for ( const Location & l : e.declarations ) {
			if ( std::find( decls.begin(), decls.end(), l ) != decls.end() ) {
				same = true;
				break;
			}
		}
		if ( ! same ) continue;
		Target t;
		t.name = e.name;
		t.library = e.library;
		t.function = e.function;
		t.kind = e.kind;
		Snapshot snap = snapOf( d );
		for ( const Location & l : decls ) {
			if ( auto m = toClient( snap, l, true ) ) t.keys.insert( { m->loc.file, m->loc.range.start } );
		}
		return t;
	}
	return std::nullopt;
}

std::set<Server::Key> Server::keysOf( const Source & s, int entity ) {
	std::set<Key> keys;
	for ( const Location & l : s.table->entities[entity].declarations ) {
		if ( auto m = toClient( s.snap, l, true ) ) keys.insert( { m->loc.file, m->loc.range.start } );
	}
	return keys;
}

// The entities called `name` with a declaration in `keys`. Their other
// declarations join `keys`, so a prototype that one unit sees links to the
// definition that another unit sees.
std::vector<Server::Match> Server::matchEntities( const std::vector<Source> & srcs, const std::string & name,
												  std::set<Key> & keys ) {
	struct Cand {
		Match m;
		std::set<Key> keys;
		bool in = false;
	};
	std::vector<Cand> cands;
	for ( size_t s = 0; s < srcs.size(); s += 1 ) {
		const auto & ents = srcs[s].table->entities;
		for ( int e = 0; e < int( ents.size() ); e += 1 ) {
			if ( ents[e].name == name ) cands.push_back( { { s, e }, keysOf( srcs[s], e ) } );
		}
	}
	for ( bool grew = true; grew; ) {
		grew = false;
		for ( Cand & c : cands ) {
			if ( c.in ) continue;
			bool hit = false;
			for ( const Key & k : c.keys ) hit = hit || keys.count( k );
			if ( ! hit ) continue;
			c.in = grew = true;
			keys.insert( c.keys.begin(), c.keys.end() );
		}
	}
	std::vector<Match> out;
	for ( const Cand & c : cands ) {
		if ( c.in ) out.push_back( c.m );
	}
	return out;
}

// A CallHierarchyItem: at the definition if one of `matches` has it, else at
// the first declaration. `data` carries the declarations for the follow-up
// requests.
std::optional<nlohmann::json> Server::callItem( const std::vector<Source> & srcs, const std::vector<Match> & matches,
											   const std::set<Key> & keys ) {
	const Source * src = nullptr;
	const UnitIndex::Entity * ent = nullptr;
	for ( const Match & mt : matches ) {
		const Source & s = srcs[mt.source];
		const UnitIndex::Entity & e = s.table->entities[mt.entity];
		if ( e.definition && ! hidden( s, e.definition->file ) ) {
			src = &s;
			ent = &e;
			break;
		}
		if ( ! ent && ! e.declarations.empty() ) {
			src = &s;
			ent = &e;
		}
	}
	if ( ! ent ) return std::nullopt;
	std::optional<Mapped> sel, whole;
	if ( ent->definition ) {
		sel = toClient( src->snap, *ent->definition, false );
		whole = toClient( src->snap, { ent->definition->file, ent->definitionRange }, false );
	} else {
		sel = whole = toClient( src->snap, ent->declarations.front(), false );
	}
	if ( ! sel || ! whole ) return std::nullopt;
	Range r = whole->loc.range;
	if ( sel->loc.range.start < r.start || r.end < sel->loc.range.end ) r = sel->loc.range;
	json data = json::array();
	for ( const auto & [file, at] : keys ) data.push_back( { file, at.line, at.col } );
	json item = { { "name", ent->name }, { "kind", ent->kind }, { "uri", uriOf( sel->loc.file ) },
				  { "range", lspRange( *whole->text, r ) }, { "selectionRange", lspRange( *sel->text, sel->loc.range ) },
				  { "data", { { "name", ent->name }, { "keys", data } } } };
	if ( ! ent->detail.empty() ) item["detail"] = ent->detail;
	return item;
}

std::pair<std::string, std::optional<nlohmann::json>> Server::resolveEntity( const std::vector<Source> & srcs, size_t source,
																			int entity ) {
	const std::string & name = srcs[source].table->entities[entity].name;
	std::set<Key> keys = keysOf( srcs[source], entity );
	std::vector<Match> matches = matchEntities( srcs, name, keys );
	if ( matches.empty() ) matches.push_back( { source, entity } );
	std::string id = name + '\x1f';
	if ( keys.empty() ) {
		id += std::to_string( source ) + ':' + std::to_string( entity );
	} else {
		const Key & k = *keys.begin();
		id += k.first + ':' + std::to_string( k.second.line ) + ':' + std::to_string( k.second.col );
	}
	return { id, callItem( srcs, matches, keys ) };
}

// The name and declarations a CallHierarchyItem stands for.
std::pair<std::string, std::set<Server::Key>> Server::itemKeys( const json & item ) {
	std::set<Key> keys;
	const json & data = member( item, "data" );
	std::string name = member( data, "name" ).is_string() ? member( data, "name" ).get<std::string>()
		: member( item, "name" ).is_string() ? member( item, "name" ).get<std::string>() : "";
	const json & ks = member( data, "keys" );
	if ( ks.is_array() ) {
		for ( const auto & k : ks ) {
			if ( k.is_array() && k.size() == 3 && k[0].is_string() && k[1].is_number_integer() && k[2].is_number_integer() ) {
				keys.insert( { k[0].get<std::string>(), Loc{ k[1].get<int>(), k[2].get<int>() } } );
			}
		}
	}
	if ( keys.empty() && member( item, "uri" ).is_string() ) {
		if ( auto path = uriToPath( item["uri"].get<std::string>() ) ) {
			std::string file = fs::path( *path ).lexically_normal().string();
			const json & st = member( member( item, "selectionRange" ), "start" );
			Loc at = textOf( file ).fromLsp( intOr( member( st, "line" ), 0 ), intOr( member( st, "character" ), 0 ), enc );
			keys.insert( { file, at } );
		}
	}
	return { name, keys };
}

nlohmann::json Server::prepareCallHierarchy( const json & params ) {
	Document * d = docFor( params );
	if ( ! d || ! d->analysis ) return nullptr;
	auto pos = cursorInSnapshot( *d, params );
	if ( ! pos ) return nullptr;
	auto t = targetAt( *d, *pos );
	if ( ! t || ! t->function ) return nullptr;
	std::vector<Source> srcs = sources();
	std::vector<Match> matches = matchEntities( srcs, t->name, t->keys );
	auto item = callItem( srcs, matches, t->keys );
	if ( ! item ) return nullptr;
	return json::array( { *item } );
}

// Incoming: the calls of the item's function, grouped by the function they
// are in. Outgoing: the calls in the item's function, grouped by callee.
nlohmann::json Server::calls( const json & params, bool incoming ) {
	auto [name, keys] = itemKeys( member( params, "item" ) );
	json out = json::array();
	if ( keys.empty() ) return out;
	std::vector<Source> srcs = sources();
	struct Group {
		json item;
		json ranges = json::array();
		std::set<std::string> seen;
	};
	std::map<std::pair<size_t, int>, std::string> ids;
	std::map<std::string, Group> groups;
	std::vector<std::string> order;
	for ( const Match & mt : matchEntities( srcs, name, keys ) ) {
		const Source & s = srcs[mt.source];
		for ( const auto & r : s.table->refs ) {
			if ( ! r.call || hidden( s, r.loc.file ) ) continue;
			if ( incoming ? r.entity != mt.entity || r.caller < 0 : r.caller != mt.entity ) continue;
			int other = incoming ? r.caller : r.entity;
			// Operators of the prelude and libcfa (int + int, sout | x) are noise here.
			const UnitIndex::Entity & callee = s.table->entities[r.entity];
			if ( ! incoming && callee.library && callee.name.find( '?' ) != std::string::npos ) continue;
			auto at = toClient( s.snap, r.loc, true );
			if ( ! at ) continue;
			auto idIt = ids.find( { mt.source, other } );
			if ( idIt == ids.end() ) {
				auto [id, item] = resolveEntity( srcs, mt.source, other );
				idIt = ids.emplace( std::pair{ mt.source, other }, id ).first;
				if ( ! groups.count( id ) && item ) {
					groups[id].item = *item;
					order.push_back( id );
				}
			}
			auto g = groups.find( idIt->second );
			if ( g == groups.end() ) continue;
			json range = lspRange( *at->text, at->loc.range );
			if ( g->second.seen.insert( range.dump() ).second ) g->second.ranges.push_back( range );
		}
	}
	for ( const std::string & id : order ) {
		out.push_back( { { incoming ? "from" : "to", groups[id].item }, { "fromRanges", groups[id].ranges } } );
	}
	return out;
}

std::optional<RenamePlan> Server::renameAcross( const Document & d, const json & params, RenameSites & sites ) {
	if ( ! d.table ) return std::nullopt;
	auto pos = cursorInSnapshot( d, params );
	if ( ! pos ) return std::nullopt;
	auto t = targetAt( d, *pos );
	if ( ! t ) return std::nullopt;						// a local
	bool elsewhere = isHeader( d.path );
	for ( const Key & k : t->keys ) elsewhere = elsewhere || k.first != d.path;
	if ( ! indexing() ) return std::nullopt;
	if ( ! elsewhere ) {
		// Declared only in this .cfa file, so the single-file rename applies.
		// But a function or variable declared again in another .cfa file
		// (`double f( int );` instead of a shared header) may be the same
		// one, and the index can't tell, since it matches by location.
		if ( t->kind == 12 || t->kind == 13 ) {
			for ( const Source & s : sources() ) {
				for ( const auto & e : s.table->entities ) {
					if ( e.name != t->name || e.kind != t->kind || e.library ) continue;
					for ( const Location & l : e.declarations ) {
						std::string file = fs::path( l.file ).lexically_normal().string();
						if ( file == d.path || hidden( s, file ) ) continue;
						throw LspError{ RequestFailed, "`" + t->name + "` is also declared in `" + fs::path( file ).filename().string() +
														   "`, which may be the same one; rename it by hand" };
					}
				}
			}
		}
		return std::nullopt;
	}
	auto plan = d.analysis->rename( d.path, *pos, true );
	if ( ! plan ) return std::nullopt;
	if ( ! plan->error.empty() ) throw LspError{ RequestFailed, plan->error };
	// A file nobody has checked yet may use it.
	bool building = indexRescan || indexScanning || ( inflight && inflight->index );
	for ( const auto & [path, e] : index ) building = building || e.stale;
	if ( building ) throw LspError{ RequestFailed, "the workspace index is still being built; try again in a moment" };
	if ( indexTruncated ) {
		throw LspError{ RequestFailed, "the workspace root has more files than the index checks, so some uses may be missing; "
									   "rename it by hand" };
	}

	const std::string quoted = "`" + plan->name + "`";
	// A file whose check failed has uses the index doesn't know.
	for ( const auto & [path, e] : index ) {
		if ( e.complete || hidden( Source{ nullptr, Snapshot(), true }, path ) ) continue;
		if ( auto text = readFile( path ); text && spellsName( *text, plan->name ) ) {
			throw LspError{ RequestFailed, quoted + " appears in " + fs::path( path ).filename().string() +
											   ", which the index could not check; fix its errors or rename it by hand" };
		}
	}
	auto add = [&]( const Snapshot & snap, const Location & l ) {
		std::string file = fs::path( l.file ).lexically_normal().string();
		std::string where = fs::path( file ).filename().string() + " line " + std::to_string( l.range.start.line + 1 );
		auto m = toClient( snap, l, true );
		auto doc = docs.find( file );
		if ( ! m || ( doc != docs.end() && m->text != &doc->second.text ) ) {
			throw LspError{ RequestFailed, "the occurrence of " + quoted + " in " + where +
											   " changed since it was last checked; try again after the next check" };
		}
		const Range & r = m->loc.range;
		std::string_view line = m->text->line( r.start.line );
		if ( r.start.line != r.end.line || r.end.col > (int)line.size() || line.substr( r.start.col, r.end.col - r.start.col ) != plan->name ) {
			throw LspError{ RequestFailed, "the use of " + quoted + " in " + where + " comes from a macro; rename it by hand" };
		}
		auto & entry = sites[file];
		entry.first = m->text;
		if ( std::find( entry.second.begin(), entry.second.end(), r ) == entry.second.end() ) entry.second.push_back( r );
	};
	Snapshot snap = snapOf( d );
	for ( const Range & r : plan->sites ) add( snap, { d.path, r } );
	if ( auto m = toClient( snap, { d.path, plan->range }, true ) ) plan->range = m->loc.range;
	else throw LspError{ RequestFailed, "the file changed since it was last checked; try again after the next check" };
	plan->sites.clear();

	std::vector<Source> srcs = sources();
	for ( const Match & mt : matchEntities( srcs, t->name, t->keys ) ) {
		const Source & s = srcs[mt.source];
		for ( const auto & r : s.table->refs ) {
			if ( r.entity == mt.entity && ! hidden( s, r.loc.file ) ) add( s.snap, r.loc );
		}
		for ( const Location & l : s.table->entities[mt.entity].declarations ) {
			if ( ! hidden( s, l.file ) ) add( s.snap, l );
		}
	}
	// Spellings the translator has no use for, in the files the rename edits,
	// and in macro bodies anywhere.
	for ( const Source & s : srcs ) {
		for ( const auto & l : s.table->loose ) {
			if ( l.name != plan->name || hidden( s, l.loc.file ) ) continue;
			std::string file = fs::path( l.loc.file ).lexically_normal().string();
			std::string where = "line " + std::to_string( l.loc.range.start.line + 1 ) + " of " + fs::path( file ).filename().string();
			if ( l.macro ) {
				throw LspError{ RequestFailed, quoted + " is used in the macro on " + where +
												   ", which the translator doesn't see; rename it by hand" };
			}
			if ( file == d.path || ! sites.count( file ) ) continue;		// rename() checked this file
			throw LspError{ RequestFailed, quoted + " appears on " + where +
											   " where the translator recorded no use (an array dimension, a designator, dead code "
											   "or code that failed to resolve); rename it by hand" };
		}
	}
	return plan;
}

// ---------------------------------------------------------------- diagnostics

nlohmann::json Server::diagJson( const Diag & d, const std::string & target, const std::string & source, const DiagSet & set ) {
	const Text * text = &textOf( target );
	Range r = d.range;
	auto dit = docs.find( target );
	if ( target == source && dit != docs.end() ) {
		r = toCurrentClamped( dit->second.editsSince( set.seq ), r );
	} else if ( auto rd = set.reads.find( target ); rd != set.reads.end() && dit != docs.end() ) {
		// Another document's check read this file from its buffer.
		r = toCurrentClamped( dit->second.editsSince( rd->second ), r );
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
			for ( const Diag & d : set.diags ) list.push_back( diagJson( d, target, source, set ) );
		}
	}
	json p = { { "uri", uriOf( target ) }, { "diagnostics", list } };
	auto d = docs.find( target );
	if ( d != docs.end() ) p["version"] = d->second.version;
	return p;
}

void Server::storeDiags( const std::string & source, uint64_t seq, const SeqMap & reads, const std::vector<Diag> & diags,
						 std::vector<json> & out ) {
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
		else diagStore[t][source] = DiagSet{ seq, reads, std::move( list ) };
	}
	for ( const auto & t : targets ) {
		if ( ! byTarget.count( t ) ) diagStore[t].erase( source );
		out.push_back( publishFor( t ) );
		if ( diagStore[t].empty() ) diagStore.erase( t );
	}
}

// ---------------------------------------------------------------- checking

CheckRequest Server::makeRequest( const std::string & path, const std::string & text ) const {
	CheckRequest req;
	req.path = path;
	req.text = text;
	req.backend = opts.backend;
	req.stopAfterResolve = opts.stopAfterResolve;
	req.skipSystemBodies = opts.skipSystemBodies;
	req.timeout = std::chrono::milliseconds( opts.timeoutMs );
	std::string dir = fs::path( path ).parent_path().string();
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
			// Idle: work on the index.
			if ( indexRescan ) {
				indexRescan = false;
				scanWorkspace( lk );
				continue;
			}
			if ( runIndexJob( lk ) ) continue;
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
		CheckRequest req = makeRequest( path, dit->second.text.str() );
		uint64_t seq = dit->second.seq;
		// Open headers with unsaved edits are read from their buffers.
		SeqMap reads;
		for ( const auto & [p, other] : docs ) {
			if ( p == path || ! isHeader( p ) || ( other.diskSeq && *other.diskSeq == other.seq ) ) continue;
			req.overlays[p] = other.text.str();
			reads[p] = other.seq;
		}
		std::shared_ptr<const Checker> chk = checker;
		auto token = std::make_shared<CancelToken>();
		inflight = InFlight{ path, seq, token, Clock::now(), reads, false };
		lk.unlock();

		log::info( "checking ", path );
		auto t0 = Clock::now();
		FrontResult fr = chk->front( req, *token );
		auto ms = []( auto d ) { return std::chrono::duration_cast<std::chrono::milliseconds>( d ).count(); };
		log::info( "front end for ", path, ": status ", (int)fr.status, ", ", fr.diags.size(), " diagnostics, ", ms( Clock::now() - t0 ), " ms" );
		std::shared_ptr<const UnitIndex> table;
		if ( fr.analysis && ! token->cancelled() ) table = std::make_shared<const UnitIndex>( fr.analysis->unitIndex() );

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
			doc.analysisReads = reads;
			doc.table = table;
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
		if ( ! fr.files.empty() ) doc.includes = fr.files;
		diskCache.clear();
		storeDiags( path, seq, reads, diags, out );
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
					storeDiags( path, seq, reads, merged, out );
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

// ---------------------------------------------------------------- index

bool Server::indexing() const {
	return opts.index && ! rootPath.empty() && checker && ! checker->toolchain().cfa.empty() &&
		   ! checker->toolchain().translator.empty();
}

// Finds the .cfa and .hfa files under the root (skipping hidden directories)
// and marks index entries stale when a file they read has changed.
void Server::scanWorkspace( std::unique_lock<std::mutex> & lk ) {
	if ( ! indexing() ) return;
	std::string root = rootPath;
	indexScanning = true;
	lk.unlock();
	std::vector<std::string> units, headers;
	size_t visited = 0;
	std::error_code ec;
	for ( fs::recursive_directory_iterator it( root, fs::directory_options::skip_permission_denied, ec ), end;
		  ! ec && it != end; it.increment( ec ) ) {
		if ( ++visited > maxScanEntries ) {
			log::warn( "index: stopped after ", maxScanEntries, " entries under ", root );
			break;
		}
		std::error_code tec;
		std::string name = it->path().filename().string();
		if ( it->is_directory( tec ) ) {
			if ( name.starts_with( "." ) ) it.disable_recursion_pending();
			continue;
		}
		if ( ! it->is_regular_file( tec ) ) continue;
		std::string ext = it->path().extension().string();
		if ( ext == ".cfa" ) units.push_back( it->path().lexically_normal().string() );
		else if ( ext == ".hfa" ) headers.push_back( it->path().lexically_normal().string() );
	}
	std::sort( units.begin(), units.end() );
	std::sort( headers.begin(), headers.end() );
	bool truncated = visited > maxScanEntries || units.size() > maxIndexedFiles;
	if ( units.size() > maxIndexedFiles ) {
		log::warn( "index: ", units.size(), " .cfa files under ", root, "; indexing the first ", maxIndexedFiles );
		units.resize( maxIndexedFiles );
	}
	lk.lock();
	indexScanning = false;
	indexTruncated = truncated;
	if ( stopping ) return;
	bool headersChanged = headers != projectHeaders;
	projectHeaders = std::move( headers );
	std::set<std::string> found( units.begin(), units.end() );
	for ( auto it = index.begin(); it != index.end(); ) {
		if ( found.count( it->first ) ) ++it;
		else it = index.erase( it );
	}
	for ( const std::string & u : units ) {
		auto [it, fresh] = index.try_emplace( u );
		IndexEntry & e = it->second;
		if ( fresh || e.stale ) continue;
		// A check that failed may work with the new set of headers.
		bool changed = headersChanged && ! e.table;
		for ( const auto & [f, when] : e.stamps ) {
			std::error_code tec;
			auto now = fs::last_write_time( f, tec );
			if ( tec || now != when ) {
				changed = true;
				break;
			}
		}
		if ( changed ) {
			e.stale = true;
			e.gen += 1;
		}
	}
	log::info( "index: ", index.size(), " files, ", projectHeaders.size(), " headers under ", root );
}

// Checks one stale file of the index from disk, with every project header
// as a focus file so their uses are recorded too.
bool Server::runIndexJob( std::unique_lock<std::mutex> & lk ) {
	if ( ! indexing() ) return false;
	auto next = std::find_if( index.begin(), index.end(), []( const auto & e ) { return e.second.stale; } );
	if ( next == index.end() ) return false;
	std::string path = next->first;
	uint64_t gen = next->second.gen;
	std::vector<std::string> headers = projectHeaders;
	std::shared_ptr<const Checker> chk = checker;
	auto token = std::make_shared<CancelToken>();
	inflight = InFlight{ path, 0, token, Clock::now(), {}, true };
	lk.unlock();

	// Taken before reading, so a change made during the check shows later.
	std::map<std::string, fs::file_time_type> stamps;
	auto stamp = [&stamps]( const std::string & f ) {
		std::error_code ec;
		auto t = fs::last_write_time( f, ec );
		if ( ! ec ) stamps[f] = t;
	};
	stamp( path );
	for ( const std::string & h : headers ) stamp( h );
	std::shared_ptr<const UnitIndex> table;
	bool usable = false, cancelled = false;
	std::map<std::string, fs::file_time_type> kept;
	if ( auto text = readFile( path ) ) {
		CheckRequest req = makeRequest( path, *text );
		req.backend = false;
		req.stopAfterResolve = true;		// nothing after Resolve changes the dump
		req.extraFocus = headers;
		auto t0 = Clock::now();
		FrontResult fr = chk->front( req, *token );
		cancelled = token->cancelled() || fr.status == FrontResult::Cancelled;
		usable = fr.usable;
		if ( fr.analysis && ! cancelled ) table = std::make_shared<const UnitIndex>( fr.analysis->unitIndex() );
		log::info( "indexed ", path, ": status ", (int)fr.status, ", ",
				   std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - t0 ).count(), " ms" );
		for ( const std::string & f : fr.files ) {
			if ( ! stamps.count( f ) ) stamp( f );		// outside the root
			if ( stamps.count( f ) ) kept[f] = stamps[f];
		}
	}
	if ( stamps.count( path ) ) kept[path] = stamps[path];

	lk.lock();
	inflight.reset();
	auto it = index.find( path );
	if ( stopping || cancelled || it == index.end() ) return true;
	IndexEntry & e = it->second;
	// Even a poor table has positions in the text now on disk; an older one
	// doesn't.
	e.table = table;
	e.complete = table && usable;
	e.stamps = std::move( kept );
	if ( e.gen == gen ) e.stale = false;
	return true;
}

} // namespace cfalsp
