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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <api/api_handler_pcb.h>
#include <api/api_utils.h>
#include <api/common/commands/variant_commands.pb.h>
#include <string_utils.h>
#include <board.h>
#include <footprint.h>
#include <pcb_edit_frame.h>

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentType;


HANDLER_RESULT<VariantsResponse> API_HANDLER_PCB::handleGetVariants( const HANDLER_CONTEXT<GetVariants>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();
    VariantsResponse response;

    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const wxString& name : board->GetVariantNames() )
    {
        types::DesignVariant* var = response.add_variants();
        var->set_name( name.ToUTF8() );
        var->set_description( board->GetVariantDescription( name ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleAddVariant( const HANDLER_CONTEXT<AddVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->AddVariant( name );

    if( aCtx.Request.has_description() )
        board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleDeleteVariant( const HANDLER_CONTEXT<DeleteVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->DeleteVariant( name );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRenameVariant( const HANDLER_CONTEXT<RenameVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->RenameVariant( oldName, newName );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleCopyVariant( const HANDLER_CONTEXT<CopyVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->AddVariant( newName );

    for( FOOTPRINT* footprint : board->Footprints() )
    {
        if( const FOOTPRINT_VARIANT* source = footprint->GetVariant( oldName ) )
        {
            FOOTPRINT_VARIANT copy( *source );
            copy.SetName( newName );
            footprint->SetVariant( copy );
        }
    }

    board->SetVariantDescription( newName, aCtx.Request.has_new_description()
            ? wxString::FromUTF8( aCtx.Request.new_description() )
            : board->GetVariantDescription( oldName ) );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetVariantDescription( const HANDLER_CONTEXT<SetVariantDescription>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetCurrentVariant( const HANDLER_CONTEXT<SetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = context()->GetBoard();

    if( aCtx.Request.has_name() && !aCtx.Request.name().empty() )
    {
        if( wxString name = wxString::FromUTF8( aCtx.Request.name() ); !board->HasVariant( name ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
            return tl::unexpected( e );
        }
    }

    wxString varName = aCtx.Request.has_name() ? wxString::FromUTF8( aCtx.Request.name() ) : wxString();

    if( frame() )
        frame()->SetCurrentVariant( varName );
    else
        board->SetCurrentVariant( varName );

    onModified();
    return Empty();
}


HANDLER_RESULT<CurrentVariantResponse>
API_HANDLER_PCB::handleGetCurrentVariant( const HANDLER_CONTEXT<GetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CurrentVariantResponse response;

    if( wxString current = context()->GetBoard()->GetCurrentVariant(); !current.IsEmpty() )
        response.set_name( current.ToUTF8() );

    return response;
}
