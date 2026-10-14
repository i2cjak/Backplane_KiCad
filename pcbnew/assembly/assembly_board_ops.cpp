#include "assembly_board_ops.h"

#include "assembly_model.h"

#include <board.h>
#include <board_commit.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_io/pcb_io_mgr.h>
#include <math/util.h>

#include <wx/filename.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>

#include <kiid.h>
#include <lockfile.h>

namespace
{
std::filesystem::path sourcePath( const ASSEMBLY_MANIFEST& aManifest,
                                  const std::string& aInstanceId )
{
    const ASSEMBLY_MANIFEST flattened = FlattenAssembly( aManifest );
    const ASSEMBLY_INSTANCE* found = nullptr;
    for( const auto& instance : flattened.instances )
        if( instance.id == aInstanceId )
        {
            if( found ) throw std::invalid_argument( "ambiguous assembly instance: " + aInstanceId );
            found = &instance;
        }
    if( !found ) throw std::invalid_argument( "assembly instance not found: " + aInstanceId );
    if( found->kind != "board" ) throw std::invalid_argument( "assembly instance is not a board: " + aInstanceId );
    std::filesystem::path result = found->source;
    if( result.is_relative() ) result = flattened.base_directory / result;
    return std::filesystem::absolute( result );
}

bool samePath( const std::filesystem::path& a, const std::filesystem::path& b )
{
    std::error_code ec;
    return std::filesystem::exists( a, ec ) && std::filesystem::exists( b, ec )
           && std::filesystem::equivalent( a, b, ec ) && !ec;
}

std::unique_ptr<BOARD> loadBoard( const std::filesystem::path& aPath )
{
    if( !std::filesystem::exists( aPath ) ) throw std::invalid_argument( "missing board source: " + aPath.string() );
    std::unique_ptr<BOARD> board = PCB_IO_MGR::Load(
            PCB_IO_MGR::FindPluginTypeFromBoardPath( wxString::FromUTF8( aPath.string() ) ),
            wxString::FromUTF8( aPath.string() ) );
    if( !board ) throw std::runtime_error( "could not load board source: " + aPath.string() );
    return board;
}

FOOTPRINT* findUnique( BOARD* aBoard, const std::string& aReference )
{
    FOOTPRINT* result = nullptr;
    for( FOOTPRINT* footprint : aBoard->Footprints() )
        if( footprint->GetReference().CmpNoCase( wxString::FromUTF8( aReference ) ) == 0 )
        {
            if( result ) throw std::invalid_argument( "ambiguous component reference: " + aReference );
            result = footprint;
        }
    if( !result ) throw std::invalid_argument( "component reference not found: " + aReference );
    return result;
}

nlohmann::json componentJson( const FOOTPRINT& aFootprint, const BOARD& aBoard )
{
    nlohmann::json result = { { "reference", aFootprint.GetReference().ToStdString() },
                              { "x_mm", aFootprint.GetPosition().x / 1e6 },
                              { "y_mm", aFootprint.GetPosition().y / 1e6 },
                              { "rotation_deg", aFootprint.GetOrientation().AsDegrees() },
                              { "side", aFootprint.GetLayer() == B_Cu ? "bottom" : "top" },
                              { "pads", nlohmann::json::array() },
                              { "models", nlohmann::json::array() },
                              { "uuid", aFootprint.m_Uuid.AsStdString() } };
    for( const PAD* pad : aFootprint.Pads() )
    {
        nlohmann::json item = { { "number", pad->GetNumber().ToStdString() },
                                { "net_code", pad->GetNetCode() }, { "net_name", "" } };
        if( const NETINFO_ITEM* net = aBoard.FindNet( pad->GetNetCode() ) )
            item["net_name"] = net->GetNetname().ToStdString();
        result["pads"].push_back( std::move( item ) );
    }
    for( const FP_3DMODEL& model : aFootprint.Models() )
        result["models"].push_back( model.m_Filename.ToStdString() );
    return result;
}
}

nlohmann::json AssemblyListComponents( const ASSEMBLY_MANIFEST& aManifest,
                                       const std::string& aInstanceId, BOARD* aActiveBoard )
{
    const auto path = sourcePath( aManifest, aInstanceId );
    std::unique_ptr<BOARD> owned;
    BOARD* board = aActiveBoard && samePath( aActiveBoard->GetFileName().ToStdString(), path )
                       ? aActiveBoard : ( owned = loadBoard( path ), owned.get() );
    nlohmann::json result = { { "instance", aInstanceId }, { "components", nlohmann::json::array() } };
    for( FOOTPRINT* footprint : board->Footprints() )
        result["components"].push_back( componentJson( *footprint, *board ) );
    return result;
}

