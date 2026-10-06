#include "Analysis.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "Lexer.hpp"
#include "TextScan.hpp"

namespace cfalsp {

using nlohmann::json;
using text::FileText;

namespace {

enum class Kind {
	Function, Variable, Parameter, Field, Struct, Union, Enum, Enumerator, Trait, Typedef,
	TypeParam, Coroutine, Monitor, Thread, Generator, Exception, Label, Other,
};

Kind parseKind( const std::string & s ) {
	static const std::unordered_map<std::string, Kind> kinds = {
		{ "function", Kind::Function }, { "variable", Kind::Variable }, { "parameter", Kind::Parameter },
		{ "field", Kind::Field }, { "struct", Kind::Struct }, { "union", Kind::Union }, { "enum", Kind::Enum },
		{ "enumerator", Kind::Enumerator }, { "trait", Kind::Trait }, { "typedef", Kind::Typedef },
		{ "typeParam", Kind::TypeParam }, { "coroutine", Kind::Coroutine }, { "monitor", Kind::Monitor },
		{ "thread", Kind::Thread }, { "generator", Kind::Generator }, { "exception", Kind::Exception },
		{ "label", Kind::Label },
	};
	auto it = kinds.find( s );
	return it == kinds.end() ? Kind::Other : it->second;
}

bool isAggregate( Kind k ) {
	switch ( k ) {
	  case Kind::Struct: case Kind::Union: case Kind::Enum: case Kind::Trait: case Kind::Coroutine:
	  case Kind::Monitor: case Kind::Thread: case Kind::Generator: case Kind::Exception:
		return true;
	  default:
		return false;
	}
}

// Where a file comes from. The order is the ranking used for completion.
enum class Origin { Focus, Project, Libcfa, Prelude, System };

Origin classify( const std::string & path ) {
	// The prelude's line markers use bare names ("prelude.cfa") or the build
	// machine's path; "<built-in>" and friends come from cpp.
	if ( path.empty() || path[0] != '/' ) return Origin::Prelude;
	if ( path.find( "/libcfa/prelude/" ) != std::string::npos || path.find( "/lib/cfa/" ) != std::string::npos ) return Origin::Prelude;
	if ( path.find( "/include/cfa/" ) != std::string::npos || path.find( "/libcfa/src/" ) != std::string::npos ) return Origin::Libcfa;
	for ( const char * sys : { "/usr/include/", "/usr/lib/", "/usr/lib64/", "/usr/local/include/" } ) {
		if ( path.starts_with( sys ) ) return Origin::System;
	}
	return Origin::Project;
}

bool isLibrary( Origin o ) { return o >= Origin::Libcfa; }

enum class Role { Read, Call, Member, Type, With };

struct Decl {
	long long id = 0;
	std::string name, kindName;
	Kind kind = Kind::Other;
	int file = -1;							// -1 when the dump gives no file
	bool hasLoc = false, hasName = false;
	Range range, nameRange;
	std::string type, signature;
	int parent = -1, typeDecl = -1;			// decl indices
	std::vector<std::string> params;
	std::optional<Range> body;
	bool generated = false, local = false;
};

struct Ref { int file; Range range; int decl; Role role; };
struct Expr { int file; Range range; std::string type; int typeDecl; };
struct Scope { int file = -1; Range range; int parent = -1; std::vector<int> decls; };

struct File {
	std::string path;
	Origin origin = Origin::Project;
	bool focus = false;
	std::optional<std::vector<int>> present;	// lines that left text in the preprocessed output
	std::vector<int> refs;					// sorted by range start
	std::vector<int> exprs;					// sorted by range start
	std::vector<int> exprEnds;				// sorted by range end
	std::vector<int> names;					// non-generated decls, sorted by name start
	std::vector<int> scopes;
	std::vector<int> bodies;				// functions with a body
};

// Global names offered by completion.
struct NameEntry {
	std::string lower, name;
	std::vector<int> decls;					// best first
	int overloads = 0;						// distinct entities
};

// -- JSON access ---------------------------------------------------------------

long long integer( const json & j, const char * key, long long def ) {
	auto it = j.find( key );
	if ( it == j.end() ) return def;
	if ( it->is_number_integer() ) return it->get<long long>();
	if ( it->is_number_float() ) return (long long)it->get<double>();
	return def;
}

int smallInt( const json & j, const char * key, int def ) {
	return int( std::clamp<long long>( integer( j, key, def ), INT_MIN / 2, INT_MAX / 2 ) );
}

std::string string( const json & j, const char * key ) {
	auto it = j.find( key );
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool boolean( const json & j, const char * key ) {
	auto it = j.find( key );
	return it != j.end() && it->is_boolean() && it->get<bool>();
}

constexpr long long noId = LLONG_MIN;

long long optId( const json & j, const char * key ) {
	auto it = j.find( key );
	return it != j.end() && it->is_number_integer() ? it->get<long long>() : noId;
}

const json & array( const json & dump, const char * key ) {
	static const json empty = json::array();
	auto it = dump.find( key );
	if ( it == dump.end() || it->is_null() ) return empty;
	if ( !it->is_array() ) throw std::runtime_error( std::string( "dump: \"" ) + key + "\" is not an array" );
	return *it;
}

// `body`, if given, is set when the start came from a macro's body, so the
// source does not spell what is there (see SourceMap::inMacroBody).
std::optional<Range> mapped( const json & j, const std::string & file, const SourceMap & map, bool * body = nullptr ) {
	if ( body ) *body = false;
	if ( !j.is_object() ) return std::nullopt;
	int line = smallInt( j, "line", 0 );
	if ( line <= 0 ) return std::nullopt;				// unknown location
	int col = std::max( smallInt( j, "col", 0 ), 0 );
	int endLine = smallInt( j, "endLine", line );
	int endCol = smallInt( j, "endCol", endLine == line ? col : 0 );
	// The line in the preprocessed text, when the translator gave it.
	int pline = std::max( smallInt( j, "pline", 0 ), 0 );
	int endPline = std::max( smallInt( j, "endPline", 0 ), 0 );
	// A range can end on a later piece of a split line, at a smaller column.
	bool laterPiece = pline > 0 && endPline > 0 && endPline != pline;
	if ( endLine < line || ( endLine == line && ( laterPiece ? endPline < pline : endCol < col ) ) ) {
		endLine = line;
		endCol = col;
		endPline = pline;
	}
	SourceMap::Point start{ line, col, pline }, end{ endLine, endCol, endPline };
	Range r = map.mapRange( file, start, end );
	if ( r.start.line < 0 ) r.start = { 0, 0 };
	if ( r.end < r.start ) r.end = r.start;
	if ( body ) *body = map.inMacroBody( file, start );
	return r;
}

// -- small helpers -------------------------------------------------------------

bool contains( const Range & r, Loc p ) { return r.start <= p && p < r.end; }

std::string lower( std::string_view s ) {
	std::string out( s );
	for ( char & c : out ) c = char( std::tolower( (unsigned char)c ) );
	return out;
}

bool startsWithNoCase( std::string_view s, std::string_view prefix ) {
	if ( prefix.size() > s.size() ) return false;
	for ( size_t i = 0; i < prefix.size(); i += 1 ) {
		if ( std::tolower( (unsigned char)s[i] ) != std::tolower( (unsigned char)prefix[i] ) ) return false;
	}
	return true;
}

std::string stripSpaces( std::string_view s ) {
	std::string out;
	for ( char c : s ) if ( !std::isspace( (unsigned char)c ) ) out += c;
	return out;
}

std::string basename( const std::string & p ) {
	size_t k = p.rfind( '/' );
	return k == std::string::npos ? p : p.substr( k + 1 );
}

// Is a variable of this type read-only? "const int", "int const", "const T &",
// "char * const" are; "const char *" is not.
bool isConstType( std::string_view t ) {
	while ( !t.empty() && std::isspace( (unsigned char)t.back() ) ) t.remove_suffix( 1 );
	if ( t.ends_with( "const" ) && ( t.size() == 5 || !text::isIdentChar( t[t.size() - 6] ) ) ) return true;
	if ( t.find_first_of( "*(" ) != std::string_view::npos ) return false;
	for ( size_t p = t.find( "const" ); p != std::string_view::npos; p = t.find( "const", p + 1 ) ) {
		bool before = p == 0 || !text::isIdentChar( t[p - 1] );
		bool after = p + 5 >= t.size() || !text::isIdentChar( t[p + 5] );
		if ( before && after ) return true;
	}
	return false;
}

// The entry of `sorted` (ordered by range start) whose range contains `pos`,
// else one that ends exactly at `pos`; the innermost wins. Returns the entry
// and whether it contains `pos`.
template<typename RangeOf>
std::pair<int, bool> findAt( const std::vector<int> & sorted, Loc pos, RangeOf rangeOf ) {
	auto it = std::upper_bound( sorted.begin(), sorted.end(), pos,
								[&]( Loc p, int i ) { return p < rangeOf( i ).start; } );
	int inside = -1, touching = -1;
	for ( int n = 0; it != sorted.begin() && n < 64; n += 1 ) {
		--it;
		const Range & r = rangeOf( *it );
		if ( contains( r, pos ) ) {
			if ( inside == -1 ) inside = *it;
			else {
				const Range & b = rangeOf( inside );
				if ( r.start == b.start && r.end < b.end ) inside = *it;
			}
		} else if ( r.end == pos && touching == -1 ) {
			touching = *it;
		}
	}
	return inside != -1 ? std::pair( inside, true ) : std::pair( touching, false );
}

const std::vector<std::string> & keywords() {
	static const std::vector<std::string> words = {
		// Cforall
		"forall", "trait", "with", "coroutine", "monitor", "thread", "generator", "exception", "suspend",
		"resume", "choose", "fallthrough", "fallthru", "catchResume", "throwResume", "fixup", "waitfor",
		"waituntil", "when", "timeout", "mutex", "nomutex", "disable", "enable", "corun", "cofor", "vtable",
		"virtual", "otype", "dtype", "ftype", "ttype", "sized", "zero_t", "one_t", "basetypeof", "finally",
		"throw", "try", "catch", "recover",
		// C
		"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
		"extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
		"short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
		"volatile", "while", "bool", "true", "false", "typeof", "alignof", "_Alignas", "_Alignof", "_Atomic",
		"_Bool", "_Complex", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local", "__attribute__",
		"asm",
	};
	return words;
}

// LSP CompletionItemKind
int completionKind( const Decl & d ) {
	switch ( d.kind ) {
	  case Kind::Function: return d.name == "?{}" ? 4 : 3;
	  case Kind::Variable: case Kind::Parameter: return 6;
	  case Kind::Field: return 5;
	  case Kind::Struct: case Kind::Union: return 22;
	  case Kind::Enum: return 13;
	  case Kind::Enumerator: return 20;
	  case Kind::Trait: return 8;
	  case Kind::TypeParam: return 25;
	  case Kind::Typedef: case Kind::Coroutine: case Kind::Monitor: case Kind::Thread:
	  case Kind::Generator: case Kind::Exception: return 7;
	  default: return 1;
	}
}

// LSP SymbolKind
int symbolKind( const Decl & d ) {
	switch ( d.kind ) {
	  case Kind::Function:
		if ( d.name == "?{}" ) return 9;
		return d.name.find( '?' ) != std::string::npos ? 25 : 12;
	  case Kind::Variable: return isConstType( d.type ) ? 14 : 13;
	  case Kind::Parameter: return 13;
	  case Kind::Field: return 8;
	  case Kind::Struct: case Kind::Union: return 23;
	  case Kind::Enum: return 10;
	  case Kind::Enumerator: return 22;
	  case Kind::Trait: return 11;
	  case Kind::TypeParam: return 26;
	  case Kind::Typedef: case Kind::Coroutine: case Kind::Monitor: case Kind::Thread:
	  case Kind::Generator: case Kind::Exception: return 5;
	  default: return 13;
	}
}

// Indices into tokenTypes().
enum TokenType { TType, TClass, TEnum, TInterface, TStruct, TTypeParameter, TParameter, TVariable,
	TProperty, TEnumMember, TFunction, TLabel, TKeyword };
enum TokenModifier { MDeclaration = 1, MReadonly = 2, MDefaultLibrary = 4 };

int tokenType( Kind k ) {
	switch ( k ) {
	  case Kind::Function: return TFunction;
	  case Kind::Variable: return TVariable;
	  case Kind::Parameter: return TParameter;
	  case Kind::Field: return TProperty;
	  case Kind::Struct: case Kind::Union: return TStruct;
	  case Kind::Enum: return TEnum;
	  case Kind::Enumerator: return TEnumMember;
	  case Kind::Trait: return TInterface;
	  case Kind::Typedef: return TType;
	  case Kind::TypeParam: return TTypeParameter;
	  case Kind::Coroutine: case Kind::Monitor: case Kind::Thread: case Kind::Generator:
	  case Kind::Exception: return TClass;
	  case Kind::Label: return TLabel;
	  default: return -1;
	}
}

} // namespace

struct Analysis::Impl {
	bool complete = false;
	std::vector<Diagnostic> diags;
	std::vector<Decl> decls;
	std::vector<Ref> refs;
	std::vector<Expr> exprs;
	std::vector<Scope> scopes;
	std::vector<File> files;
	std::unordered_map<std::string, int> fileIndex;
	std::unordered_map<long long, int> byId;
	std::unordered_map<std::string, std::vector<int>> byName;
	std::vector<std::vector<int>> children, refsOf;
	// Declarations of one entity (a prototype and its definition, a forward
	// declaration and the full struct) share a canonical index.
	std::vector<int> entity;
	std::vector<std::vector<int>> entityMembers;
	std::vector<NameEntry> globals;			// sorted by `lower`
	SourceMap::Reader read;

	mutable std::mutex textLock;
	mutable std::unordered_map<int, std::shared_ptr<const FileText>> texts;

	void load( const json & dump, const SourceMap & map );

	int fileOf( const std::string & path ) {
		auto [it, fresh] = fileIndex.emplace( path, int( files.size() ) );
		if ( fresh ) {
			files.emplace_back();
			files.back().path = path;
		}
		return it->second;
	}
	int findFile( const std::string & path ) const {
		auto it = fileIndex.find( path );
		return it == fileIndex.end() ? -1 : it->second;
	}
	std::shared_ptr<const FileText> text( int file ) const;
	// Comments and string or character literals in a file, sorted, read on first use.
	mutable std::unordered_map<int, std::shared_ptr<const std::vector<Range>>> quiet;
	bool inCommentOrLiteral( int fi, Loc pos ) const;

	Origin origin( int d ) const { return decls[d].file >= 0 ? files[decls[d].file].origin : Origin::Prelude; }
	bool ranksBefore( int a, int b ) const;
	bool isGlobalName( int d ) const;
	Location location( int d ) const { return { files[decls[d].file].path, decls[d].nameRange }; }

	std::string docOf( int d ) const;
	std::string docFor( int d ) const;
	std::string where( int d, const std::string & fromFile ) const;
	std::string whereIn( int file, int line, const std::string & fromFile ) const;
	// #define lines of the files in the dump, read on first use. The dump has
	// no macros: cpp expanded them before the translator ran.
	struct Macro {
		int file;
		Range name;
		std::string text;
		int undefLine = -1;				// the line of the #undef that ends it in its file
		bool function = false;			// function-like: #define F( x ) ...
	};
	mutable std::once_flag macrosOnce;
	mutable std::unordered_map<std::string, std::vector<Macro>> macros;
	void loadMacros() const;
	const Macro * macroNamed( const std::string & name, int fi, Loc pos ) const;
	const Macro * macroAt( int fi, Loc pos ) const;
	int typeParamAt( int fi, Loc pos, const std::string & name ) const;
	int typeParamUnder( int fi, Loc pos, Range & range ) const;
	bool internalOverload( int d ) const;
	std::optional<SignatureHelp> macroSignature( const std::string & name, int fi, Loc pos, int argIndex ) const;
	std::string describeMacro( const Macro & m, const std::string & fromFile ) const;
	// The file named by an #include directive on the line at `pos`.
	std::optional<int> includeAt( int fi, Loc pos ) const;
	std::string kindLabel( int d ) const;
	int otherOverloads( int d ) const;
	std::string describe( int d, const std::string & fromFile, bool viaWith ) const;
	int definitionOf( int d ) const;
	std::vector<Location> entityReferences( int decl, bool includeDeclaration ) const;
	int declAtLocation( const std::vector<Location> & locs ) const;

