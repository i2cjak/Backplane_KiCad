#include <boost/test/unit_test.hpp>

#include <board_structure.h>
#include <geometry/shape_poly_set.h>

namespace
{
double polygonArea( const BOARD_STRUCTURE_POLYGON& polygon )
{
    auto ringArea = []( const std::vector<VECTOR2I>& ring )
    { double area = 0.0; for( size_t i = 0; i < ring.size(); ++i ) area += double( ring[i].x ) * ring[( i + 1 ) % ring.size()].y - double( ring[i].y ) * ring[( i + 1 ) % ring.size()].x; return std::abs( area ) * 0.5; };
    double area = ringArea( polygon.outline );
    for( const auto& hole : polygon.holes ) area -= ringArea( hole );
    return area;
}

SHAPE_POLY_SET square()
{
    SHAPE_LINE_CHAIN ring;
    ring.Append( VECTOR2I( 0, 0 ) ); ring.Append( VECTOR2I( 100, 0 ) );
    ring.Append( VECTOR2I( 100, 100 ) ); ring.Append( VECTOR2I( 0, 100 ) );
    ring.SetClosed( true );
    SHAPE_POLY_SET result; result.AddOutline( ring );
    return result;
}
}

BOOST_AUTO_TEST_SUITE( BackplaneStructurePartition )

BOOST_AUTO_TEST_CASE( StraightCrosscutPreservesSeedAssignment )
{
    BOARD_STRUCTURE structure;
    structure.boundaries.push_back( { "split", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } } );
    structure.stacks.push_back( { "rigid", "", BOARD_STRUCTURE_STACK_KIND::RIGID } );
    structure.stacks.push_back( { "flex", "", BOARD_STRUCTURE_STACK_KIND::FLEX } );
    structure.region_seeds.push_back( { "left", VECTOR2I( 25, 50 ) } );
    structure.region_seeds.push_back( { "right", VECTOR2I( 75, 50 ) } );
    structure.regions.push_back( { "left", "Left", "rigid", {} } );
    structure.regions.push_back( { "right", "Right", "flex", {} } );

    BOOST_REQUIRE( structure.DerivePartition( square() ) );
    BOOST_REQUIRE_EQUAL( structure.regions.size(), 2 );
    BOOST_CHECK_EQUAL( structure.RegionAt( VECTOR2I( 25, 50 ) )->id, "left" );
    BOOST_CHECK_EQUAL( structure.RegionAt( VECTOR2I( 75, 50 ) )->id, "right" );
    BOOST_CHECK_EQUAL( structure.RegionAt( VECTOR2I( 25, 50 ) )->stack_id, "rigid" );
    BOOST_CHECK_EQUAL( structure.RegionAt( VECTOR2I( 75, 50 ) )->stack_id, "flex" );
}

BOOST_AUTO_TEST_CASE( DanglingCrosscutIsAtomic )
{
    BOARD_STRUCTURE structure;
    structure.boundaries.push_back( { "dangling", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 50 ) } } );
    BOARD_STRUCTURE_REGION old{ "old", "", "", {} };
    old.shape.outline = { VECTOR2I( 1, 1 ), VECTOR2I( 2, 1 ), VECTOR2I( 2, 2 ) };
    structure.regions.push_back( old );
    std::string error;
    BOOST_CHECK( !structure.DerivePartition( square(), &error ) );
    BOOST_CHECK_EQUAL( structure.regions.size(), 1 );
    BOOST_CHECK_EQUAL( structure.regions.front().id, "old" );
    BOOST_CHECK( !error.empty() );
}

BOOST_AUTO_TEST_CASE( TJoinAndLPolylineMakeFinitePieces )
{
    BOARD_STRUCTURE structure;
    structure.boundaries = {
        { "main", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } },
        { "tee", { VECTOR2I( 50, 50 ), VECTOR2I( 100, 50 ) } }
    };
    BOOST_REQUIRE( structure.DerivePartition( square() ) );
    BOOST_CHECK_EQUAL( structure.regions.size(), 3 );

    BOARD_STRUCTURE l;
    l.boundaries.push_back( { "l", { VECTOR2I( 0, 25 ), VECTOR2I( 50, 25 ), VECTOR2I( 50, 100 ) } } );
    BOOST_REQUIRE( l.DerivePartition( square() ) );
    BOOST_CHECK_EQUAL( l.regions.size(), 2 );
}

BOOST_AUTO_TEST_CASE( CutoutAndAreaArePreserved )
{
    SHAPE_POLY_SET board = square();
    SHAPE_LINE_CHAIN hole;
    hole.Append( VECTOR2I( 40, 40 ) ); hole.Append( VECTOR2I( 60, 40 ) );
    hole.Append( VECTOR2I( 60, 60 ) ); hole.Append( VECTOR2I( 40, 60 ) ); hole.SetClosed( true );
    board.AddHole( hole, 0 );
    BOARD_STRUCTURE structure;
    structure.boundaries.push_back( { "split", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } } );
    std::string error;
    BOOST_REQUIRE_MESSAGE( structure.DerivePartition( board, &error ), error );
    BOOST_CHECK_EQUAL( structure.regions.size(), 2 );
    double area = 0.0;
    for( const auto& region : structure.regions ) area += polygonArea( region.shape );
    BOOST_CHECK( structure.RegionAt( VECTOR2I( 50, 50 ) ) == nullptr );
    BOOST_CHECK_CLOSE( area, 9600.0, 1e-6 );
}

