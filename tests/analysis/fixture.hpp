#pragma once

// Loads the hand-written dumps in tests/fixtures/dumps. Dumps name files
// "/fixture/<dir>/<file>", a fake libcfa header and a fake prelude; reader()
// maps those to the fixture tree.

#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Analysis.hpp"

namespace fixture {

inline std::string root() {
	const char * env = std::getenv( "CFA_LSP_FIXTURES" );
	return std::string( env && *env ? env : "tests/fixtures" ) + "/dumps";
}

inline std::optional<std::string> slurp( const std::string & path ) {
	std::ifstream in( path, std::ios::binary );
	if ( !in ) return std::nullopt;
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

inline constexpr const char * libcfa = "/opt/cfa/include/cfa/fstream.hfa";
inline constexpr const char * cfa = "/fixture/shapes/shapes.cfa";
inline constexpr const char * hfa = "/fixture/shapes/shapes.hfa";
inline constexpr const char * broken = "/fixture/broken/broken.cfa";

inline std::optional<std::string> read( const std::string & path ) {
	if ( path.starts_with( "/fixture/" ) ) return slurp( root() + "/" + path.substr( 9 ) );
	if ( path == libcfa ) return slurp( root() + "/shapes/lib/fstream.hfa" );
	if ( path == "prelude.cfa" ) return slurp( root() + "/shapes/lib/prelude.cfa" );
	return std::nullopt;
}

inline nlohmann::json dump( const std::string & name ) {
	auto text = slurp( root() + "/" + name + "/dump.json" );
	if ( !text ) throw std::runtime_error( "missing fixture " + name );
	return nlohmann::json::parse( *text );
}

inline std::shared_ptr<const cfalsp::Analysis> load( const std::string & name ) {
	return cfalsp::Analysis::load( dump( name ), cfalsp::SourceMap::identity(), read );
}

inline std::shared_ptr<const cfalsp::Analysis> loadText( const std::string & json, cfalsp::SourceMap::Reader r = read ) {
	return cfalsp::Analysis::load( nlohmann::json::parse( json ), cfalsp::SourceMap::identity(), std::move( r ) );
}

// Text of the file up to (line, col), both 0-based.
inline std::string textBefore( const std::string & path, int line, int col ) {
	std::string t = read( path ).value_or( "" );
	size_t off = 0;
	for ( int l = 0; l < line; l += 1 ) off = t.find( '\n', off ) + 1;
	return t.substr( 0, off + col );
}

} // namespace fixture