	struct Target { int decl = -1; Range range; Role role = Role::Read; };
	Target targetAt( int fi, Loc pos ) const;
	int innermostScope( int fi, Loc pos ) const;
	std::vector<int> enclosingFunctions( int fi, Loc pos ) const;
	std::vector<int> localsAt( int fi, Loc pos ) const;
	std::vector<int> withAggregates( int fi, Loc pos ) const;
	std::vector<int> fieldsOf( int agg ) const;
	int fieldNamed( int agg, const std::string & name ) const;
	std::vector<int> visibleNamed( int fi, Loc pos, const std::string & name, bool useWith ) const;
	int resolveChain( int fi, Loc pos, const std::vector<std::string> & chain, bool useWith ) const;
	int memberAggregate( int fi, Loc pos, std::string_view lineBefore, const text::CompletionContext & ctx,
						 std::string_view textBefore ) const;
	int aggregateNamed( const std::string & name ) const;
	std::optional<std::pair<int, int>> declaredInText( std::string_view text, const std::string & name ) const;
	bool namesType( const std::string & name ) const;
	struct TextDecl {
		std::string name, type;				// type as written, whitespace collapsed
		Range nameRange;					// the declared name, in the scanned text
		Range useRange;						// the name at the cursor, in the scanned text
	};
	std::optional<TextDecl> textDeclarationAt( const std::string & text, size_t offset ) const;
	std::vector<int> unresolvedAt( int fi, Loc pos, Range & range ) const;
	std::vector<int> functionsNamed( const std::string & name ) const;
	CompletionItem item( int d, char rank, const std::string & detailSuffix, bool withDoc ) const;
};

// -- diagnostics ---------------------------------------------------------------

namespace {

std::string trimmed( std::string_view s ) {
	size_t b = s.find_first_not_of( " \t" ), e = s.find_last_not_of( " \t" );
	return b == std::string_view::npos ? std::string() : std::string( s.substr( b, e - b + 1 ) );
}

int indentOf( std::string_view s ) {
	size_t i = s.find_first_not_of( ' ' );
	return i == std::string_view::npos ? int( s.size() ) : int( i );
}

// The identifier after "Name: " on a line of a resolver dump ("Name: third... from aggregate:").
std::string nameOn( std::string_view line ) {
	size_t k = line.find( "Name: " );
	if ( k == std::string_view::npos ) return {};
	k += 6;
	size_t e = k;
	while ( e < line.size() && !std::isspace( (unsigned char)line[e] ) && line.compare( e, 3, "..." ) != 0 ) e += 1;
	return std::string( line.substr( k, e - k ) );
}

// A resolver type as people write it: "array of char with dimension of
// Constant Expression (5: unsigned long int)" -> "char [5]".
std::string shortType( std::string t ) {
	auto replaceAll = [&]( const std::string & from, const std::string & to ) {
		for ( size_t k = t.find( from ); k != std::string::npos; k = t.find( from, k + to.size() ) ) t.replace( k, from.size(), to );
	};
	if ( t.starts_with( "array of " ) ) {
		size_t dim = t.find( " with dimension of Constant Expression (" );
		if ( dim != std::string::npos ) {
			size_t colon = t.find( ':', dim );
			std::string n = colon == std::string::npos ? "" : t.substr( dim + 40, colon - dim - 40 );
			t = t.substr( 9, dim - 9 ) + " [" + n + "]";
		}
	}
	while ( t.starts_with( "pointer to " ) ) t = t.substr( 11 ) + " *";
	while ( t.starts_with( "reference to " ) ) t = t.substr( 13 ) + " &";
	replaceAll( "instance of ", "" );
	replaceAll( " with body", "" );
	replaceAll( "signed int", "int" );
	if ( t.size() > 60 ) t = t.substr( 0, 57 ) + "...";
	return t;
}

// The argument types in the "...to:" part of a resolver dump.
std::vector<std::string> argumentTypes( const std::vector<std::string> & lines ) {
	std::vector<std::string> out;
	size_t i = 0;
	while ( i < lines.size() && trimmed( lines[i] ) != "...to:" ) i += 1;
	if ( i == lines.size() ) return out;
	std::string type;
	bool inArg = false;
	for ( i += 1; i < lines.size(); i += 1 ) {
		const std::string & l = lines[i];
		if ( indentOf( l ) != 2 ) continue;
		std::string t = trimmed( l );
		if ( t == "... with resolved type:" ) {
			if ( i + 1 < lines.size() ) type = trimmed( lines[i + 1] );
			continue;
		}
		if ( t.starts_with( "..." ) ) continue;
		if ( inArg ) out.push_back( shortType( type ) );
		inArg = true;
		type = "?";
	}
	if ( inArg ) out.push_back( shortType( type ) );
	return out;
}

} // namespace

// The translator's message is its first line, and resolver errors put a dump
// of the expression in the detail. The first line is what editors show inline,
// so name the problem there, and keep only the useful lines of the dump.
static std::string condenseDiagnostic( const std::string & message, const std::string & detail ) {
	std::vector<std::string> lines;
	for ( size_t b = 0; b <= detail.size(); ) {
		size_t e = detail.find( '\n', b );
		if ( e == std::string::npos ) e = detail.size();
		lines.push_back( detail.substr( b, e - b ) );
		b = e + 1;
	}
	std::string summary;
	const std::string undeclared = "No alternatives for expression Name: ";
	if ( message.starts_with( undeclared ) ) {
		summary = "use of undeclared identifier `" + trimmed( message.substr( undeclared.size() ) ) + "`";
	} else if ( message.find( "Untyped Member Expression, with field:" ) != std::string::npos ) {
		std::string field, aggregate;
		for ( size_t i = 0; i < lines.size(); i += 1 ) {
			if ( field.empty() ) field = nameOn( lines[i] );
			size_t from = lines[i].find( "from aggregate:" );
			if ( from == std::string::npos ) continue;
			if ( i + 1 < lines.size() && trimmed( lines[i + 1] ).starts_with( "Name: " ) ) aggregate = nameOn( lines[i + 1] );
			break;
		}
		if ( !field.empty() ) summary = "no field `" + field + "` in " + ( aggregate.empty() ? "this expression" : "`" + aggregate + "`" );
	} else if ( message.ends_with( "Applying untyped:" ) && !lines.empty() ) {
		std::string fn = nameOn( lines[0] );
		if ( !fn.empty() ) {
			std::string args;
			for ( const std::string & a : argumentTypes( lines ) ) args += ( args.empty() ? "" : ", " ) + a;
			if ( message.starts_with( "Invalid application" ) ) summary = "no overload of `" + fn + "` accepts the arguments (" + args + ")";
			else if ( message.starts_with( "Cannot choose" ) ) summary = "ambiguous call to `" + fn + "` with arguments (" + args + ")";
			else summary = "no matching call to `" + fn + "` with arguments (" + args + ")";
		}
	}

	// The lines worth keeping: candidates and unsatisfied assertions.
	static const char * keep[] = { "Alternatives are", "Could not satisfy", "Unsatisfiable alternative", "Cost (" };
	std::vector<std::string> kept;
	bool alternatives = false;
	for ( const std::string & l : lines ) {
		std::string t = trimmed( l );
		if ( t.empty() ) continue;
		bool want = alternatives && indentOf( l ) > 0;
		alternatives = alternatives && want;
		for ( const char * k : keep ) {
			if ( t.find( k ) != std::string::npos ) want = true;
		}
		if ( t.starts_with( "Alternatives are" ) ) alternatives = true;
		if ( summary.empty() && ( t.find( "Name: " ) != std::string::npos || t.find( "Variable Expression: " ) != std::string::npos ) ) want = true;
		if ( !want || std::find( kept.begin(), kept.end(), t ) != kept.end() ) continue;
		if ( kept.size() == 12 ) {
			kept.push_back( "..." );
			break;
		}
		kept.push_back( t );
	}
	std::string out = summary.empty() ? message : summary;
	if ( !kept.empty() ) {
		out += "\n";
		for ( const std::string & k : kept ) out += "\n" + k;
	}
	return out;
}

// -- loading -------------------------------------------------------------------

// A use inside a macro expansion maps to the whole invocation. Returns the
// part of `r` that spells `name`, or nullopt if the source there doesn't
// (the name came from the macro's body).
static std::optional<Range> spelledName( const FileText & ft, Range r, const std::string & name ) {
	if ( r.start.line != r.end.line ) return std::nullopt;
	std::string_view line = ft.line( r.start.line );
	int b = std::clamp( r.start.col, 0, int( line.size() ) ), e = std::clamp( r.end.col, b, int( line.size() ) );
	std::string_view span = line.substr( b, e - b );
	if ( span == name ) return r;
	size_t k = span.find( name );
	while ( k != std::string_view::npos ) {
		bool left = k == 0 || !text::isIdentChar( span[k - 1] ) || !text::isIdentChar( name.front() );
		bool right = k + name.size() == span.size() || !text::isIdentChar( span[k + name.size()] ) || !text::isIdentChar( name.back() );
		if ( left && right ) break;
		k = span.find( name, k + 1 );
	}
	if ( k == std::string_view::npos ) return std::nullopt;
	r.start.col = b + int( k );
	r.end.col = r.start.col + int( name.size() );
	return r;
}

// How a use of a declaration can be spelled besides its name: a postfix
// function ?`len is called as x`len, an operator ?+? is written +, and
// ?[?], ?() and ?{} are refs at their opening bracket (a[i], f( x ), p{ 1 }).
static std::string otherSpelling( const std::string & name ) {
	if ( name.starts_with( "?`" ) ) return name.substr( 2 );
	if ( name == "?[?]" ) return "[";
	if ( name == "?()" ) return "(";
	if ( name == "?{}" ) return "{";
	if ( name.size() < 2 || name.find( '?' ) == std::string::npos ) return {};
	std::string symbol;
	for ( char c : name ) if ( c != '?' ) symbol += c;
	if ( symbol.empty() || symbol.find_first_not_of( "+-*/%<>=!&|^~\\" ) != std::string::npos ) return {};
	return symbol;
}

static std::optional<Range> spelledUse( const FileText & ft, Range r, const std::string & name, bool identOnly = false ) {
	if ( auto s = spelledName( ft, r, name ) ) return s;
	std::string other = otherSpelling( name );
	if ( other.empty() || ( identOnly && !text::isIdentifier( other ) ) ) return std::nullopt;
	return spelledName( ft, r, other );
}


void Analysis::Impl::load( const json & dump, const SourceMap & map ) {
	if ( !dump.is_object() ) throw std::runtime_error( "dump is not a JSON object" );
	if ( integer( dump, "format", -1 ) != 1 ) throw std::runtime_error( "dump: missing or unsupported \"format\"" );
	complete = boolean( dump, "complete" );

	for ( const json & j : array( dump, "diagnostics" ) ) {
		if ( !j.is_object() ) continue;
		Diagnostic d;
		d.loc.file = string( j, "file" );
		if ( !d.loc.file.empty() ) {
			if ( auto r = mapped( j, d.loc.file, map ) ) d.loc.range = *r;
		}
		std::string sev = string( j, "severity" );
		d.severity = sev == "warning" ? 2 : sev == "note" ? 3 : 1;
		d.message = string( j, "message" );
		std::string detail = string( j, "detail" );
		d.message = condenseDiagnostic( d.message, detail );
		diags.push_back( std::move( d ) );
	}

	std::vector<std::pair<long long, long long>> links;	// parent, typeDecl ids
	for ( const json & j : array( dump, "decls" ) ) {
		if ( !j.is_object() ) continue;
		long long id = optId( j, "id" );
		if ( id == noId || byId.count( id ) ) continue;
		Decl d;
		d.id = id;
		d.name = string( j, "name" );
		d.kindName = string( j, "kind" );
		d.kind = parseKind( d.kindName );
		std::string path = string( j, "file" );
		if ( !path.empty() ) {
			d.file = fileOf( path );
			if ( auto r = mapped( j, path, map ) ) { d.hasLoc = true; d.range = *r; }
			auto nr = j.find( "nameRange" );
			bool madeUp = false;			// the name came from a macro's body
			if ( nr != j.end() ) {
				if ( auto r = mapped( *nr, path, map, &madeUp ) ; r && !madeUp ) { d.hasName = true; d.nameRange = *r; }
			}
			if ( d.hasName && !d.hasLoc ) { d.hasLoc = true; d.range = d.nameRange; }
			if ( d.hasLoc && !d.hasName ) d.nameRange = { d.range.start, d.range.start };
			if ( d.hasName ) {
				d.range.start = std::min( d.range.start, d.nameRange.start );
				d.range.end = std::max( d.range.end, d.nameRange.end );
			}
			auto body = j.find( "body" );
			if ( body != j.end() ) d.body = mapped( *body, path, map );
		}
		d.type = string( j, "type" );
		d.signature = string( j, "signature" );
		auto params = j.find( "params" );
		if ( params != j.end() && params->is_array() ) {
			for ( const json & p : *params ) d.params.push_back( p.is_string() ? p.get<std::string>() : std::string() );
		}
		d.generated = boolean( j, "generated" );
		d.local = boolean( j, "local" );
		links.emplace_back( optId( j, "parent" ), optId( j, "typeDecl" ) );
		byId.emplace( id, int( decls.size() ) );
		decls.push_back( std::move( d ) );
	}
	auto index = [&]( long long id ) {
		if ( id == noId ) return -1;
		auto it = byId.find( id );
		return it == byId.end() ? -1 : it->second;
	};
	const int n = int( decls.size() );
	children.assign( n, {} );
	refsOf.assign( n, {} );
	for ( int i = 0; i < n; i += 1 ) {
		decls[i].parent = index( links[i].first );
		decls[i].typeDecl = index( links[i].second );
		if ( decls[i].parent == i ) decls[i].parent = -1;
		if ( decls[i].parent >= 0 ) children[decls[i].parent].push_back( i );
		byName[decls[i].name].push_back( i );
	}

	for ( const json & j : array( dump, "refs" ) ) {
		if ( !j.is_object() ) continue;
		std::string path = string( j, "file" );
		int d = index( optId( j, "decl" ) );
		bool inBody = false;			// a use that only a macro's body spells
		auto r = mapped( j, path, map, &inBody );
		if ( path.empty() || d < 0 || !r || inBody ) continue;
		if ( auto ft = text( fileOf( path ) ) ) {
			r = spelledUse( *ft, *r, decls[d].name );
			if ( !r ) continue;
		}
		std::string role = string( j, "role" );
		Role ro = role == "call" ? Role::Call : role == "member" ? Role::Member : role == "type" ? Role::Type
			: role == "with" ? Role::With : Role::Read;
		int fi = fileOf( path );
		files[fi].refs.push_back( int( refs.size() ) );
		refsOf[d].push_back( int( refs.size() ) );
		refs.push_back( { fi, *r, d, ro } );
	}

	for ( const json & j : array( dump, "exprs" ) ) {
		if ( !j.is_object() ) continue;
		std::string path = string( j, "file" );
		auto r = mapped( j, path, map );
		if ( path.empty() || !r ) continue;
		int fi = fileOf( path );
		files[fi].exprs.push_back( int( exprs.size() ) );
		exprs.push_back( { fi, *r, string( j, "type" ), index( optId( j, "typeDecl" ) ) } );
	}

	const json & js = array( dump, "scopes" );
	scopes.resize( js.size() );
	for ( size_t i = 0; i < js.size(); i += 1 ) {
		const json & j = js[i];
		if ( !j.is_object() ) continue;
		std::string path = string( j, "file" );
		auto r = mapped( j, path, map );
		if ( path.empty() || !r ) continue;
		Scope & s = scopes[i];
		s.file = fileOf( path );
		s.range = *r;
		long long p = integer( j, "parent", -1 );
		s.parent = p >= 0 && p < (long long)js.size() && p != (long long)i ? int( p ) : -1;
		auto ds = j.find( "decls" );
		if ( ds != j.end() && ds->is_array() ) {
			for ( const json & id : *ds ) {
				int d = id.is_number_integer() ? index( id.get<long long>() ) : -1;
				if ( d >= 0 ) s.decls.push_back( d );
			}
		}
		files[s.file].scopes.push_back( int( i ) );
	}

	// Files with nothing in the dump, such as headers of macros only, are still
	// searched for macro definitions.
	for ( const std::string & f : map.files() ) {
		if ( !f.empty() && f[0] != '<' ) fileOf( f );
	}
	for ( auto & f : files ) f.present = map.outputLines( f.path );

	// Focus files are the ones with resolved uses or locals.
	for ( auto & f : files ) f.focus = !f.refs.empty() || !f.exprs.empty() || !f.scopes.empty();
	for ( const auto & d : decls ) if ( d.local && d.file >= 0 ) files[d.file].focus = true;
	for ( auto & f : files ) f.origin = f.focus ? Origin::Focus : classify( f.path );

	for ( int i = 0; i < n; i += 1 ) {
		const Decl & d = decls[i];
		if ( d.file < 0 ) continue;
		if ( d.hasName && !d.generated ) files[d.file].names.push_back( i );
		if ( d.kind == Kind::Function && d.body ) files[d.file].bodies.push_back( i );
	}
	for ( auto & f : files ) {
		auto byStart = [&]( auto rangeOf ) {
			return [rangeOf]( int a, int b ) {
				const Range & x = rangeOf( a ), & y = rangeOf( b );
				return x.start != y.start ? x.start < y.start : x.end < y.end;
			};
		};
		std::stable_sort( f.refs.begin(), f.refs.end(), byStart( [this]( int i ) -> const Range & { return refs[i].range; } ) );
		std::stable_sort( f.exprs.begin(), f.exprs.end(), byStart( [this]( int i ) -> const Range & { return exprs[i].range; } ) );
		std::stable_sort( f.names.begin(), f.names.end(), byStart( [this]( int i ) -> const Range & { return decls[i].nameRange; } ) );
		f.exprEnds = f.exprs;
		std::stable_sort( f.exprEnds.begin(), f.exprEnds.end(), [this]( int a, int b ) {
			const Range & x = exprs[a].range, & y = exprs[b].range;
			return x.end != y.end ? x.end < y.end : y.start < x.start;		// innermost first
		} );
	}

	// Entities: within one translation unit, two global declarations with the
	// same kind, name, type and parent declare the same thing.
	entity.assign( n, -1 );
	std::map<std::string, int> keys;
	std::function<int( int )> entityOf = [&]( int i ) -> int {
		if ( entity[i] >= 0 ) return entity[i];
		if ( entity[i] == -2 ) return i;					// parent cycle
		entity[i] = -2;
		const Decl & d = decls[i];
		int e = i;
		bool merge = !d.local && !d.generated && !d.name.empty() && d.kind != Kind::Parameter &&
			d.kind != Kind::TypeParam && d.kind != Kind::Label;
		if ( merge ) {
			int pe = d.parent >= 0 ? entityOf( d.parent ) : -1;
			std::string key = d.kindName + '\x1f' + std::to_string( pe ) + '\x1f' + d.name + '\x1f' + d.type;
			e = keys.emplace( key, i ).first->second;
		}
		entity[i] = e;
		return e;
	};
	entityMembers.assign( n, {} );
	for ( int i = 0; i < n; i += 1 ) entityMembers[entityOf( i )].push_back( i );

	for ( auto & [name, ds] : byName ) {
		std::stable_sort( ds.begin(), ds.end(), [this]( int a, int b ) { return ranksBefore( a, b ); } );
		if ( !text::isIdentifier( name ) ) continue;
		NameEntry e{ lower( name ), name, {}, 0 };
		std::unordered_set<int> seen;
		for ( int d : ds ) {
			if ( !isGlobalName( d ) ) continue;
			e.decls.push_back( d );
			if ( seen.insert( entity[d] ).second ) e.overloads += 1;
		}
		if ( !e.decls.empty() ) globals.push_back( std::move( e ) );
	}
	std::sort( globals.begin(), globals.end(), []( const NameEntry & a, const NameEntry & b ) {
		return a.lower != b.lower ? a.lower < b.lower : a.name < b.name;
	} );
}

bool Analysis::Impl::ranksBefore( int a, int b ) const {
	Origin oa = origin( a ), ob = origin( b );
	if ( oa != ob ) return oa < ob;
	const Decl & x = decls[a], & y = decls[b];
	if ( x.file != y.file ) {
		if ( x.file < 0 || y.file < 0 ) return y.file < 0;
		return files[x.file].path < files[y.file].path;
	}
	return x.nameRange.start < y.nameRange.start;
}

// Names visible at file scope: what completion offers and lookup falls back to.
bool Analysis::Impl::isGlobalName( int d ) const {
	const Decl & x = decls[d];
	if ( x.local || x.generated ) return false;
	switch ( x.kind ) {
	  case Kind::Field: case Kind::Parameter: case Kind::TypeParam: case Kind::Label: case Kind::Other:
		return false;
	  case Kind::Function: case Kind::Variable:
		return x.parent < 0 || decls[x.parent].kind != Kind::Trait;	// trait assertions aren't callable by name
	  default:
		return true;
	}
}

std::shared_ptr<const FileText> Analysis::Impl::text( int file ) const {
	if ( file < 0 || !read ) return nullptr;
	std::lock_guard lock( textLock );
	auto it = texts.find( file );
	if ( it != texts.end() ) return it->second;
	std::shared_ptr<const FileText> t;
	if ( auto s = read( files[file].path ) ) t = std::make_shared<const FileText>( std::move( *s ) );
	texts.emplace( file, t );
	return t;
}


bool Analysis::Impl::inCommentOrLiteral( int fi, Loc pos ) const {
	if ( fi < 0 || !read ) return false;
	std::shared_ptr<const std::vector<Range>> ranges;
	{
		std::lock_guard lock( textLock );
		auto it = quiet.find( fi );
		if ( it != quiet.end() ) ranges = it->second;
	}
	if ( !ranges ) {
		auto v = std::make_shared<std::vector<Range>>();
		if ( auto s = read( files[fi].path ) ) {
			LexOptions opts;
			opts.comments = true;
			for ( const Token & t : lex( *s, opts ) ) {
				if ( t.kind == TokKind::Comment || t.kind == TokKind::String || t.kind == TokKind::Char ) {
					v->push_back( { { t.line, t.col }, { t.endLine, t.endCol } } );
				}
			}
		}
		std::lock_guard lock( textLock );
		ranges = quiet.emplace( fi, std::move( v ) ).first->second;
	}
	auto it = std::upper_bound( ranges->begin(), ranges->end(), pos, []( Loc p, const Range & r ) { return p < r.start; } );
	return it != ranges->begin() && contains( *std::prev( it ), pos );
}
// -- describing declarations ---------------------------------------------------

std::string Analysis::Impl::docOf( int d ) const {
	const Decl & x = decls[d];
	if ( x.file < 0 || !x.hasLoc || x.generated ) return {};
	if ( x.kind == Kind::Parameter || x.kind == Kind::TypeParam || x.kind == Kind::Label ) return {};
	auto ft = text( x.file );
	if ( !ft ) return {};
	// Start from the first line of the declaration, unless the declaration
	// starts much earlier (e.g. at an enclosing forall).
	int nameLine = x.nameRange.start.line;
	Loc start = x.range.start.line <= nameLine && nameLine - x.range.start.line <= 3 ? x.range.start : x.nameRange.start;
	std::string_view lead = ft->line( start.line ).substr( 0, std::max( start.col, 0 ) );
	std::string above;
	if ( lead.find_first_not_of( " \t" ) == std::string_view::npos ) above = text::docAbove( *ft, start.line );
	std::string trailing;
	if ( x.kind == Kind::Variable || x.kind == Kind::Field || x.kind == Kind::Enumerator ) {
		Loc after = x.range.end.line == nameLine ? std::max( x.range.end, x.nameRange.end ) : x.nameRange.end;
		trailing = text::docTrailing( *ft, after );
	}
	if ( above.empty() ) return trailing;
	if ( trailing.empty() ) return above;
	return above + "\n\n" + trailing;
}

// A prototype's comment documents the definition too, and the other way round.
std::string Analysis::Impl::docFor( int d ) const {
	std::string doc = docOf( d );
	if ( !doc.empty() ) return doc;
	for ( int m : entityMembers[entity[d]] ) {
		if ( m == d ) continue;
		doc = docOf( m );
		if ( !doc.empty() ) return doc;
	}
	return {};
}

std::string Analysis::Impl::where( int d, const std::string & fromFile ) const {
	const Decl & x = decls[d];
	if ( x.file < 0 || !x.hasLoc ) return {};
	return whereIn( x.file, x.nameRange.start.line, fromFile );
}

std::string Analysis::Impl::whereIn( int file, int line, const std::string & fromFile ) const {
	const std::string & p = files[file].path;
	std::string shown = p;
	switch ( files[file].origin ) {
	  case Origin::Libcfa:
		for ( const char * root : { "/include/cfa/", "/libcfa/src/" } ) {
			size_t k = p.find( root );
			if ( k != std::string::npos ) { shown = p.substr( k + std::string_view( root ).size() ); break; }
		}
		break;
	  case Origin::System: {
		size_t k = p.rfind( "/include/" );
		shown = k == std::string::npos ? basename( p ) : p.substr( k + 9 );
		break;
	  }
	  case Origin::Prelude:
		shown = basename( p );
		break;
	  default: {
		size_t k = fromFile.rfind( '/' );
		if ( k != std::string::npos && p.compare( 0, k + 1, fromFile, 0, k + 1 ) == 0 ) shown = p.substr( k + 1 );
	  }
	}
	return shown + ":" + std::to_string( line + 1 );
}

std::string Analysis::Impl::kindLabel( int d ) const {
	const Decl & x = decls[d];
	auto ofParent = [&]( const char * what ) {
		if ( x.parent < 0 ) return std::string( what );
		const Decl & p = decls[x.parent];
		if ( p.name.empty() || p.name.starts_with( "__anonymous" ) ) return std::string( what ) + " of anonymous " + p.kindName;
		return std::string( what ) + " of `" + p.name + "`";
	};
	std::string label;
	switch ( x.kind ) {
	  case Kind::Variable: label = x.local ? "local variable" : "global variable"; break;
	  case Kind::Field: label = ofParent( "field" ); break;
	  case Kind::Enumerator: label = ofParent( "enumerator" ); break;
	  case Kind::TypeParam: label = "type parameter"; break;
	  case Kind::Function: label = x.parent >= 0 && decls[x.parent].kind == Kind::Trait ? ofParent( "assertion" ) : "function"; break;
	  default: label = x.kindName.empty() ? "declaration" : x.kindName;
	}
	if ( x.generated ) label += " (generated)";
	return label;
}

int Analysis::Impl::otherOverloads( int d ) const {
	const Decl & x = decls[d];
	bool assertion = x.parent >= 0 && decls[x.parent].kind == Kind::Trait;
	if ( x.local || assertion || ( x.kind != Kind::Function && x.kind != Kind::Variable ) ) return 0;
	auto it = byName.find( x.name );
	if ( it == byName.end() ) return 0;
	std::unordered_set<int> seen;
	for ( int o : it->second ) {
		const Decl & y = decls[o];
		if ( !isGlobalName( o ) || ( y.kind != Kind::Function && y.kind != Kind::Variable ) ) continue;
		if ( internalOverload( o ) && !internalOverload( d ) ) continue;
		if ( entity[o] != entity[d] ) seen.insert( entity[o] );
	}
	return int( seen.size() );
}

std::string Analysis::Impl::describe( int d, const std::string & fromFile, bool viaWith ) const {
	const Decl & x = decls[d];
	std::string sig = x.signature;
	if ( sig.empty() ) sig = x.type.empty() ? x.name : x.type + " " + x.name;
	std::string md = "```cfa\n" + sig + "\n```\n\n" + kindLabel( d );
	if ( viaWith ) md += " (through `with`)";
	std::string w = where( d, fromFile );
	if ( !w.empty() ) md += " · `" + w + "`";
	int others = otherOverloads( d );
	if ( others > 0 ) md += " · " + std::to_string( others ) + ( others == 1 ? " other overload" : " other overloads" );
	std::string doc = docFor( d );
	if ( !doc.empty() ) md += "\n\n" + doc;
	return md;
}

// Go to the body if the chosen declaration is a prototype or forward declaration.
int Analysis::Impl::definitionOf( int d ) const {
	if ( decls[d].body ) return d;
	for ( int m : entityMembers[entity[d]] ) {
		if ( decls[m].body && decls[m].hasLoc ) return m;
	}
	return d;
}

// -- positions and scopes ------------------------------------------------------

Analysis::Impl::Target Analysis::Impl::targetAt( int fi, Loc pos ) const {
	const File & f = files[fi];
	auto [r, rIn] = findAt( f.refs, pos, [this]( int i ) -> const Range & { return refs[i].range; } );
	auto [n, nIn] = findAt( f.names, pos, [this]( int i ) -> const Range & { return decls[i].nameRange; } );
	if ( r >= 0 && ( rIn || n < 0 || !nIn ) ) return { refs[r].decl, refs[r].range, refs[r].role };
	if ( n >= 0 ) return { n, decls[n].nameRange, Role::Read };
	return {};
}

int Analysis::Impl::innermostScope( int fi, Loc pos ) const {
	int best = -1;
	for ( int s : files[fi].scopes ) {
		const Range & r = scopes[s].range;
		if ( !contains( r, pos ) ) continue;
		if ( best < 0 ) { best = s; continue; }
		const Range & b = scopes[best].range;
		if ( b.start < r.start || ( r.start == b.start && r.end < b.end ) ) best = s;
	}
	return best;
}

std::vector<int> Analysis::Impl::enclosingFunctions( int fi, Loc pos ) const {
	std::vector<int> out;
	for ( int d : files[fi].bodies ) if ( contains( *decls[d].body, pos ) ) out.push_back( d );
	std::sort( out.begin(), out.end(), [this]( int a, int b ) { return decls[b].body->start < decls[a].body->start; } );
	return out;
}

// Local declarations visible at pos, innermost first.
std::vector<int> Analysis::Impl::localsAt( int fi, Loc pos ) const {
	std::vector<int> out;
	auto before = [&]( int d ) { return decls[d].file == fi && decls[d].hasLoc && decls[d].nameRange.start < pos; };
	int s = innermostScope( fi, pos );
	bool haveScopes = s >= 0;
	for ( size_t guard = 0; s >= 0 && guard < scopes.size(); s = scopes[s].parent, guard += 1 ) {
		const auto & ds = scopes[s].decls;
		for ( auto it = ds.rbegin(); it != ds.rend(); ++it ) if ( before( *it ) ) out.push_back( *it );
	}
	// Type parameters, and everything local if the dump has no scopes here.
	for ( int fn : enclosingFunctions( fi, pos ) ) {
		for ( int c : children[fn] ) {
			const Decl & d = decls[c];
			bool want = d.kind == Kind::TypeParam || ( !haveScopes && d.local && d.kind != Kind::Label && before( c ) );
			if ( want && std::find( out.begin(), out.end(), c ) == out.end() ) out.push_back( c );
		}
	}
	return out;
}

std::vector<int> Analysis::Impl::fieldsOf( int agg ) const {
	std::vector<int> out;
	if ( agg < 0 ) return out;
	for ( int m : entityMembers[entity[agg]] ) {
		for ( int c : children[m] ) {
			if ( decls[c].kind == Kind::Field && !decls[c].generated ) out.push_back( c );
		}
		if ( !out.empty() ) break;
	}
	return out;
}

int Analysis::Impl::fieldNamed( int agg, const std::string & name ) const {
	for ( int f : fieldsOf( agg ) ) if ( decls[f].name == name ) return f;
	return -1;
}

// Aggregates opened by `with` clauses around pos.
std::vector<int> Analysis::Impl::withAggregates( int fi, Loc pos ) const {
	std::vector<int> out;
	auto add = [&]( int agg ) {
		if ( agg >= 0 && std::find( out.begin(), out.end(), agg ) == out.end() ) out.push_back( agg );
	};
	auto open = [&]( const std::vector<std::string> & exprs ) {
		for ( const auto & e : exprs ) add( resolveChain( fi, pos, text::identChain( e ), false ) );
	};
	auto fns = enclosingFunctions( fi, pos );
	if ( auto ft = text( fi ) ) {
		// `void f( S & s ) with( s ) {`
		for ( int fn : fns ) {
			const Decl & d = decls[fn];
			std::string head = ft->slice( { d.nameRange.end, d.body->start } );
			size_t p = head.find( '(' );
			int depth = 0;
			for ( ; p < head.size(); p += 1 ) {
				if ( head[p] == '(' ) depth += 1;
				else if ( head[p] == ')' && --depth == 0 ) break;
			}
			if ( p < head.size() ) open( text::withClauseAtEnd( std::string_view( head ).substr( p + 1 ) ) );
		}
		// `with( s ) { ... }` statements: the clause is just before the block,
		// or at the start of the scope's range.
		int s = innermostScope( fi, pos );
		for ( size_t guard = 0; s >= 0 && guard < scopes.size(); s = scopes[s].parent, guard += 1 ) {
			Loc start = scopes[s].range.start;
			open( text::withClauseAtEnd( ft->slice( { { std::max( start.line - 2, 0 ), 0 }, start } ) ) );
			std::string head = ft->slice( { start, { start.line + 2, 0 } } );
			size_t brace = head.find( '{' );
			if ( brace != std::string::npos && brace > 0 ) open( text::withClauseAtEnd( std::string_view( head ).substr( 0, brace ) ) );
		}
	}
	// Fields the translator resolved through `with` in the enclosing function.
	if ( !fns.empty() ) {
		const Range & body = *decls[fns.front()].body;
		const auto & rs = files[fi].refs;
		auto it = std::lower_bound( rs.begin(), rs.end(), body.start,
									[this]( int r, Loc p ) { return refs[r].range.start < p; } );
		for ( ; it != rs.end() && refs[*it].range.start < body.end; ++it ) {
			const Ref & r = refs[*it];
			if ( r.role != Role::With ) continue;
			int p = decls[r.decl].parent;
			if ( p >= 0 && isAggregate( decls[p].kind ) ) add( p );
		}
	}
	return out;
}

// Declarations named `name` visible at pos: locals innermost first, fields
// opened by `with`, then globals, best origin first.
std::vector<int> Analysis::Impl::visibleNamed( int fi, Loc pos, const std::string & name, bool useWith ) const {
	std::vector<int> out;
	if ( fi >= 0 ) {
		for ( int d : localsAt( fi, pos ) ) if ( decls[d].name == name ) out.push_back( d );
		if ( useWith ) {
			for ( int agg : withAggregates( fi, pos ) ) {
				int f = fieldNamed( agg, name );
				if ( f >= 0 ) out.push_back( f );
			}
		}
	}
	auto it = byName.find( name );
	if ( it != byName.end() ) {
		for ( int d : it->second ) {
			if ( isGlobalName( d ) && std::find( out.begin(), out.end(), d ) == out.end() ) out.push_back( d );
		}
	}
	return out;
}

// The aggregate that `a.b.c` has as its type, by name lookup alone.
int Analysis::Impl::resolveChain( int fi, Loc pos, const std::vector<std::string> & chain, bool useWith ) const {
	if ( chain.empty() ) return -1;
	auto cands = visibleNamed( fi, pos, chain[0], useWith );
	if ( cands.empty() ) return -1;
	int agg = decls[cands[0]].typeDecl;
	if ( agg < 0 && decls[cands[0]].kind == Kind::Function ) {
		for ( int c : cands ) {
			if ( decls[c].kind == Kind::Function && decls[c].typeDecl >= 0 ) { agg = decls[c].typeDecl; break; }
		}
	}
	for ( size_t i = 1; i < chain.size() && agg >= 0; i += 1 ) {
		int f = fieldNamed( agg, chain[i] );
		agg = f >= 0 ? decls[f].typeDecl : -1;
	}
	return agg;
}

namespace {

// The number of pointer levels in a simple type ("Node **", "const S * const &"), or -1 if the type has
// parentheses or brackets.
int pointerLevels( std::string_view type ) {
	if ( type.empty() || type.find_first_of( "([" ) != std::string_view::npos ) return -1;
	return int( std::count( type.begin(), type.end(), '*' ) );
}

} // namespace

// The aggregate type of a declaration found by name: an aggregate, or what a typedef names.
int Analysis::Impl::aggregateNamed( const std::string & name ) const {
	auto it = byName.find( name );
	if ( it == byName.end() ) return -1;
	for ( int d : it->second ) {
		const Decl & x = decls[d];
		if ( isAggregate( x.kind ) && x.kind != Kind::Trait ) return d;
		if ( x.kind == Kind::Typedef && x.typeDecl >= 0 ) return x.typeDecl;
	}
	return -1;
}

// A declaration of `name` in text the translator hasn't seen (`Circle & c = w;`, `Rect r;`): the aggregate
// of its type and its pointer levels. The nearest declaration before the end of `text` wins.
std::optional<std::pair<int, int>> Analysis::Impl::declaredInText( std::string_view text, const std::string & name ) const {
	for ( const text::TextDeclaration & d : text::declarationsOf( text, name ) ) {
		int agg = aggregateNamed( d.typeName );
		if ( agg >= 0 ) return std::pair( agg, d.pointers );
	}
	return std::nullopt;
}

// Is `name` a type: a basic type keyword, or an aggregate, typedef or type parameter in the dump?
bool Analysis::Impl::namesType( const std::string & name ) const {
	static const std::set<std::string_view> basic = {
		"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool", "_Bool",
		"_Complex", "zero_t", "one_t",
	};
	if ( basic.count( name ) ) return true;
	auto it = byName.find( name );
	if ( it == byName.end() ) return false;
	for ( int d : it->second ) {
		Kind k = decls[d].kind;
		if ( ( isAggregate( k ) && k != Kind::Trait ) || k == Kind::Typedef || k == Kind::TypeParam ) return true;
	}
	return false;
}

// The declaration of the name at `offset` in `text`, by scanning the text: the nearest one before it whose type
// is a type.
std::optional<Analysis::Impl::TextDecl> Analysis::Impl::textDeclarationAt( const std::string & text, size_t offset ) const {
	auto found = text::declarationsAt( text, offset );
	if ( !found ) return std::nullopt;
	for ( const text::TextDeclaration & d : found->declarations ) {
		if ( !namesType( d.typeName ) ) continue;
		auto locOf = [&]( size_t off ) {
			size_t nl = off == 0 ? std::string::npos : text.rfind( '\n', off - 1 );
			return Loc{ int( std::count( text.begin(), text.begin() + off, '\n' ) ),
						int( nl == std::string::npos ? off : off - nl - 1 ) };
		};
		TextDecl out;
		out.name = found->name;
		std::string type;
		for ( char c : std::string_view( text ).substr( d.typeStart, d.typeEnd - d.typeStart ) ) {
			bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
			if ( !space ) type += c;
			else if ( !type.empty() && type.back() != ' ' ) type += ' ';
		}
		out.type = type;
		out.nameRange = { locOf( d.nameStart ), locOf( d.nameEnd ) };
		out.useRange = { locOf( found->start ), locOf( found->end ) };
		return out;
	}
	return std::nullopt;
}

// The aggregate on the left of the `.` or `->` being completed.
int Analysis::Impl::memberAggregate( int fi, Loc pos, std::string_view lineBefore, const text::CompletionContext & ctx,
									 std::string_view textBefore ) const {
	std::string_view after = lineBefore.substr( std::min<size_t>( ctx.operandEnd, lineBefore.size() ) );
	after.remove_prefix( std::min( after.size(), after.find_first_not_of( " \t" ) ) );
	bool arrow = after.starts_with( "->" );
	// `.` needs an aggregate and `->` a pointer to one. Checked when the operand is a plain name, possibly
	// dereferenced as (*p), whose type is known.
	auto levelsFit = [&]( int levels, int derefs ) { return levels < 0 || levels - derefs == ( arrow ? 1 : 0 ); };
	bool plainName = ctx.chain.size() == 1 && ctx.operand == ctx.chain[0];

	if ( fi >= 0 ) {
		// Where the operand would end and start in the snapshot, assuming the
		// line hasn't changed before the cursor.
		Loc end{ pos.line, pos.col - ( int( lineBefore.size() ) - ctx.operandEnd ) };
		Loc start{ pos.line, pos.col - ( int( lineBefore.size() ) - ctx.operandStart ) };
		if ( end.col >= 0 ) {
			auto ft = text( fi );
			auto matches = [&]( const Range & r ) { return !ft || stripSpaces( ft->slice( r ) ) == ctx.operand; };
			const auto & ends = files[fi].exprEnds;
			auto it = std::lower_bound( ends.begin(), ends.end(), end,
										[this]( int e, Loc p ) { return exprs[e].range.end < p; } );
			int best = -1;
			for ( ; it != ends.end() && exprs[*it].range.end == end; ++it ) {
				const Expr & e = exprs[*it];
				if ( e.typeDecl < 0 || !matches( e.range ) ) continue;
				if ( best < 0 || ( e.range.start == start && exprs[best].range.start != start ) ) best = *it;
			}
			if ( best >= 0 ) {
				if ( plainName && !levelsFit( pointerLevels( exprs[best].type ), 0 ) ) return -1;
				return exprs[best].typeDecl;
			}

			if ( !ctx.chain.empty() && text::isIdentChar( ctx.operand.back() ) ) {
				const auto & rs = files[fi].refs;
				auto rit = std::upper_bound( rs.begin(), rs.end(), end,
											 [this]( Loc p, int r ) { return p < refs[r].range.start; } );
				for ( int k = 0; rit != rs.begin() && k < 16; k += 1 ) {
					--rit;
					const Ref & r = refs[*rit];
					if ( r.range.end == end && decls[r.decl].name == ctx.chain.back() && decls[r.decl].typeDecl >= 0 ) {
						if ( plainName && !levelsFit( pointerLevels( decls[r.decl].type ), 0 ) ) return -1;
						return decls[r.decl].typeDecl;
					}
				}
			}
		}
	}

	// By name: a chain a.b->c, or a dereferenced one, (*p) or (**pp).
	std::vector<std::string> chain = ctx.chain;
	int derefs = 0;
	if ( chain.empty() && ctx.operand.size() > 3 && ctx.operand.front() == '(' && ctx.operand.back() == ')' ) {
		std::string_view inner( ctx.operand );
		inner = inner.substr( 1, inner.size() - 2 );
		while ( !inner.empty() && inner.front() == '*' ) {
			derefs += 1;
			inner.remove_prefix( 1 );
		}
		if ( derefs > 0 ) chain = text::identChain( inner );
		plainName = chain.size() == 1;
	}
	if ( chain.empty() ) return -1;
	auto cands = visibleNamed( fi, pos, chain[0], true );
	int agg = -1, levels = -1;
	if ( !cands.empty() ) {
		agg = resolveChain( fi, pos, { chain[0] }, true );
		levels = pointerLevels( decls[cands[0]].type );
	} else if ( auto found = declaredInText( textBefore, chain[0] ) ) {
		// Declared since the last successful check.
		agg = found->first;
		levels = found->second;
	}
	if ( plainName && !levelsFit( levels, derefs ) ) return -1;
	for ( size_t i = 1; i < chain.size() && agg >= 0; i += 1 ) {
		int f = fieldNamed( agg, chain[i] );
		agg = f >= 0 ? decls[f].typeDecl : -1;
	}
	return agg;
}

// Candidate declarations for an identifier the translator left unresolved.
std::vector<int> Analysis::Impl::unresolvedAt( int fi, Loc pos, Range & range ) const {
	auto ft = text( fi );
	if ( !ft ) return {};
	std::string_view line = ft->line( pos.line );
	auto id = text::identifierAt( line, pos.col );
	if ( !id ) return {};
	std::string name( line.substr( id->first, id->second - id->first ) );
	range = { { pos.line, id->first }, { pos.line, id->second } };

	auto ctx = text::completionContext( line.substr( 0, id->first ) );
	if ( ctx.kind == text::CompletionContext::Member ) {
		int f = fieldNamed( resolveChain( fi, pos, ctx.chain, true ), name );
		if ( f >= 0 ) return { f };
		std::vector<int> out;
		auto it = byName.find( name );
		if ( it != byName.end() ) {
			for ( int d : it->second ) if ( decls[d].kind == Kind::Field && !decls[d].generated ) out.push_back( d );
		}
		return out;
	}
	return visibleNamed( fi, pos, name, true );
}

std::vector<int> Analysis::Impl::functionsNamed( const std::string & name ) const {
	std::vector<int> out, generated;
	auto it = byName.find( name );
	if ( it == byName.end() ) return out;
	std::unordered_set<int> seen;
	for ( int d : it->second ) {
		const Decl & x = decls[d];
		if ( x.kind != Kind::Function || ( x.parent >= 0 && decls[x.parent].kind == Kind::Trait ) ) continue;
		if ( !seen.insert( entity[d] ).second ) continue;
		( decls[d].generated ? generated : out ).push_back( d );
	}
	return out.empty() ? generated : out;
}

CompletionItem Analysis::Impl::item( int d, char rank, const std::string & detailSuffix, bool withDoc ) const {
	const Decl & x = decls[d];
	CompletionItem c;
	c.label = x.name;
	c.kind = completionKind( x );
	switch ( x.kind ) {
	  case Kind::Function: c.detail = x.signature.empty() ? x.type : x.signature; break;
	  case Kind::Variable: case Kind::Parameter: case Kind::Field: case Kind::Enumerator: c.detail = x.type; break;
	  default: c.detail = x.signature.empty() ? x.kindName : x.signature;
	}
	c.detail += detailSuffix;
	if ( withDoc ) c.documentation = docFor( d );
	c.sortText = std::string( 1, rank ) + x.name;
	return c;
}

// -- macros --------------------------------------------------------------------

namespace {

// Whether cpp kept a conditional branch, as far as we can tell.
enum class Branch { Live, Dead, Unknown };

// The first character of `l` outside comments and whitespace, or -1.
// `comment` says whether a /* */ comment is open, at the start of the line
// and after it.
int firstCode( std::string_view l, bool & comment ) {
	int first = -1;
	for ( size_t i = 0; i < l.size(); ) {
		if ( comment ) {
			size_t e = l.find( "*/", i );
			if ( e == std::string_view::npos ) return first;
			comment = false;
			i = e + 2;
			continue;
		}
		char c = l[i];
		if ( c == '/' && i + 1 < l.size() && l[i + 1] == '/' ) return first;
		if ( c == '/' && i + 1 < l.size() && l[i + 1] == '*' ) {
			comment = true;
			i += 2;
			continue;
		}
		if ( std::isspace( (unsigned char)c ) ) {
			i += 1;
			continue;
		}
		if ( first < 0 ) first = int( i );
		if ( c == '"' || c == '\'' ) {
			size_t j = i + 1;
			while ( j < l.size() && l[j] != c ) j += l[j] == '\\' ? 2 : 1;
			i = j + 1;
			continue;
		}
		i += 1;
	}
	return first;
}

bool continues( std::string_view l ) {
	size_t e = l.find_last_not_of( " \t\r" );
	return e != std::string_view::npos && l[e] == '\\';
}

// The directive on a line whose first code character is the '#' at `hash`:
// its name and the text after it.
std::pair<std::string_view, std::string_view> directiveAt( std::string_view l, int hash ) {
	size_t i = l.find_first_not_of( " \t", hash + 1 );
	if ( i == std::string_view::npos ) return {};
	size_t e = i;
	while ( e < l.size() && text::isIdentChar( l[e] ) ) e += 1;
	return { l.substr( i, e - i ), l.substr( e ) };
}

// What a macro means in an #if.
struct MacroDef {
	enum State { Unknown, Undefined, Defined } state = Unknown;
	std::string body;						// replacement text, for Defined
	bool function = false;
};

// Evaluates a #if expression as far as the known macros allow: nullopt when
// it depends on something unknown (a macro from the compiler or the command
// line, a function-like macro, __has_include, ...).
class CondEval {
  public:
	using Lookup = std::function<MacroDef( const std::string & )>;
	explicit CondEval( Lookup lookup ) : lookup( std::move( lookup ) ) {}

