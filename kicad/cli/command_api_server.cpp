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

#include <csignal>
#include <algorithm>
#include <optional>
#include <ranges>
#include <vector>

#include <api/api_handler_common.h>
#include <api/api_server.h>
#include <api/api_utils.h>
#include <cli/exit_codes.h>
#include <lib_id.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>
#include <wx/app.h>
#include <wx/crt.h>
#include <wx/filename.h>

#include "command_api_server.h"

#define ARG_PATH "path"
#define ARG_SOCKET "--socket"


void apiServerSignalHandler( int )
{
    if( wxTheApp )
        wxTheApp->ExitMainLoop();
}


CLI::API_SERVER_COMMAND::API_SERVER_COMMAND() :
        COMMAND( "api-server" )
{
    m_argParser.add_description( UTF8STDSTR( _( "Run the KiCad IPC API server in headless mode" ) ) );

    m_argParser.add_argument( ARG_PATH )
            .default_value( std::string() )
            .nargs( argparse::nargs_pattern::optional )
            .help( UTF8STDSTR( _( "Optional path to a .kicad_pro, .kicad_pcb, or .kicad_sch file to pre-load" ) ) )
            .metavar( "PROJECT_OR_FILE" );

    m_argParser.add_argument( ARG_SOCKET )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Override API socket path" ) ) )
            .metavar( "SOCKET_PATH" );
}


