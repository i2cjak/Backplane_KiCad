/* Focused regression coverage for the fork-only board structure model. */
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

#include <board_structure.h>

namespace
{
BOARD_STRUCTURE_POLYGON rectangle( int x0, int y0, int x1, int y1 )
{
    BOARD_STRUCTURE_POLYGON result;
    result.outline = { VECTOR2I( x0, y0 ), VECTOR2I( x1, y0 ),
                       VECTOR2I( x1, y1 ), VECTOR2I( x0, y1 ) };
    return result;
}

void loadUnsupportedSchema()
{
    BOARD_STRUCTURE structure;
    structure.FromJson( nlohmann::json{ { "schema_version", BOARD_STRUCTURE::SCHEMA_VERSION + 1 } } );
}

void loadStringSchema()
{
    BOARD_STRUCTURE structure;
    structure.FromJson( nlohmann::json{ { "schema_version", "one" } } );
}
}

BOOST_AUTO_TEST_SUITE( BackplaneBoardStructureReview )

BOOST_AUTO_TEST_CASE( AdjacentRegionsAreNotAnOverlap )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "stack";
    structure.stacks.push_back( stack );

    BOARD_STRUCTURE_REGION left{ "left", "Left", "stack", rectangle( 0, 0, 100, 100 ) };
    BOARD_STRUCTURE_REGION right{ "right", "Right", "stack", rectangle( 100, 0, 200, 100 ) };
    structure.regions = { left, right };

    // Regions sharing an edge are a valid partition of a board.  DRC should
    // report positive-area overlap only; this also protects the public query
    // used by the DRC provider.
    const auto hits = structure.RegionsIntersecting( left.shape );
    BOOST_CHECK( std::none_of( hits.begin(), hits.end(),
                               [&]( const BOARD_STRUCTURE_REGION* region )
                               { return region && region->id == "right"; } ) );
}

BOOST_AUTO_TEST_CASE( PolygonInsideHoleDoesNotIntersect )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "stack";
    structure.stacks.push_back( stack );

    BOARD_STRUCTURE_REGION region{ "r", "R", "stack", rectangle( 0, 0, 100, 100 ) };
    region.shape.holes.push_back( rectangle( 25, 25, 75, 75 ).outline );
    structure.regions.push_back( region );

    BOOST_CHECK( structure.RegionAt( VECTOR2I( 50, 50 ) ) == nullptr );
    BOOST_CHECK( structure.RegionAt( VECTOR2I( 10, 10 ) ) != nullptr );
    BOOST_CHECK( structure.RegionsIntersecting( rectangle( 40, 40, 60, 60 ) ).empty() );
}

BOOST_AUTO_TEST_CASE( HoleOutsideOutlineIsRejected )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "stack";
    stack.copper_layers = { 0 };
    structure.stacks.push_back( stack );

    BOARD_STRUCTURE_REGION region{ "region", "Region", "stack", rectangle( 0, 0, 100, 100 ) };
    region.shape.holes.push_back( rectangle( 75, 75, 125, 125 ).outline );
    structure.regions.push_back( region );

    const auto validation = structure.Validate();
    BOOST_CHECK( !validation.IsValid() );
    BOOST_CHECK( std::any_of( validation.errors.begin(), validation.errors.end(),
                              []( const std::string& error )
                              { return error.find( "hole extends outside outline" ) != std::string::npos; } ) );
}

BOOST_AUTO_TEST_CASE( EnclosedHoleIsValidPhysicalRegion )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "stack";
    stack.copper_layers = { 0 };
    structure.stacks.push_back( stack );

    BOARD_STRUCTURE_REGION region{ "region", "Region", "stack", rectangle( 0, 0, 100, 100 ) };
    region.shape.holes.push_back( rectangle( 25, 25, 75, 75 ).outline );
    structure.regions.push_back( region );

    const auto validation = structure.Validate();
    BOOST_CHECK_MESSAGE( validation.IsValid(), "An enclosed region hole should be valid" );
}

BOOST_AUTO_TEST_CASE( SelfIntersectingHoleIsRejected )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "stack";
    stack.copper_layers = { 0 };
    structure.stacks.push_back( stack );

    BOARD_STRUCTURE_REGION region{ "region", "Region", "stack", rectangle( 0, 0, 100, 100 ) };
    region.shape.holes.push_back( { VECTOR2I( 25, 25 ), VECTOR2I( 75, 75 ),
                                    VECTOR2I( 25, 75 ), VECTOR2I( 75, 25 ) } );
    structure.regions.push_back( region );

    const auto validation = structure.Validate();
    BOOST_CHECK( !validation.IsValid() );
    BOOST_CHECK( std::any_of( validation.errors.begin(), validation.errors.end(),
                              []( const std::string& error )
                              { return error.find( "invalid or zero-area hole" ) != std::string::npos; } ) );
}

