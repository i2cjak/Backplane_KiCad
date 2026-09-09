/*
 * This program source code file is part of KiCad, a free EDA application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 */

#ifndef KICAD_API_CLIENT_H
#define KICAD_API_CLIENT_H

#include <string>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>
#include <wx/string.h>

#include <import_export.h>
#include <kicommon.h>

#include <google/protobuf/message.h>

#include <api/common/envelope.pb.h>

class KICOMMON_API KICAD_API_CLIENT
{
public:
    explicit KICAD_API_CLIENT( int aTimeoutMs = 1000 );
    ~KICAD_API_CLIENT();

    bool Connect( const wxString& aSocketUrl );
    void Disconnect();

    bool IsConnected() const { return m_isOpen && m_isConnected; }

    bool Send( const google::protobuf::Message& aRequest, kiapi::common::ApiResponse& aResponse,
               const std::string& aClientName = "kicad" );

    const wxString& GetLastError() const { return m_lastError; }

private:
    nng_socket m_socket;
    bool       m_isOpen;
    bool       m_isConnected;
    wxString   m_lastError;
};

#endif // KICAD_API_CLIENT_H
