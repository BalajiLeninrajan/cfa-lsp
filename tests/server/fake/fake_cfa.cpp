// A stand-in for cfa, the forked cfa-cpp and gcc in server tests. The mode
// comes from the arguments:
//
//   -E ... IN            preprocess: copy IN to stdout, expanding
//                        #include "x" from the directory named in IN's first
//                        line marker. A line containing FAKE_CPP_ERROR makes
//                        it fail with a gcc-style error on that line.
//   --lsp OUT ...        translate: copy $FAKE_CFA_DIR/dump.json (or
//                        dump-error.json if the input contains
//                        FAKE_HEADER_ERROR or FAKE_CHECK_ERROR) to OUT and
//                        $FAKE_CFA_DIR/out.c to the --lsp-c-out file, with
//                        @FILE@ replaced by the --lsp-focus path and @DIR@ by
//                        its directory. Like the real translator, it writes no
//                        C after errors, except with FAKE_CHECK_ERROR (errors
//                        from a pass that only checks), where the C also gets a
//                        FAKE_GCC_ERROR line. If the input contains FAKE_SLOW
//                        it first forks a grandchild, writes both pids to
//                        $FAKE_CFA_PIDS and sleeps.
//   -fsyntax-only ... C  backend: print $FAKE_CFA_DIR/gcc.err to stderr with
//                        @CFILE@ replaced by C, and an error on line 6 if C
//                        contains FAKE_GCC_ERROR.
//
// Every invocation appends its argv to $FAKE_CFA_LOG if set.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

static std::string readFile( const std::string & p ) {
	std::ifstream in( p, std::ios::binary );
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static std::string replaceAll( std::string s, const std::string & from, const std::string & to ) {
	size_t pos = 0;
	while ( ( pos = s.find( from, pos ) ) != std::string::npos ) {
		s.replace( pos, from.size(), to );
		pos += to.size();
	}
	return s;
}

static std::string env( const char * name ) {
	const char * v = std::getenv( name );
	return v ? v : "";
}

static void preprocessFile( const std::string & text, const std::string & name, std::string & out, int depth ) {
	std::istringstream in( text );
	std::string line;
	int n = 0;
	std::string dir = name.substr( 0, name.rfind( '/' ) + 1 );
	while ( std::getline( in, line ) ) {
		n += 1;
		if ( depth == 0 && n == 1 && line.rfind( "# 1 \"", 0 ) == 0 ) {
			out += line + "\n";
			continue;
		}
		if ( line.find( "FAKE_CPP_ERROR" ) != std::string::npos ) {
			int lineNo = depth == 0 ? n - 1 : n;
			std::fprintf( stderr, "%s:%d:1: error: #error FAKE_CPP_ERROR\n", name.c_str(), lineNo );
			std::exit( 1 );
		}
		if ( line.rfind( "#include \"", 0 ) == 0 ) {
			std::string inc = line.substr( 10, line.find( '"', 10 ) - 10 );
			std::string path = dir + inc;
			std::ifstream f( path );
			if ( ! f ) {
				std::fprintf( stderr, "%s:%d:10: fatal error: %s: No such file or directory\ncompilation terminated.\n",
							  name.c_str(), depth == 0 ? n - 1 : n, inc.c_str() );
				std::exit( 1 );
			}
			out += "# 1 \"" + path + "\" 1\n";
			preprocessFile( readFile( path ), path, out, depth + 1 );
			out += "# " + std::to_string( depth == 0 ? n : n + 1 ) + " \"" + name + "\" 2\n";
			continue;
		}
		out += line + "\n";
	}
}

int main( int argc, char * argv[] ) {
	std::vector<std::string> args( argv + 1, argv + argc );
	if ( ! env( "FAKE_CFA_LOG" ).empty() ) {
		std::ofstream log( env( "FAKE_CFA_LOG" ), std::ios::app );
		for ( const auto & a : args ) log << a << ' ';
		log << '\n';
	}
	auto has = [&]( const std::string & f ) {
		for ( const auto & a : args ) if ( a == f ) return true;
		return false;
	};
	auto after = [&]( const std::string & f ) -> std::string {
		for ( size_t i = 0; i + 1 < args.size(); i += 1 ) if ( args[i] == f ) return args[i + 1];
		return "";
	};
	std::string dir = env( "FAKE_CFA_DIR" );

	if ( has( "-E" ) ) {
		std::string in = args.back();
		std::string text = readFile( in );
		// The real name comes from the first line marker.
		std::string name = in;
		if ( text.rfind( "# 1 \"", 0 ) == 0 ) name = text.substr( 5, text.find( '"', 5 ) - 5 );
		std::string out;
		preprocessFile( text, name, out, 0 );
		std::fwrite( out.data(), 1, out.size(), stdout );
		return 0;
	}

	if ( has( "--lsp" ) ) {
		std::string in = args.back();
		std::string focus = after( "--lsp-focus" );
		if ( readFile( in ).find( "FAKE_SLOW" ) != std::string::npos ) {
			pid_t kid = fork();
			if ( kid == 0 ) {
				sleep( 60 );
				_exit( 0 );
			}
			if ( ! env( "FAKE_CFA_PIDS" ).empty() ) {
				std::ofstream p( env( "FAKE_CFA_PIDS" ) + ".tmp" );
				p << getpid() << ' ' << kid << '\n';
				p.close();
				std::rename( ( env( "FAKE_CFA_PIDS" ) + ".tmp" ).c_str(), env( "FAKE_CFA_PIDS" ).c_str() );
			}
			sleep( 60 );
		}
		std::string focusDir = focus.substr( 0, focus.rfind( '/' ) );
		bool headerError = readFile( in ).find( "FAKE_HEADER_ERROR" ) != std::string::npos;
		bool checkError = readFile( in ).find( "FAKE_CHECK_ERROR" ) != std::string::npos;
		std::string dumpName = headerError || checkError ? "/dump-error.json" : "/dump.json";
		std::string dump = replaceAll( readFile( dir + dumpName ), "@FILE@", focus );
		std::ofstream( after( "--lsp" ) ) << replaceAll( dump, "@DIR@", focusDir );
		std::string cOut = after( "--lsp-c-out" );
		if ( ! cOut.empty() && ! headerError ) {
			std::ofstream c( cOut );
			c << replaceAll( readFile( dir + "/out.c" ), "@FILE@", focus );
			if ( checkError ) c << "/* FAKE_GCC_ERROR */\n";
		}
		return 0;
	}

	if ( has( "-fsyntax-only" ) ) {
		std::string err = replaceAll( readFile( dir + "/gcc.err" ), "@CFILE@", args.back() );
		if ( readFile( args.back() ).find( "FAKE_GCC_ERROR" ) != std::string::npos ) err += args.back() + ":6:1: error: fake gcc error\n";
		std::fwrite( err.data(), 1, err.size(), stderr );
		return 0;
	}

	std::fprintf( stderr, "fake_cfa: unknown mode\n" );
	return 2;
}
