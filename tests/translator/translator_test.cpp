// Runs `cfa -E` and the forked translator (--lsp) on the fixtures in
// tests/fixtures/translator and checks the dump.
//
// Columns asserted here are taken from the fixture source, so they are only
// used on lines without indentation, macros or repeated spaces, where the
// preprocessed line and the original line agree.

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string env( const char * name ) {
	const char * value = std::getenv( name );
	return value ? value : "";
}

std::string quote( const std::string & s ) {
	std::string out = "'";
	for ( char c : s ) {
		if ( c == '\'' ) out += "'\\''";
		else out += c;
	}
	return out + "'";
}

std::string readFile( const fs::path & path ) {
	std::ifstream in( path, std::ios::binary );
	std::ostringstream buf;
	buf << in.rdbuf();
	return buf.str();
}

std::string findOnPath( const std::string & name ) {
	std::stringstream path( env( "PATH" ) );
	std::string dir;
	while ( std::getline( path, dir, ':' ) ) {
		fs::path cand = fs::path( dir ) / name;
		if ( ! dir.empty() && access( cand.c_str(), X_OK ) == 0 ) return cand.string();
	}
	return "";
}

struct Tools {
	std::string translator, cfa, prelude, fixtures, skip;
};

const Tools & tools() {
	static Tools t = [] {
		Tools t;
		t.translator = env( "CFA_LSP_TRANSLATOR" );
		t.cfa = env( "CFA" ).empty() ? findOnPath( "cfa" ) : env( "CFA" );
		std::string fixtures = env( "CFA_LSP_FIXTURES" );
		t.fixtures = fixtures.empty() ? "tests/fixtures" : fixtures;
		if ( t.translator.empty() || access( t.translator.c_str(), X_OK ) != 0 ) {
			t.skip = "translator not built (CFA_LSP_TRANSLATOR)";
			return t;
		}
		if ( t.cfa.empty() ) {
			t.skip = "cfa not found";
			return t;
		}
		// The installed driver lives in <prefix>/bin; the prelude in <prefix>/lib/cfa/<arch>-debug.
		t.prelude = env( "CFA_LSP_PRELUDE" );
		if ( t.prelude.empty() ) {
			std::error_code ec;
			fs::path prefix = fs::canonical( t.cfa, ec ).parent_path().parent_path();
			for ( const char * arch : { "x64-debug", "arm64-debug", "x86-debug" } ) {
				fs::path dir = prefix / "lib" / "cfa" / arch;
				if ( fs::exists( dir / "prelude.cfa" ) ) {
					t.prelude = dir.string();
					break;
				}
			}
		}
		if ( t.prelude.empty() ) t.skip = "cfa prelude directory not found (CFA_LSP_PRELUDE)";
		return t;
	}();
	return t;
}

struct Run {
	std::string file;									// the fixture, absolute
	std::vector<std::string> lines;						// its original text, 1-based via line()
	json dump;
	int status = -1;
	std::string c;										// generated C, with --lsp-c-out

	const std::string & line( int n ) const {
		static const std::string none;
		return n >= 1 && n <= (int)lines.size() ? lines[n - 1] : none;
	}

	// 0-based column of the nth occurrence of `word` (as a whole identifier) on a line.
	int col( int n, const std::string & word, int nth = 0 ) const {
		const std::string & text = line( n );
		auto isId = []( char c ) { return isalnum( (unsigned char)c ) || c == '_'; };
		for ( size_t pos = text.find( word ); pos != std::string::npos; pos = text.find( word, pos + 1 ) ) {
			bool start = pos == 0 || ! isId( text[pos - 1] ) || ! isId( word[0] );
			bool end = pos + word.size() >= text.size() || ! isId( text[pos + word.size()] ) || ! isId( word.back() );
			if ( start && end && nth-- == 0 ) return (int)pos;
		}
		return -1;
	}
};

