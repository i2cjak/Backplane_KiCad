#include "assembly_model.h"

#include <fstream>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <cmath>
#include <limits>

namespace
{
std::array<double, 3> rotate( const std::array<double, 3>& p, const std::array<double, 3>& degrees )
{
    constexpr double d2r = 3.14159265358979323846 / 180.0;
    const double cx = std::cos( degrees[0] * d2r ), sx = std::sin( degrees[0] * d2r );
    const double cy = std::cos( degrees[1] * d2r ), sy = std::sin( degrees[1] * d2r );
    const double cz = std::cos( degrees[2] * d2r ), sz = std::sin( degrees[2] * d2r );
    const std::array<double, 3> x = { p[0], cx * p[1] - sx * p[2], sx * p[1] + cx * p[2] };
    const std::array<double, 3> y = { cy * x[0] + sy * x[2], x[1], -sy * x[0] + cy * x[2] };
    return { cz * y[0] - sz * y[1], sz * y[0] + cz * y[1], y[2] };
}
// Convert orthonormal matrix columns to Rz * Ry * Rx Euler angles.
std::array<double, 3> euler( const std::array<double, 3>& x,
                             const std::array<double, 3>& y,
                             const std::array<double, 3>& z )
{
    constexpr double r2d = 180.0 / 3.14159265358979323846;
    const double cy = std::hypot( x[0], x[1] );
    if( cy < 1e-10 )
        return { std::atan2( -z[1], y[1] ) * r2d,
                 std::atan2( -x[2], cy ) * r2d, 0.0 };
    return { std::atan2( y[2], z[2] ) * r2d,
             std::atan2( -x[2], cy ) * r2d,
             std::atan2( x[1], x[0] ) * r2d };
}

std::array<double, 3> composeRotation( const std::array<double, 3>& parent,
                                       const std::array<double, 3>& child )
{
    return euler( rotate( rotate( { 1, 0, 0 }, child ), parent ),
                  rotate( rotate( { 0, 1, 0 }, child ), parent ),
                  rotate( rotate( { 0, 0, 1 }, child ), parent ) );
}

double dot( const std::array<double, 3>& a, const std::array<double, 3>& b ) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
std::array<double, 3> cross( const std::array<double, 3>& a, const std::array<double, 3>& b )
{ return { a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0] }; }
double norm( const std::array<double, 3>& a ) { return std::sqrt( dot( a, a ) ); }
std::array<double, 3> unit( const std::array<double, 3>& a )
{ const double n = norm( a ); if( n <= 1e-12 ) throw std::invalid_argument( "mate axis must be nonzero" ); return { a[0]/n, a[1]/n, a[2]/n }; }
std::array<double, 3> axisAlign( const std::array<double, 3>& from, const std::array<double, 3>& to )
{
    const auto a = unit( from ), b = unit( to ); const auto v = cross( a, b );
    const double c = std::clamp( dot( a, b ), -1.0, 1.0 );
    double angle = std::acos( c ); std::array<double, 3> axis = v;
    if( norm( axis ) <= 1e-12 )
    {
        if( c > 0.0 ) return { 0.0, 0.0, 0.0 };
        axis = cross( a, std::abs( a[0] ) < 0.8 ? std::array<double,3>{1,0,0} : std::array<double,3>{0,1,0} );
        angle = 3.14159265358979323846;
    }
    axis = unit( axis );
    // Convert the Rodrigues rotation to the same XYZ Euler convention as rotate().
    const double s = std::sin( angle ), t = 1.0 - std::cos( angle );
    const double c0 = std::cos( angle );
    return euler( { t*axis[0]*axis[0]+c0, t*axis[0]*axis[1]+s*axis[2], t*axis[0]*axis[2]-s*axis[1] },
                  { t*axis[0]*axis[1]-s*axis[2], t*axis[1]*axis[1]+c0, t*axis[1]*axis[2]+s*axis[0] },
                  { t*axis[0]*axis[2]+s*axis[1], t*axis[1]*axis[2]-s*axis[0], t*axis[2]*axis[2]+c0 } );
}
ASSEMBLY_BOUNDS transformedBounds( const ASSEMBLY_INSTANCE& i )
{
    ASSEMBLY_BOUNDS out; out.valid = i.bounds.valid;
    if( !out.valid ) return out;
    out.minimum = { 1e300, 1e300, 1e300 }; out.maximum = { -1e300, -1e300, -1e300 };
    for( int mask = 0; mask < 8; ++mask )
    {
        std::array<double, 3> p;
        for( size_t k = 0; k < 3; ++k ) p[k] = ( mask & ( 1 << k ) ) ? i.bounds.maximum[k] : i.bounds.minimum[k];
        p = rotate( p, i.transform.rotation );
        for( size_t k = 0; k < 3; ++k ) { p[k] += i.transform.translation[k]; out.minimum[k] = std::min( out.minimum[k], p[k] ); out.maximum[k] = std::max( out.maximum[k], p[k] ); }
    }
    return out;
}
template<typename T> void readArray( const nlohmann::json& j, const char* key, std::array<T, 3>& out )
{
    if( j.contains( key ) )
    {
        const auto& a = j.at( key );
        if( !a.is_array() || a.size() != 3 )
            throw std::invalid_argument( std::string( key ) + " must contain three values" );
        for( size_t i = 0; i < 3; ++i ) out[i] = a.at( i ).get<T>();
    }
}
template<typename T> nlohmann::json arrayJson( const std::array<T, 3>& a )
{
    return nlohmann::json::array( { a[0], a[1], a[2] } );
}
}

