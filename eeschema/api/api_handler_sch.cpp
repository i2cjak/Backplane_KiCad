/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <api/api_handler_sch.h>
#include <api/api_enums.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <api/cross_probe_client.h>
#include <api/sch_context.h>
#include <magic_enum.hpp>
#include <base_screen.h>
#include <jobs/job_export_sch_bom.h>
#include <jobs/job_export_sch_netlist.h>
#include <jobs/job_export_sch_plot.h>
#include <kiway.h>
#include <sch_field.h>
#include <sch_group.h>
#include <connection_graph.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_label.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <project.h>
#include <string_utils.h>
#include <tool/tool_manager.h>
#include <tools/sch_selection_tool.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>

using namespace kiapi::common::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;


namespace
{
using SELECTED_SCH_ITEM = std::pair<SCH_ITEM*, SCH_SHEET_PATH>;


void collectSelectedSchematicItems( const SCHEMATIC* aSchematic,
                                    std::vector<SELECTED_SCH_ITEM>& aItems )
{
    if( !aSchematic )
        return;

    for( const SCH_SHEET_PATH& path : aSchematic->Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->IsSelected() )
                aItems.emplace_back( item, path );

            item->RunOnChildren(
                    [&]( SCH_ITEM* child )
                    {
                        if( child->IsSelected() )
                            aItems.emplace_back( child, path );
                    },
                    RECURSE_MODE::RECURSE );
        }
    }
}


void clearSelectedSchematicItems( const SCHEMATIC* aSchematic )
{
    std::vector<SELECTED_SCH_ITEM> selected;
    collectSelectedSchematicItems( aSchematic, selected );

    for( const auto& [item, path] : selected )
        item->ClearSelected();
}


void packSchematicSelectionItem( google::protobuf::Any& aOutput, SCH_ITEM* aItem,
                                 const SCH_SHEET_PATH& aPath )
{
    if( aItem->Type() == SCH_SYMBOL_T )
    {
        kiapi::schematic::types::SchematicSymbolInstance symbol;

        if( PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( aItem ), aPath ) )
            aOutput.PackFrom( symbol );
    }
    else if( aItem->Type() == SCH_SHEET_T )
    {
        kiapi::schematic::types::SheetSymbol sheet;

        if( PackSheet( &sheet, static_cast<SCH_SHEET*>( aItem ), aPath ) )
            aOutput.PackFrom( sheet );
    }
    else
    {
        aItem->Serialize( aOutput );
    }
}


struct SCH_SYNC_TARGET
{
    SCH_SHEET_PATH path;
    SCH_ITEM* focus = nullptr;
    std::vector<SCH_ITEM*> items;
};


bool matchesFootprintSpec( const SCH_REFERENCE& aReference, const SelectionSpec& aSpec )
{
    return aSpec.spec_case() == SelectionSpec::kFootprint
           && aReference.GetRef() + aReference.GetRefNumber()
                      == wxString::FromUTF8( aSpec.footprint().reference() );
}


void appendMatchingSchematicItems( const SCH_SHEET_PATH& aPath,
                                   const SelectionSpec& aSpec,
                                   std::vector<SCH_ITEM*>& aItems )
{
    if( aSpec.spec_case() != SelectionSpec::kFootprint
        && aSpec.spec_case() != SelectionSpec::kPad )
        return;

    SCH_REFERENCE_LIST references;
    aPath.GetSymbols( references, SYMBOL_FILTER_NON_POWER, true );

    for( unsigned i = 0; i < references.GetCount(); ++i )
    {
        SCH_REFERENCE& reference = references[i];

        if( reference.IsSplitNeeded() )
            reference.Split();

        if( !matchesFootprintSpec( reference, aSpec )
            && !( aSpec.spec_case() == SelectionSpec::kPad
                  && reference.GetRef() + reference.GetRefNumber()
                             == wxString::FromUTF8( aSpec.pad().reference() ) ) )
        {
            continue;
        }

        SCH_SYMBOL* symbol = reference.GetSymbol();

        if( aSpec.spec_case() == SelectionSpec::kFootprint )
        {
            aItems.push_back( symbol );
            continue;
        }

        const wxString padNumber = wxString::FromUTF8( aSpec.pad().number() );

        for( SCH_PIN* pin : symbol->GetPins( &aPath ) )
        {
            for( const wxString& expanded : ExpandStackedPinNotation( pin->GetEffectivePadNumber() ) )
            {
                if( expanded == padNumber )
                {
                    aItems.push_back( pin );
                    break;
                }
            }
        }
    }
}


std::optional<SCH_SYNC_TARGET> resolveSyncSelection( const SCHEMATIC& aSchematic,
                                                     const SyncSelection& aRequest )
{
    const SCH_SHEET_LIST hierarchy = aSchematic.Hierarchy();
    std::vector<SCH_SHEET_PATH> paths;
    paths.reserve( hierarchy.size() );
    paths.push_back( aSchematic.CurrentSheet() );

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path != aSchematic.CurrentSheet() )
            paths.push_back( path );
    }

    for( const SCH_SHEET_PATH& path : paths )
    {
        SCH_SYNC_TARGET target;
        target.path = path;

        if( aRequest.has_focus_item() )
            appendMatchingSchematicItems( path, aRequest.focus_item(), target.items );

        for( const SelectionSpec& spec : aRequest.items() )
            appendMatchingSchematicItems( path, spec, target.items );

        if( !target.items.empty() )
        {
            if( aRequest.has_focus_item() )
            {
                std::vector<SCH_ITEM*> focusItems;
                appendMatchingSchematicItems( path, aRequest.focus_item(), focusItems );

                if( !focusItems.empty() )
                    target.focus = focusItems.front();
            }

            return target;
        }
    }

    return std::nullopt;
}
}