	std::optional<long long> run( std::string_view expr ) {
		// Saved for a nested run, which evaluates a macro's body.
		std::vector<std::string> saved = toks;
		size_t savedAt = at;
		bool savedBad = bad;
		toks = tokenize( expr );
		at = 0;
		bad = false;
		V v = cond();
		if ( at != toks.size() ) bad = true;
		bool failed = bad;
		toks = std::move( saved );
		at = savedAt;
		bad = savedBad;
		if ( failed || !v.known ) return std::nullopt;
		return v.value;
	}

  private:
	struct V {
		bool known = false;
		long long value = 0;
	};
	static V unknown() { return {}; }
	static V of( long long v ) { return { true, v }; }

	Lookup lookup;
	std::vector<std::string> toks;
	size_t at = 0;
	bool bad = false;
	std::set<std::string> expanding;		// macros being evaluated, against recursion

	static std::vector<std::string> tokenize( std::string_view s ) {
		std::vector<std::string> out;
		static const char * two[] = { "&&", "||", "==", "!=", "<=", ">=", "<<", ">>" };
		for ( size_t i = 0; i < s.size(); ) {
			char c = s[i];
			if ( std::isspace( (unsigned char)c ) || c == '\\' ) {
				i += 1;
			} else if ( c == '/' && i + 1 < s.size() && s[i + 1] == '/' ) {
				break;
			} else if ( c == '/' && i + 1 < s.size() && s[i + 1] == '*' ) {
				size_t e = s.find( "*/", i + 2 );
				i = e == std::string_view::npos ? s.size() : e + 2;
			} else if ( text::isIdentChar( c ) ) {		// identifiers and numbers, with their suffixes
				size_t e = i;
				while ( e < s.size() && text::isIdentChar( s[e] ) ) e += 1;
				out.emplace_back( s.substr( i, e - i ) );
				i = e;
			} else if ( c == '\'' ) {
				size_t e = i + 1;
				while ( e < s.size() && s[e] != '\'' ) e += s[e] == '\\' ? 2 : 1;
				e = std::min( e + 1, s.size() );
				out.emplace_back( s.substr( i, e - i ) );
				i = e;
			} else {
				std::string op( 1, c );
				for ( const char * t : two ) {
					if ( s.compare( i, 2, t ) == 0 ) op = t;
				}
				out.push_back( op );
				i += op.size();
			}
		}
		return out;
	}