ASSEMBLY_MANIFEST ASSEMBLY_MANIFEST::FromJson( const nlohmann::json& j )
{
    if( !j.is_object() ) throw std::invalid_argument( "assembly manifest must be an object" );
    ASSEMBLY_MANIFEST m;
    m.schema = j.value( "schema", 1 );
    m.name = j.value( "name", "" );
    m.anchor = j.value( "anchor", "" );
    m.electrical = j.value( "electrical", nlohmann::json::object() );
    if( m.schema != 1 ) throw std::invalid_argument( "unsupported assembly schema" );
    m.extra_json = j;
    for( const char* key : { "schema", "name", "anchor", "instances", "mates", "electrical" } ) m.extra_json.erase( key );
    for( const auto& x : j.value( "instances", nlohmann::json::array() ) )
    {
        ASSEMBLY_INSTANCE i;
        i.id = x.at( "id" ).get<std::string>();
        i.source = x.at( "source" ).get<std::string>();
        i.kind = x.value( "kind", "board" );
        readArray( x.value( "transform", nlohmann::json::object() ), "translation", i.transform.translation );
        readArray( x.value( "transform", nlohmann::json::object() ), "rotation", i.transform.rotation );
        if( x.contains( "bounds" ) )
        {
            readArray( x.at( "bounds" ), "minimum", i.bounds.minimum );
            readArray( x.at( "bounds" ), "maximum", i.bounds.maximum );
            i.bounds.valid = true;
        }
        i.connector_pins = x.value( "connector_pins", std::map<std::string, std::string>() );
        i.folded = x.value( "folded", false );
        i.extra_json = x;
        for( const char* key : { "id", "source", "kind", "transform", "bounds", "connector_pins", "folded" } ) i.extra_json.erase( key );
        m.instances.push_back( std::move( i ) );
    }
    for( const auto& x : j.value( "mates", nlohmann::json::array() ) )
    {
        ASSEMBLY_MATE mate;
        mate.id = x.at( "id" ).get<std::string>();
        mate.type = x.value( "type", "point" );
        mate.first_instance = x.at( "first_instance" ).get<std::string>();
        mate.second_instance = x.at( "second_instance" ).get<std::string>();
        readArray( x, "first", mate.first );
        readArray( x, "second", mate.second );
        mate.clearance_mm = x.value( "clearance_mm", 0.0 );
        readArray( x, "first_axis", mate.first_axis ); readArray( x, "second_axis", mate.second_axis );
        mate.opposing = x.value( "opposing", false );
        mate.extra_json = x;
        for( const char* key : { "id", "type", "first_instance", "second_instance", "first", "second", "clearance_mm", "first_axis", "second_axis", "opposing" } ) mate.extra_json.erase( key );
        m.mates.push_back( std::move( mate ) );
    }
    return m;
}