std::set<KICAD_T> API_HANDLER_SCH::s_allowedTypes = {
    SCH_MARKER_T,
    SCH_JUNCTION_T,
    SCH_NO_CONNECT_T,
    SCH_BUS_WIRE_ENTRY_T,
    SCH_BUS_BUS_ENTRY_T,
    SCH_LINE_T,
    SCH_SHAPE_T,
    SCH_RULE_AREA_T,
    SCH_BITMAP_T,
    SCH_TEXTBOX_T,
    SCH_TEXT_T,
    SCH_TABLE_T,
    SCH_LABEL_T,
    SCH_GLOBAL_LABEL_T,
    SCH_GROUP_T,
    SCH_HIER_LABEL_T,
    SCH_DIRECTIVE_LABEL_T,
    SCH_SYMBOL_T,
    SCH_SHEET_T,
};


HANDLER_RESULT<types::RunJobResponse> ExecuteSchematicJob( KIWAY* aKiway, JOB& aJob )
{
    types::RunJobResponse response;
    WX_STRING_REPORTER reporter;
    int exitCode = aKiway->ProcessJob( KIWAY::FACE_SCH, &aJob, &reporter );

    for( const JOB_OUTPUT& output : aJob.GetOutputs() )
        response.add_output_path( output.m_outputPath.ToUTF8() );

    if( exitCode == 0 )
    {
        response.set_status( types::JobStatus::JS_SUCCESS );
        return response;
    }

    response.set_status( types::JobStatus::JS_ERROR );
    response.set_message( fmt::format( "Schematic export job '{}' failed with exit code {}: {}",
                                       aJob.GetType(), exitCode,
                                       reporter.GetMessages().ToStdString() ) );
    return response;
}


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_SCH( CreateSchFrameContext( aFrame ), aFrame )
{
}


API_HANDLER_SCH::API_HANDLER_SCH( std::shared_ptr<SCH_CONTEXT> aContext,
                                  SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame ),
        m_frame( aFrame ),
        m_context( std::move( aContext ) )
{
    using namespace kiapi::schematic::jobs;
    using namespace kiapi::schematic::types;
    using namespace kiapi::schematic::commands;

    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );
    registerHandler<SaveDocument, google::protobuf::Empty>(
            &API_HANDLER_SCH::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, google::protobuf::Empty>(
            &API_HANDLER_SCH::handleSaveCopyOfDocument );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_SCH::handleGetItems );
    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_SCH::handleGetItemsById );
    registerHandler<GetSelection, SelectionResponse>( &API_HANDLER_SCH::handleGetSelection );
    registerHandler<ClearSelection, Empty>( &API_HANDLER_SCH::handleClearSelection );
    registerHandler<AddToSelection, SelectionResponse>( &API_HANDLER_SCH::handleAddToSelection );
    registerHandler<RemoveFromSelection, SelectionResponse>( &API_HANDLER_SCH::handleRemoveFromSelection );
    registerHandler<CrossProbeAnnounce, CrossProbeAnnounceResponse>(
            &API_HANDLER_SCH::handleCrossProbeAnnounce );
    registerHandler<SyncSelection, SyncSelectionResponse>( &API_HANDLER_SCH::handleSyncSelection );
    registerHandler<HighlightNets, HighlightNetsResponse>( &API_HANDLER_SCH::handleHighlightNets );
    registerHandler<FocusOnItem, FocusOnItemResponse>( &API_HANDLER_SCH::handleFocusOnItem );

    registerHandler<RunSchematicJobExportSvg, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportSvg );
    registerHandler<RunSchematicJobExportDxf, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportDxf );
    registerHandler<RunSchematicJobExportPdf, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportPdf );
    registerHandler<RunSchematicJobExportPs, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportPs );
    registerHandler<RunSchematicJobExportNetlist, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportNetlist );
    registerHandler<RunSchematicJobExportBOM, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportBOM );
    registerHandler<GetSchematicHierarchy, SchematicHierarchyResponse>( &API_HANDLER_SCH::handleGetSchematicHierarchy );
    registerHandler<GetPageSettings, types::PageSettings>( &API_HANDLER_SCH::handleGetPageSettings );
    registerHandler<SetPageSettings, types::PageSettings>( &API_HANDLER_SCH::handleSetPageSettings );
    registerHandler<GetSchematicNetlist, SchematicNetlistResponse>( &API_HANDLER_SCH::handleGetSchematicNetlist );
    registerHandler<GetVariants, VariantsResponse>( &API_HANDLER_SCH::handleGetVariants );
    registerHandler<AddVariant, Empty>( &API_HANDLER_SCH::handleAddVariant );
    registerHandler<DeleteVariant, Empty>( &API_HANDLER_SCH::handleDeleteVariant );
    registerHandler<RenameVariant, Empty>( &API_HANDLER_SCH::handleRenameVariant );
    registerHandler<CopyVariant, Empty>( &API_HANDLER_SCH::handleCopyVariant );
    registerHandler<SetVariantDescription, Empty>( &API_HANDLER_SCH::handleSetVariantDescription );
    registerHandler<SetCurrentVariant, Empty>( &API_HANDLER_SCH::handleSetCurrentVariant );
    registerHandler<GetCurrentVariant, CurrentVariantResponse>( &API_HANDLER_SCH::handleGetCurrentVariant );

    // KiCad 10.0.6 published these two commands in the `types` package.  The
    // command package was corrected upstream, but Any::UnpackTo also checks
    // the type URL, so merely registering the corrected handler leaves old
    // clients with an "unhandled" request.  Decode the old wire-compatible
    // payload into the corrected request and return the old response URL so
    // generated 10.0.6 clients can still unpack it.
    // Register the two compatibility entries explicitly so each can decode
    // its concrete request and invoke the shared handler.
    m_handlers.emplace( "kiapi.schematic.types.GetSchematicHierarchy",
            [this]( ApiRequest& aRequest ) -> API_RESULT
            {
                GetSchematicHierarchy request;
                ApiResponse envelope;

                if( !request.ParseFromString( aRequest.message().value() ) )
                {
                    envelope.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
                    envelope.mutable_status()->set_error_message( "could not unpack legacy schematic hierarchy command" );
                    return envelope;
                }

                HANDLER_CONTEXT<GetSchematicHierarchy> context;
                context.ClientName = aRequest.header().client_name();
                context.Request = std::move( request );
                HANDLER_RESULT<SchematicHierarchyResponse> response = handleGetSchematicHierarchy( context );

                if( !response.has_value() )
                    return tl::unexpected( response.error() );

                envelope.mutable_status()->set_status( ApiStatusCode::AS_OK );
                envelope.mutable_message()->PackFrom( *response );
                envelope.mutable_message()->set_type_url(
                        "type.googleapis.com/kiapi.schematic.types.SchematicHierarchyResponse" );
                return envelope;
            } );

    m_handlers.emplace( "kiapi.schematic.types.GetSchematicNetlist",
            [this]( ApiRequest& aRequest ) -> API_RESULT
            {
                GetSchematicNetlist request;
                ApiResponse envelope;

                if( !request.ParseFromString( aRequest.message().value() ) )
                {
                    envelope.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
                    envelope.mutable_status()->set_error_message( "could not unpack legacy schematic netlist command" );
                    return envelope;
                }

                HANDLER_CONTEXT<GetSchematicNetlist> context;
                context.ClientName = aRequest.header().client_name();
                context.Request = std::move( request );
                HANDLER_RESULT<SchematicNetlistResponse> response = handleGetSchematicNetlist( context );

                if( !response.has_value() )
                    return tl::unexpected( response.error() );

                envelope.mutable_status()->set_status( ApiStatusCode::AS_OK );
                envelope.mutable_message()->PackFrom( *response );
                envelope.mutable_message()->set_type_url(
                        "type.googleapis.com/kiapi.schematic.types.SchematicNetlistResponse" );
                return envelope;
            } );
}


