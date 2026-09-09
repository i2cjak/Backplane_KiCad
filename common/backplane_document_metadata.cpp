/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <backplane_document_metadata.h>
#include <eda_item.h>
#include <eda_shape.h>
#include <line_ending.h>
#include <richio.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

namespace
{
nlohmann::json packEnding( const LINE_ENDING& aEnding )
{
    const KIGFX::COLOR4D color = aEnding.GetStroke().GetColor();

    return { { "style", static_cast<int>( aEnding.GetStyle() ) },
             { "length", aEnding.GetLength() }, { "width", aEnding.GetWidth() },
             { "stroke_width", aEnding.GetStrokeWidth() },
             { "stroke_style", static_cast<int>( aEnding.GetStroke().GetLineStyle() ) },
             { "stroke_color", { color.r, color.g, color.b, color.a } } };
}


LINE_ENDING unpackEnding( const nlohmann::json& aData )
{
    const int style = aData.at( "style" ).get<int>();

    if( style < static_cast<int>( LINE_ENDING_STYLE::NONE )
        || style > static_cast<int>( LINE_ENDING_STYLE::ARROW_OPEN ) )
    {
        THROW_IO_ERROR( "Unknown line-ending style in Backplane companion metadata" );
    }

    LINE_ENDING ending( static_cast<LINE_ENDING_STYLE>( style ),
                        aData.at( "length" ).get<int>(), aData.at( "width" ).get<int>(),
                        aData.at( "stroke_width" ).get<int>() );
    const auto& color = aData.at( "stroke_color" );

    ending.SetStroke( STROKE_PARAMS(
            ending.GetStrokeWidth(), static_cast<LINE_STYLE>( aData.at( "stroke_style" ).get<int>() ),
            KIGFX::COLOR4D( color.at( 0 ).get<double>(), color.at( 1 ).get<double>(),
                            color.at( 2 ).get<double>(), color.at( 3 ).get<double>() ) ) );

    return ending;
}
}


wxString BACKPLANE_DOCUMENT_METADATA::CompanionPath( const wxString& aDocumentPath )
{
    return aDocumentPath + wxS( ".backplane.json" );
}


void BACKPLANE_DOCUMENT_METADATA::Load( const wxString& aDocumentPath )
{
    m_items = nlohmann::json::object();
    const wxString path = CompanionPath( aDocumentPath );

    if( !wxFileExists( path ) )
        return;

    wxFile file( path );
    wxString contents;

    if( !file.IsOpened() || !file.ReadAll( &contents, wxConvUTF8 ) )
        THROW_IO_ERROR( wxString::Format( "Cannot read Backplane companion metadata: %s", path ) );

    try
    {
        const auto data = nlohmann::json::parse( contents.ToUTF8().data() );

        if( data.at( "format" ) != "backplane-kicad-metadata" || data.at( "version" ) != 1
            || !data.at( "items" ).is_object() )
        {
            THROW_IO_ERROR( wxString::Format( "Unsupported Backplane companion metadata: %s", path ) );
        }

        m_items = data.at( "items" );
    }
    catch( const nlohmann::json::exception& error )
    {
        THROW_IO_ERROR( wxString::Format( "Invalid Backplane companion metadata %s: %s", path,
                                         wxString::FromUTF8( error.what() ) ) );
    }
}


void BACKPLANE_DOCUMENT_METADATA::Save( const wxString& aDocumentPath ) const
{
    const wxString path = CompanionPath( aDocumentPath );

    if( m_items.empty() )
    {
        if( wxFileExists( path ) && !wxRemoveFile( path ) )
            THROW_IO_ERROR( wxString::Format( "Cannot remove empty Backplane companion metadata: %s", path ) );

        return;
    }

    const nlohmann::json data = { { "format", "backplane-kicad-metadata" }, { "version", 1 },
                                  { "items", m_items } };
    const std::string contents = data.dump( 2 ) + "\n";
    wxFile file;
    const wxString temporary = wxFileName::CreateTempFileName( path + wxS( ".tmp-" ), &file );

    if( temporary.empty() || !file.IsOpened() )
        THROW_IO_ERROR( wxString::Format( "Cannot create Backplane companion metadata: %s", path ) );

    const bool written = file.Write( contents.data(), contents.size() ) == contents.size();
    const bool closed = file.Close();

    if( !written || !closed )
    {
        wxRemoveFile( temporary );
        THROW_IO_ERROR( wxString::Format( "Cannot write Backplane companion metadata: %s", path ) );
    }

    if( !wxRenameFile( temporary, path, true ) )
    {
        wxRemoveFile( temporary );
        THROW_IO_ERROR( wxString::Format( "Cannot replace Backplane companion metadata: %s", path ) );
    }
}