// Runs each fixture once (per withC). withC adds --lsp-c-out and keeps the generated C in Run::c.
const Run * run( const std::string & name, bool withC = false ) {
	static std::map<std::string, Run> runs;
	const Tools & t = tools();
	if ( ! t.skip.empty() ) {
		MESSAGE( "skipping: " << t.skip );
		return nullptr;
	}
	std::string key = name + ( withC ? ":c" : "" );
	auto found = runs.find( key );
	if ( found != runs.end() ) return &found->second;

	Run r;
	r.file = fs::absolute( fs::path( t.fixtures ) / "translator" / name ).lexically_normal().string();
	std::stringstream text( readFile( r.file ) );
	for ( std::string l; std::getline( text, l ); ) r.lines.push_back( l );

	std::string dirTemplate = ( fs::temp_directory_path() / "cfa-lsp-translator-XXXXXX" ).string();
	std::vector<char> buf( dirTemplate.begin(), dirTemplate.end() );
	buf.push_back( '\0' );
	const char * dir = mkdtemp( buf.data() );
	REQUIRE( dir );
	fs::path work( dir );

	std::string pre = t.cfa + " -E " + quote( r.file ) + " > " + quote( ( work / "in.i" ).string() ) + " 2> " + quote( ( work / "cpp.err" ).string() );
	REQUIRE_MESSAGE( std::system( pre.c_str() ) == 0, "cfa -E failed: " << readFile( work / "cpp.err" ) );

	std::string cmd = quote( t.translator ) + " --prelude-dir=" + quote( t.prelude )
		+ " --lsp " + quote( ( work / "out.json" ).string() ) + " --lsp-focus " + quote( r.file )
		+ ( withC ? " --lsp-c-out " + quote( ( work / "out.c" ).string() ) : std::string() )
		+ " " + quote( ( work / "in.i" ).string() ) + " > " + quote( ( work / "translator.err" ).string() ) + " 2>&1";
	int rc = std::system( cmd.c_str() );
	r.status = WIFEXITED( rc ) ? WEXITSTATUS( rc ) : -1;
	INFO( readFile( work / "translator.err" ) );
	REQUIRE( r.status == 0 );
	r.dump = json::parse( readFile( work / "out.json" ) );
	if ( withC ) r.c = readFile( work / "out.c" );
	fs::remove_all( work );
	return &runs.emplace( key, std::move( r ) ).first->second;
}

const json * declById( const Run & r, int id ) {
	const json & decls = r.dump["decls"];
	if ( id < 0 || id >= (int)decls.size() ) return nullptr;
	return &decls[id];
}

// The reference whose range covers (line, col) in the fixture.
const json * refAt( const Run & r, int line, int col ) {
	for ( const json & ref : r.dump["refs"] ) {
		if ( ref["file"] == r.file && ref["line"] == line && ref["col"].get<int>() <= col && col < ref["endCol"].get<int>() ) return &ref;
	}
	return nullptr;
}

std::vector<const json *> declsNamed( const Run & r, const std::string & name ) {
	std::vector<const json *> out;
	for ( const json & d : r.dump["decls"] ) {
		if ( d["name"] == name ) out.push_back( &d );
	}
	return out;
}

const json * declAt( const Run & r, const std::string & name, int line ) {
	for ( const json * d : declsNamed( r, name ) ) {
		if ( (*d)["file"] == r.file && (*d)["nameRange"]["line"] == line ) return d;
	}
	return nullptr;
}

// The reference at the nth `word` on `line` resolves to the declaration of `word` whose name is on `declLine`
// (at the nth occurrence of the name there), with the given role.
void expectRef( const Run & r, int line, const std::string & word, int declLine, const char * role = nullptr,
		int nth = 0, int declNth = 0, const std::string & declName = "" ) {
	std::string name = declName.empty() ? word : declName;
	int col = r.col( line, word, nth );
	INFO( "ref " << word << " at " << line << ":" << col << ": " << r.line( line ) );
	REQUIRE( col >= 0 );
	const json * ref = refAt( r, line, col );
	REQUIRE( ref );
	CHECK( (*ref)["col"] == col );
	CHECK( (*ref)["endCol"] == col + (int)word.size() );
	if ( role ) CHECK( (*ref)["role"] == role );
	const json * decl = declById( r, (*ref)["decl"] );
	REQUIRE( decl );
	INFO( "decl " << decl->dump() );
	CHECK( (*decl)["name"] == name );
	CHECK( (*decl)["file"] == r.file );
	CHECK( (*decl)["nameRange"]["line"] == declLine );
	CHECK( (*decl)["nameRange"]["col"] == r.col( declLine, name, declNth ) );
}

