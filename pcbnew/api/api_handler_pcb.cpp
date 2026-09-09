/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <magic_enum.hpp>
#include <properties/property.h>

#include <common.h>
#include <api/api_handler_pcb.h>
#include <api/api_pcb_utils.h>
#include <api/api_enums.h>
#include <api/cross_probe_client.h>
#include <api/board_context.h>
#include <api/api_utils.h>
#include <api/common/commands/variant_commands.pb.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <kicad_clipboard.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_group.h>
#include <pcb_reference_image.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_table.h>
#include <pcb_track.h>
#include <pcbnew_id.h>
#include <pcb_marker.h>
#include <pcb_plot_params.h>
#include <kiway.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <drc/drc_rule_condition.h>
#include <drc/drc_rule_parser.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <jobs/job_export_pcb_3d.h>
#include <jobs/job_export_pcb_dxf.h>
#include <jobs/job_export_pcb_drill.h>
#include <jobs/job_export_pcb_gencad.h>
#include <jobs/job_export_pcb_gerber.h>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_ipc2581.h>
#include <jobs/job_export_pcb_ipcd356.h>
#include <jobs/job_export_pcb_odb.h>
#include <jobs/job_export_pcb_pdf.h>
#include <jobs/job_export_pcb_pos.h>
#include <jobs/job_export_pcb_ps.h>
#include <jobs/job_export_pcb_stats.h>
#include <jobs/job_export_pcb_svg.h>
#include <view/view.h>
#include <jobs/job_pcb_render.h>
#include <layer_ids.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <project.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <tools/zone_filler_tool.h>
#include <zone.h>
#include <zone_filler.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>
#include <widgets/appearance_controls.h>
#include <widgets/report_severity.h>

using namespace kiapi::common::commands;
using kiapi::board::BoardDesignRules;
using kiapi::board::CustomRule;
using kiapi::board::DrcExclusion;
using kiapi::board::DrcSeveritySetting;
using kiapi::board::MinimumConstraints;
using kiapi::board::PredefinedSizes;
using kiapi::board::PresetDiffPairDimension;
using kiapi::board::PresetTrackWidth;
using kiapi::board::PresetViaDimension;
using kiapi::board::SolderMaskPasteDefaults;
using kiapi::board::ViaProtectionDefaults;
using types::CommandStatus;
using types::DocumentType;
using types::ItemRequestStatus;

namespace
{
void collectSelectedBoardItems( BOARD* aBoard, std::vector<EDA_ITEM*>& aItems )
{
    for( BOARD_ITEM* item : aBoard->GetItemSet() )
    {
        if( item->IsSelected() )
            aItems.push_back( item );

        item->RunOnChildren(
                [&]( BOARD_ITEM* child )
                {
                    if( child->IsSelected() )
                        aItems.push_back( child );
                },
                RECURSE_MODE::RECURSE );
    }
}

void clearSelectedBoardItems( BOARD* aBoard )
{
    for( BOARD_ITEM* item : aBoard->GetItemSet() )
    {
        item->ClearSelected();
        item->RunOnChildren( []( BOARD_ITEM* child ) { child->ClearSelected(); },
                             RECURSE_MODE::RECURSE );
    }
}


std::vector<BOARD_ITEM*> resolveSyncSelection(
        const BOARD* aBoard,
        const google::protobuf::RepeatedPtrField<SelectionSpec>& aSpecs )
{
    std::vector<std::pair<int, BOARD_ITEM*>> ordered;

    if( !aBoard )
        return {};

    std::vector<KIID_PATH> sheetPaths( aSpecs.size() );

    for( int index = 0; index < aSpecs.size(); ++index )
    {
        if( aSpecs[index].spec_case() == SelectionSpec::kSheetPath )
            sheetPaths[index] = UnpackSheetPath( aSpecs[index].sheet_path() );
    }

    for( FOOTPRINT* footprint : aBoard->Footprints() )
    {
        for( int index = 0; index < aSpecs.size(); ++index )
        {
            const SelectionSpec& spec = aSpecs[index];

            switch( spec.spec_case() )
            {
            case SelectionSpec::kFootprint:
                if( footprint->GetReference() == wxString::FromUTF8( spec.footprint().reference() ) )
                    ordered.emplace_back( index, footprint );
                break;

            case SelectionSpec::kPad:
                if( footprint->GetReference() == wxString::FromUTF8( spec.pad().reference() ) )
                {
                    wxString number = wxString::FromUTF8( spec.pad().number() );

                    for( PAD* pad : footprint->Pads() )
                    {
                        if( pad->GetNumber() == number )
                            ordered.emplace_back( index, pad );
                    }
                }
                break;

            case SelectionSpec::kSheetPath:
                if( footprint->GetPath().EndsWith( sheetPaths[index] ) )
                    ordered.emplace_back( index, footprint );
                break;

            default:
                break;
            }
        }
    }

    std::ranges::sort( ordered,
                       []( const auto& a, const auto& b ) { return a.first < b.first; } );

    std::vector<BOARD_ITEM*> result;
    result.reserve( ordered.size() );

    for( const auto& [index, item] : ordered )
        result.push_back( item );

    return result;
}

}


API_HANDLER_PCB::API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_PCB( CreatePcbFrameContext( aFrame ), aFrame )
{
}


API_HANDLER_PCB::API_HANDLER_PCB( std::shared_ptr<BOARD_CONTEXT> aContext,
                                  PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame ),
        m_context( std::move( aContext ) )
{
    wxCHECK( m_context, /* void */ );

    registerHandler<RunAction, RunActionResponse>( &API_HANDLER_PCB::handleRunAction );
    registerHandler<GetVariants, VariantsResponse>( &API_HANDLER_PCB::handleGetVariants );
    registerHandler<AddVariant, Empty>( &API_HANDLER_PCB::handleAddVariant );
    registerHandler<DeleteVariant, Empty>( &API_HANDLER_PCB::handleDeleteVariant );
    registerHandler<RenameVariant, Empty>( &API_HANDLER_PCB::handleRenameVariant );
    registerHandler<CopyVariant, Empty>( &API_HANDLER_PCB::handleCopyVariant );
    registerHandler<SetVariantDescription, Empty>( &API_HANDLER_PCB::handleSetVariantDescription );
    registerHandler<SetCurrentVariant, Empty>( &API_HANDLER_PCB::handleSetCurrentVariant );
    registerHandler<GetCurrentVariant, CurrentVariantResponse>( &API_HANDLER_PCB::handleGetCurrentVariant );
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_PCB::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_PCB::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, Empty>( &API_HANDLER_PCB::handleSaveCopyOfDocument );
    registerHandler<RevertDocument, Empty>( &API_HANDLER_PCB::handleRevertDocument );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetItems );
    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsById );
    registerHandler<GetItemsByNet, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNet );
    registerHandler<GetItemsByNetClass, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNetClass );
    registerHandler<GetConnectedItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetConnectedItems );
    registerHandler<CrossProbeAnnounce, CrossProbeAnnounceResponse>(
            &API_HANDLER_PCB::handleCrossProbeAnnounce );
    registerHandler<SyncSelection, SyncSelectionResponse>( &API_HANDLER_PCB::handleSyncSelection );
    registerHandler<HighlightNets, HighlightNetsResponse>( &API_HANDLER_PCB::handleHighlightNets );
    registerHandler<FocusOnItem, FocusOnItemResponse>( &API_HANDLER_PCB::handleFocusOnItem );
    registerHandler<GetEmbeddedFiles, common::types::EmbeddedFiles>(
            &API_HANDLER_PCB::handleGetEmbeddedFiles );
    registerHandler<AddEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleAddEmbeddedFiles );
    registerHandler<SetEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleSetEmbeddedFiles );

    registerHandler<GetSelection, SelectionResponse>( &API_HANDLER_PCB::handleGetSelection );
    registerHandler<ClearSelection, Empty>( &API_HANDLER_PCB::handleClearSelection );
    registerHandler<AddToSelection, SelectionResponse>( &API_HANDLER_PCB::handleAddToSelection );
    registerHandler<RemoveFromSelection, SelectionResponse>(
            &API_HANDLER_PCB::handleRemoveFromSelection );

    registerHandler<GetBoardStackup, BoardStackupResponse>( &API_HANDLER_PCB::handleGetStackup );
    registerHandler<GetBoardEnabledLayers, BoardEnabledLayersResponse>(
        &API_HANDLER_PCB::handleGetBoardEnabledLayers );
    registerHandler<SetBoardEnabledLayers, BoardEnabledLayersResponse>(
        &API_HANDLER_PCB::handleSetBoardEnabledLayers );
    registerHandler<GetGraphicsDefaults, GraphicsDefaultsResponse>(
            &API_HANDLER_PCB::handleGetGraphicsDefaults );
    registerHandler<GetBoardDesignRules, BoardDesignRulesResponse>(
            &API_HANDLER_PCB::handleGetBoardDesignRules );
    registerHandler<SetBoardDesignRules, BoardDesignRulesResponse>(
            &API_HANDLER_PCB::handleSetBoardDesignRules );
    registerHandler<GetCustomDesignRules, CustomRulesResponse>(
            &API_HANDLER_PCB::handleGetCustomDesignRules );
    registerHandler<SetCustomDesignRules, CustomRulesResponse>(
            &API_HANDLER_PCB::handleSetCustomDesignRules );
    registerHandler<GetBoundingBox, GetBoundingBoxResponse>(
            &API_HANDLER_PCB::handleGetBoundingBox );
    registerHandler<GetPadShapeAsPolygon, PadShapeAsPolygonResponse>(
            &API_HANDLER_PCB::handleGetPadShapeAsPolygon );
    registerHandler<CheckPadstackPresenceOnLayers, PadstackPresenceResponse>(
            &API_HANDLER_PCB::handleCheckPadstackPresenceOnLayers );
    registerHandler<GetTitleBlockInfo, types::TitleBlockInfo>(
            &API_HANDLER_PCB::handleGetTitleBlockInfo );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_PCB::handleExpandTextVariables );
    registerHandler<GetBoardOrigin, types::Vector2>( &API_HANDLER_PCB::handleGetBoardOrigin );
    registerHandler<SetBoardOrigin, Empty>( &API_HANDLER_PCB::handleSetBoardOrigin );
    registerHandler<GetBoardLayerName, BoardLayerNameResponse>( &API_HANDLER_PCB::handleGetBoardLayerName );
    registerHandler<GetBoardLayerByName, BoardLayerResponse>( &API_HANDLER_PCB::handleGetBoardLayerByName );

    registerHandler<InteractiveMoveItems, Empty>( &API_HANDLER_PCB::handleInteractiveMoveItems );
    registerHandler<GetNets, NetsResponse>( &API_HANDLER_PCB::handleGetNets );
    registerHandler<GetNetClassForNets, NetClassForNetsResponse>(
            &API_HANDLER_PCB::handleGetNetClassForNets );
    registerHandler<RefillZones, Empty>( &API_HANDLER_PCB::handleRefillZones );
    registerHandler<ImportNetlist, ImportNetlistResponse>( &API_HANDLER_PCB::handleImportNetlist );

    registerHandler<SaveDocumentToString, SavedDocumentResponse>(
            &API_HANDLER_PCB::handleSaveDocumentToString );
    registerHandler<SaveSelectionToString, SavedSelectionResponse>(
            &API_HANDLER_PCB::handleSaveSelectionToString );
    registerHandler<ParseAndCreateItemsFromString, CreateItemsResponse>(
            &API_HANDLER_PCB::handleParseAndCreateItemsFromString );
    registerHandler<GetVisibleLayers, BoardLayers>( &API_HANDLER_PCB::handleGetVisibleLayers );
    registerHandler<SetVisibleLayers, Empty>( &API_HANDLER_PCB::handleSetVisibleLayers );
    registerHandler<GetActiveLayer, BoardLayerResponse>( &API_HANDLER_PCB::handleGetActiveLayer );
    registerHandler<SetActiveLayer, Empty>( &API_HANDLER_PCB::handleSetActiveLayer );
    registerHandler<GetBoardEditorAppearanceSettings, BoardEditorAppearanceSettings>(
            &API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings );
    registerHandler<SetBoardEditorAppearanceSettings, Empty>(
            &API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings );
    registerHandler<GetBoardPlotSettings, BoardPlotSettingsResponse>(
            &API_HANDLER_PCB::handleGetBoardPlotSettings );
    registerHandler<SetBoardPlotSettings, Empty>( &API_HANDLER_PCB::handleSetBoardPlotSettings );
    registerHandler<InjectDrcError, InjectDrcErrorResponse>(
            &API_HANDLER_PCB::handleInjectDrcError );

        registerHandler<RunBoardJobExport3D, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExport3D );
        registerHandler<RunBoardJobExportRender, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportRender );
        registerHandler<RunBoardJobExportSvg, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportSvg );
        registerHandler<RunBoardJobExportDxf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDxf );
        registerHandler<RunBoardJobExportPdf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPdf );
        registerHandler<RunBoardJobExportPs, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPs );
        registerHandler<RunBoardJobExportGerbers, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGerbers );
        registerHandler<RunBoardJobExportDrill, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDrill );
        registerHandler<RunBoardJobExportPosition, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPosition );
        registerHandler<RunBoardJobExportGencad, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGencad );
        registerHandler<RunBoardJobExportIpc2581, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpc2581 );
        registerHandler<RunBoardJobExportIpcD356, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpcD356 );
        registerHandler<RunBoardJobExportODB, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportODB );
        registerHandler<RunBoardJobExportStats, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportStats );
}


