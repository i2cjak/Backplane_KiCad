/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <boost/test/unit_test.hpp>
#include <backplane_document_metadata.h>
#include <pcb_shape.h>
#include <richio.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

namespace
{
struct METADATA_FIXTURE
{
    wxString path = wxFileName::CreateTempFileName( wxS( "backplane-metadata-" ) );
    ~METADATA_FIXTURE()
    {
        wxRemoveFile( path );
        if( wxFileExists( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) ) )
            wxRemoveFile( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) );
    }

    void Write( const nlohmann::json& aData )
    {
        wxFile file( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ), wxFile::write );
        const std::string text = aData.dump();
        BOOST_REQUIRE( file.IsOpened() );
        BOOST_REQUIRE_EQUAL( file.Write( text.data(), text.size() ), text.size() );
    }

    nlohmann::json Read()
    {
        wxFile file( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) );
        wxString text;
        BOOST_REQUIRE( file.ReadAll( &text, wxConvUTF8 ) );
        return nlohmann::json::parse( text.ToUTF8().data() );
    }
};
}

BOOST_FIXTURE_TEST_SUITE( BackplaneMetadataMigration, METADATA_FIXTURE )

BOOST_AUTO_TEST_CASE( NativeValuesWinAndLegacyItemsAreNotWritten )
{
    const nlohmann::json ending = { { "style", 1 }, { "length", 700000 }, { "width", 400000 },
                                    { "stroke_width", 100000 }, { "stroke_style", 0 },
                                    { "stroke_color", { 0.2, 0.3, 0.4, 1.0 } } };
    const nlohmann::json extension = { { "future", true } };
    Write( { { "format", "backplane-kicad-metadata" }, { "version", 1 },
             { "items", { { "shape", { { "custom_properties", { { "native", "stale" },
                                                                     { "legacy", "migrated" } } },
                                         { "start_ending", ending }, { "end_ending", ending } } },
                          { "root", { { "board_structure", extension } } } } } } );
    PCB_SHAPE shape( nullptr, SHAPE_T::SEGMENT );
    shape.SetCustomProperty( wxS( "native" ), wxS( "authoritative" ) );
    shape.SetStartEnding( LINE_ENDING( LINE_ENDING_STYLE::CIRCLE, 300000, 200000 ) );
    BACKPLANE_DOCUMENT_METADATA metadata;
    metadata.Load( path );
    metadata.Apply( wxS( "shape" ), shape );
    BOOST_CHECK( shape.GetCustomProperties().at( wxS( "native" ) ) == wxS( "authoritative" ) );
    BOOST_CHECK( shape.GetCustomProperties().at( wxS( "legacy" ) ) == wxS( "migrated" ) );
    BOOST_CHECK( shape.GetStartEnding().GetStyle() == LINE_ENDING_STYLE::CIRCLE );
    BOOST_CHECK( shape.GetEndEnding().GetStyle() == LINE_ENDING_STYLE::ARROW );
    BOOST_CHECK_EQUAL( shape.GetEndEnding().GetLength(), 700000 );
    BOOST_CHECK( metadata.ReadExtension( wxS( "root" ), "board_structure" ) == extension );
    metadata.Save( path );
    const auto v2 = Read();
    BOOST_CHECK_EQUAL( v2.at( "version" ).get<int>(), 2 );
    BOOST_CHECK( !v2.contains( "items" ) );
    BOOST_CHECK( v2.at( "extensions" ).at( "root" ).at( "board_structure" ) == extension );
    BACKPLANE_DOCUMENT_METADATA reopened;
    reopened.Load( path );
    BOOST_CHECK( reopened.ReadExtension( wxS( "root" ), "board_structure" ) == extension );
    reopened.CaptureExtension( wxS( "root" ), "board_structure", nullptr );
    reopened.Save( path );
    BOOST_CHECK( !wxFileExists( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) ) );
}

BOOST_AUTO_TEST_CASE( ItemsOnlyCompanionIsDeletedAfterMigration )
{
    Write( { { "format", "backplane-kicad-metadata" }, { "version", 1 },
             { "items", { { "shape", { { "custom_properties", { { "legacy", "value" } } } } } } } } );
    BACKPLANE_DOCUMENT_METADATA metadata;
    metadata.Load( path );
    PCB_SHAPE shape( nullptr, SHAPE_T::SEGMENT );
    metadata.Apply( wxS( "shape" ), shape );
    BOOST_CHECK( shape.HasCustomProperties() );
    metadata.Save( path );
    BOOST_CHECK( !wxFileExists( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) ) );
}

BOOST_AUTO_TEST_CASE( EmptyV2RecordsDoNotLeaveCompanionsAndMalformedRecordsFail )
{
    Write( { { "format", "backplane-kicad-metadata" }, { "version", 2 },
             { "extensions", { { "root", nlohmann::json::object() } } } } );
    BACKPLANE_DOCUMENT_METADATA metadata;
    metadata.Load( path );
    metadata.Save( path );
    BOOST_CHECK( !wxFileExists( BACKPLANE_DOCUMENT_METADATA::CompanionPath( path ) ) );
    Write( { { "format", "backplane-kicad-metadata" }, { "version", 2 },
             { "extensions", { { "root", 123 } } } } );
    BOOST_CHECK_THROW( metadata.Load( path ), IO_ERROR );
}

BOOST_AUTO_TEST_SUITE_END()
