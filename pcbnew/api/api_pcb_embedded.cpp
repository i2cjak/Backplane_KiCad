/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <api/api_handler_pcb.h>
#include <api/api_pcb_embedded_utils.h>
#include <board.h>
#include <embedded_files.h>

using namespace kiapi::board::commands;

namespace
{
HANDLER_RESULT<google::protobuf::Empty> decodeFiles(
        EMBEDDED_FILES& aOutput, const common::types::EmbeddedFiles& aProto )
{
    if( !kiapi::board::UnpackEmbeddedFiles( aOutput, aProto ) )
    {
        ApiResponseStatus error;
        error.set_status( AS_BAD_REQUEST );
        error.set_error_message( "embedded file validation failed" );
        return tl::unexpected( error );
    }

    return google::protobuf::Empty();
}
}


HANDLER_RESULT<common::types::EmbeddedFiles> API_HANDLER_PCB::handleGetEmbeddedFiles(
        const HANDLER_CONTEXT<GetEmbeddedFiles>& aCtx )
{
    if( auto busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( auto validation = validateDocument( aCtx.Request.board() ); !validation )
        return tl::unexpected( validation.error() );

    common::types::EmbeddedFiles response;
    kiapi::board::PackEmbeddedFiles( response, *board() );
    return response;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_PCB::handleAddEmbeddedFiles(
        const HANDLER_CONTEXT<AddEmbeddedFiles>& aCtx )
{
    if( auto busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( auto validation = validateDocument( aCtx.Request.board() ); !validation )
        return tl::unexpected( validation.error() );

    EMBEDDED_FILES files;
    auto result = decodeFiles( files, aCtx.Request.files() );

    if( !result )
        return result;

    for( const auto& [name, file] : files.EmbeddedFileMap() )
        board()->GetEmbeddedFiles()->AddFile( std::make_shared<EMBEDDED_FILES::EMBEDDED_FILE>( *file ) );

    onModified();
    return result;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_PCB::handleSetEmbeddedFiles(
        const HANDLER_CONTEXT<SetEmbeddedFiles>& aCtx )
{
    if( auto busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( auto validation = validateDocument( aCtx.Request.board() ); !validation )
        return tl::unexpected( validation.error() );

    auto result = decodeFiles( *board()->GetEmbeddedFiles(), aCtx.Request.files() );

    if( result )
        onModified();

    return result;
}
