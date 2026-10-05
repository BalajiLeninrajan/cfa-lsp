#include "Transport.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

#include "Log.hpp"

namespace cfalsp {

void FrameParser::feed( const char * data, size_t n ) {
	// Drop consumed bytes now and then so the buffer doesn't grow forever.
	if ( pos > 0 && pos >= buf.size() / 2 ) {
		buf.erase( 0, pos );
		pos = 0;
	}
	buf.append( data, n );
}

static std::string lower( std::string s ) {
	for ( auto & c : s ) c = (char)std::tolower( (unsigned char)c );
	return s;
}

static std::string trim( const std::string & s ) {
	size_t b = 0, e = s.size();
	while ( b < e && std::isspace( (unsigned char)s[b] ) ) b += 1;
	while ( e > b && std::isspace( (unsigned char)s[e - 1] ) ) e -= 1;
	return s.substr( b, e - b );
}

std::optional<std::string> FrameParser::next() {
	for ( ;; ) {
		if ( bodyLen < 0 ) {
			size_t end = buf.find( "\r\n\r\n", pos );
			if ( end == std::string::npos ) return std::nullopt;
			std::string headers = buf.substr( pos, end - pos );
			pos = end + 4;
			long long len = -1;
			size_t i = 0;
			while ( i <= headers.size() ) {
				size_t nl = headers.find( "\r\n", i );
				if ( nl == std::string::npos ) nl = headers.size();
				std::string line = headers.substr( i, nl - i );
				i = nl + 2;
				size_t colon = line.find( ':' );
				if ( colon == std::string::npos ) continue;
				if ( lower( trim( line.substr( 0, colon ) ) ) == "content-length" ) {
					std::string v = trim( line.substr( colon + 1 ) );
					char * endp = nullptr;
					long long n = std::strtoll( v.c_str(), &endp, 10 );
					if ( ! v.empty() && endp && *endp == '\0' && n >= 0 ) len = n;
				}
			}
			if ( len < 0 ) {
				badHeader = true;
				continue;
			}
			bodyLen = len;
		}
		if ( buf.size() - pos < (size_t)bodyLen ) return std::nullopt;
		std::string body = buf.substr( pos, (size_t)bodyLen );
		pos += (size_t)bodyLen;
		bodyLen = -1;
		return body;
	}
}

bool FrameParser::takeError() {
	bool e = badHeader;
	badHeader = false;
	return e;
}

std::optional<std::string> MessageReader::read() {
	for ( ;; ) {
		if ( auto m = parser.next() ) return m;
		if ( parser.takeError() ) log::warn( "skipped a message header without Content-Length" );
		if ( eof ) return std::nullopt;
		char chunk[65536];
		ssize_t n = ::read( fd, chunk, sizeof( chunk ) );
		if ( n < 0 && errno == EINTR ) continue;
		if ( n <= 0 ) {
			eof = true;
			return std::nullopt;
		}
		parser.feed( chunk, (size_t)n );
	}
}

std::optional<std::string> MessageReader::tryRead() {
	for ( ;; ) {
		if ( auto m = parser.next() ) return m;
		if ( parser.takeError() ) log::warn( "skipped a message header without Content-Length" );
		if ( eof ) return std::nullopt;
		pollfd p{ fd, POLLIN, 0 };
		int r = ::poll( &p, 1, 0 );
		if ( r < 0 && errno == EINTR ) continue;
		if ( r <= 0 ) return std::nullopt;
		char chunk[65536];
		ssize_t n = ::read( fd, chunk, sizeof( chunk ) );
		if ( n < 0 && errno == EINTR ) continue;
		if ( n <= 0 ) {
			eof = true;
			return std::nullopt;
		}
		parser.feed( chunk, (size_t)n );
	}
}

std::string frame( const std::string & body ) {
	return "Content-Length: " + std::to_string( body.size() ) + "\r\n\r\n" + body;
}

bool MessageWriter::write( const std::string & body ) {
	std::string out = frame( body );
	std::lock_guard<std::mutex> lock( mtx );
	size_t off = 0;
	while ( off < out.size() ) {
		ssize_t n = ::write( fd, out.data() + off, out.size() - off );
		if ( n < 0 && errno == EINTR ) continue;
		if ( n <= 0 ) return false;
		off += (size_t)n;
	}
	return true;
}

} // namespace cfalsp
