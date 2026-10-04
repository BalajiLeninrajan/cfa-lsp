#include "Uri.hpp"

#include <cctype>

namespace cfalsp {

static bool unreserved( unsigned char c ) {
	return std::isalnum( c ) || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string pathToUri( const std::string & path ) {
	static const char hex[] = "0123456789ABCDEF";
	std::string out = "file://";
	for ( unsigned char c : path ) {
		if ( unreserved( c ) || c == '/' ) {
			out += (char)c;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

static int hexval( char c ) {
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

std::optional<std::string> uriToPath( const std::string & uri ) {
	const std::string scheme = "file://";
	if ( uri.size() < scheme.size() ) return std::nullopt;
	for ( size_t i = 0; i < scheme.size(); i += 1 ) {
		if ( std::tolower( (unsigned char)uri[i] ) != scheme[i] ) return std::nullopt;
	}
	std::string rest = uri.substr( scheme.size() );
	size_t slash = rest.find( '/' );
	if ( slash == std::string::npos ) return std::nullopt;
	std::string authority = rest.substr( 0, slash );
	if ( ! authority.empty() && authority != "localhost" ) return std::nullopt;
	rest = rest.substr( slash );
	// Drop any query or fragment.
	size_t q = rest.find_first_of( "?#" );
	if ( q != std::string::npos ) rest.resize( q );
	std::string out;
	for ( size_t i = 0; i < rest.size(); i += 1 ) {
		if ( rest[i] == '%' && i + 2 < rest.size() && hexval( rest[i + 1] ) >= 0 && hexval( rest[i + 2] ) >= 0 ) {
			out += (char)( hexval( rest[i + 1] ) * 16 + hexval( rest[i + 2] ) );
			i += 2;
		} else {
			out += rest[i];
		}
	}
	return out;
}

} // namespace cfalsp
