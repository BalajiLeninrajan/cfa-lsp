#pragma once

namespace cfalsp {

// From `git describe` at build time: "0.2.0" on the commit tagged v0.2.0,
// "0.2.0-3-gabc1234" three commits after it, with "-dirty" for uncommitted
// changes. The Makefile writes the definition (build/version.cpp).
extern const char * const version;

} // namespace cfalsp
