#pragma once

#include <compare>
#include <string>
#include <utility>
#include <vector>

namespace cfalsp {

// A position in an original source file: 0-based line, 0-based byte column.
// The server converts to and from LSP's UTF-16 columns at the protocol edge.
struct Loc {
	int line = 0;
	int col = 0;
	friend bool operator==( const Loc &, const Loc & ) = default;
	friend auto operator<=>( const Loc &, const Loc & ) = default;
};

// Half-open: end is exclusive.
struct Range {
	Loc start, end;
	friend bool operator==( const Range &, const Range & ) = default;
};

// `file` is an absolute path (the real file, never the server's temp copy).
struct Location {
	std::string file;
	Range range;
	friend bool operator==( const Location &, const Location & ) = default;
};

// Severity and kind integers use the LSP enum values, so the server can pass
// them through unchanged.
struct Diagnostic {
	Location loc;
	int severity = 1;						// 1 error, 2 warning, 3 information, 4 hint
	std::string message;
	std::string source = "cfa";
};

struct HoverResult {
	std::string markdown;
	Range range;							// in the hovered file
};

struct CompletionItem {
	std::string label;
	int kind = 1;							// LSP CompletionItemKind
	std::string detail;						// one-line signature or type
	std::string documentation;				// markdown, may be empty
	std::string insertText;					// empty means use label
	std::string sortText;					// empty means use label
};

struct SignatureInfo {
	std::string label;
	std::vector<std::pair<int, int>> params;	// [start, end) byte offsets into label
	std::string documentation;
};

struct SignatureHelp {
	std::vector<SignatureInfo> signatures;
	int activeSignature = 0;
	int activeParameter = 0;
};

struct Symbol {
	std::string name;
	int kind = 13;							// LSP SymbolKind (13 = Variable)
	std::string detail;
	Range range, selectionRange;
	std::vector<Symbol> children;
};

struct DocumentHighlight {
	Range range;
	int kind = 1;							// LSP DocumentHighlightKind: 1 text (a declaration), 2 read (a use)
};

// A hint shown inline at `pos`: a parameter name before an argument, or the
// type of a call after it.
struct InlayHint {
	Loc pos;
	std::string label;
	int kind = 2;							// LSP InlayHintKind: 1 type, 2 parameter
	Range span;								// the call the hint belongs to; edited since the snapshot means stale
};

// What renaming the entity under the cursor touches.
struct RenamePlan {
	std::string name;						// the current name
	Range range;							// the occurrence under the cursor
	std::vector<Range> sites;				// every occurrence, declarations included, all in the queried file
	std::string error;						// non-empty: the rename is refused, and why
};

// One token for textDocument/semanticTokens, before delta encoding.
struct SemanticToken {
	Loc start;
	int length = 0;							// bytes; tokens never span lines
	int type = 0;							// index into Analysis::tokenTypes()
	int modifiers = 0;						// bit set over Analysis::tokenModifiers()
};

} // namespace cfalsp