	bool peek( const char * t ) const { return at < toks.size() && toks[at] == t; }
	bool take( const char * t ) {
		if ( !peek( t ) ) return false;
		at += 1;
		return true;
	}
	void expect( const char * t ) {
		if ( !take( t ) ) bad = true;
	}

	V cond() {
		V c = lor();
		if ( !take( "?" ) ) return c;
		V a = cond();
		expect( ":" );
		V b = cond();
		if ( !c.known ) return unknown();
		return c.value ? a : b;
	}
	V lor() {
		V a = land();
		while ( take( "||" ) ) {
			V b = land();
			if ( ( a.known && a.value ) || ( b.known && b.value ) ) a = of( 1 );
			else if ( a.known && b.known ) a = of( 0 );
			else a = unknown();
		}
		return a;
	}
	V land() {
		V a = binary( 0 );
		while ( take( "&&" ) ) {
			V b = binary( 0 );
			if ( ( a.known && !a.value ) || ( b.known && !b.value ) ) a = of( 0 );
			else if ( a.known && b.known ) a = of( 1 );
			else a = unknown();
		}
		return a;
	}
	// Left-associative binary operators, loosest first.
	V binary( int level ) {
		static const std::vector<std::vector<std::string>> levels = {
			{ "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<", ">", "<=", ">=" }, { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
		};
		if ( level == int( levels.size() ) ) return unary();
		V a = binary( level + 1 );
		for ( ;; ) {
			if ( at >= toks.size() ) return a;
			const std::string & op = toks[at];
			if ( std::find( levels[level].begin(), levels[level].end(), op ) == levels[level].end() ) return a;
			at += 1;
			V b = binary( level + 1 );
			if ( !a.known || !b.known ) {
				a = unknown();
				continue;
			}
			long long x = a.value, y = b.value;
			if ( ( op == "/" || op == "%" ) && ( y == 0 || ( x == LLONG_MIN && y == -1 ) ) ) {
				a = unknown();
				continue;
			}
			if ( ( op == "<<" || op == ">>" ) && ( y < 0 || y > 62 ) ) {
				a = unknown();
				continue;
			}
			// +, -, * and << wrap instead of overflowing, which is undefined
			// for signed numbers.
			unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y;
			long long r = op == "|" ? x | y : op == "^" ? x ^ y : op == "&" ? x & y : op == "==" ? x == y : op == "!=" ? x != y
						: op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : op == ">=" ? x >= y
						: op == "<<" ? (long long)( ux << y ) : op == ">>" ? x >> y : op == "+" ? (long long)( ux + uy )
						: op == "-" ? (long long)( ux - uy ) : op == "*" ? (long long)( ux * uy ) : op == "/" ? x / y : x % y;
			a = of( r );
		}
	}
	V unary() {
		if ( take( "!" ) ) { V v = unary(); return v.known ? of( !v.value ) : v; }
		if ( take( "~" ) ) { V v = unary(); return v.known ? of( ~v.value ) : v; }
		if ( take( "-" ) ) { V v = unary(); return v.known ? of( (long long)( 0ULL - (unsigned long long)v.value ) ) : v; }
		if ( take( "+" ) ) return unary();
		return primary();
	}
	// Skips a parenthesised argument list, if one follows.
	void skipArgs() {
		if ( !take( "(" ) ) return;
		for ( int depth = 1; at < toks.size() && depth > 0; at += 1 ) {
			if ( toks[at] == "(" ) depth += 1;
			else if ( toks[at] == ")" ) depth -= 1;
		}
	}
	V primary() {
		if ( at >= toks.size() ) {
			bad = true;
			return unknown();
		}
		std::string t = toks[at++];
		if ( t == "(" ) {
			V v = cond();
			expect( ")" );
			return v;
		}
		if ( t[0] == '\'' ) return character( t );
		if ( std::isdigit( (unsigned char)t[0] ) ) return number( t );
		if ( !text::isIdentStart( t[0] ) ) {
			bad = true;
			return unknown();
		}
		if ( t == "defined" ) {
			bool paren = take( "(" );
			if ( at >= toks.size() || !text::isIdentStart( toks[at][0] ) ) {
				bad = true;
				return unknown();
			}
			MacroDef d = lookup( toks[at++] );
			if ( paren ) expect( ")" );
			if ( d.state == MacroDef::Unknown ) return unknown();
			return of( d.state == MacroDef::Defined );
		}
		MacroDef d = lookup( t );
		if ( d.state == MacroDef::Undefined ) return of( 0 );		// as in cpp
		if ( d.state == MacroDef::Unknown || d.function || expanding.count( t ) || expanding.size() > 16 ) {
			skipArgs();										// __has_include( x ), F( y )
			return unknown();
		}
		expanding.insert( t );
		std::optional<long long> v = run( d.body );
		expanding.erase( t );
		return v ? of( *v ) : unknown();
	}
	static V number( const std::string & t ) {
		std::string digits = t;
		while ( !digits.empty() && ( digits.back() == 'u' || digits.back() == 'U' || digits.back() == 'l' || digits.back() == 'L' ) ) digits.pop_back();
		if ( digits.empty() ) return unknown();
		errno = 0;
		char * end = nullptr;
		unsigned long long v = std::strtoull( digits.c_str(), &end, 0 );
		if ( errno != 0 || !end || *end != '\0' ) return unknown();
		return of( (long long)v );
	}
	static V character( const std::string & t ) {
		if ( t.size() == 3 && t[1] != '\\' ) return of( (unsigned char)t[1] );
		if ( t.size() == 4 && t[1] == '\\' ) {
			switch ( t[2] ) {
			  case 'n': return of( '\n' );
			  case 't': return of( '\t' );
			  case '0': return of( 0 );
			  case '\\': return of( '\\' );
			  case '\'': return of( '\'' );
			}
		}
		return unknown();
	}
};

Branch evaluate( std::string_view directive, std::string_view rest, const CondEval::Lookup & lookup ) {
	if ( directive == "ifdef" || directive == "ifndef" ) {
		size_t b = rest.find_first_not_of( " \t" );
		if ( b == std::string_view::npos ) return Branch::Unknown;
		size_t e = b;
		while ( e < rest.size() && text::isIdentChar( rest[e] ) ) e += 1;
		MacroDef d = lookup( std::string( rest.substr( b, e - b ) ) );
		if ( d.state == MacroDef::Unknown ) return Branch::Unknown;
		return ( d.state == MacroDef::Defined ) == ( directive == "ifdef" ) ? Branch::Live : Branch::Dead;
	}
	std::optional<long long> v = CondEval( lookup ).run( rest );
	if ( !v ) return Branch::Unknown;
	return *v ? Branch::Live : Branch::Dead;
}

// What the preprocessed output says about the branches of a file's
// conditionals, keyed by the line of the #if, #elif or #else that opens the
// branch. A branch that left any line in the output was kept. One with code
// of its own (outside nested conditionals and comments) that left none was
// dropped. A branch with only directives and comments says nothing.
std::unordered_map<int, Branch> branchEvidence( const FileText & t, const std::vector<int> & present ) {
	struct Open { int start; bool code; };
	std::vector<Open> stack;
	std::unordered_map<int, Branch> out;
	auto close = [&]( int end ) {
		const Open & o = stack.back();
		auto it = std::upper_bound( present.begin(), present.end(), o.start );
		if ( it != present.end() && *it < end ) out[o.start] = Branch::Live;
		else if ( o.code ) out[o.start] = Branch::Dead;
	};
	bool comment = false, inDirective = false;
	for ( int n = 0; n < t.lineCount(); n += 1 ) {
		std::string_view l = t.line( n );
		int first = firstCode( l, comment );
		if ( inDirective ) {
			inDirective = continues( l );
			continue;
		}
		if ( first < 0 ) continue;
		if ( l[first] != '#' ) {
			if ( !stack.empty() ) stack.back().code = true;
			continue;
		}
		inDirective = continues( l );
		std::string_view directive = directiveAt( l, first ).first;
		if ( directive == "if" || directive == "ifdef" || directive == "ifndef" ) {
			stack.push_back( { n, false } );
		} else if ( ( directive == "elif" || directive == "else" ) && !stack.empty() ) {
			close( n );
			stack.back() = { n, false };
		} else if ( directive == "endif" && !stack.empty() ) {
			close( n );
			stack.pop_back();
		}
	}
	return out;
}

// "#define NAME( a ) body" -> its replacement text, for #if.
MacroDef macroDefinition( const std::string & text, bool function ) {
	MacroDef d;
	d.state = MacroDef::Defined;
	d.function = function;
	size_t i = text.find( "define" );
	if ( i == std::string::npos ) return d;
	i = text.find_first_not_of( " \t", i + 6 );
	while ( i < text.size() && text::isIdentChar( text[i] ) ) i += 1;
	if ( i >= text.size() ) return d;
	std::string body = text.substr( i );
	for ( size_t k; ( k = body.find( "\\\n" ) ) != std::string::npos; ) body.replace( k, 2, " " );
	d.body = body;
	return d;
}

} // namespace

void Analysis::Impl::loadMacros() const {
	std::call_once( macrosOnce, [this] {
		// Libraries first, so their macros are known in the #if conditions
		// of the project's files.
		std::vector<int> order( files.size() );
		for ( size_t i = 0; i < files.size(); i += 1 ) order[i] = int( i );
		std::stable_sort( order.begin(), order.end(), [this]( int a, int b ) { return files[b].origin < files[a].origin; } );
		for ( int f : order ) {
			auto t = text( f );
			if ( !t ) continue;
			std::unordered_map<int, Branch> evidence;
			if ( files[f].present ) evidence = branchEvidence( *t, *files[f].present );
			struct Level {
				Branch branch;
				bool taken;						// an earlier branch of this #if was live
				bool unsure;					// an earlier branch was unknown
			};
			std::vector<Level> stack;
			std::unordered_map<std::string, std::vector<size_t>> open;	// name -> macros of this file not yet #undef'd
			std::unordered_set<std::string> undefined;	// #undef'd in this file and not defined again
			auto dead = [&] {
				for ( const Level & l : stack ) if ( l.branch == Branch::Dead ) return true;
				return false;
			};
			CondEval::Lookup lookup = [&]( const std::string & name ) -> MacroDef {
				auto o = open.find( name );
				if ( o != open.end() && !o->second.empty() ) {
					const Macro & m = macros[name][o->second.back()];
					return macroDefinition( m.text, m.function );
				}
				if ( undefined.count( name ) ) return { MacroDef::Undefined, "", false };
				if ( name == "__cforall" || name == "__CFA__" || name == "__CFORALL__" ) return { MacroDef::Defined, "1", false };
				if ( name == "__cplusplus" ) return { MacroDef::Undefined, "", false };
				auto it = macros.find( name );
				if ( it != macros.end() ) {
					for ( const Macro & m : it->second ) {
						if ( m.file != f && m.undefLine < 0 ) return macroDefinition( m.text, m.function );
					}
				}
				return {};
			};
			auto evidenceAt = [&]( int n ) -> std::optional<Branch> {
				auto it = evidence.find( n );
				if ( it == evidence.end() ) return std::nullopt;
				return it->second;
			};
			bool comment = false, inDirective = false;
			for ( int n = 0; n < t->lineCount(); n += 1 ) {
				std::string_view l = t->line( n );
				int first = firstCode( l, comment );
				if ( inDirective ) {
					inDirective = continues( l );
					continue;
				}
				if ( first < 0 || l[first] != '#' ) continue;
				inDirective = continues( l );
				auto [directive, rest] = directiveAt( l, first );
				if ( directive == "if" || directive == "ifdef" || directive == "ifndef" || directive == "elif" ) {
					// The condition, with its continuation lines.
					std::string cond( rest );
					for ( int k = n; k + 1 < t->lineCount() && continues( t->line( k ) ); k += 1 ) cond += "\n" + std::string( t->line( k + 1 ) );
					if ( directive != "elif" ) {
						Branch b = evidenceAt( n ).value_or( evaluate( directive, cond, lookup ) );
						stack.push_back( { b, b == Branch::Live, b == Branch::Unknown } );
						continue;
					}
					if ( stack.empty() ) continue;
					Level & top = stack.back();
					if ( auto ev = evidenceAt( n ) ) top.branch = *ev;
					else if ( top.taken ) top.branch = Branch::Dead;
					else {
						top.branch = evaluate( directive, cond, lookup );
						if ( top.branch == Branch::Live && top.unsure ) top.branch = Branch::Unknown;
					}
					top.taken = top.taken || top.branch == Branch::Live;
					top.unsure = top.unsure || top.branch == Branch::Unknown;
					continue;
				}
				if ( directive == "else" ) {
					if ( stack.empty() ) continue;
					Level & top = stack.back();
					if ( auto ev = evidenceAt( n ) ) top.branch = *ev;
					else if ( top.taken ) top.branch = Branch::Dead;
					else top.branch = top.unsure ? Branch::Unknown : Branch::Live;
					top.taken = top.taken || top.branch == Branch::Live;
					continue;
				}
				if ( directive == "endif" ) {
					if ( !stack.empty() ) stack.pop_back();
					continue;
				}
				bool define = directive == "define";
				if ( !define && directive != "undef" ) continue;
				if ( dead() ) continue;
				size_t de = rest.data() - l.data();		// end of the directive name
				size_t b = l.find_first_not_of( " \t", de );
				if ( b == std::string_view::npos || b == de ) continue;
				size_t e = b;
				while ( e < l.size() && text::isIdentChar( l[e] ) ) e += 1;
				if ( e == b ) continue;
				std::string name( l.substr( b, e - b ) );
				if ( !define ) {
					for ( size_t k : open[name] ) macros[name][k].undefLine = n;
					open.erase( name );
					undefined.insert( name );
					continue;
				}
				undefined.erase( name );
				std::string body( l.substr( first ) );
				for ( int k = n; k + 1 < t->lineCount() && !t->line( k ).empty() && t->line( k ).back() == '\\'; k += 1 ) {
					if ( k - n == 20 ) { body += "\n..."; break; }
					body += "\n" + std::string( t->line( k + 1 ) );
				}
				auto & list = macros[name];
				open[name].push_back( list.size() );
				list.push_back( { f, { { n, int( b ) }, { n, int( e ) } }, std::move( body ), -1, e < l.size() && l[e] == '(' } );
			}
		}
		// Same file and project macros first: macroNamed() takes the first
		// one from another file.
		for ( auto & entry : macros ) {
			std::stable_sort( entry.second.begin(), entry.second.end(),
							  [this]( const Macro & a, const Macro & b ) { return files[a.file].origin < files[b.file].origin; } );
		}
	} );
}

// The macro named `name` that is defined at `pos`: one from the same file
// defined above it and not #undef'd yet, else one from another file that is
// never #undef'd.
const Analysis::Impl::Macro * Analysis::Impl::macroNamed( const std::string & name, int fi, Loc pos ) const {
	loadMacros();
	auto it = macros.find( name );
	if ( it == macros.end() ) return nullptr;
	const Macro * other = nullptr;
	for ( const Macro & m : it->second ) {
		if ( m.file == fi ) {
			if ( m.name.start.line <= pos.line && ( m.undefLine < 0 || pos.line < m.undefLine ) ) return &m;
		} else if ( m.undefLine < 0 && !other ) {
			other = &m;
		}
	}
	return other;
}

const Analysis::Impl::Macro * Analysis::Impl::macroAt( int fi, Loc pos ) const {
	auto ft = text( fi );
	if ( !ft ) return nullptr;
	auto id = text::identifierAt( ft->line( pos.line ), pos.col );
	if ( !id ) return nullptr;
	if ( inCommentOrLiteral( fi, pos ) ) return nullptr;
	std::string name( ft->line( pos.line ).substr( id->first, id->second - id->first ) );
	if ( typeParamAt( fi, pos, name ) >= 0 ) return nullptr;
	return macroNamed( name, fi, pos );
}

// A type parameter named `name` of a declaration around pos (forall( T ) ...
// { T x; }, including its assertions).
int Analysis::Impl::typeParamAt( int fi, Loc pos, const std::string & name ) const {
	auto it = byName.find( name );
	if ( it == byName.end() ) return -1;
	int best = -1;
	for ( int d : it->second ) {
		const Decl & x = decls[d];
		if ( x.kind != Kind::TypeParam || x.file != fi || x.parent < 0 ) continue;
		const Decl & p = decls[x.parent];
		if ( p.file != fi || !contains( p.range, pos ) ) continue;
		if ( best < 0 || decls[decls[best].parent].range.start < p.range.start ) best = d;
	}
	return best;
}

// The type parameter named by the identifier at pos, if one is in scope there.
int Analysis::Impl::typeParamUnder( int fi, Loc pos, Range & range ) const {
	auto ft = text( fi );
	if ( !ft ) return -1;
	auto id = text::identifierAt( ft->line( pos.line ), pos.col );
	if ( !id || inCommentOrLiteral( fi, pos ) ) return -1;
	range = { { pos.line, id->first }, { pos.line, id->second } };
	return typeParamAt( fi, pos, std::string( ft->line( pos.line ).substr( id->first, id->second - id->first ) ) );
}

// A library declaration that mentions libcfa's internal names (thread$, coroutine$).
bool Analysis::Impl::internalOverload( int d ) const {
	return isLibrary( origin( d ) ) && ( decls[d].signature.find( '$' ) != std::string::npos || decls[d].name.find( '$' ) != std::string::npos );
}

// Signature help in a call of a function-like macro.
std::optional<SignatureHelp> Analysis::Impl::macroSignature( const std::string & name, int fi, Loc pos, int argIndex ) const {
	if ( fi < 0 ) return std::nullopt;
	const Macro * mac = macroNamed( name, fi, pos );
	if ( !mac || !mac->function ) return std::nullopt;
	// "#define SQ( x ) ((x) * (x))" -> "SQ( x )"
	size_t b = mac->text.find( name );
	size_t e = b == std::string::npos ? b : mac->text.find( ')', b );
	if ( e == std::string::npos ) return std::nullopt;
	SignatureInfo s;
	s.label = mac->text.substr( b, e - b + 1 );
	s.params = text::paramSpans( s.label, name );
	s.documentation = "macro · `" + whereIn( mac->file, mac->name.start.line, files[fi].path ) + "`";
	SignatureHelp help;
	help.signatures.push_back( std::move( s ) );
	help.activeParameter = argIndex;
	return help;
}

std::optional<int> Analysis::Impl::includeAt( int fi, Loc pos ) const {
	auto ft = text( fi );
	if ( !ft ) return std::nullopt;
	std::string_view l = ft->line( pos.line );
	size_t i = l.find_first_not_of( " \t" );
	if ( i == std::string_view::npos || l[i] != '#' ) return std::nullopt;
	i = l.find_first_not_of( " \t", i + 1 );
	if ( i == std::string_view::npos || l.compare( i, 7, "include" ) != 0 ) return std::nullopt;
	size_t open = l.find_first_of( "\"<", i );
	if ( open == std::string_view::npos ) return std::nullopt;
	size_t close = l.find( l[open] == '"' ? '"' : '>', open + 1 );
	if ( close == std::string_view::npos ) return std::nullopt;
	std::string name = "/" + std::string( l.substr( open + 1, close - open - 1 ) );
	std::string dir = files[fi].path.substr( 0, files[fi].path.rfind( '/' ) );
	std::optional<int> found;
	for ( size_t f = 0; f < files.size(); f += 1 ) {
		const std::string & p = files[f].path;
		if ( p.size() < name.size() || p.compare( p.size() - name.size(), name.size(), name ) != 0 ) continue;
		if ( l[open] == '"' && p == dir + name ) return int( f );	// next to the including file
		if ( !found ) found = int( f );
	}
	return found;
}

std::string Analysis::Impl::describeMacro( const Macro & m, const std::string & fromFile ) const {
	return "```c\n" + m.text + "\n```\n\nmacro · `" + whereIn( m.file, m.name.start.line, fromFile ) + "`";
}

// -- public interface ----------------------------------------------------------

Analysis::Analysis() : impl( std::make_unique<Impl>() ) {}
Analysis::~Analysis() = default;

std::shared_ptr<const Analysis> Analysis::load( const json & dump, const SourceMap & map, SourceMap::Reader read ) {
	std::shared_ptr<Analysis> a( new Analysis() );
	a->impl->read = std::move( read );
	try {
		a->impl->load( dump, map );
	} catch ( const json::exception & e ) {
		throw std::runtime_error( std::string( "malformed dump: " ) + e.what() );
	}
	return a;
}

bool Analysis::complete() const { return impl->complete; }
std::vector<Diagnostic> Analysis::diagnostics() const { return impl->diags; }

std::optional<HoverResult> Analysis::hover( const std::string & file, Loc pos ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return std::nullopt;
	auto t = m.targetAt( fi, pos );
	if ( t.decl >= 0 ) {
		std::string md = m.describe( t.decl, file, t.role == Role::With );
		// A field of a generic aggregate has its instance's type here (T first -> int).
		if ( t.role == Role::Member || t.role == Role::With ) {
			auto [e, in] = findAt( m.files[fi].exprs, pos, [&m]( int i ) -> const Range & { return m.exprs[i].range; } );
			if ( e >= 0 && in && m.exprs[e].range == t.range && !m.exprs[e].type.empty() && m.exprs[e].type != m.decls[t.decl].type ) {
				md += "\n\nType here: `" + m.exprs[e].type + "`";
			}
		}
		return HoverResult{ md, t.range };
	}
	Range idRange;
	if ( int p = m.typeParamUnder( fi, pos, idRange ); p >= 0 ) return HoverResult{ m.describe( p, file, false ), idRange };
	if ( auto mac = m.macroAt( fi, pos ) ) {
		auto id = text::identifierAt( m.text( fi )->line( pos.line ), pos.col );
		return HoverResult{ m.describeMacro( *mac, file ), { { pos.line, id->first }, { pos.line, id->second } } };
	}

	auto [e, in] = findAt( m.files[fi].exprs, pos, [&m]( int i ) -> const Range & { return m.exprs[i].range; } );
	if ( e >= 0 && !m.exprs[e].type.empty() ) {
		return HoverResult{ "```cfa\n" + m.exprs[e].type + "\n```", m.exprs[e].range };
	}

	if ( m.inCommentOrLiteral( fi, pos ) ) return std::nullopt;
	Range range;
	auto cands = m.unresolvedAt( fi, pos, range );
	if ( cands.empty() ) return std::nullopt;
	std::string md = m.describe( cands[0], file, false ) + "\n\n*Not resolved by the translator";
	if ( cands.size() > 1 ) md += "; " + std::to_string( cands.size() ) + " declarations have this name";
	md += ".*";
	return HoverResult{ md, range };
}

std::vector<Location> Analysis::definition( const std::string & file, Loc pos ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	std::vector<Location> out;
	if ( auto inc = m.includeAt( fi, pos ) ) return { { m.files[*inc].path, {} } };
	auto t = m.targetAt( fi, pos );
	if ( t.decl >= 0 ) {
		int d = m.definitionOf( t.decl );
		if ( m.decls[d].hasLoc ) out.push_back( m.location( d ) );
		return out;
	}
	Range idRange;
	if ( int p = m.typeParamUnder( fi, pos, idRange ); p >= 0 ) return { m.location( p ) };
	if ( auto mac = m.macroAt( fi, pos ) ) return { { m.files[mac->file].path, mac->name } };
	if ( m.inCommentOrLiteral( fi, pos ) ) return {};
	Range range;
	std::unordered_set<int> seen;
	for ( int c : m.unresolvedAt( fi, pos, range ) ) {
		int d = m.definitionOf( c );
		if ( m.decls[d].hasLoc && seen.insert( m.entity[d] ).second ) out.push_back( m.location( d ) );
	}
	return out;
}

std::optional<HoverResult> Analysis::hoverInText( const std::string & text, size_t offset ) const {
	auto d = impl->textDeclarationAt( text, offset );
	if ( !d ) return std::nullopt;
	std::string md = "```cfa\n" + d->type + " " + d->name + "\n```\n\nline " +
					 std::to_string( d->nameRange.start.line + 1 ) +
					 " · *from the text: the translator has not checked this declaration*";
	return HoverResult{ md, d->useRange };
}

std::optional<Range> Analysis::declarationInText( const std::string & text, size_t offset ) const {
	auto d = impl->textDeclarationAt( text, offset );
	if ( !d ) return std::nullopt;
	return d->nameRange;
}

std::vector<Location> Analysis::Impl::entityReferences( int decl, bool includeDeclaration ) const {
	std::vector<Location> found;
	for ( int d : entityMembers[entity[decl]] ) {
		for ( int r : refsOf[d] ) found.push_back( { files[refs[r].file].path, refs[r].range } );
		if ( includeDeclaration && decls[d].hasLoc && !decls[d].generated ) found.push_back( location( d ) );
	}
	std::sort( found.begin(), found.end(), []( const Location & a, const Location & b ) {
		if ( a.file != b.file ) return a.file < b.file;
		if ( a.range.start != b.range.start ) return a.range.start < b.range.start;
		return a.range.end < b.range.end;
	} );
	found.erase( std::unique( found.begin(), found.end() ), found.end() );
	return found;
}

// The declaration at one of `locs` (by file and name position).
int Analysis::Impl::declAtLocation( const std::vector<Location> & locs ) const {
	for ( const Location & l : locs ) {
		int fi = findFile( l.file );
		if ( fi < 0 ) continue;
		auto [n, in] = findAt( files[fi].names, l.range.start, [this]( int i ) -> const Range & { return decls[i].nameRange; } );
		if ( n >= 0 && in && decls[n].nameRange.start == l.range.start ) return n;
	}
	return -1;
}

std::vector<Location> Analysis::references( const std::string & file, Loc pos, bool includeDeclaration ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto t = m.targetAt( fi, pos );
	if ( t.decl < 0 ) return {};
	return m.entityReferences( t.decl, includeDeclaration );
}

std::vector<Location> Analysis::declarationsAt( const std::string & file, Loc pos ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto t = m.targetAt( fi, pos );
	if ( t.decl < 0 ) return {};
	std::vector<Location> out;
	for ( int d : m.entityMembers[m.entity[t.decl]] ) {
		if ( m.decls[d].hasLoc && !m.decls[d].generated ) out.push_back( m.location( d ) );
	}
	return out;
}

std::vector<Location> Analysis::referencesTo( const std::vector<Location> & declarations, bool includeDeclaration ) const {
	int d = impl->declAtLocation( declarations );
	return d < 0 ? std::vector<Location>() : impl->entityReferences( d, includeDeclaration );
}

std::optional<Location> Analysis::definitionOf( const std::vector<Location> & declarations ) const {
	const Impl & m = *impl;
	int d = m.declAtLocation( declarations );
	if ( d < 0 ) return std::nullopt;
	d = m.definitionOf( d );
	if ( !m.decls[d].body || !m.decls[d].hasLoc ) return std::nullopt;
	return m.location( d );
}

std::vector<Symbol> Analysis::documentSymbols( const std::string & file ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto ft = m.text( fi );
	std::unordered_set<int> picked;
	for ( int d : m.files[fi].names ) {
		const Decl & x = m.decls[d];
		if ( x.local || x.kind == Kind::Parameter || x.kind == Kind::TypeParam || x.kind == Kind::Label || x.kind == Kind::Other ) continue;
		// Declarations a macro made up (ExceptionDecl( E ) declares E_vt) are not in the outline.
		if ( ft && !spelledName( *ft, x.nameRange, x.name ) ) continue;
		picked.insert( d );
	}
	std::function<Symbol( int, int )> build = [&]( int d, int depth ) {
		const Decl & x = m.decls[d];
		Symbol s;
		s.name = x.name.empty() || x.name.starts_with( "__anonymous" ) ? "(anonymous " + x.kindName + ")" : x.name;
		s.kind = symbolKind( x );
		s.detail = isAggregate( x.kind ) || x.kind == Kind::Typedef ? x.kindName : x.type;
		s.range = x.range;
		s.selectionRange = x.nameRange;
		if ( depth < 16 ) {
			for ( int c : m.children[d] ) if ( picked.count( c ) ) s.children.push_back( build( c, depth + 1 ) );
		}
		return s;
	};
	std::vector<Symbol> out;
	for ( int d : m.files[fi].names ) {
		if ( !picked.count( d ) ) continue;
		int p = m.decls[d].parent;
		if ( p >= 0 && picked.count( p ) ) continue;
		out.push_back( build( d, 0 ) );
	}
	return out;
}

UnitIndex Analysis::unitIndex() const {
	const Impl & m = *impl;
	UnitIndex out;
	auto inProject = [&]( int fi ) {
		const File & f = m.files[fi];
		return !isLibrary( f.origin ) && !f.path.empty() && f.path[0] == '/';
	};
	auto wanted = [&]( int d ) {
		const Decl & x = m.decls[d];
		if ( x.local || x.generated || x.name.empty() ) return false;
		switch ( x.kind ) {
		  case Kind::Parameter: case Kind::TypeParam: case Kind::Label: case Kind::Other:
			return false;
		  default:
			return true;
		}
	};
	std::unordered_map<int, int> slot;		// canonical decl -> index in out.entities
	auto entityOf = [&]( int d ) -> int {
		int e = m.entity[d];
		if ( auto it = slot.find( e ); it != slot.end() ) return it->second;
		const Decl & x = m.decls[e];
		UnitIndex::Entity ent;
		ent.name = x.name;
		ent.kind = symbolKind( x );
		ent.detail = x.signature.empty() ? x.type : x.signature;
		ent.function = x.kind == Kind::Function;
		ent.library = true;
		for ( int k : m.entityMembers[e] ) {
			const Decl & y = m.decls[k];
			if ( y.file < 0 || !y.hasLoc || y.generated ) continue;
			ent.declarations.push_back( m.location( k ) );
			if ( inProject( y.file ) ) ent.library = false;
		}
		int def = m.definitionOf( e );
		if ( m.decls[def].body && m.decls[def].hasLoc && m.decls[def].file >= 0 ) {
			ent.definition = m.location( def );
			ent.definitionRange = m.decls[def].range;
		}
		int i = int( out.entities.size() );
		slot.emplace( e, i );
		out.entities.push_back( std::move( ent ) );
		return i;
	};

	m.loadMacros();
	for ( int fi = 0; fi < int( m.files.size() ); fi += 1 ) {
		const File & f = m.files[fi];
		if ( !f.focus || f.path.empty() || f.path[0] != '/' ) continue;
		// Spellings the dump doesn't account for, the same ones rename() checks.
		if ( auto source = m.read ? m.read( f.path ) : std::nullopt ) {
			std::set<Loc> known;
			for ( int r : f.refs ) known.insert( m.refs[r].range.start );
			for ( int n : f.names ) known.insert( m.decls[n].nameRange.start );
			for ( const Token & tok : lex( *source ) ) {
				if ( tok.kind != TokKind::Identifier || tok.line != tok.endLine || known.count( Loc{ tok.line, tok.col } ) ||
					 !text::isIdentifier( tok.text ) || isKeyword( tok.text ) ) {
					continue;
				}
				out.loose.push_back( { tok.text, { f.path, { { tok.line, tok.col }, { tok.line, tok.endCol } } }, false } );
			}
		}
		for ( const auto & [name, list] : m.macros ) {
			for ( const Impl::Macro & mac : list ) {
				if ( mac.file != fi ) continue;
				std::string_view body( mac.text );
				size_t k = body.find( "define" );
				if ( k != std::string_view::npos ) k = body.find( name, k + 6 );
				if ( k == std::string_view::npos ) continue;
				body.remove_prefix( k + name.size() );
				std::set<std::string> params;
				if ( mac.function ) {
					size_t close = body.find( ')' );
					if ( close == std::string_view::npos ) continue;
					for ( const Token & t : lex( body.substr( 0, close ) ) ) params.insert( t.text );
					body.remove_prefix( close + 1 );
				}
				std::set<std::string> named;
				for ( const Token & t : lex( body ) ) {
					if ( t.kind != TokKind::Identifier || params.count( t.text ) || isKeyword( t.text ) || !named.insert( t.text ).second ) continue;
					out.loose.push_back( { t.text, { f.path, mac.name }, true } );
				}
			}
		}
		for ( int r : f.refs ) {
			const Ref & ref = m.refs[r];
			if ( !wanted( ref.decl ) ) continue;
			UnitIndex::Ref u;
			u.loc = { f.path, ref.range };
			u.entity = entityOf( ref.decl );
			u.call = ref.role == Role::Call;
			// Calls in a nested function count for the function around it.
			for ( int fn : m.enclosingFunctions( fi, ref.range.start ) ) {
				if ( wanted( fn ) ) {
					u.caller = entityOf( fn );
					break;
				}
			}
			out.refs.push_back( std::move( u ) );
		}
	}

	std::function<void( const Symbol &, const std::string &, const std::string & )> flat =
		[&]( const Symbol & s, const std::string & file, const std::string & container ) {
			out.symbols.push_back( { s.name, container, s.kind, { file, s.selectionRange } } );
			for ( const Symbol & c : s.children ) flat( c, file, s.name );
		};
	for ( int fi = 0; fi < int( m.files.size() ); fi += 1 ) {
		if ( !inProject( fi ) ) continue;
		for ( int d : m.files[fi].names ) {
			if ( wanted( d ) ) entityOf( d );
		}
		const std::string & path = m.files[fi].path;
		for ( const Symbol & s : documentSymbols( path ) ) flat( s, path, "" );
	}
	return out;
}

// libcfa marks its internal names with a '$' (thread$, file$).
static bool internalName( const std::string & name, const std::string & prefix ) {
	return name.find( '$' ) != std::string::npos && prefix.find( '$' ) == std::string::npos;
}

std::vector<CompletionItem> Analysis::completion( const std::string & file, Loc pos, const std::string & lineBefore,
												  const std::string & textBefore ) const {
	const Impl & m = *impl;
	auto ctx = text::completionContext( lineBefore );
	if ( ctx.kind == text::CompletionContext::None ) return {};
	int fi = m.findFile( file );
	std::vector<CompletionItem> out;

	if ( ctx.kind == text::CompletionContext::Member ) {
		int agg = m.memberAggregate( fi, pos, lineBefore, ctx, textBefore );
		for ( int f : m.fieldsOf( agg ) ) {
			if ( internalName( m.decls[f].name, ctx.prefix ) && isLibrary( m.origin( f ) ) ) continue;
			if ( startsWithNoCase( m.decls[f].name, ctx.prefix ) ) out.push_back( m.item( f, '0', "", true ) );
		}
		return out;
	}

	std::unordered_set<std::string> seen;
	auto wanted = [&]( const std::string & name ) {
		return text::isIdentifier( name ) && startsWithNoCase( name, ctx.prefix ) && !seen.count( name );
	};
	if ( fi >= 0 ) {
		for ( int d : m.localsAt( fi, pos ) ) {
			if ( m.decls[d].generated || !wanted( m.decls[d].name ) ) continue;
			seen.insert( m.decls[d].name );
			out.push_back( m.item( d, '0', "", true ) );
		}
		for ( int agg : m.withAggregates( fi, pos ) ) {
			std::string suffix = m.decls[agg].name.empty() ? "" : "  (with " + m.decls[agg].name + ")";
			for ( int f : m.fieldsOf( agg ) ) {
				if ( !wanted( m.decls[f].name ) ) continue;
				seen.insert( m.decls[f].name );
				out.push_back( m.item( f, '0', suffix, true ) );
			}
		}
	}

	std::string lp = lower( ctx.prefix );
	auto first = std::lower_bound( m.globals.begin(), m.globals.end(), lp,
								   []( const NameEntry & e, const std::string & p ) { return e.lower < p; } );
	auto last = first;
	while ( last != m.globals.end() && last->lower.starts_with( lp ) ) ++last;
	bool smallList = last - first <= 50;
	bool wantUnderscore = !ctx.prefix.empty() && ctx.prefix[0] == '_';
	for ( auto it = first; it != last; ++it ) {
		if ( seen.count( it->name ) ) continue;
		int d = it->decls.front();
		Origin o = m.origin( d );
		if ( isLibrary( o ) && it->name[0] == '_' && !wantUnderscore ) continue;	// reserved names
		if ( isLibrary( o ) && internalName( it->name, ctx.prefix ) ) continue;
		seen.insert( it->name );
		std::string suffix;
		if ( it->overloads > 1 ) {
			int others = it->overloads - 1;
			suffix = "  (+" + std::to_string( others ) + ( others == 1 ? " overload)" : " overloads)" );
		}
		char rank = o == Origin::Focus ? '1' : o == Origin::Project ? '2' : '3';
		out.push_back( m.item( d, rank, suffix, !isLibrary( o ) || smallList ) );
	}

	// Macros defined at the cursor. Library macros only once something is typed.
	if ( fi >= 0 ) {
		m.loadMacros();
		for ( const auto & [name, list] : m.macros ) {
			if ( !wanted( name ) ) continue;
			const Impl::Macro * mac = m.macroNamed( name, fi, pos );
			if ( !mac ) continue;
			Origin o = m.files[mac->file].origin;
			if ( isLibrary( o ) && ( ctx.prefix.empty() || ( name[0] == '_' && !wantUnderscore ) ) ) continue;
			seen.insert( name );
			CompletionItem c;
			c.label = name;
			c.kind = mac->function ? 3 : 21;			// Function, Constant
			c.detail = mac->text.substr( 0, mac->text.find( '\n' ) );
			c.sortText = std::string( 1, o == Origin::Focus ? '1' : o == Origin::Project ? '2' : '3' ) + name;
			out.push_back( std::move( c ) );
		}
	}

	for ( const auto & k : keywords() ) {
		if ( !startsWithNoCase( k, ctx.prefix ) || seen.count( k ) ) continue;
		CompletionItem c;
		c.label = k;
		c.kind = 14;
		c.sortText = "4" + k;
		out.push_back( std::move( c ) );
	}
	return out;
}

// Optimal string alignment distance (Levenshtein plus swaps of neighbouring
// characters), or limit + 1 once it is known to exceed `limit`.
static int editDistance( std::string_view a, std::string_view b, int limit ) {
	int na = int( a.size() ), nb = int( b.size() );
	if ( std::abs( na - nb ) > limit ) return limit + 1;
	std::vector<int> prev2( nb + 1 ), prev( nb + 1 ), cur( nb + 1 );
	for ( int j = 0; j <= nb; j += 1 ) prev[j] = j;
	for ( int i = 1; i <= na; i += 1 ) {
		cur[0] = i;
		int best = cur[0];
		for ( int j = 1; j <= nb; j += 1 ) {
			int cost = a[i - 1] == b[j - 1] ? 0 : 1;
			cur[j] = std::min( { prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost } );
			if ( i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] ) cur[j] = std::min( cur[j], prev2[j - 2] + 1 );
			best = std::min( best, cur[j] );
		}
		if ( best > limit ) return limit + 1;
		std::swap( prev2, prev );
		std::swap( prev, cur );
	}
	return std::min( prev[nb], limit + 1 );
}

std::vector<std::string> Analysis::similarNames( const std::string & file, Loc pos, const std::string & name, size_t max ) const {
	const Impl & m = *impl;
	if ( !text::isIdentifier( name ) ) return {};
	// About one edit in three characters, and never all of either name: x is not a typo of y.
	const int limit = std::max( 1, ( int( name.size() ) + 2 ) / 3 );
	struct Cand { int dist, rank; std::string name; };
	std::vector<Cand> cands;
	std::unordered_set<std::string> seen;
	auto distance = [&]( const std::string & n ) {
		if ( n == name || !text::isIdentifier( n ) ) return limit + 1;
		int d = editDistance( name, n, limit );
		return d >= int( name.size() ) || d >= int( n.size() ) ? limit + 1 : d;
	};
	auto consider = [&]( const std::string & n, int rank ) {
		if ( seen.count( n ) ) return;
		int d = distance( n );
		if ( d > limit ) return;
		seen.insert( n );
		cands.push_back( { d, rank, n } );
	};
	auto rankOf = []( Origin o ) { return o == Origin::Focus ? 2 : o == Origin::Project ? 3 : 4; };
	bool wantUnderscore = name[0] == '_';

	int fi = m.findFile( file );
	if ( fi >= 0 ) {
		for ( int d : m.localsAt( fi, pos ) ) {
			if ( !m.decls[d].generated ) consider( m.decls[d].name, 0 );
		}
		for ( int agg : m.withAggregates( fi, pos ) ) {
			for ( int f : m.fieldsOf( agg ) ) consider( m.decls[f].name, 1 );
		}
	}
	for ( const NameEntry & e : m.globals ) {
		Origin o = m.origin( e.decls.front() );
		if ( isLibrary( o ) && ( ( e.name[0] == '_' && !wantUnderscore ) || internalName( e.name, name ) ) ) continue;
		consider( e.name, rankOf( o ) );
	}
	if ( fi >= 0 ) {
		m.loadMacros();
		for ( const auto & [n, list] : m.macros ) {
			if ( seen.count( n ) || distance( n ) > limit ) continue;
			const Impl::Macro * mac = m.macroNamed( n, fi, pos );
			if ( !mac ) continue;
			Origin o = m.files[mac->file].origin;
			if ( isLibrary( o ) && n[0] == '_' && !wantUnderscore ) continue;
			consider( n, rankOf( o ) );
		}
	}
	std::sort( cands.begin(), cands.end(), []( const Cand & a, const Cand & b ) {
		if ( a.dist != b.dist ) return a.dist < b.dist;
		if ( a.rank != b.rank ) return a.rank < b.rank;
		return a.name < b.name;
	} );
	std::vector<std::string> out;
	for ( const Cand & c : cands ) {
		if ( out.size() == max ) break;
		out.push_back( c.name );
	}
	return out;
}

std::optional<SignatureHelp> Analysis::signatureHelp( const std::string & file, Loc pos, const std::string & textBefore ) const {
	const Impl & m = *impl;
	auto call = text::callBefore( textBefore );
	if ( !call ) return std::nullopt;

	// An operator run like "x=?+?" holds the name as a suffix; try the longest.
	std::string name;
	std::vector<int> fns;
	for ( size_t k = 0; k < call->name.size(); k += 1 ) {
		if ( k > 0 && text::isIdentStart( call->name[0] ) ) break;
		fns = m.functionsNamed( call->name.substr( k ) );
		if ( !fns.empty() ) { name = call->name.substr( k ); break; }
	}
	if ( fns.empty() ) return m.macroSignature( call->name, m.findFile( file ), pos, call->argIndex );

	// The overload the translator chose, from the call's ref in the snapshot.
	int resolved = -1;
	int fi = m.findFile( file );
	if ( fi >= 0 ) {
		size_t nameStart = call->nameEnd - name.size();
		auto lineCol = [&]( size_t off ) {
			int line = int( std::count( textBefore.begin(), textBefore.begin() + off, '\n' ) );
			size_t nl = off == 0 ? std::string::npos : textBefore.rfind( '\n', off - 1 );
			return Loc{ line, int( off - ( nl == std::string::npos ? 0 : nl + 1 ) ) };
		};
		Loc cursor = lineCol( textBefore.size() ), at = lineCol( nameStart );
		Loc guess = at.line == cursor.line ? Loc{ pos.line, pos.col - ( cursor.col - at.col ) }
										   : Loc{ pos.line - ( cursor.line - at.line ), at.col };
		auto [r, in] = findAt( m.files[fi].refs, guess, [&m]( int i ) -> const Range & { return m.refs[i].range; } );
		if ( r >= 0 ) {
			int d = m.refs[r].decl;
			if ( m.decls[d].kind == Kind::Function && m.decls[d].name == name ) resolved = d;
		}
	}
	if ( resolved >= 0 ) {
		auto it = std::find_if( fns.begin(), fns.end(), [&]( int d ) { return m.entity[d] == m.entity[resolved]; } );
		if ( it != fns.end() ) fns.erase( it );
		fns.insert( fns.begin(), resolved );
	}
	// libcfa's internal overloads (resume( coroutine$ * )) unless the translator chose one.
	std::erase_if( fns, [&]( int d ) { return d != resolved && m.internalOverload( d ); } );
	if ( fns.empty() ) return std::nullopt;
	if ( fns.size() > 64 ) fns.resize( 64 );

	SignatureHelp help;
	help.activeParameter = call->argIndex;
	help.activeSignature = -1;
	for ( int d : fns ) {
		const Decl & x = m.decls[d];
		SignatureInfo s;
		s.label = !x.signature.empty() ? x.signature : x.type.empty() ? x.name + "()" : x.type + " " + x.name;
		s.params = text::paramSpans( s.label, x.name );
		s.documentation = m.docFor( d );
		bool variadic = !s.params.empty() && s.label.compare( s.params.back().first, 3, "..." ) == 0;
		bool fits = int( s.params.size() ) > call->argIndex || variadic || ( s.params.empty() && call->argIndex == 0 );
		if ( help.activeSignature < 0 && fits ) help.activeSignature = int( help.signatures.size() );
		help.signatures.push_back( std::move( s ) );
	}
	if ( help.activeSignature < 0 ) help.activeSignature = 0;
	return help;
}

std::vector<SemanticToken> Analysis::semanticTokens( const std::string & file ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto ft = m.text( fi );
	struct Tok { SemanticToken t; int prio; };
	std::vector<Tok> toks;
	auto add = [&]( Range r, int d, bool declaration ) {
		const Decl & x = m.decls[d];
		if ( r.start.line != r.end.line || x.name.empty() ) return;
		int type = tokenType( x.kind );
		if ( type < 0 ) return;
		if ( ft ) {
			auto s = spelledUse( *ft, r, x.name, true );
			if ( !s ) return;
			r = *s;
		}
		int len = r.end.col - r.start.col;
		if ( len <= 0 ) return;
		int mods = declaration ? MDeclaration : 0;
		if ( x.kind == Kind::Enumerator || ( ( x.kind == Kind::Variable || x.kind == Kind::Parameter || x.kind == Kind::Field ) && isConstType( x.type ) ) ) mods |= MReadonly;
		if ( isLibrary( m.origin( d ) ) ) mods |= MDefaultLibrary;
		toks.push_back( { { r.start, len, type, mods }, declaration ? 0 : 1 } );
	};
	for ( int r : m.files[fi].refs ) add( m.refs[r].range, m.refs[r].decl, false );
	for ( int d : m.files[fi].names ) if ( m.decls[d].hasName ) add( m.decls[d].nameRange, d, true );
	std::stable_sort( toks.begin(), toks.end(), []( const Tok & a, const Tok & b ) {
		return a.t.start != b.t.start ? a.t.start < b.t.start : a.prio < b.prio;
	} );
	std::vector<SemanticToken> out;
	for ( const Tok & t : toks ) {
		if ( !out.empty() ) {
			const SemanticToken & p = out.back();
			if ( p.start.line == t.t.start.line && t.t.start.col < p.start.col + p.length ) continue;
		}
		out.push_back( t.t );
	}
	return out;
}

std::vector<SemanticToken> Analysis::keywordTokens( std::string_view text ) {
	// The words cforall/src/Parser/lex.ll always reads as keywords, minus the
	// basic type names, which tree-sitter's C++ grammar colours well enough.
	static const std::unordered_set<std::string_view> words = {
		"alignas", "_Alignas", "alignof", "_Alignof", "__alignof", "__alignof__", "and", "asm", "__asm",
		"__asm__", "_Atomic", "__attribute", "__attribute__", "auto", "__auto_type", "basetypeof", "break",
		"case", "catch", "catchResume", "choose", "coerce", "cofor", "const", "__const", "__const__",
		"continue", "coroutine", "corun", "countof", "default", "disable", "do", "dtype", "else", "enable",
		"enum", "exception", "__extension__", "extern", "fallthrough", "finally", "fixup", "for", "forall",
		"fortran", "ftype", "generator", "_Generic", "goto", "if", "inline", "__inline", "__inline__",
		"__label__", "monitor", "mutex", "_Noreturn", "or", "otype", "recover", "register", "report",
		"restrict", "__restrict", "__restrict__", "return", "sizeof", "static", "_Static_assert",
		"static_assert", "struct", "suspend", "switch", "thread", "__thread", "_Thread_local",
		"thread_local", "throw", "throwResume", "timeout", "trait", "try", "ttype", "typedef", "typeid",
		"typeof", "__typeof", "__typeof__", "union", "virtual", "volatile", "__volatile", "__volatile__",
		"vtable", "waitfor", "waituntil", "when", "while", "with",
	};
	std::vector<SemanticToken> out;
	for ( const Token & t : lex( text ) ) {
		if ( t.kind != TokKind::Identifier || t.endLine != t.line || !words.count( t.text ) ) continue;
		out.push_back( { { t.line, t.col }, t.endCol - t.col, TKeyword, 0 } );
	}
	return out;
}

// -- highlights, inlay hints, rename -------------------------------------------

namespace {

bool isHeaderPath( const std::string & path ) {
	for ( std::string_view ext : { ".hfa", ".h", ".ifa" } ) {
		if ( path.size() > ext.size() && path.ends_with( ext ) ) return true;
	}
	return false;
}

// The return type in a signature from the dump: "forall( T ) T * biggest( T a, T b )" -> "T *". Empty if it
// can't be found.
std::string declaredReturnType( std::string_view sig, const std::string & name ) {
	size_t b = 0;
	while ( sig.substr( b ).starts_with( "forall" ) ) {
		size_t i = sig.find( '(', b );
		int depth = 0;
		for ( ; i < sig.size(); i += 1 ) {
			if ( sig[i] == '(' ) depth += 1;
			else if ( sig[i] == ')' && --depth == 0 ) break;
		}
		if ( i >= sig.size() ) return {};
		b = i + 1;
		while ( b < sig.size() && sig[b] == ' ' ) b += 1;
	}
	size_t k = sig.find( " " + name + "(", b );
	if ( k == std::string_view::npos ) return {};
	return trimmed( sig.substr( b, k - b ) );
}

// Is `name` an identifier in `text`, outside string literals?
bool mentions( std::string_view text, const std::string & name ) {
	for ( const Token & t : lex( text ) ) {
		if ( t.kind == TokKind::Identifier && t.text == name ) return true;
	}
	return false;
}

} // namespace

std::vector<DocumentHighlight> Analysis::documentHighlights( const std::string & file, Loc pos ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto t = m.targetAt( fi, pos );
	if ( t.decl < 0 ) return {};
	std::vector<DocumentHighlight> out;
	for ( int d : m.entityMembers[m.entity[t.decl]] ) {
		const Decl & x = m.decls[d];
		if ( x.file == fi && x.hasName && !x.generated ) out.push_back( { x.nameRange, 1 } );
		for ( int r : m.refsOf[d] ) {
			if ( m.refs[r].file == fi ) out.push_back( { m.refs[r].range, 2 } );
		}
	}
	std::sort( out.begin(), out.end(), []( const DocumentHighlight & a, const DocumentHighlight & b ) {
		return a.range.start != b.range.start ? a.range.start < b.range.start : a.kind < b.kind;
	} );
	out.erase( std::unique( out.begin(), out.end(),
							[]( const DocumentHighlight & a, const DocumentHighlight & b ) { return a.range == b.range; } ),
			   out.end() );
	return out;
}

std::vector<InlayHint> Analysis::inlayHints( const std::string & file, Range range ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return {};
	auto ft = m.text( fi );
	if ( !ft ) return {};
	const File & f = m.files[fi];
	auto inside = [&]( Loc p ) { return range.start <= p && p <= range.end; };
	std::vector<InlayHint> out;
	std::set<std::pair<Loc, std::string>> seen;
	auto add = [&]( InlayHint h ) {
		if ( seen.insert( { h.pos, h.label } ).second ) out.push_back( std::move( h ) );
	};
	for ( int r : f.refs ) {
		const Ref & ref = m.refs[r];
		if ( range.end < ref.range.start ) break;
		if ( ref.role != Role::Call || ref.range.end.line + 64 < range.start.line ) continue;
		const Decl & fn = m.decls[ref.decl];
		if ( fn.kind != Kind::Function || !text::isIdentifier( fn.name ) || ft->slice( ref.range ) != fn.name ) continue;
		auto call = text::callArguments( *ft, ref.range.end );
		if ( !call || call->end < range.start ) continue;
		Range span{ ref.range.start, call->end };

		for ( size_t i = 0; i < call->args.size() && i < fn.params.size(); i += 1 ) {
			const std::string & param = fn.params[i];
			const Range & arg = call->args[i];
			if ( param.empty() || param.starts_with( "__" ) || arg.start == arg.end || !inside( arg.start ) ) continue;
			// Not when the argument already says it: f( x ), f( &x ), f( s.x ) for a parameter x.
			std::string a = ft->slice( arg );
			size_t k = std::min( a.find_first_not_of( "&* \t" ), a.size() );
			auto chain = text::identChain( std::string_view( a ).substr( k ) );
			if ( !chain.empty() && chain.back() == param ) continue;
			add( { arg.start, param + ":", 2, span } );
		}

		// What a polymorphic call returns here, when that says more than the declaration.
		if ( !fn.signature.starts_with( "forall" ) || !inside( call->end ) ) continue;
		auto it = std::lower_bound( f.exprs.begin(), f.exprs.end(), ref.range.start,
									[&m]( int e, Loc p ) { return m.exprs[e].range.start < p; } );
		int best = -1;
		for ( ; it != f.exprs.end() && m.exprs[*it].range.start == ref.range.start; ++it ) {
			const Range & er = m.exprs[*it].range;
			if ( er.end <= ref.range.end || call->end < er.end ) continue;
			if ( best < 0 || m.exprs[best].range.end < er.end ) best = *it;
		}
		if ( best < 0 ) continue;
		const std::string & type = m.exprs[best].type;
		if ( type.empty() || type == "void" || stripSpaces( type ) == stripSpaces( declaredReturnType( fn.signature, fn.name ) ) ) continue;
		add( { call->end, ": " + type, 1, span } );
	}
	std::stable_sort( out.begin(), out.end(), []( const InlayHint & a, const InlayHint & b ) { return a.pos < b.pos; } );
	return out;
}

std::optional<RenamePlan> Analysis::rename( const std::string & file, Loc pos, bool acrossFiles ) const {
	const Impl & m = *impl;
	int fi = m.findFile( file );
	if ( fi < 0 ) return std::nullopt;
	auto t = m.targetAt( fi, pos );
	if ( t.decl < 0 ) return std::nullopt;
	const Decl & x = m.decls[t.decl];
	RenamePlan plan;
	plan.name = x.name;
	plan.range = t.range;
	const std::string quoted = "`" + x.name + "`";
	auto refuse = [&]( const std::string & why ) {
		plan.error = why;
		plan.sites.clear();
		return plan;
	};
	if ( !text::isIdentifier( x.name ) ) return refuse( quoted + " is not an identifier; only identifiers can be renamed" );
	for ( int d : m.entityMembers[m.entity[t.decl]] ) {
		if ( m.decls[d].generated || m.decls[d].file == fi ) continue;
		std::string w = m.where( d, file );
		if ( acrossFiles && !isLibrary( m.origin( d ) ) ) continue;
		return refuse( quoted + " is declared in " + ( w.empty() ? std::string( "another file" ) : "`" + w + "`" ) +
					   ( acrossFiles ? "" : "; renaming it across files needs the workspace index" ) );
	}
	if ( isHeaderPath( file ) && !x.local && !acrossFiles ) {
		return refuse( quoted + " is declared in a header, and the files that include it are not known; "
					   "only local names can be renamed in a header" );
	}
	auto ft = m.text( fi );
	for ( const Location & l : m.entityReferences( t.decl, true ) ) {
		if ( l.file != file && acrossFiles ) continue;
		if ( l.file != file ) return refuse( quoted + " is used in another file; renaming it across files needs the workspace index" );
		if ( ft && ft->slice( l.range ) != x.name ) return refuse( "an occurrence of " + quoted + " does not spell its name; rename it by hand" );
		plan.sites.push_back( l.range );
	}
	// Uses inside macro bodies have no refs; don't leave them behind.
	m.loadMacros();
	for ( const auto & [name, list] : m.macros ) {
		for ( const Impl::Macro & mac : list ) {
			if ( mac.file != fi ) continue;
			std::string_view body( mac.text );
			size_t k = body.find( "define" );
			if ( k != std::string_view::npos ) k = body.find( name, k + 6 );
			if ( k == std::string_view::npos ) continue;
			body.remove_prefix( k + name.size() );
			if ( mac.function ) {
				// A parameter of the same name is not a use.
				size_t close = body.find( ')' );
				if ( close == std::string_view::npos ) continue;
				if ( mentions( body.substr( 0, close ), x.name ) ) continue;
				body.remove_prefix( close + 1 );
			}
			if ( mentions( body, x.name ) ) {
				return refuse( quoted + " is used in the macro `" + name + "` on line " + std::to_string( mac.name.start.line + 1 ) +
							   ", which the translator doesn't see; rename it by hand" );
			}
		}
	}
	// The dump has no refs in array dimensions, designators, dead #if
	// branches or functions that failed to resolve. A spelling of the name
	// that is neither a site nor the ref or name of another declaration may be
	// one of those, and renaming would leave it behind.
	std::optional<std::string> source;
	if ( m.read ) source = m.read( m.files[fi].path );
	if ( source ) {
		std::set<Loc> known;
		for ( const Range & r : plan.sites ) known.insert( r.start );
		for ( int r : m.files[fi].refs ) known.insert( m.refs[r].range.start );
		for ( int n : m.files[fi].names ) known.insert( m.decls[n].nameRange.start );
		for ( const Token & tok : lex( *source ) ) {
			if ( tok.kind != TokKind::Identifier || tok.text != x.name || known.count( Loc{ tok.line, tok.col } ) ) continue;
			return refuse( quoted + " appears on line " + std::to_string( tok.line + 1 ) +
						   " where the translator recorded no use (an array dimension, a designator, dead code "
						   "or code that failed to resolve); rename it by hand" );
		}
	}
	return plan;
}

bool Analysis::isKeyword( std::string_view word ) {
	const auto & words = keywords();
	return std::find( words.begin(), words.end(), word ) != words.end();
}

std::vector<std::string> Analysis::projectFiles() const {
	std::vector<std::string> out;
	for ( const File & f : impl->files ) {
		if ( ( f.origin == Origin::Focus || f.origin == Origin::Project ) && !f.names.empty() ) out.push_back( f.path );
	}
	return out;
}

const std::vector<std::string> & Analysis::tokenTypes() {
	static const std::vector<std::string> types = {
		"type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable",
		"property", "enumMember", "function", "label", "keyword",
	};
	return types;
}

const std::vector<std::string> & Analysis::tokenModifiers() {
	static const std::vector<std::string> mods = { "declaration", "readonly", "defaultLibrary" };
	return mods;
}

} // namespace cfalsp
