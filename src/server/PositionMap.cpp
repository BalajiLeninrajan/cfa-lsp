#include "PositionMap.hpp"

namespace cfalsp {

TextEdit makeEdit( Loc start, Loc end, std::string_view newText ) {
	Loc ne = start;
	for ( char c : newText ) {
		if ( c == '\n' ) {
			ne.line += 1;
			ne.col = 0;
		} else {
			ne.col += 1;
		}
	}
	return { start, end, ne };
}

namespace {

// p >= e.end in the old text -> its place in the new text.
Loc shiftForward( Loc p, const TextEdit & e ) {
	if ( p.line == e.end.line ) return { e.newEnd.line, e.newEnd.col + ( p.col - e.end.col ) };
	return { p.line + ( e.newEnd.line - e.end.line ), p.col };
}

// p >= e.newEnd in the new text -> its place in the old text.
Loc shiftBack( Loc p, const TextEdit & e ) {
	if ( p.line == e.newEnd.line ) return { e.end.line, e.end.col + ( p.col - e.newEnd.col ) };
	return { p.line - ( e.newEnd.line - e.end.line ), p.col };
}

bool isNoop( const TextEdit & e ) { return e.start == e.end && e.newEnd == e.start; }

Loc clampStart( Loc s, const TextEdit & e ) {
	if ( s < e.start ) return s;
	if ( s >= e.end ) return shiftForward( s, e );
	return e.start;
}

Loc clampEnd( Loc x, const TextEdit & e ) {
	if ( x <= e.start ) return x;
	if ( x >= e.end ) return shiftForward( x, e );
	return e.newEnd;
}

} // namespace

MappedLoc toSnapshot( const EditList & edits, Loc cur ) {
	MappedLoc m{ cur, true };
	for ( auto it = edits.rbegin(); it != edits.rend(); ++it ) {
		const TextEdit & e = *it;
		if ( m.loc < e.start ) continue;
		if ( m.loc < e.newEnd ) {				// on a character this edit inserted
			m.loc = e.start;
			m.exact = false;
		} else {
			m.loc = shiftBack( m.loc, e );
		}
	}
	return m;
}

std::optional<Range> toCurrentExact( const EditList & edits, Range r ) {
	for ( const TextEdit & e : edits ) {
		if ( isNoop( e ) ) continue;
		if ( r.start == r.end ) {
			if ( r.start < e.start ) continue;
			if ( r.start >= e.end ) {
				r.start = r.end = shiftForward( r.start, e );
			} else if ( r.start != e.start ) {
				return std::nullopt;
			}
			continue;
		}
		if ( r.end <= e.start ) continue;
		if ( r.start >= e.end ) {
			r.start = shiftForward( r.start, e );
			r.end = shiftForward( r.end, e );
			continue;
		}
		return std::nullopt;
	}
	return r;
}

Range toCurrentClamped( const EditList & edits, Range r ) {
	for ( const TextEdit & e : edits ) {
		if ( isNoop( e ) ) continue;
		bool empty = !( r.start < r.end );
		r.start = clampStart( r.start, e );
		r.end = empty ? r.start : clampEnd( r.end, e );
		if ( r.end < r.start ) r.end = r.start;
	}
	return r;
}

} // namespace cfalsp
