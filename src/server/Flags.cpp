#include "Flags.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cfalsp {

std::vector<std::string> defaultFlags() { return { "-Wall", "-Wextra" }; }

std::vector<std::string> parseFlagsFile( const std::string & content ) {
	std::vector<std::string> out;
	std::istringstream in( content );
	std::string line;
	while ( std::getline( in, line ) ) {
		size_t b = line.find_first_not_of( " \t\r" );
		if ( b == std::string::npos || line[b] == '#' ) continue;
		std::istringstream words( line );
		std::string w;
		while ( words >> w ) out.push_back( w );
	}
	return out;
}

std::optional<std::string> findFlagsFile( const std::string & dir ) {
	std::error_code ec;
	fs::path d = fs::path( dir ).lexically_normal();
	for ( ;; ) {
		fs::path f = d / "cfa_flags.txt";
		if ( fs::is_regular_file( f, ec ) ) return f.string();
		if ( ! d.has_parent_path() || d.parent_path() == d ) return std::nullopt;
		d = d.parent_path();
	}
}

bool isTranslatorWarning( const std::string & name ) {
	// cforall/src/Common/SemanticError.hpp, WarningFormats
	static const char * names[] = {
		"self-assign", "reference-conversion", "aggregate-forward-decl", "superfluous-decl",
		"superfluous-else", "gcc-attributes", "c++-like-copy", "depreciated-trait-syntax",
	};
	for ( const char * n : names ) {
		if ( name == n ) return true;
	}
	return false;
}

static bool startsWith( const std::string & s, const std::string & p ) {
	return s.compare( 0, p.size(), p ) == 0;
}

static std::string absolute( const std::string & p, const std::string & base ) {
	if ( p.empty() || p[0] == '/' || base.empty() ) return p;
	return ( fs::path( base ) / p ).lexically_normal().string();
}

FlagSet classifyFlags( const std::vector<std::string> & flags, const std::string & baseDir ) {
	// Preprocessor flags that take a path argument, joined or separate.
	static const std::vector<std::string> pathFlags = {
		"-I", "-iquote", "-isystem", "-idirafter", "-include", "-imacros", "-isysroot",
	};
	static const std::vector<std::string> argFlags = { "-D", "-U", "-iprefix", "-iwithprefix", "-iwithprefixbefore" };
	// Flags whose separate argument is dropped with them.
	static const std::vector<std::string> skipWithArg = { "-o", "-MF", "-MT", "-MQ", "-x", "-Xlinker", "-l", "-L", "-compiler" };

	FlagSet out;
	for ( size_t i = 0; i < flags.size(); i += 1 ) {
		const std::string & f = flags[i];
		auto next = [&]() -> std::optional<std::string> {
			if ( i + 1 < flags.size() ) return flags[++i];
			return std::nullopt;
		};

		bool handled = false;
		for ( const auto & pf : pathFlags ) {
			if ( f == pf || ( startsWith( f, pf ) && pf.size() == 2 ) ) {
				std::optional<std::string> arg = f == pf ? next() : std::optional<std::string>( f.substr( pf.size() ) );
				if ( arg ) {
					std::string name = pf == "-iquote" ? "-I" : pf;
					out.cpp.push_back( name );
					out.cpp.push_back( absolute( *arg, baseDir ) );
				}
				handled = true;
				break;
			}
			if ( startsWith( f, pf ) && pf.size() > 2 && f.size() > pf.size() ) {
				// -isystem/foo style
				std::string name = pf == "-iquote" ? "-I" : pf;
				out.cpp.push_back( name );
				out.cpp.push_back( absolute( f.substr( pf.size() ), baseDir ) );
				handled = true;
				break;
			}
		}
		if ( handled ) continue;
		for ( const auto & af : argFlags ) {
			if ( f == af ) {
				if ( auto arg = next() ) {
					out.cpp.push_back( f );
					out.cpp.push_back( *arg );
				}
				handled = true;
				break;
			}
			if ( af.size() == 2 && startsWith( f, af ) ) {
				out.cpp.push_back( f );
				handled = true;
				break;
			}
		}
		if ( handled ) continue;
		for ( const auto & sf : skipWithArg ) {
			if ( f == sf ) {
				next();
				handled = true;
				break;
			}
		}
		if ( handled ) continue;

		if ( f == "-nodebug" ) {
			out.debug = false;
			out.cpp.push_back( f );
		} else if ( f == "-debug" ) {
			out.debug = true;
			out.cpp.push_back( f );
		} else if ( f == "-nolib" ) {
			out.nolib = true;
			out.cpp.push_back( f );
		} else if ( f == "-no-include-stdhdr" ) {
			out.cpp.push_back( f );
		} else if ( f == "-m32" || f == "-m64" ) {
			out.m32 = f == "-m32";
			out.cpp.push_back( f );
			out.backend.push_back( f );
		} else if ( startsWith( f, "-std=" ) || startsWith( f, "--std=" ) ) {
			out.cpp.push_back( f );
			out.backend.push_back( f );
		} else if ( f == "-w" ) {
			out.cpp.push_back( f );
			out.translator.push_back( f );
			out.backend.push_back( f );
		} else if ( startsWith( f, "-Wl," ) || startsWith( f, "-Wa," ) || startsWith( f, "-Wp," ) ) {
			// linker/assembler/preprocessor pass-through: not ours
		} else if ( startsWith( f, "-W" ) ) {
			out.cpp.push_back( f );
			out.backend.push_back( f );
			if ( f == "-Wall" || f == "-Werror" ) {
				out.translator.push_back( f );
			} else {
				std::string name = f.substr( startsWith( f, "-Wno-" ) ? 5 : 2 );
				if ( isTranslatorWarning( name ) ) out.translator.push_back( f );
			}
		} else if ( startsWith( f, "-pedantic" ) || startsWith( f, "-O" ) ) {
			out.backend.push_back( f );
		} else if ( startsWith( f, "-f" ) && ! startsWith( f, "-fdiagnostics" ) ) {
			out.backend.push_back( f );
		}
		// Anything else (-c, -g, -o, inputs, linker flags) doesn't matter for checking.
	}
	return out;
}

} // namespace cfalsp
