#include <board_structure.h>
#include <geometry/shape_poly_set.h>
#include <layer_ids.h>
#include <backplane_rigid_flex_geometry.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace
{
template <typename T> bool duplicateIds( const std::vector<T>& aItems, std::vector<std::string>& aErrors,
                                         const char* aKind )
{
    std::set<std::string> ids;
    bool ok = true;
    for( const T& item : aItems )
    {
        if( item.id.empty() )
        {
            aErrors.emplace_back( std::string( aKind ) + " has an empty id" );
            ok = false;
        }
        else if( !ids.insert( item.id ).second )
        {
            aErrors.emplace_back( std::string( "duplicate " ) + aKind + " id: " + item.id );
            ok = false;
        }
    }
    return ok;
}

SHAPE_POLY_SET polySet( const BOARD_STRUCTURE_POLYGON& polygon )
{
    SHAPE_POLY_SET result;
    if( polygon.outline.size() < 3 ) return result;
    SHAPE_LINE_CHAIN outline;
    for( const auto& point : polygon.outline ) outline.Append( point );
    outline.SetClosed( true );
    result.AddOutline( outline );
    for( const auto& hole : polygon.holes )
    {
        SHAPE_LINE_CHAIN ring;
        for( const auto& point : hole ) ring.Append( point );
        ring.SetClosed( true );
        result.AddHole( ring, 0 );
    }
    return result;
}

bool contains( const BOARD_STRUCTURE_POLYGON& polygon, const VECTOR2I& point )
{
    return polySet( polygon ).Contains( point );
}

bool polygonsIntersect( const BOARD_STRUCTURE_POLYGON& a, const BOARD_STRUCTURE_POLYGON& b )
{
    SHAPE_POLY_SET result = polySet( a );
    result.BooleanIntersection( polySet( b ) );
    return !result.IsEmpty() && std::abs( result.Area() ) > 1.0;
}

bool pointOnChain( const SHAPE_LINE_CHAIN& chain, const VECTOR2I& p )
{
    for( int i = 0; i < chain.PointCount(); ++i )
    {
        const VECTOR2I a = chain.CPoint( i );
        const VECTOR2I b = chain.CPoint( ( i + 1 ) % chain.PointCount() );
        const int64_t dx = int64_t( b.x ) - a.x, dy = int64_t( b.y ) - a.y;
        const int64_t px = int64_t( p.x ) - a.x, py = int64_t( p.y ) - a.y;
        if( dx * py - dy * px == 0 && px * dx + py * dy >= 0
            && px * ( p.x - b.x ) + py * ( p.y - b.y ) <= 0 )
            return true;
    }
    return false;
}

bool pointOnBoundary( const SHAPE_POLY_SET& set, const VECTOR2I& p )
{
    for( int i = 0; i < set.OutlineCount(); ++i )
    {
        if( pointOnChain( set.COutline( i ), p ) ) return true;
        for( int h = 0; h < set.HoleCount( i ); ++h )
            if( pointOnChain( set.CHole( i, h ), p ) ) return true;
    }
    return false;
}

bool insertOnContour( std::vector<VECTOR2I> contour, const VECTOR2I& point,
                      std::vector<VECTOR2I>& result, int& index )
{
    for( size_t i = 0; i < contour.size(); ++i )
        if( contour[i] == point ) { result = std::move( contour ); index = int( i ); return true; }
    for( size_t i = 0; i < contour.size(); ++i )
    {
        const VECTOR2I a = contour[i], b = contour[( i + 1 ) % contour.size()];
        const int64_t dx = int64_t( b.x ) - a.x, dy = int64_t( b.y ) - a.y;
        const int64_t px = int64_t( point.x ) - a.x, py = int64_t( point.y ) - a.y;
        if( dx * py - dy * px == 0 && px * dx + py * dy >= 0
            && px * ( point.x - b.x ) + py * ( point.y - b.y ) <= 0 )
        {
            result = std::move( contour );
            result.insert( result.begin() + i + 1, point );
            index = int( i + 1 );
            return true;
        }
    }
    return false;
}

bool splitPiece( const SHAPE_POLY_SET& piece, const std::vector<VECTOR2I>& cut,
                 std::vector<SHAPE_POLY_SET>& output )
{
    const VECTOR2I a = cut.front(), b = cut.back();
    if( piece.OutlineCount() != 1 || !pointOnChain( piece.COutline( 0 ), a )
        || !pointOnChain( piece.COutline( 0 ), b ) )
        return false;
    std::vector<VECTOR2I> contour, withB;
    int ia = -1, ib = -1;
    for( const auto& p : piece.COutline( 0 ).CPoints() ) contour.push_back( p );
    if( !insertOnContour( contour, a, withB, ia ) || !insertOnContour( withB, b, contour, ib ) ) return false;
    ia = -1; ib = -1;
    for( size_t i = 0; i < contour.size(); ++i )
        if( contour[i] == a ) ia = int( i ); else if( contour[i] == b ) ib = int( i );
    if( ia < 0 || ib < 0 || ia == ib ) return false;
    auto path = [&]( int from, int to )
    {
        std::vector<VECTOR2I> p;
        for( int i = from;; i = ( i + 1 ) % int( contour.size() ) ) { p.push_back( contour[i] ); if( i == to ) break; }
        return p;
    };
    std::vector<VECTOR2I> rev( cut.rbegin(), cut.rend() );
    auto make = [&]( std::vector<VECTOR2I> boundary, const std::vector<VECTOR2I>& closing )
    {
        boundary.insert( boundary.end(), closing.begin() + 1, closing.end() );
        SHAPE_LINE_CHAIN ring( boundary, true );
        SHAPE_POLY_SET result; result.AddOutline( ring );
        result.BooleanIntersection( piece );
        return result;
    };
    SHAPE_POLY_SET first = make( path( ia, ib ), rev );
    SHAPE_POLY_SET second = make( path( ib, ia ), cut );
    if( first.IsEmpty() || second.IsEmpty() || std::abs( first.Area() ) <= 1.0 || std::abs( second.Area() ) <= 1.0 ) return false;
    output.push_back( std::move( first ) ); output.push_back( std::move( second ) );
    return true;
}

BOARD_STRUCTURE_POLYGON toPolygon( const SHAPE_POLY_SET& set, int index )
{
    BOARD_STRUCTURE_POLYGON p;
    for( const VECTOR2I& v : set.COutline( index ).CPoints() ) p.outline.push_back( v );
    for( int h = 0; h < set.HoleCount( index ); ++h )
    {
        std::vector<VECTOR2I> hole;
        for( const VECTOR2I& v : set.CHole( index, h ).CPoints() ) hole.push_back( v );
        p.holes.push_back( std::move( hole ) );
    }
    return p;
}

VECTOR2I interiorPoint( const BOARD_STRUCTURE_POLYGON& polygon )
{
    int minX = polygon.outline.front().x, maxX = minX;
    int minY = polygon.outline.front().y, maxY = minY;
    int64_t sx = 0, sy = 0;
    for( const auto& p : polygon.outline ) minX = std::min( minX, p.x ), maxX = std::max( maxX, p.x ), minY = std::min( minY, p.y ), maxY = std::max( maxY, p.y ), sx += p.x, sy += p.y;
    VECTOR2I candidate( KiROUND( double( sx ) / polygon.outline.size() ), KiROUND( double( sy ) / polygon.outline.size() ) );
    if( contains( polygon, candidate ) && !pointOnBoundary( polySet( polygon ), candidate ) ) return candidate;
    for( int y = minY; y <= maxY; y += std::max( 1, ( maxY - minY ) / 32 ) )
        for( int x = minX; x <= maxX; x += std::max( 1, ( maxX - minX ) / 32 ) )
            if( contains( polygon, VECTOR2I( x, y ) )
                && !pointOnBoundary( polySet( polygon ), VECTOR2I( x, y ) ) )
                return VECTOR2I( x, y );
    return polygon.outline.front();
}

double bendThicknessMm( const BOARD_STRUCTURE& structure, const BOARD_STRUCTURE_BEND& bend )
{
    for( const auto& region : structure.regions )
        if( region.id == bend.region_id )
            if( const auto* stack = structure.StackForRegion( region ) )
            {
                double result = 0.0;
                for( const auto& material : stack->materials )
                    result += material.thickness_nm * 1e-6;
                if( result > 0.0 )
                    return result;
            }
    return 0.0;
}

double bendWidthIu( const BOARD_STRUCTURE& structure, const BOARD_STRUCTURE_BEND& bend )
{
    return BOARD_STRUCTURE::BendStripLengthMm( bend, bendThicknessMm( structure, bend ) ) * 1e6;
}

BOARD_STRUCTURE_POLYGON bendStrip( const BOARD_STRUCTURE& structure,
                                   const BOARD_STRUCTURE_BEND& bend )
{
    const double dx = static_cast<double>( bend.end.x - bend.start.x );
    const double dy = static_cast<double>( bend.end.y - bend.start.y );
    const double d = std::hypot( dx, dy );
    const double width = bendWidthIu( structure, bend );
    const double nx = -dy / d * ( bend.moving_side_left ? 1.0 : -1.0 );
    const double ny = dx / d * ( bend.moving_side_left ? 1.0 : -1.0 );
    BOARD_STRUCTURE_POLYGON result;
    result.outline = { bend.start, bend.end,
                       VECTOR2I( KiROUND( bend.end.x + nx * width ),
                                 KiROUND( bend.end.y + ny * width ) ),
                       VECTOR2I( KiROUND( bend.start.x + nx * width ),
                                 KiROUND( bend.start.y + ny * width ) ) };
    return result;
}

// Segment/strip contact is checked separately from positive-area strip
// overlap.  A later hinge crossing an earlier curved strip has no valid rigid
// tangent frame, even when the two zero-width hinge lines only touch.
bool segmentIntersectsBendStrip( const BOARD_STRUCTURE_BEND& segment,
                                 const BOARD_STRUCTURE_BEND& strip, double width )
{
    const double sdx = static_cast<double>( strip.end.x - strip.start.x );
    const double sdy = static_cast<double>( strip.end.y - strip.start.y );
    const double length = std::hypot( sdx, sdy );
    const double nx = -sdy / length * ( strip.moving_side_left ? 1.0 : -1.0 );
    const double ny = sdx / length * ( strip.moving_side_left ? 1.0 : -1.0 );
    auto coords = [&]( const VECTOR2I& p )
    {
        const double dx = p.x - strip.start.x;
        const double dy = p.y - strip.start.y;
        return VECTOR2D( ( dx * sdx + dy * sdy ) / length,
                         dx * nx + dy * ny );
    };
    const VECTOR2D a = coords( segment.start );
    const VECTOR2D b = coords( segment.end );
    const double minAlong = -1e-7;
    const double maxAlong = length + 1e-7;
    const double minSide = -1e-7;
    const double maxSide = width + 1e-7;
    double t0 = 0.0;
    double t1 = 1.0;
    const double da = b.x - a.x;
    const double ds = b.y - a.y;
    auto clip = [&]( double p, double q )
    {
        if( std::abs( p ) < 1e-15 )
            return q >= 0.0;
        const double r = q / p;
        if( p < 0.0 )
            t0 = std::max( t0, r );
        else
            t1 = std::min( t1, r );
        return t0 <= t1;
    };
    return clip( -da, a.x - minAlong ) && clip( da, maxAlong - a.x )
        && clip( -ds, a.y - minSide ) && clip( ds, maxSide - a.y );
}

// New known values replace old values; unknown object fields survive. Arrays
// of identified objects follow IDs so deleting/reordering does not resurrect data.
nlohmann::json mergeUnknown( const nlohmann::json& old, nlohmann::json value )
{
    if( old.is_object() && value.is_object() )
    {
        for( auto it = old.begin(); it != old.end(); ++it )
        {
            if( !value.contains( it.key() ) ) value[it.key()] = it.value();
            else value[it.key()] = mergeUnknown( it.value(), value[it.key()] );
        }
    }
    else if( old.is_array() && value.is_array() )
    {
        for( auto& item : value )
        {
            if( !item.is_object() ) continue;
            const char* key = item.contains( "id" ) ? "id" : "material_id";
            if( !item.contains( key ) ) continue;
            for( const auto& previous : old )
                if( previous.is_object() && previous.value( key, nlohmann::json() ) == item[key] )
                { item = mergeUnknown( previous, item ); break; }
        }
    }
    return value;
}

template <typename T> nlohmann::json point( const T& p ) { return { p.x, p.y }; }
VECTOR2I readPoint( const nlohmann::json& j )
{
    if( !j.is_array() || j.size() != 2 )
        throw std::invalid_argument( "point must be [x,y]" );
    for( const auto& coordinate : j )
        if( !coordinate.is_number_integer() || coordinate.get<int64_t>() < std::numeric_limits<int>::min()
            || coordinate.get<int64_t>() > std::numeric_limits<int>::max() )
            throw std::invalid_argument( "coordinate must be an integer in the board coordinate range" );
    return VECTOR2I( j[0].get<int>(), j[1].get<int>() );
}
BOARD_STRUCTURE_POLYGON readPoly( const nlohmann::json& j )
{
    BOARD_STRUCTURE_POLYGON p;
    for( const auto& v : j.value( "outline", nlohmann::json::array() ) ) p.outline.push_back( readPoint( v ) );
    for( const auto& h : j.value( "holes", nlohmann::json::array() ) )
    {
        std::vector<VECTOR2I> ring;
        for( const auto& v : h ) ring.push_back( readPoint( v ) );
        p.holes.push_back( std::move( ring ) );
    }
    return p;
}
nlohmann::json writePoly( const BOARD_STRUCTURE_POLYGON& p )
{
    nlohmann::json j = { { "outline", nlohmann::json::array() }, { "holes", nlohmann::json::array() } };
    for( const auto& v : p.outline ) j["outline"].push_back( point( v ) );
    for( const auto& h : p.holes ) { nlohmann::json r = nlohmann::json::array(); for( const auto& v : h ) r.push_back( point( v ) ); j["holes"].push_back( r ); }
    return j;
}
}

