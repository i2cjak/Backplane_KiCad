/* PCB assembly IPC handlers.
 *
 * Assembly manifests are deliberately kept outside the native PCB file.  The
 * path supplied by the client is therefore also the base used to resolve and
 * re-emit relative instance sources.
 */
#include <api/api_handler_pcb.h>
#include <board.h>
#include <pcb_edit_frame.h>

#include <assembly/assembly_exporter.h>
#include <assembly/dialog_assembly.h>
#include <assembly/assembly_model.h>
#include <assembly/assembly_board_ops.h>
#include <exporters/step/exporter_step.h>
#include <jobs/job_export_pcb_3d.h>
#include <pcb_io/pcb_io_mgr.h>
#include <reporter.h>

#include <filesystem>
#include <algorithm>
#include <stdexcept>

using namespace kiapi::backplane;
using kiapi::common::types::CommandStatus;

namespace
{
std::filesystem::path manifestPath( const std::string& aPath )
{
    if( aPath.empty() )
        return {};

    std::filesystem::path path = aPath;
    if( path.has_parent_path() )
        return std::filesystem::absolute( path );

    return std::filesystem::absolute( std::filesystem::current_path() / path );
}

ASSEMBLY_MANIFEST loadManifest( const std::string& aPath, const std::string& aJson )
{
    if( !aJson.empty() )
    {
        ASSEMBLY_MANIFEST result = ASSEMBLY_MANIFEST::FromJson( nlohmann::json::parse( aJson ) );
        if( !aPath.empty() )
            result.base_directory = manifestPath( aPath ).parent_path();
        return result;
    }

    if( aPath.empty() )
        throw std::invalid_argument( "assembly path or manifest_json is required" );

    return ASSEMBLY_MANIFEST::Load( manifestPath( aPath ) );
}

void setFailure( AssemblyResponse& aResponse, const std::exception& aError )
{
    aResponse.set_status( CommandStatus::CS_FAILED );
    aResponse.set_error_message( aError.what() );
}

void setFailure( AssemblyResponse& aResponse, const std::string& aError )
{
    aResponse.set_status( CommandStatus::CS_FAILED );
    aResponse.set_error_message( aError );
}

ASSEMBLY_BOARD_EXPORTER boardExporter( const ASSEMBLY_MANIFEST& aManifest )
{
    return [&aManifest]( const ASSEMBLY_INSTANCE& aInstance, const std::string& aOutput,
                         REPORTER* aReporter )
    {
        std::filesystem::path source = aManifest.base_directory / aInstance.source;
        if( !std::filesystem::exists( source ) )
            source = aInstance.source;

        std::unique_ptr<BOARD> sourceBoard = PCB_IO_MGR::Load(
                PCB_IO_MGR::FindPluginTypeFromBoardPath( wxString::FromUTF8( source.string() ) ),
                wxString::FromUTF8( source.string() ) );
        if( !sourceBoard )
            return false;

        EXPORTER_STEP_PARAMS params;
        // TODO(assembly): pass the instance's folded state once the rigid-flex
        // STEP/exporter hook lands. Assembly export is unfolded on this base.
        EXPORTER_STEP exporter( sourceBoard.get(), params, aReporter );
        exporter.m_outputFile = wxString::FromUTF8( aOutput );
        const bool result = exporter.Export();
        return result;
    };
}
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleGetAssembly(
        const HANDLER_CONTEXT<GetAssembly>& aCtx )
{
    AssemblyResponse response;
    try
    {
        const auto path = manifestPath( aCtx.Request.path() );
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const auto errors = manifest.Validate();
        if( !errors.empty() )
            throw std::invalid_argument( errors.front() );
        response.set_status( CommandStatus::CS_OK );
        response.set_path( path.empty() ? aCtx.Request.path() : path.string() );
        response.set_manifest_json( manifest.ToJson().dump( 2 ) );
    }
    catch( const std::exception& e )
    {
        setFailure( response, e );
    }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleSaveAssembly(
        const HANDLER_CONTEXT<SaveAssembly>& aCtx )
{
    AssemblyResponse response;
    try
    {
        if( aCtx.Request.path().empty() )
            throw std::invalid_argument( "assembly path is required for save" );
        const auto path = manifestPath( aCtx.Request.path() );
        if( path.has_parent_path() && !std::filesystem::exists( path.parent_path() ) )
            throw std::invalid_argument( "assembly destination directory does not exist" );
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        manifest.base_directory = path.parent_path();
        const auto errors = manifest.Validate();
        if( !errors.empty() )
            throw std::invalid_argument( errors.front() );
        manifest.Save( path );
        response.set_status( CommandStatus::CS_OK );
        response.set_path( path.string() );
        response.set_manifest_json( manifest.ToJson().dump( 2 ) );
    }
    catch( const std::exception& e )
    {
        setFailure( response, e );
    }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleResolveAssemblyMates(
        const HANDLER_CONTEXT<ResolveAssemblyMates>& aCtx )
{
    AssemblyResponse response;
    try
    {
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const auto errors = manifest.Validate();
        if( !errors.empty() )
            throw std::invalid_argument( errors.front() );
        if( !ApplyAssemblyMates( manifest ) )
            throw std::invalid_argument( "assembly mate constraints could not be applied" );
        response.set_status( CommandStatus::CS_OK );
        response.set_manifest_json( manifest.ToJson().dump( 2 ) );
    }
    catch( const std::exception& e )
    {
        setFailure( response, e );
    }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleCheckAssemblyCollisions(
        const HANDLER_CONTEXT<CheckAssemblyCollisions>& aCtx )
{
    AssemblyResponse response;
    try
    {
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const auto errors = manifest.Validate( true );
        if( !errors.empty() )
            throw std::invalid_argument( errors.front() );
        const auto collisions = ASSEMBLY_EXPORTER::CheckCollisions(
                manifest, &NULL_REPORTER::GetInstance(), boardExporter( manifest ) );
        response.set_status( CommandStatus::CS_OK );
        for( const auto& collision : collisions )
        {
            auto* item = response.add_collisions();
            item->set_first( collision.first );
            item->set_second( collision.second );
            item->set_clearance_mm( collision.clearance_mm );
        }
    }
    catch( const std::exception& e )
    {
        setFailure( response, e );
    }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleExportAssemblyStep(
        const HANDLER_CONTEXT<ExportAssemblyStep>& aCtx )
{
    AssemblyResponse response;
    try
    {
        if( aCtx.Request.output_path().empty() )
            throw std::invalid_argument( "output_path is required" );
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const auto errors = manifest.Validate( true );
        if( !errors.empty() )
            throw std::invalid_argument( errors.front() );
        const auto output = manifestPath( aCtx.Request.output_path() );
        if( output.has_parent_path() && !std::filesystem::exists( output.parent_path() ) )
            throw std::invalid_argument( "STEP destination directory does not exist" );
        if( !ASSEMBLY_EXPORTER::Export( manifest, output.string(), &NULL_REPORTER::GetInstance(),
                                        boardExporter( manifest ) ) )
            throw std::runtime_error( "assembly STEP export failed" );
        response.set_status( CommandStatus::CS_OK );
        response.set_path( output.string() );
    }
    catch( const std::exception& e )
    {
        setFailure( response, e );
    }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleListAssemblyComponents(
        const HANDLER_CONTEXT<ListAssemblyComponents>& aCtx )
{
    if( aCtx.Request.has_document() )
    {
        if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
            return tl::unexpected( validation.error() );
    }
    AssemblyResponse response;
    try
    {
        const auto manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        response.set_result_json( AssemblyListComponents( manifest, aCtx.Request.instance_id(), board() ).dump( 2 ) );
        response.set_status( CommandStatus::CS_OK );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleMoveAssemblyComponent(
        const HANDLER_CONTEXT<MoveAssemblyComponent>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );
    if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
        return tl::unexpected( validation.error() );
    if( !m_activeClients.empty() )
    {
        ApiResponseStatus status;
        status.set_status( ApiStatusCode::AS_BAD_REQUEST );
        status.set_error_message( "Assembly component edits cannot participate in an item edit transaction" );
        return tl::unexpected( status );
    }
    AssemblyResponse response;
    try
    {
        if( aCtx.Request.path().empty() ) throw std::invalid_argument( "assembly path is required" );
        if( !aCtx.Request.has_x_mm() || !aCtx.Request.has_y_mm() || !aCtx.Request.has_rotation_deg() )
            throw std::invalid_argument( "x_mm, y_mm, and rotation_deg are required" );
        const auto path = manifestPath( aCtx.Request.path() );
        const auto manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const auto flat = FlattenAssembly( manifest );
        auto instance = std::find_if( flat.instances.begin(), flat.instances.end(),
                [&]( const auto& i ) { return i.id == aCtx.Request.instance_id(); } );
        std::error_code ec;
        if( instance == flat.instances.end() || !board()
            || !std::filesystem::equivalent( instance->source,
                        board()->GetFileName().ToStdString(), ec ) || ec )
            throw std::invalid_argument( "Open and target the source PCB document before moving its component" );
        if( !AssemblyMoveComponent( manifest, aCtx.Request.instance_id(), aCtx.Request.reference(),
                                    aCtx.Request.x_mm(), aCtx.Request.y_mm(),
                                    aCtx.Request.rotation_deg(), frame(), board() ) )
            throw std::runtime_error( "assembly component move failed" );
        response.set_status( CommandStatus::CS_OK ); response.set_path( path.string() );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleSyncAssemblyElectrical(
        const HANDLER_CONTEXT<SyncAssemblyElectrical>& aCtx )
{
    if( aCtx.Request.has_document() )
    {
        if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
            return tl::unexpected( validation.error() );
    }
    AssemblyResponse response;
    try
    {
        ASSEMBLY_MANIFEST manifest = loadManifest( aCtx.Request.path(), aCtx.Request.manifest_json() );
        const nlohmann::json snapshot = AssemblyElectricalSync( manifest, board() );
        response.set_result_json( snapshot.dump( 2 ) );
        if( snapshot.contains( "errors" ) && !snapshot.at( "errors" ).empty() )
            throw std::invalid_argument( snapshot.at( "errors" ).at( 0 ).get<std::string>() );
        if( aCtx.Request.save_snapshot() )
        {
            if( aCtx.Request.path().empty() ) throw std::invalid_argument( "assembly path is required to save snapshot" );
            manifest.electrical["snapshot"] = snapshot;
            manifest.base_directory = manifestPath( aCtx.Request.path() ).parent_path();
            manifest.Save( manifestPath( aCtx.Request.path() ) );
            response.set_path( manifestPath( aCtx.Request.path() ).string() );
        }
        response.set_status( CommandStatus::CS_OK );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleOpenAssemblyTab(
        const HANDLER_CONTEXT<OpenAssemblyTab>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "OpenAssemblyTab" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );
    if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
        return tl::unexpected( validation.error() );
    AssemblyResponse response;
    try
    {
        if( !frame() ) throw std::invalid_argument( "Assembly workspace requires a PCB editor window" );
        auto* workspace = DIALOG_ASSEMBLY::FindOrCreate( frame() );
        if( !workspace->OpenTab( wxString::FromUTF8( aCtx.Request.path() ) ) )
            throw std::runtime_error( "Could not open assembly tab" );
        response.set_result_json( workspace->GetOpenTabs().dump() );
        response.set_status( CommandStatus::CS_OK );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleListAssemblyTabs(
        const HANDLER_CONTEXT<ListAssemblyTabs>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "ListAssemblyTabs" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );
    if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
        return tl::unexpected( validation.error() );
    AssemblyResponse response;
    try
    {
        if( !frame() ) throw std::invalid_argument( "Assembly workspace requires a PCB editor window" );
        auto* workspace = DIALOG_ASSEMBLY::FindOrCreate( frame() );

        response.set_result_json( workspace->GetOpenTabs().dump() );
        response.set_status( CommandStatus::CS_OK );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}

HANDLER_RESULT<AssemblyResponse> API_HANDLER_PCB::handleShowAssembly3D(
        const HANDLER_CONTEXT<ShowAssembly3D>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "ShowAssembly3D" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );
    if( auto validation = validateDocument( aCtx.Request.document() ); !validation )
        return tl::unexpected( validation.error() );
    AssemblyResponse response;
    try
    {
        if( !frame() ) throw std::invalid_argument( "Assembly workspace requires a PCB editor window" );
        auto* workspace = DIALOG_ASSEMBLY::FindOrCreate( frame() );
        if( !workspace->ShowTab3D( aCtx.Request.tab_index() ) )
            throw std::runtime_error( "Could not display assembly tab in 3D" );
        response.set_result_json( workspace->GetOpenTabs().dump() );
        response.set_status( CommandStatus::CS_OK );
    }
    catch( const std::exception& e ) { setFailure( response, e ); }
    return response;
}
