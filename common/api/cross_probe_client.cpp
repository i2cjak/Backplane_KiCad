/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#include <api/cross_probe_client.h>

#include <api/api_client.h>
#include <api/api_server.h>
#include <api/common/envelope.pb.h>
#include <api/api_utils.h>
#include <pgm_base.h>
#include <wx/log.h>

#include <mutex>

std::mutex CROSS_PROBE_CLIENT::s_mutex;
std::map<FRAME_T, std::string> CROSS_PROBE_CLIENT::s_peers;


static bool sendRequest( const std::string& aUrl, const google::protobuf::Message& aRequest )
{
    KICAD_API_CLIENT client( 100 );

    if( !client.Connect( wxString::FromUTF8( aUrl ) ) )
    {
        wxLogTrace( traceApi, wxString::Format( wxS( "crossprobe: failed to connect to %s" ),
                                                wxString::FromUTF8( aUrl ) ) );
        return false;
    }

    kiapi::common::ApiResponse response;

    if( !client.Send( aRequest, response, kiapi::common::StandaloneCrossProbeClientName ) )
    {
        wxLogTrace( traceApi, wxString::Format( wxS( "crossprobe: failed to send to %s" ),
                                                wxString::FromUTF8( aUrl ) ) );
        return false;
    }

    return response.status().status() == kiapi::common::AS_OK;
}


bool CROSS_PROBE_CLIENT::SendToFrame( FRAME_T aTarget, const google::protobuf::Message& aRequest )
{
    std::optional<std::string> targetUrl;

    {
        std::lock_guard<std::mutex> lock( s_mutex );

        if( s_peers.contains( aTarget ) )
            targetUrl = s_peers.at( aTarget );
    }

    KICAD_API_SERVER& server = Pgm().GetApiServer();

    if( !targetUrl )
        targetUrl = KICAD_API_SERVER::StandardSocketUrl();

    if( server.Running() && *targetUrl == server.SocketPath() )
        return false;

    return sendRequest( *targetUrl, aRequest );
}


void CROSS_PROBE_CLIENT::AnnounceToPrimary( FRAME_T aFrameType )
{
    KICAD_API_SERVER& server = Pgm().GetApiServer();

    if( !server.Running() )
        return;

    std::string primaryUrl = KICAD_API_SERVER::StandardSocketUrl();
    std::string ourUrl = server.SocketPath();

    if( primaryUrl == ourUrl )
        return;

    kiapi::common::commands::CrossProbeAnnounce announce;
    announce.set_frame_type( static_cast<kiapi::common::types::FrameType>( aFrameType ) );
    announce.set_socket_path( ourUrl );
    announce.set_api_token( server.Token() );

    sendRequest( primaryUrl, announce );
}


void CROSS_PROBE_CLIENT::RegisterPeer( FRAME_T aFrameType, const std::string& aSocketPath )
{
    std::lock_guard<std::mutex> lock( s_mutex );
    s_peers[aFrameType] = aSocketPath;
}


bool CROSS_PROBE_CLIENT::IsOnStandardSocketPath()
{
    KICAD_API_SERVER& server = Pgm().GetApiServer();
    return server.Running() && server.SocketPath() == KICAD_API_SERVER::StandardSocketUrl();
}