void BOARD_STRUCTURE::Clear() { stacks.clear(); regions.clear(); boundaries.clear(); region_seeds.clear(); stiffeners.clear(); coverlays.clear(); bends.clear(); unknown = nlohmann::json::object(); m_original = nlohmann::json::object(); }
bool BOARD_STRUCTURE::Empty() const { return stacks.empty() && regions.empty() && boundaries.empty() && region_seeds.empty() && stiffeners.empty() && coverlays.empty() && bends.empty() && unknown.empty(); }

nlohmann::json BOARD_STRUCTURE::ToJson() const
{
    nlohmann::json j = unknown.is_object() ? unknown : nlohmann::json::object();
    j["schema_version"] = SCHEMA_VERSION;
    j["stacks"] = nlohmann::json::array();
    for( const auto& s : stacks )
    {
        nlohmann::json x = {{"id",s.id},{"name",s.name},{"kind",s.kind==BOARD_STRUCTURE_STACK_KIND::FLEX?"flex":"rigid"},{"copper_layers",s.copper_layers},{"continuity_id",s.continuity_id},{"materials",nlohmann::json::array()}};
        for( const auto& m : s.materials )
            x["materials"].push_back({{"id",m.id},{"category",m.category},{"material",m.material},{"thickness_nm",m.thickness_nm},{"min_thickness_nm",m.min_thickness_nm},{"max_thickness_nm",m.max_thickness_nm}});
        x["layers"] = nlohmann::json::array();
        for( const auto& l : s.layers )
            x["layers"].push_back({{"material_id",l.material_id},{"global_copper_layer",l.global_copper_layer},{"z_offset_nm",l.z_offset_nm}});
        j["stacks"].push_back( x );
    }
    j["regions"] = nlohmann::json::array(); for( const auto& r : regions ) j["regions"].push_back({{"id",r.id},{"name",r.name},{"stack_id",r.stack_id},{"shape",writePoly(r.shape)}});
    j["boundaries"] = nlohmann::json::array(); for( const auto& b : boundaries ) { nlohmann::json x={{"id",b.id},{"polyline",nlohmann::json::array()}}; for( const auto& p : b.polyline ) x["polyline"].push_back(point(p)); j["boundaries"].push_back(x); }
    j["region_seeds"] = nlohmann::json::array(); for( const auto& s : region_seeds ) j["region_seeds"].push_back({{"region_id",s.region_id},{"point",point(s.point)}});
    j["stiffeners"] = nlohmann::json::array(); for( const auto& s : stiffeners ) j["stiffeners"].push_back({{"id",s.id},{"region_id",s.region_id},{"shape",writePoly(s.shape)},{"material",s.material},{"adhesive",s.adhesive},{"thickness_nm",s.thickness_nm},{"min_thickness_nm",s.min_thickness_nm},{"max_thickness_nm",s.max_thickness_nm},{"adhesive_thickness_nm",s.adhesive_thickness_nm},{"top",s.top},{"role",s.role},{"support_targets",s.support_targets}});
    j["coverlays"] = nlohmann::json::array(); for( const auto& c : coverlays ) { nlohmann::json x={{"id",c.id},{"region_id",c.region_id},{"coverage",writePoly(c.coverage)},{"material",c.material},{"adhesive",c.adhesive},{"thickness_nm",c.thickness_nm},{"adhesive_thickness_nm",c.adhesive_thickness_nm},{"top",c.top},{"registration_tolerance_nm",c.registration_tolerance_nm},{"openings",nlohmann::json::array()}}; for(const auto&o:c.openings)x["openings"].push_back(writePoly(o)); j["coverlays"].push_back(x); }
    j["bends"] = nlohmann::json::array(); for( const auto& b : bends ) j["bends"].push_back({{"id",b.id},{"region_id",b.region_id},{"start",point(b.start)},{"end",point(b.end)},{"angle_degrees",b.angle_degrees},{"inner_radius_mm",b.inner_radius_mm},{"neutral_axis_offset_mm",b.neutral_axis_offset_mm},{"moving_side_left",b.moving_side_left},{"sequence",b.sequence}});
    return mergeUnknown( m_original, j );
}

