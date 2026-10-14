// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_structure_tool.h"
#include <board.h>
#include <board_structure.h>
#include <pcb_edit_frame.h>
#include <pcb_draw_panel_gal.h>
#include <dialogs/dialog_structure_properties.h>
#include <geometry/shape_poly_set.h>
#include <geometry/seg.h>
#include <geometry/shape_poly_set.h>
#include <view/view_controls.h>
#include <tools/pcb_grid_helper.h>
#include <board_construction_layer.h>
#include <wx/choicdlg.h>
#include <wx/msgdlg.h>
#include <algorithm>

namespace
{
bool inside( const BOARD_STRUCTURE_POLYGON& polygon, VECTOR2I point )
{
    SHAPE_POLY_SET shape;
    shape.NewOutline();
    for( const auto& p : polygon.outline ) shape.Append( p.x, p.y );
    for( const auto& hole : polygon.holes )
    {
        int h = shape.NewHole();
        for( const auto& p : hole ) shape.Append( p.x, p.y, 0, h );
    }
    return shape.Contains( point );
}
}

int BOARD_STRUCTURE_TOOL::EditFeature( const TOOL_EVENT& )
{
    auto* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    controls()->ShowCursor( true );
    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) break;
        if( !event->IsClick( BUT_LEFT ) ) { event->SetPassEvent(); continue; }
        const VECTOR2I point = event->HasPosition() ? event->Position() : controls()->GetMousePosition();
        BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
        std::vector<std::pair<int, size_t>> hits;
        wxArrayString labels;
        auto hit = [&]( int category, size_t index, const wxString& label )
        { hits.emplace_back( category, index ); labels.Add( label ); };
        const auto* canvas = editFrame->GetCanvas();
        const CONSTRUCTION_LAYER construction = canvas->GetActiveConstructionLayer();
        if( ( construction == CONSTRUCTION_LAYER::NONE || construction == CONSTRUCTION_LAYER::BEND )
            && canvas->IsStructureVisible( 3 ) )
            for( size_t i = 0; i < next.bends.size(); ++i )
                if( SEG( next.bends[i].start, next.bends[i].end ).Distance( point ) <= view()->ToWorld( 8.0 ) )
                    hit( 3, i, _( "Bend: " ) + wxString::FromUTF8( next.bends[i].id ) );
        if( ( construction == CONSTRUCTION_LAYER::NONE
              || construction == CONSTRUCTION_LAYER::F_STIFFENER
              || construction == CONSTRUCTION_LAYER::B_STIFFENER )
            && canvas->IsStructureVisible( 2 ) )
            for( size_t i = 0; i < next.stiffeners.size(); ++i )
                if( inside( next.stiffeners[i].shape, point )
                    && ( construction != CONSTRUCTION_LAYER::F_STIFFENER
                         || next.stiffeners[i].top )
                    && ( construction != CONSTRUCTION_LAYER::B_STIFFENER
                         || !next.stiffeners[i].top ) )
                    hit( 2, i, _( "Stiffener: " ) + wxString::FromUTF8( next.stiffeners[i].id ) );
        if( construction == CONSTRUCTION_LAYER::NONE )
        for( size_t i = 0; i < next.regions.size(); ++i )
        {
            const auto* stack = next.StackForRegion( next.regions[i] );
            int category = stack && stack->kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? 1 : 0;
            if( canvas->IsStructureVisible( category ) && inside( next.regions[i].shape, point ) )
                hit( 0, i, _( "Region: " ) + wxString::FromUTF8( next.regions[i].name ) );
        }
        if( hits.empty() ) continue;
        int selected = 0;
        if( hits.size() > 1 )
        {
            wxSingleChoiceDialog chooser( editFrame, _( "Choose the feature to edit:" ),
                                          _( "Structure Selection" ), labels );
            if( chooser.ShowModal() != wxID_OK ) break;
            selected = chooser.GetSelection();
        }
        auto [category, index] = hits.at( selected );
        if( EditStructureFeatureProperties( editFrame, next, category, index ) )
            editFrame->CommitBoardStructure( next, _( "Edit structure feature" ) );
        break;
    }
    return 0;
}

