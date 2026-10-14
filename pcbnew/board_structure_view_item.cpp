#include "board_structure_view_item.h"

#include <board.h>
#include <board_structure.h>
#include <geometry/shape_poly_set.h>
#include <gal/graphics_abstraction_layer.h>
#include <layer_ids.h>
#include <view/view.h>
#include <geometry/eda_angle.h>

#include <cmath>

namespace
{
constexpr int STRUCTURE_LAYER = LAYER_GP_OVERLAY;

SHAPE_POLY_SET polySet( const BOARD_STRUCTURE_POLYGON& aShape )
{
    SHAPE_POLY_SET result;
    if( aShape.outline.size() < 3 )
        return result;

    SHAPE_LINE_CHAIN outline;
    for( const VECTOR2I& point : aShape.outline ) outline.Append( point );
    outline.SetClosed( true );
    result.AddOutline( outline );

    for( const std::vector<VECTOR2I>& hole : aShape.holes )
    {
        SHAPE_LINE_CHAIN ring;
        for( const VECTOR2I& point : hole ) ring.Append( point );
        ring.SetClosed( true );
        result.AddHole( ring, 0 );
    }
    return result;
}

void drawShape( KIGFX::GAL* aGal, const BOARD_STRUCTURE_POLYGON& aShape,
                const KIGFX::COLOR4D& aColor )
{
    SHAPE_POLY_SET poly = polySet( aShape );
    if( poly.IsEmpty() ) return;
    poly.CacheTriangulation();
    aGal->AdvanceDepth();
    aGal->SetFillColor( aColor.WithAlpha( 0.16 ) );
    aGal->SetStrokeColor( aColor.WithAlpha( 0.9 ) );
    aGal->SetIsFill( true );
    aGal->SetIsStroke( true );
    aGal->SetLineWidth( 120000 );
    aGal->DrawPolygon( poly );
}
}

BOARD_STRUCTURE_VIEW_ITEM::BOARD_STRUCTURE_VIEW_ITEM( BOARD* aBoard ) :
        EDA_ITEM( NOT_USED ), m_board( aBoard )
{
}

const BOX2I BOARD_STRUCTURE_VIEW_ITEM::ViewBBox() const
{
    BOX2I result;
    bool initialized = false;
    if( !m_board ) return result;
    auto addPoint = [&]( const VECTOR2I& aPoint )
    {
        if( !initialized ) { result.SetOrigin( aPoint ); result.SetEnd( aPoint ); initialized = true; }
        else result.Merge( aPoint );
    };
    const BOARD_STRUCTURE& structure = m_board->GetBoardStructure();
    for( const auto& region : structure.regions ) for( const auto& point : region.shape.outline ) addPoint( point );
    for( const auto& stiffener : structure.stiffeners ) for( const auto& point : stiffener.shape.outline ) addPoint( point );
    for( const auto& bend : structure.bends ) { addPoint( bend.start ); addPoint( bend.end ); }
    for( const auto& boundary : structure.boundaries )
        for( const auto& point : boundary.polyline ) addPoint( point );
    return result;
}

