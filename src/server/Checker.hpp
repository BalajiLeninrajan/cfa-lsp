#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "Analysis.hpp"
#include "Flags.hpp"
#include "Process.hpp"
#include "Toolchain.hpp"
#include "Types.hpp"

namespace cfalsp {

// A diagnostic on its way to the client. Positions are bytes in the text
// the check read: the request buffer for the main file, disk contents for
// other files.
struct Diag {
	struct Related {
		std::string file;
		Range range;
		std::string message;
	};
	std::string file;						// absolute path
	Range range;
	bool wholeLine = false;					// cover the line's text, ignore range columns
	int severity = 1;						// LSP DiagnosticSeverity
	std::string message;
	std::string source = "cfa";
	std::string code;						// e.g. -Wunused-variable
	std::vector<Related> related;
};

// A private directory removed (recursively) on destruction.
// Removes the temp directories (<TMPDIR>/cfa-lsp-XXXXXX) of this user that
// have not changed for an hour: left behind by a server that was killed
// with SIGKILL. A check never takes that long.
void removeStaleTempDirs();

class TempDir {
  public:
	TempDir();								// throws std::runtime_error
	~TempDir();
	TempDir( const TempDir & ) = delete;
	TempDir & operator=( const TempDir & ) = delete;
	const std::string & path() const { return dir; }
  private:
	std::string dir;
};

struct CheckRequest {
	std::string path;						// real absolute path of the buffer
	std::string text;						// buffer contents
	std::vector<std::string> flags;			// the user's flags
	std::string flagsBase;					// directory relative flag paths resolve against
	bool backend = true;
	std::chrono::milliseconds timeout{ 120000 };	// per child process
};

struct FrontResult {
	enum Status { Ok, Cancelled, NoCfa, PreprocessFailed, TranslatorFailed, LoadFailed, Fallback } status = Ok;
	std::vector<Diag> diags;				// main file and project headers only
	std::shared_ptr<const Analysis> analysis;
	bool usable = false;					// analysis has declarations for the main file
	bool backendReady = false;				// translator wrote C and reported no errors

	// Kept for the backend stage.
	std::shared_ptr<TempDir> tmp;
	std::string cOut;
	FlagSet flags;
};

class Checker {
  public:
	explicit Checker( Toolchain tc ) : tc( std::move( tc ) ) {}
	const Toolchain & toolchain() const { return tc; }

	// Preprocess, translate and load. Without a translator, falls back to
	// running the whole cfa driver and parsing its output (status Fallback).
	FrontResult front( const CheckRequest & req, const CancelToken & cancel ) const;

	// gcc -fsyntax-only on the generated C. Only diagnostics in project
	// files are returned, deduplicated, one whole line each.
	std::vector<Diag> back( const CheckRequest & req, const FrontResult & fr, const CancelToken & cancel ) const;

	// The commands, exposed for tests.
	std::vector<std::string> cppCommand( const CheckRequest & req, const FlagSet & fs, const std::string & in ) const;
	std::vector<std::string> translatorCommand( const CheckRequest & req, const FlagSet & fs, const std::string & in,
												const std::string & json, const std::string & cOut ) const;
	std::vector<std::string> backendCommand( const FlagSet & fs, const std::string & cFile ) const;

  private:
	FrontResult fallback( const CheckRequest & req, const FlagSet & fs, TempDir & tmp, const std::string & in,
						  const CancelToken & cancel ) const;
	Toolchain tc;
};

// Turns compiler output into Diags: maps `tmpInput` to `realPath`, resolves
// relative names against `cwd`, drops system files and noise.
std::vector<Diag> diagsFromOutput( const std::string & output, const std::string & realPath,
								   const std::string & tmpInput, const std::string & cwd,
								   const Toolchain & tc, const std::string & source );

} // namespace cfalsp
