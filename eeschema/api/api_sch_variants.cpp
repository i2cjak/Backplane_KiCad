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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <api/api_handler_sch.h>
#include <api/api_utils.h>
#include <api/common/commands/variant_commands.pb.h>
#include <string_utils.h>
#include <schematic.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <class_draw_panel_gal.h>

namespace
{
wxString canonicalVariantName( const SCHEMATIC& aSchematic, const wxString& aName )
{
    for( const wxString& name : aSchematic.GetVariantNames() )
    {
        if( name.CmpNoCase( aName ) == 0 )
            return name;
    }

    return wxEmptyString;
}
}

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentType;


HANDLER_RESULT<VariantsResponse> API_HANDLER_SCH::handleGetVariants( const HANDLER_CONTEXT<GetVariants>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    VariantsResponse response;

    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const wxString& name : schematic()->GetVariantNames() )
    {
        types::DesignVariant* var = response.add_variants();
        var->set_name( name.ToUTF8() );
        var->set_description( schematic()->GetVariantDescription( name ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleAddVariant( const HANDLER_CONTEXT<AddVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || !canonicalVariantName( *schematic, name ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a usable new variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    schematic->AddVariant( name );

    if( aCtx.Request.has_description() )
        schematic->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    if( m_frame )
        m_frame->UpdateVariantSelectionCtrl( m_frame->Schematic().GetVariantNamesForUI() );

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleDeleteVariant( const HANDLER_CONTEXT<DeleteVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? m_frame->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( canonicalVariantName( *schematic, name ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    name = canonicalVariantName( *schematic, name );

    if( schematic->GetCurrentVariant().CmpNoCase( name ) == 0 )
        schematic->SetCurrentVariant( wxEmptyString );

    schematic->DeleteVariant( name, &commit );
    commit.Push( _( "Delete Variant" ) );

    if( m_frame )
    {
        if( m_frame->Schematic().GetCurrentVariant().CmpNoCase( name ) == 0 )
            m_frame->SetCurrentVariant( wxEmptyString );

        m_frame->UpdateVariantSelectionCtrl( m_frame->Schematic().GetVariantNamesForUI() );
        m_frame->GetCanvas()->Refresh();
    }

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleRenameVariant( const HANDLER_CONTEXT<RenameVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? m_frame->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString   newName = wxString::FromUTF8( aCtx.Request.new_name() );

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

    if( canonicalVariantName( *schematic, oldName ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( !canonicalVariantName( *schematic, newName ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    oldName = canonicalVariantName( *schematic, oldName );

    schematic->RenameVariant( oldName, newName, &commit );
    commit.Push( _( "Rename Variant" ) );

    if( m_frame )
        m_frame->UpdateVariantSelectionCtrl( m_frame->Schematic().GetVariantNamesForUI() );

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleCopyVariant( const HANDLER_CONTEXT<CopyVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? m_frame->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString   newName = wxString::FromUTF8( aCtx.Request.new_name() );

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

    if( canonicalVariantName( *schematic, oldName ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( !canonicalVariantName( *schematic, newName ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    oldName = canonicalVariantName( *schematic, oldName );

    schematic->CopyVariant( oldName, newName, &commit );
    commit.Push( _( "Copy Variant" ) );

    if( aCtx.Request.has_new_description() )
        schematic->SetVariantDescription( newName, wxString::FromUTF8( aCtx.Request.new_description() ) );

    if( m_frame )
        m_frame->UpdateVariantSelectionCtrl( m_frame->Schematic().GetVariantNamesForUI() );

    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSetVariantDescription( const HANDLER_CONTEXT<SetVariantDescription>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || canonicalVariantName( *schematic, name ).IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    schematic->SetVariantDescription( canonicalVariantName( *schematic, name ),
                                      wxString::FromUTF8( aCtx.Request.description() ) );
    onModified();
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSetCurrentVariant( const HANDLER_CONTEXT<SetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();

    if( aCtx.Request.has_name() && !aCtx.Request.name().empty() )
    {
        if( wxString name = wxString::FromUTF8( aCtx.Request.name() ); canonicalVariantName( *schematic, name ).IsEmpty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
            return tl::unexpected( e );
        }
    }

    wxString name = aCtx.Request.has_name() ? wxString::FromUTF8( aCtx.Request.name() ) : wxString();

    name = canonicalVariantName( *schematic, name );

    if( m_frame )
        m_frame->SetCurrentVariant( name );
    else
        schematic->SetCurrentVariant( name );

    onModified();
    return Empty();
}


HANDLER_RESULT<CurrentVariantResponse>
API_HANDLER_SCH::handleGetCurrentVariant( const HANDLER_CONTEXT<GetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus status;
        status.set_status( AS_UNHANDLED );
        return tl::unexpected( status );
    }

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CurrentVariantResponse response;

    if( wxString current = schematic()->GetCurrentVariant(); !current.IsEmpty() )
        response.set_name( current.ToUTF8() );

    return response;
}