std::optional<bool> API_HANDLER_PCB::documentIsModified() const
{
    return board()->IsModified() || hasPendingChanges();
}


PCB_EDIT_FRAME* API_HANDLER_PCB::frame() const
{
    return static_cast<PCB_EDIT_FRAME*>( m_frame );
}


std::optional<ApiResponseStatus> API_HANDLER_PCB::checkForHeadless( const std::string& aCommandName ) const
{
    if( frame() )
        return std::nullopt;

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
    e.set_error_message( fmt::format( "{} is not available in headless mode", aCommandName ) );
    return e;
}


HANDLER_RESULT<RunActionResponse> API_HANDLER_PCB::handleRunAction(
        const HANDLER_CONTEXT<RunAction>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "RunAction" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    RunActionResponse response;

    if( toolManager()->RunAction( aCtx.Request.action(), true ) )
        response.set_status( RunActionStatus::RAS_OK );
    else
        response.set_status( RunActionStatus::RAS_INVALID );

    return response;
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_PCB::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( context()->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_PCB );
    doc.set_board_filename( fn.GetFullName() );

    doc.mutable_project()->set_name( project().GetProjectName().ToStdString() );
    doc.mutable_project()->set_path( project().GetProjectDirectory().ToStdString() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveDocument(
        const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !context()->SaveBoard() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save PCB document" );
        return tl::unexpected( e );
    }

    // SaveBoard() is implemented by both the GUI and headless contexts.  The
    // latter writes through PCB_IO directly, so clear the root dirty flag here
    // after a confirmed successful write just as the GUI save path does.
    board()->ClearFlags( IS_CHANGED );
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveCopyOfDocument(
        const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName boardPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !boardPath.IsOk() || !boardPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' could not be opened",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.FileExists()
        && ( !boardPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.GetExt() != FILEEXT::KiCadPcbFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_pcb extension",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    BOARD* board = this->board();

    if( board->GetFileName().Matches( boardPath.GetFullPath() ) )
    {
        if( !context()->SaveBoard() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "failed to save PCB document" );
            return tl::unexpected( e );
        }

        board->ClearFlags( IS_CHANGED );
        return Empty();
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    if( !context()->SavePcbCopy( boardPath.GetFullPath(), includeProject, /* aHeadless = */ true ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save PCB copy" );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRevertDocument(
        const HANDLER_CONTEXT<RevertDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "RevertDocument" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName fn = project().AbsolutePath( board()->GetFileName() );

    frame()->GetScreen()->SetContentModified( false );
    frame()->ReleaseFile();
    frame()->OpenProjectFiles( std::vector<wxString>( 1, fn.GetFullPath() ), KICTL_REVERT );

    return Empty();
}


void API_HANDLER_PCB::pushCurrentCommit( const std::string& aClientName, const wxString& aMessage )
{
    const auto current = m_commits.find( aClientName );
    const bool hadPendingChanges = current != m_commits.end() && current->second.second
                                   && !current->second.second->Empty();
    API_HANDLER_EDITOR::pushCurrentCommit( aClientName, aMessage );

    // A headless BOARD_COMMIT has no frame to mark the document dirty.
    if( hadPendingChanges )
        onModified();

    if( frame() )
        frame()->Refresh();
}


std::unique_ptr<COMMIT> API_HANDLER_PCB::createCommit()
{
    if( frame() )
        return std::make_unique<BOARD_COMMIT>( frame() );

    return std::make_unique<BOARD_COMMIT>( toolManager(), true, false );
}


std::optional<BOARD_ITEM*> API_HANDLER_PCB::getItemById( const KIID& aId ) const
{
    BOARD_ITEM* item = board()->ResolveItem( aId, true );

    if( !item )
        return std::nullopt;

    return item;
}


tl::expected<bool, ApiResponseStatus>
API_HANDLER_PCB::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_PCB )
        return false;

    wxFileName fn( context()->GetCurrentFileName() );
    return 0 == aDocument.board_filename().compare( fn.GetFullName() );
}


HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> API_HANDLER_PCB::createItemForType( KICAD_T aType,
        BOARD_ITEM_CONTAINER* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == PCB_PAD_T && !dynamic_cast<FOOTPRINT*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pad in {}, which is not a footprint",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == PCB_FOOTPRINT_T && !dynamic_cast<BOARD*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a footprint in {}, which is not a board",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_ITEM> created = CreateItemForType( aType, aContainer );

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


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_PCB::handleCreateUpdateItemsInternal( bool aCreate,
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

    BOARD* board = this->board();
    BOARD_ITEM_CONTAINER* container = board;

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;
        std::optional<BOARD_ITEM*> optItem = getItemById( containerId );

        if( optItem )
        {
            container = dynamic_cast<BOARD_ITEM_CONTAINER*>( *optItem );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid board item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    BOARD_COMMIT* commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aClientName ) );

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

        if( aCreate && *type == PCB_TABLECELL_T )
        {
            status.set_code( ItemStatusCode::ISC_IMMUTABLE );
            status.set_error_message( "table cells cannot be created independently; update the table instead" );
            aItemHandler( status, anyItem );
            continue;
        }

        if( type == PCB_DIMENSION_T )
        {
            board::types::Dimension dimension;
            anyItem.UnpackTo( &dimension );

            switch( dimension.dimension_style_case() )
            {
            case board::types::Dimension::kAligned:    type = PCB_DIM_ALIGNED_T;    break;
            case board::types::Dimension::kOrthogonal: type = PCB_DIM_ORTHOGONAL_T; break;
            case board::types::Dimension::kRadial:     type = PCB_DIM_RADIAL_T;     break;
            case board::types::Dimension::kLeader:     type = PCB_DIM_LEADER_T;     break;
            case board::types::Dimension::kCenter:     type = PCB_DIM_CENTER_T;     break;
            case board::types::Dimension::DIMENSION_STYLE_NOT_SET: break;
            }
        }

        HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> creationResult =
                createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<BOARD_ITEM> item( std::move( *creationResult ) );

        if( !item->Deserialize( anyItem ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( aCreate && item->Type() == PCB_FIELD_T
            && static_cast<PCB_FIELD*>( item.get() )->IsMandatory() )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message( "mandatory footprint fields cannot be created; create a user field instead" );
            aItemHandler( status, anyItem );
            continue;
        }

        std::optional<BOARD_ITEM*> optItem = getItemById( item->m_Uuid );

        if( aCreate && optItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !optItem )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( aCreate && item->Type() != PCB_GROUP_T
            && !( board->GetEnabledLayers() & item->GetLayerSet() ).any() )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message(
                "attempted to add item with no overlapping layers with the board" );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            if( item->Type() == PCB_FOOTPRINT_T )
            {
                // Ensure children have unique identifiers; in case the API client created this new
                // footprint by cloning an existing one and only changing the parent UUID.
                item->RunOnChildren(
                        []( BOARD_ITEM* aChild )
                        {
                            const_cast<KIID&>( aChild->m_Uuid ) = KIID();
                        },
                        RECURSE );
            }

            item->Serialize( newItem );

            // BOARD_COMMIT records footprint children at the parent-footprint
            // undo level in the board editor.  Insert the child into that
            // parent explicitly or the commit contains only a parent image
            // and the created pad/field is lost.
            if( FOOTPRINT* parentFootprint = item->GetParentFootprint() )
            {
                commit->Modify( parentFootprint );
                BOARD_ITEM* child = item.release();
                parentFootprint->Add( child );

                if( KIGFX::VIEW* view = toolManager()->GetView() )
                    view->Add( child );
            }
            else
            {
                commit->Add( item.release() );
            }
        }
        else
        {
            BOARD_ITEM* boardItem = *optItem;

            // Footprints can't be modified by CopyFrom at the moment because the commit system
            // doesn't currently know what to do with a footprint that has had its children
            // replaced with other children; which results in things like the view not having its
            // cached geometry for footprint children updated when you move a footprint around.
            // And also, groups are special because they can contain any item type, so we
            // can't use CopyFrom on them either.
            if( boardItem->Type() == PCB_FOOTPRINT_T  || boardItem->Type() == PCB_GROUP_T )
            {
                // Save group membership before removal, since Remove() severs the relationship
                PCB_GROUP* parentGroup = dynamic_cast<PCB_GROUP*>( boardItem->GetParentGroup() );

                commit->Remove( boardItem );
                item->Serialize( newItem );

                BOARD_ITEM* newBoardItem = item.release();
                commit->Add( newBoardItem );

                // Restore group membership for the newly added item
                if( parentGroup )
                    parentGroup->AddItem( newBoardItem );
            }
            else
            {
                commit->Modify( boardItem );
                boardItem->CopyFrom( item.get() );
                boardItem->Serialize( newItem );
            }
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.count( aClientName ) )
    {
        pushCurrentCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }


    return ItemRequestStatus::IRS_OK;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;

    BOARD* board = this->board();
    std::vector<BOARD_ITEM*> items;
    std::set<KICAD_T> typesRequested, typesInserted;
    bool handledAnything = false;

    for( int typeRaw : aCtx.Request.types() )
    {
        auto typeMessage = static_cast<common::types::KiCadObjectType>( typeRaw );
        KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage );

        if( type == TYPE_NOT_INIT )
            continue;

        typesRequested.emplace( type );

        if( typesInserted.count( type ) )
            continue;

        switch( type )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
            handledAnything = true;
            std::copy( board->Tracks().begin(), board->Tracks().end(),
                       std::back_inserter( items ) );
            typesInserted.insert( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } );
            break;

        case PCB_PAD_T:
        {
            handledAnything = true;

            for( FOOTPRINT* fp : board->Footprints() )
            {
                std::copy( fp->Pads().begin(), fp->Pads().end(),
                           std::back_inserter( items ) );
            }

            typesInserted.insert( PCB_PAD_T );
            break;
        }

        case PCB_FOOTPRINT_T:
        {
            handledAnything = true;

            std::copy( board->Footprints().begin(), board->Footprints().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_FOOTPRINT_T );
            break;
        }

        case PCB_DIMENSION_T:
        {
            handledAnything = true;
            const std::set<KICAD_T> dimensions = { PCB_DIM_ALIGNED_T, PCB_DIM_ORTHOGONAL_T,
                                                 PCB_DIM_RADIAL_T, PCB_DIM_LEADER_T,
                                                 PCB_DIM_CENTER_T };
            typesRequested.insert( dimensions.begin(), dimensions.end() );

            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( dimensions.count( item->Type() ) )
                    items.emplace_back( item );
            }

            typesInserted.insert( PCB_DIMENSION_T );
            break;
        }

        case PCB_REFERENCE_IMAGE_T:
        case PCB_SHAPE_T:
        case PCB_TABLE_T:
        case PCB_TEXT_T:
        case PCB_TEXTBOX_T:
        case PCB_BARCODE_T:
        {
            handledAnything = true;
            bool inserted = false;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( item->Type() == type )
                {
                    items.emplace_back( item );
                    inserted = true;
                }
            }

            if( inserted )
                typesInserted.insert( type );

            break;
        }

        case PCB_TABLECELL_T:
        {
            handledAnything = true;

            auto collectCells = [&]( const DRAWINGS& drawings )
            {
                for( BOARD_ITEM* drawing : drawings )
                {
                    if( PCB_TABLE* table = dynamic_cast<PCB_TABLE*>( drawing ) )
                    {
                        for( PCB_TABLECELL* cell : table->GetCells() )
                            items.emplace_back( cell );
                    }
                }
            };

            collectCells( board->Drawings() );

            for( FOOTPRINT* footprint : board->Footprints() )
                collectCells( footprint->GraphicalItems() );

            typesInserted.insert( PCB_TABLECELL_T );
            break;
        }

        case PCB_ZONE_T:
        {
            handledAnything = true;

            std::copy( board->Zones().begin(), board->Zones().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_ZONE_T );
            break;
        }

        case PCB_GROUP_T:
        {
            handledAnything = true;

            std::copy( board->Groups().begin(), board->Groups().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_GROUP_T );
            break;
        }
        default:
            break;
        }
    }

    if( !handledAnything )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    for( const BOARD_ITEM* item : items )
    {
        if( !typesRequested.count( item->Type() ) )
            continue;

        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsById(
        const HANDLER_CONTEXT<GetItemsById>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;

    std::vector<BOARD_ITEM*> items;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            items.emplace_back( *item );
    }

    if( items.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid" );
        return tl::unexpected( e );
    }

    for( const BOARD_ITEM* item : items )
    {
        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}

void API_HANDLER_PCB::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    BOARD* board = this->board();
    std::vector<BOARD_ITEM*> validatedItems;

    for( std::pair<const KIID, ItemDeletionStatus> pair : aItemsToDelete )
    {
        if( BOARD_ITEM* item = board->ResolveItem( pair.first, true ) )
        {
            if( item->Type() == PCB_FIELD_T && static_cast<PCB_FIELD*>( item )->IsMandatory() )
            {
                aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_IMMUTABLE;
                continue;
            }

            if( item->Type() == PCB_TABLECELL_T )
            {
                aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_IMMUTABLE;
                continue;
            }

            validatedItems.push_back( item );
            aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_OK;
        }

        // Note: we don't currently support locking items from API modification, but here is where
        // to add it in the future (and return IDS_IMMUTABLE)
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    // BOARD_COMMIT owns ordinary child removal, including connectivity and
    // undo bookkeeping.  Custom fields are special: its CHT_REMOVE path only
    // hides mandatory-field slots, so detach user fields through a parent
    // snapshot and dispose of the owned child here.
    for( BOARD_ITEM* item : validatedItems )
    {
        if( item->Type() == PCB_FIELD_T && !static_cast<PCB_FIELD*>( item )->IsMandatory()
            && item->GetParentFootprint() )
        {
            FOOTPRINT* parentFootprint = item->GetParentFootprint();
            commit->Modify( parentFootprint );

            if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
                selectionTool->RemoveItemFromSel( item, true /* quiet */ );

            if( KIGFX::VIEW* view = toolManager()->GetView() )
                view->Remove( item );

            parentFootprint->Remove( item );
            delete item;
        }
        else
        {
            commit->Remove( item );
        }
    }

    if( !m_activeClients.count( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_PCB::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    return getItemById( aId );
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleGetSelection(
            const HANDLER_CONTEXT<GetSelection>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> filter;

    for( int typeRaw : aCtx.Request.types() )
    {
        auto typeMessage = static_cast<types::KiCadObjectType>( typeRaw );
        KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage );

        if( type == TYPE_NOT_INIT )
            continue;

        filter.insert( type );
    }

    SelectionResponse response;

    std::vector<EDA_ITEM*> selected;

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
            for( EDA_ITEM* item : selectionTool->GetSelection() )
                selected.push_back( item );
    }
    else
    {
        collectSelectedBoardItems( board(), selected );
    }

    for( EDA_ITEM* item : selected )
    {
        if( filter.empty() || filter.contains( item->Type() ) )
            item->Serialize( *response.add_items() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleClearSelection(
        const HANDLER_CONTEXT<ClearSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( frame() )
    {
        toolManager()->RunAction( ACTIONS::selectionClear );
        frame()->Refresh();
    }
    else
    {
        clearSelectedBoardItems( board() );
    }

    return Empty();
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleAddToSelection(
        const HANDLER_CONTEXT<AddToSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<EDA_ITEM*> toAdd;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toAdd.emplace_back( *item );
    }

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
            selectionTool->AddItemsToSel( &toAdd );
        frame()->Refresh();
    }
    else
    {
        for( EDA_ITEM* item : toAdd )
            item->SetSelected();
    }

    SelectionResponse response;

    std::vector<EDA_ITEM*> selected;

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
            for( EDA_ITEM* item : selectionTool->GetSelection() )
                selected.push_back( item );
    }
    else
    {
        collectSelectedBoardItems( board(), selected );
    }

    for( EDA_ITEM* item : selected )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_PCB::handleRemoveFromSelection(
        const HANDLER_CONTEXT<RemoveFromSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::vector<EDA_ITEM*> toRemove;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toRemove.emplace_back( *item );
    }

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
            selectionTool->RemoveItemsFromSel( &toRemove );
        frame()->Refresh();
    }
    else
    {
        for( EDA_ITEM* item : toRemove )
            item->ClearSelected();
    }

    SelectionResponse response;

    std::vector<EDA_ITEM*> selected;

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
            for( EDA_ITEM* item : selectionTool->GetSelection() )
                selected.push_back( item );
    }
    else
    {
        collectSelectedBoardItems( board(), selected );
    }

    for( EDA_ITEM* item : selected )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<BoardStackupResponse> API_HANDLER_PCB::handleGetStackup(
        const HANDLER_CONTEXT<GetBoardStackup>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardStackupResponse  response;
    google::protobuf::Any any;

    board()->GetStackupOrDefault().Serialize( any );

    any.UnpackTo( response.mutable_stackup() );

    // User-settable layer names are not stored in BOARD_STACKUP at the moment
    for( board::BoardStackupLayer& layer : *response.mutable_stackup()->mutable_layers() )
    {
        if( layer.type() == board::BoardStackupLayerType::BSLT_DIELECTRIC )
            continue;

        PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( layer.layer() );
        wxCHECK2( id != UNDEFINED_LAYER, continue );

        layer.set_user_name( board()->GetLayerName( id ) );
    }

    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleGetBoardEnabledLayers(
        const HANDLER_CONTEXT<GetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardEnabledLayersResponse response;

    BOARD* board = this->board();
    int copperLayerCount = board->GetCopperLayerCount();

    response.set_copper_layer_count( copperLayerCount );

    LSET enabled = board->GetEnabledLayers();

    // The Rescue layer is an internal detail and should be hidden from the API
    enabled.reset( Rescue );

    // Just in case this is out of sync; the API should always return the expected copper layers
    enabled |= LSET::AllCuMask( copperLayerCount );

    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleSetBoardEnabledLayers(
        const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.copper_layer_count() < 2 || aCtx.Request.copper_layer_count() % 2 != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "copper_layer_count must be an even number of at least 2" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.copper_layer_count() > MAX_CU_LAYERS )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "copper_layer_count must be at most {}", MAX_CU_LAYERS ) );
        return tl::unexpected( e );
    }

    int copperLayerCount = static_cast<int>( aCtx.Request.copper_layer_count() );
    LSET enabled = board::UnpackLayerSet( aCtx.Request.layers() );

    // Sanitize the input
    enabled |= LSET( { Edge_Cuts, Margin, F_CrtYd, B_CrtYd } );
    enabled &= ~LSET::AllCuMask();
    enabled |= LSET::AllCuMask( copperLayerCount );

    BOARD* board = this->board();

    LSET previousEnabled = board->GetEnabledLayers();
    LSET changedLayers = enabled ^ previousEnabled;

    board->SetEnabledLayers( enabled );
    board->SetVisibleLayers( board->GetVisibleLayers() | changedLayers );

    LSEQ removedLayers;

    for( PCB_LAYER_ID layer_id : previousEnabled )
    {
        if( !enabled[layer_id] && board->HasItemsOnLayer( layer_id ) )
            removedLayers.push_back( layer_id );
    }

    bool modified = false;

    if( !removedLayers.empty() )
    {
        if( frame() )
            toolManager()->RunAction( PCB_ACTIONS::selectionClear );

        for( PCB_LAYER_ID layer_id : removedLayers )
            modified |= board->RemoveAllItemsOnLayer( layer_id );
    }

    if( enabled != previousEnabled )
    {
        if( frame() )
            frame()->UpdateUserInterface();

        onModified();
    }

    if( modified )
    {
        if( frame() )
            frame()->OnModify();
        else
            onModified();
    }

    BoardEnabledLayersResponse response;

    response.set_copper_layer_count( copperLayerCount );
    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<GraphicsDefaultsResponse> API_HANDLER_PCB::handleGetGraphicsDefaults(
        const HANDLER_CONTEXT<GetGraphicsDefaults>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();
    GraphicsDefaultsResponse response;

    // TODO: This should change to be an enum class
    constexpr std::array<kiapi::board::BoardLayerClass, LAYER_CLASS_COUNT> classOrder = {
        kiapi::board::BLC_SILKSCREEN,
        kiapi::board::BLC_COPPER,
        kiapi::board::BLC_EDGES,
        kiapi::board::BLC_COURTYARD,
        kiapi::board::BLC_FABRICATION,
        kiapi::board::BLC_OTHER
    };

    for( int i = 0; i < LAYER_CLASS_COUNT; ++i )
    {
        kiapi::board::BoardLayerGraphicsDefaults* l = response.mutable_defaults()->add_layers();

        l->set_layer( classOrder[i] );
        l->mutable_line_thickness()->set_value_nm( bds.m_LineThickness[i] );

        kiapi::common::types::TextAttributes* text = l->mutable_text();
        text->mutable_size()->set_x_nm( bds.m_TextSize[i].x );
        text->mutable_size()->set_y_nm( bds.m_TextSize[i].y );
        text->mutable_stroke_width()->set_value_nm( bds.m_TextThickness[i] );
        text->set_italic( bds.m_TextItalic[i] );
        text->set_keep_upright( bds.m_TextUpright[i] );
    }

    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleGetBoardDesignRules(
        const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();
    BoardDesignRulesResponse response;
    BoardDesignRules* rules = response.mutable_rules();
    MinimumConstraints* constraints = rules->mutable_constraints();

    constraints->mutable_min_clearance()->set_value_nm( bds.m_MinClearance );
    constraints->mutable_min_groove_width()->set_value_nm( bds.m_MinGrooveWidth );
    constraints->mutable_min_connection_width()->set_value_nm( bds.m_MinConn );
    constraints->mutable_min_track_width()->set_value_nm( bds.m_TrackMinWidth );
    constraints->mutable_min_via_annular_width()->set_value_nm( bds.m_ViasMinAnnularWidth );
    constraints->mutable_min_via_size()->set_value_nm( bds.m_ViasMinSize );
    constraints->mutable_min_through_drill()->set_value_nm( bds.m_MinThroughDrill );
    constraints->mutable_min_microvia_size()->set_value_nm( bds.m_MicroViasMinSize );
    constraints->mutable_min_microvia_drill()->set_value_nm( bds.m_MicroViasMinDrill );
    constraints->mutable_copper_edge_clearance()->set_value_nm( bds.m_CopperEdgeClearance );
    constraints->mutable_hole_clearance()->set_value_nm( bds.m_HoleClearance );
    constraints->mutable_hole_to_hole_min()->set_value_nm( bds.m_HoleToHoleMin );
    constraints->mutable_silk_clearance()->set_value_nm( bds.m_SilkClearance );
    constraints->set_min_resolved_spokes( bds.m_MinResolvedSpokes );
    constraints->mutable_min_silk_text_height()->set_value_nm( bds.m_MinSilkTextHeight );
    constraints->mutable_min_silk_text_thickness()->set_value_nm( bds.m_MinSilkTextThickness );

    PredefinedSizes* sizes = rules->mutable_predefined_sizes();

    for( size_t ii = 1; ii < bds.m_TrackWidthList.size(); ++ii )
        sizes->add_tracks()->mutable_width()->set_value_nm( bds.m_TrackWidthList[ii] );

    for( size_t ii = 1; ii < bds.m_ViasDimensionsList.size(); ++ii )
    {
        PresetViaDimension* via = sizes->add_vias();
        via->mutable_diameter()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Diameter );
        via->mutable_drill()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Drill );
    }

    for( size_t ii = 1; ii < bds.m_DiffPairDimensionsList.size(); ++ii )
    {
        PresetDiffPairDimension* pair = sizes->add_diff_pairs();
        pair->mutable_width()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Width );
        pair->mutable_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Gap );
        pair->mutable_via_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_ViaGap );
    }

    SolderMaskPasteDefaults* maskPaste = rules->mutable_solder_mask_paste();
    maskPaste->mutable_mask_expansion()->set_value_nm( bds.m_SolderMaskExpansion );
    maskPaste->mutable_mask_min_width()->set_value_nm( bds.m_SolderMaskMinWidth );
    maskPaste->mutable_mask_to_copper_clearance()->set_value_nm( bds.m_SolderMaskToCopperClearance );
    maskPaste->mutable_paste_margin()->set_value_nm( bds.m_SolderPasteMargin );
    maskPaste->set_paste_margin_ratio( bds.m_SolderPasteMarginRatio );
    maskPaste->set_allow_soldermask_bridges_in_footprints( bds.m_AllowSoldermaskBridgesInFPs );

    kiapi::board::TeardropDefaults* teardrops = rules->mutable_teardrops();

    teardrops->set_target_vias( bds.m_TeardropParamsList.m_TargetVias );
    teardrops->set_target_pth_pads( bds.m_TeardropParamsList.m_TargetPTHPads );
    teardrops->set_target_smd_pads( bds.m_TeardropParamsList.m_TargetSMDPads );
    teardrops->set_target_track_to_track( bds.m_TeardropParamsList.m_TargetTrack2Track );
    teardrops->set_use_round_shapes_only( bds.m_TeardropParamsList.m_UseRoundShapesOnly );

    TEARDROP_PARAMETERS_LIST& tdList = const_cast<TEARDROP_PARAMETERS_LIST&>( bds.m_TeardropParamsList );

    for( int target = TARGET_ROUND; target <= TARGET_TRACK; ++target )
    {
        const TEARDROP_PARAMETERS* params = tdList.GetParameters( static_cast<TARGET_TD>( target ) );
        kiapi::board::TeardropTargetEntry* entry = teardrops->add_target_params();

        entry->set_target( target == TARGET_ROUND ? board::TDT_ROUND
                           : target == TARGET_RECT ? board::TDT_RECT : board::TDT_TRACK );
        entry->mutable_params()->set_enabled( params->m_Enabled );
        entry->mutable_params()->mutable_max_length()->set_value_nm( params->m_TdMaxLen );
        entry->mutable_params()->mutable_max_width()->set_value_nm( params->m_TdMaxWidth );
        entry->mutable_params()->set_best_length_ratio( params->m_BestLengthRatio );
        entry->mutable_params()->set_best_width_ratio( params->m_BestWidthRatio );
        entry->mutable_params()->set_width_to_size_filter_ratio( params->m_WidthtoSizeFilterRatio );
        entry->mutable_params()->set_curved_edges( params->m_CurvedEdges );
        entry->mutable_params()->set_allow_two_tracks( params->m_AllowUseTwoTracks );
        entry->mutable_params()->set_on_pads_in_zones( params->m_TdOnPadsInZones );
    }

    ViaProtectionDefaults* viaProtection = rules->mutable_via_protection();
    viaProtection->set_tent_front( bds.m_TentViasFront );
    viaProtection->set_tent_back( bds.m_TentViasBack );
    viaProtection->set_cover_front( bds.m_CoverViasFront );
    viaProtection->set_cover_back( bds.m_CoverViasBack );
    viaProtection->set_plug_front( bds.m_PlugViasFront );
    viaProtection->set_plug_back( bds.m_PlugViasBack );
    viaProtection->set_cap( bds.m_CapVias );
    viaProtection->set_fill( bds.m_FillVias );

    for( const auto& [errorCode, severity] : bds.m_DRCSeverities )
    {
        std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( errorCode );

        if( !drcItem || drcItem->GetSettingsKey().IsEmpty() )
            continue;

        DrcSeveritySetting* setting = rules->add_severities();
        setting->set_error_key( drcItem->GetSettingsKey().ToStdString() );
        setting->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( severity ) );
    }

    for( const wxString& serialized : bds.m_DrcExclusions )
    {
        DrcExclusion* exclusion = rules->add_exclusions();
        exclusion->mutable_marker()->mutable_id()->set_opaque_id( serialized.ToStdString() );

        auto it = bds.m_DrcExclusionComments.find( serialized );

        if( it != bds.m_DrcExclusionComments.end() )
            exclusion->set_comment( it->second.ToStdString() );
    }

    response.set_custom_rules_status( CustomRulesStatus::CRS_NONE );
    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleSetBoardDesignRules(
        const HANDLER_CONTEXT<SetBoardDesignRules>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BOARD_DESIGN_SETTINGS settings( board()->GetDesignSettings() );
    const BoardDesignRules& rules = aCtx.Request.rules();

    if( rules.has_constraints() )
    {
        const MinimumConstraints& constraints = rules.constraints();
        settings.m_MinClearance = constraints.min_clearance().value_nm();
        settings.m_MinGrooveWidth = constraints.min_groove_width().value_nm();
        settings.m_MinConn = constraints.min_connection_width().value_nm();
        settings.m_TrackMinWidth = constraints.min_track_width().value_nm();
        settings.m_ViasMinAnnularWidth = constraints.min_via_annular_width().value_nm();
        settings.m_ViasMinSize = constraints.min_via_size().value_nm();
        settings.m_MinThroughDrill = constraints.min_through_drill().value_nm();
        settings.m_MicroViasMinSize = constraints.min_microvia_size().value_nm();
        settings.m_MicroViasMinDrill = constraints.min_microvia_drill().value_nm();
        settings.m_CopperEdgeClearance = constraints.copper_edge_clearance().value_nm();
        settings.m_HoleClearance = constraints.hole_clearance().value_nm();
        settings.m_HoleToHoleMin = constraints.hole_to_hole_min().value_nm();
        settings.m_SilkClearance = constraints.silk_clearance().value_nm();
        settings.m_MinResolvedSpokes = constraints.min_resolved_spokes();
        settings.m_MinSilkTextHeight = constraints.min_silk_text_height().value_nm();
        settings.m_MinSilkTextThickness = constraints.min_silk_text_thickness().value_nm();
    }

    if( rules.has_predefined_sizes() )
    {
        settings.m_TrackWidthList.clear();
        settings.m_TrackWidthList.emplace_back( 0 );

        for( const PresetTrackWidth& track : rules.predefined_sizes().tracks() )
            settings.m_TrackWidthList.emplace_back( track.width().value_nm() );

        settings.m_ViasDimensionsList.clear();
        settings.m_ViasDimensionsList.emplace_back( 0, 0 );

        for( const PresetViaDimension& via : rules.predefined_sizes().vias() )
            settings.m_ViasDimensionsList.emplace_back( static_cast<int>( via.diameter().value_nm() ),
                                                       static_cast<int>( via.drill().value_nm() ) );

        settings.m_DiffPairDimensionsList.clear();
        settings.m_DiffPairDimensionsList.emplace_back( 0, 0, 0 );

        for( const PresetDiffPairDimension& pair : rules.predefined_sizes().diff_pairs() )
            settings.m_DiffPairDimensionsList.emplace_back( static_cast<int>( pair.width().value_nm() ),
                                                           static_cast<int>( pair.gap().value_nm() ),
                                                           static_cast<int>( pair.via_gap().value_nm() ) );
    }

    if( rules.has_solder_mask_paste() )
    {
        const SolderMaskPasteDefaults& maskPaste = rules.solder_mask_paste();
        settings.m_SolderMaskExpansion = maskPaste.mask_expansion().value_nm();
        settings.m_SolderMaskMinWidth = maskPaste.mask_min_width().value_nm();
        settings.m_SolderMaskToCopperClearance = maskPaste.mask_to_copper_clearance().value_nm();
        settings.m_SolderPasteMargin = maskPaste.paste_margin().value_nm();
        settings.m_SolderPasteMarginRatio = maskPaste.paste_margin_ratio();
        settings.m_AllowSoldermaskBridgesInFPs = maskPaste.allow_soldermask_bridges_in_footprints();
    }

    if( rules.has_teardrops() )
    {
        const kiapi::board::TeardropDefaults& teardrops = rules.teardrops();

        settings.m_TeardropParamsList.m_TargetVias = teardrops.target_vias();
        settings.m_TeardropParamsList.m_TargetPTHPads = teardrops.target_pth_pads();
        settings.m_TeardropParamsList.m_TargetSMDPads = teardrops.target_smd_pads();
        settings.m_TeardropParamsList.m_TargetTrack2Track = teardrops.target_track_to_track();
        settings.m_TeardropParamsList.m_UseRoundShapesOnly = teardrops.use_round_shapes_only();

        for( const kiapi::board::TeardropTargetEntry& entry : teardrops.target_params() )
        {
            TARGET_TD target;

            switch( entry.target() )
            {
            case board::TDT_ROUND: target = TARGET_ROUND; break;
            case board::TDT_RECT: target = TARGET_RECT; break;
            case board::TDT_TRACK: target = TARGET_TRACK; break;
            default:
            {
                ApiResponseStatus error;
                error.set_status( AS_BAD_REQUEST );
                error.set_error_message( "invalid teardrop target" );
                return tl::unexpected( error );
            }
            }

            TEARDROP_PARAMETERS* params = settings.m_TeardropParamsList.GetParameters( target );

            params->m_Enabled = entry.params().enabled();
            params->m_TdMaxLen = entry.params().max_length().value_nm();
            params->m_TdMaxWidth = entry.params().max_width().value_nm();
            params->m_BestLengthRatio = entry.params().best_length_ratio();
            params->m_BestWidthRatio = entry.params().best_width_ratio();
            params->m_WidthtoSizeFilterRatio = entry.params().width_to_size_filter_ratio();
            params->m_CurvedEdges = entry.params().curved_edges();
            params->m_AllowUseTwoTracks = entry.params().allow_two_tracks();
            params->m_TdOnPadsInZones = entry.params().on_pads_in_zones();
        }
    }

    if( rules.has_via_protection() )
    {
        const ViaProtectionDefaults& viaProtection = rules.via_protection();
        settings.m_TentViasFront = viaProtection.tent_front();
        settings.m_TentViasBack = viaProtection.tent_back();
        settings.m_CoverViasFront = viaProtection.cover_front();
        settings.m_CoverViasBack = viaProtection.cover_back();
        settings.m_PlugViasFront = viaProtection.plug_front();
        settings.m_PlugViasBack = viaProtection.plug_back();
        settings.m_CapVias = viaProtection.cap();
        settings.m_FillVias = viaProtection.fill();
    }

    if( rules.severities_size() )
    {
        for( const DrcSeveritySetting& severity : rules.severities() )
        {
            const wxString errorKey = wxString::FromUTF8( severity.error_key() );
            std::shared_ptr<DRC_ITEM> item = DRC_ITEM::Create( errorKey );

            // Stable 10.0 has severity entries (for example via_diameter) outside the
            // settings-dialog list used by Create(string). Accept every key we expose.
            if( !item )
            {
                for( const auto& [errorCode, currentSeverity] : settings.m_DRCSeverities )
                {
                    std::shared_ptr<DRC_ITEM> candidate = DRC_ITEM::Create( errorCode );

                    if( candidate && candidate->GetSettingsKey() == errorKey )
                    {
                        item = std::move( candidate );
                        break;
                    }
                }
            }

            if( !item )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "Unknown DRC error key '{}'", severity.error_key() ) );
                return tl::unexpected( e );
            }

            settings.m_DRCSeverities[item->GetErrorCode()] =
                    FromProtoEnum<SEVERITY>( severity.severity() );
        }
    }

    {
        settings.m_DrcExclusions.clear();
        settings.m_DrcExclusionComments.clear();

        for( const DrcExclusion& exclusion : rules.exclusions() )
        {
            wxString id = wxString::FromUTF8( exclusion.marker().id().opaque_id() );

            if( id.IsEmpty() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "DrcExclusion marker id must not be empty" );
                return tl::unexpected( e );
            }

            settings.m_DrcExclusions.insert( id );
            settings.m_DrcExclusionComments[id] = wxString::FromUTF8( exclusion.comment() );
        }
    }

    board()->GetDesignSettings() = settings;
    onModified();

    BoardDesignRulesResponse response;
    response.mutable_rules()->CopyFrom( rules );
    response.set_custom_rules_status( CustomRulesStatus::CRS_NONE );
    return response;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleGetCustomDesignRules(
        const HANDLER_CONTEXT<GetCustomDesignRules>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.board() ); !validation )
        return tl::unexpected( validation.error() );

    CustomRulesResponse response;
    response.set_status( CustomRulesStatus::CRS_NONE );

    wxFileName path = context()->GetBoard()->GetFileName();
    path.SetExt( FILEEXT::DesignRulesFileExtension );
    wxString rulesPath = context()->Prj().AbsolutePath( path.GetFullName() );

    if( rulesPath.IsEmpty() || !wxFileName::IsFileReadable( rulesPath ) )
        return response;

    wxFFile file( rulesPath, "r" );

    if( !file.IsOpened() )
    {
        response.set_status( CustomRulesStatus::CRS_INVALID );
        response.set_error_text( "Failed to open custom rules file" );
        return response;
    }

    wxString content;
    file.ReadAll( &content );
    file.Close();

    std::vector<std::shared_ptr<DRC_RULE>> parsedRules;

    try
    {
        DRC_RULES_PARSER parser( content, "File" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& error )
    {
        response.set_status( CustomRulesStatus::CRS_INVALID );
        response.set_error_text( error.What().ToStdString() );
        return response;
    }

    for( const std::shared_ptr<DRC_RULE>& rule : parsedRules )
    {
        board::CustomRule* customRule = response.add_rules();
        customRule->set_name( rule->m_Name.ToUTF8() );
        customRule->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( rule->m_Severity ) );

        if( rule->m_Condition )
            customRule->set_condition( rule->m_Condition->GetExpression().ToUTF8() );

        for( const DRC_CONSTRAINT& constraint : rule->m_Constraints )
            constraint.ToProto( *customRule->add_constraints() );

        if( rule->m_LayerSource.CmpNoCase( wxS( "outer" ) ) == 0 )
            customRule->set_layer_mode( board::CRLM_OUTER );
        else if( rule->m_LayerSource.CmpNoCase( wxS( "inner" ) ) == 0 )
            customRule->set_layer_mode( board::CRLM_INNER );
        else if( !rule->m_LayerSource.IsEmpty() )
        {
            int layer = LSET::NameToLayer( rule->m_LayerSource );

            if( layer >= 0 && layer < PCB_LAYER_ID_COUNT )
                customRule->set_single_layer( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                        static_cast<PCB_LAYER_ID>( layer ) ) );
        }

        wxString original = DRC_RULE_LOADER::ExtractRuleText( content, rule->m_Name );
        wxString comment = DRC_RULE_LOADER::ExtractRuleComment( original );

        if( !comment.IsEmpty() )
            customRule->set_comments( comment.ToUTF8() );
    }

    response.set_status( CustomRulesStatus::CRS_VALID );
    return response;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleSetCustomDesignRules(
        const HANDLER_CONTEXT<SetCustomDesignRules>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.board() ); !validation )
        return tl::unexpected( validation.error() );

    wxFileName path = context()->GetBoard()->GetFileName();
    path.SetExt( FILEEXT::DesignRulesFileExtension );
    wxString rulesPath = context()->Prj().AbsolutePath( path.GetFullName() );

    if( aCtx.Request.rules_size() == 0 )
    {
        if( wxFileName::FileExists( rulesPath ) && !wxRemoveFile( rulesPath ) )
        {
            CustomRulesResponse response;
            response.set_status( CustomRulesStatus::CRS_INVALID );
            response.set_error_text( "Failed to remove custom rules file" );
            return response;
        }

        CustomRulesResponse response;
        response.set_status( CustomRulesStatus::CRS_NONE );
        return response;
    }

    wxString rulesText( "(version 1)\n" );

    for( const board::CustomRule& rule : aCtx.Request.rules() )
    {
        wxString errorText;
        wxString serialized = DRC_RULE::FormatRuleFromProto( rule, &errorText );

        if( serialized.IsEmpty() )
        {
            CustomRulesResponse response;
            response.set_status( CustomRulesStatus::CRS_INVALID );
            response.set_error_text( errorText.IsEmpty() ? "Failed to serialize custom rule"
                                                         : errorText.ToUTF8() );
            return response;
        }

        rulesText << '\n' << serialized;
    }

    try
    {
        std::vector<std::shared_ptr<DRC_RULE>> parsedRules;
        DRC_RULES_PARSER parser( rulesText, "SetCustomDesignRules" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& error )
    {
        CustomRulesResponse response;
        response.set_status( CustomRulesStatus::CRS_INVALID );
        response.set_error_text( error.What().ToStdString() );
        return response;
    }

    wxTempFile file( rulesPath );

    if( !file.IsOpened() || !file.Write( rulesText ) || !file.Commit() )
    {

        CustomRulesResponse response;
        response.set_status( CustomRulesStatus::CRS_INVALID );
        response.set_error_text( "Failed to write custom rules file" );
        return response;
    }

    HANDLER_CONTEXT<GetCustomDesignRules> getCtx = { aCtx.ClientName, GetCustomDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();
    return handleGetCustomDesignRules( getCtx );
}


HANDLER_RESULT<types::Vector2> API_HANDLER_PCB::handleGetBoardOrigin(
        const HANDLER_CONTEXT<GetBoardOrigin>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin;
    const BOARD_DESIGN_SETTINGS& settings = board()->GetDesignSettings();

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
        origin = settings.GetGridOrigin();
        break;

    case BOT_DRILL:
        origin = settings.GetAuxOrigin();
        break;

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    types::Vector2 reply;
    PackVector2( reply, origin );
    return reply;
}

HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardOrigin(
        const HANDLER_CONTEXT<SetBoardOrigin>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin = UnpackVector2( aCtx.Request.origin() );

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            f->CallAfter( [f, origin]()
                          {
                              // gridSetOrigin takes ownership and frees this
                              VECTOR2D* dorigin = new VECTOR2D( origin );
                              TOOL_MANAGER* mgr = f->GetToolManager();
                              mgr->RunAction( PCB_ACTIONS::gridSetOrigin, dorigin );
                              f->Refresh();
                          } );
        }
        else
        {
            board()->GetDesignSettings().SetGridOrigin( origin );
            onModified();
        }
        break;
    }

    case BOT_DRILL:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            f->CallAfter( [f, origin]()
                          {
                              TOOL_MANAGER* mgr = f->GetToolManager();
                              mgr->RunAction( PCB_ACTIONS::drillSetOrigin, origin );
                              f->Refresh();
                          } );
        }
        else
        {
            board()->GetDesignSettings().SetAuxOrigin( origin );
            onModified();
        }
        break;
    }

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    return Empty();
}