int BOARD_STRUCTURE_TOOL::EditBoundary( const TOOL_EVENT& )
{
    auto* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    const int tolerance = view()->ToWorld( 10.0 );
    PCB_GRID_HELPER grid( m_toolMgr, editFrame->GetMagneticItemsSettings() );
    LSET layers;
    layers.set();

    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) break;
        if( !event->IsClick( BUT_LEFT ) && !event->IsClick( BUT_RIGHT ) )
        { event->SetPassEvent(); continue; }

        const VECTOR2I point = event->HasPosition() ? event->Position() : controls()->GetMousePosition();
        BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
        enum class KIND { BOUNDARY } kind = KIND::BOUNDARY;
        size_t item = 0, vertex = 0;
        int best = tolerance;
        bool found = false;
        auto consider = [&]( KIND k, size_t i, size_t v, const VECTOR2I& candidate )
        {
            const int distance = candidate.Distance( point );
            if( distance <= best ) { best = distance; kind = k; item = i; vertex = v; found = true; }
        };
        for( size_t i = 0; i < next.boundaries.size(); ++i )
            for( size_t v = 0; v < next.boundaries[i].polyline.size(); ++v )
                consider( KIND::BOUNDARY, i, v, next.boundaries[i].polyline[v] );
        if( !found ) continue;

        // A right click removes a vertex only when the resulting polygon is
        // still valid.  All changes are staged in a copy and committed once.
        if( event->IsClick( BUT_RIGHT ) )
        {
            auto& line = next.boundaries[item].polyline;
            if( line.size() <= 2 )
                next.boundaries.erase( next.boundaries.begin() + item );
            else
                line.erase( line.begin() + vertex );
            SHAPE_POLY_SET outline;
            if( !editFrame->GetBoard()->GetBoardPolygonOutlines( outline, false ) )
            {
                wxMessageBox( _( "The board has no valid Edge.Cuts outline." ),
                              _( "Invalid Structure Boundary" ), wxOK | wxICON_ERROR, editFrame );
                continue;
            }
            std::string error;
            if( !next.DerivePartition( outline, &error ) )
            {
                wxMessageBox( wxString::FromUTF8( error ), _( "Invalid Structure Boundary" ),
                              wxOK | wxICON_ERROR, editFrame );
                continue;
            }
            editFrame->CommitBoardStructure( next, _( "Delete structure boundary vertex" ) );
            break;
        }

        // Select a vertex, then click its destination. Escape leaves the
        // staged copy untouched.
        while( TOOL_EVENT* move = Wait() )
        {
            if( move->IsCancelInteractive() || move->IsActivate() ) return 0;
            if( !move->IsClick( BUT_LEFT ) ) { move->SetPassEvent(); continue; }
            const VECTOR2I destination = grid.ResolveSnap(
                    move->HasPosition() ? move->Position() : controls()->GetMousePosition(), layers,
                    GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            VECTOR2I snapped = destination;
            int snapDistance = tolerance;
            for( const auto& boundary : next.boundaries )
            {
                for( size_t i = 0; i < boundary.polyline.size(); ++i )
                {
                    const int d = boundary.polyline[i].Distance( destination );
                    if( d <= snapDistance ) { snapDistance = d; snapped = boundary.polyline[i]; }
                    if( i + 1 < boundary.polyline.size() )
                    {
                        const SEG segment( boundary.polyline[i], boundary.polyline[i + 1] );
                        const VECTOR2I candidate = segment.NearestPoint( destination );
                        const int sd = candidate.Distance( destination );
                        if( sd <= snapDistance ) { snapDistance = sd; snapped = candidate; }
                    }
                }
            }
            next.boundaries[item].polyline[vertex] = snapped;
            SHAPE_POLY_SET outline;
            if( !editFrame->GetBoard()->GetBoardPolygonOutlines( outline, false )
                || !next.DerivePartition( outline ) )
            {
                wxMessageBox( _( "The new vertex would make the structure invalid." ),
                              _( "Invalid Structure Boundary" ), wxOK | wxICON_ERROR, editFrame );
                return 0;
            }
            editFrame->CommitBoardStructure( next, _( "Move structure boundary vertex" ) );
            return 0;
        }
    }
    return 0;
}
