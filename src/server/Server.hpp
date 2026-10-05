#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"
#include "Checker.hpp"
#include "PositionMap.hpp"
#include "Text.hpp"
#include "Transport.hpp"

namespace cfalsp {

// The language server: reads JSON-RPC from one fd, writes to another.
// Requests are answered on the calling thread from the last good analysis;
// one worker thread runs checks and publishes diagnostics.
class Server {
  public:
	Server( int inFd, int outFd, std::string exeDir );
	~Server();

	// Serves until `exit` or end of input. Returns the process exit code:
	// 0 if `shutdown` came before `exit`, 1 otherwise.
	int run();

	// For a termination signal: cancels the check in flight (killing its
	// processes and removing its temp directory) and stops the worker.
	void terminate();

  private:
	using json = nlohmann::json;
	using Clock = std::chrono::steady_clock;

	struct DiagSet {
		uint64_t seq = 0;					// document seq the positions refer to (main file only)
		std::vector<Diag> diags;
	};

	struct Document {
		std::string uri;					// as the client sent it
		std::string path;
		int version = 0;
		Text text;
		uint64_t seq = 0;					// bumped by every change
		std::vector<std::pair<uint64_t, TextEdit>> log;	// edit that produced seq N

		std::shared_ptr<const Analysis> analysis;
		uint64_t analysisSeq = 0;
		// The seq at which the buffer had the contents on disk (when opened or
		// saved), which is what other documents' checks read; unset if unknown.
		std::optional<uint64_t> diskSeq;

		EditList editsSince( uint64_t s ) const;
	};

	struct InFlight {
		std::string path;
		uint64_t seq = 0;
		std::shared_ptr<CancelToken> token;
	};

	struct Options {
		int debounceMs = 500;
		bool backend = true;
		int timeoutMs = 120000;
		std::optional<std::vector<std::string>> flags;
	};

	// protocol
	void handle( const json & msg );
	json request( const std::string & method, const json & params );
	void notification( const std::string & method, const json & params );
	void send( const json & msg );
	void notify( const std::string & method, json params );

	// lifecycle
	json initialize( const json & params );
	void startWorker();
	void stopWorker();

	// documents
	void didOpen( const json & params );
	void didChange( const json & params );
	void didClose( const json & params );
	void didSave( const json & params );
	void schedule( const std::string & path, int delayMs );	// caller holds mtx
	void trimLog( Document & doc );		// caller holds mtx

	// queries (caller holds mtx)
	Document * docFor( const json & params );
	json hover( const json & params );
	json definition( const json & params );
	json references( const json & params );
	json documentSymbol( const json & params );
	json completion( const json & params );
	json signatureHelp( const json & params );
	json semanticTokens( const json & params );
	json documentHighlight( const json & params );
	json inlayHint( const json & params );
	json prepareRename( const json & params );
	json rename( const json & params );
	json switchSourceHeader( const json & params );
	json workspaceSymbol( const json & params );
	// The cursor in `params` in d's snapshot; nullopt if it is in text typed since.
	std::optional<Loc> cursorInSnapshot( const Document & d, const json & params ) const;
	// The rename at the cursor with its ranges in the current buffer; nullopt
	// if there is nothing to rename. Throws if the rename is refused.
	std::optional<RenamePlan> renameAt( const Document & d, const json & params );

	// conversions (caller holds mtx)
	json lspPos( const Text & text, Loc l ) const;
	json lspRange( const Text & text, Range r ) const;
	std::optional<json> lspLocation( const Document & from, const Location & loc, bool exact );
	const Text & textOf( const std::string & path );	// open document or disk (cached per request)
	// A range in `path` as read from disk, in the text the client sees: the
	// open buffer if the edits since its disk contents are known, else the
	// disk text. nullopt if `exact` and an edit touched the range.
	std::optional<std::pair<const Text *, Range>> fromDisk( const std::string & path, Range r, bool exact );
	const Text & diskText( const std::string & path );	// cached per request
	// Is the identifier at `cur` the one the snapshot had there? Typing onto
	// the end of a name makes a different name.
	bool sameIdentifier( const Document & d, Loc cur, const EditList & edits ) const;
	std::string uriOf( const std::string & path ) const;

	// diagnostics
	void storeDiags( const std::string & source, uint64_t seq, const std::vector<Diag> & diags,
					 std::vector<json> & out );	// caller holds mtx
	json publishFor( const std::string & target );	// caller holds mtx
	json diagJson( const Diag & d, const std::string & target, const std::string & source, uint64_t seq );

	// checking
	void workerLoop();
	CheckRequest makeRequest( const Document & doc ) const;

	MessageReader reader;
	MessageWriter writer;
	std::string exeDir;

	enum class Phase { Uninitialized, Running, ShuttingDown } phase = Phase::Uninitialized;
	Encoding enc = Encoding::Utf16;
	bool hierarchicalSymbols = false;
	bool prepareRenameSupport = false;
	std::string rootPath;
	Options opts;
	std::unique_ptr<Checker> checker;
	bool warnedMissing = false;

	// Held from building a publishDiagnostics notification until it is sent,
	// so publishes for one file go out in the order they were built. Taken
	// while holding mtx, never the other way round.
	std::mutex publishMtx;
	std::mutex mtx;							// guards everything below
	std::map<std::string, Document> docs;	// by path
	// target file -> source document (the check that produced them) -> diagnostics
	std::map<std::string, std::map<std::string, DiagSet>> diagStore;
	std::map<std::string, Clock::time_point> pending;	// path -> when to check
	std::optional<InFlight> inflight;
	std::condition_variable cv;
	bool stopping = false;
	std::thread worker;
	std::map<std::string, Text> diskCache;	// cleared per request
};

} // namespace cfalsp
