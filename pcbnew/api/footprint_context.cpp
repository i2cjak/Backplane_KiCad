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

#include <api/footprint_context.h>

#include <board.h>
#include <footprint_edit_frame.h>
#include <ki_exception.h>
#include <pcb_io/pcb_io.h>
#include <pcb_io/pcb_io_mgr.h>


class FOOTPRINT_EDIT_FRAME_CONTEXT : public FOOTPRINT_CONTEXT
{
public:
    explicit FOOTPRINT_EDIT_FRAME_CONTEXT( FOOTPRINT_EDIT_FRAME* aFrame ) :
            m_frame( aFrame )
    {
    }

    BOARD* GetBoard() const override
    {
        return m_frame->GetBoard();
    }

    PROJECT& Prj() const override
    {
        return m_frame->Prj();
    }

    TOOL_MANAGER* GetToolManager() const override
    {
        return m_frame->GetToolManager();
    }

    KIWAY* GetKiway() const override
    {
        return &m_frame->Kiway();
    }

    wxString GetCurrentFileName() const override
    {
        return m_frame->GetCurrentFileName();
    }

    bool CanAcceptApiCommands() const override
    {
        return m_frame->CanAcceptApiCommands();
    }

    bool SaveBoard() override
    {
        bool saved = m_frame->SaveFootprint( m_frame->GetBoard()->GetFirstFootprint() );

        if( saved && m_frame->GetScreen() )
            m_frame->GetScreen()->SetContentModified( false );

        if( saved && m_frame->GetBoard() )
            m_frame->GetBoard()->ClearFlags( IS_CHANGED );

        return saved;
    }

    bool SavePcbCopy( const wxString& aFileName, bool, bool ) override
    {
        FOOTPRINT* footprint = m_frame->GetBoard()->GetFirstFootprint();

        if( !footprint || aFileName.IsEmpty() )
            return false;

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

    LIB_ID GetLoadedFPID() const override
    {
        return m_frame->GetLoadedFPID();
    }

    bool OpenFootprint( const LIB_ID& aFPID ) override
    {
        m_frame->LoadFootprintFromLibrary( aFPID );
        return true;
    }

    bool RevertFootprint() override
    {
        m_frame->GetScreen()->SetContentModified( false );
        return m_frame->RevertFootprint();
    }

    bool SaveFootprint( FOOTPRINT* aFootprint ) override
    {
        return m_frame->SaveFootprint( aFootprint );
    }

    bool SaveFootprintInLibrary( FOOTPRINT* aFootprint, const wxString& aLibraryName ) override
    {
        return m_frame->SaveFootprintInLibrary( aFootprint, aLibraryName );
    }

private:
    FOOTPRINT_EDIT_FRAME* m_frame;
};


std::shared_ptr<FOOTPRINT_CONTEXT> CreateFootprintFrameContext( FOOTPRINT_EDIT_FRAME* aFrame )
{
    return std::make_shared<FOOTPRINT_EDIT_FRAME_CONTEXT>( aFrame );
}