void BOARD_STRUCTURE::FromJson( const nlohmann::json& j )
{
    BOARD_STRUCTURE parsed;
    parsed.ParseJson( j );
    *this = std::move( parsed );
}

void BOARD_STRUCTURE::ParseJson( const nlohmann::json& j )
{
    if( !j.is_object() || j.value("schema_version",0) != SCHEMA_VERSION ) throw std::invalid_argument("unsupported board_structure schema_version");
    for( const char* key : {"stacks","regions","boundaries","region_seeds","stiffeners","coverlays","bends"} )
        if( j.contains( key ) && !j.at( key ).is_array() )
            throw std::invalid_argument( std::string( key ) + " must be an array" );
    Clear(); m_original = j; unknown = j;
    for( const char* key : {"schema_version","stacks","regions","boundaries","region_seeds","stiffeners","coverlays","bends"} ) unknown.erase(key);
    for( const auto& x : j.value("stacks",nlohmann::json::array()) ) { BOARD_STRUCTURE_STACK s; if( x.value("kind", "rigid") != "rigid" && x.value("kind", "rigid") != "flex" ) throw std::invalid_argument("invalid stack kind"); s.id=x.value("id",""); s.name=x.value("name",""); s.kind=x.value("kind", "rigid")=="flex"?BOARD_STRUCTURE_STACK_KIND::FLEX:BOARD_STRUCTURE_STACK_KIND::RIGID; s.copper_layers=x.value("copper_layers",std::vector<int>{}); s.continuity_id=x.value("continuity_id",""); for(const auto&m:x.value("materials",nlohmann::json::array())){BOARD_STRUCTURE_MATERIAL q;q.id=m.value("id","");q.category=m.value("category","");q.material=m.value("material","");q.thickness_nm=m.value("thickness_nm",0LL);q.min_thickness_nm=m.value("min_thickness_nm",0LL);q.max_thickness_nm=m.value("max_thickness_nm",0LL);s.materials.push_back(q);} for(const auto&l:x.value("layers",nlohmann::json::array())){BOARD_STRUCTURE_STACK::LAYER_INSTANCE q;q.material_id=l.value("material_id","");q.global_copper_layer=l.value("global_copper_layer",-1);q.z_offset_nm=l.value("z_offset_nm",0LL);s.layers.push_back(q);} stacks.push_back(s); }
    for( const auto& x : j.value("regions",nlohmann::json::array()) ) { BOARD_STRUCTURE_REGION r; r.id=x.value("id","");r.name=x.value("name","");r.stack_id=x.value("stack_id","");r.shape=readPoly(x.value("shape",nlohmann::json::object()));regions.push_back(r); }
    for( const auto& x : j.value("boundaries",nlohmann::json::array()) ) { BOARD_STRUCTURE_BOUNDARY b; b.id=x.value("id",""); for(const auto& p:x.value("polyline",nlohmann::json::array())) b.polyline.push_back(readPoint(p)); boundaries.push_back(std::move(b)); }
    for( const auto& x : j.value("region_seeds",nlohmann::json::array()) ) { BOARD_STRUCTURE_REGION_SEED s; s.region_id=x.value("region_id",""); s.point=readPoint(x.value("point",nlohmann::json::array())); region_seeds.push_back(s); }
    for( const auto& x : j.value("stiffeners",nlohmann::json::array()) ) { BOARD_STRUCTURE_STIFFENER s; s.id=x.value("id","");s.region_id=x.value("region_id","");s.shape=readPoly(x.value("shape",nlohmann::json::object()));s.material=x.value("material","");s.adhesive=x.value("adhesive","");s.thickness_nm=x.value("thickness_nm",0LL);s.min_thickness_nm=x.value("min_thickness_nm",0LL);s.max_thickness_nm=x.value("max_thickness_nm",0LL);s.adhesive_thickness_nm=x.value("adhesive_thickness_nm",0LL);s.top=x.value("top",true);s.role=x.value("role","");s.support_targets=x.value("support_targets",std::vector<std::string>{});stiffeners.push_back(s); }
    for( const auto& x : j.value("coverlays",nlohmann::json::array()) ) { BOARD_STRUCTURE_COVERLAY c; c.id=x.value("id","");c.region_id=x.value("region_id","");c.coverage=readPoly(x.value("coverage",nlohmann::json::object()));c.material=x.value("material","");c.adhesive=x.value("adhesive","");c.thickness_nm=x.value("thickness_nm",0LL);c.adhesive_thickness_nm=x.value("adhesive_thickness_nm",0LL);c.top=x.value("top",true);c.registration_tolerance_nm=x.value("registration_tolerance_nm",0LL);for(const auto&o:x.value("openings",nlohmann::json::array()))c.openings.push_back(readPoly(o));coverlays.push_back(c); }
    for( const auto& x : j.value("bends",nlohmann::json::array()) ) { BOARD_STRUCTURE_BEND b; b.id=x.value("id","");b.region_id=x.value("region_id","");b.start=readPoint(x.value("start",nlohmann::json::array()));b.end=readPoint(x.value("end",nlohmann::json::array()));b.angle_degrees=x.value("angle_degrees",0.0);b.inner_radius_mm=x.value("inner_radius_mm",0.0);b.neutral_axis_offset_mm=x.value("neutral_axis_offset_mm",-1.0);b.moving_side_left=x.value("moving_side_left",true);b.sequence=x.value("sequence",0);bends.push_back(b); }
}

