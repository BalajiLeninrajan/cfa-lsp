#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "server/PositionMap.hpp"
#include "server/Text.hpp"

using namespace cfalsp;

namespace {

// Applies `newText` over [a, b) of `text`, records the edit like the server
// does and returns it.
TextEdit apply( Text & text, Loc a, Loc b, const std::string & newText ) {
	auto [s, e] = text.replace( a, b, newText );
	return makeEdit( s, e, newText );
}

Range R( int l1, int c1, int l2, int c2 ) { return { { l1, c1 }, { l2, c2 } }; }

} // namespace

TEST_CASE( "makeEdit computes the end of the inserted text" ) {
	CHECK( makeEdit( { 2, 3 }, { 2, 5 }, "" ).newEnd == Loc{ 2, 3 } );
	CHECK( makeEdit( { 2, 3 }, { 2, 5 }, "abc" ).newEnd == Loc{ 2, 6 } );
	CHECK( makeEdit( { 2, 3 }, { 4, 0 }, "x\nyz" ).newEnd == Loc{ 3, 2 } );
	CHECK( makeEdit( { 0, 0 }, { 0, 0 }, "\n" ).newEnd == Loc{ 1, 0 } );
}

TEST_CASE( "no edits map positions unchanged" ) {
	EditList none;
	CHECK( toSnapshot( none, { 3, 4 } ).loc == Loc{ 3, 4 } );
	CHECK( toSnapshot( none, { 3, 4 } ).exact );
	CHECK( *toCurrentExact( none, R( 1, 2, 1, 5 ) ) == R( 1, 2, 1, 5 ) );
	CHECK( toCurrentClamped( none, R( 1, 2, 3, 5 ) ) == R( 1, 2, 3, 5 ) );
}

TEST_CASE( "insertion before, inside and after a token" ) {
	// "int count = 0;" with the token "count" at [4, 9).
	Range tok = R( 0, 4, 0, 9 );
	SUBCASE( "typing before the token shifts it" ) {
		Text t( "int count = 0;" );
		EditList ed{ apply( t, { 0, 0 }, { 0, 0 }, "static " ) };
		CHECK( *toCurrentExact( ed, tok ) == R( 0, 11, 0, 16 ) );
		CHECK( t.str().substr( 11, 5 ) == "count" );
	}
	SUBCASE( "typing right at the token start shifts it" ) {
		Text t( "int count = 0;" );
		EditList ed{ apply( t, { 0, 4 }, { 0, 4 }, "my_" ) };
		CHECK( *toCurrentExact( ed, tok ) == R( 0, 7, 0, 12 ) );
	}
	SUBCASE( "typing inside the token drops it" ) {
		Text t( "int count = 0;" );
		EditList ed{ apply( t, { 0, 6 }, { 0, 6 }, "X" ) };
		CHECK( ! toCurrentExact( ed, tok ) );
		// but a clamped range grows to cover it
		CHECK( toCurrentClamped( ed, tok ) == R( 0, 4, 0, 10 ) );
	}
	SUBCASE( "typing right at the token end leaves it alone" ) {
		Text t( "int count = 0;" );
		EditList ed{ apply( t, { 0, 9 }, { 0, 9 }, "er" ) };
		CHECK( *toCurrentExact( ed, tok ) == tok );
	}
	SUBCASE( "a new line above moves it down" ) {
		Text t( "int count = 0;" );
		EditList ed{ apply( t, { 0, 0 }, { 0, 0 }, "// hi\n" ) };
		CHECK( *toCurrentExact( ed, tok ) == R( 1, 4, 1, 9 ) );
	}
	SUBCASE( "joining lines moves later tokens up and right" ) {
		Text t( "a\nint count = 0;" );
		Range tok2 = R( 1, 4, 1, 9 );
		EditList ed{ apply( t, { 0, 1 }, { 1, 0 }, " " ) };
		CHECK( t.str() == "a int count = 0;" );
		CHECK( *toCurrentExact( ed, tok2 ) == R( 0, 6, 0, 11 ) );
	}
}

TEST_CASE( "current to snapshot" ) {
	Text t( "x = foo;\ny = bar;" );
	EditList ed{ apply( t, { 0, 4 }, { 0, 7 }, "pu" ) };	// "x = pu;"
	CHECK( t.str() == "x = pu;\ny = bar;" );
	auto m = toSnapshot( ed, { 0, 5 } );						// on 'u': new text
	CHECK( ! m.exact );
	CHECK( m.loc == Loc{ 0, 4 } );
	m = toSnapshot( ed, { 0, 6 } );							// ';' existed
	CHECK( m.exact );
	CHECK( m.loc == Loc{ 0, 7 } );
	m = toSnapshot( ed, { 1, 4 } );
	CHECK( m.exact );
	CHECK( m.loc == Loc{ 1, 4 } );
	m = toSnapshot( ed, { 0, 2 } );
	CHECK( m.loc == Loc{ 0, 2 } );
}

TEST_CASE( "clamped ranges survive deletions" ) {
	Text t( "void f() {\n  int x;\n  int y;\n}\n" );
	Range body = R( 0, 9, 3, 1 );
	Range decl = R( 1, 2, 1, 8 );
	EditList ed{ apply( t, { 1, 0 }, { 2, 0 }, "" ) };		// delete "  int x;\n"
	CHECK( toCurrentClamped( ed, body ) == R( 0, 9, 2, 1 ) );
	CHECK( ! toCurrentExact( ed, decl ) );
	CHECK( toCurrentClamped( ed, decl ) == R( 1, 0, 1, 0 ) );
}

