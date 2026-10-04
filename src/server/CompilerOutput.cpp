#include "CompilerOutput.hpp"

#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace cfalsp {

std::string demangle( const std::string & name ) {
	// _X<length><name><type and scope>
	if ( name.size() < 3 || name[0] != '_' || name[1] != 'X' ) return name;
	size_t i = 2;
	size_t n = 0;
	while ( i < name.size() && std::isdigit( (unsigned char)name[i] ) ) {
		n = n * 10 + ( name[i] - '0' );
		i += 1;
	}
	if ( i == 2 || n == 0 || i + n > name.size() ) return name;
	return name.substr( i, n );
}

std::string demangleMessage( const std::string & msg ) {
	static const std::regex quoted( "('|\xE2\x80\x98)(_X[0-9]+[^'\xE2]*)('|\xE2\x80\x99)" );
	std::string out;
	auto begin = std::sregex_iterator( msg.begin(), msg.end(), quoted );
	size_t last = 0;
	for ( auto it = begin; it != std::sregex_iterator(); ++it ) {
		const auto & m = *it;
		out += msg.substr( last, m.position( 0 ) - last );
		out += "'" + demangle( m[2].str() ) + "'";
		last = m.position( 0 ) + m.length( 0 );
	}
	out += msg.substr( last );
	return out;
}

namespace {

const std::regex gccRe( R"(^([^:\s][^:]*):(\d+):(?:(\d+):)? (fatal error|error|warning|note): (.*)$)" );
const std::regex cfaRe( R"(^([^:\s][^:]*):(\d+):(\d+) (error|warning): (.*)$)" );
const std::regex toolRe( R"(^(cc1|cfa-cpp|cfa|gcc|as|ld|collect2|cpp): (fatal error|error|warning): (.*)$)" );
const std::regex noiseRe( R"(^(?:[^:]+: In (?:function|member function|instantiation).*:$|In file included from .*|\s+from .*|compilation terminated\.$|CFA Version .*)$)" );
const std::regex keepRe( R"((Name: |Variable Expression: |Alternatives are|Could not satisfy|Unsatisfiable alternative|with field|from aggregate|Cost \())" );
const std::regex optionRe( R"(^(.*) \[(-W[^\]]+)\]$)" );

void condense( RawDiag & d, const std::vector<std::string> & body ) {
	std::set<std::string> seen;
	for ( const auto & raw : body ) {
		size_t b = raw.find_first_not_of( " \t" );
		if ( b == std::string::npos ) continue;
		std::string line = raw.substr( b );
		if ( ! std::regex_search( line, keepRe ) ) continue;
		while ( ! line.empty() && ( line.back() == ':' || line.back() == ' ' ) ) line.pop_back();
		if ( ! seen.insert( line ).second ) continue;
		d.detail.push_back( line );
		if ( d.detail.size() >= 12 ) {
			d.detail.push_back( "..." );
			break;
		}
	}
}

} // namespace

std::vector<RawDiag> parseCompilerOutput( const std::string & text ) {
	std::vector<RawDiag> out;
	std::vector<std::string> body;
	bool inBody = false;
	auto flush = [&]() {
		if ( inBody && ! out.empty() ) condense( out.back(), body );
		body.clear();
		inBody = false;
	};
	std::istringstream in( text );
	std::string line;
	std::smatch m;
	while ( std::getline( in, line ) ) {
		if ( ! line.empty() && line.back() == '\r' ) line.pop_back();
		if ( std::regex_match( line, m, gccRe ) ) {
			flush();
			RawDiag d;
			d.file = m[1];
			d.line = std::stoi( m[2] );
			d.col = m[3].matched ? std::stoi( m[3] ) : 0;
			d.severity = m[4] == "fatal error" ? "error" : m[4].str();
			std::string msg = m[5];
			std::smatch om;
			if ( std::regex_match( msg, om, optionRe ) ) {
				d.option = om[2];
				msg = om[1];
			}
			d.message = demangleMessage( msg );
			out.push_back( std::move( d ) );
		} else if ( std::regex_match( line, m, cfaRe ) ) {
			flush();
			RawDiag d;
			d.file = m[1];
			d.line = std::stoi( m[2] );
			d.col = 0;						// the resolver's column is always 1
			d.severity = m[4];
			d.message = m[5];
			while ( ! d.message.empty() && d.message.back() == ' ' ) d.message.pop_back();
			out.push_back( std::move( d ) );
			inBody = true;
		} else if ( std::regex_match( line, m, toolRe ) ) {
			flush();
			RawDiag d;
			d.severity = m[2] == "fatal error" ? "error" : m[2].str();
			d.message = m[1].str() + ": " + m[3].str();
			out.push_back( std::move( d ) );
		} else if ( std::regex_match( line, noiseRe ) ) {
			flush();
		} else if ( inBody ) {
			body.push_back( line );
		}
	}
	flush();
	return out;
}