BOARD_STRUCTURE_VALIDATION BOARD_STRUCTURE::Validate() const
{
    BOARD_STRUCTURE_VALIDATION result;
    duplicateIds( stacks, result.errors, "stack" );
    duplicateIds( regions, result.errors, "region" );
    duplicateIds( boundaries, result.errors, "boundary" );
    duplicateIds( stiffeners, result.errors, "stiffener" );
    duplicateIds( coverlays, result.errors, "coverlay" );
    duplicateIds( bends, result.errors, "bend" );
    auto error = [&]( const std::string& text, const std::string& id )
    { result.errors.push_back( text + ": " + id ); };
    auto checkShape = [&]( const BOARD_STRUCTURE_POLYGON& shape, const std::string& id )
    {
        if( shape.outline.size() < 3 ) { error( "outline needs at least three points", id ); return; }
        // IsSelfIntersecting operates on all contours in a polygon set.  A
        // hole is a separate contour, so checking the combined set can
        // incorrectly classify an otherwise valid outer contour plus hole.
        // Validate each ring independently, then check containment below.
        BOARD_STRUCTURE_POLYGON outerOnly = shape;
        outerOnly.holes.clear();
        SHAPE_POLY_SET outerSet = polySet( outerOnly );
        if( outerSet.IsSelfIntersecting() || std::abs( outerSet.Area() ) <= 1.0 )
            error( "invalid or zero-area polygon", id );
        for( const auto& hole : shape.holes )
        {
            if( hole.size() < 3 ) { error( "hole needs at least three points", id ); continue; }
            SHAPE_POLY_SET remainder = polySet( BOARD_STRUCTURE_POLYGON{ hole, {} } );
            if( remainder.IsSelfIntersecting() || std::abs( remainder.Area() ) <= 1.0 )
                error( "invalid or zero-area hole", id );
            remainder.BooleanSubtract( outerSet );
            if( !remainder.IsEmpty() && std::abs( remainder.Area() ) > 1.0 )
                error( "hole extends outside outline", id );
        }
    };
    std::set<std::string> stackIds, regionIds;
    for( const auto& stack : stacks )
    {
        stackIds.insert( stack.id );
        std::set<int> copper;
        for( int layer : stack.copper_layers )
            if( layer < 0 || layer >= PCB_LAYER_ID_COUNT || !IsCopperLayer( layer )
                || !copper.insert( layer ).second ) error( "invalid or duplicate copper layer", stack.id );
        if( copper.empty() ) error( "stack has no copper layers", stack.id );
        duplicateIds( stack.materials, result.errors, "material" );
        std::map<std::string, const BOARD_STRUCTURE_MATERIAL*> materials;
        for( const auto& material : stack.materials )
        {
            materials[material.id] = &material;
            if( material.thickness_nm <= 0 ) error( "material thickness must be positive", material.id );
            if( material.min_thickness_nm < 0 || material.max_thickness_nm < 0
                || ( material.max_thickness_nm > 0 &&
                     ( material.min_thickness_nm > material.thickness_nm
                       || material.thickness_nm > material.max_thickness_nm ) ) )
                error( "invalid material thickness interval", material.id );
        }
        std::set<int> mappedCopper;
        std::vector<std::pair<double, double>> intervals;
        for( const auto& layer : stack.layers )
        {
            if( !materials.contains( layer.material_id ) )
            {
                error( "layer references missing material", stack.id );
                continue;
            }
            if( layer.global_copper_layer != -1 )
            {
                if( !copper.contains( layer.global_copper_layer ) )
                    error( "layer mapping not present in stack copper", stack.id );
                if( !mappedCopper.insert( layer.global_copper_layer ).second )
                    error( "duplicate physical copper mapping", stack.id );
            }
            const auto* material = materials.at( layer.material_id );
            if( ( material->category == "copper" ) != ( layer.global_copper_layer != -1 ) )
                error( "copper material must map to a copper layer", stack.id );
            const double lo = static_cast<double>( layer.z_offset_nm );
            intervals.emplace_back( lo, lo + static_cast<double>( material->thickness_nm ) );
        }
        if( !stack.materials.empty() && stack.layers.empty() )
            error( "physical stack has no layer instances", stack.id );
        if( !stack.layers.empty() && mappedCopper != copper )
            error( "physical stack must map every copper layer", stack.id );
        std::sort( intervals.begin(), intervals.end() );
        for( size_t i = 1; i < intervals.size(); ++i )
            if( intervals[i].first < intervals[i-1].second )
                error( "physical stack layers overlap", stack.id );
    }
    for( const auto& region : regions )
    {
        regionIds.insert( region.id );
        checkShape( region.shape, region.id );
        if( !stackIds.contains( region.stack_id ) ) error( "region references missing stack", region.id );
    }
    std::set<std::string> seedRegions;
    for( const auto& boundary : boundaries )
    {
        if( boundary.polyline.size() < 2 ) error( "boundary needs at least two points", boundary.id );
        for( size_t i = 1; i < boundary.polyline.size(); ++i )
            if( boundary.polyline[i] == boundary.polyline[i-1] ) error( "boundary has a degenerate segment", boundary.id );
    }
    for( const auto& seed : region_seeds )
    {
        if( !regionIds.contains( seed.region_id ) ) error( "seed references missing region", seed.region_id );
        if( !seedRegions.insert( seed.region_id ).second ) error( "duplicate region seed", seed.region_id );
    }
    for( const auto& stiffener : stiffeners )
    {
        checkShape( stiffener.shape, stiffener.id );
        if( !regionIds.contains( stiffener.region_id ) ) error( "stiffener references missing region", stiffener.id );
        if( stiffener.thickness_nm <= 0 || stiffener.adhesive_thickness_nm < 0 )
            error( "invalid stiffener or adhesive thickness", stiffener.id );
        if( stiffener.min_thickness_nm < 0 || stiffener.max_thickness_nm < 0
            || ( stiffener.max_thickness_nm > 0 &&
                 ( stiffener.min_thickness_nm > stiffener.thickness_nm
                   || stiffener.thickness_nm > stiffener.max_thickness_nm ) ) )
            error( "invalid stiffener thickness interval", stiffener.id );
    }
    for( const auto& coverlay : coverlays )
    {
        checkShape( coverlay.coverage, coverlay.id );
        for( const auto& opening : coverlay.openings ) checkShape( opening, coverlay.id );
        if( !regionIds.contains( coverlay.region_id ) ) error( "coverlay references missing region", coverlay.id );
        if( coverlay.thickness_nm < 0 || coverlay.adhesive_thickness_nm < 0
            || coverlay.registration_tolerance_nm < 0 ) error( "invalid coverlay thickness/tolerance", coverlay.id );
    }
    for( const auto& bend : bends )
    {
        if( !regionIds.contains( bend.region_id ) ) error( "bend references missing region", bend.id );
        for( const auto& region : regions )
            if( region.id == bend.region_id )
                if( const auto* stack = StackForRegion( region ); stack && stack->kind != BOARD_STRUCTURE_STACK_KIND::FLEX )
                    error( "bend must belong to a flex region", bend.id );
        if( !std::isfinite( bend.inner_radius_mm ) || bend.inner_radius_mm <= 0 ) error( "bend radius must be positive", bend.id );
        if( bend.start == bend.end ) error( "bend axis is degenerate", bend.id );
        if( !std::isfinite( bend.angle_degrees ) || std::abs( bend.angle_degrees ) > 360 ) error( "invalid bend angle", bend.id );
        if( !std::isfinite( bend.neutral_axis_offset_mm ) || bend.neutral_axis_offset_mm < -1 ) error( "invalid neutral axis", bend.id );
    }

    // Sequence numbers define the developed-to-folded transport order.  Keep
    // them unambiguous, and reject geometry for which a later hinge cannot be
    // assigned one rigid tangent frame after an earlier bend.
    for( size_t i = 0; i < bends.size(); ++i )
        for( size_t j = i + 1; j < bends.size(); ++j )
        {
            if( bends[i].sequence == bends[j].sequence )
            {
                result.errors.emplace_back( "duplicate bend sequence: "
                                            + std::to_string( bends[i].sequence ) );
            }

            if( bends[i].start == bends[i].end || bends[j].start == bends[j].end
                || bendWidthIu( *this, bends[i] ) <= 0.0 || bendWidthIu( *this, bends[j] ) <= 0.0 )
                continue;

            if( polygonsIntersect( bendStrip( *this, bends[i] ), bendStrip( *this, bends[j] ) ) )
            {
                result.errors.emplace_back( "intersecting bend strips: "
                                            + bends[i].id + " and " + bends[j].id );
            }

            const BOARD_STRUCTURE_BEND* earlier = &bends[i];
            const BOARD_STRUCTURE_BEND* later = &bends[j];
            if( earlier->sequence > later->sequence )
                std::swap( earlier, later );
            if( segmentIntersectsBendStrip( *later, *earlier, bendWidthIu( *this, *earlier ) ) )
            {
                result.errors.emplace_back( "later bend hinge crosses an earlier bend strip: "
                                            + later->id );
            }
        }

    // FoldPointMm evaluates one continuous mechanical surface.  Parent
    // stacks with different developed thicknesses would require a distinct
    // neutral datum and cannot be represented by that evaluator without
    // silently distorting one of the bends, so reject that topology here.
    double commonThickness = -1.0;
    const double commonDatum = BendDatumZMm();
    for( const auto& bend : bends )
    {
        const double thickness = bendThicknessMm( *this, bend );
        BOARD_STRUCTURE singleBend = *this;
        singleBend.bends = { bend };
        if( std::abs( singleBend.BendDatumZMm() - commonDatum ) > 1e-9 )
            error( "bends require a common parent stack center", bend.id );
        if( thickness > 0.0 && bend.neutral_axis_offset_mm > thickness )
            error( "neutral axis must lie within the flex stack", bend.id );
        if( bend.neutral_axis_offset_mm < 0.0 && bend.neutral_axis_offset_mm != -1.0 )
            error( "neutral axis must use -1 for automatic or a nonnegative offset", bend.id );
        if( thickness <= 0.0 )
            continue;
        if( commonThickness < 0.0 )
            commonThickness = thickness;
        else if( std::abs( commonThickness - thickness ) > 1e-9 )
            result.errors.emplace_back( "bends require a common parent stack thickness: " + bend.id );
    }
    return result;
}