BOOST_AUTO_TEST_CASE( StructureRoundTripPreservesLayerIdsUnitsAndUnknownFields )
{
    BOARD_STRUCTURE source;
    source.unknown["future_extension"] = { { "enabled", true } };
    BOARD_STRUCTURE_STACK stack;
    stack.id = "flex";
    stack.copper_layers = { 0, 2 };
    stack.layers.push_back( { "cu-top", 0, 123456 } );
    source.stacks.push_back( stack );
    source.bends.push_back( { "bend", "region", VECTOR2I( 123456, 654321 ),
                              VECTOR2I( 123456, 1654321 ), 90.0, 2.5 } );

    BOARD_STRUCTURE copy;
    copy.FromJson( source.ToJson() );
    BOOST_CHECK_EQUAL( copy.stacks.front().copper_layers[0], 0 );
    BOOST_CHECK_EQUAL( copy.stacks.front().copper_layers[1], 2 );
    BOOST_CHECK_EQUAL( copy.stacks.front().layers.front().z_offset_nm, 123456 );
    BOOST_CHECK_EQUAL( copy.bends.front().start.x, 123456 );
    BOOST_CHECK_EQUAL( copy.bends.front().start.y, 654321 );
    BOOST_CHECK( copy.unknown["future_extension"]["enabled"].get<bool>() );
}

BOOST_AUTO_TEST_CASE( NestedUnknownFieldsSurviveKnownObjectRewrite )
{
    nlohmann::json input = {
        { "schema_version", BOARD_STRUCTURE::SCHEMA_VERSION },
        { "stacks", nlohmann::json::array( {
            { { "id", "s" }, { "copper_layers", { 0 } },
              { "future_stack_field", { { "enabled", true } } },
              { "materials", nlohmann::json::array( {
                  { { "id", "m" }, { "thickness_nm", 50000 },
                    { "future_material_field", "keep-me" } }
              } ) } }
        } ) }
    };
    BOARD_STRUCTURE structure;
    structure.FromJson( input );
    structure.stacks.front().name = "edited";
    const auto output = structure.ToJson();
    BOOST_CHECK( output["stacks"][0]["future_stack_field"]["enabled"].get<bool>() );
    BOOST_CHECK_EQUAL( output["stacks"][0]["materials"][0]["future_material_field"], "keep-me" );
}

BOOST_AUTO_TEST_CASE( NegativeMaterialIntervalsAreRejected )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "s";
    stack.copper_layers = { 0 };
    stack.materials.push_back( { "m", "dielectric", "PI", 50000, -1, 60000 } );
    structure.stacks.push_back( stack );
    const auto result = structure.Validate();
    BOOST_CHECK( !result.IsValid() );
}

BOOST_AUTO_TEST_CASE( PositiveAreaOverlapIsDetectedButSharedEdgeIsAllowed )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "s";
    stack.copper_layers = { 0 };
    structure.stacks.push_back( stack );
    const auto left = BOARD_STRUCTURE_REGION{ "left", "", "s", rectangle( 0, 0, 100, 100 ) };
    const auto touching = BOARD_STRUCTURE_REGION{ "touching", "", "s", rectangle( 100, 0, 200, 100 ) };
    const auto overlap = BOARD_STRUCTURE_REGION{ "overlap", "", "s", rectangle( 99, 0, 199, 100 ) };
    structure.regions = { left, touching, overlap };
    const auto touchingHits = structure.RegionsIntersecting( left.shape );
    BOOST_CHECK( std::none_of( touchingHits.begin(), touchingHits.end(),
                               []( const auto* r ) { return r && r->id == "touching"; } ) );
    BOOST_CHECK( std::any_of( touchingHits.begin(), touchingHits.end(),
                              []( const auto* r ) { return r && r->id == "overlap"; } ) );
}

BOOST_AUTO_TEST_CASE( UnknownOnlyExtensionRemainsPersistable )
{
    BOARD_STRUCTURE structure;
    structure.FromJson( nlohmann::json{ { "schema_version", BOARD_STRUCTURE::SCHEMA_VERSION },
                                        { "future_extension", true } } );
    // The board serializer uses Empty() to decide whether to emit the
    // companion extension.  Unknown fields must keep an otherwise empty
    // structure serializable across a load/save cycle.
    BOOST_CHECK( !structure.Empty() );
    BOOST_CHECK( structure.ToJson()["future_extension"].get<bool>() );
}

BOOST_AUTO_TEST_CASE( MalformedSchemaIsRejected )
{
    BOOST_CHECK_THROW( loadUnsupportedSchema(), std::invalid_argument );
    BOOST_CHECK_THROW( loadStringSchema(), std::exception );
}

BOOST_AUTO_TEST_CASE( PhysicalStackRejectsOverlapsAndMissingCopper )
{
    BOARD_STRUCTURE structure;
    BOARD_STRUCTURE_STACK stack;
    stack.id = "physical";
    stack.copper_layers = { 0, 2 };
    stack.materials = { { "top", "copper", "Copper", 18000, 18000, 18000 },
                        { "core", "dielectric", "PI", 114000, 114000, 114000 },
                        { "bottom", "copper", "Copper", 18000, 18000, 18000 } };
    stack.layers = { { "bottom", 2, -75000 }, { "core", -1, -57000 },
                     { "top", 0, 57000 } };
    structure.stacks = { stack };
    BOOST_CHECK( structure.Validate().IsValid() );
    structure.stacks[0].layers[1].z_offset_nm = -58000;
    BOOST_CHECK( !structure.Validate().IsValid() );
    structure.stacks[0] = stack;
    structure.stacks[0].layers.pop_back();
    BOOST_CHECK( !structure.Validate().IsValid() );
    structure.stacks[0] = stack;
    structure.stacks[0].layers[2].global_copper_layer = 2;
    BOOST_CHECK( !structure.Validate().IsValid() );
}

BOOST_AUTO_TEST_SUITE_END()
