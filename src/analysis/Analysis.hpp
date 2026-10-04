#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "SourceMap.hpp"
#include "Types.hpp"

namespace cfalsp {

// Read-only queries over one translator dump (docs/dump-format.md).
//
// All positions are in the coordinates of the text the dump was made from
// (the "snapshot"). The server maps between the snapshot and the current
// buffer when the user has edited since; Analysis never sees edits.
//
// Thread safety: a loaded Analysis is immutable and safe to query from any
// thread.
class Analysis {
  public:
	// Throws std::runtime_error on a malformed dump. `read` returns current
	// file text (same contract as SourceMap::Reader), used for doc comments.
	static std::shared_ptr<const Analysis> load( const nlohmann::json & dump, const SourceMap & map,
												 SourceMap::Reader read );
	~Analysis();

	bool complete() const;
	std::vector<Diagnostic> diagnostics() const;

	std::optional<HoverResult> hover( const std::string & file, Loc pos ) const;
	std::vector<Location> definition( const std::string & file, Loc pos ) const;
	std::vector<Location> references( const std::string & file, Loc pos, bool includeDeclaration ) const;
	std::vector<Symbol> documentSymbols( const std::string & file ) const;

	// For combining the analyses of several open documents: the declarations
	// of the entity at `pos` (a prototype and its definition, say), to look up
	// in another analysis with referencesTo() and definitionOf(). A
	// declaration is matched by its file and the start of its name.
	std::vector<Location> declarationsAt( const std::string & file, Loc pos ) const;
	// Uses of the entity declared at any of `declarations` (and with
	// includeDeclaration, its declarations) in this analysis.
	std::vector<Location> referencesTo( const std::vector<Location> & declarations, bool includeDeclaration ) const;
	// The declaration with a body of that entity, if this analysis has one.
	std::optional<Location> definitionOf( const std::vector<Location> & declarations ) const;

	// `lineBefore` is the CURRENT buffer text of the cursor's line up to the
	// cursor; it may contain code the snapshot has never seen (e.g. "x." or
	// "pu"). `pos` is the cursor mapped into snapshot coordinates (clamped to
	// the nearest surviving point if it is inside newly typed text).
	// `textBefore`, the current buffer up to the cursor, lets member completion
	// find declarations typed since the snapshot; it may be empty.
	std::vector<CompletionItem> completion( const std::string & file, Loc pos,
											const std::string & lineBefore,
											const std::string & textBefore = std::string() ) const;

	// `textBefore` is the current buffer text from the start of the file up
	// to the cursor, so the call's name and argument index can be found even
	// in unparsed code. `pos` as for completion().
	std::optional<SignatureHelp> signatureHelp( const std::string & file, Loc pos,
												const std::string & textBefore ) const;

	std::vector<SemanticToken> semanticTokens( const std::string & file ) const;
	static const std::vector<std::string> & tokenTypes();		// LSP legend
	static const std::vector<std::string> & tokenModifiers();	// LSP legend

  private:
	Analysis();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace cfalsp