const json * diagnosticOn( const Run & r, int line, const std::string & severity ) {
	for ( const json & d : r.dump["diagnostics"] ) {
		if ( d["line"] == line && d["severity"] == severity && d["file"] == r.file ) return &d;
	}
	return nullptr;
}

// Invariants of every dump: unique ids, refs to present non-generated decls, refs in the focus file.
void checkInvariants( const Run & r ) {
	const json & decls = r.dump["decls"];
	for ( size_t i = 0; i < decls.size(); i += 1 ) {
		REQUIRE( decls[i]["id"] == (int)i );
	}
	for ( const json & ref : r.dump["refs"] ) {
		int id = ref["decl"];
		REQUIRE( id >= 0 );
		REQUIRE( id < (int)decls.size() );
		CHECK_FALSE( decls[id]["generated"].get<bool>() );
		CHECK( ref["file"] == r.file );
		std::string role = ref["role"];
		CHECK( std::set<std::string>{ "read", "call", "member", "type", "with" }.count( role ) );
	}
	for ( const json & d : decls ) {
		for ( const char * link : { "parent", "typeDecl" } ) {
			if ( ! d[link].is_null() ) CHECK( d[link].get<int>() < (int)decls.size() );
		}
	}
	for ( const json & s : r.dump["scopes"] ) {
		for ( const json & id : s["decls"] ) CHECK( id.get<int>() < (int)decls.size() );
		if ( ! s["parent"].is_null() ) CHECK( s["parent"].get<int>() < (int)r.dump["scopes"].size() );
	}
}

} // namespace