nlohmann::json ASSEMBLY_MANIFEST::ToJson() const
{
    nlohmann::json j = extra_json;
    j["schema"] = schema; j["name"] = name; j["anchor"] = anchor;
    j["electrical"] = electrical;
    j["instances"] = nlohmann::json::array(); j["mates"] = nlohmann::json::array();
    for( const auto& i : instances )
    {
        std::string source = i.source;
        if( !base_directory.empty() && std::filesystem::path( source ).is_absolute() )
        {
            std::error_code ec; auto rel = std::filesystem::relative( source, base_directory, ec );
            if( !ec ) source = rel.string();
        }
        nlohmann::json x = i.extra_json;
        x["id"] = i.id; x["source"] = source; x["kind"] = i.kind;
        x["transform"] =
                             { { "translation", arrayJson( i.transform.translation ) },
                                               { "rotation", arrayJson( i.transform.rotation ) } };
        x["connector_pins"] = i.connector_pins; x["folded"] = i.folded;
        if( i.bounds.valid ) x["bounds"] = { { "minimum", arrayJson( i.bounds.minimum ) },
                                              { "maximum", arrayJson( i.bounds.maximum ) } };
        j["instances"].push_back( std::move( x ) );
    }
    for( const auto& m : mates )
    {
        nlohmann::json x = m.extra_json;
        x.update( { { "id", m.id }, { "type", m.type },
                                 { "first_instance", m.first_instance }, { "second_instance", m.second_instance },
                                 { "first", arrayJson( m.first ) }, { "second", arrayJson( m.second ) },
                                 { "clearance_mm", m.clearance_mm }, { "first_axis", arrayJson( m.first_axis ) },
                                 { "second_axis", arrayJson( m.second_axis ) }, { "opposing", m.opposing } } );
        j["mates"].push_back( std::move( x ) );
    }
    return j;
}

ASSEMBLY_MANIFEST ASSEMBLY_MANIFEST::Load( const std::filesystem::path& p )
{
    std::ifstream in( p );
    if( !in ) throw std::runtime_error( "cannot open assembly manifest: " + p.string() );
    auto result = FromJson( nlohmann::json::parse( in ) ); result.base_directory = std::filesystem::absolute( p ).parent_path();
    return result;
}

void ASSEMBLY_MANIFEST::Save( const std::filesystem::path& p ) const
{
    const auto errors = Validate();
    if( !errors.empty() ) throw std::invalid_argument( errors.front() );
    ASSEMBLY_MANIFEST saved = *this;
    for( auto& instance : saved.instances )
        instance.source = std::filesystem::absolute( base_directory / instance.source ).string();
    saved.base_directory = std::filesystem::absolute( p ).parent_path();
    const auto staging = std::filesystem::path( p.string() + ".tmp" );
    try
    {
        std::ofstream out( staging );
        if( !out ) throw std::runtime_error( "cannot write assembly manifest: " + p.string() );
        out << saved.ToJson().dump( 2 ) << '\n';
        out.close();
        if( !out ) throw std::runtime_error( "cannot finish writing assembly manifest: " + p.string() );
        std::filesystem::rename( staging, p );
    }
    catch( ... )
    {
        std::error_code ec;
        std::filesystem::remove( staging, ec );
        throw;
    }
}

