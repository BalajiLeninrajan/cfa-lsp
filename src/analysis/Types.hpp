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

// One token for textDocument/semanticTokens, before delta encoding.
struct SemanticToken {
	Loc start;
	int length = 0;							// bytes; tokens never span lines
	int type = 0;							// index into Analysis::tokenTypes()
	int modifiers = 0;						// bit set over Analysis::tokenModifiers()
};

} // namespace cfalsp
