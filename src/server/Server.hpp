#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"
#include "Checker.hpp"
#include "Headers.hpp"
#include "PositionMap.hpp"
#include "Text.hpp"
#include "Transport.hpp"

namespace cfalsp {

// The language server: reads JSON-RPC from one fd, writes to another.
// Requests are answered on the calling thread from the last good analysis;
// one worker thread runs checks and publishes diagnostics. An edit cancels
// the check in flight if it has run less than half as long as the document's
// last check (but not two in a row) or more than twice as long. Otherwise it
// finishes and publishes, mapped through the edits made since, without the
// diagnostics on text those edits changed, and the next check starts after it. When it has nothing else
// to do, the worker checks every .cfa file under the workspace root from disk
// (the background index) for cross-file queries.
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

	// For tests: called at points where a race could happen, so a test can
	// hold one thread there. "publish": the worker has built a
	// publishDiagnostics batch and released the document lock, but not sent
	// it. "closed": didClose has sent its empty publish. Set before run().
	void setTestHook( std::function<void( const char * point )> hook ) { testHook = std::move( hook ); }

  private:
	using json = nlohmann::json;
	using Clock = std::chrono::steady_clock;

	using SeqMap = std::map<std::string, uint64_t>;	// path -> document seq

	struct DiagSet {
		uint64_t seq = 0;					// document seq the positions refer to (main file only)
		SeqMap reads;						// other documents the check read from their buffers
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
		SeqMap analysisReads;				// other documents its check read from their buffers
		std::shared_ptr<const UnitIndex> table;	// from `analysis`, for cross-file queries
		std::vector<std::string> includes;	// project files the last check read
		// The seq at which the buffer had the contents on disk (when opened or
		// saved), which is what other documents' checks read; unset if unknown.
		std::optional<uint64_t> diskSeq;
		// The last backend run's diagnostics, at backSeq for the main file.
		// Shown again while later checks can't run the backend, minus those
		// on lines edited since.
		std::vector<Diag> backDiags;
		uint64_t backSeq = 0;
		// How long the last check that finished took, or how long one that an
		// edit cancelled had run. Zero until a check finishes.
		Clock::duration lastCheck{};
		// An edit cancelled the last check early; the next one finishes.
		bool cancelledEarly = false;

		EditList editsSince( uint64_t s ) const;
		EditList editsBetween( uint64_t from, uint64_t to ) const;	// took seq `from` to seq `to`
	};

	struct InFlight {
		std::string path;
		uint64_t seq = 0;
		std::shared_ptr<CancelToken> token;
		Clock::time_point started;
		SeqMap reads;
		bool index = false;					// a background index check, from disk
	};

	// How the positions in one check's results relate to the texts the client
	// has now: `main` was read from its buffer at `mainSeq`, the files in
	// `reads` from their buffers at those seqs, every other file from disk.
	struct Snapshot {
		std::string main;
		uint64_t mainSeq = 0;
		SeqMap reads;
	};

	// One check's tables for cross-file queries.
	struct Source {
		std::shared_ptr<const UnitIndex> table;
		Snapshot snap;
		bool index = false;					// from the background index
	};

	// A declaration as cross-file queries match it: file and name start, in
	// the text the client has now.
	using Key = std::pair<std::string, Loc>;

	struct Match {
		size_t source;
		int entity;
	};

	// The entity under the cursor, if the document's table knows it (it
	// leaves out locals).
	struct Target {
		std::string name;
		std::set<Key> keys;
		int kind = 0;						// LSP SymbolKind
		bool library = false;
		bool function = false;
	};

	struct Mapped {
		const Text * text;
		Location loc;
	};

	struct IndexEntry {
		std::shared_ptr<const UnitIndex> table;	// null until checked, or if the check failed
		// Modification times of the files the check read, taken before it ran.
		std::map<std::string, std::filesystem::file_time_type> stamps;
		bool stale = true;
		bool complete = false;				// the check gave a usable analysis, so the table has every use
		uint64_t gen = 0;					// bumped whenever it goes stale
	};

	// Where a cross-file rename edits: file -> (text the ranges are in, ranges).
	using RenameSites = std::map<std::string, std::pair<const Text *, std::vector<Range>>>;

	struct Options {
		int debounceMs = 500;
		bool backend = true;
		int timeoutMs = 120000;
		bool stopAfterResolve = false;
		bool skipSystemBodies = true;
		bool index = true;
		std::optional<std::vector<std::string>> flags;
	};

	struct Incoming {
		json msg;
		std::string error;					// set if the body isn't JSON
	};

	// protocol
	static Incoming parse( const std::string & body );
	// Reads the messages that have already arrived, so a $/cancelRequest
	// for `current` or for a queued request is seen before it is answered.
	void readAhead( const json & current );
	void handle( const json & msg );
	void response( const json & msg );
	int sendRequest( const std::string & method, json params );
	json request( const std::string & method, const json & params );
	void notification( const std::string & method, const json & params );
	void send( const json & msg );
	void notify( const std::string & method, json params );

	// lifecycle
	json initialize( const json & params );
	void startWorker();
	void stopWorker();

	// configuration
	void applyOptions( const json & io );	// initializationOptions merged with settings
	void didChangeConfiguration( const json & params );
	void applySettings( const json & settings );
	void requestConfiguration();
	void warnMissing();