HANDLER_RESULT<BoardLayerNameResponse> API_HANDLER_PCB::handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerNameResponse response;

    PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    response.set_name( board()->GetLayerName( id ) );

    return response;
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_PCB::handleGetBoardLayerByName(
        const HANDLER_CONTEXT<GetBoardLayerByName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PCB_LAYER_ID layer = board()->GetLayerID( wxString::FromUTF8( aCtx.Request.name() ) );

    if( layer == UNDEFINED_LAYER )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "unknown board layer '{}'", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    BoardLayerResponse response;
    response.set_layer( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );
    return response;
}


HANDLER_RESULT<GetBoundingBoxResponse> API_HANDLER_PCB::handleGetBoundingBox(
        const HANDLER_CONTEXT<GetBoundingBox>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetBoundingBoxResponse response;
    bool includeText = aCtx.Request.mode() == BoundingBoxMode::BBM_ITEM_AND_CHILD_TEXT;

    for( const types::KIID& idMsg : aCtx.Request.items() )
    {
        KIID id( idMsg.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        BOARD_ITEM* item = *optItem;
        BOX2I bbox;

        if( item->Type() == PCB_FOOTPRINT_T )
            bbox = static_cast<FOOTPRINT*>( item )->GetBoundingBox( includeText );
        else
            bbox = item->GetBoundingBox();

        response.add_items()->set_value( idMsg.value() );
        PackBox2( *response.add_boxes(), bbox );
    }

    return response;
}


HANDLER_RESULT<PadShapeAsPolygonResponse> API_HANDLER_PCB::handleGetPadShapeAsPolygon(
        const HANDLER_CONTEXT<GetPadShapeAsPolygon>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadShapeAsPolygonResponse response;
    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( aCtx.Request.layer() );

    for( const types::KIID& padRequest : aCtx.Request.pads() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optPad = getItemById( id );

        if( !optPad || ( *optPad )->Type() != PCB_PAD_T )
            continue;

        response.add_pads()->set_value( padRequest.value() );

        PAD* pad = static_cast<PAD*>( *optPad );
        SHAPE_POLY_SET poly;
        pad->TransformShapeToPolygon( poly, pad->Padstack().EffectiveLayerFor( layer ), 0,
                                      pad->GetMaxError(), ERROR_INSIDE );

        types::PolygonWithHoles* polyMsg = response.mutable_polygons()->Add();
        PackPolyLine( *polyMsg->mutable_outline(), poly.COutline( 0 ) );
    }

    return response;
}


HANDLER_RESULT<PadstackPresenceResponse> API_HANDLER_PCB::handleCheckPadstackPresenceOnLayers(
        const HANDLER_CONTEXT<CheckPadstackPresenceOnLayers>& aCtx )
{
    using board::types::BoardLayer;

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadstackPresenceResponse response;

    LSET layers;

    for( const int layer : aCtx.Request.layers() )
        layers.set( FromProtoEnum<PCB_LAYER_ID, BoardLayer>( static_cast<BoardLayer>( layer ) ) );

    for( const types::KIID& padRequest : aCtx.Request.items() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        switch( ( *optItem )->Type() )
        {
        case PCB_PAD_T:
        {
            PAD* pad = static_cast<PAD*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( pad->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( pad->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        case PCB_VIA_T:
        {
            PCB_VIA* via = static_cast<PCB_VIA*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( via->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( via->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        default:
            break;
        }
    }

    return response;
}


HANDLER_RESULT<types::TitleBlockInfo> API_HANDLER_PCB::handleGetTitleBlockInfo(
        const HANDLER_CONTEXT<GetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = this->board();
    const TITLE_BLOCK& block = board->GetTitleBlock();

    types::TitleBlockInfo response;

    response.set_title( block.GetTitle().ToUTF8() );
    response.set_date( block.GetDate().ToUTF8() );
    response.set_revision( block.GetRevision().ToUTF8() );
    response.set_company( block.GetCompany().ToUTF8() );
    response.set_comment1( block.GetComment( 0 ).ToUTF8() );
    response.set_comment2( block.GetComment( 1 ).ToUTF8() );
    response.set_comment3( block.GetComment( 2 ).ToUTF8() );
    response.set_comment4( block.GetComment( 3 ).ToUTF8() );
    response.set_comment5( block.GetComment( 4 ).ToUTF8() );
    response.set_comment6( block.GetComment( 5 ).ToUTF8() );
    response.set_comment7( block.GetComment( 6 ).ToUTF8() );
    response.set_comment8( block.GetComment( 7 ).ToUTF8() );
    response.set_comment9( block.GetComment( 8 ).ToUTF8() );

    return response;
}


std::optional<TITLE_BLOCK*> API_HANDLER_PCB::getTitleBlock()
{
    if( !board() )
        return std::nullopt;

    return &board()->GetTitleBlock();
}


std::optional<PAGE_INFO> API_HANDLER_PCB::getPageSettings()
{
    if( !board() )
        return std::nullopt;

    return board()->GetPageSettings();
}


bool API_HANDLER_PCB::setPageSettings( const PAGE_INFO& aPageInfo )
{
    if( !board() )
        return false;

    board()->SetPageSettings( aPageInfo );
    return true;
}


void API_HANDLER_PCB::onModified()
{
    if( board() )
        board()->SetModified();

    if( frame() )
        frame()->OnModify();
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_PCB::handleExpandTextVariables(
    const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ExpandTextVariablesResponse reply;
    BOARD* board = this->board();

    std::function<bool( wxString* )> textResolver =
            [&]( wxString* token ) -> bool
            {
                // Handles m_board->GetTitleBlock() *and* m_board->GetProject()
                return board->ResolveTextVar( token, 0 );
            };

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString text = ExpandTextVars( wxString::FromUTF8( textMsg ), &textResolver );
        reply.add_text( text.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleInteractiveMoveItems(
        const HANDLER_CONTEXT<InteractiveMoveItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "InteractiveMoveItems" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* mgr = toolManager();
    std::vector<EDA_ITEM*> toSelect;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toSelect.emplace_back( static_cast<EDA_ITEM*>( *item ) );
    }

    if( toSelect.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "None of the given items exist on the board",
                                          aCtx.Request.board().board_filename() ) );
        return tl::unexpected( e );
    }

    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    selectionTool->GetSelection().SetReferencePoint( toSelect[0]->GetPosition() );

    mgr->RunAction( ACTIONS::selectionClear );
    mgr->RunAction<EDA_ITEMS*>( ACTIONS::selectItems, &toSelect );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    mgr->PostAPIAction( PCB_ACTIONS::move, commit );

    return Empty();
}


HANDLER_RESULT<NetsResponse> API_HANDLER_PCB::handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    NetsResponse response;
    BOARD* board = this->board();

    std::set<wxString> netclassFilter;

    for( const std::string& nc : aCtx.Request.netclass_filter() )
        netclassFilter.insert( wxString( nc.c_str(), wxConvUTF8 ) );

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        NETCLASS* nc = net->GetNetClass();

        if( !netclassFilter.empty() && nc )
        {
            bool inClass = false;

            for( const wxString& filter : netclassFilter )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        board::types::Net* netProto = response.add_nets();
        netProto->set_name( net->GetNetname() );
        netProto->mutable_code()->set_value( net->GetNetCode() );
    }

    return response;
}


HANDLER_RESULT<NetClassForNetsResponse> API_HANDLER_PCB::handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx )
{
    NetClassForNetsResponse response;

    BOARD* board = this->board();
    const NETINFO_LIST& nets = board->GetNetInfo();
    google::protobuf::Any any;

    for( const board::types::Net& net : aCtx.Request.net() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        netInfo->GetNetClass()->Serialize( any );
        auto [pair, rc] = response.mutable_classes()->insert( { net.name(), {} } );
        any.UnpackTo( &pair->second );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* mgr = toolManager();

    if( !mgr->FindTool( ZONE_FILLER_TOOL_NAME ) )
        mgr->RegisterTool( new ZONE_FILLER_TOOL );

    if( aCtx.Request.zones().empty() )
    {
        if( frame() )
        {
            frame()->CallAfter( [mgr]()
                                {
                                    mgr->RunAction( PCB_ACTIONS::zoneFillAll );
                                } );
        }
        else
        {
            mgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, nullptr, true );
        }
    }
    else
    {
        std::vector<ZONE*> toFill;

        for( const types::KIID& id : aCtx.Request.zones() )
        {
            std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

            if( !item || ( *item )->Type() != PCB_ZONE_T )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "zone with ID {} not found on the board", id.value() ) );
                return tl::unexpected( e );
            }

            ZONE* zone = static_cast<ZONE*>( *item );

            if( zone->GetIsRuleArea() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "zone with ID {} is a rule area and cannot be filled",
                                                  id.value() ) );
                return tl::unexpected( e );
            }

            if( std::find( toFill.begin(), toFill.end(), zone ) == toFill.end() )
                toFill.push_back( zone );
        }

        std::unique_ptr<COMMIT> commit = createCommit();
        ZONE_FILLER filler( board(), commit.get() );

        if( !filler.Fill( toFill ) )
        {
            commit->Revert();

            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNKNOWN );
            e.set_error_message( "zone fill failed" );
            return tl::unexpected( e );
        }

        commit->Push( _( "Fill Zone(s)" ), SKIP_CONNECTIVITY | ZONE_FILL_OP );
        board()->BuildConnectivity();

        if( frame() )
        {
            frame()->GetCanvas()->RedrawRatsnest();
            frame()->GetCanvas()->Refresh();
        }
    }

    return Empty();
}


