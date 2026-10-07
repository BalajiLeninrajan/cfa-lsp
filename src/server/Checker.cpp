#include "Checker.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "CompilerOutput.hpp"
#include "Log.hpp"
#include "SourceMap.hpp"

namespace fs = std::filesystem;

namespace cfalsp {

namespace {

std::optional<std::string> readFile( const std::string & path ) {
	std::ifstream in( path, std::ios::binary );
	if ( ! in ) return std::nullopt;
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool writeFile( const std::string & path, const std::string & data ) {
	std::ofstream out( path, std::ios::binary | std::ios::trunc );
	out << data;
	return (bool)out;
}

std::string quoteForMarker( const std::string & path ) {
	std::string out;
	for ( char c : path ) {
		if ( c == '"' || c == '\\' ) out += '\\';
		out += c;
	}
	return out;
}

// Absolute paths are normalized lexically; relative ones (prelude names,
// <built-in>) are left alone.
std::string normalize( const std::string & p ) {
	if ( p.empty() || p[0] != '/' ) return p;
	return fs::path( p ).lexically_normal().string();
}

// What quoteDirFlags makes of a directory without a comma.
const std::string quotePrefix = "-Wp,-iquote";

// An include directory as a key: normalized, without a trailing '/'.
std::string dirKey( const std::string & d ) {
	std::string n = normalize( d );
	while ( n.size() > 1 && n.back() == '/' ) n.pop_back();
	return n;
}

std::string replaceAll( std::string s, const std::string & from, const std::string & to ) {
	if ( from.empty() ) return s;
	for ( size_t p = s.find( from ); p != std::string::npos; p = s.find( from, p + to.size() ) ) s.replace( p, from.size(), to );
	return s;
}

// The first and last few non-empty lines of a tool's stderr: usage errors
// come first, crash reports last.
std::string excerpt( const std::string & text ) {
	std::vector<std::string> lines;
	std::istringstream in( text );
	std::string l;
	while ( std::getline( in, l ) ) {
		if ( ! l.empty() ) lines.push_back( l );
	}
	const size_t head = 3, tail = 7;
	std::string out;
	for ( size_t i = 0; i < lines.size(); i += 1 ) {
		if ( lines.size() > head + tail && i == head ) {
			out += "\n...";
			i = lines.size() - tail;
		}
		out += ( out.empty() ? "" : "\n" ) + lines[i];
	}
	return out;
}

int severityOf( const std::string & s ) {
	if ( s == "error" ) return 1;
	if ( s == "warning" ) return 2;
	return 3;
}

Diag fileLevel( const std::string & file, const std::string & message, int severity = 1 ) {
	Diag d;
	d.file = file;
	d.range = { { 0, 0 }, { 0, 0 } };
	d.wholeLine = true;
	d.severity = severity;
	d.message = message;
	d.source = "cfa-lsp";
	return d;
}

// ASCII quotes in gcc messages; compiler temp files go in our directory so
// they are removed with it even if the compiler is killed.
std::vector<std::pair<std::string, std::string>> childEnv( const std::string & dir ) {
	return { { "LC_ALL", "C" }, { "TMPDIR", dir } };
}

std::string baseName( const std::string & p ) {
	size_t s = p.rfind( '/' );
	return s == std::string::npos ? p : p.substr( s + 1 );
}

// One error on the main file's #include line (1-based `line`) standing for
// the errors in an included file.
std::optional<Diag> includeSummary( const std::string & file, const std::vector<Diag> & list,
									const std::string & mainPath, std::optional<int> line ) {
	if ( ! line ) return std::nullopt;
	int errors = 0;
	const Diag * first = nullptr;
	for ( const auto & d : list ) {
		if ( d.severity == 1 ) {
			errors += 1;
			if ( ! first ) first = &d;
		}
	}
	if ( errors == 0 ) return std::nullopt;
	Diag s;
	s.file = mainPath;
	s.range = { { *line - 1, 0 }, { *line - 1, 0 } };
	s.wholeLine = true;
	s.severity = 1;
	s.source = first->source;
	std::string firstLine = first->message.substr( 0, first->message.find( '\n' ) );
	s.message = std::to_string( errors ) + ( errors == 1 ? " error" : " errors" ) + " in included file " +
				baseName( file ) + ": " + firstLine;
	for ( const auto & d : list ) {
		if ( d.severity == 1 && s.related.size() < 20 ) s.related.push_back( { file, d.range, d.message } );
	}
	return s;
}

// Without line markers: the first #include line in `text` naming the file.
std::optional<int> includeLineByName( const std::string & text, const std::string & file ) {
	std::istringstream in( text );
	std::string l;
	std::string name = baseName( file );
	for ( int n = 1; std::getline( in, l ); n += 1 ) {
		size_t h = l.find_first_not_of( " \t" );
		if ( h != std::string::npos && l[h] == '#' && l.find( "include" ) != std::string::npos &&
			 ( l.find( "\"" + name + "\"" ) != std::string::npos || l.find( "/" + name + "\"" ) != std::string::npos ) ) {
			return n;
		}
	}
	return std::nullopt;
}

// Where each file was included, with the temp input renamed to the real file.
IncludeSites includeSites( const std::string & preprocessed, const std::string & tmpInput, const std::string & mainPath ) {
	IncludeSites sites;
	for ( auto & [file, site] : findIncludeSites( preprocessed ) ) {
		std::string parent = normalize( site.first ) == normalize( tmpInput ) ? mainPath : normalize( site.first );
		sites[normalize( file )] = { parent, site.second };
	}
	return sites;
}

// The main file's diagnostics, then for each other file a summary on the
// main file's #include line and the file's own diagnostics. In libcfa and
// system headers only errors are kept; names that aren't paths (<built-in>,
// prelude.cfa) get only the summary.
std::vector<Diag> withIncludeSummaries( std::vector<Diag> all, const std::string & mainPath, const IncludeSites & sites,
										const Toolchain & tc ) {
	std::vector<Diag> out;
	std::map<std::string, std::vector<Diag>> elsewhere;
	for ( auto & d : all ) {
		if ( d.file == mainPath ) out.push_back( std::move( d ) );
		else elsewhere[d.file].push_back( std::move( d ) );
	}
	for ( auto & [file, list] : elsewhere ) {
		if ( auto s = includeSummary( file, list, mainPath, includeLineInMain( sites, file, mainPath ) ) ) {
			out.push_back( std::move( *s ) );
		}
		bool system = tc.isSystemPath( file );
		if ( system && ( file.empty() || file[0] != '/' ) ) continue;
		for ( auto & d : list ) {
			if ( ! system || d.severity == 1 ) out.push_back( std::move( d ) );
		}
	}
	return out;
}

} // namespace

static std::string tempBase() {
	const char * t = std::getenv( "TMPDIR" );
	return t && *t ? t : "/tmp";
}

void removeStaleTempDirs() {
	std::error_code ec;
	auto now = fs::file_time_type::clock::now();
	for ( fs::directory_iterator it( tempBase(), ec ), end; ! ec && it != end; it.increment( ec ) ) {
		std::string name = it->path().filename().string();
		if ( name.size() != 14 || name.rfind( "cfa-lsp-", 0 ) != 0 ) continue;
		struct stat st;
		if ( lstat( it->path().c_str(), &st ) != 0 || ! S_ISDIR( st.st_mode ) || st.st_uid != getuid() ) continue;
		std::error_code tec;
		auto when = fs::last_write_time( it->path(), tec );
		if ( tec || now - when < std::chrono::hours( 1 ) ) continue;
		log::info( "removing stale ", it->path().string() );
		fs::remove_all( it->path(), tec );
	}
}

TempDir::TempDir() {
	std::string base = tempBase();
	std::string tmpl = base + "/cfa-lsp-XXXXXX";
	std::vector<char> buf( tmpl.begin(), tmpl.end() );
	buf.push_back( '\0' );
	if ( ! mkdtemp( buf.data() ) ) throw std::runtime_error( "cannot create a temporary directory in " + base );
	dir = buf.data();
}

TempDir::~TempDir() {
	std::error_code ec;
	fs::remove_all( dir, ec );
}

std::vector<Diag> diagsFromOutput( const std::string & output, const std::string & realPath,
								   const std::string & tmpInput, const std::string & cwd,
								   const Toolchain & tc, const std::string & source ) {
	std::vector<Diag> out;
	bool lastKept = false;					// notes belong to the diagnostic before them
	for ( const RawDiag & r : parseCompilerOutput( output ) ) {
		// An open header is checked as the main file. gcc quotes the pragma: "'#pragma once' in main file".
		if ( r.message.find( "#pragma once" ) != std::string::npos && r.message.find( "in main file" ) != std::string::npos ) {
			lastKept = false;
			continue;
		}
		std::string file = r.file;
		if ( file.empty() ) file = realPath;
		if ( ! file.empty() && file[0] != '/' && file[0] != '<' && ! cwd.empty() ) {
			// gcc prints names relative to its working directory.
			std::error_code ec;
			if ( fs::exists( fs::path( cwd ) / file, ec ) ) file = ( fs::path( cwd ) / file ).string();
		}
		file = normalize( file );
		if ( file == normalize( tmpInput ) ) file = realPath;
		if ( r.severity == "note" ) {
			if ( lastKept && file[0] == '/' ) {
				out.back().related.push_back( { file, { { r.line - 1, 0 }, { r.line - 1, 0 } }, r.message } );
			}
			continue;
		}
		lastKept = false;
		int severity = severityOf( r.severity );
		// In libcfa and system headers only errors are kept, for the header
		// itself: the warnings there aren't the user's to fix.
		if ( file != realPath && tc.isSystemPath( file ) && ( severity != 1 || file[0] != '/' ) ) continue;
		Diag d;
		d.file = file;
		d.severity = severity;
		d.message = r.message;
		for ( const auto & l : r.detail ) d.message += "\n" + l;
		d.source = source;
		d.code = r.option;
		if ( r.line <= 0 ) {
			d.wholeLine = true;
			d.range = { { 0, 0 }, { 0, 0 } };
		} else if ( r.col <= 0 ) {
			d.wholeLine = true;
			d.range = { { r.line - 1, 0 }, { r.line - 1, 0 } };
		} else {
			d.range = { { r.line - 1, r.col - 1 }, { r.line - 1, r.col - 1 } };
		}
		out.push_back( std::move( d ) );
		lastKept = true;
	}
	return out;
}

std::vector<std::string> Checker::cppCommand( const CheckRequest & req, const FlagSet & flags, const std::string & in,
											  const OverlayDirs & overlays ) const {
	std::vector<std::string> cmd = { tc.cfa, "-E", "-fdiagnostics-plain-output", "-fdiagnostics-column-unit=byte" };
	auto overlayOf = [&]( const std::string & dir ) -> const std::string * {
		std::string key = dirKey( dir );
		for ( const auto & [d, o] : overlays ) {
			if ( d == key ) return &o;
		}
		return nullptr;
	};
	// cpp looks for #include "x" in the directory of the file it reads, which
	// is our temp directory. The real file's directory stands in for it, as a
	// quote-only directory searched before the user's: with -I it would also
	// be searched for <x>, and a project header named like a libcfa header
	// would hide that one. Each directory holding an unsaved header comes
	// right after its overlay.
	std::string dir = fs::path( req.path ).parent_path().string();
	if ( auto o = overlayOf( dir ) ) {
		for ( const auto & f : quoteDirFlags( *o ) ) cmd.push_back( f );
	}
	for ( const auto & f : quoteDirFlags( dir ) ) cmd.push_back( f );
	for ( size_t i = 0; i < flags.cpp.size(); i += 1 ) {
		const std::string & f = flags.cpp[i];
		if ( f == "-I" && i + 1 < flags.cpp.size() ) {
			if ( auto o = overlayOf( flags.cpp[i + 1] ) ) {
				cmd.push_back( "-I" );
				cmd.push_back( *o );
			}
		} else if ( f.rfind( quotePrefix, 0 ) == 0 ) {
			if ( auto o = overlayOf( f.substr( quotePrefix.size() ) ) ) {
				for ( const auto & q : quoteDirFlags( *o ) ) cmd.push_back( q );
			}
		}
		cmd.push_back( f );
	}
	cmd.push_back( in );
	return cmd;
}

std::vector<std::string> Checker::translatorCommand( const CheckRequest & req, const FlagSet & flags,
													 const std::string & in, const std::string & json,
													 const std::string & cOut ) const {
	// cforall/driver/cfa.cc passes these to cfa-cpp through __CFA_FLAG*
	// variables (-Wall, -Werror, -w, CFA warnings, --prelude-dir, -L), and
	// cc1.cc adds the input file and --colors.
	std::vector<std::string> cmd = { tc.translator, "--lsp", json, "--lsp-focus", req.path };
	if ( req.stopAfterResolve ) cmd.push_back( "--lsp-stop-after-resolve" );
	for ( const std::string & f : req.extraFocus ) {
		if ( f == req.path ) continue;
		cmd.push_back( "--lsp-focus" );
		cmd.push_back( f );
	}
	if ( ! cOut.empty() ) {
		cmd.push_back( "--lsp-c-out" );
		cmd.push_back( cOut );
	}
	cmd.insert( cmd.end(), flags.translator.begin(), flags.translator.end() );
	std::string prelude = tc.preludeFor( flags );
	if ( ! prelude.empty() ) cmd.push_back( "--prelude-dir=" + prelude );
	cmd.push_back( "-L" );
	cmd.push_back( "--colors=never" );
	if ( req.skipSystemBodies ) {
		for ( const std::string & dir : tc.systemDirs() ) {
			cmd.push_back( "--lsp-skip-bodies" );
			cmd.push_back( dir );
		}
	}
	cmd.push_back( in );
	return cmd;
}

std::vector<std::string> Checker::backendCommand( const FlagSet & flags, const std::string & cFile ) const {
	// What cc1.cc runs on cfa-cpp's output (see `cfa -v`), minus code
	// generation.
	std::vector<std::string> cmd = { tc.cc, "-fsyntax-only", "-fdiagnostics-plain-output",
									 "-fdiagnostics-column-unit=byte", "-x", "cpp-output" };
	bool x86 = true;
#if ! defined( __x86_64__ ) && ! defined( __i386__ )
	x86 = false;
#endif
	if ( x86 ) cmd.push_back( "-mcx16" );
	bool hasStd = false, hasM = false;
	for ( const auto & f : flags.backend ) {
		if ( f.rfind( "-std=", 0 ) == 0 || f.rfind( "--std=", 0 ) == 0 ) hasStd = true;
		if ( f == "-m32" || f == "-m64" ) hasM = true;
	}
	if ( x86 && ! hasM ) cmd.push_back( "-m64" );
	cmd.push_back( "-fexceptions" );
	cmd.push_back( "-fwrapv" );
	cmd.insert( cmd.end(), flags.backend.begin(), flags.backend.end() );
	cmd.push_back( "-Wno-deprecated" );
	cmd.push_back( "-Wno-strict-aliasing" );
	cmd.push_back( "-Wno-cast-function-type" );
	if ( ! hasStd ) cmd.push_back( "-std=gnu11" );
	cmd.push_back( cFile );
	return cmd;
}

FrontResult Checker::fallback( const CheckRequest & req, const FlagSet & flags, TempDir & tmp, const std::string & in,
							   const CancelToken & cancel ) const {
	FrontResult fr;
	fr.status = FrontResult::Fallback;
	std::string cwd = fs::path( req.path ).parent_path().string();
	std::vector<std::string> cmd = { tc.cfa, "-fdiagnostics-plain-output", "-fdiagnostics-column-unit=byte" };
	for ( const auto & f : quoteDirFlags( cwd ) ) cmd.push_back( f );	// as in cppCommand
	cmd.insert( cmd.end(), flags.cpp.begin(), flags.cpp.end() );
	for ( const auto & f : flags.backend ) {
		if ( f.rfind( "-W", 0 ) != 0 && f.rfind( "-std", 0 ) != 0 && f.rfind( "--std", 0 ) != 0 && f != "-w" ) cmd.push_back( f );
	}
	cmd.insert( cmd.end(), { "-c", in, "-o", "/dev/null" } );
	RunOptions o;
	o.cwd = cwd;
	o.stderrPath = tmp.path() + "/cfa.err";
	o.timeout = req.timeout;
	o.env = childEnv( tmp.path() );
	RunResult r = runProcess( cmd, o, &cancel );
	if ( r.status == RunResult::Cancelled ) {
		fr.status = FrontResult::Cancelled;
		return fr;
	}
	std::string err = readFile( o.stderrPath ).value_or( "" );
	std::vector<Diag> all = diagsFromOutput( err, req.path, in, cwd, tc, "cfa" );
	std::map<std::string, std::vector<Diag>> elsewhere;
	for ( auto & d : all ) {
		if ( d.file != req.path ) elsewhere[d.file].push_back( d );
		fr.diags.push_back( std::move( d ) );
	}
	for ( const auto & [file, list] : elsewhere ) {
		if ( auto s = includeSummary( file, list, req.path, includeLineByName( req.text, file ) ) ) fr.diags.push_back( std::move( *s ) );
	}
	if ( ! r.ok() && fr.diags.empty() ) {
		fr.diags.push_back( fileLevel( req.path, "cfa " + r.describe() + ( err.empty() ? "" : ":\n" + excerpt( err ) ) ) );
	}
	return fr;
}

// builtins.cfa in the prelude directory is cpp output whose line markers name
// files on the machine libcfa was built on (.../libcfa/prelude/builtins.c,
// ../src/exception.h). For each such file, the runs of its lines in the
// prelude files: [first, first + count) are lines physical, physical + 1, ...
// of `prelude`.
struct MarkerRun {
	int first, count, physical;
	std::string prelude;
};
using PreludeMarkers = std::map<std::string, std::vector<MarkerRun>>;

static const PreludeMarkers & preludeMarkers( const std::string & preludeDir ) {
	static std::mutex m;
	static std::map<std::string, PreludeMarkers> cache;
	std::lock_guard<std::mutex> lock( m );
	auto found = cache.find( preludeDir );
	if ( found != cache.end() ) return found->second;
	PreludeMarkers & out = cache[preludeDir];
	std::error_code ec;
	for ( fs::directory_iterator it( preludeDir, ec ), end; ! ec && it != end; it.increment( ec ) ) {
		if ( it->path().extension() != ".cfa" ) continue;
		std::string prelude = it->path().string();
		std::istringstream in( readFile( prelude ).value_or( "" ) );
		std::string l;
		MarkerRun * run = nullptr;
		for ( int n = 1; std::getline( in, l ); n += 1 ) {
			if ( l.size() > 3 && l[0] == '#' && l[1] == ' ' && std::isdigit( (unsigned char)l[2] ) ) {
				size_t q = l.find( '"' ), e = q == std::string::npos ? q : l.find( '"', q + 1 );
				run = nullptr;
				if ( e == std::string::npos || l[q + 1] != '/' ) continue;
				auto & runs = out[l.substr( q + 1, e - q - 1 )];
				runs.push_back( { std::atoi( l.c_str() + 2 ), 0, n + 1, prelude } );
				run = &runs.back();
				continue;
			}
			if ( run ) run->count += 1;
		}
	}
	return out;
}

// The translator names prelude files without a directory (prelude.cfa,
// extras.cfa), or by the build machine's paths (see preludeMarkers). Point
// declarations there at the files in the prelude directory, so definition and
// doc comments can reach them.
static void preludePaths( nlohmann::json & dump, const std::string & preludeDir ) {
	if ( preludeDir.empty() || ! dump.is_object() || ! dump.contains( "decls" ) || ! dump["decls"].is_array() ) return;
	std::map<std::string, std::string> seen;
	std::map<std::string, bool> missing;
	const PreludeMarkers * markers = nullptr;
	for ( auto & d : dump["decls"] ) {
		if ( ! d.is_object() || ! d.contains( "file" ) || ! d["file"].is_string() ) continue;
		const std::string f = d["file"].get<std::string>();
		if ( f.empty() || f[0] == '<' ) continue;
		if ( f[0] != '/' ) {
			auto it = seen.find( f );
			if ( it == seen.end() ) {
				std::error_code ec;
				std::string p = preludeDir + "/" + f;
				it = seen.emplace( f, fs::is_regular_file( p, ec ) ? p : "" ).first;
			}
			if ( ! it->second.empty() ) d["file"] = it->second;
			continue;
		}
		auto gone = missing.find( f );
		if ( gone == missing.end() ) {
			std::error_code ec;
			gone = missing.emplace( f, ! fs::exists( f, ec ) ).first;
		}
		if ( ! gone->second ) continue;
		if ( ! markers ) markers = &preludeMarkers( preludeDir );
		auto runs = markers->find( f );
		if ( runs == markers->end() ) continue;
		auto physical = [&]( int line ) -> const MarkerRun * {
			for ( const MarkerRun & r : runs->second ) {
				if ( r.first <= line && line < r.first + r.count ) return &r;
			}
			return nullptr;
		};
		int line = d.value( "line", 0 );
		const MarkerRun * run = physical( line );
		if ( ! run ) continue;
		d["file"] = run->prelude;
		auto shift = [&]( nlohmann::json & j ) {
			if ( ! j.is_object() ) return;
			for ( const char * k : { "line", "endLine" } ) {
				if ( j.contains( k ) && j[k].is_number_integer() ) {
					const MarkerRun * r = physical( j[k].get<int>() );
					if ( r && r->prelude == run->prelude ) j[k] = r->physical + ( j[k].get<int>() - r->first );
				}
			}
		};
		shift( d );
		if ( d.contains( "nameRange" ) ) shift( d["nameRange"] );
		if ( d.contains( "body" ) ) shift( d["body"] );
	}
}

FrontResult Checker::front( const CheckRequest & req, const CancelToken & cancel ) const {
	FrontResult fr;
	fr.flags = classifyFlags( req.flags, req.flagsBase );
	if ( tc.cfa.empty() ) {
		fr.status = FrontResult::NoCfa;
		return fr;
	}
	try {
		fr.tmp = std::make_shared<TempDir>();
	} catch ( const std::exception & e ) {
		fr.status = FrontResult::PreprocessFailed;
		fr.diags.push_back( fileLevel( req.path, std::string( "cfa-lsp: " ) + e.what() ) );
		return fr;
	}
	const std::string dir = fr.tmp->path();
	const std::string in = dir + "/in.cfa";
	const std::string cwd = fs::path( req.path ).parent_path().string();
	if ( ! writeFile( in, "# 1 \"" + quoteForMarker( req.path ) + "\"\n" + req.text ) ) {
		fr.status = FrontResult::PreprocessFailed;
		fr.diags.push_back( fileLevel( req.path, "cfa-lsp: cannot write " + in ) );
		return fr;
	}

	if ( tc.translator.empty() ) return fallback( req, fr.flags, *fr.tmp, in, cancel );

	auto clock = std::chrono::steady_clock::now();
	auto lap = [&clock]( const char * what ) {
		auto now = std::chrono::steady_clock::now();
		log::info( what, ": ", std::chrono::duration_cast<std::chrono::milliseconds>( now - clock ).count(), " ms" );
		clock = now;
	};

	// Unsaved headers: for each include directory (then the file's own) that
	// holds one, a copy of it under an overlay directory searched first. The
	// overlay paths in cpp's output are put back to the real ones.
	OverlayDirs overlays;
	if ( ! req.overlays.empty() ) {
		std::vector<std::string> search = { dirKey( cwd ) };
		for ( size_t i = 0; i < fr.flags.cpp.size(); i += 1 ) {
			const std::string & f = fr.flags.cpp[i];
			if ( f == "-I" && i + 1 < fr.flags.cpp.size() ) search.push_back( dirKey( fr.flags.cpp[i + 1] ) );
			else if ( f.rfind( quotePrefix, 0 ) == 0 ) search.push_back( dirKey( f.substr( quotePrefix.size() ) ) );
		}
		for ( const std::string & d : search ) {
			bool seen = d.size() <= 1;
			for ( const auto & ov : overlays ) seen = seen || ov.first == d;
			if ( seen ) continue;
			std::string odir = dir + "/overlay" + std::to_string( overlays.size() );
			bool used = false;
			for ( const auto & [path, text] : req.overlays ) {
				std::string p = normalize( path );
				if ( p.size() <= d.size() + 1 || p.compare( 0, d.size(), d ) != 0 || p[d.size()] != '/' ) continue;
				fs::path target = fs::path( odir ) / p.substr( d.size() + 1 );
				std::error_code ec;
				fs::create_directories( target.parent_path(), ec );
				if ( ec || ! writeFile( target.string(), text ) ) {
					log::warn( "cannot write overlay ", target.string() );
					continue;
				}
				used = true;
			}
			if ( used ) overlays.push_back( { d, odir } );
		}
	}
	auto unOverlay = [&overlays]( std::string s ) {
		for ( const auto & [d, o] : overlays ) s = replaceAll( std::move( s ), o + "/", d + "/" );
		return s;
	};

	// 1. Preprocess.
	RunOptions o;
	o.cwd = cwd;
	o.stdoutPath = dir + "/in.i";
	o.stderrPath = dir + "/cpp.err";
	o.timeout = req.timeout;
	o.env = childEnv( dir );
	RunResult r = runProcess( cppCommand( req, fr.flags, in, overlays ), o, &cancel );
	if ( r.status == RunResult::Cancelled ) {
		fr.status = FrontResult::Cancelled;
		return fr;
	}
	lap( "cfa -E" );
	std::string cppErr = unOverlay( readFile( o.stderrPath ).value_or( "" ) );
	if ( ! overlays.empty() ) {
		// The translator reads in.i itself.
		if ( ! writeFile( o.stdoutPath, unOverlay( readFile( o.stdoutPath ).value_or( "" ) ) ) && r.ok() ) {
			fr.status = FrontResult::PreprocessFailed;
			fr.diags.push_back( fileLevel( req.path, "cfa-lsp: cannot write " + o.stdoutPath ) );
			return fr;
		}
	}
	std::vector<Diag> cppDiags = diagsFromOutput( cppErr, req.path, in, cwd, tc, "cpp" );
	if ( ! r.ok() ) {
		fr.status = FrontResult::PreprocessFailed;
		// cpp writes what it has so far, so the include sites up to the
		// error are there.
		fr.diags = withIncludeSummaries( cppDiags, req.path, includeSites( readFile( o.stdoutPath ).value_or( "" ), in, req.path ), tc );
		bool anyError = false;
		for ( const auto & d : cppDiags ) anyError = anyError || d.severity == 1;
		if ( ! anyError ) {
			fr.diags.push_back( fileLevel( req.path, "cfa -E " + r.describe() + ( cppErr.empty() ? "" : ":\n" + excerpt( cppErr ) ) ) );
		}
		return fr;
	}
	if ( cancel.cancelled() ) {
		fr.status = FrontResult::Cancelled;
		return fr;
	}

	// 2. Translate.
	const std::string json = dir + "/out.json";
	// Code generation comes after the passes that stopAfterResolve skips.
	const std::string cOut = req.backend && ! req.stopAfterResolve ? dir + "/out.c" : "";
	RunOptions t;
	t.cwd = cwd;
	t.stdoutPath = dir + "/cfa-cpp.out";
	t.stderrPath = dir + "/cfa-cpp.err";
	t.timeout = req.timeout;
	t.env = childEnv( dir );
	r = runProcess( translatorCommand( req, fr.flags, dir + "/in.i", json, cOut ), t, &cancel );
	if ( r.status == RunResult::Cancelled ) {
		fr.status = FrontResult::Cancelled;
		return fr;
	}
	lap( "translator" );
	auto dumpText = readFile( json );
	if ( ! r.ok() || ! dumpText || dumpText->empty() ) {
		fr.status = FrontResult::TranslatorFailed;
		fr.diags = cppDiags;
		// cfa-cpp prints some errors (bad options) on stdout.
		std::string err = readFile( t.stdoutPath ).value_or( "" ) + readFile( t.stderrPath ).value_or( "" );
		// The translator writes the dump whenever it can, even after an assertion failure, so getting none is a
		// crash. Our own limits are reported as such.
		std::string what = r.status == RunResult::TimedOut || r.status == RunResult::SpawnFailed
			? "cfa-lsp: the translator " + r.describe()
			: "internal translator error: cfa-cpp " + ( r.ok() ? std::string( "wrote no output" ) : r.describe() );
		fr.diags.push_back( fileLevel( req.path, what + ( err.empty() ? "" : ":\n" + excerpt( err ) ) ) );
		return fr;
	}

	// 3. Load.
	auto preprocessed = readFile( o.stdoutPath ).value_or( "" );
	struct Cache {
		std::mutex m;
		std::map<std::string, std::optional<std::string>> files;
	};
	auto cache = std::make_shared<Cache>();
	SourceMap::Reader reader = [cache, main = req.path, text = req.text, buffers = req.overlays, cwd]( const std::string & p ) -> std::optional<std::string> {
		std::string path = p;
		if ( ! path.empty() && path[0] != '/' ) path = ( fs::path( cwd ) / path ).string();
		path = normalize( path );
		if ( path == main ) return text;
		if ( auto b = buffers.find( path ); b != buffers.end() ) return b->second;
		std::lock_guard<std::mutex> lock( cache->m );
		auto it = cache->files.find( path );
		if ( it != cache->files.end() ) return it->second;
		return cache->files[path] = readFile( path );
	};
	nlohmann::json dump;
	try {
		dump = nlohmann::json::parse( *dumpText );
		preludePaths( dump, tc.preludeFor( fr.flags ) );
		SourceMap sm( preprocessed, reader );
		fr.analysis = Analysis::load( dump, sm, reader );
		lap( "load" );
	} catch ( const std::exception & e ) {
		fr.status = FrontResult::LoadFailed;
		fr.diags = cppDiags;
		fr.diags.push_back( fileLevel( req.path, std::string( "cfa-lsp: cannot load the translator's output: " ) + e.what() ) );
		return fr;
	}

	fr.usable = dump.value( "complete", false );
	if ( ! fr.usable && dump.contains( "decls" ) && dump["decls"].is_array() ) {
		for ( const auto & d : dump["decls"] ) {
			if ( ! d.is_object() || d.value( "generated", false ) ) continue;
			auto f = d.find( "file" );
			if ( f != d.end() && f->is_string() && normalize( f->get<std::string>() ) == req.path ) {
				fr.usable = true;
				break;
			}
		}
	}

	// 4. Diagnostics: main file, other files, and a summary on the #include
	// line for errors in included files.
	std::vector<Diag> all = cppDiags;
	bool anyError = false;
	for ( const Diagnostic & d : fr.analysis->diagnostics() ) {
		Diag x;
		std::string file = d.loc.file;
		if ( ! file.empty() && file[0] != '/' && file[0] != '<' ) {
			std::error_code ec;
			if ( fs::exists( fs::path( cwd ) / file, ec ) ) file = ( fs::path( cwd ) / file ).string();
		}
		x.file = normalize( file );
		if ( x.file == normalize( in ) ) x.file = req.path;
		x.range = d.loc.range;
		x.severity = d.severity;
		x.message = d.message;
		x.source = d.source.empty() ? "cfa" : d.source;
		if ( x.severity == 1 ) anyError = true;
		all.push_back( std::move( x ) );
	}
	IncludeSites sites = includeSites( preprocessed, in, req.path );
	for ( const auto & [f, site] : sites ) {
		if ( ! f.empty() && f[0] == '/' && f != req.path && f != normalize( in ) && ! tc.isSystemPath( f ) ) fr.files.push_back( f );
	}
	fr.diags = withIncludeSummaries( std::move( all ), req.path, sites, tc );

	// Errors in the passes that only check the program don't stop code
	// generation, so there can be C even when the translator reported errors.
	std::error_code ec;
	fr.cOut = cOut;
	fr.translatorErrors = anyError;
	fr.backendReady = ! cOut.empty() && fs::file_size( cOut, ec ) > 0 && ! ec;
	fr.status = FrontResult::Ok;
	return fr;
}

std::vector<Diag> Checker::back( const CheckRequest & req, const FrontResult & fr, const CancelToken & cancel ) const {
	std::vector<Diag> out;
	if ( ! fr.backendReady || ! fr.tmp ) return out;
	const std::string dir = fr.tmp->path();
	const std::string cwd = fs::path( req.path ).parent_path().string();
	RunOptions o;
	o.cwd = dir;
	o.stderrPath = dir + "/gcc.err";
	o.timeout = req.timeout;
	o.env = childEnv( dir );
	RunResult r = runProcess( backendCommand( fr.flags, fr.cOut ), o, &cancel );
	if ( r.status == RunResult::Cancelled ) return out;
	std::string err = readFile( o.stderrPath ).value_or( "" );
	if ( r.status != RunResult::Exited ) {
		log::warn( "backend compiler ", r.describe() );
		return out;
	}

	std::optional<LineMarkerMap> markers;
	std::set<std::tuple<std::string, int, int, std::string>> seen;
	bool lastKept = false;
	for ( const RawDiag & raw : parseCompilerOutput( err ) ) {
		std::string file = raw.file;
		int line = raw.line;
		bool generated = file.empty() || normalize( file[0] == '/' ? file : dir + "/" + file ) == normalize( fr.cOut );
		if ( generated && line > 0 ) {
			if ( ! markers ) markers.emplace( readFile( fr.cOut ).value_or( "" ) );
			auto src = markers->lookup( line );
			if ( ! src ) {
				lastKept = false;
				continue;
			}
			file = src->first;
			line = src->second;
		} else if ( generated ) {
			lastKept = false;
			continue;
		}
		if ( ! file.empty() && file[0] != '/' ) {
			std::error_code ec;
			if ( fs::exists( fs::path( cwd ) / file, ec ) ) file = ( fs::path( cwd ) / file ).string();
		}
		file = normalize( file );
		if ( file == normalize( dir + "/in.cfa" ) ) file = req.path;
		if ( raw.severity == "note" ) {
			if ( lastKept && file[0] == '/' && ! out.empty() ) {
				out.back().related.push_back( { file, { { line - 1, 0 }, { line - 1, 0 } }, raw.message } );
			}
			continue;
		}
		lastKept = false;
		if ( line <= 0 || ( file != req.path && tc.isSystemPath( file ) ) ) continue;
		int sev = severityOf( raw.severity );
		// C made despite translator errors: gcc's errors there are about
		// code the translator already rejected.
		if ( fr.translatorErrors && sev == 1 ) continue;
		if ( ! seen.insert( { file, line, sev, raw.message } ).second ) continue;
		Diag d;
		d.file = file;
		d.range = { { line - 1, 0 }, { line - 1, 0 } };
		d.wholeLine = true;
		d.severity = sev;
		d.message = raw.message;
		d.source = "gcc";
		d.code = raw.option;
		out.push_back( std::move( d ) );
		lastKept = true;
	}
	return out;
}

} // namespace cfalsp