std::optional<std::pair<int, std::string>> parseLineMarker( const std::string & line ) {
	size_t i = 0;
	if ( line.empty() || line[0] != '#' ) return std::nullopt;
	i = 1;
	while ( i < line.size() && ( line[i] == ' ' || line[i] == '\t' ) ) i += 1;
	if ( line.compare( i, 4, "line" ) == 0 ) {
		i += 4;
		while ( i < line.size() && ( line[i] == ' ' || line[i] == '\t' ) ) i += 1;
	}
	if ( i >= line.size() || ! std::isdigit( (unsigned char)line[i] ) ) return std::nullopt;
	long n = 0;
	while ( i < line.size() && std::isdigit( (unsigned char)line[i] ) ) {
		n = n * 10 + ( line[i] - '0' );
		if ( n > 100000000 ) return std::nullopt;
		i += 1;
	}
	while ( i < line.size() && line[i] == ' ' ) i += 1;
	if ( i >= line.size() || line[i] != '"' ) return std::nullopt;
	i += 1;
	std::string file;
	while ( i < line.size() && line[i] != '"' ) {
		if ( line[i] == '\\' && i + 1 < line.size() ) i += 1;
		file += line[i];
		i += 1;
	}
	if ( i >= line.size() ) return std::nullopt;
	return std::make_pair( (int)n, file );
}

namespace {

// Calls f( markerLine1Based, lineNumberSetByMarker, file, flags ) for markers
// and g( line1Based, file, sourceLine ) for other lines.
template<typename OnMarker, typename OnLine>
void scanMarkers( const std::string & text, OnMarker onMarker, OnLine onLine ) {
	std::string cur;
	int curLine = 0;
	int n = 0;
	size_t pos = 0;
	while ( pos <= text.size() ) {
		size_t nl = text.find( '\n', pos );
		if ( nl == std::string::npos ) nl = text.size();
		std::string line = text.substr( pos, nl - pos );
		n += 1;
		auto mk = line.size() && line[0] == '#' ? parseLineMarker( line ) : std::nullopt;
		if ( mk ) {
			std::string flags;
			size_t q = line.rfind( '"' );
			if ( q != std::string::npos ) flags = line.substr( q + 1 );
			onMarker( n, cur, curLine, mk->second, mk->first, flags );
			cur = mk->second;
			curLine = mk->first;
		} else {
			onLine( n, cur, curLine );
			curLine += 1;
		}
		if ( nl == text.size() ) break;
		pos = nl + 1;
	}
}

} // namespace

LineMarkerMap::LineMarkerMap( const std::string & text ) {
	scanMarkers( text,
		[&]( int, const std::string &, int, const std::string &, int, const std::string & ) {
			lines.push_back( std::nullopt );
		},
		[&]( int, const std::string & file, int line ) {
			if ( file.empty() ) lines.push_back( std::nullopt );
			else lines.push_back( Entry{ file, line } );
		} );
}

std::optional<std::pair<std::string, int>> LineMarkerMap::lookup( int line ) const {
	if ( line < 1 || line > (int)lines.size() || ! lines[line - 1] ) return std::nullopt;
	return std::make_pair( lines[line - 1]->file, lines[line - 1]->line );
}

IncludeSites findIncludeSites( const std::string & preprocessed ) {
	IncludeSites sites;
	scanMarkers( preprocessed,
		[&]( int, const std::string & curFile, int curLine, const std::string & newFile, int, const std::string & flags ) {
			// Flag 1 means "entering a new file"; the line counter of the
			// file we're in is then at the #include line.
			std::istringstream fl( flags );
			int f;
			bool entering = false;
			while ( fl >> f ) {
				if ( f == 1 ) entering = true;
			}
			if ( entering && ! curFile.empty() && ! sites.count( newFile ) ) {
				sites[newFile] = { curFile, curLine };
			}
		},
		[]( int, const std::string &, int ) {} );
	return sites;
}

std::optional<int> includeLineInMain( const IncludeSites & sites, const std::string & file,
									  const std::string & mainFile ) {
	std::string f = file;
	for ( int depth = 0; depth < 200; depth += 1 ) {
		auto it = sites.find( f );
		if ( it == sites.end() ) return std::nullopt;
		if ( it->second.first == mainFile ) return it->second.second;
		f = it->second.first;
	}
	return std::nullopt;
}

} // namespace cfalsp
