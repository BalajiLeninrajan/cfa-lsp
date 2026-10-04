#include <doctest/doctest.h>

#include <fcntl.h>
#include <random>
#include <thread>
#include <unistd.h>

#include "server/Transport.hpp"

using namespace cfalsp;

TEST_CASE( "frame parser: whole messages" ) {
	FrameParser p;
	p.feed( frame( "{\"a\":1}" ) + frame( "{}" ) );
	CHECK( p.next() == std::optional<std::string>( "{\"a\":1}" ) );
	CHECK( p.next() == std::optional<std::string>( "{}" ) );
	CHECK( ! p.next() );
}

TEST_CASE( "frame parser: byte-at-a-time and random chunking" ) {
	std::string bodies[] = { "{\"x\":\"\xC3\xA9\"}", "", "[1,2,3]", std::string( 70000, 'a' ) };
	std::string stream;
	for ( const auto & b : bodies ) stream += frame( b );

	FrameParser one;
	std::vector<std::string> got;
	for ( char c : stream ) {
		one.feed( &c, 1 );
		while ( auto m = one.next() ) got.push_back( *m );
	}
	REQUIRE( got.size() == 4 );
	for ( int i = 0; i < 4; i += 1 ) CHECK( got[i] == bodies[i] );

	std::mt19937 rng( 7 );
	for ( int round = 0; round < 50; round += 1 ) {
		FrameParser p;
		got.clear();
		size_t i = 0;
		while ( i < stream.size() ) {
			size_t n = std::min( stream.size() - i, (size_t)( rng() % 300 + 1 ) );
			p.feed( stream.data() + i, n );
			i += n;
			while ( auto m = p.next() ) got.push_back( *m );
		}
		REQUIRE( got.size() == 4 );
		CHECK( got[3] == bodies[3] );
	}
}

TEST_CASE( "frame parser: header variants" ) {
	FrameParser p;
	p.feed( "content-length:  5\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\nhello" );
	CHECK( p.next() == std::optional<std::string>( "hello" ) );
	p.feed( "Content-Type: x\r\nCONTENT-LENGTH: 2\r\n\r\nok" );
	CHECK( p.next() == std::optional<std::string>( "ok" ) );
	// A header block without a length is skipped; the next message still parses.
	p.feed( "X-Junk: 1\r\n\r\nContent-Length: 2\r\n\r\n{}" );
	CHECK( p.next() == std::optional<std::string>( "{}" ) );
	CHECK( p.takeError() );
	CHECK( ! p.takeError() );
}

TEST_CASE( "reader and writer over a pipe" ) {
	int fds[2];
	REQUIRE( pipe2( fds, O_CLOEXEC ) == 0 );
	MessageWriter w( fds[1] );
	std::thread t( [&] {
		for ( int i = 0; i < 100; i += 1 ) w.write( "{\"n\":" + std::to_string( i ) + "}" );
		close( fds[1] );
	} );
	MessageReader r( fds[0] );
	int n = 0;
	while ( auto m = r.read() ) {
		CHECK( *m == "{\"n\":" + std::to_string( n ) + "}" );
		n += 1;
	}
	t.join();
	close( fds[0] );
	CHECK( n == 100 );
}