bool AssemblyMoveComponent( const ASSEMBLY_MANIFEST& aManifest, const std::string& aInstanceId,
                            const std::string& aReference, double aXmm, double aYmm,
                            double aRotationDeg, PCB_EDIT_FRAME* aFrame, BOARD* aActiveBoard )
{
    if( !std::isfinite( aXmm ) || !std::isfinite( aYmm ) || !std::isfinite( aRotationDeg ) )
        throw std::invalid_argument( "component position and rotation must be finite" );
    if( std::abs( aXmm ) > 2147.0 || std::abs( aYmm ) > 2147.0 )
        throw std::invalid_argument( "component position is outside the board coordinate range" );
    const auto path = sourcePath( aManifest, aInstanceId );
    if( !aActiveBoard && aFrame ) aActiveBoard = aFrame->GetBoard();
    const bool active = aActiveBoard && samePath( aActiveBoard->GetFileName().ToStdString(), path );
    std::unique_ptr<LOCKFILE> sourceLock;
    if( !active )
    {
        sourceLock = std::make_unique<LOCKFILE>( wxString::FromUTF8( path.string() ) );
        if( !sourceLock->Locked() )
            throw std::invalid_argument( "Source PCB is open elsewhere or not writable; edit it in its own PCB editor" );
    }
    std::unique_ptr<BOARD> ownedBoard;
    BOARD* board = active ? aActiveBoard : ( ownedBoard = loadBoard( path ), ownedBoard.get() );
    FOOTPRINT* footprint = findUnique( board, aReference );
    const VECTOR2I position( KiROUND( aXmm * 1e6 ), KiROUND( aYmm * 1e6 ) );
    if( active && aFrame )
    {
        BOARD_COMMIT commit( aFrame );
        commit.Stage( footprint, CHT_MODIFY );
        footprint->SetPosition( position );
        footprint->SetOrientation( EDA_ANGLE( aRotationDeg, DEGREES_T ) );
        commit.Push( _( "Move assembly component" ) );
        aFrame->OnModify();
        return true;
    }
    footprint->SetPosition( position );
    footprint->SetOrientation( EDA_ANGLE( aRotationDeg, DEGREES_T ) );
    if( active )
    {
        board->SetModified();
        return true;
    }
    const auto backupDir = path.parent_path() / ".backplane-backups";
    std::filesystem::create_directories( backupDir );
    const auto backup = backupDir / ( path.filename().string() + ".bak" );
    std::filesystem::copy_file( path, backup, std::filesystem::copy_options::overwrite_existing );
    const auto companion = std::filesystem::path( path.string() + ".backplane.json" );
    if( std::filesystem::exists( companion ) )
        std::filesystem::copy_file( companion, backupDir / ( companion.filename().string() + ".bak" ),
                                    std::filesystem::copy_options::overwrite_existing );
    const auto staging = std::filesystem::path( path.string() + ".backplane-stage" );
    const auto stagingCompanion = std::filesystem::path( staging.string() + ".backplane.json" );
    try
    {
        PCB_IO_MGR::Save( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( staging.string() ), *board,
                          nullptr );
        std::unique_ptr<BOARD> verified = PCB_IO_MGR::Load(
                PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( staging.string() ) );
        if( !verified ) throw std::runtime_error( "Could not reopen staged source board" );
        FOOTPRINT* saved = findUnique( verified.get(), aReference );
        if( saved->GetPosition() != position || saved->GetOrientation() != footprint->GetOrientation() )
            throw std::runtime_error( "Staged component edit failed verification" );
        // Component movement does not change construction. Preserve the original
        // companion byte-for-byte; only the verified native file is promoted.
        std::filesystem::rename( staging, path );
        std::error_code ec;
        std::filesystem::remove( stagingCompanion, ec );
    }
    catch( ... )
    {
        std::error_code ec;
        std::filesystem::remove( staging, ec );
        std::filesystem::remove( stagingCompanion, ec );
        throw;
    }
    return true;
}