std::unique_ptr<COMMIT> API_HANDLER_SCH::createCommit()
{
    if( m_frame )
        return std::make_unique<SCH_COMMIT>( m_frame );

    return std::make_unique<SCH_COMMIT>( m_context->GetToolManager() );
}


SCHEMATIC* API_HANDLER_SCH::schematic() const
{
    wxCHECK( m_context, nullptr );
    return m_context->GetSchematic();
}


std::optional<SCH_ITEM*> API_HANDLER_SCH::getItemById( const KIID& aId, SCH_SHEET_PATH* aPathOut ) const
{
    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    SCH_ITEM* item = schematic()->ResolveItem( aId, aPathOut, true );

    if( !item )
        return std::nullopt;

    return item;
}


tl::expected<bool, ApiResponseStatus>
API_HANDLER_SCH::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
        return false;

    const PROJECT& prj = m_context->Prj();

    if( aDocument.project().name().compare( prj.GetProjectName().ToUTF8() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open",
                                          aDocument.project().name() ) );
        return tl::unexpected( e );
    }

    if( aDocument.project().path().compare( prj.GetProjectDirectory().ToUTF8() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open at path {}",
                                          aDocument.project().name(),
                                          aDocument.project().path() ) );
        return tl::unexpected( e );
    }

    if( aDocument.has_sheet_path() )
    {
        KIID_PATH path = UnpackSheetPath( aDocument.sheet_path() );

        if( !schematic()->Hierarchy().HasPath( path ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "the requested sheet path {} is not valid for this schematic",
                                              path.AsString().ToStdString() ) );
            return tl::unexpected( e );
        }
    }

    return true;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_SCH::handleSaveDocument( const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !context()->SaveSchematic() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save schematic" );
        return tl::unexpected( e );
    }

    return google::protobuf::Empty();
}


