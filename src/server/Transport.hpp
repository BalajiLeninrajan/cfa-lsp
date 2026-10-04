#pragma once

#include <mutex>
#include <optional>
#include <string>

namespace cfalsp {

// Splits a byte stream into LSP messages framed with
// "Content-Length: N\r\n...\r\n\r\n<N bytes>". Bytes can arrive in any
// chunking. Headers are matched case-insensitively; others are ignored.
class FrameParser {
  public:
	void feed( const char * data, size_t n );
	void feed( const std::string & s ) { feed( s.data(), s.size() ); }

	// The next complete message body, or nullopt if more bytes are needed.
	// A header block without a usable Content-Length is skipped and
	// reported through `takeError()`.
	std::optional<std::string> next();

	// Set when a malformed header block was skipped; cleared by the call.
	bool takeError();

  private:
	std::string buf;
	size_t pos = 0;							// start of unconsumed bytes in buf
	long long bodyLen = -1;					// -1: still reading headers
	bool badHeader = false;
};

// Reads framed messages from a file descriptor.
class MessageReader {
  public:
	explicit MessageReader( int fd ) : fd( fd ) {}
	// Blocks until a message arrives. nullopt on EOF or a read error.
	std::optional<std::string> read();

  private:
	int fd;
	FrameParser parser;
};

// Writes framed messages; safe to call from several threads.
class MessageWriter {
  public:
	explicit MessageWriter( int fd ) : fd( fd ) {}
	bool write( const std::string & body );

  private:
	int fd;
	std::mutex mtx;
};

std::string frame( const std::string & body );

} // namespace cfalsp
