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
    m_hadExtensions = false;
    m_items = nlohmann::json::object();
    m_extensions = nlohmann::json::object();
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

        if( !data.at( "version" ).is_number_integer() )
            THROW_IO_ERROR( "Backplane companion version must be an integer" );

        const int version = data.at( "version" ).get<int>();

        if( data.at( "format" ) != "backplane-kicad-metadata" || ( version != 1 && version != 2 ) )
            THROW_IO_ERROR( wxString::Format( "Unsupported Backplane companion metadata: %s", path ) );

        if( version == 1 )
        {
            m_items = data.at( "items" );

            if( !m_items.is_object() )
                THROW_IO_ERROR( "Backplane companion items must be an object" );

            for( const auto& [key, entry] : m_items.items() )
            {
                if( !entry.is_object() )
                    THROW_IO_ERROR( "Backplane companion item must be an object" );

                for( const auto& [name, value] : entry.items() )
                {
                    if( name != "custom_properties" && name != "start_ending" && name != "end_ending"
                        && name != "footprint" )
                        m_extensions[key][name] = value;
                }
            }
        }
        else
        {
            m_extensions = data.at( "extensions" );

            if( !m_extensions.is_object() )
                THROW_IO_ERROR( "Backplane companion extensions must be an object" );
        }

        for( auto entry = m_extensions.begin(); entry != m_extensions.end(); )
        {
            if( !entry->is_object() )
                THROW_IO_ERROR( "Backplane companion extension record must be an object" );

            if( entry->empty() )
                entry = m_extensions.erase( entry );
            else
                ++entry;
        }
        m_hadExtensions = !m_extensions.empty();
    }
    catch( const nlohmann::json::exception& error )
    {
        THROW_IO_ERROR( wxString::Format( "Invalid Backplane companion metadata %s: %s", path,
                                         wxString::FromUTF8( error.what() ) ) );
    }
}


nlohmann::json BACKPLANE_DOCUMENT_METADATA::ToJson() const
{
    return { { "format", "backplane-kicad-metadata" }, { "version", 2 },
             { "extensions", m_extensions } };
}


void BACKPLANE_DOCUMENT_METADATA::Save( const wxString& aDocumentPath ) const
{
    const wxString path = CompanionPath( aDocumentPath );

    if( m_extensions.empty() )
    {
        if( wxFileExists( path ) && !wxRemoveFile( path ) )
            THROW_IO_ERROR( wxString::Format( "Cannot remove empty Backplane companion metadata: %s", path ) );

        return;
    }

    const std::string contents = ToJson().dump( 2 ) + "\n";
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

            auto properties = aItem.GetCustomProperties();

            for( const auto& [name, value] : entry->at( "custom_properties" ).items() )
                properties.try_emplace( wxString::FromUTF8( name ),
                                        wxString::FromUTF8( value.get<std::string>() ) );

            aItem.SetCustomProperties( properties );
            aItem.RemoveConflictingCustomProperties();
        }

        if( auto* shape = dynamic_cast<EDA_SHAPE*>( &aItem ) )
        {
            LINE_ENDING start, end;

            if( ReadLineEndings( aKey, start, end ) )
            {
                if( shape->GetStartEnding().GetStyle() == LINE_ENDING_STYLE::NONE )
                    shape->SetStartEnding( start );

                if( shape->GetEndEnding().GetStyle() == LINE_ENDING_STYLE::NONE )
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
        m_hadExtensions = true;
        m_extensions[aKey.ToStdString()][aName] = aData;
    }
    else if( auto entry = m_extensions.find( aKey.ToStdString() ); entry != m_extensions.end() )
    {
        entry->erase( aName );

        if( entry->empty() )
            m_extensions.erase( entry );
    }
}


nlohmann::json BACKPLANE_DOCUMENT_METADATA::ReadExtension( const wxString& aKey,
                                                          const std::string& aName ) const
{
    const auto entry = m_extensions.find( aKey.ToStdString() );

    if( entry == m_extensions.end() || !entry->contains( aName ) )
    {
        // Footprint simulation flags are native on master too; expose them only
        // during legacy load, without retaining them in the v2 extension set.
        const auto legacy = m_items.find( aKey.ToStdString() );

        if( aName == "footprint" && legacy != m_items.end() && legacy->contains( aName ) )
            return legacy->at( aName );

        return nullptr;
    }

    return entry->at( aName );
}
