/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <board.h>
#include <board_structure.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_test_provider.h>
#include <pcb_track.h>
#include <footprint.h>
#include <pad.h>
#include <zone.h>
#include <geometry/shape_poly_set.h>
#include <algorithm>
#include <map>

namespace
{
SHAPE_POLY_SET polygon( const BOARD_STRUCTURE_POLYGON& shape )
{
    SHAPE_POLY_SET result;
    if( shape.outline.size() < 3 ) return result;
    SHAPE_LINE_CHAIN chain;
    for( const auto& p : shape.outline ) chain.Append( p );
    chain.SetClosed( true );
    result.AddOutline( chain );
    for( const auto& hole : shape.holes )
    {
        SHAPE_LINE_CHAIN inner;
        for( const auto& p : hole ) inner.Append( p );
        inner.SetClosed( true );
        result.AddHole( inner, 0 );
    }
    return result;
}
bool positiveArea( SHAPE_POLY_SET shape ) { return !shape.IsEmpty() && std::abs( shape.Area() ) > 1.0; }
}

class DRC_TEST_PROVIDER_BOARD_STRUCTURE : public DRC_TEST_PROVIDER
{
public:
    const wxString GetName() const override { return wxS( "board_structure" ); }
    bool Run() override;
private:
    void report( const wxString& message, const VECTOR2I& position, BOARD_ITEM* source = nullptr )
    {
        auto item = DRC_ITEM::Create( DRCE_GENERIC_ERROR );
        item->SetErrorMessage( message );
        if( source ) item->SetItems( source );
        reportViolation( item, position, source ? source->GetLayer() : UNDEFINED_LAYER );
    }
};