BOOST_AUTO_TEST_CASE( RepeatDeriveKeepsIdsAndRejectsConflicts )
{
    BOARD_STRUCTURE structure;
    structure.stacks.push_back( { "a", "", BOARD_STRUCTURE_STACK_KIND::RIGID } );
    structure.region_seeds.push_back( { "base", VECTOR2I( 25, 50 ) } );
    structure.regions.push_back( { "base", "", "a", {} } );
    structure.boundaries.push_back( { "split", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } } );
    BOOST_REQUIRE( structure.DerivePartition( square() ) );
    std::vector<std::string> ids;
    for( const auto& region : structure.regions ) ids.push_back( region.id );
    BOOST_REQUIRE( structure.DerivePartition( square() ) );
    std::vector<std::string> ids2;
    for( const auto& region : structure.regions ) ids2.push_back( region.id );
    BOOST_CHECK_EQUAL_COLLECTIONS( ids.begin(), ids.end(), ids2.begin(), ids2.end() );
    BOOST_CHECK( structure.regions[1].stack_id == "a" );

    BOARD_STRUCTURE conflict;
    conflict.stacks.push_back( { "a", "", BOARD_STRUCTURE_STACK_KIND::RIGID } );
    conflict.stacks.push_back( { "b", "", BOARD_STRUCTURE_STACK_KIND::FLEX } );
    conflict.regions.push_back( { "one", "", "a", {} } );
    conflict.regions.push_back( { "two", "", "b", {} } );
    conflict.region_seeds = { { "one", VECTOR2I( 40, 50 ) }, { "two", VECTOR2I( 60, 50 ) } };
    std::string error;
    BOOST_CHECK( !conflict.DerivePartition( square(), &error ) );
    BOOST_CHECK( error.find( "conflicting" ) != std::string::npos );
}

BOOST_AUTO_TEST_CASE( DisconnectedIslandIsUnaffectedAndAssignedChildrenInherit )
{
    SHAPE_POLY_SET board = square();
    SHAPE_LINE_CHAIN island;
    island.Append( VECTOR2I( 200, 0 ) ); island.Append( VECTOR2I( 220, 0 ) );
    island.Append( VECTOR2I( 220, 20 ) ); island.Append( VECTOR2I( 200, 20 ) ); island.SetClosed( true );
    board.AddOutline( island );
    BOARD_STRUCTURE structure;
    structure.stacks.push_back( { "rigid", "", BOARD_STRUCTURE_STACK_KIND::RIGID } );
    BOARD_STRUCTURE_REGION source{ "source", "", "rigid", {} };
    source.shape.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 100, 0 ), VECTOR2I( 100, 100 ), VECTOR2I( 0, 100 ) };
    structure.regions.push_back( source );
    structure.region_seeds.push_back( { "source", VECTOR2I( 25, 50 ) } );
    structure.boundaries.push_back( { "split", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } } );
    BOOST_REQUIRE( structure.DerivePartition( board ) );
    BOOST_CHECK_EQUAL( structure.regions.size(), 3 );
    BOOST_CHECK_EQUAL( structure.RegionAt( VECTOR2I( 25, 50 ) )->stack_id, "rigid" );
    BOOST_CHECK( structure.RegionAt( VECTOR2I( 210, 10 ) )->stack_id.empty() );
}

BOOST_AUTO_TEST_CASE( SpanningStiffenerRejectsAtomically )
{
    BOARD_STRUCTURE structure;
    structure.stacks.push_back( { "rigid", "", BOARD_STRUCTURE_STACK_KIND::RIGID } );
    BOARD_STRUCTURE_REGION source{ "source", "", "rigid", {} };
    source.shape.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 100, 0 ), VECTOR2I( 100, 100 ), VECTOR2I( 0, 100 ) };
    structure.regions.push_back( source );
    structure.region_seeds.push_back( { "source", VECTOR2I( 25, 50 ) } );
    BOARD_STRUCTURE_STIFFENER stiffener;
    stiffener.id = "stiff"; stiffener.region_id = "source";
    stiffener.shape.outline = { VECTOR2I( 40, 40 ), VECTOR2I( 60, 40 ), VECTOR2I( 60, 60 ), VECTOR2I( 40, 60 ) };
    structure.stiffeners.push_back( stiffener );
    structure.boundaries.push_back( { "split", { VECTOR2I( 50, 0 ), VECTOR2I( 50, 100 ) } } );
    std::string error;
    BOOST_CHECK( !structure.DerivePartition( square(), &error ) );
    BOOST_CHECK_EQUAL( structure.regions.size(), 1 );
    BOOST_CHECK( error.find( "stiffener" ) != std::string::npos );
}

BOOST_AUTO_TEST_SUITE_END()