void BOARD_STRUCTURE_VIEW_ITEM::ViewDraw( int aLayer, KIGFX::VIEW* aView ) const
{
    if( aLayer != STRUCTURE_LAYER || !m_board ) return;
    const BOARD_STRUCTURE& structure = m_board->GetBoardStructure();
    KIGFX::GAL* gal = aView->GetGAL();
    KIGFX::GAL_SCOPED_ATTRS attributes( *gal, KIGFX::GAL_SCOPED_ATTRS::ALL_ATTRS );
    const KIGFX::COLOR4D rigid( 0.95, 0.55, 0.12, 1.0 ), flex( 0.18, 0.75, 0.95, 1.0 );
    const KIGFX::COLOR4D stiffener( 0.72, 0.35, 0.95, 1.0 ), bendColor( 1.0, 0.9, 0.15, 1.0 );

    for( const auto& region : structure.regions )
    {
        const auto* stack = structure.StackForRegion( region );
        const bool isFlex = stack && stack->kind == BOARD_STRUCTURE_STACK_KIND::FLEX;
        if( m_visible[isFlex ? FLEX : RIGID] )
        {
            drawShape( gal, region.shape, isFlex ? flex : rigid );

            if( !region.shape.outline.empty() )
            {
                const VECTOR2I labelPos = region.shape.outline.front();
                const wxString kind = isFlex ? wxT( "FLEX" ) : wxT( "RIGID" );
                const wxString label = wxString::FromUTF8( region.name ) + wxT( " [" ) + kind + wxT( "]" );
                gal->SetIsFill( false );
                gal->SetIsStroke( true );
                gal->SetStrokeColor( ( isFlex ? flex : rigid ).WithAlpha( 0.95 ) );
                gal->AdvanceDepth();
                const int labelSize = std::max( 700000, static_cast<int>( aView->ToWorld( 11.0 ) ) );
                gal->SetGlyphSize( VECTOR2I( labelSize, labelSize ) );
                gal->SetLineWidth( 90000 );
                gal->BitmapText( label, labelPos, ANGLE_HORIZONTAL );
            }
        }
    }
    if( m_visible[STIFFENER] )
        for( const auto& item : structure.stiffeners )
            if( m_visible[item.top ? F_STIFFENER : B_STIFFENER] )
                drawShape( gal, item.shape,
                           item.top ? stiffener : KIGFX::COLOR4D( 0.95, 0.38, 0.68, 1.0 ) );

    if( m_visible[BOUNDARY] )
    {
        const KIGFX::COLOR4D color( 0.15, 0.85, 0.85, 1.0 );
        gal->AdvanceDepth();
        gal->SetIsFill( true );
        gal->SetIsStroke( true );
        gal->SetFillColor( color );
        gal->SetStrokeColor( color );
        gal->SetLineWidth( 180000 );
        for( const auto& boundary : structure.boundaries )
            for( size_t i = 1; i < boundary.polyline.size(); ++i )
                gal->DrawSegment( VECTOR2D( boundary.polyline[i - 1] ),
                                  VECTOR2D( boundary.polyline[i] ), 180000 );
    }

    if( !m_visible[BEND] ) return;
    gal->AdvanceDepth();
    gal->SetIsFill( true ); gal->SetFillColor( bendColor );
    gal->SetIsStroke( true ); gal->SetStrokeColor( bendColor ); gal->SetLineWidth( 180000 );
    for( const auto& item : structure.bends )
    {
        gal->DrawSegment( VECTOR2D( item.start.x, item.start.y ),
                          VECTOR2D( item.end.x, item.end.y ), 180000 );
        const VECTOR2D delta( item.end.x - item.start.x, item.end.y - item.start.y );
        const double length = std::hypot( delta.x, delta.y );
        if( length <= 1.0 ) continue;
        const VECTOR2D unit = delta / length, normal( -unit.y, unit.x );
        const VECTOR2D tip( item.start.x + delta.x * 0.55, item.start.y + delta.y * 0.55 );
        constexpr double arrowLength = 1000000.0, arrowWidth = 400000.0;
        const VECTOR2I left( tip.x - unit.x * arrowLength + normal.x * arrowWidth,
                             tip.y - unit.y * arrowLength + normal.y * arrowWidth );
        const VECTOR2I right( tip.x - unit.x * arrowLength - normal.x * arrowWidth,
                              tip.y - unit.y * arrowLength - normal.y * arrowWidth );
        gal->DrawSegment( VECTOR2D( tip.x, tip.y ), VECTOR2D( left.x, left.y ), 180000 );
        gal->DrawSegment( VECTOR2D( tip.x, tip.y ), VECTOR2D( right.x, right.y ), 180000 );

        // The perpendicular arrow identifies which side of the axis moves.
        const double side = item.moving_side_left ? 1.0 : -1.0;
        const VECTOR2D sideTip( item.start.x + delta.x * 0.5 + normal.x * side * 1600000.0,
                                item.start.y + delta.y * 0.5 + normal.y * side * 1600000.0 );
        const VECTOR2D sideBase( item.start.x + delta.x * 0.5,
                                 item.start.y + delta.y * 0.5 );
        const VECTOR2D sideUnit( normal.x * side, normal.y * side );
        const VECTOR2D sideLeft( sideTip.x - sideUnit.x * 450000.0 + unit.x * 250000.0,
                                 sideTip.y - sideUnit.y * 450000.0 + unit.y * 250000.0 );
        const VECTOR2D sideRight( sideTip.x - sideUnit.x * 450000.0 - unit.x * 250000.0,
                                  sideTip.y - sideUnit.y * 450000.0 - unit.y * 250000.0 );
        gal->DrawSegment( sideBase, sideTip, 180000 );
        gal->DrawSegment( sideTip, sideLeft, 180000 );
        gal->DrawSegment( sideTip, sideRight, 180000 );

        const wxString bendLabel = wxString::FromUTF8( item.id )
                                  + wxString::Format( wxT( "  %.1f deg  R%.2f  #%d" ),
                                                      item.angle_degrees, item.inner_radius_mm, item.sequence );
        gal->AdvanceDepth();
        const int labelSize = std::max( 600000, static_cast<int>( aView->ToWorld( 11.0 ) ) );
        gal->SetGlyphSize( VECTOR2I( labelSize, labelSize ) );
        gal->BitmapText( bendLabel, VECTOR2I( sideTip.x, sideTip.y ), ANGLE_HORIZONTAL );
    }
}

std::vector<int> BOARD_STRUCTURE_VIEW_ITEM::ViewGetLayers() const { return { STRUCTURE_LAYER }; }
void BOARD_STRUCTURE_VIEW_ITEM::SetVisible( int aCategory, bool aVisible )
{ if( aCategory >= 0 && aCategory < CATEGORY_COUNT ) m_visible[aCategory] = aVisible; }
bool BOARD_STRUCTURE_VIEW_ITEM::IsVisible( int aCategory ) const
{ return aCategory >= 0 && aCategory < CATEGORY_COUNT && m_visible[aCategory]; }
