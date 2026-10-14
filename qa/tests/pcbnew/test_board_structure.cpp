#include <boost/test/unit_test.hpp>

#include <board_structure.h>

BOOST_AUTO_TEST_SUITE( BackplaneBoardStructure )

BOOST_AUTO_TEST_CASE( CompanionRoundTripAndUnknownFields )
{
    BOARD_STRUCTURE source;
    source.unknown["future_extension"] = { { "enabled", true } };
    BOARD_STRUCTURE_STACK stack;
    stack.id = "flex";
    stack.name = "Flex";
    stack.kind = BOARD_STRUCTURE_STACK_KIND::FLEX;
    stack.copper_layers = { 0, 2 };
    stack.continuity_id = "cu0";
    source.stacks.push_back( stack );
    BOARD_STRUCTURE_REGION region;
    region.id = "tail";
    region.name = "Connector tail";
    region.stack_id = "flex";
    region.shape.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 100, 0 ), VECTOR2I( 100, 50 ), VECTOR2I( 0, 50 ) };
    source.regions.push_back( region );
    source.bends.push_back( { "bend", "tail", VECTOR2I( 10, 0 ), VECTOR2I( 10, 50 ), 90.0, 2.0 } );

    BOARD_STRUCTURE copy;
    copy.FromJson( source.ToJson() );
    BOOST_REQUIRE( copy.Validate().IsValid() );
    BOOST_CHECK( copy.unknown["future_extension"]["enabled"].get<bool>() );
    BOOST_CHECK_EQUAL( copy.RegionAt( VECTOR2I( 25, 25 ) )->id, "tail" );
    BOOST_CHECK_CLOSE( BOARD_STRUCTURE::BendStripLengthMm( copy.bends.front() ), 3.141592653589793, 1e-8 );
}

BOOST_AUTO_TEST_CASE( InvalidReferencesAndRadiusAreReported )
{
    BOARD_STRUCTURE structure;
    structure.regions.push_back( { "r", "", "missing", {} } );
    structure.bends.push_back( { "b", "r", VECTOR2I( 0, 0 ), VECTOR2I( 0, 0 ), 0.0, 0.0 } );
    const auto result = structure.Validate();
    BOOST_CHECK( !result.IsValid() );
    BOOST_CHECK( result.errors.size() >= 3 );
}

BOOST_AUTO_TEST_SUITE_END()
