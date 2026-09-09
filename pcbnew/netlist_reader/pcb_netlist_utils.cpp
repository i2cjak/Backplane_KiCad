/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 */

#include "pcb_netlist_utils.h"

#include <memory>

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <lib_id.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>
#include <project_pcb.h>
#include <reporter.h>


FOOTPRINT* LoadFootprintFromProject( BOARD* aBoard, const LIB_ID& aFootprintId, bool aKeepUuid )
{
    FOOTPRINT* footprint = nullptr;

    try
    {
        FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( nullptr );
        footprint = adapter->LoadFootprintWithOptionalNickname( aFootprintId, aKeepUuid );
    }
    catch( const IO_ERROR& )
    {
    }

    if( footprint )
    {
        footprint->ClearAllNets();

        if( aBoard && !aBoard->IsFootprintHolder() )
        {
            BOARD_DESIGN_SETTINGS& bds = aBoard->GetDesignSettings();
            footprint->ApplyDefaultSettings( *aBoard, bds.m_StyleFPFields, bds.m_StyleFPText,
                                             bds.m_StyleFPShapes, bds.m_StyleFPDimensions,
                                             bds.m_StyleFPBarcodes );
        }
    }

    return footprint;
}


void LoadNetlistFootprints( BOARD* aBoard, NETLIST& aNetlist, REPORTER& aReporter )
{
    if( !aBoard || aNetlist.IsEmpty() )
        return;

    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( nullptr );

    if( !adapter || adapter->Rows().empty() )
        return;

    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();
    aNetlist.SortByFPID();

    LIB_ID lastFpid;
    std::unique_ptr<FOOTPRINT> cached;

    for( unsigned ii = 0; ii < aNetlist.GetCount(); ++ii )
    {
        COMPONENT* component = aNetlist.GetComponent( ii );

        if( component->GetFPID().GetLibItemName().empty() )
        {
            aReporter.Report( wxString::Format( _( "No footprint defined for symbol %s." ),
                                                component->GetReference() ),
                              RPT_SEVERITY_ERROR );
            continue;
        }

        FOOTPRINT* onBoard = nullptr;

        if( aNetlist.IsFindByTimeStamp() )
        {
            for( const KIID& uuid : component->GetKIIDs() )
            {
                KIID_PATH path = component->GetPath();
                path.push_back( uuid );
                onBoard = aBoard->FindFootprintByPath( path );

                if( onBoard )
                    break;
            }
        }
        else
        {
            onBoard = aBoard->FindFootprintByReference( component->GetReference() );
        }

        bool mismatch = false;

        if( onBoard )
        {
            mismatch = component->GetFPID().IsLegacy()
                               ? onBoard->GetFPID().GetLibItemName()
                                         != component->GetFPID().GetLibItemName()
                               : onBoard->GetFPID() != component->GetFPID();
        }

        if( mismatch && !aNetlist.GetReplaceFootprints() )
        {
            aReporter.Report(
                    wxString::Format( _( "Footprint of %s changed: board footprint '%s', netlist footprint '%s'." ),
                                      component->GetReference(), onBoard->GetFPID().Format().wx_str(),
                                      component->GetFPID().Format().wx_str() ),
                    RPT_SEVERITY_WARNING );
            continue;
        }

        if( !aNetlist.GetReplaceFootprints() || ( onBoard && !mismatch ) )
            continue;

        if( component->GetFPID() != lastFpid )
        {
            cached.reset( LoadFootprintFromProject( aBoard, component->GetFPID() ) );
            lastFpid = component->GetFPID();
        }

        if( cached )
        {
            FOOTPRINT* footprint = new FOOTPRINT( *cached );
            footprint->ResetUuidDirect();
            component->SetFootprint( footprint );
        }
        else
            aReporter.Report(
                    wxString::Format( _("%s footprint '%s' not found in any libraries in the footprint library table."),
                                      component->GetReference(),
                                      component->GetFPID().GetLibItemName().wx_str() ),
                    RPT_SEVERITY_ERROR );
    }
}