bool BOARD_STRUCTURE::DerivePartition( const SHAPE_POLY_SET& aOutline, std::string* aError )
{
    auto fail = [&]( const std::string& message )
    { if( aError ) *aError = message; return false; };
    if( aOutline.IsEmpty() ) return fail( "invalid Edge.Cuts outline" );
    // The generic poly-set self-intersection test treats the closure of a
    // hole as non-adjacent to its own first segment. Check rings separately.
    for( int i = 0; i < aOutline.OutlineCount(); ++i )
    {
        SHAPE_POLY_SET outer;
        outer.AddOutline( aOutline.COutline( i ) );
        if( outer.IsSelfIntersecting() || outer.Area() <= 1.0 )
            return fail( "invalid Edge.Cuts outline" );
        SHAPE_POLY_SET holes;
        for( int h = 0; h < aOutline.HoleCount( i ); ++h )
        {
            SHAPE_POLY_SET hole;
            hole.AddOutline( aOutline.CHole( i, h ) );
            if( hole.IsSelfIntersecting() || hole.Area() <= 1.0 )
                return fail( "invalid Edge.Cuts cutout" );
            SHAPE_POLY_SET outside = hole;
            outside.BooleanSubtract( outer );
            if( !outside.IsEmpty() ) return fail( "Edge.Cuts cutout leaves board outline" );
            SHAPE_POLY_SET overlap = hole;
            overlap.BooleanIntersection( holes );
            if( !overlap.IsEmpty() ) return fail( "Edge.Cuts cutouts overlap" );
            holes.Append( hole );
        }
    }

    // Validate the complete edit before changing the model.  A cut must join
    // two existing boundary points and must be a straight (possibly
    // multi-vertex) crosscut; this is the supported, deterministic subset.
    for( const auto& boundary : boundaries )
    {
        if( boundary.id.empty() || boundary.polyline.size() < 2 ) return fail( "invalid boundary record" );
        for( size_t i = 0; i < boundary.polyline.size(); ++i )
            for( size_t j = i + 1; j < boundary.polyline.size(); ++j )
                if( boundary.polyline[i] == boundary.polyline[j] ) return fail( "boundary self-intersects" );
        const VECTOR2I a = boundary.polyline.front(), b = boundary.polyline.back();
        bool joined = pointOnBoundary( aOutline, a ) && pointOnBoundary( aOutline, b );
        for( const auto& prior : boundaries )
            if( &prior != &boundary && prior.polyline.size() >= 2 )
            {
                SHAPE_LINE_CHAIN chain( prior.polyline, false );
                joined = joined || pointOnChain( chain, a ) || pointOnChain( chain, b );
            }
        if( a == b || !joined )
            return fail( "boundary must join the Edge.Cuts boundary" );
        for( size_t i = 1; i + 1 < boundary.polyline.size(); ++i )
        {
            const VECTOR2I p = boundary.polyline[i];
            if( !aOutline.Contains( p ) && !pointOnBoundary( aOutline, p ) )
                return fail( "boundary vertex lies outside Edge.Cuts" );
        }
    }

    SHAPE_POLY_SET pieces = aOutline;
    for( const auto& boundary : boundaries )
    {
        const VECTOR2I a = boundary.polyline.front(), b = boundary.polyline.back();
        SHAPE_POLY_SET next;
        bool touched = false;
        for( int i = 0; i < pieces.OutlineCount(); ++i )
        {
            SHAPE_POLY_SET piece = pieces.UnitSet( i );
            std::vector<SHAPE_POLY_SET> split;
            if( pointOnBoundary( piece, a ) || pointOnBoundary( piece, b ) )
            {
                if( pointOnChain( piece.COutline( 0 ), a ) && pointOnChain( piece.COutline( 0 ), b ) )
                {
                    if( !splitPiece( piece, boundary.polyline, split ) ) return fail( "boundary does not split the current region" );
                    touched = true;
                    for( auto& part : split ) next.Append( part );
                    continue;
                }
                if( pointOnBoundary( piece, a ) != pointOnBoundary( piece, b ) )
                {
                    // A T-join touches only the child on one side of an
                    // existing cut.  The other child is unrelated and must
                    // pass through unchanged; an endpoint in its interior is
                    // a genuinely dangling cut.
                    const bool aInside = piece.Contains( a ) && !pointOnBoundary( piece, a );
                    const bool bInside = piece.Contains( b ) && !pointOnBoundary( piece, b );
                    if( aInside || bInside ) return fail( "boundary is dangling on the current partition" );
                    next.Append( piece );
                    continue;
                }
                if( pointOnBoundary( piece, a ) )
                    return fail( "crosscuts ending on cutouts are unsupported" );
            }
            next.Append( piece );
        }
        if( !touched ) return fail( "boundary does not touch the current partition" );
        double areaSum = 0.0;
        for( int i = 0; i < next.OutlineCount(); ++i ) areaSum += std::abs( next.UnitSet( i ).Area() );
        if( std::abs( areaSum - std::abs( pieces.Area() ) ) > 1.0 ) return fail( "boundary split loses board area" );
        for( int i = 0; i < next.OutlineCount(); ++i )
            for( int j = i + 1; j < next.OutlineCount(); ++j )
            {
                SHAPE_POLY_SET overlap = next.UnitSet( i );
                overlap.BooleanIntersection( next.UnitSet( j ) );
                if( !overlap.IsEmpty() && std::abs( overlap.Area() ) > 1.0 )
                    return fail( "boundary split overlaps regions" );
            }
        SHAPE_POLY_SET difference = next;
        difference.BooleanXor( pieces );
        if( !difference.IsEmpty() && std::abs( difference.Area() ) > 1.0 )
            return fail( "boundary split changes board coverage" );
        pieces = std::move( next );
    }

    std::vector<BOARD_STRUCTURE_REGION_SEED> seeds = region_seeds;
    if( seeds.empty() )
        for( const auto& region : regions )
        {
            if( region.shape.outline.empty() ) continue;
            seeds.push_back( { region.id, interiorPoint( region.shape ) } );
        }
    std::vector<BOARD_STRUCTURE_REGION> derived;
    std::set<std::string> used;
    for( const auto& old : regions ) used.insert( old.id );
    int generated = 0;
    for( int i = 0; i < pieces.OutlineCount(); ++i )
    {
        BOARD_STRUCTURE_REGION region;
        region.shape = toPolygon( pieces, i );
        std::string seededStack;
        for( const auto& seed : seeds )
            if( contains( region.shape, seed.point )
                && !pointOnBoundary( polySet( region.shape ), seed.point ) )
                for( const auto& old : regions )
                    if( old.id == seed.region_id )
                    {
                        if( !seededStack.empty() && seededStack != old.stack_id )
                            return fail( "partition merges regions with conflicting stacks" );
                        seededStack = old.stack_id;
                    }
        bool assigned = false;
        for( const auto& seed : seeds )
            if( contains( region.shape, seed.point )
                && !pointOnBoundary( polySet( region.shape ), seed.point ) )
            {
                region.id = seed.region_id; used.insert( region.id );
                for( const auto& old : regions ) if( old.id == region.id ) region.name = old.name, region.stack_id = old.stack_id;
                assigned = true;
                break;
            }
        if( region.id.empty() )
            for( const auto& old : regions )
                if( polygonsIntersect( region.shape, old.shape ) )
                {
                    region.name = old.name;
                    region.stack_id = old.stack_id;
                    region.id = old.id + "-part-" + std::to_string( ++generated );
                    break;
                }
        if( region.id.empty() )
        {
            do region.id = "partition-" + std::to_string( ++generated ); while( used.contains( region.id ) );
        }
        if( assigned )
            ;
        else
            while( used.contains( region.id ) ) region.id += "-part";
        used.insert( region.id );
        derived.push_back( std::move( region ) );
    }
    if( derived.empty() ) return fail( "partition produced no regions" );
    auto reassociate = [&]( const std::string& oldId, const SHAPE_POLY_SET& shape, std::string& newId )
    {
        bool known = false;
        for( const auto& old : regions ) if( old.id == oldId ) { known = true; break; }
        if( !known ) return false;
        SHAPE_POLY_SET remaining = shape;
        int matches = 0;
        for( const auto& region : derived )
        {
            SHAPE_POLY_SET overlap = remaining;
            overlap.BooleanIntersection( polySet( region.shape ) );
            if( !overlap.IsEmpty() && std::abs( overlap.Area() ) > 1.0 ) newId = region.id, ++matches;
            remaining.BooleanSubtract( polySet( region.shape ) );
        }
        return matches == 1 && ( remaining.IsEmpty() || std::abs( remaining.Area() ) <= 1.0 );
    };
    auto updatedStiffeners = stiffeners;
    for( auto& item : updatedStiffeners )
        if( !item.shape.outline.empty() && !reassociate( item.region_id, polySet( item.shape ), item.region_id ) )
            return fail( "stiffener spans or leaves its partition region" );
    auto updatedCoverlays = coverlays;
    for( auto& item : updatedCoverlays )
        if( !item.coverage.outline.empty() && !reassociate( item.region_id, polySet( item.coverage ), item.region_id ) )
            return fail( "coverlay spans or leaves its partition region" );
    auto updatedBends = bends;
    for( auto& item : updatedBends )
    {
        BOARD_STRUCTURE_POLYGON axis;
        axis.outline = { item.start, item.end, item.end + VECTOR2I( 1, 0 ), item.start + VECTOR2I( 1, 0 ) };
        const BOARD_STRUCTURE_POLYGON& bendShape = std::abs( item.angle_degrees ) > 1e-9 ? bendStrip( *this, item ) : axis;
        if( !reassociate( item.region_id, polySet( bendShape ), item.region_id ) )
            return fail( "bend leaves or spans its partition region" );
    }
    regions = std::move( derived );
    stiffeners = std::move( updatedStiffeners );
    coverlays = std::move( updatedCoverlays );
    bends = std::move( updatedBends );
    region_seeds.clear();
    for( const auto& region : regions ) region_seeds.push_back( { region.id, interiorPoint( region.shape ) } );
    return true;
}