// Brute-force model: each edit is applied to a flat string. For every old
// offset we know where that character went (or that it was deleted), so
// ranges can be mapped one edit at a time without line arithmetic.
namespace {

struct Step {
	size_t a, b, ins;						// replaced [a, b) with `ins` bytes
	size_t oldLen;

	// Old offset -> new offset, or npos if the character was deleted. The
	// end-of-text offset always survives.
	size_t fwd( size_t j ) const {
		if ( j < a ) return j;
		if ( j >= b ) return j - b + a + ins;
		return std::string::npos;
	}
	// New offset -> old offset, plus whether that character is new.
	std::pair<size_t, bool> back( size_t p ) const {
		if ( p < a ) return { p, true };
		if ( p < a + ins ) return { a, false };
		return { p - a - ins + b, true };
	}
};

std::optional<std::pair<size_t, size_t>> modelExact( const std::vector<Step> & steps, size_t s, size_t e ) {
	for ( const Step & st : steps ) {
		if ( st.a == st.b && st.ins == 0 ) continue;
		for ( size_t j = s; j < e; j += 1 ) {
			if ( st.fwd( j ) == std::string::npos ) return std::nullopt;
			if ( j + 1 < e && st.fwd( j + 1 ) != st.fwd( j ) + 1 ) return std::nullopt;
		}
		size_t ns = st.fwd( s ), ne = st.fwd( e - 1 ) + 1;
		s = ns;
		e = ne;
	}
	return std::make_pair( s, e );
}

std::pair<size_t, size_t> modelClamped( const std::vector<Step> & steps, size_t s, size_t e ) {
	for ( const Step & st : steps ) {
		if ( st.a == st.b && st.ins == 0 ) continue;
		bool empty = s >= e;
		size_t ns = st.fwd( s ) != std::string::npos ? st.fwd( s ) : st.a;
		size_t ne;
		if ( empty ) ne = ns;
		else if ( e == 0 ) ne = 0;
		else ne = st.fwd( e - 1 ) != std::string::npos ? st.fwd( e - 1 ) + 1 : st.a + st.ins;
		if ( ne < ns ) ne = ns;
		s = ns;
		e = ne;
	}
	return { s, e };
}

std::pair<size_t, bool> modelBack( const std::vector<Step> & steps, size_t p ) {
	bool exact = true;
	for ( auto it = steps.rbegin(); it != steps.rend(); ++it ) {
		auto [q, ok] = it->back( p );
		p = q;
		exact = exact && ok;
	}
	return { p, exact };
}

} // namespace

TEST_CASE( "randomized edits agree with the brute-force model" ) {
	std::mt19937 rng( 12345 );
	const std::vector<std::string> pieces = { "a", "b", "\n", "xy", "é", "\n\n", "q\nr" };
	int checkedExact = 0, droppedExact = 0;
	for ( int round = 0; round < 300; round += 1 ) {
		std::string start;
		int len = (int)( rng() % 40 );
		for ( int i = 0; i < len; i += 1 ) start += pieces[rng() % pieces.size()];
		Text snap( start ), cur( start );
		EditList edits;
		std::vector<Step> steps;
		int nEdits = 1 + (int)( rng() % 6 );
		for ( int k = 0; k < nEdits; k += 1 ) {
			size_t n = cur.str().size();
			size_t a = n ? rng() % ( n + 1 ) : 0;
			size_t b = a + ( n - a ? rng() % ( std::min<size_t>( n - a, 8 ) + 1 ) : 0 );
			std::string ins;
			int m = (int)( rng() % 4 );
			for ( int i = 0; i < m; i += 1 ) ins += pieces[rng() % pieces.size()];
			steps.push_back( { a, b, ins.size(), n } );
			edits.push_back( apply( cur, cur.loc( a ), cur.loc( b ), ins ) );
		}
		const size_t sn = snap.str().size(), cn = cur.str().size();

		// current -> snapshot for every offset, including end of text
		for ( size_t p = 0; p <= cn; p += 1 ) {
			auto [q, exact] = modelBack( steps, p );
			MappedLoc m = toSnapshot( edits, cur.loc( p ) );
			INFO( "round " << round << " p " << p );
			REQUIRE( m.exact == exact );
			REQUIRE( snap.offset( m.loc ) == q );
			REQUIRE( m.loc == snap.loc( q ) );
		}

		// snapshot ranges -> current
		for ( size_t s = 0; s <= sn; s += 1 ) {
			for ( size_t e = s; e <= sn && e <= s + 6; e += 1 ) {
				Range r{ snap.loc( s ), snap.loc( e ) };
				INFO( "round " << round << " range " << s << ".." << e );
				auto [cs, ce] = modelClamped( steps, s, e );
				Range c = toCurrentClamped( edits, r );
				REQUIRE( c.start == cur.loc( cs ) );
				REQUIRE( c.end == cur.loc( ce ) );
				if ( s == e ) continue;
				auto want = modelExact( steps, s, e );
				auto got = toCurrentExact( edits, r );
				REQUIRE( got.has_value() == want.has_value() );
				if ( want ) {
					REQUIRE( got->start == cur.loc( want->first ) );
					REQUIRE( got->end == cur.loc( want->second ) );
					// The text under a surviving range is unchanged.
					REQUIRE( cur.str().substr( want->first, want->second - want->first ) == snap.str().substr( s, e - s ) );
					checkedExact += 1;
				} else {
					droppedExact += 1;
				}
			}
		}
	}
	// Make sure the generator exercised both outcomes.
	CHECK( checkedExact > 1000 );
	CHECK( droppedExact > 1000 );
}