int CLI::API_SERVER_COMMAND::doPerform( KIWAY& aKiway )
{
    using namespace kiapi::common;

    std::unique_ptr<KICAD_API_SERVER> server = std::make_unique<KICAD_API_SERVER>( false );
    API_HANDLER_COMMON                commonHandler;

    wxString socketPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_SOCKET ) );

    if( !socketPath.IsEmpty() )
        server->SetSocketPath( socketPath );

    // The API server owns one project at a time, but can keep a PCB and
    // schematic document from that project open simultaneously.
    std::optional<wxFileName> openProjectPath;

    struct OPEN_DOCUMENT
    {
        types::DocumentType type;
        wxString            fileName;
        LIB_ID              libId;
    };

    std::vector<OPEN_DOCUMENT> openDocuments;

    auto faceForDocument = []( types::DocumentType aType ) -> KIWAY::FACE_T
    {
        switch( aType )
        {
        case types::DOCTYPE_SCHEMATIC:  return KIWAY::FACE_SCH;
        case types::DOCTYPE_PCB:        return KIWAY::FACE_PCB;
        case types::DOCTYPE_FOOTPRINT:  return KIWAY::FACE_PCB;
        default:                        return KIWAY::KIWAY_FACE_COUNT;
        }
    };

    auto closeAllDocuments =
            [&]( const commands::CloseAllDocuments& aRequest ) -> HANDLER_RESULT<google::protobuf::Empty>
    {
        // Check every child before closing any of them.  This keeps a failed
        // non-forced close atomic and prevents the project from being unloaded
        // while an unsaved child remains inaccessible to the client.
        if( !aRequest.force() )
        {
            for( const OPEN_DOCUMENT& doc : openDocuments )
            {
                if( doc.type == types::DOCTYPE_PROJECT )
                    continue;

                bool     modified = false;
                wxString error;

                if( !aKiway.ProcessApiDocumentIsModified( faceForDocument( doc.type ), doc.fileName,
                                                          &modified, &error ) )
                {
                    ApiResponseStatus e;
                    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    e.set_error_message( wxString::Format( "cannot determine modified state for %s: %s",
                                                           doc.fileName, error ).ToStdString() );
                    return tl::unexpected( e );
                }

                if( modified )
                {
                    ApiResponseStatus e;
                    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    e.set_error_message( wxString::Format( "document %s has unsaved changes; save it or close with force=true",
                                                           doc.fileName ).ToStdString() );
                    return tl::unexpected( e );
                }
            }
        }

        // Close child documents first, then release the project.  Erase each
        // entry only after its face confirms the close so a failed close keeps
        // the lifecycle registry accurate.
        for( auto it = openDocuments.begin(); it != openDocuments.end(); )
        {
            if( it->type == types::DOCTYPE_PROJECT )
            {
                ++it;
                continue;
            }

            wxString error;

            if( !aKiway.ProcessApiCloseDocument( faceForDocument( it->type ), it->fileName,
                                                 server.get(), aRequest.force(), &error ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( error.ToStdString() );
                return tl::unexpected( e );
            }

            it = openDocuments.erase( it );
        }

        if( openProjectPath )
        {
            PROJECT& project = Pgm().GetSettingsManager().Prj();
            Pgm().GetSettingsManager().UnloadProject( &project, false );
        }

        openDocuments.clear();
        openProjectPath.reset();
        return google::protobuf::Empty();
    };

    auto openDocument = [&]( const commands::OpenDocument& aRequest )
            -> HANDLER_RESULT<commands::OpenDocumentResponse>
    {
        types::DocumentType requestType = aRequest.type();

        if( requestType != types::DOCTYPE_PCB
            && requestType != types::DOCTYPE_SCHEMATIC
            && requestType != types::DOCTYPE_PROJECT
            && requestType != types::DOCTYPE_FOOTPRINT )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
            e.set_error_message( "Only PCB, schematic, footprint, and project document types are supported" );
            return tl::unexpected( e );
        }

        wxString inputPath = wxString::FromUTF8( aRequest.path() );

        if( inputPath.IsEmpty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "OpenDocument requires a non-empty path" );
            return tl::unexpected( e );
        }

        if( requestType == types::DOCTYPE_FOOTPRINT )
        {
            wxFileName nativePath( inputPath );
            const bool  nativeFile = nativePath.GetExt().CmpNoCase(
                                              FILEEXT::KiCadFootprintFileExtension ) == 0;
            LIB_ID fpid;

            if( !nativeFile && fpid.Parse( inputPath ) >= 0 )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( wxString::Format( wxS( "Invalid footprint LIB_ID: %s" ),
                                                       inputPath ).ToStdString() );
                return tl::unexpected( e );
            }

            if( nativeFile )
            {
                nativePath.MakeAbsolute();
                fpid = LIB_ID( wxEmptyString, nativePath.GetName() );
            }

            auto existing = std::ranges::find_if( openDocuments,
                                                  [&]( const OPEN_DOCUMENT& d )
                                                  {
                                                      return d.type == types::DOCTYPE_PCB
                                                             || d.type == types::DOCTYPE_FOOTPRINT;
                                                  } );

            if( existing != openDocuments.end() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "a PCB or footprint document is already open" );
                return tl::unexpected( e );
            }

            wxString error;
            wxString projectPathForFootprint;

            if( openProjectPath )
                projectPathForFootprint = openProjectPath->GetFullPath();

            if( !aKiway.ProcessApiOpenFootprint( KIWAY::FACE_PCB, projectPathForFootprint,
                                                 inputPath, server.get(), &error ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( error.ToStdString() );
                return tl::unexpected( e );
            }

            wxString documentIdentity = fpid.GetUniStringLibId();

            if( nativeFile )
            {
                documentIdentity = nativePath.GetFullPath();
            }

            openDocuments.push_back( { requestType, documentIdentity, fpid } );

            commands::OpenDocumentResponse response;
            types::DocumentSpecifier*      doc = response.mutable_document();
            doc->set_type( requestType );
            doc->mutable_lib_id()->set_library_nickname( fpid.GetUniStringLibNickname() );
            doc->mutable_lib_id()->set_entry_name( fpid.GetUniStringLibItemName() );

            if( openProjectPath )
            {
                PROJECT& project = Pgm().GetSettingsManager().Prj();
                doc->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
                doc->mutable_project()->set_path( project.GetProjectDirectory().ToUTF8() );
            }

            return response;
        }

        wxFileName projectPath( inputPath );
        projectPath.SetExt( FILEEXT::ProjectFileExtension );
        projectPath.MakeAbsolute();

        if( openProjectPath && projectPath.GetFullPath() != openProjectPath->GetFullPath() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( "cannot open a document from project '%s' because project '%s' is already open",
                                                   projectPath.GetFullName(), openProjectPath->GetFullName() )
                                         .ToStdString() );
            return tl::unexpected( e );
        }

        if( requestType == types::DOCTYPE_PROJECT )
        {
            if( !openProjectPath )
            {
                if( !openDocuments.empty() )
                {
                    auto closeResult = closeAllDocuments( commands::CloseAllDocuments() );

                    if( !closeResult )
                        return tl::unexpected( closeResult.error() );
                }

                if( !Pgm().GetSettingsManager().LoadProject( projectPath.GetFullPath(), true ) )
                    wxLogTrace( traceApi, "Warning: no project file found for %s", inputPath );

                if( !Pgm().GetSettingsManager().GetProject( projectPath.GetFullPath() ) )
                {
                    ApiResponseStatus e;
                    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    e.set_error_message( wxString::Format( "failed to load project '%s'", projectPath.GetFullPath() )
                                                 .ToStdString() );
                    return tl::unexpected( e );
                }

                openProjectPath = projectPath;
            }

            if( std::ranges::find_if( openDocuments,
                                      []( const OPEN_DOCUMENT& d )
                                      {
                                          return d.type == types::DOCTYPE_PROJECT;
                                      } ) == openDocuments.end() )
            {
                openDocuments.push_back( { types::DOCTYPE_PROJECT, projectPath.GetFullName(), LIB_ID() } );
            }

            commands::OpenDocumentResponse response;
            types::DocumentSpecifier*      doc = response.mutable_document();
            PROJECT&                       project = Pgm().GetSettingsManager().Prj();

            doc->set_type( types::DOCTYPE_PROJECT );
            doc->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
            doc->mutable_project()->set_path( project.GetProjectDirectory().ToUTF8() );
            return response;
        }

        KIWAY::FACE_T face = faceForDocument( requestType );
        wxString      error;

        if( face == KIWAY::KIWAY_FACE_COUNT )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "unsupported document type" );
            return tl::unexpected( e );
        }

        if( requestType == types::DOCTYPE_PCB || requestType == types::DOCTYPE_SCHEMATIC )
        {
            auto existing = std::ranges::find_if( openDocuments,
                                                  [&]( const OPEN_DOCUMENT& d )
                                                  {
                                                      if( requestType == types::DOCTYPE_PCB )
                                                          return d.type == types::DOCTYPE_PCB
                                                                 || d.type == types::DOCTYPE_FOOTPRINT;

                                                      return d.type == requestType;
                                                  } );

            if( existing != openDocuments.end() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( requestType == types::DOCTYPE_PCB
                                             ? "a PCB or footprint document is already open"
                                             : "a document of this type is already open" );
                return tl::unexpected( e );
            }
        }

        if( !aKiway.ProcessApiOpenDocument( face, projectPath.GetFullPath(), server.get(), &error ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( error.ToStdString() );
            return tl::unexpected( e );
        }

        wxFileName docFile( inputPath );
        docFile.MakeAbsolute();
        openDocuments.push_back( { requestType, docFile.GetFullName(), LIB_ID() } );
        openProjectPath = projectPath;

        commands::OpenDocumentResponse response;
        types::DocumentSpecifier*      doc = response.mutable_document();
        PROJECT&                       project = Pgm().GetSettingsManager().Prj();

        doc->set_type( requestType );

        if( requestType == types::DOCTYPE_PCB )
        {
            doc->set_board_filename( docFile.GetFullName().ToStdString() );
        }
        doc->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
        doc->mutable_project()->set_path( project.GetProjectDirectory().ToUTF8() );

        return response;
    };

    auto closeDocument =
            [&]( const commands::CloseDocument& aRequest ) -> HANDLER_RESULT<google::protobuf::Empty>
    {
        if( openDocuments.empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "No document is currently open" );
            return tl::unexpected( e );
        }

        auto it = openDocuments.begin();

        if( aRequest.has_document() )
        {
            it = std::ranges::find_if( openDocuments,
                                       [&]( const OPEN_DOCUMENT& d )
                                       {
                                           return d.type == aRequest.document().type();
                                       } );

            if( it == openDocuments.end() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "Requested document type does not match any open document" );
                return tl::unexpected( e );
            }

            if( aRequest.document().type() == types::DOCTYPE_PCB
                && !aRequest.document().board_filename().empty()
                && it->fileName != wxString::FromUTF8( aRequest.document().board_filename() ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "Requested document does not match the open document" );
                return tl::unexpected( e );
            }

            if( aRequest.document().type() == types::DOCTYPE_SCHEMATIC && aRequest.document().has_project()
                && openProjectPath
                && aRequest.document().project().name() != openProjectPath->GetName().ToStdString() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "Requested document does not match the open project" );
                return tl::unexpected( e );
            }

            if( aRequest.document().type() == types::DOCTYPE_FOOTPRINT
                && aRequest.document().has_lib_id() )
            {
                LIB_ID requested = LibIdFromProto( aRequest.document().lib_id() );

                if( ( !requested.IsValid() && !requested.IsLegacy() ) || requested != it->libId )
                {
                    ApiResponseStatus e;
                    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    e.set_error_message( "Requested footprint does not match the open document" );
                    return tl::unexpected( e );
                }
            }

            if( ( aRequest.document().type() == types::DOCTYPE_SCHEMATIC
                  || aRequest.document().type() == types::DOCTYPE_FOOTPRINT )
                && aRequest.document().has_project() && openProjectPath
                && aRequest.document().project().name() != openProjectPath->GetName().ToStdString() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "Requested document does not match the open project" );
                return tl::unexpected( e );
            }
        }

        if( it->type == types::DOCTYPE_PROJECT )
        {
            // The project has no document face; release it after child documents.
            auto closeResult = closeAllDocuments( commands::CloseAllDocuments() );

            if( !closeResult )
                return tl::unexpected( closeResult.error() );

            return google::protobuf::Empty();
        }

        wxString error;

        if( !aKiway.ProcessApiCloseDocument( faceForDocument( it->type ), it->fileName,
                                             server.get(), false, &error ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( error.ToStdString() );
            return tl::unexpected( e );
        }

        openDocuments.erase( it );

        if( openDocuments.empty() && openProjectPath )
        {
            PROJECT& project = Pgm().GetSettingsManager().Prj();
            Pgm().GetSettingsManager().UnloadProject( &project, false );
            openProjectPath.reset();
        }

        return google::protobuf::Empty();
    };

    commonHandler.SetOpenDocumentHandler( openDocument );
    commonHandler.SetCloseDocumentHandler( closeDocument );
    commonHandler.SetCloseAllDocumentsHandler( closeAllDocuments );

    server->RegisterHandler( &commonHandler );
    server->Start();

    if( !server->Running() )
    {
        wxFprintf( stderr, _( "Failed to start API server\n" ) );
        return EXIT_CODES::ERR_UNKNOWN;
    }

    wxString preloadPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_PATH ) );

    if( !preloadPath.IsEmpty() )
    {
        using namespace kiapi::common;

        wxFileName preloadFile( preloadPath );
        types::DocumentType preloadType = types::DOCTYPE_PROJECT;

        if( preloadFile.GetExt() == FILEEXT::KiCadSchematicFileExtension )
            preloadType = types::DOCTYPE_SCHEMATIC;
        else if( preloadFile.GetExt() == FILEEXT::KiCadPcbFileExtension )
            preloadType = types::DOCTYPE_PCB;

        commands::OpenDocument request;
        request.set_type( preloadType );
        request.set_path( preloadPath.ToStdString() );

        auto preloadResult = openDocument( request );

        if( !preloadResult )
        {
            wxFprintf( stderr, "%s\n", preloadResult.error().error_message() );
            server->DeregisterHandler( &commonHandler );
            return EXIT_CODES::ERR_ARGS;
        }
    }

    server->SetReadyToReply( true );

    wxString listenPath = wxString::FromUTF8( server->SocketPath() );
    wxFprintf( stdout, "KiCad API server listening at %s\n", listenPath );

    auto oldSigInt = std::signal( SIGINT, apiServerSignalHandler );
#ifdef SIGTERM
    auto oldSigTerm = std::signal( SIGTERM, apiServerSignalHandler );
#endif

    wxTheApp->MainLoop();

    std::signal( SIGINT, oldSigInt );
#ifdef SIGTERM
    std::signal( SIGTERM, oldSigTerm );
#endif

    wxFprintf( stdout, "Shutting down\n" );

    commands::CloseAllDocuments shutdownRequest;
    shutdownRequest.set_force( true );
    closeAllDocuments( shutdownRequest );
    server->DeregisterHandler( &commonHandler );

    return EXIT_CODES::OK;
}
