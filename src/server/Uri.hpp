#pragma once

#include <optional>
#include <string>

namespace cfalsp {

// file:// URIs <-> absolute paths. Bytes outside the unreserved set and '/'
// are percent-encoded; decoding accepts any case and a "localhost"
// authority. Returns nullopt for other schemes or remote authorities.
std::string pathToUri( const std::string & path );
std::optional<std::string> uriToPath( const std::string & uri );

} // namespace cfalsp
