/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <api/headless_board_context.h>
#include <board.h>
#include <pcbnew_scripting_helpers.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <wildcards_and_files_ext.h>
#include <wx/debug.h>
#include <wx/filefn.h>
#include <wx/filename.h>


HEADLESS_BOARD_CONTEXT::HEADLESS_BOARD_CONTEXT( std::unique_ptr<BOARD> aBoard, PROJECT* aProject,
                        APP_SETTINGS_BASE* aSettings,
                        KIWAY* aKiway ) :
        m_board( std::move( aBoard ) ),
        m_project( aProject ),
        m_kiway( aKiway ),
        m_toolManager( std::make_unique<TOOL_MANAGER>() )
{
    wxCHECK( m_board, /* void */ );
    wxCHECK( m_project, /* void */ );

    m_board->SetProject( m_project );
    m_toolManager->SetEnvironment( m_board.get(), nullptr, nullptr, aSettings, nullptr );
}


HEADLESS_BOARD_CONTEXT::~HEADLESS_BOARD_CONTEXT() = default;


BOARD* HEADLESS_BOARD_CONTEXT::GetBoard() const
{
    return m_board.get();
}


PROJECT& HEADLESS_BOARD_CONTEXT::Prj() const
{
    // Shouldn't be able to construct this without a project
    wxASSERT( m_project );
    return *m_project;
}


TOOL_MANAGER* HEADLESS_BOARD_CONTEXT::GetToolManager() const
{
    return m_toolManager.get();
}


wxString HEADLESS_BOARD_CONTEXT::GetCurrentFileName() const
{
    if( !m_board )
        return wxEmptyString;

    return m_board->GetFileName();
}


bool HEADLESS_BOARD_CONTEXT::SaveBoard()
{
    if( !m_board )
        return false;

    wxString fileName = m_board->GetFileName();

    if( fileName.IsEmpty() )
        return false;

    if( !::SaveBoard( fileName, m_board.get(), true ) )
        return false;

    // The scripting helper owns a different settings manager.  IPC documents
    // belong to the application's manager, which must save their live settings.
    return m_project->IsReadOnly()
           || Pgm().GetSettingsManager().SaveProject( wxEmptyString, m_project );
}


bool HEADLESS_BOARD_CONTEXT::SavePcbCopy( const wxString& aFileName, bool aCreateProject, bool aHeadless )
{
    if( !m_board || aFileName.IsEmpty() )
        return false;

    wxString outPath = aFileName;
    if( !::SaveBoard( outPath, m_board.get(), true ) )
        return false;

    if( aCreateProject )
    {
        wxFileName projectFile( aFileName );
        projectFile.SetExt( FILEEXT::ProjectFileExtension );

        // Save a copy without renaming the live project or its open documents.
        if( !projectFile.FileExists() )
            Pgm().GetSettingsManager().SaveProjectCopy( projectFile.GetFullPath(), m_project );

        if( !projectFile.FileExists() )
            return false;

        wxFileName sourceRules( m_project->GetProjectFullName() );
        wxFileName targetRules( aFileName );
        sourceRules.SetExt( FILEEXT::DesignRulesFileExtension );
        targetRules.SetExt( FILEEXT::DesignRulesFileExtension );

        if( sourceRules.FileExists() && !targetRules.FileExists()
            && !wxCopyFile( sourceRules.GetFullPath(), targetRules.GetFullPath(), false ) )
        {
            return false;
        }
    }

    return true;
}