void BACKPLANE_DOCUMENT_METADATA::Capture( const wxString& aKey, const EDA_ITEM& aItem )
{
    const std::string key = aKey.ToStdString();

    if( aItem.HasCustomProperties() )
    {
        auto& properties = m_items[key]["custom_properties"] = nlohmann::json::object();

        for( const auto& [name, value] : aItem.GetCustomProperties() )
            properties[name.ToStdString()] = value.ToStdString();
    }
    else if( auto entry = m_items.find( key ); entry != m_items.end() )
    {
        entry->erase( "custom_properties" );

        if( entry->empty() )
            m_items.erase( entry );
    }

    if( const auto* shape = dynamic_cast<const EDA_SHAPE*>( &aItem ) )
        CaptureLineEndings( aKey, shape->GetStartEnding(), shape->GetEndEnding() );
}


void BACKPLANE_DOCUMENT_METADATA::Apply( const wxString& aKey, EDA_ITEM& aItem ) const
{
    const auto entry = m_items.find( aKey.ToStdString() );

    if( entry == m_items.end() )
        return;

    try
    {
        if( entry->contains( "custom_properties" ) )
        {
            if( !entry->at( "custom_properties" ).is_object() )
                THROW_IO_ERROR( "Custom properties in Backplane metadata must be an object" );

            std::map<wxString, wxString> properties;

            for( const auto& [name, value] : entry->at( "custom_properties" ).items() )
                properties[wxString::FromUTF8( name )] = wxString::FromUTF8( value.get<std::string>() );

            aItem.SetCustomProperties( properties );
            aItem.RemoveConflictingCustomProperties();
        }

        if( auto* shape = dynamic_cast<EDA_SHAPE*>( &aItem ) )
        {
            LINE_ENDING start, end;

            if( ReadLineEndings( aKey, start, end ) )
            {
                shape->SetStartEnding( start );
                shape->SetEndEnding( end );
            }
        }
    }
    catch( const nlohmann::json::exception& error )
    {
        THROW_IO_ERROR( wxString::Format( "Invalid Backplane item metadata for %s: %s", aKey,
                                         wxString::FromUTF8( error.what() ) ) );
    }
}


void BACKPLANE_DOCUMENT_METADATA::CaptureLineEndings( const wxString& aKey,
                                                     const LINE_ENDING& aStart,
                                                     const LINE_ENDING& aEnd )
{
    if( aStart.GetStyle() != LINE_ENDING_STYLE::NONE || aEnd.GetStyle() != LINE_ENDING_STYLE::NONE )
    {
        m_items[aKey.ToStdString()]["start_ending"] = packEnding( aStart );
        m_items[aKey.ToStdString()]["end_ending"] = packEnding( aEnd );
    }
    else if( auto entry = m_items.find( aKey.ToStdString() ); entry != m_items.end() )
    {
        entry->erase( "start_ending" );
        entry->erase( "end_ending" );

        if( entry->empty() )
            m_items.erase( entry );
    }
}


bool BACKPLANE_DOCUMENT_METADATA::ReadLineEndings( const wxString& aKey, LINE_ENDING& aStart,
                                                  LINE_ENDING& aEnd ) const
{
    const auto entry = m_items.find( aKey.ToStdString() );

    if( entry == m_items.end() || !entry->contains( "start_ending" ) )
        return false;

    try
    {
        aStart = unpackEnding( entry->at( "start_ending" ) );
        aEnd = unpackEnding( entry->at( "end_ending" ) );
    }
    catch( const nlohmann::json::exception& error )
    {
        THROW_IO_ERROR( wxString::Format( "Invalid Backplane line-ending metadata for %s: %s", aKey,
                                         wxString::FromUTF8( error.what() ) ) );
    }

    return true;
}


void BACKPLANE_DOCUMENT_METADATA::CaptureExtension( const wxString& aKey,
                                                    const std::string& aName,
                                                    const nlohmann::json& aData )
{
    if( !aData.empty() )
    {
        m_items[aKey.ToStdString()][aName] = aData;
    }
    else if( auto entry = m_items.find( aKey.ToStdString() ); entry != m_items.end() )
    {
        entry->erase( aName );

        if( entry->empty() )
            m_items.erase( entry );
    }
}


nlohmann::json BACKPLANE_DOCUMENT_METADATA::ReadExtension( const wxString& aKey,
                                                          const std::string& aName ) const
{
    const auto entry = m_items.find( aKey.ToStdString() );

    if( entry == m_items.end() || !entry->contains( aName ) )
        return nullptr;

    return entry->at( aName );
}
