/*
 * This program source code file is part of KiCad, a free EDA application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 */

#include <api/api_client.h>

#include <cstring>

#include <wx/log.h>

KICAD_API_CLIENT::KICAD_API_CLIENT( int aTimeoutMs ) :
        m_socket(),
        m_isOpen( false ),
        m_isConnected( false )
{
    int ret = nng_req0_open( &m_socket );

    if( ret == 0 )
    {
        m_isOpen = true;
        nng_socket_set_ms( m_socket, NNG_OPT_RECVTIMEO, aTimeoutMs );
        nng_socket_set_ms( m_socket, NNG_OPT_SENDTIMEO, aTimeoutMs );
    }
    else
    {
        m_lastError = wxString::Format( wxS( "nng_req0_open failed: %s" ),
                                        wxString::FromUTF8( nng_strerror( ret ) ) );
    }
}


KICAD_API_CLIENT::~KICAD_API_CLIENT()
{
    if( m_isOpen )
        nng_close( m_socket );
}


bool KICAD_API_CLIENT::Connect( const wxString& aSocketUrl )
{
    if( !m_isOpen )
        return false;

    if( m_isConnected )
        return true;

    int ret = nng_dial( m_socket, aSocketUrl.ToStdString().c_str(), nullptr, 0 );

    if( ret != 0 )
    {
        m_lastError = wxString::Format( wxS( "nng_dial failed: %s" ),
                                        wxString::FromUTF8( nng_strerror( ret ) ) );
        return false;
    }

    m_isConnected = true;
    return true;
}


void KICAD_API_CLIENT::Disconnect()
{
    if( !m_isOpen )
        return;

    nng_close( m_socket );
    m_isOpen = false;
    m_isConnected = false;

    int ret = nng_req0_open( &m_socket );

    if( ret == 0 )
    {
        m_isOpen = true;
        nng_socket_set_ms( m_socket, NNG_OPT_RECVTIMEO, 1000 );
        nng_socket_set_ms( m_socket, NNG_OPT_SENDTIMEO, 1000 );
    }
}


bool KICAD_API_CLIENT::Send( const google::protobuf::Message& aRequest,
                             kiapi::common::ApiResponse& aResponse,
                             const std::string& aClientName )
{
    if( !IsConnected() )
    {
        m_lastError = wxS( "API client is not connected" );
        return false;
    }

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( aClientName );

    if( !request.mutable_message()->PackFrom( aRequest ) )
    {
        m_lastError = wxS( "Failed to pack command into ApiRequest" );
        return false;
    }

    std::string requestStr = request.SerializeAsString();
    void* sendBuf = nng_alloc( requestStr.size() );

    if( !sendBuf )
    {
        m_lastError = wxS( "nng_alloc failed" );
        return false;
    }

    std::memcpy( sendBuf, requestStr.data(), requestStr.size() );

    int ret = nng_send( m_socket, sendBuf, requestStr.size(), NNG_FLAG_ALLOC );

    if( ret != 0 )
    {
        nng_free( sendBuf, requestStr.size() );
        m_lastError = wxString::Format( wxS( "nng_send failed: %s" ),
                                        wxString::FromUTF8( nng_strerror( ret ) ) );
        return false;
    }

    char* reply = nullptr;
    size_t replySize = 0;
    ret = nng_recv( m_socket, &reply, &replySize, NNG_FLAG_ALLOC );

    if( ret != 0 )
    {
        m_lastError = wxString::Format( wxS( "nng_recv failed: %s" ),
                                        wxString::FromUTF8( nng_strerror( ret ) ) );
        return false;
    }

    std::string responseStr( reply, replySize );
    nng_free( reply, replySize );

    if( !aResponse.ParseFromString( responseStr ) )
    {
        m_lastError = wxS( "Failed to parse reply from KiCad" );
        return false;
    }

    return true;
}
