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

#include <api/headless_footprint_context.h>
#include <board.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <pcb_io/pcb_io_mgr.h>
#include <pcb_io/pcb_io.h>
#include <project.h>
#include <project_pcb.h>
#include <tool/tool_manager.h>
#include <wx/debug.h>


HEADLESS_FOOTPRINT_CONTEXT::HEADLESS_FOOTPRINT_CONTEXT( std::unique_ptr<FOOTPRINT> aFootprint,
                                                        const LIB_ID& aFPID,
                                                        PROJECT* aProject,
                                                        APP_SETTINGS_BASE* aSettings,
                                                        KIWAY* aKiway,
                                                        const wxString& aFileName ) :
        m_board( std::make_unique<BOARD>() ),
        m_fpid( aFPID ),
        m_fileName( aFileName ),
        m_project( aProject ),
        m_kiway( aKiway ),
        m_toolManager( std::make_unique<TOOL_MANAGER>() )
{
    wxCHECK( aFootprint, /* void */ );
    wxCHECK( m_project, /* void */ );

    m_board->SetBoardUse( BOARD_USE::FPHOLDER );
    m_board->SetProject( m_project );
    aFootprint->SetParent( m_board.get() );
    m_board->Add( aFootprint.release() );
    m_toolManager->SetEnvironment( m_board.get(), nullptr, nullptr, aSettings, nullptr );
}


HEADLESS_FOOTPRINT_CONTEXT::~HEADLESS_FOOTPRINT_CONTEXT() = default;


BOARD* HEADLESS_FOOTPRINT_CONTEXT::GetBoard() const
{
    return m_board.get();
}


PROJECT& HEADLESS_FOOTPRINT_CONTEXT::Prj() const
{
    wxASSERT( m_project );
    return *m_project;
}


TOOL_MANAGER* HEADLESS_FOOTPRINT_CONTEXT::GetToolManager() const
{
    return m_toolManager.get();
}


wxString HEADLESS_FOOTPRINT_CONTEXT::GetCurrentFileName() const
{
    return m_fileName;
}


LIB_ID HEADLESS_FOOTPRINT_CONTEXT::GetLoadedFPID() const
{
    return m_fpid;
}


bool HEADLESS_FOOTPRINT_CONTEXT::OpenFootprint( const LIB_ID& aFPID )
{
    if( !m_project || !m_board || !aFPID.IsValid() )
        return false;

    try
    {
        FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( m_project );
        std::unique_ptr<FOOTPRINT> footprint(
                adapter->LoadFootprintWithOptionalNickname( aFPID, true ) );

        if( !footprint )
            return false;

        m_board->DeleteAllFootprints();
        footprint->SetParent( m_board.get() );
        m_board->Add( footprint.release() );
        m_fpid = aFPID;
        m_fileName.clear();
        m_board->ClearFlags( IS_CHANGED );
        return true;
    }
    catch( const IO_ERROR& )
    {
        return false;
    }
}


bool HEADLESS_FOOTPRINT_CONTEXT::RevertFootprint()
{
    if( m_fileName.IsEmpty() )
        return OpenFootprint( m_fpid );

    try
    {
        IO_RELEASER<PCB_IO> io( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::KICAD_SEXP ) );
        wxString footprintName;
        std::unique_ptr<FOOTPRINT> footprint( io->ImportFootprint( m_fileName, footprintName ) );

        if( !footprint )
            return false;

        m_board->DeleteAllFootprints();
        footprint->SetParent( m_board.get() );
        m_board->Add( footprint.release() );
        m_fpid = LIB_ID( wxEmptyString, footprintName );
        m_board->ClearFlags( IS_CHANGED );
        return true;
    }
    catch( const IO_ERROR& )
    {
        return false;
    }
}


bool HEADLESS_FOOTPRINT_CONTEXT::SaveFootprint( FOOTPRINT* aFootprint )
{
    if( !aFootprint )
        return false;

    wxString libraryName = m_fpid.GetLibNickname();

    if( libraryName.IsEmpty() )
        return false;

    return SaveFootprintInLibrary( aFootprint, libraryName );
}


bool HEADLESS_FOOTPRINT_CONTEXT::SaveBoard()
{
    if( !m_fileName.IsEmpty() )
    {
        FOOTPRINT* footprint = m_board ? m_board->GetFirstFootprint() : nullptr;

        if( !footprint )
            return false;

        try
        {
            IO_RELEASER<PCB_IO> io( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::KICAD_SEXP ) );
            io->FootprintSave( m_fileName, footprint );
            m_board->ClearFlags( IS_CHANGED );
            return true;
        }
        catch( const IO_ERROR& )
        {
            return false;
        }
    }

    bool saved = SaveFootprint( m_board ? m_board->GetFirstFootprint() : nullptr );

    if( saved && m_board )
        m_board->ClearFlags( IS_CHANGED );

    return saved;
}


bool HEADLESS_FOOTPRINT_CONTEXT::SavePcbCopy( const wxString& aFileName, bool, bool )
{
    if( !m_board || aFileName.IsEmpty() )
        return false;

    FOOTPRINT* footprint = m_board->GetFirstFootprint();

    if( !footprint )
        return false;

    // A footprint copy is a library item, so the path is interpreted as a .kicad_mod output
    // path.  Use the native plugin directly to preserve the caller's requested destination.
    try
    {
        IO_RELEASER<PCB_IO> io( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::KICAD_SEXP ) );
        io->FootprintSave( aFileName, footprint );
        return true;
    }
    catch( const IO_ERROR& )
    {
        return false;
    }
}


bool HEADLESS_FOOTPRINT_CONTEXT::SaveFootprintInLibrary( FOOTPRINT* aFootprint,
                                                         const wxString& aLibraryName )
{
    if( !aFootprint )
        return false;

    const LIB_ID originalFPID = aFootprint->GetFPID();

    try
    {
        aFootprint->SetFPID( LIB_ID( wxEmptyString, aFootprint->GetFPID().GetLibItemName() ) );

        aFootprint->RunOnChildren(
                []( BOARD_ITEM* child )
                {
                    child->ClearFlags();
                },
                RECURSE_MODE::RECURSE );

        FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( m_project );
        if( adapter->SaveFootprint( aLibraryName, aFootprint, true )
            != FOOTPRINT_LIBRARY_ADAPTER::SAVE_OK )
        {
            aFootprint->SetFPID( originalFPID );
            return false;
        }

        aFootprint->SetFPID( LIB_ID( aLibraryName, originalFPID.GetLibItemName() ) );
        m_fpid = aFootprint->GetFPID();

        return true;
    }
    catch( const IO_ERROR& )
    {
        aFootprint->SetFPID( originalFPID );
        return false;
    }
}
