/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sketch_route_board.h"

#include <board.h>
#include <board_design_settings.h>
#include <commit.h>
#include <drc/drc_engine.h>
#include <netinfo.h>
#include <pcb_track.h>
#include <project.h>
#include <wildcards_and_files_ext.h>

#include "pns_arc.h"
#include "pns_kicad_iface.h"
#include "pns_node.h"
#include "pns_router.h"
#include "pns_routing_settings.h"
#include "pns_segment.h"
#include "pns_via.h"


SKETCH_ROUTE_RESULT RouteSketchOnBoard( BOARD* aBoard, const SKETCH_ROUTE_REQUEST& aRequest,
                                        COMMIT* aCommit )
{
    SKETCH_ROUTE_RESULT result;

    if( !aBoard )
        return result;

    // The router takes clearances and widths from the board's DRC engine, which a board loaded
    // without an editor doesn't have yet.  Set one up the way the zone filler does.
    BOARD_DESIGN_SETTINGS& bds = aBoard->GetDesignSettings();

    if( !bds.m_DRCEngine )
    {
        std::shared_ptr<DRC_ENGINE> drcEngine = std::make_shared<DRC_ENGINE>( aBoard, &bds );
        wxString                    rulesPath;

        if( !aBoard->GetFileName().IsEmpty() && aBoard->GetProject() )
        {
            wxFileName fn( aBoard->GetFileName() );
            fn.SetExt( FILEEXT::DesignRulesFileExtension );
            rulesPath = aBoard->GetProject()->AbsolutePath( fn.GetFullName() );
        }

        try
        {
            drcEngine->InitEngine( wxFileName( rulesPath ) );
        }
        catch( ... )
        {
            // Custom rules that fail to compile only matter to DRC; the implicit constraints
            // from the board setup still apply
        }

        bds.m_DRCEngine = drcEngine;
    }

    // Nor the ratsnest, which is where the unrouted connections come from
    aBoard->BuildConnectivity();

    PNS::ROUTER           router;
    PNS_KICAD_IFACE_BASE  iface;
    PNS::ROUTING_SETTINGS settings( nullptr, "" );

    iface.SetBoard( aBoard );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    router.LoadSettings( &settings );
    settings.SetCornerMode( aRequest.m_cornerMode );

    PNS::SKETCH_ROUTER          sketch( &router );
    PNS::SKETCH_ROUTER_OPTIONS& options = sketch.Options();

    options.m_layers = PNS::SKETCH_ROUTER::RoutableLayers( aBoard, &iface );
    options.m_boundary = aBoard->GetBoardEdgesBoundingBox();
    options.m_guide = aRequest.m_guide;
    options.m_allowVias = aRequest.m_allowVias;

    if( aRequest.m_timeLimitMs > 0 )
        options.m_timeLimitMs = aRequest.m_timeLimitMs;

    if( IsCopperLayer( aRequest.m_preferredLayer ) )
        options.m_preferredLayer = iface.GetPNSLayerFromBoardLayer( aRequest.m_preferredLayer );

    if( aRequest.m_progress )
        sketch.SetProgressCallback( aRequest.m_progress );

    if( aRequest.m_configure )
        aRequest.m_configure( options );

    std::vector<PNS::SKETCH_CONNECTION> connections =
            PNS::SKETCH_ROUTER::CollectConnections( aBoard, router.GetWorld(), aRequest.m_items );

    result.m_connections = (int) connections.size();

    if( connections.empty() )
        return result;

    std::unique_ptr<PNS::NODE> branch( sketch.Route( router.GetWorld(), connections ) );
    result.m_stats = sketch.Stats();

    if( !branch )
        return result;

    // The sketch router only ever adds items
    PNS::NODE::ITEM_VECTOR removed, added;
    branch->GetUpdatedItems( removed, added );

    for( PNS::ITEM* item : added )
    {
        NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( item->Net() );
        BOARD_ITEM*   boardItem = nullptr;

        switch( item->Kind() )
        {
        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( item );
            PCB_TRACK*    track = new PCB_TRACK( aBoard );
            track->SetStart( seg->Seg().A );
            track->SetEnd( seg->Seg().B );
            track->SetWidth( seg->Width() );
            track->SetLayer( iface.GetBoardLayerFromPNSLayer( seg->Layer() ) );
            track->SetNet( net );
            boardItem = track;
            break;
        }

        case PNS::ITEM::ARC_T:
        {
            PNS::ARC* arc = static_cast<PNS::ARC*>( item );
            PCB_ARC*  pcbArc = new PCB_ARC( aBoard, static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) ) );
            pcbArc->SetWidth( arc->Width() );
            pcbArc->SetLayer( iface.GetBoardLayerFromPNSLayer( arc->Layer() ) );
            pcbArc->SetNet( net );
            boardItem = pcbArc;
            break;
        }

        case PNS::ITEM::VIA_T:
        {
            PNS::VIA* via = static_cast<PNS::VIA*>( item );
            PCB_VIA*  pcbVia = new PCB_VIA( aBoard );
            pcbVia->SetPosition( via->Pos() );
            pcbVia->SetWidth( PADSTACK::ALL_LAYERS, via->Diameter( 0 ) );
            pcbVia->SetDrill( via->Drill() );
            pcbVia->SetViaType( via->ViaType() );
            pcbVia->SetLayerPair( iface.GetBoardLayerFromPNSLayer( via->Layers().Start() ),
                                  iface.GetBoardLayerFromPNSLayer( via->Layers().End() ) );
            pcbVia->SetNet( net );
            boardItem = pcbVia;
            break;
        }

        default:
            break;
        }

        if( !boardItem )
            continue;

        if( aCommit )
            aCommit->Add( boardItem );
        else
            aBoard->Add( boardItem );

        result.m_added.push_back( boardItem );
    }

    return result;
}