const BOARD_STRUCTURE_REGION* BOARD_STRUCTURE::RegionAt( const VECTOR2I& p ) const { for(const auto&r:regions)if(contains(r.shape,p))return &r; return nullptr; }
const BOARD_STRUCTURE_STACK* BOARD_STRUCTURE::StackForRegion( const BOARD_STRUCTURE_REGION& r ) const { for(const auto&s:stacks)if(s.id==r.stack_id)return &s; return nullptr; }
std::vector<const BOARD_STRUCTURE_REGION*> BOARD_STRUCTURE::RegionsIntersecting( const BOARD_STRUCTURE_POLYGON& p ) const { std::vector<const BOARD_STRUCTURE_REGION*> out; for(const auto&r:regions)if(polygonsIntersect(r.shape,p))out.push_back(&r);return out; }
double BOARD_STRUCTURE::BendDatumZMm() const
{
    if( bends.empty() ) return 0.0;
    for( const auto& region : regions )
        if( region.id == bends.front().region_id )
            if( const auto* stack = StackForRegion( region ) )
            {
                double lo = std::numeric_limits<double>::max();
                double hi = std::numeric_limits<double>::lowest();
                for( const auto& layer : stack->layers )
                    for( const auto& material : stack->materials )
                        if( material.id == layer.material_id )
                        {
                            lo = std::min( lo, layer.z_offset_nm * 1e-6 );
                            hi = std::max( hi, ( layer.z_offset_nm + material.thickness_nm ) * 1e-6 );
                        }
                if( lo <= hi ) return ( lo + hi ) * 0.5;
            }
    return 0.0;
}