HANDLER_RESULT<SavedDocumentResponse> API_HANDLER_PCB::handleSaveDocumentToString(
        const HANDLER_CONTEXT<SaveDocumentToString>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SavedDocumentResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SaveBoard( wxEmptyString, board(), nullptr );

    return response;
}


HANDLER_RESULT<SavedSelectionResponse> API_HANDLER_PCB::handleSaveSelectionToString(
        const HANDLER_CONTEXT<SaveSelectionToString>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SaveSelectionToString" ) )
        return tl::unexpected( *headless );

    SavedSelectionResponse response;

    TOOL_MANAGER* mgr = toolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    PCB_SELECTION& selection = selectionTool->GetSelection();

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SetBoard( board() );
    io.SaveSelection( selection, false );

    return response;
}


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_PCB::handleParseAndCreateItemsFromString(
        const HANDLER_CONTEXT<ParseAndCreateItemsFromString>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CreateItemsResponse response;
    return response;
}


HANDLER_RESULT<BoardLayers> API_HANDLER_PCB::handleGetVisibleLayers(
        const HANDLER_CONTEXT<GetVisibleLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardLayers response;

    for( PCB_LAYER_ID layer : board()->GetVisibleLayers() )
        response.add_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetVisibleLayers(
        const HANDLER_CONTEXT<SetVisibleLayers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    LSET visible;
    LSET enabled = board()->GetEnabledLayers();

    for( int layerIdx : aCtx.Request.layers() )
    {
        PCB_LAYER_ID layer =
                FromProtoEnum<PCB_LAYER_ID>( static_cast<board::types::BoardLayer>( layerIdx ) );

        if( enabled.Contains( layer ) )
            visible.set( layer );
    }

    board()->SetVisibleLayers( visible );

    if( frame() )
    {
        frame()->GetAppearancePanel()->OnBoardChanged();
        frame()->GetCanvas()->SyncLayersVisibility( board() );
        frame()->Refresh();
    }

    onModified();
    return Empty();
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_PCB::handleGetActiveLayer(
        const HANDLER_CONTEXT<GetActiveLayer>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardLayerResponse response;
    response.set_layer(
            ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                    frame() ? frame()->GetActiveLayer() : board()->GetActiveLayer() ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetActiveLayer(
        const HANDLER_CONTEXT<SetActiveLayer>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    if( !board()->GetEnabledLayers().Contains( layer ) )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_BAD_REQUEST );
        err.set_error_message( fmt::format( "Layer {} is not a valid layer for the given board",
                                            magic_enum::enum_name( layer ) ) );
        return tl::unexpected( err );
    }

    if( frame() )
        frame()->SetActiveLayer( layer );
    else
        board()->SetActiveLayer( layer );

    onModified();
    return Empty();
}


HANDLER_RESULT<ImportNetlistResponse> API_HANDLER_PCB::handleImportNetlist(
        const HANDLER_CONTEXT<ImportNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    wxFileName netlistPath( project().AbsolutePath(
            wxString::FromUTF8( aCtx.Request.netlist_path() ) ) );

    if( !netlistPath.IsOk() || !netlistPath.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "netlist file '{}' could not be opened",
                                          netlistPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    WX_STRING_REPORTER reporter;
    const bool lookupByTimestamp = aCtx.Request.match_mode() != NetlistMatchMode::NMM_REFERENCE;
    NETLIST netlist;
    netlist.SetFindByTimeStamp( lookupByTimestamp );
    netlist.SetReplaceFootprints( aCtx.Request.update_footprints() );

    bool loaded = false;

    if( frame() )
    {
        loaded = frame()->ReadNetlistFromFile( netlistPath.GetFullPath(), netlist, reporter );
    }
    else
    {
        try
        {
            std::unique_ptr<NETLIST_READER> reader( NETLIST_READER::GetNetlistReader(
                    &netlist, netlistPath.GetFullPath(), wxEmptyString ) );

            if( reader )
            {
                reader->LoadNetlist();
                LoadNetlistFootprints( board(), netlist, reporter );
                loaded = true;
            }
            else
            {
                reporter.Report( wxString::Format( _( "Cannot open netlist file '%s'." ),
                                                   netlistPath.GetFullPath() ),
                                 RPT_SEVERITY_ERROR );
            }
        }
        catch( const IO_ERROR& ioe )
        {
            reporter.Report( wxString::Format( _( "Error loading netlist.\n%s" ),
                                               ioe.What() ),
                             RPT_SEVERITY_ERROR );
        }
    }

    if( !loaded )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "unable to handle netlist file '{}': {}",
                                          netlistPath.GetFullPath().ToStdString(),
                                          reporter.GetMessages().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_NETLIST_UPDATER> updater;

    if( frame() )
        updater = std::make_unique<BOARD_NETLIST_UPDATER>( frame(), board() );
    else
        updater = std::make_unique<BOARD_NETLIST_UPDATER>( toolManager(), board() );

    updater->SetReporter( &reporter );
    updater->SetIsDryRun( aCtx.Request.dry_run() );
    updater->SetLookupByTimestamp( lookupByTimestamp );
    updater->SetDeleteUnusedFootprints( aCtx.Request.delete_extra_footprints() );
    updater->SetReplaceFootprints( aCtx.Request.update_footprints() );
    updater->SetTransferGroups( aCtx.Request.transfer_groups() );
    updater->SetOverrideLocks( aCtx.Request.override_locks() );
    updater->SetUpdateFields( true );

    const bool success = updater->UpdateNetlist( netlist );

    if( !aCtx.Request.dry_run() && success && frame() )
    {
        bool runDragCommand = false;
        frame()->OnNetlistChanged( *updater, &runDragCommand );
    }

    ImportNetlistResponse response;
    response.set_report( reporter.GetMessages().ToUTF8() );
    response.set_error_count( updater->GetErrorCount() );
    response.set_warning_count( updater->GetWarningCount() );
    response.set_new_footprint_count( updater->GetNewFootprintCount() );
    return response;
}


HANDLER_RESULT<BoardEditorAppearanceSettings> API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    BoardEditorAppearanceSettings reply;

    // TODO: might be nice to put all these things in one place and have it derive SERIALIZABLE

    const PCB_DISPLAY_OPTIONS& displayOptions = frame()->GetDisplayOptions();

    reply.set_inactive_layer_display( ToProtoEnum<HIGH_CONTRAST_MODE, InactiveLayerDisplayMode>(
            displayOptions.m_ContrastModeDisplay ) );
    reply.set_net_color_display(
            ToProtoEnum<NET_COLOR_MODE, NetColorDisplayMode>( displayOptions.m_NetColorMode ) );

    reply.set_board_flip( frame()->GetCanvas()->GetView()->IsMirroredX()
                                  ? BoardFlipMode::BFM_FLIPPED_X
                                  : BoardFlipMode::BFM_NORMAL );

    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();

    reply.set_ratsnest_display( ToProtoEnum<RATSNEST_MODE, RatsnestDisplayMode>(
            editorSettings->m_Display.m_RatsnestMode ) );

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    PCB_DISPLAY_OPTIONS options = frame()->GetDisplayOptions();
    KIGFX::PCB_VIEW* view = frame()->GetCanvas()->GetView();
    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();
    const BoardEditorAppearanceSettings& newSettings = aCtx.Request.settings();

    options.m_ContrastModeDisplay =
            FromProtoEnum<HIGH_CONTRAST_MODE>( newSettings.inactive_layer_display() );
    options.m_NetColorMode =
            FromProtoEnum<NET_COLOR_MODE>( newSettings.net_color_display() );

    bool flip = newSettings.board_flip() == BoardFlipMode::BFM_FLIPPED_X;

    if( flip != view->IsMirroredX() )
    {
        view->SetMirror( !view->IsMirroredX(), view->IsMirroredY() );
        view->RecacheAllItems();
    }

    editorSettings->m_Display.m_RatsnestMode =
            FromProtoEnum<RATSNEST_MODE>( newSettings.ratsnest_display() );

    frame()->SetDisplayOptions( options );
    frame()->GetCanvas()->GetView()->UpdateAllLayersColor();
    frame()->GetCanvas()->Refresh();

    return Empty();
}


HANDLER_RESULT<BoardPlotSettingsResponse> API_HANDLER_PCB::handleGetBoardPlotSettings(
        const HANDLER_CONTEXT<GetBoardPlotSettings>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    const PCB_PLOT_PARAMS& plotOpts = board()->GetPlotOptions();
    BoardPlotSettingsResponse response;
    BoardPlotSettings* settings = response.mutable_plot_settings();

    board::PackLayerSet( *settings->mutable_layers(), plotOpts.GetLayerSelection() );

    for( PCB_LAYER_ID layer : plotOpts.GetPlotOnAllLayersSequence() )
        settings->add_common_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    settings->set_mirror( plotOpts.GetMirror() );
    settings->set_black_and_white( plotOpts.GetBlackAndWhite() );
    settings->set_negative( plotOpts.GetNegative() );
    settings->set_scale( plotOpts.GetScale() );
    settings->set_sketch_pads_on_fab_layers( plotOpts.GetSketchPadsOnFabLayers() );
    settings->set_hide_dnp_footprints_on_fab_layers( plotOpts.GetHideDNPFPsOnFabLayers() );
    settings->set_sketch_dnp_footprints_on_fab_layers( plotOpts.GetSketchDNPFPsOnFabLayers() );
    settings->set_crossout_dnp_footprints_on_fab_layers( plotOpts.GetCrossoutDNPFPsOnFabLayers() );
    settings->set_plot_footprint_values( plotOpts.GetPlotValue() );
    settings->set_plot_reference_designators( plotOpts.GetPlotReference() );
    settings->set_plot_drawing_sheet( plotOpts.GetPlotFrameRef() );
    settings->set_subtract_solder_mask_from_silk( plotOpts.GetSubtractMaskFromSilk() );
    settings->set_plot_pad_numbers( plotOpts.GetPlotPadNumbers() );
    settings->set_drill_marks( ToProtoEnum<DRILL_MARKS, PlotDrillMarks>( plotOpts.GetDrillMarksType() ) );
    settings->set_use_drill_origin( plotOpts.GetUseAuxOrigin() );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardPlotSettings(
        const HANDLER_CONTEXT<SetBoardPlotSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    const BoardPlotSettings& settings = aCtx.Request.plot_settings();
    PCB_PLOT_PARAMS plotOpts = board()->GetPlotOptions();
    plotOpts.SetLayerSelection( board::UnpackLayerSet( settings.layers() ) );

    LSEQ commonLayers;

    for( int layer : settings.common_layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( !IsPcbLayer( layerId ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "SetBoardPlotSettings contains an invalid layer {}",
                                              magic_enum::enum_name( layerId ) ) );
            return tl::unexpected( e );
        }

        commonLayers.push_back( layerId );
    }

    plotOpts.SetPlotOnAllLayersSequence( commonLayers );
    plotOpts.SetMirror( settings.mirror() );
    plotOpts.SetBlackAndWhite( settings.black_and_white() );
    plotOpts.SetNegative( settings.negative() );
    plotOpts.SetScale( settings.scale() );
    plotOpts.SetSketchPadsOnFabLayers( settings.sketch_pads_on_fab_layers() );
    plotOpts.SetHideDNPFPsOnFabLayers( settings.hide_dnp_footprints_on_fab_layers() );
    plotOpts.SetSketchDNPFPsOnFabLayers( settings.sketch_dnp_footprints_on_fab_layers() );
    plotOpts.SetCrossoutDNPFPsOnFabLayers( settings.crossout_dnp_footprints_on_fab_layers() );
    plotOpts.SetPlotValue( settings.plot_footprint_values() );
    plotOpts.SetPlotReference( settings.plot_reference_designators() );
    plotOpts.SetPlotFrameRef( settings.plot_drawing_sheet() );
    plotOpts.SetSubtractMaskFromSilk( settings.subtract_solder_mask_from_silk() );
    plotOpts.SetPlotPadNumbers( settings.plot_pad_numbers() );
    plotOpts.SetDrillMarksType( FromProtoEnum<DRILL_MARKS>( settings.drill_marks() ) );
    plotOpts.SetUseAuxOrigin( settings.use_drill_origin() );

    board()->SetPlotOptions( plotOpts );
    onModified();
    return Empty();
}


HANDLER_RESULT<InjectDrcErrorResponse> API_HANDLER_PCB::handleInjectDrcError(
        const HANDLER_CONTEXT<InjectDrcError>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SEVERITY severity = FromProtoEnum<SEVERITY>( aCtx.Request.severity() );
    int      layer = severity == RPT_SEVERITY_WARNING ? LAYER_DRC_WARNING : LAYER_DRC_ERROR;
    int      code = severity == RPT_SEVERITY_WARNING ? DRCE_GENERIC_WARNING : DRCE_GENERIC_ERROR;

    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( code );

    drcItem->SetErrorMessage( wxString::FromUTF8( aCtx.Request.message() ) );

    RC_ITEM::KIIDS ids;

    for( const auto& id : aCtx.Request.items() )
        ids.emplace_back( KIID( id.value() ) );

    if( !ids.empty() )
        drcItem->SetItems( ids );

    const auto& pos = aCtx.Request.position();
    VECTOR2I    position( static_cast<int>( pos.x_nm() ), static_cast<int>( pos.y_nm() ) );

    PCB_MARKER* marker = new PCB_MARKER( drcItem, position, layer );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    commit->Add( marker );
    commit->Push( wxS( "API injected DRC marker" ) );

    InjectDrcErrorResponse response;
    response.mutable_marker()->set_value( marker->GetUUID().AsStdString() );

    return response;
}


std::optional<ApiResponseStatus> ValidateUnitsInchMm( types::Units aUnits,
                                                      const std::string& aCommandName )
{
    if( aUnits == types::Units::U_INCH || aUnits == types::Units::U_MM
        || aUnits == types::Units::U_UNKNOWN )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} supports only inch and mm units", aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus>
ValidatePaginationModeForSingleOrPerFile( kiapi::board::jobs::BoardJobPaginationMode aMode,
                                          const std::string& aCommandName )
{
    if( aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_UNKNOWN
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} does not support EACH_LAYER_OWN_PAGE pagination mode",
                                      aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus> ApplyBoardPlotSettings( const BoardPlotSettings& aSettings,
                                                         JOB_EXPORT_PCB_PLOT& aJob )
{
    for( int layer : aSettings.layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( layerId == PCB_LAYER_ID::UNDEFINED_LAYER )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid layer" );
            return e;
        }

        aJob.m_plotLayerSequence.push_back( layerId );
    }

    for( int layer : aSettings.common_layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( layerId == PCB_LAYER_ID::UNDEFINED_LAYER )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid common layer" );
            return e;
        }

        aJob.m_plotOnAllLayersSequence.push_back( layerId );
    }

    aJob.m_colorTheme = wxString::FromUTF8( aSettings.color_theme() );
    aJob.m_drawingSheet = wxString::FromUTF8( aSettings.drawing_sheet() );
    aJob.m_variant = wxString::FromUTF8( aSettings.variant() );

    aJob.m_mirror = aSettings.mirror();
    aJob.m_blackAndWhite = aSettings.black_and_white();
    aJob.m_negative = aSettings.negative();
    aJob.m_scale = aSettings.scale();

    aJob.m_sketchPadsOnFabLayers = aSettings.sketch_pads_on_fab_layers();
    aJob.m_hideDNPFPsOnFabLayers = aSettings.hide_dnp_footprints_on_fab_layers();
    aJob.m_sketchDNPFPsOnFabLayers = aSettings.sketch_dnp_footprints_on_fab_layers();
    aJob.m_crossoutDNPFPsOnFabLayers = aSettings.crossout_dnp_footprints_on_fab_layers();

    aJob.m_plotFootprintValues = aSettings.plot_footprint_values();
    aJob.m_plotRefDes = aSettings.plot_reference_designators();
    aJob.m_plotDrawingSheet = aSettings.plot_drawing_sheet();
    aJob.m_subtractSolderMaskFromSilk = aSettings.subtract_solder_mask_from_silk();
    aJob.m_plotPadNumbers = aSettings.plot_pad_numbers();

    aJob.m_drillShapeOption = FromProtoEnum<DRILL_MARKS>( aSettings.drill_marks() );

    aJob.m_useDrillOrigin = aSettings.use_drill_origin();
    aJob.m_checkZonesBeforePlot = aSettings.check_zones_before_plot();

    return std::nullopt;
}


HANDLER_RESULT<types::RunJobResponse> ExecuteBoardJob( BOARD_CONTEXT* aContext, JOB& aJob )
{
    types::RunJobResponse response;
    WX_STRING_REPORTER reporter;

    if( !aContext || !aContext->GetKiway() )
    {
        response.set_status( types::JobStatus::JS_ERROR );
        response.set_message( "Internal error" );
        return response;
        wxCHECK_MSG( false, response, "context missing valid kiway in ExecuteBoardJob?" );
    }

    int exitCode = aContext->GetKiway()->ProcessJob( KIWAY::FACE_PCB, &aJob, &reporter );

    for( const JOB_OUTPUT& output : aJob.GetOutputs() )
        response.add_output_path( output.m_outputPath.ToUTF8() );

    if( exitCode == 0 )
    {
        response.set_status( types::JobStatus::JS_SUCCESS );
        return response;
    }

    response.set_status( types::JobStatus::JS_ERROR );
    response.set_message( fmt::format( "Board export job '{}' failed with exit code {}: {}",
                                       aJob.GetType(), exitCode,
                                       reporter.GetMessages().ToStdString() ) );
    return response;
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExport3D(
        const HANDLER_CONTEXT<RunBoardJobExport3D>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_3D job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_3D::FORMAT>( aCtx.Request.format() );

    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    job.m_3dparams.m_NetFilter = wxString::FromUTF8( aCtx.Request.net_filter() );
    job.m_3dparams.m_ComponentFilter = wxString::FromUTF8( aCtx.Request.component_filter() );

    job.m_hasUserOrigin = aCtx.Request.has_user_origin();
    job.m_3dparams.m_Origin = VECTOR2D( aCtx.Request.origin().x_nm(), aCtx.Request.origin().y_nm() );

    job.m_3dparams.m_Overwrite = aCtx.Request.overwrite();
    job.m_3dparams.m_UseGridOrigin = aCtx.Request.use_grid_origin();
    job.m_3dparams.m_UseDrillOrigin = aCtx.Request.use_drill_origin();
    job.m_3dparams.m_UseDefinedOrigin = aCtx.Request.use_defined_origin() || aCtx.Request.has_user_origin();
    job.m_3dparams.m_UsePcbCenterOrigin = aCtx.Request.use_pcb_center_origin();

    job.m_3dparams.m_IncludeUnspecified = aCtx.Request.include_unspecified();
    job.m_3dparams.m_IncludeDNP = aCtx.Request.include_dnp();
    job.m_3dparams.m_SubstModels = aCtx.Request.substitute_models();

    job.m_3dparams.m_BoardOutlinesChainingEpsilon = aCtx.Request.board_outlines_chaining_epsilon();
    job.m_3dparams.m_BoardOnly = aCtx.Request.board_only();
    job.m_3dparams.m_CutViasInBody = aCtx.Request.cut_vias_in_body();
    job.m_3dparams.m_ExportBoardBody = aCtx.Request.export_board_body();
    job.m_3dparams.m_ExportComponents = aCtx.Request.export_components();
    job.m_3dparams.m_ExportTracksVias = aCtx.Request.export_tracks_and_vias();
    job.m_3dparams.m_ExportPads = aCtx.Request.export_pads();
    job.m_3dparams.m_ExportZones = aCtx.Request.export_zones();
    job.m_3dparams.m_ExportInnerCopper = aCtx.Request.export_inner_copper();
    job.m_3dparams.m_ExportSilkscreen = aCtx.Request.export_silkscreen();
    job.m_3dparams.m_ExportSoldermask = aCtx.Request.export_soldermask();
    job.m_3dparams.m_FuseShapes = aCtx.Request.fuse_shapes();
    job.m_3dparams.m_FillAllVias = aCtx.Request.fill_all_vias();
    job.m_3dparams.m_OptimizeStep = aCtx.Request.optimize_step();
    job.m_3dparams.m_ExtraPadThickness = aCtx.Request.extra_pad_thickness();

    job.m_vrmlUnits = FromProtoEnum<JOB_EXPORT_PCB_3D::VRML_UNITS>( aCtx.Request.vrml_units() );

    job.m_vrmlModelDir = wxString::FromUTF8( aCtx.Request.vrml_model_dir() );
    job.m_vrmlRelativePaths = aCtx.Request.vrml_relative_paths();

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportRender(
        const HANDLER_CONTEXT<RunBoardJobExportRender>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_PCB_RENDER job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_PCB_RENDER::FORMAT>( aCtx.Request.format() );
    job.m_quality = FromProtoEnum<JOB_PCB_RENDER::QUALITY>( aCtx.Request.quality() );
    job.m_bgStyle = FromProtoEnum<JOB_PCB_RENDER::BG_STYLE>( aCtx.Request.background_style() );

    job.m_width = aCtx.Request.width();
    job.m_height = aCtx.Request.height();
    job.m_appearancePreset = aCtx.Request.appearance_preset();
    job.m_useBoardStackupColors = aCtx.Request.use_board_stackup_colors();

    job.m_side = FromProtoEnum<JOB_PCB_RENDER::SIDE>( aCtx.Request.side() );

    job.m_zoom = aCtx.Request.zoom();
    job.m_perspective = aCtx.Request.perspective();

    job.m_rotation = UnpackVector3D( aCtx.Request.rotation() );
    job.m_pan = UnpackVector3D( aCtx.Request.pan() );
    job.m_pivot = UnpackVector3D( aCtx.Request.pivot() );

    job.m_proceduralTextures = aCtx.Request.procedural_textures();
    job.m_floor = aCtx.Request.floor();
    job.m_antiAlias = aCtx.Request.anti_alias();
    job.m_postProcess = aCtx.Request.post_process();

    job.m_lightTopIntensity = UnpackVector3D( aCtx.Request.light_top_intensity() );
    job.m_lightBottomIntensity = UnpackVector3D( aCtx.Request.light_bottom_intensity() );
    job.m_lightCameraIntensity = UnpackVector3D( aCtx.Request.light_camera_intensity() );
    job.m_lightSideIntensity = UnpackVector3D( aCtx.Request.light_side_intensity() );
    job.m_lightSideElevation = aCtx.Request.light_side_elevation();

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportSvg(
        const HANDLER_CONTEXT<RunBoardJobExportSvg>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_SVG job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_fitPageToBoard = aCtx.Request.fit_page_to_board();
    job.m_precision = aCtx.Request.precision();

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportSvg" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_SVG::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDxf(
        const HANDLER_CONTEXT<RunBoardJobExportDxf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_DXF job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_plotGraphicItemsUsingContours = aCtx.Request.plot_graphic_items_using_contours();
    job.m_polygonMode = aCtx.Request.polygon_mode();

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_dxfUnits = FromProtoEnum<JOB_EXPORT_PCB_DXF::DXF_UNITS>( aCtx.Request.units() );

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_DXF::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPdf(
        const HANDLER_CONTEXT<RunBoardJobExportPdf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_PDF job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_pdfFrontFPPropertyPopups = aCtx.Request.front_footprint_property_popups();
    job.m_pdfBackFPPropertyPopups = aCtx.Request.back_footprint_property_popups();
    job.m_pdfMetadata = aCtx.Request.include_metadata();
    job.m_pdfSingle = aCtx.Request.single_document();
    job.m_pdfBackgroundColor = wxString::FromUTF8( aCtx.Request.background_color() );

    job.m_pdfGenMode = FromProtoEnum<JOB_EXPORT_PCB_PDF::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPs(
        const HANDLER_CONTEXT<RunBoardJobExportPs>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_PS job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    if( std::optional<ApiResponseStatus> paginationError =
        ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                  "RunBoardJobExportPs" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_PS::GEN_MODE>( aCtx.Request.page_mode() );

    job.m_trackWidthCorrection = aCtx.Request.track_width_correction();
    job.m_XScaleAdjust = aCtx.Request.x_scale_adjust();
    job.m_YScaleAdjust = aCtx.Request.y_scale_adjust();
    job.m_forceA4 = aCtx.Request.force_a4();
    job.m_useGlobalSettings = aCtx.Request.use_global_settings();

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGerbers(
        const HANDLER_CONTEXT<RunBoardJobExportGerbers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.layers().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "RunBoardJobExportGerbers requires at least one layer" );
        return tl::unexpected( e );
    }

    JOB_EXPORT_PCB_GERBERS job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    for( int layer : aCtx.Request.layers() )
    {
        PCB_LAYER_ID layerId =
                FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                        static_cast<board::types::BoardLayer>( layer ) );

        if( layerId == PCB_LAYER_ID::UNDEFINED_LAYER )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "RunBoardJobExportGerbers contains an invalid layer" );
            return tl::unexpected( e );
        }

        job.m_plotLayerSequence.push_back( layerId );
    }

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDrill(
        const HANDLER_CONTEXT<RunBoardJobExportDrill>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_DRILL job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_FORMAT>( aCtx.Request.format() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPosition(
        const HANDLER_CONTEXT<RunBoardJobExportPosition>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_POS job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_useDrillPlaceFileOrigin = aCtx.Request.use_drill_place_file_origin();
    job.m_smdOnly = aCtx.Request.smd_only();
    job.m_excludeFootprintsWithTh = aCtx.Request.exclude_footprints_with_th();
    job.m_excludeDNP = aCtx.Request.exclude_dnp();
    job.m_excludeBOM = aCtx.Request.exclude_from_bom();
    job.m_negateBottomX = aCtx.Request.negate_bottom_x();
    job.m_singleFile = aCtx.Request.single_file();
    job.m_nakedFilename = aCtx.Request.naked_filename();
    job.m_gerberBoardEdge = aCtx.Request.include_board_edge_for_gerber();
    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );

    job.m_side = FromProtoEnum<JOB_EXPORT_PCB_POS::SIDE>( aCtx.Request.side() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportPosition" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_POS::UNITS>( aCtx.Request.units() );
    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_POS::FORMAT>( aCtx.Request.format() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGencad(
        const HANDLER_CONTEXT<RunBoardJobExportGencad>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_GENCAD job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_flipBottomPads = aCtx.Request.flip_bottom_pads();
    job.m_useIndividualShapes = aCtx.Request.use_individual_shapes();
    job.m_storeOriginCoords = aCtx.Request.store_origin_coords();
    job.m_useDrillOrigin = aCtx.Request.use_drill_origin();
    job.m_useUniquePins = aCtx.Request.use_unique_pins();

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpc2581(
        const HANDLER_CONTEXT<RunBoardJobExportIpc2581>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_IPC2581 job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_drawingSheet = wxString::FromUTF8( aCtx.Request.drawing_sheet() );
    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    job.m_precision = aCtx.Request.precision();
    job.m_compress = aCtx.Request.compress();
    job.m_colInternalId = wxString::FromUTF8( aCtx.Request.internal_id_column() );
    job.m_colMfgPn = wxString::FromUTF8( aCtx.Request.manufacturer_part_number_column() );
    job.m_colMfg = wxString::FromUTF8( aCtx.Request.manufacturer_column() );
    job.m_colDistPn = wxString::FromUTF8( aCtx.Request.distributor_part_number_column() );
    job.m_colDist = wxString::FromUTF8( aCtx.Request.distributor_column() );
    job.m_bomRev = wxString::FromUTF8( aCtx.Request.bom_revision() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportIpc2581" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS>( aCtx.Request.units() );
    job.m_version = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION>( aCtx.Request.version() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpcD356(
        const HANDLER_CONTEXT<RunBoardJobExportIpcD356>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_IPCD356 job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportODB(
        const HANDLER_CONTEXT<RunBoardJobExportODB>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_ODB job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_drawingSheet = wxString::FromUTF8( aCtx.Request.drawing_sheet() );
    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    job.m_precision = aCtx.Request.precision();

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportODB" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_ODB::ODB_UNITS>( aCtx.Request.units() );
    job.m_compressionMode = FromProtoEnum<JOB_EXPORT_PCB_ODB::ODB_COMPRESSION>( aCtx.Request.compression() );

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportStats(
        const HANDLER_CONTEXT<RunBoardJobExportStats>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    JOB_EXPORT_PCB_STATS job;
    job.m_filename = context()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT>( aCtx.Request.format() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportStats" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_STATS::UNITS>( aCtx.Request.units() );

    job.m_excludeFootprintsWithoutPads = aCtx.Request.exclude_footprints_without_pads();
    job.m_subtractHolesFromBoardArea = aCtx.Request.subtract_holes_from_board_area();
    job.m_subtractHolesFromCopperAreas = aCtx.Request.subtract_holes_from_copper_areas();

    return ExecuteBoardJob( context(), job );
}


HANDLER_RESULT<CrossProbeAnnounceResponse> API_HANDLER_PCB::handleCrossProbeAnnounce(
        const HANDLER_CONTEXT<CrossProbeAnnounce>& aCtx )
{
    CROSS_PROBE_CLIENT::RegisterPeer( static_cast<FRAME_T>( aCtx.Request.frame_type() ),
                                      aCtx.Request.socket_path() );

    CrossProbeAnnounceResponse response;
    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<SyncSelectionResponse> API_HANDLER_PCB::handleSyncSelection(
        const HANDLER_CONTEXT<SyncSelection>& aCtx )
{
    std::vector<BOARD_ITEM*> items = resolveSyncSelection( board(), aCtx.Request.items() );

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
        {
            selectionTool->ClearSelection( true );

            std::vector<EDA_ITEM*> asEdaItems( items.begin(), items.end() );
            selectionTool->AddItemsToSel( &asEdaItems );
        }

        frame()->Refresh();
    }
    else
    {
        clearSelectedBoardItems( board() );

        for( BOARD_ITEM* item : items )
            item->SetSelected();
    }

    SyncSelectionResponse response;
    response.set_status( items.empty() && aCtx.Request.items_size() > 0 ? CPS_NOT_FOUND : CPS_OK );
    return response;
}


HANDLER_RESULT<HighlightNetsResponse> API_HANDLER_PCB::handleHighlightNets(
        const HANDLER_CONTEXT<HighlightNets>& aCtx )
{
    HighlightNetsResponse response;

    if( !frame() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "PCB net highlighting requires a GUI document" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.net_name().empty() )
    {
        response.set_status( CPS_INVALID );
        response.set_message( "HighlightNets requires at least one net name" );
        return response;
    }

    std::string names;

    for( const std::string& name : aCtx.Request.net_name() )
    {
        if( !names.empty() )
            names += ",";

        names += name;
    }

    // This is a receiver command.  Apply it to this PCB frame rather than
    // sending a reverse-direction Kiway mail to the schematic editor.
    std::string payload = "$NETS: \"" + names + "\"";
    frame()->ExecuteRemoteCommand( payload.c_str() );
    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<FocusOnItemResponse> API_HANDLER_PCB::handleFocusOnItem(
        const HANDLER_CONTEXT<FocusOnItem>& aCtx )
{
    SyncSelection request;
    request.set_mode( SSM_ITEMS_ONLY );
    request.mutable_items()->Add()->mutable_footprint()->CopyFrom( aCtx.Request.focus_item().footprint() );

    if( aCtx.Request.focus_item().has_pad() )
    {
        request.mutable_items()->Clear();
        request.mutable_items()->Add()->mutable_pad()->CopyFrom( aCtx.Request.focus_item().pad() );
    }

    std::vector<BOARD_ITEM*> items = resolveSyncSelection( board(), request.items() );
    FocusOnItemResponse response;

    if( items.empty() )
    {
        response.set_status( CPS_NOT_FOUND );
        return response;
    }

    if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
        {
            selectionTool->ClearSelection( true );
            std::vector<EDA_ITEM*> selected( items.begin(), items.end() );
            selectionTool->AddItemsToSel( &selected );
        }

        frame()->FocusOnLocation( items.front()->GetBoundingBox().Centre() );
        frame()->Refresh();
    }
    else
    {
        clearSelectedBoardItems( board() );
        items.front()->SetSelected();
    }

    response.set_status( CPS_OK );
    return response;
}
