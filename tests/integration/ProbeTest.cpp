// A manual probe, not a regression test: with CFA_LSP_PROBE=<file.cfa> it
// opens the file in the real server, then asks for hover and definition on
// every identifier and prints what comes back. Use it to look at the
// server's answers on programs that can't live in this repo:
//
//   make test ARGS='-tc=probe -s' CFA_LSP_PROBE=/path/to/prog.cfa
//
// (CFA_LSP_PROBE goes in the environment.) CFA_LSP_PROBE_FULL=1 prints whole
// hovers, CFA_LSP_PROBE_LOG=<file> keeps the server's stderr and
// CFA_LSP_PROBE_BACKEND=1 runs gcc on the generated C. Skipped when
// CFA_LSP_PROBE is unset.
#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>

#include "LspClient.hpp"

using namespace cfalsp::test;
namespace fs = std::filesystem;

namespace {

std::string firstLine( const std::string & s ) {
	size_t a = s.find_first_not_of( "`\n" );
	if ( a == std::string::npos ) return "";
	if ( s.compare( a, 3, "cfa" ) == 0 ) a = s.find( '\n', a ) + 1;
	size_t b = s.find( '\n', a );
	std::string l = s.substr( a, b == std::string::npos ? std::string::npos : b - a );
	return l.size() > 90 ? l.substr( 0, 90 ) + "..." : l;
}

} // namespace

TEST_CASE( "probe" ) {
	std::string file = envOr( "CFA_LSP_PROBE" );
	if ( file.empty() ) return;
	if ( serverBinary().empty() || ! haveCfa() ) {
		MESSAGE( "probe needs the server and cfa" );
		return;
	}
	file = fs::canonical( file ).string();
	std::string uri = "file://" + file;
	std::string text = readAll( file );

	LspClient c( envOr( "CFA_LSP_PROBE_LOG" ) );
	c.initialize( { { "debounceMs", 0 }, { "backend", envOr( "CFA_LSP_PROBE_BACKEND" ) == "1" } } );
	auto t0 = std::chrono::steady_clock::now();
	c.open( uri, text );
	auto d = c.diagnostics( uri, 1, std::chrono::seconds( 300 ) );
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
	REQUIRE( d );
	std::ostringstream out;
	out << "check took " << ms << " ms\n";
	for ( const auto & x : ( *d )["diagnostics"] ) {
		out << "diag " << x["range"]["start"]["line"].get<int>() + 1 << ":" << x["range"]["start"]["character"] << "-"
			<< x["range"]["end"]["line"].get<int>() + 1 << ":" << x["range"]["end"]["character"] << " "
			<< firstLine( x["message"] ) << "\n";
	}

	// Every identifier outside comments, strings and directives.
	int line = 0, col = 0;
	size_t i = 0;
	int hovers = 0, defs = 0, idents = 0;
	bool full = envOr( "CFA_LSP_PROBE_FULL" ) == "1";
	while ( i < text.size() ) {
		char ch = text[i];
		if ( ch == '\n' ) { line += 1; col = 0; i += 1; continue; }
		if ( col == 0 && ( ch == '#' ) ) {
			while ( i < text.size() && text[i] != '\n' ) i += 1;
			continue;
		}
		if ( text.compare( i, 2, "//" ) == 0 ) {
			while ( i < text.size() && text[i] != '\n' ) i += 1;
			continue;
		}
		if ( text.compare( i, 2, "/*" ) == 0 ) {
			size_t e = text.find( "*/", i + 2 );
			e = e == std::string::npos ? text.size() : e + 2;
			for ( ; i < e; i += 1 ) {
				if ( text[i] == '\n' ) { line += 1; col = 0; } else col += 1;
			}
			continue;
		}
		if ( ch == '"' || ch == '\'' ) {
			size_t j = i + 1;
			while ( j < text.size() && text[j] != ch && text[j] != '\n' ) j += text[j] == '\\' ? 2 : 1;
			col += int( j + 1 - i );
			i = j + 1;
			continue;
		}
		if ( std::isalpha( (unsigned char)ch ) || ch == '_' ) {
			size_t j = i;
			while ( j < text.size() && ( std::isalnum( (unsigned char)text[j] ) || text[j] == '_' ) ) j += 1;
			std::string name = text.substr( i, j - i );
			idents += 1;
			json td = { { "uri", uri } };
			json p = { { "textDocument", td }, { "position", lspPos( line, col ) } };
			json h = c.result( "textDocument/hover", p );
			json def = c.result( "textDocument/definition", p );
			out << line + 1 << ":" << col << " " << name << "\n";
			if ( ! h.is_null() ) {
				hovers += 1;
				if ( full ) out << "    hover:\n" << h["contents"]["value"].get<std::string>() << "\n";
				else out << "    hover: " << firstLine( h["contents"]["value"] ) << "\n";
			}
			for ( const auto & l : def ) {
				defs += 1;
				std::string f = l["uri"];
				out << "    def: " << ( f == uri ? "" : fs::path( f ).filename().string() ) << ":"
					<< l["range"]["start"]["line"].get<int>() + 1 << ":" << l["range"]["start"]["character"] << "\n";
			}
			col += int( j - i );
			i = j;
			continue;
		}
		col += 1;
		i += 1;
	}
	json syms = c.result( "textDocument/documentSymbol", { { "textDocument", { { "uri", uri } } } } );
	json toks = c.result( "textDocument/semanticTokens/full", { { "textDocument", { { "uri", uri } } } } )["data"];
	out << idents << " identifiers, " << hovers << " hovers, " << defs << " definitions, " << syms.size()
		<< " top-level symbols, " << toks.size() / 5 << " semantic tokens\n";
	MESSAGE( out.str() );
	CHECK( c.shutdown() == 0 );
}