double BOARD_STRUCTURE::BendStripLengthMm( const BOARD_STRUCTURE_BEND& b, double t ) { const double r=b.inner_radius_mm+(b.neutral_axis_offset_mm>=0?b.neutral_axis_offset_mm:t/2.0); return r*std::abs(b.angle_degrees)*M_PI/180.0; }

VECTOR3D BOARD_STRUCTURE::FoldPointMm( const BOARD_STRUCTURE_BEND& b, const VECTOR3D& p,
                                       double t )
{
    BACKPLANE_RIGID_FLEX::BEND_SPEC spec;
    spec.axis.origin = VECTOR2D( b.start.x * 1e-6, b.start.y * 1e-6 );
    spec.axis.direction = VECTOR2D( ( b.end.x - b.start.x ) * 1e-6,
                                    ( b.end.y - b.start.y ) * 1e-6 );
    spec.innerRadius = b.inner_radius_mm;
    spec.neutralAxisOffset = b.neutral_axis_offset_mm >= 0.0
                                   ? b.neutral_axis_offset_mm
                                   : std::numeric_limits<double>::quiet_NaN();
    spec.angleRadians = b.angle_degrees * M_PI / 180.0;
    spec.movingSide = b.moving_side_left ? 1 : -1;
    return BACKPLANE_RIGID_FLEX::transformPoint( spec, t, p );
}

