#include "Toolchain.hpp"

#include <cstdlib>
#include <filesystem>
#include <sys/utsname.h>
#include <unistd.h>

#include "Process.hpp"

namespace fs = std::filesystem;

namespace cfalsp {

static bool isExecutable( const std::string & p ) {
	std::error_code ec;
	return ! p.empty() && fs::is_regular_file( p, ec ) && access( p.c_str(), X_OK ) == 0;
}

static bool isDir( const std::string & p ) {
	std::error_code ec;
	return ! p.empty() && fs::is_directory( p, ec );
}

std::string executableDir() {
	std::error_code ec;
	fs::path p = fs::read_symlink( "/proc/self/exe", ec );
	if ( ec ) return "";
	return p.parent_path().string();
}

static std::string hostArch() {
	struct utsname u;
	if ( uname( &u ) != 0 ) return "x64";
	std::string m = u.machine;
	if ( m == "x86_64" ) return "x64";
	if ( m == "aarch64" || m == "arm64" ) return "arm64";
	if ( m.size() == 4 && m[0] == 'i' && m.substr( 2 ) == "86" ) return "x86";
	return m;
}

std::string derivePreludeDir( const std::string & cfaPrefix, bool debug, bool nolib, bool m32 ) {
	if ( cfaPrefix.empty() ) return "";
	std::string arch = m32 ? "x86" : hostArch();
	std::string base = cfaPrefix + "/lib/cfa/" + arch + "-";
	std::string want = base + ( nolib ? "nolib" : debug ? "debug" : "nodebug" );
	// The driver falls back to nolib when the configuration isn't installed.
	for ( const std::string & d : { want, base + "nolib" } ) {
		if ( isDir( d + "/prelude" ) ) return d + "/prelude";	// build tree layout
		if ( isDir( d ) ) return d;
	}
	return "";
}

std::string Toolchain::preludeFor( const FlagSet & flags ) const {
	if ( ! preludeDir.empty() ) return preludeDir;
	return derivePreludeDir( cfaPrefix, flags.debug, flags.nolib, flags.m32 );
}

static bool under( const std::string & path, const std::string & dir ) {
	if ( dir.empty() ) return false;
	std::string d = dir.back() == '/' ? dir : dir + "/";
	return path.compare( 0, d.size(), d ) == 0;
}

bool Toolchain::isSystemPath( const std::string & path ) const {
	if ( path.empty() || path[0] != '/' ) return true;	// <built-in>, prelude names, ...
	for ( const std::string & dir : systemDirs() ) {
		if ( under( path, dir ) ) return true;
	}
	return false;
}

std::vector<std::string> Toolchain::systemDirs() const {
	std::vector<std::string> dirs;
	// Only the installed headers and libraries: when cfa is a dev build or a
	// test stand-in, its prefix can contain the user's project.
	if ( ! cfaPrefix.empty() ) {
		dirs.push_back( cfaPrefix + "/include" );
		dirs.push_back( cfaPrefix + "/lib" );
	}
	if ( ! preludeDir.empty() ) dirs.push_back( preludeDir );
	for ( const char * sys : { "/usr", "/lib", "/lib64", "/opt" } ) dirs.push_back( sys );
	return dirs;
}

Toolchain discoverToolchain( const ToolchainOptions & opts ) {
	Toolchain tc;
	tc.cfa = opts.cfa.empty() ? findInPath( "cfa" ) : ( isExecutable( opts.cfa ) ? opts.cfa : findInPath( opts.cfa ) );
	if ( ! tc.cfa.empty() ) {
		std::error_code ec;
		fs::path real = fs::canonical( tc.cfa, ec );
		if ( ! ec ) tc.cfaPrefix = real.parent_path().parent_path().string();
	}

	std::vector<std::string> cands;
	if ( ! opts.translator.empty() ) {
		cands.push_back( opts.translator );
	} else {
		if ( const char * env = std::getenv( "CFA_LSP_TRANSLATOR" ) ) {
			if ( *env ) cands.push_back( env );
		}
		if ( ! opts.exeDir.empty() ) {
			cands.push_back( opts.exeDir + "/../libexec/cfa-lsp/cfa-cpp" );
			cands.push_back( opts.exeDir + "/cforall/driver/cfa-cpp" );		// make BUILD=build
			cands.push_back( opts.exeDir + "/../cforall/driver/cfa-cpp" );	// make BUILD=build/<name>
		}
	}
	for ( const auto & c : cands ) {
		if ( isExecutable( c ) ) {
			tc.translator = fs::path( c ).lexically_normal().string();
			break;
		}
	}

	if ( ! opts.cc.empty() ) tc.cc = opts.cc;
	tc.preludeDir = opts.preludeDir;
	return tc;
}

} // namespace cfalsp
