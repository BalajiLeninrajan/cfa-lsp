#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Types.hpp"

namespace cfalsp {

// Maps positions between a snapshot (the text an Analysis was made from)
// and the current buffer, given the edits made since the snapshot.
//
// An edit replaces [start, end) of the text before it with new text whose
// end, in the text after it, is newEnd. Positions are (line, byte column).

struct TextEdit {
	Loc start, end;							// in the text before the edit
	Loc newEnd;								// in the text after the edit
	friend bool operator==( const TextEdit &, const TextEdit & ) = default;
};

TextEdit makeEdit( Loc start, Loc end, std::string_view newText );

using EditList = std::vector<TextEdit>;		// oldest first

struct MappedLoc {
	Loc loc;
	bool exact = true;						// false: `cur` was inside newly typed text
};

// Current position -> snapshot position. A position on a character that
// did not exist in the snapshot clamps to the start of the edit that
// created it and is marked inexact.
MappedLoc toSnapshot( const EditList & edits, Loc cur );

// Snapshot range -> current range for things that must stay exact
// (semantic tokens, references, definition targets): nullopt if any edit
// touched the inside of the range. Edits that end at the range start or
// begin at its end only shift it.
std::optional<Range> toCurrentExact( const EditList & edits, Range r );

// Snapshot range -> current range for things that should survive edits
// (diagnostics, symbol extents): endpoints inside an edited region clamp to
// the edges of the replacement text, so the range never disappears.
Range toCurrentClamped( const EditList & edits, Range r );

} // namespace cfalsp
