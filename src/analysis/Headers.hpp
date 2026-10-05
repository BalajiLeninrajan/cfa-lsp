#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cfalsp {

// A name a header declares at file scope.
struct HeaderName {
	std::string name;
	bool type = false;						// an aggregate, enum, trait or typedef
};

// The names `text` (a header) declares at file scope: functions, variables,
// aggregates, enums and their enumerators, traits, typedefs and macros,
// including declarations inside `forall( ... ) { }` and `extern "C" { }`
// blocks. It is a token scan, not a parse, so it misses declarations made
// by macros. Reserved names (leading `_`) and libcfa's internal names (with
// a `$`) are left out.
std::vector<HeaderName> fileScopeNames( std::string_view text );

// Which libcfa header declares a name, for the fix that adds a missing
// #include.
class HeaderIndex {
  public:
	// Indexes the .hfa files directly in `includeDir` (<prefix>/include/cfa)
	// and in its concurrency/ and collections/ subdirectories: the
	// directories the cfa driver passes with -I, so each header is named the
	// way users include it (fstream.hfa, thread.hfa, string.hfa). When two
	// directories have a header with the same name, the first one wins, as
	// it does for the preprocessor. A missing directory gives an empty index.
	static HeaderIndex scan( const std::string & includeDir );

	void add( const std::string & header, std::string_view text );

	// The headers that declare `name`, sorted; with `typesOnly`, only those
	// where it is a type.
	std::vector<std::string> headersFor( const std::string & name, bool typesOnly = false ) const;
	bool empty() const { return names.empty(); }

  private:
	struct Entry {
		std::string header;
		bool type = false;
	};
	std::unordered_map<std::string, std::vector<Entry>> names;
};

} // namespace cfalsp