HANDLER_RESULT<google::protobuf::Empty>
API_HANDLER_SCH::handleSaveCopyOfDocument( const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName schematicPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !schematicPath.IsOk() || !schematicPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message(
                fmt::format( "save path '{}' could not be opened", schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( schematicPath.FileExists() && ( !schematicPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( schematicPath.GetExt() != FILEEXT::KiCadSchematicFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_sch extension",
                                          schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    if( !context()->SaveSchematicCopy( schematicPath.GetFullPath(), includeProject ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save schematic copy" );
        return tl::unexpected( e );
    }

    return google::protobuf::Empty();
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SCH::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;

        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( m_context->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_SCHEMATIC );

    // OpenDocument identifies the schematic file as a whole.  Do not add the
    // currently selected sheet here: the CLI OpenDocument response has no
    // sheet selector, and adding a transient current-sheet path makes the two
    // otherwise identical document specifiers compare unequal.

    PackProject( *doc.mutable_project(), m_context->Prj() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


void API_HANDLER_SCH::filterValidSchTypes( std::set<KICAD_T>& aTypeList )
{
    std::erase_if( aTypeList,
                   []( KICAD_T aType )
                   {
                       return !s_allowedTypes.contains( aType );
                   } );
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SCH::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<std::optional<KIID>> valid = validateItemHeaderDocument( aCtx.Request.header() );
        !valid.has_value() )
    {
        return tl::unexpected( valid.error() );
    }

    std::set<KICAD_T> typesRequested, typesInserted;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
        typesRequested.insert( type );

    filterValidSchTypes( typesRequested );

    if( typesRequested.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Schematic object" );
        return tl::unexpected( e );
    }

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    std::optional<SCH_SHEET_PATH> pathFilter;

    if( aCtx.Request.header().document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aCtx.Request.header().document().sheet_path() );
        pathFilter = hierarchy.GetSheetPathByKIIDPath( kp );
    }

    std::map<KICAD_T, std::vector<std::pair<EDA_ITEM*, SCH_SHEET_PATH>>> itemMap;

    auto processScreen =
        [&]( const SCH_SHEET_PATH& aPath )
        {
            const SCH_SCREEN* aScreen = aPath.LastScreen();

            for( SCH_ITEM* aItem : aScreen->Items() )
            {
                itemMap[ aItem->Type() ].emplace_back( aItem, aPath );

                aItem->RunOnChildren(
                        [&]( SCH_ITEM* aChild )
                        {
                            itemMap[ aChild->Type() ].emplace_back( aChild, aPath );
                        },
                        RECURSE_MODE::NO_RECURSE );
            }
        };

    if( pathFilter )
    {
        processScreen( *pathFilter );
    }
    else
    {
        for( const SCH_SHEET_PATH& path : hierarchy )
            processScreen( path );
    }

    GetItemsResponse response;
    google::protobuf::Any any;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
    {
        if( !s_allowedTypes.contains( type ) )
            continue;

        if( typesInserted.contains( type ) )
            continue;

        for( const auto& [item, itemPath] : itemMap[type] )
        {
            if( item->Type() == SCH_SYMBOL_T )
            {
                kiapi::schematic::types::SchematicSymbolInstance symbol;

                if( !PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( item ), itemPath ) )
                    continue;

                any.PackFrom( symbol );
            }
            else if( item->Type() == SCH_SHEET_T )
            {
                kiapi::schematic::types::SheetSymbol sheet;

                if( !PackSheet( &sheet, static_cast<SCH_SHEET*>( item ), itemPath ) )
                    continue;

                any.PackFrom( sheet );
            }
            else
            {
                item->Serialize( any );
            }

            response.mutable_items()->Add( std::move( any ) );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SCH::handleGetItemsById( const HANDLER_CONTEXT<GetItemsById>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    std::optional<SCH_SHEET_PATH> pathFilter;

    if( aCtx.Request.header().document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aCtx.Request.header().document().sheet_path() );
        pathFilter = hierarchy.GetSheetPathByKIIDPath( kp );
    }

    GetItemsResponse response;
    SCH_ITEM* item = nullptr;
    google::protobuf::Any any;

    for( const types::KIID& idProto : aCtx.Request.items() )
    {
        KIID id( idProto.value() );

        SCH_SHEET_PATH itemPath;

        if( pathFilter )
        {
            item = pathFilter->ResolveItem( id );
            itemPath = *pathFilter;
        }
        else
        {
            item = hierarchy.ResolveItem( id, &itemPath, true );
        }

        if( !item || !s_allowedTypes.contains( item->Type() ) )
            continue;

        if( item->Type() == SCH_SYMBOL_T )
        {
            kiapi::schematic::types::SchematicSymbolInstance symbol;

            if( !PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( item ), itemPath ) )
                continue;

            any.PackFrom( symbol );
        }
        else if( item->Type() == SCH_SHEET_T )
        {
            kiapi::schematic::types::SheetSymbol sheet;

            if( !PackSheet( &sheet, static_cast<SCH_SHEET*>( item ), itemPath ) )
                continue;

            any.PackFrom( sheet );
        }
        else
        {
            item->Serialize( any );
        }

        response.mutable_items()->Add( std::move( any ) );
    }

    if( response.items().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid" );
        return tl::unexpected( e );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_SCH::handleGetSelection(
        const HANDLER_CONTEXT<GetSelection>& aCtx )
{
    if( HANDLER_RESULT<std::optional<KIID>> valid = validateItemHeaderDocument( aCtx.Request.header() );
        !valid.has_value() )
    {
        return tl::unexpected( valid.error() );
    }

    std::set<KICAD_T> filter;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
    {
        if( s_allowedTypes.contains( type ) )
            filter.insert( type );
    }

    std::vector<SELECTED_SCH_ITEM> selected;

    if( m_frame )
    {
        if( SCH_SELECTION_TOOL* selectionTool = toolManager()->GetTool<SCH_SELECTION_TOOL>() )
        {
            SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();

            for( EDA_ITEM* selectedItem : selectionTool->GetSelection() )
            {
                SCH_ITEM* item = dynamic_cast<SCH_ITEM*>( selectedItem );

                if( !item )
                    continue;

                SCH_SHEET_PATH path;

                if( SCH_ITEM* resolved = hierarchy.ResolveItem( item->m_Uuid, &path, true ) )
                    selected.emplace_back( resolved, path );
            }
        }
    }
    else
    {
        collectSelectedSchematicItems( schematic(), selected );
    }

    SelectionResponse response;

    for( const auto& [item, path] : selected )
    {
        if( filter.empty() || filter.contains( item->Type() ) )
        {
            google::protobuf::Any packed;
            packSchematicSelectionItem( packed, item, path );
            response.mutable_items()->Add( std::move( packed ) );
        }
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleClearSelection(
        const HANDLER_CONTEXT<ClearSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( m_frame )
    {
        if( SCH_SELECTION_TOOL* selectionTool = toolManager()->GetTool<SCH_SELECTION_TOOL>() )
            selectionTool->ClearSelection();

        m_frame->Refresh();
    }
    else
    {
        clearSelectedSchematicItems( schematic() );
    }

    return Empty();
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_SCH::handleAddToSelection(
        const HANDLER_CONTEXT<AddToSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    EDA_ITEMS toAdd;
    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();

    for( const types::KIID& id : aCtx.Request.items() )
    {
        SCH_SHEET_PATH path;

        if( SCH_ITEM* item = hierarchy.ResolveItem( KIID( id.value() ), &path, true ) )
            toAdd.push_back( item );
    }

    if( m_frame )
    {
        if( SCH_SELECTION_TOOL* selectionTool = toolManager()->GetTool<SCH_SELECTION_TOOL>() )
            selectionTool->AddItemsToSel( &toAdd );

        m_frame->Refresh();
    }
    else
    {
        for( EDA_ITEM* item : toAdd )
            item->SetSelected();
    }

    HANDLER_CONTEXT<GetSelection> getContext;
    getContext.ClientName = aCtx.ClientName;
    getContext.Request.mutable_header()->CopyFrom( aCtx.Request.header() );
    return handleGetSelection( getContext );
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_SCH::handleRemoveFromSelection(
        const HANDLER_CONTEXT<RemoveFromSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    EDA_ITEMS toRemove;
    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();

    for( const types::KIID& id : aCtx.Request.items() )
    {
        SCH_SHEET_PATH path;

        if( SCH_ITEM* item = hierarchy.ResolveItem( KIID( id.value() ), &path, true ) )
            toRemove.push_back( item );
    }

    if( m_frame )
    {
        if( SCH_SELECTION_TOOL* selectionTool = toolManager()->GetTool<SCH_SELECTION_TOOL>() )
            selectionTool->RemoveItemsFromSel( &toRemove );

        m_frame->Refresh();
    }
    else
    {
        for( EDA_ITEM* item : toRemove )
            item->ClearSelected();
    }

    HANDLER_CONTEXT<GetSelection> getContext;
    getContext.ClientName = aCtx.ClientName;
    getContext.Request.mutable_header()->CopyFrom( aCtx.Request.header() );
    return handleGetSelection( getContext );
}


HANDLER_RESULT<CrossProbeAnnounceResponse> API_HANDLER_SCH::handleCrossProbeAnnounce(
        const HANDLER_CONTEXT<CrossProbeAnnounce>& aCtx )
{
    CROSS_PROBE_CLIENT::RegisterPeer( static_cast<FRAME_T>( aCtx.Request.frame_type() ),
                                      aCtx.Request.socket_path() );

    CrossProbeAnnounceResponse response;
    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<SyncSelectionResponse> API_HANDLER_SCH::handleSyncSelection(
        const HANDLER_CONTEXT<SyncSelection>& aCtx )
{
    std::optional<SCH_SYNC_TARGET> target = resolveSyncSelection( *schematic(), aCtx.Request );

    if( m_frame )
    {
        if( SCH_SELECTION_TOOL* selectionTool = toolManager()->GetTool<SCH_SELECTION_TOOL>() )
        {
            selectionTool->ClearSelection( true );
            EDA_ITEMS selected;

            if( target )
            {
                for( SCH_ITEM* item : target->items )
                    selected.push_back( item );
            }

            selectionTool->AddItemsToSel( &selected );
        }

        m_frame->Refresh();
    }
    else
    {
        clearSelectedSchematicItems( schematic() );

        if( target )
        {
            for( SCH_ITEM* item : target->items )
                item->SetSelected();
        }
    }

    SyncSelectionResponse response;
    response.set_status( target || aCtx.Request.items_size() == 0 ? CPS_OK : CPS_NOT_FOUND );
    return response;
}


HANDLER_RESULT<HighlightNetsResponse> API_HANDLER_SCH::handleHighlightNets(
        const HANDLER_CONTEXT<HighlightNets>& aCtx )
{
    HighlightNetsResponse response;

    if( !m_frame )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "schematic net highlighting requires a GUI document" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.net_name().empty() )
    {
        response.set_status( CPS_INVALID );
        response.set_message( "HighlightNets requires at least one net name" );
        return response;
    }

    // This is a receiver command.  Apply it to this schematic frame instead
    // of forwarding it back to the PCB editor, which would reverse the
    // direction of a standalone cross-probe request.
    for( const std::string& name : aCtx.Request.net_name() )
    {
        std::string payload = "$NET: \"" + name + "\"";
        m_frame->ExecuteRemoteCommand( payload.c_str() );
    }

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<FocusOnItemResponse> API_HANDLER_SCH::handleFocusOnItem(
        const HANDLER_CONTEXT<FocusOnItem>& aCtx )
{
    SyncSelection request;
    request.set_mode( SSM_ITEMS_AND_NETS );
    request.mutable_focus_item()->CopyFrom( aCtx.Request.focus_item() );
    request.mutable_items()->Add()->CopyFrom( aCtx.Request.focus_item() );

    std::optional<SCH_SYNC_TARGET> target = resolveSyncSelection( *schematic(), request );
    FocusOnItemResponse response;

    if( !target )
    {
        response.set_status( CPS_NOT_FOUND );
        return response;
    }

    if( m_frame )
    {
        if( target->focus )
            m_frame->FocusOnItem( target->focus );

        m_frame->Refresh();
    }

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> API_HANDLER_SCH::createItemForType( KICAD_T aType, EDA_ITEM* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( !s_allowedTypes.contains( aType ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "type {} is not supported by the schematic API handler",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    if( aType == SCH_PIN_T && !dynamic_cast<SCH_SYMBOL*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pin in {}, which is not a symbol",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SHEET_T && !dynamic_cast<SCH_SCREEN*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a sheet symbol in {}, which is not a "
                                          "schematic sheet",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SYMBOL_T && !dynamic_cast<SCH_SCREEN*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a symbol in {}, which is not a "
                                          "schematic sheet",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<EDA_ITEM> created = CreateItemForType( aType, aContainer );

    if( created && !created->GetParent() )
        created->SetParent( aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SCH::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    SCH_SCREEN* targetScreen = schematic()->GetCurrentScreen();
    SCH_SHEET_PATH targetPath = m_context->GetCurrentSheet().value_or( *hierarchy.begin() );

    if( aHeader.document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aHeader.document().sheet_path() );
        if( std::optional<SCH_SHEET_PATH> path = hierarchy.GetSheetPathByKIIDPath( kp ) )
        {
            targetPath = *path;
            targetScreen = targetPath.LastScreen();
        }
    }

    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aClientName ) );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        // ERC markers are transient read-only diagnostics.  They have no
        // stable KIID and constructing one through the normal item factory
        // lacks the live ERC context required by SCH_MARKER::Deserialize.
        if( *type == SCH_MARKER_T )
        {
            status.set_code( ItemStatusCode::ISC_IMMUTABLE );
            status.set_error_message( "schematic ERC markers are read-only API results" );
            aItemHandler( status, anyItem );
            continue;
        }

        EDA_ITEM* container = targetScreen;

        HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> creationResult = createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item( std::move( *creationResult ) );

        bool unpacked = false;

        if( *type == SCH_SYMBOL_T )
        {
            kiapi::schematic::types::SchematicSymbolInstance symbol;
            unpacked = anyItem.UnpackTo( &symbol )
                       && UnpackSymbol( static_cast<SCH_SYMBOL*>( item.get() ), symbol );
        }
        else if( *type == SCH_SHEET_T )
        {
            kiapi::schematic::types::SheetSymbol sheetProto;
            unpacked = anyItem.UnpackTo( &sheetProto );

            if( unpacked )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item.get() );

                if( tl::expected<bool, ApiResponseStatus> result = UnpackSheet( sheet, sheetProto );
                    result.has_value() )
                {
                    unpacked = *result;
                    SCH_SHEET_INSTANCE instance;

                    if( !sheet->GetInstances().empty() )
                        instance = *sheet->GetInstances().begin();

                    if( instance.m_PageNumber.IsEmpty() )
                        instance.m_PageNumber = hierarchy.GetNextPageNumber();

                    if( instance.m_Path.empty() )
                    {
                        SCH_SHEET_PATH newPath( targetPath );
                        newPath.push_back( sheet );
                        instance.m_Path = newPath.Path();
                    }

                    sheet->AddInstance( instance );
                }
                else
                {
                    return tl::unexpected( result.error() );
                }
            }
        }
        else
        {
            unpacked = item->Deserialize( anyItem );
        }

        if( !unpacked )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        SCH_ITEM* existingItem = nullptr;
        SCH_SHEET_PATH existingPath;

        existingItem = targetPath.ResolveItem( item->m_Uuid );

        if( existingItem )
            existingPath = targetPath;

        if( aCreate && existingItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !existingItem )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( !aCreate )
        {
            SCH_SCREEN* itemScreen = existingPath.LastScreen();

            if( itemScreen != targetScreen )
            {
                status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                status.set_error_message( fmt::format( "item {} exists on a different sheet than targeted",
                                                       item->m_Uuid.AsStdString() ) );
                aItemHandler( status, anyItem );
                continue;
            }
        }

        if( *type == SCH_SHEET_T )
        {
            SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item.get() );

            if( aCreate && !sheet->GetScreen() )
                sheet->SetScreen( new SCH_SCREEN( schematic() ) );

            SCH_SHEET_PATH parentPath;

            if( aCreate )
                parentPath = targetPath;
            else
                parentPath = existingPath;

            wxString destFilePath = parentPath.LastScreen()->GetFileName();

            if( !destFilePath.IsEmpty() )
            {
                SCH_SHEET_LIST schematicSheets = schematic()->Hierarchy();
                SCH_SHEET_LIST loadedSheets( sheet );

                if( schematicSheets.TestForRecursion( loadedSheets, destFilePath ) )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( "sheet update would create recursive hierarchy" );
                    aItemHandler( status, anyItem );
                    continue;
                }
            }
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            SCH_ITEM* createdItem = static_cast<SCH_ITEM*>( item.release() );
            commit->Add( createdItem, targetScreen );

            if( !createdItem )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "could not add the requested item to its parent container" );
                return tl::unexpected( e );
            }

            if( createdItem->Type() == SCH_SYMBOL_T )
            {
                kiapi::schematic::types::SchematicSymbolInstance symbol;

                if( PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( createdItem ), targetPath ) )
                    newItem.PackFrom( symbol );
            }
            else if( createdItem->Type() == SCH_SHEET_T )
            {
                kiapi::schematic::types::SheetSymbol sheet;

                if( PackSheet( &sheet, static_cast<SCH_SHEET*>( createdItem ), targetPath ) )
                    newItem.PackFrom( sheet );
            }
            else
            {
                createdItem->Serialize( newItem );
            }
        }
        else
        {
            commit->Modify( existingItem, targetScreen );
            existingItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );

            if( existingItem->Type() == SCH_SYMBOL_T )
            {
                SCH_SHEET_PATH path = existingPath;
                kiapi::schematic::types::SchematicSymbolInstance symbol;

                if( PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( existingItem ), path ) )
                    newItem.PackFrom( symbol );
            }
            else if( existingItem->Type() == SCH_SHEET_T )
            {
                SCH_SHEET_PATH path = existingPath;
                kiapi::schematic::types::SheetSymbol sheet;

                if( PackSheet( &sheet, static_cast<SCH_SHEET*>( existingItem ), path ) )
                    newItem.PackFrom( sheet );
            }
            else
            {
                existingItem->Serialize( newItem );
            }
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.contains( aClientName ) )
    {
        pushCurrentCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }


    return ItemRequestStatus::IRS_OK;
}


void API_HANDLER_SCH::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    COMMIT* commit = getCurrentCommit( aClientName );

    for( auto& [id, status] : aItemsToDelete )
    {
        SCH_SHEET_PATH path;
        SCH_ITEM* item = hierarchy.ResolveItem( id, &path, true );

        if( !item )
            continue;

        if( !s_allowedTypes.contains( item->Type() ) )
        {
            status = ItemDeletionStatus::IDS_IMMUTABLE;
            continue;
        }

        commit->Remove( item, path.LastScreen() );
        status = ItemDeletionStatus::IDS_OK;
    }

    if( !m_activeClients.contains( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument, const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    SCH_ITEM* item = schematic()->Hierarchy().ResolveItem( aId, nullptr, true );

    if( !item)
        return std::nullopt;

    return item;
}


std::optional<TITLE_BLOCK*> API_HANDLER_SCH::getTitleBlock()
{
    wxCHECK( m_context->GetCurrentSheet(), std::nullopt );
    return &m_context->GetCurrentSheet()->LastScreen()->GetTitleBlock();
}


std::optional<PAGE_INFO> API_HANDLER_SCH::getPageSettings()
{
    wxCHECK( m_context->GetCurrentSheet(), std::nullopt );
    return m_context->GetCurrentSheet()->LastScreen()->GetPageSettings();
}


bool API_HANDLER_SCH::setPageSettings( const PAGE_INFO& aPageInfo )
{
    wxCHECK( m_context->GetCurrentSheet(), false );
    m_context->GetCurrentSheet()->LastScreen()->SetPageSettings( aPageInfo );
    return true;
}


wxString API_HANDLER_SCH::getDrawingSheetFileName()
{
    return BASE_SCREEN::m_DrawingSheetFileName;
}


void API_HANDLER_SCH::setDrawingSheetFileName( const wxString& aFileName )
{
    BASE_SCREEN::m_DrawingSheetFileName = aFileName;
    schematic()->Settings().m_SchDrawingSheetFileName = aFileName;

    if( m_frame )
        m_frame->LoadDrawingSheet();
}


void API_HANDLER_SCH::onModified()
{
    // A headless context has no frame to route OnModify() through.  Mark every
    // screen directly so edits to a subsheet (and project-level variant
    // changes) are included by the subsequent save.
    if( SCHEMATIC* sch = schematic() )
    {
        SCH_SCREENS screens( sch->Root() );

        for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
            screen->SetContentModified();
    }

    if( m_frame )
    {
        m_frame->Refresh();
        m_frame->OnModify();
    }
}


std::optional<bool> API_HANDLER_SCH::documentIsModified() const
{
    if( !schematic() )
        return std::nullopt;

    SCH_SCREENS screens( schematic()->Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        if( screen->IsContentModified() )
            return true;
    }

    return hasPendingChanges();
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportSvg(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportSvg>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_SVG>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return ExecuteSchematicJob( m_context->GetKiway(), *plotJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportDxf(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportDxf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_DXF>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return ExecuteSchematicJob( m_context->GetKiway(), *plotJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportPdf(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportPdf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_PDF>( false );
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    plotJob->m_PDFPropertyPopups = aCtx.Request.property_popups();
    plotJob->m_PDFHierarchicalLinks = aCtx.Request.hierarchical_links();
    plotJob->m_PDFMetadata = aCtx.Request.include_metadata();

    return ExecuteSchematicJob( m_context->GetKiway(), *plotJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportPs(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportPs>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_PS>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return ExecuteSchematicJob( m_context->GetKiway(), *plotJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportNetlist(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.format() == kiapi::schematic::jobs::SchematicNetlistFormat::SNF_UNKNOWN )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "RunSchematicJobExportNetlist requires a valid format" );
        return tl::unexpected( e );
    }

    JOB_EXPORT_SCH_NETLIST netlistJob;
    netlistJob.m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        netlistJob.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    netlistJob.format = FromProtoEnum<JOB_EXPORT_SCH_NETLIST::FORMAT>( aCtx.Request.format() );

    if( !aCtx.Request.variant_name().empty() )
        netlistJob.m_variantNames.emplace_back( wxString::FromUTF8( aCtx.Request.variant_name() ) );

    return ExecuteSchematicJob( m_context->GetKiway(), netlistJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportBOM(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportBOM>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_SCH_BOM bomJob;
    bomJob.m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        bomJob.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    bomJob.m_bomFmtPresetName = wxString::FromUTF8( aCtx.Request.format().preset_name() );
    bomJob.m_fieldDelimiter = wxString::FromUTF8( aCtx.Request.format().field_delimiter() );
    bomJob.m_stringDelimiter = wxString::FromUTF8( aCtx.Request.format().string_delimiter() );
    bomJob.m_refDelimiter = wxString::FromUTF8( aCtx.Request.format().ref_delimiter() );
    bomJob.m_refRangeDelimiter = wxString::FromUTF8( aCtx.Request.format().ref_range_delimiter() );
    bomJob.m_keepTabs = aCtx.Request.format().keep_tabs();
    bomJob.m_keepLineBreaks = aCtx.Request.format().keep_line_breaks();

    bomJob.m_bomPresetName = wxString::FromUTF8( aCtx.Request.fields().preset_name() );
    bomJob.m_sortField = wxString::FromUTF8( aCtx.Request.fields().sort_field() );
    bomJob.m_filterString = wxString::FromUTF8( aCtx.Request.fields().filter() );

    if( aCtx.Request.fields().sort_direction() == kiapi::schematic::jobs::BOMSortDirection::BSD_ASCENDING )
    {
        bomJob.m_sortAsc = true;
    }
    else if( aCtx.Request.fields().sort_direction() == kiapi::schematic::jobs::BOMSortDirection::BSD_DESCENDING )
    {
        bomJob.m_sortAsc = false;
    }

    for( const kiapi::schematic::jobs::BOMField& field : aCtx.Request.fields().fields() )
    {
        bomJob.m_fieldsOrdered.emplace_back( wxString::FromUTF8( field.name() ) );
        bomJob.m_fieldsLabels.emplace_back( wxString::FromUTF8( field.label() ) );

        if( field.group_by() )
            bomJob.m_fieldsGroupBy.emplace_back( wxString::FromUTF8( field.name() ) );
    }

    bomJob.m_excludeDNP = aCtx.Request.exclude_dnp();
    bomJob.m_groupSymbols = aCtx.Request.group_symbols();

    if( !aCtx.Request.variant_name().empty() )
        bomJob.m_variantNames.emplace_back( wxString::FromUTF8( aCtx.Request.variant_name() ) );

    return ExecuteSchematicJob( m_context->GetKiway(), bomJob );
}


void API_HANDLER_SCH::packSheetInstance( kiapi::schematic::types::SheetInstance* aInstance, SCH_SHEET_PATH& aPath,
                                          SCH_SHEET* aSheet )
{
    aPath.push_back( aSheet );

    PackSheetPath( *aInstance->mutable_path(), aPath.Path() );

    wxString sheetName = aSheet->GetShownName( false );

    if( sheetName.IsEmpty() && aSheet->GetScreen() )
    {
        wxFileName fn( aSheet->GetScreen()->GetFileName() );
        sheetName = fn.GetName();
    }

    aInstance->set_name( sheetName.ToUTF8() );
    aInstance->set_filename( aSheet->GetFileName().ToUTF8() );
    aInstance->set_page_number( aPath.GetPageNumber().ToUTF8() );

    if( aSheet->GetScreen() )
    {
        std::vector<SCH_ITEM*> childSheets;
        aSheet->GetScreen()->GetSheets( &childSheets );

        std::ranges::sort( childSheets,
                           [&]( SCH_ITEM* a, SCH_ITEM* b )
                           {
                               SCH_SHEET_PATH pathA = aPath;
                               pathA.push_back( static_cast<SCH_SHEET*>( a ) );

                               SCH_SHEET_PATH pathB = aPath;
                               pathB.push_back( static_cast<SCH_SHEET*>( b ) );

                               return pathA.ComparePageNum( pathB ) < 0;
                           } );

        for( SCH_ITEM* childItem : childSheets )
        {
            SCH_SHEET* childSheet = static_cast<SCH_SHEET*>( childItem );
            kiapi::schematic::types::SheetInstance* childInstance = aInstance->add_children();
            packSheetInstance( childInstance, aPath, childSheet );
        }
    }

    aPath.pop_back();
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicHierarchyResponse> API_HANDLER_SCH::handleGetSchematicHierarchy(
        const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicHierarchy>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    kiapi::schematic::commands::SchematicHierarchyResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    SCH_SHEET_PATH path;
    std::vector<SCH_SHEET*> topLevelSheets = schematic()->GetTopLevelSheets();

    std::ranges::sort( topLevelSheets,
               [&]( SCH_SHEET* a, SCH_SHEET* b )
               {
                   SCH_SHEET_PATH pathA;
                   pathA.push_back( a );

                   SCH_SHEET_PATH pathB;
                   pathB.push_back( b );

                   return pathA.ComparePageNum( pathB ) < 0;
               } );

    for( SCH_SHEET* topSheet : topLevelSheets )
    {
        kiapi::schematic::types::SheetInstance* instance = response.add_top_level_sheets();
        packSheetInstance( instance, path, topSheet );
    }

    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicNetlistResponse>
API_HANDLER_SCH::handleGetSchematicNetlist( const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Schematic object" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> typeFilter( types.begin(), types.end() );

    CONNECTION_GRAPH* connectionGraph = schematic()->ConnectionGraph();

    if( !connectionGraph )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "schematic has no connection graph" );
        return tl::unexpected( e );
    }

    kiapi::schematic::commands::SchematicNetlistResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const auto& [key, subgraphList] : connectionGraph->GetNetMap() )
    {
        if( subgraphList.empty() )
            continue;

        CONNECTION_SUBGRAPH* firstSubgraph = subgraphList[0];

        if( firstSubgraph->GetDriverConnection() && firstSubgraph->GetDriverConnection()->IsBus() )
            continue;

        if( firstSubgraph->GetDriverPriority() < CONNECTION_SUBGRAPH::PRIORITY::PIN )
            continue;

        kiapi::schematic::types::SchematicNet* net = response.add_nets();
        net->set_name( key.Name.ToUTF8() );

        for( CONNECTION_SUBGRAPH* subGraph : subgraphList )
        {
            kiapi::schematic::types::SchematicNetSheetContents* sheetContents = net->add_sheets();
            PackSheetPath( *sheetContents->mutable_path(), subGraph->GetSheet().Path() );

            for( SCH_ITEM* item : subGraph->GetItems() )
            {
                if( filterByType && !typeFilter.contains( item->Type() ) )
                    continue;

                sheetContents->add_items()->set_value( item->m_Uuid.AsStdString() );
            }
        }
    }

    return response;
}