bool DRC_TEST_PROVIDER_BOARD_STRUCTURE::Run()
{
    BOARD* board = m_drcEngine->GetBoard();
    const BOARD_STRUCTURE& structure = board->GetBoardStructure();
    if( structure.Empty() ) return true;
    if( !reportPhase( _( "Checking flex and rigid-flex structure..." ) ) ) return false;
    const auto validation = structure.Validate();
    for( const auto& error : validation.errors ) report( wxString::FromUTF8( error ), {} );
    if( !validation.IsValid() ) return !m_drcEngine->IsCancelled();

    SHAPE_POLY_SET allRegions;
    std::map<int, SHAPE_POLY_SET> copperDomains;
    std::map<std::string, SHAPE_POLY_SET> regions;
    for( const auto& region : structure.regions )
    {
        SHAPE_POLY_SET shape = polygon( region.shape );
        SHAPE_POLY_SET overlap = shape;
        overlap.BooleanIntersection( allRegions );
        if( positiveArea( overlap ) )
            report( wxString::Format( _( "Physical region overlaps another region: %s" ),
                                      wxString::FromUTF8( region.name ) ), region.shape.outline.front() );
        allRegions.BooleanAdd( shape );
        regions[region.id] = shape;
        if( const auto* stack = structure.StackForRegion( region ) )
            for( int layer : stack->copper_layers ) copperDomains[layer].BooleanAdd( shape );
    }
    SHAPE_POLY_SET outline;
    if( board->GetBoardPolygonOutlines( outline, false ) )
    {
        SHAPE_POLY_SET difference = allRegions;
        difference.BooleanXor( outline );
        if( positiveArea( difference ) )
            report( _( "Physical regions must cover the board outline without gaps or excess material" ),
                    difference.Outline(0).CPoint(0) );
    }
    // Check the entire copper shape against the union of valid regions. This
    // treats touching shared boundaries as legal and also catches uncovered gaps.
    auto checkCopper = [&]( BOARD_ITEM* item )
    {
        for( PCB_LAYER_ID layer : LSET( item->GetLayerSet() & board->GetEnabledLayers() & LSET::AllCuMask() ).Seq() )
        {
            SHAPE_POLY_SET shape;
            item->TransformShapeToPolygon( shape, layer, 0, 1000, ERROR_OUTSIDE );
            shape.BooleanSubtract( copperDomains[layer] );
            if( positiveArea( shape ) )
                report( wxString::Format( _( "Copper extends outside regional layer %s" ),
                                          board->GetLayerName( layer ) ), item->GetPosition(), item );
        }
    };
    for( PCB_TRACK* track : board->Tracks() ) checkCopper( track );
    for( FOOTPRINT* fp : board->Footprints() ) for( PAD* pad : fp->Pads() ) checkCopper( pad );
    for( ZONE* zone : board->Zones() )
    {
        if( zone->GetIsRuleArea() ) continue;
        for( PCB_LAYER_ID layer : LSET( zone->GetLayerSet() & LSET::AllCuMask() ).Seq() )
        {
            const auto filled = zone->GetFilledPolysList( layer );
            if( !filled ) continue;
            SHAPE_POLY_SET excess = *filled;
            excess.BooleanSubtract( copperDomains[layer] );
            if( positiveArea( excess ) ) report( _( "Zone fill exceeds its regional copper extent; refill zones" ), zone->GetPosition(), zone );
        }
    }
    std::vector<SHAPE_POLY_SET> bendStrips;
    for( const auto& bend : structure.bends )
    {
        double thickness = 0;
        for( const auto& region : structure.regions )
            if( region.id == bend.region_id )
                if( const auto* stack = structure.StackForRegion( region ) )
                    for( const auto& material : stack->materials ) thickness += material.thickness_nm * 1e-6;
        const VECTOR2D delta( bend.end.x - bend.start.x, bend.end.y - bend.start.y );
        const double length = std::hypot( delta.x, delta.y );
        if( length <= 0.0 ) continue;
        const double width = BOARD_STRUCTURE::BendStripLengthMm( bend, thickness ) * 1e6;
        const double side = bend.moving_side_left ? 1.0 : -1.0;
        const VECTOR2I shift( std::lround( -delta.y / length * width * side ),
                              std::lround( delta.x / length * width * side ) );
        BOARD_STRUCTURE_POLYGON rectangle;
        rectangle.outline = { bend.start, bend.end, bend.end + shift, bend.start + shift };
        SHAPE_POLY_SET strip = polygon( rectangle );
        SHAPE_POLY_SET excess = strip;
        excess.BooleanSubtract( regions[bend.region_id] );
        if( positiveArea( excess ) ) report( _( "Bend strip extends outside its flex region" ), bend.start );
        for( const auto& other : bendStrips )
        {
            SHAPE_POLY_SET overlap = strip;
            overlap.BooleanIntersection( other );
            if( positiveArea( overlap ) ) report( _( "Bend strips overlap" ), bend.start );
        }
        bendStrips.push_back( strip );
        auto checkBend = [&]( BOARD_ITEM* item )
        {
            for( PCB_LAYER_ID layer : LSET( item->GetLayerSet() & board->GetEnabledLayers() & LSET::AllCuMask() ).Seq() )
            {
                SHAPE_POLY_SET shape;
                item->TransformShapeToPolygon( shape, layer, 0, 1000, ERROR_OUTSIDE );
                shape.BooleanIntersection( strip );
                if( positiveArea( shape ) )
                {
                    report( _( "Pad or via intersects an active bend strip" ), item->GetPosition(), item );
                    break;
                }
            }
        };
        for( PCB_TRACK* track : board->Tracks() ) if( track->Type() == PCB_VIA_T ) checkBend( track );
        for( FOOTPRINT* fp : board->Footprints() ) for( PAD* pad : fp->Pads() ) checkBend( pad );
    }
    for( const auto& stiffener : structure.stiffeners )
    {
        SHAPE_POLY_SET shape = polygon( stiffener.shape );
        SHAPE_POLY_SET excess = shape;
        excess.BooleanSubtract( regions[stiffener.region_id] );
        if( positiveArea( excess ) ) report( _( "Stiffener extends outside its parent region" ), stiffener.shape.outline.front() );
        for( const auto& strip : bendStrips )
        {
            SHAPE_POLY_SET overlap = shape;
            overlap.BooleanIntersection( strip );
            if( positiveArea( overlap ) ) report( _( "Stiffener intersects an active bend strip" ), stiffener.shape.outline.front() );
        }
    }
    return !m_drcEngine->IsCancelled();
}
static DRC_REGISTER_TEST_PROVIDER<DRC_TEST_PROVIDER_BOARD_STRUCTURE> dummy;
