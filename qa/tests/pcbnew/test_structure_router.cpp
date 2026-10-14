/*
 * This program source code file is part of KiCad, a free EDA application.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 or later.
 */

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <memory>

#include <board.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_node.h>
#include <router/pns_router.h>
#include <router/pns_segment.h>
#include <router/pns_via.h>


namespace
{
std::unique_ptr<BOARD> loadStructureExemplar()
{
    // __FILE__ is an absolute source path in the configured QA build.  Keep
    // the fixture in demos so this test exercises companion metadata loading.
    std::filesystem::path root = std::filesystem::path( __FILE__ );
    for( int i = 0; i < 4; ++i )
        root = root.parent_path();

    const std::filesystem::path boardPath = root / "demos/backplane-structure/rigid-flex-bridge.kicad_pcb";
    PCB_IO_KICAD_SEXPR         io;
    return std::unique_ptr<BOARD>( io.LoadBoard( wxString::FromUTF8( boardPath.string() ), nullptr ) );
}


size_t querySegment( PNS::NODE& aWorld, PCB_LAYER_ID aLayer, const VECTOR2I& aStart,
                     const VECTOR2I& aEnd )
{
    PNS::SEGMENT segment( SEG( aStart, aEnd ), (PNS::NET_HANDLE) 99 );
    segment.SetLayer( aLayer );
    segment.SetWidth( 100000 );
    PNS::NODE::OBSTACLES obstacles;
    aWorld.QueryColliding( &segment, obstacles );
    return obstacles.size();
}
} // namespace


BOOST_AUTO_TEST_CASE( PNSStructureRegionalKeepouts )
{
    std::unique_ptr<BOARD> board = loadStructureExemplar();
    BOOST_REQUIRE_MESSAGE( board, "Could not load rigid-flex structure exemplar" );
    BOOST_REQUIRE( !board->GetBoardStructure().Empty() );

    PNS_KICAD_IFACE_BASE iface;
    iface.SetBoard( board.get() );
    PNS::ROUTER router;
    router.SetInterface( &iface );
    router.SyncWorld();
    BOOST_REQUIRE( router.GetWorld() );

    // F.Cu is absent from the flex stack (x=45..75 mm), so a route wholly in
    // that bridge must collide with the structure keepout.
    BOOST_CHECK_GT( querySegment( *router.GetWorld(), F_Cu,
                                  VECTOR2I( 50000000, 30000000 ),
                                  VECTOR2I( 53000000, 30000000 ) ), 0u );

    // In1.Cu is present in the flex stack.  This short route does not cross
    // the bend axis, so it must remain routable.
    BOOST_CHECK_EQUAL( querySegment( *router.GetWorld(), In1_Cu,
                                     VECTOR2I( 50000000, 30000000 ),
                                     VECTOR2I( 53000000, 30000000 ) ), 0u );

    // Bend strips are keepouts on every copper layer and prohibit vias.
    PNS::VIA via( VECTOR2I( 55000000, 30000000 ), PNS_LAYER_RANGE( F_Cu, B_Cu ),
                  1000000, 500000, (PNS::NET_HANDLE) 99, VIATYPE::THROUGH );
    PNS::NODE::OBSTACLES obstacles;
    router.GetWorld()->QueryColliding( &via, obstacles );
    BOOST_CHECK_GT( obstacles.size(), 0u );
}