	// documents
	void didOpen( const json & params );
	void didChange( const json & params );
	void didClose( const json & params );
	void didSave( const json & params );
	void schedule( const std::string & path, int delayMs );	// caller holds mtx
	void scheduleDependents( const std::string & header );	// caller holds mtx
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
	json prepareCallHierarchy( const json & params );
	// The cursor in `params` in d's snapshot; nullopt if it is in text typed since.
	std::optional<Loc> cursorInSnapshot( const Document & d, const json & params ) const;
	// The rename at the cursor with its ranges in the current buffer; nullopt
	// if there is nothing to rename. Throws if the rename is refused.
	std::optional<RenamePlan> renameAt( const Document & d, const json & params );
	json codeAction( const json & params );
	json formatting( const json & params, bool range );
	// The same across files, through the index: plan.sites is empty and the
	// edits are in `sites`. nullopt when the index can't do it (no index, or
	// the name is local), and renameAt applies.
	std::optional<RenamePlan> renameAcross( const Document & d, const json & params, RenameSites & sites );

	// cross-file queries (caller holds mtx)
	std::vector<Source> sources();
	bool hidden( const Source & s, const std::string & file ) const;
	std::optional<Target> targetAt( const Document & d, Loc pos );
	std::set<Key> keysOf( const Source & s, int entity );
	std::vector<Match> matchEntities( const std::vector<Source> & srcs, const std::string & name, std::set<Key> & keys );
	std::optional<json> callItem( const std::vector<Source> & srcs, const std::vector<Match> & matches,
								  const std::set<Key> & keys );
	// For grouping calls: the same id for an entity in every source, and its item.
	std::pair<std::string, std::optional<json>> resolveEntity( const std::vector<Source> & srcs, size_t source, int entity );
	std::pair<std::string, std::set<Key>> itemKeys( const json & item );
	json calls( const json & params, bool incoming );

	// conversions (caller holds mtx)
	json lspPos( const Text & text, Loc l ) const;
	json lspRange( const Text & text, Range r ) const;
	Snapshot snapOf( const Document & d ) const;
	// The edits that take positions `snap` has in `file` to the text the
	// client sees, which is stored in `text`.
	EditList editsFor( const Snapshot & snap, const std::string & file, const Text *& text );
	// A location from a check, in the client's text; nullopt if `exact` and
	// an edit touched it.
	std::optional<Mapped> toClient( const Snapshot & snap, const Location & loc, bool exact );
	std::optional<json> lspLocation( const Snapshot & snap, const Location & loc, bool exact );
	std::optional<json> lspLocation( const Document & from, const Location & loc, bool exact ) {
		return lspLocation( snapOf( from ), loc, exact );
	}
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
	// libcfa's headers, indexed on first use (caller holds mtx).
	const HeaderIndex & headerIndex();

	// diagnostics
	void storeDiags( const std::string & source, uint64_t seq, const SeqMap & reads, const std::vector<Diag> & diags,
					 std::vector<json> & out );	// caller holds mtx
	json publishFor( const std::string & target );	// caller holds mtx
	// nullopt if an edit made since the check changed the text the diagnostic is about.
	std::optional<json> diagJson( const Diag & d, const std::string & target, const std::string & source,
								  const DiagSet & set );

	// checking
	void workerLoop();
	CheckRequest makeRequest( const std::string & path, const std::string & text ) const;

	// background index (worker thread, holding `lk` except while it works)
	bool indexing() const;
	void scanWorkspace( std::unique_lock<std::mutex> & lk );
	bool runIndexJob( std::unique_lock<std::mutex> & lk );	// false if nothing is stale
	void rebaseBackend( Document & doc, uint64_t seq );	// caller holds mtx

	MessageReader reader;
	MessageWriter writer;
	std::string exeDir;

	enum class Phase { Uninitialized, Running, ShuttingDown } phase = Phase::Uninitialized;
	Encoding enc = Encoding::Utf16;
	bool hierarchicalSymbols = false;
	bool prepareRenameSupport = false;
	bool watchFiles = false;				// the client takes dynamic registration of file watchers
	bool inlayHintRefresh = false;			// the client takes workspace/inlayHint/refresh
	std::string rootPath;
	bool configurationPull = false;			// client answers workspace/configuration
	bool configurationRegistration = false;	// client takes dynamic registration of didChangeConfiguration
	json initOptions = json::object();
	json settings = json::object();			// from the client's configuration, our section only
	json effective;							// what opts and checker were made from
	std::string warnedMissing;				// the last missing-tool warning shown
	std::deque<Incoming> inbox;				// read ahead, not handled yet
	std::set<std::string> cancelled;		// ids (dumped) of requests to answer with RequestCancelled
	int nextRequestId = 1;
	std::set<int> configRequests;			// our workspace/configuration requests awaiting a response
	std::function<void( const char * )> testHook;

	// Held from building a publishDiagnostics notification until it is sent,
	// so publishes for one file go out in the order they were built. Taken
	// while holding mtx, never the other way round.
	std::mutex publishMtx;
	std::mutex mtx;							// guards everything below
	// opts and checker change only on the reader thread, which can read
	// them without the lock.
	Options opts;
	std::shared_ptr<const Checker> checker;
	std::map<std::string, Document> docs;	// by path
	// target file -> source document (the check that produced them) -> diagnostics
	std::map<std::string, std::map<std::string, DiagSet>> diagStore;
	std::map<std::string, Clock::time_point> pending;	// path -> when to check
	std::optional<InFlight> inflight;
	std::condition_variable cv;
	bool stopping = false;
	std::thread worker;
	std::map<std::string, Text> diskCache;	// cleared per request
	int refreshRequests = 0;				// ids of the requests we send
	std::optional<HeaderIndex> headers;		// see headerIndex()
	std::map<std::string, IndexEntry> index;	// every .cfa under the root, by path
	std::vector<std::string> projectHeaders;	// .hfa files under the root, focus files for index checks
	bool indexRescan = false;				// walk the root again and look for changed files
	bool indexScanning = false;				// a walk is under way
	bool indexTruncated = false;			// the last walk hit maxScanEntries or maxIndexedFiles
};

} // namespace cfalsp