std::vector<std::string> ASSEMBLY_MANIFEST::Validate( bool requireSources ) const
{
    std::vector<std::string> errors;
    if( schema != 1 ) errors.push_back( "unsupported assembly schema" );
    std::set<std::string> ids;
    for( const auto& i : instances )
    {
        if( i.id.empty() || !ids.insert( i.id ).second ) errors.push_back( "duplicate or empty instance id: " + i.id );
        if( i.source.empty() ) errors.push_back( "instance has no source: " + i.id );
        if( i.kind != "board" && i.kind != "part" && i.kind != "assembly" ) errors.push_back( "invalid instance kind: " + i.id );
        if( requireSources && !std::filesystem::exists( base_directory / i.source ) && !std::filesystem::exists( i.source ) ) errors.push_back( "missing source: " + i.source );
        for( double v : i.transform.translation ) if( !std::isfinite( v ) ) errors.push_back( "nonfinite translation: " + i.id );
        for( double v : i.transform.rotation ) if( !std::isfinite( v ) ) errors.push_back( "nonfinite rotation: " + i.id );
        for( const auto& pin : i.connector_pins )
            if( pin.first.empty() || pin.second.empty() ) errors.push_back( "empty connector pin mapping: " + i.id );
        if( i.bounds.valid ) for( size_t k = 0; k < 3; ++k )
            if( i.bounds.minimum[k] > i.bounds.maximum[k] ) errors.push_back( "invalid bounds: " + i.id );
    }
    if( !anchor.empty() && !ids.count( anchor ) ) errors.push_back( "anchor instance does not exist: " + anchor );
    std::set<std::string> mateIds;
    for( const auto& m : mates )
    {
        if( m.id.empty() || !mateIds.insert( m.id ).second ) errors.push_back( "duplicate or empty mate id: " + m.id );
        if( !ids.count( m.first_instance ) || !ids.count( m.second_instance ) ) errors.push_back( "mate references missing instance: " + m.id );
        if( m.type != "point" && m.type != "axis" ) errors.push_back( "invalid mate type: " + m.id );
        if( m.type == "axis" )
        {
            if( !std::isfinite( norm( m.first_axis ) ) || !std::isfinite( norm( m.second_axis ) )
                || norm( m.first_axis ) <= 1e-12 || norm( m.second_axis ) <= 1e-12 )
                errors.push_back( "axis mate has a zero axis: " + m.id );
        }
        if( !std::isfinite( m.clearance_mm ) || m.clearance_mm < 0 ) errors.push_back( "invalid mate clearance: " + m.id );
        for( double value : m.first ) if( !std::isfinite( value ) ) errors.push_back( "nonfinite mate point: " + m.id );
        for( double value : m.second ) if( !std::isfinite( value ) ) errors.push_back( "nonfinite mate point: " + m.id );
        if( m.first_instance == m.second_instance ) errors.push_back( "mate cannot reference the same instance twice: " + m.id );
        if( m.second_instance == anchor ) errors.push_back( "mate cannot move the anchored instance: " + m.id );
    }
    return errors;
}

std::vector<ASSEMBLY_COLLISION> CheckAssemblyBounds( const ASSEMBLY_MANIFEST& m, double clearance )
{
    std::vector<ASSEMBLY_COLLISION> result;
    for( size_t a = 0; a < m.instances.size(); ++a ) for( size_t b = a + 1; b < m.instances.size(); ++b )
    {
        const auto& x = m.instances[a]; const auto& y = m.instances[b];
        const auto xb = transformedBounds( x ); const auto yb = transformedBounds( y );
        if( !xb.valid || !yb.valid ) continue;
        bool overlap = true; double gap = 0.0;
        for( size_t k = 0; k < 3; ++k )
        {
            const double xmin = xb.minimum[k]; const double xmax = xb.maximum[k];
            const double ymin = yb.minimum[k]; const double ymax = yb.maximum[k];
            const double axisGap = std::max( { ymin - xmax, xmin - ymax, 0.0 } );
            if( axisGap > clearance ) overlap = false;
            gap = std::max( gap, axisGap );
        }
        if( overlap ) result.push_back( { x.id, y.id, gap } );
    }
    return result;
}