VECTOR3D BOARD_STRUCTURE::FoldPointMm( const std::vector<BOARD_STRUCTURE_BEND>& aBends,
                                       const VECTOR3D& p, double t, double aBendDatumZ )
{
    std::vector<const BOARD_STRUCTURE_BEND*> ordered;
    ordered.reserve( aBends.size() );
    for( const auto& bend : aBends )
        ordered.push_back( &bend );
    std::stable_sort( ordered.begin(), ordered.end(),
                      []( const auto* lhs, const auto* rhs )
                      { return lhs->sequence < rhs->sequence; } );

    std::vector<BACKPLANE_RIGID_FLEX::BEND_SPEC> specs;
    specs.reserve( ordered.size() );
    for( const auto* bend : ordered )
    {
        BACKPLANE_RIGID_FLEX::BEND_SPEC spec;
        spec.axis.origin = VECTOR2D( bend->start.x * 1e-6, bend->start.y * 1e-6 );
        spec.axis.z = aBendDatumZ;
        spec.axis.direction = VECTOR2D( ( bend->end.x - bend->start.x ) * 1e-6,
                                        ( bend->end.y - bend->start.y ) * 1e-6 );
        spec.innerRadius = bend->inner_radius_mm;
        spec.neutralAxisOffset = bend->neutral_axis_offset_mm >= 0.0
                                       ? bend->neutral_axis_offset_mm
                                       : std::numeric_limits<double>::quiet_NaN();
        spec.angleRadians = bend->angle_degrees * M_PI / 180.0;
        spec.movingSide = bend->moving_side_left ? 1 : -1;
        specs.push_back( spec );
    }
    return BACKPLANE_RIGID_FLEX::transformPointSequence( specs, t, p );
}

bool BOARD_STRUCTURE::StiffenerFits( double aBaseMinMm, double aBaseMaxMm,
                                     double aAdhesiveMinMm, double aAdhesiveMaxMm,
                                     double aStiffenerMinMm, double aStiffenerMaxMm,
                                     double aRequiredMinMm, double aRequiredMaxMm )
{
    return BACKPLANE_RIGID_FLEX::stiffenerFits( aBaseMinMm, aBaseMaxMm,
                                                aAdhesiveMinMm, aAdhesiveMaxMm,
                                                aStiffenerMinMm, aStiffenerMaxMm,
                                                aRequiredMinMm, aRequiredMaxMm );
}