TEST_CASE( "translator: overloads, including on return type and variables" ) {
	const Run * r = run( "overload.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	CHECK( r->dump["diagnostics"].empty() );
	expectRef( *r, 10, "f", 2, "call" );				// int f( int )
	expectRef( *r, 11, "f", 3, "call" );				// double f( double )
	expectRef( *r, 12, "g", 4, "call" );				// int g(), chosen by the return type
	expectRef( *r, 13, "g", 5, "call" );				// double g()
	expectRef( *r, 14, "v", 7, "read" );				// double v
	expectRef( *r, 15, "v", 6, "read" );				// int v
	expectRef( *r, 16, "a", 10 );
	const json * f = declAt( *r, "f", 3 );
	REQUIRE( f );
	CHECK( (*f)["kind"] == "function" );
	CHECK( (*f)["params"] == json::array( { "x" } ) );
	CHECK( (*f)["signature"].get<std::string>().find( "double f(" ) != std::string::npos );
	CHECK_FALSE( (*f)["body"].is_null() );
	// An explicit cast is an expression covering the whole cast.
	bool cast = false;
	for ( const json & e : r->dump["exprs"] ) {
		if ( e["line"] == 16 && e["col"] == r->col( 16, "(int)" ) ) {
			cast = true;
			CHECK( e["type"] == "int" );
			CHECK( e["endCol"] == (int)r->line( 16 ).size() - 1 );
		}
	}
	CHECK( cast );
}

TEST_CASE( "translator: forall functions and traits" ) {
	const Run * r = run( "forall.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	expectRef( *r, 11, "twice", 6, "call" );
	expectRef( *r, 12, "twice", 6, "call" );
	expectRef( *r, 13, "self", 8, "call" );
	expectRef( *r, 6, "addable", 2, "type" );			// trait use in the assertion
	expectRef( *r, 6, "T", 6, "type", 2 );				// return type T is the forall's T
	const json * trait = declAt( *r, "addable", 2 );
	REQUIRE( trait );
	CHECK( (*trait)["kind"] == "trait" );
	const json * t = declAt( *r, "T", 6 );
	REQUIRE( t );
	CHECK( (*t)["kind"] == "typeParam" );
	CHECK( (*t)["local"] == true );
}

TEST_CASE( "translator: members, references and with clauses" ) {
	const Run * r = run( "members.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	expectRef( *r, 5, "x", 2, "with" );				// field found through with( p )
	expectRef( *r, 5, "dx", 4, "read" );
	expectRef( *r, 6, "y", 2, "with" );
	expectRef( *r, 4, "p", 4, "read", 1 );			// the with clause names the parameter
	expectRef( *r, 4, "Point", 2, "type" );
	expectRef( *r, 9, "x", 2, "member" );				// pp->x
	expectRef( *r, 9, "pp", 9, "read", 1 );
	expectRef( *r, 14, "x", 2, "member" );				// r.x through a reference
	expectRef( *r, 14, "r", 13, "read" );
	expectRef( *r, 16, "y", 2, "member" );
	expectRef( *r, 17, "ri", 16, "read" );
	expectRef( *r, 15, "shift", 4, "call" );

	const json * x = declAt( *r, "x", 2 );
	REQUIRE( x );
	CHECK( (*x)["kind"] == "field" );
	const json * point = declAt( *r, "Point", 2 );
	REQUIRE( point );
	CHECK( (*x)["parent"] == (*point)["id"] );
	CHECK( (*point)["kind"] == "struct" );
	CHECK_FALSE( (*point)["body"].is_null() );

	// typeDecl strips the reference down to the struct.
	const json * rr = declAt( *r, "r", 13 );
	REQUIRE( rr );
	CHECK( (*rr)["typeDecl"] == (*point)["id"] );
	const json * pp = declAt( *r, "pp", 9 );
	REQUIRE( pp );
	CHECK( (*pp)["kind"] == "parameter" );
	CHECK( (*pp)["typeDecl"] == (*point)["id"] );

	// The body of shift is a scope holding its parameters.
	const json * p = declAt( *r, "p", 4 );
	REQUIRE( p );
	bool found = false;
	for ( const json & s : r->dump["scopes"] ) {
		if ( s["line"] == 4 && s["endLine"] == 7 ) {
			found = true;
			CHECK( s["decls"].size() == 2 );
			CHECK( s["decls"][0] == (*p)["id"] );
		}
	}
	CHECK( found );

	// Hover data: the expression r.x has type int.
	bool expr = false;
	for ( const json & e : r->dump["exprs"] ) {
		if ( e["line"] == 14 && e["col"] == r->col( 14, "x" ) ) {
			expr = true;
			CHECK( e["type"].get<std::string>().find( "int" ) != std::string::npos );
		}
	}
	CHECK( expr );
}

TEST_CASE( "translator: generic types and typedefs" ) {
	const Run * r = run( "generic.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	expectRef( *r, 9, "Box", 2, "type" );
	expectRef( *r, 10, "IntBox", 6, "type" );
	expectRef( *r, 11, "get", 4, "call" );
	expectRef( *r, 12, "item", 2, "member" );
	expectRef( *r, 4, "item", 2, "member" );
	const json * b = declAt( *r, "b", 9 );
	const json * box = declAt( *r, "Box", 2 );
	REQUIRE( b );
	REQUIRE( box );
	CHECK( (*b)["typeDecl"] == (*box)["id"] );
	const json * intbox = declAt( *r, "IntBox", 6 );
	REQUIRE( intbox );
	CHECK( (*intbox)["kind"] == "typedef" );
}

TEST_CASE( "translator: coroutines, monitors and threads" ) {
	const Run * r = run( "concurrency.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	CHECK( r->dump["diagnostics"].empty() );
	const json * gen = declAt( *r, "Gen", 6 );
	const json * account = declAt( *r, "Account", 14 );
	const json * worker = declAt( *r, "Worker", 18 );
	REQUIRE( gen );
	REQUIRE( account );
	REQUIRE( worker );
	CHECK( (*gen)["kind"] == "coroutine" );
	CHECK( (*account)["kind"] == "monitor" );
	CHECK( (*worker)["kind"] == "thread" );
	expectRef( *r, 8, "Gen", 6, "type" );
	expectRef( *r, 12, "value", 6, "member" );
	expectRef( *r, 16, "balance", 14, "member" );
	expectRef( *r, 24, "next", 12, "call" );
	expectRef( *r, 24, "gen", 23, "read" );
	expectRef( *r, 26, "deposit", 16, "call" );
	expectRef( *r, 26, "acc", 25, "read" );
	expectRef( *r, 27, "Worker", 18, "type" );
	// Desugaring adds declarations (e.g. the coroutine's main wrappers, monitor guards) at user locations;
	// none of them may be referenced or appear as user declarations.
	for ( const json & d : r->dump["decls"] ) {
		if ( d["file"] != r->file || d["generated"].get<bool>() ) continue;
		std::string name = d["name"];
		CHECK_MESSAGE( name.rfind( "__", 0 ) != 0, "generated name dumped as a user declaration: " << d.dump() );
	}
	// Only the two user mains are functions named main besides int main.
	int mains = 0;
	for ( const json * d : declsNamed( *r, "main" ) ) {
		if ( (*d)["file"] == r->file && ! (*d)["generated"].get<bool>() ) mains += 1;
	}
	CHECK( mains == 3 );
}

TEST_CASE( "translator: exceptions" ) {
	const Run * r = run( "exceptions.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	CHECK( r->dump["diagnostics"].empty() );
	const json * oops = declAt( *r, "Oops", 2 );
	REQUIRE( oops );
	CHECK( (*oops)["kind"] == "exception" );
	expectRef( *r, 16, "risky", 5, "call" );
	expectRef( *r, 18, "code", 14, "read" );
	expectRef( *r, 18, "code", 2, "member", 1 );		// e->code
	expectRef( *r, 18, "e", 17, "read" );
	expectRef( *r, 21, "gentle", 9, "call" );
	expectRef( *r, 3, "Oops", 2, "type" );
	// The handler's declaration is in the handler's block.
	const json * e = declAt( *r, "e", 17 );
	REQUIRE( e );
	bool inHandler = false;
	for ( const json & s : r->dump["scopes"] ) {
		for ( const json & id : s["decls"] ) {
			if ( id == (*e)["id"] ) inHandler = s["line"] == 17 && s["endLine"] == 19;
		}
	}
	CHECK( inHandler );
}

TEST_CASE( "translator: macros and irregular spacing (lines only)" ) {
	const Run * r = run( "macros.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	bool call = false, square = false, positive = false;
	for ( const json & ref : r->dump["refs"] ) {
		const json * d = declById( *r, ref["decl"] );
		if ( ref["line"] == 11 && (*d)["name"] == "square_it" ) {
			call = true;
			CHECK( (*d)["nameRange"]["line"] == 5 );
		}
		if ( ref["line"] == 12 && (*d)["name"] == "spaced" ) square = true;
		if ( ref["line"] == 13 && (*d)["name"] == "positive" ) {
			positive = true;
			CHECK( (*d)["nameRange"]["line"] == 8 );
		}
	}
	CHECK( call );
	CHECK( square );
	CHECK( positive );
	CHECK( declAt( *r, "spaced", 11 ) );
	CHECK( declAt( *r, "tabbed", 12 ) );
	// cpp splits line 8 around the expansion of bool; the name is in the second piece.
	const json * p = declAt( *r, "positive", 8 );
	REQUIRE( p );
	CHECK_FALSE( (*p)["generated"].get<bool>() );
}

TEST_CASE( "translator: syntax error" ) {
	const Run * r = run( "parse_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == false );
	const json * d = diagnosticOn( *r, 5, "error" );
	REQUIRE( d );
	CHECK( (*d)["col"] == r->col( 5, ";" ) );
	CHECK( (*d)["endCol"] == r->col( 5, ";" ) + 1 );
	CHECK( (*d)["message"].get<std::string>().find( "syntax error" ) != std::string::npos );
}

TEST_CASE( "translator: a type error keeps the rest of the dump" ) {
	const Run * r = run( "type_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == false );
	const json * d = diagnosticOn( *r, 9, "error" );
	REQUIRE( d );
	CHECK( (*d)["message"].get<std::string>().find( "No alternatives" ) != std::string::npos );
	CHECK( (*d)["col"] == r->col( 9, "nope" ) );
	// The statements around the error are still resolved.
	expectRef( *r, 8, "good", 4, "call" );
	expectRef( *r, 12, "good", 4, "call" );
	expectRef( *r, 12, "bad", 6, "call" );
	expectRef( *r, 7, "S", 2, "type" );
}

TEST_CASE( "translator: errors from checking passes and warnings" ) {
	const Run * r = run( "check_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	const json * d = diagnosticOn( *r, 3, "error" );
	REQUIRE( d );
	CHECK( (*d)["message"].get<std::string>().find( "returns no values" ) != std::string::npos );
	CHECK( (*d)["col"] == 0 );
	// The check does not stop translation, so the file is still resolved.
	expectRef( *r, 9, "noreturn_value", 2, "call" );
	const json * w = diagnosticOn( *r, 8, "warning" );
	REQUIRE( w );
	CHECK( (*w)["message"].get<std::string>().find( "self assignment" ) != std::string::npos );
	CHECK( (*w)["col"] == 0 );
	CHECK( (*w)["endCol"] == 5 );					// x = x
	CHECK( r->dump["complete"] == true );			// the check does not make the AST incomplete
}

TEST_CASE( "translator: an error before Resolve still gives a dump" ) {
	const Run * r = run( "prepass_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == false );
	const json * d = diagnosticOn( *r, 3, "error" );
	REQUIRE( d );
	CHECK( (*d)["message"].get<std::string>().find( "undeclared trait nosuch" ) != std::string::npos );
	CHECK( (*d)["col"] == r->col( 3, "nosuch" ) );
	CHECK( (*d)["endCol"] == r->col( 3, "nosuch" ) + 6 );
	// Declarations come from the partly validated AST.
	CHECK( declAt( *r, "S", 2 ) );
	CHECK( declAt( *r, "g", 4 ) );
}

TEST_CASE( "translator: --lsp-c-out also generates C" ) {
	const Run * r = run( "overload.cfa", true );
	if ( ! r ) return;
	CHECK( r->dump["complete"] == true );
	CHECK( r->c.find( "main" ) != std::string::npos );
	// The dump is the same with or without code generation.
	const Run * plain = run( "overload.cfa" );
	REQUIRE( plain );
	CHECK( plain->dump["refs"] == r->dump["refs"] );
}

TEST_CASE( "translator: readable types, system typedefs and failed declarations" ) {
	const Run * r = run( "readable.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == false );
	// Only the failing initializer is an error; the later use of `bad` is not.
	REQUIRE( r->dump["diagnostics"].size() == 1 );
	CHECK( r->dump["diagnostics"][0]["line"] == 10 );
	expectRef( *r, 11, "bad", 10, "read" );

	const json * pick = declAt( *r, "pick", 3 );
	REQUIRE( pick );
	CHECK( (*pick)["signature"] == "forall( T ) T pick( T a, T b )" );
	CHECK( (*pick)["type"] == "T (T, T)" );
	const json * same = declAt( *r, "same", 4 );
	REQUIRE( same );
	CHECK( (*same)["signature"] == "forall( U & ) U & same( U &u )" );
	const json * t = declAt( *r, "T", 3 );
	REQUIRE( t );
	CHECK( (*t)["signature"] == "T" );
	const json * big = declAt( *r, "big", 9 );
	REQUIRE( big );
	CHECK( (*big)["type"] == "long" );
	const json * one = declAt( *r, "ONE", 5 );
	REQUIRE( one );
	CHECK( (*one)["type"].get<std::string>().find( "(anonymous)" ) != std::string::npos );

	// Typedefs from system headers are dumped, and their uses are refs.
	for ( auto [line, name] : { std::pair{ 7, "FILE" }, std::pair{ 8, "size_t" } } ) {
		const json * ref = refAt( *r, line, 0 );
		REQUIRE( ref );
		CHECK( (*ref)["role"] == "type" );
		const json * decl = declById( *r, (*ref)["decl"] );
		REQUIRE( decl );
		CHECK( (*decl)["name"] == name );
		CHECK( (*decl)["kind"] == "typedef" );
		CHECK( (*decl)["local"] == false );
	}
}

namespace {

const json * scopeAt( const Run & r, int line, int col ) {
	for ( const json & s : r.dump["scopes"] ) {
		if ( s["line"] == line && s["col"] == col ) return &s;
	}
	return nullptr;
}

bool scopeHas( const json & scope, const json & decl ) {
	for ( const json & id : scope["decls"] ) if ( id == decl["id"] ) return true;
	return false;
}

} // namespace

TEST_CASE( "translator: names moved into generated code by desugaring are refs" ) {
	const Run * r = run( "desugar.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	CHECK( r->dump["diagnostics"].empty() );
	expectRef( *r, 2, "b", 2, "read", 1 );				// return [ b, a ];
	expectRef( *r, 2, "a", 2, "read", 1 );
	expectRef( *r, 13, "u", 12, "read" );				// tuple assignment
	expectRef( *r, 13, "swap", 2, "call" );
	expectRef( *r, 13, "v", 12, "read", 1 );
	expectRef( *r, 14, "u", 12, "read" );				// compound literals
	expectRef( *r, 15, "u", 12, "read" );
	expectRef( *r, 15, "use", 5, "call" );
	expectRef( *r, 8, "Boom_vt", 7, "read" );			// throw (Boom){ &Boom_vt, c }
	expectRef( *r, 8, "c", 8, "read", 1 );
	expectRef( *r, 16, "make", 4, "call" );			// with on an rvalue
	expectRef( *r, 16, "u", 12, "read" );
	expectRef( *r, 17, "r", 15, "read" );				// sizeof( r )
	expectRef( *r, 22, "x", 3, "member" );				// a. on the line before

	// A call expression covers its arguments.
	int swap = r->col( 13, "swap" );
	bool call = false;
	for ( const json & e : r->dump["exprs"] ) {
		if ( e["line"] == 13 && e["col"] == swap ) {
			call = true;
			CHECK( e["endCol"] == swap + (int)std::string( "swap( u, v )" ).size() );
		}
	}
	CHECK( call );
	// Desugaring makes blocks at user locations; only the user's are scopes.
	for ( const json & s : r->dump["scopes"] ) {
		CHECK_MESSAGE( ( s["line"] != 13 && s["line"] != 14 && s["line"] != 15 ), s.dump() );
		CHECK_MESSAGE( ! ( s["line"] == 2 && s["col"] == r->col( 2, "swap" ) ), s.dump() );
	}

	// Local typedefs are in their block's scope.
	const json * real = declAt( *r, "Real", 18 );
	const json * small = declAt( *r, "Small", 20 );
	REQUIRE( real );
	REQUIRE( small );
	const json * body = scopeAt( *r, 11, r->col( 11, "{" ) );
	const json * block = scopeAt( *r, 20, 0 );
	REQUIRE( body );
	REQUIRE( block );
	CHECK( scopeHas( *body, *real ) );
	CHECK( scopeHas( *block, *small ) );

	// A generator main's body is its braces, and its parameter is in that scope.
	const json * gmain = nullptr;
	for ( const json * d : declsNamed( *r, "main" ) ) {
		if ( (*d)["file"] == r->file && (*d)["nameRange"]["line"] == 10 ) gmain = d;
	}
	REQUIRE( gmain );
	int brace = r->col( 10, "{" );
	CHECK( (*gmain)["body"]["col"] == brace );
	const json * g = declAt( *r, "g", 10 );
	REQUIRE( g );
	const json * gscope = scopeAt( *r, 10, brace );
	REQUIRE( gscope );
	CHECK( scopeHas( *gscope, *g ) );

	// exception and vtable declarations keep their whole range.
	const json * boom = declAt( *r, "Boom", 6 );
	REQUIRE( boom );
	CHECK( (*boom)["col"] == 0 );
	CHECK( (*boom)["endCol"] == (int)r->line( 6 ).size() );
	CHECK_FALSE( (*boom)["body"].is_null() );
	const json * vt = declAt( *r, "Boom_vt", 7 );
	REQUIRE( vt );
	CHECK( (*vt)["col"] == 0 );
}

TEST_CASE( "translator: generated names are shown as written, operators and postfix calls are refs" ) {
	const Run * r = run( "names.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	for ( const json & d : r->dump["decls"] ) {
		if ( d["file"] != r->file || d["generated"].get<bool>() ) continue;
		for ( const char * k : { "type", "signature", "name" } ) {
			CHECK_MESSAGE( d[k].get<std::string>().find( "__" ) == std::string::npos, d.dump() );
		}
	}
	const json * anon = declAt( *r, "Anon", 1 );
	REQUIRE( anon );
	CHECK( (*anon)["signature"] == "typedef struct { ... } Anon" );
	CHECK_FALSE( (*anon)["typeDecl"].is_null() );			// the anonymous struct, for member completion
	const json * an = declAt( *r, "an", 10 );
	REQUIRE( an );
	CHECK( (*an)["type"] == "Anon" );
	const json * inner = declAt( *r, "Inner", 2 );
	REQUIRE( inner );
	CHECK_FALSE( (*inner)["generated"].get<bool>() );
	const json * in = declAt( *r, "in", 2 );
	REQUIRE( in );
	CHECK( (*in)["type"] == "Inner" );
	const json * anonField = declAt( *r, "anon", 2 );
	REQUIRE( anonField );
	CHECK( (*anonField)["type"] == "(anonymous)" );

	// Generic aggregates: the parameter keeps its name and kind, and its uses are refs.
	const json * box = declAt( *r, "Box", 3 );
	REQUIRE( box );
	CHECK( (*box)["signature"] == "forall( T ) struct Box" );
	const json * val = declAt( *r, "val", 3 );
	REQUIRE( val );
	CHECK( (*val)["type"] == "T" );
	const json * t = declAt( *r, "T", 3 );
	REQUIRE( t );
	CHECK( (*t)["kind"] == "typeParam" );
	CHECK( (*t)["local"] == true );
	expectRef( *r, 3, "T", 3, "type", 1 );
	const json * ref = declAt( *r, "Ref", 4 );
	REQUIRE( ref );
	CHECK( (*ref)["signature"] == "forall( T & ) struct Ref" );

	const json * len = declAt( *r, "?`len", 7 );
	REQUIRE( len );
	CHECK( (*len)["signature"] == "int ?`len( Vec v )" );
	const json * put = declAt( *r, "put", 8 );
	REQUIRE( put );
	CHECK( (*put)["signature"] == "void put( int a, int b = 4, const char *m = \"r\" )" );

	expectRef( *r, 15, "len", 7, "call", 0, 0, "?`len" );	// v`len
	expectRef( *r, 14, "+", 6, "call", 0, 0, "?+?" );		// v + w
}

TEST_CASE( "translator: an error in the range of a for loop is reported once" ) {
	const Run * r = run( "dup_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	REQUIRE( r->dump["diagnostics"].size() == 1 );
	CHECK( r->dump["diagnostics"][0]["line"] == 3 );
}

TEST_CASE( "translator: an error after Resolve leaves the dump complete" ) {
	const Run * r = run( "post_error.cfa" );
	if ( ! r ) return;
	checkInvariants( *r );
	CHECK( r->dump["complete"] == true );
	const json * d = diagnosticOn( *r, 4, "error" );
	REQUIRE( d );
	CHECK( (*d)["message"].get<std::string>().find( "used before being constructed" ) != std::string::npos );
	expectRef( *r, 5, "w", 3, "member" );
}

TEST_CASE( "translator: bytes that are not UTF-8 in messages don't break the dump" ) {
	const Run * nbsp = run( "nbsp.cfa" );					// run() requires exit status 0 and parses the JSON
	if ( ! nbsp ) return;
	const json * d = diagnosticOn( *nbsp, 3, "error" );
	REQUIRE( d );
	CHECK( (*d)["message"].get<std::string>().find( "\\xc2" ) != std::string::npos );
	const Run * latin1 = run( "latin1.cfa" );
	REQUIRE( latin1 );
	CHECK( diagnosticOn( *latin1, 4, "error" ) );
}