nlohmann::json AssemblyElectricalSync( const ASSEMBLY_MANIFEST& aManifest, BOARD* aActiveBoard )
{
    // Electrical synchronization is intentionally read-only here.  The IPC
    // layer compares this fresh snapshot with the manifest's stored snapshot.
    nlohmann::json result = { { "schema", 1 }, { "links", nlohmann::json::array() },
                              { "errors", nlohmann::json::array() } };
    if( !aManifest.electrical.is_object() )
    {
        result["errors"].push_back( "electrical must be an object" );
        return result;
    }
    if( !aManifest.electrical.contains( "links" ) || !aManifest.electrical["links"].is_array() )
    {
        result["errors"].push_back( "electrical.links must be an array" );
        return result;
    }
    const ASSEMBLY_MANIFEST flattened = FlattenAssembly( aManifest );
    for( const auto& link : aManifest.electrical["links"] )
    {
        nlohmann::json fresh = link;
        if( !link.is_object() || !link.contains( "endpoints" ) || !link["endpoints"].is_array() )
        {
            result["errors"].push_back( "electrical link has no endpoints" );
            continue;
        }
        fresh.erase( "system_net" );
        if( fresh["endpoints"].size() < 2 ) result["errors"].push_back( "An electrical link needs at least two endpoints" );
        for( auto& endpoint : fresh["endpoints"] )
        {
            try
            {
                if( !endpoint.is_object() ) throw std::invalid_argument( "Electrical endpoint must be an object" );
                endpoint.erase( "net_name" );
                endpoint.erase( "uuid" );
                const auto instance = endpoint.at( "instance" ).get<std::string>();
                const auto reference = endpoint.at( "reference" ).get<std::string>();
                const auto padNumber = endpoint.at( "pad" ).get<std::string>();
                std::unique_ptr<BOARD> owned;
                BOARD* board = aActiveBoard && samePath( aActiveBoard->GetFileName().ToStdString(),
                                                        sourcePath( flattened, instance ) )
                                   ? aActiveBoard : ( owned = loadBoard( sourcePath( flattened, instance ) ), owned.get() );
                FOOTPRINT* footprint = findUnique( board, reference );
                const PAD* foundPad = nullptr;
                for( const PAD* pad : footprint->Pads() )
                    if( pad->GetNumber() == wxString::FromUTF8( padNumber ) )
                    {
                        if( foundPad ) throw std::invalid_argument( "ambiguous pad: " + padNumber );
                        foundPad = pad;
                    }
                if( !foundPad ) throw std::invalid_argument( "pad not found: " + padNumber );
                const NETINFO_ITEM* net = board->FindNet( foundPad->GetNetCode() );
                if( !net || net->GetNetname().IsEmpty() ) throw std::invalid_argument( "unnamed net" );
                endpoint["net_name"] = net->GetNetname().ToStdString();
                endpoint["uuid"] = KIID::FromName( "backplane-endpoint:" + instance + ":" +
                                                   footprint->m_Uuid.AsStdString() + ":" + foundPad->m_Uuid.AsStdString() ).AsStdString();
                endpoint["footprint_uuid"] = footprint->m_Uuid.AsStdString();
                endpoint["pad_uuid"] = foundPad->m_Uuid.AsStdString();
            }
            catch( const std::exception& e )
            {
                result["errors"].push_back( e.what() );
            }
        }
        result["links"].push_back( std::move( fresh ) );
    }
    // Union endpoints which refer to the same native board net, then union
    // all endpoints in each declared link. Native names may differ across
    // boards; only conflicting explicit system link names are errors.
    std::map<std::string, int> nodes;
    std::vector<int> parent;
    auto node = [&]( const std::string& key )
    {
        auto [it, inserted] = nodes.emplace( key, (int) parent.size() );
        if( inserted ) parent.push_back( it->second );
        return it->second;
    };
    auto root = [&]( int value ) { while( parent[value] != value ) { parent[value] = parent[parent[value]]; value = parent[value]; } return value; };
    auto join = [&]( int a, int b ) { a = root( a ); b = root( b ); if( a != b ) parent[b] = a; };
    for( auto& link : result["links"] )
    {
        int first = -1;
        for( const auto& endpoint : link.value( "endpoints", nlohmann::json::array() ) )
        {
            if( !endpoint.contains( "net_name" ) ) continue;
            const std::string key = endpoint.value( "instance", "" ) + ":" + endpoint.value( "net_name", "" );
            const int current = node( key );
            if( first < 0 ) first = current; else join( first, current );
        }
    }
    std::map<int, std::string> identities;
    for( const auto& [key, index] : nodes ) identities[root( index )] += std::to_string( key.size() ) + ":" + key;
    std::map<int, std::set<std::string>> names;
    for( const auto& link : result["links"] )
        for( const auto& endpoint : link.value( "endpoints", nlohmann::json::array() ) )
            if( endpoint.contains( "net_name" ) )
            {
                const std::string key = endpoint.value( "instance", "" ) + ":" + endpoint.value( "net_name", "" );
                if( link.contains( "name" ) && !link["name"].get<std::string>().empty() )
                    names[root( nodes.at( key ) )].insert( link["name"].get<std::string>() );
            }
    for( auto& link : result["links"] )
    {
        std::set<int> components;
        for( const auto& endpoint : link.value( "endpoints", nlohmann::json::array() ) )
            if( endpoint.contains( "net_name" ) )
                components.insert( root( nodes.at( endpoint.value( "instance", "" ) + ":" + endpoint.value( "net_name", "" ) ) ) );
        for( const int component : components )
        {
            const std::string system = KIID::FromName( "backplane-electrical:" + identities[component] ).AsStdString();
            link["system_net"] = system;
            if( names[component].size() > 1 )
                result["errors"].push_back( "distinct system link names merged in electrical link" );
        }
    }
    auto previous = aManifest.electrical.value( "snapshot", nlohmann::json() );
    if( previous.is_object() ) previous.erase( "changed" );
    result["changed"] = previous.is_null() || previous != result;
    return result;
}