bool ApplyAssemblyMates( ASSEMBLY_MANIFEST& m )
{
    if( !m.Validate().empty() ) return false;
    auto find = [&]( const std::string& id ) -> ASSEMBLY_INSTANCE* { for( auto& i : m.instances ) if( i.id == id ) return &i; return nullptr; };
    std::set<std::string> pending;
    for( const auto& mate : m.mates )
        if( !pending.insert( mate.second_instance ).second ) return false;
    std::set<std::string> applied;
    while( applied.size() < m.mates.size() )
    {
        bool progress = false;
        for( const auto& mate : m.mates )
        {
            if( applied.contains( mate.id ) || pending.contains( mate.first_instance ) ) continue;
            ASSEMBLY_INSTANCE* first = find( mate.first_instance );
            ASSEMBLY_INSTANCE* second = find( mate.second_instance );
            if( !first || !second ) return false;
            if( mate.type == "axis" )
            {
                auto target = unit( rotate( mate.first_axis, first->transform.rotation ) ); if( mate.opposing ) for( double& v : target ) v = -v;
                const auto local = axisAlign( rotate( mate.second_axis, second->transform.rotation ), target );
                second->transform.rotation = composeRotation( local, second->transform.rotation );
            }
            const auto a = rotate( mate.first, first->transform.rotation );
            const auto b = rotate( mate.second, second->transform.rotation );
            for( size_t k = 0; k < 3; ++k )
                second->transform.translation[k] = first->transform.translation[k] + a[k] - b[k];
            applied.insert( mate.id );
            pending.erase( mate.second_instance );
            progress = true;
        }
        if( !progress ) return false;
    }
    return true;
}

namespace
{
void flattenInto( const ASSEMBLY_MANIFEST& source, const std::string& prefix,
                  const ASSEMBLY_TRANSFORM& parentTransform, ASSEMBLY_MANIFEST& out,
                  std::set<std::filesystem::path>& active, unsigned depth )
{
    if( depth > 32 ) throw std::invalid_argument( "nested assembly depth exceeds 32" );
    ASSEMBLY_MANIFEST working = source;
    if( !ApplyAssemblyMates( working ) ) throw std::invalid_argument( "nested assembly mates could not be applied" );
    for( const auto& child : working.instances )
    {
        ASSEMBLY_TRANSFORM combined;
        const auto moved = rotate( child.transform.translation, parentTransform.rotation );
        for( size_t k = 0; k < 3; ++k )
        {
            combined.translation[k] = parentTransform.translation[k] + moved[k];

        }
        combined.rotation = composeRotation( parentTransform.rotation, child.transform.rotation );
        const std::string id = prefix.empty() ? child.id : prefix + "/" + child.id;
        if( child.kind == "assembly" )
        {
            const auto childPath = std::filesystem::weakly_canonical( std::filesystem::absolute( working.base_directory / child.source ) );
            if( !active.insert( childPath ).second ) throw std::invalid_argument( "nested assembly cycle at: " + child.source );
            ASSEMBLY_MANIFEST nested = ASSEMBLY_MANIFEST::Load( childPath );
            flattenInto( nested, id, combined, out, active, depth + 1 );
            active.erase( childPath );
            continue;
        }
        ASSEMBLY_INSTANCE leaf = child;
        leaf.id = id; leaf.source = std::filesystem::weakly_canonical( std::filesystem::absolute( working.base_directory / child.source ) ).string();
        leaf.transform = combined;
        out.instances.push_back( std::move( leaf ) );
    }
    // Parent-level clearance requirements apply to every descendant pair.
    // Keep these separate from mates: flattening has already solved transforms.
    for( const auto& mate : working.mates )
    {
        if( mate.clearance_mm <= 0.0 ) continue;
        const std::string first = prefix.empty() ? mate.first_instance : prefix + "/" + mate.first_instance;
        const std::string second = prefix.empty() ? mate.second_instance : prefix + "/" + mate.second_instance;
        auto descendant = []( const std::string& id, const std::string& ancestor )
        { return id == ancestor || id.starts_with( ancestor + "/" ); };
        for( const auto& a : out.instances )
            if( descendant( a.id, first ) )
                for( const auto& b : out.instances )
                    if( descendant( b.id, second ) )
                        out.resolved_clearances.push_back( { a.id, b.id, mate.clearance_mm } );
    }
}
}

ASSEMBLY_MANIFEST FlattenAssembly( const ASSEMBLY_MANIFEST& aManifest )
{
    if( aManifest.flattened ) return aManifest;
    ASSEMBLY_MANIFEST result = aManifest;
    result.resolved_clearances.clear();
    result.instances.clear(); result.mates.clear(); result.anchor.clear();
    result.base_directory.clear();
    std::set<std::filesystem::path> active;
    flattenInto( aManifest, "", ASSEMBLY_TRANSFORM{}, result, active, 0 );
    result.flattened = true;
    return result;
}
