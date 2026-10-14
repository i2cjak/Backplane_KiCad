#include "assembly_exporter.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_Editor.hxx>
#include <TDF_LabelDataMap.hxx>
#include <TDF_LabelSequence.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <TDataStd_Name.hxx>
#include <TCollection_ExtendedString.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <reporter.h>

#include <cmath>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

namespace
{
struct TEMP_DIR
{
    std::filesystem::path path;
    ~TEMP_DIR() { std::error_code ec; std::filesystem::remove_all( path, ec ); }
};
bool ReadShape( const std::filesystem::path& path, TopoDS_Shape& shape )
{
    STEPControl_Reader reader;
    if( reader.ReadFile( path.string().c_str() ) != IFSelect_RetDone ) return false;
    reader.TransferRoots(); shape = reader.OneShape(); return !shape.IsNull();
}
gp_Trsf InstanceTransform( const ASSEMBLY_INSTANCE& i )
{
    constexpr double p = 3.14159265358979323846 / 180.0;
    gp_Trsf t; t.SetTranslation( gp_Vec( i.transform.translation[0], i.transform.translation[1], i.transform.translation[2] ) );
    gp_Trsf r; r.SetRotation( gp_Ax1( gp_Pnt(), gp_Dir( 0, 0, 1 ) ), i.transform.rotation[2] * p );
    t = t * r; r.SetRotation( gp_Ax1( gp_Pnt(), gp_Dir( 0, 1, 0 ) ), i.transform.rotation[1] * p ); t = t * r;
    r.SetRotation( gp_Ax1( gp_Pnt(), gp_Dir( 1, 0, 0 ) ), i.transform.rotation[0] * p ); return t * r;
}
}

bool ASSEMBLY_EXPORTER::Export( const ASSEMBLY_MANIFEST& manifest, const std::string& output,
                                REPORTER* reporter, const ASSEMBLY_BOARD_EXPORTER& boardExporter )
{
    ASSEMBLY_MANIFEST working = FlattenAssembly( manifest );
    if( !working.Validate( true ).empty() ) return false;
    Handle( TDocStd_Document ) destination = new TDocStd_Document( "BackplaneAssembly" );
    Handle( XCAFDoc_ShapeTool ) destinationShapes = XCAFDoc_DocumentTool::ShapeTool( destination->Main() );
    const TDF_Label assemblyLabel = destinationShapes->NewShape();
    TDataStd_Name::Set( assemblyLabel, TCollection_ExtendedString( manifest.name.c_str() ) );
    TEMP_DIR temp{ std::filesystem::temp_directory_path() /
                         ( "backplane-assembly-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) ) };
    std::filesystem::create_directories( temp.path );
    for( const auto& instance : working.instances )
    {
        std::filesystem::path source = instance.source;
        if( !std::filesystem::exists( source ) ) source = instance.source;
        if( instance.kind == "board" && boardExporter )
        {
            source = temp.path / ( std::to_string( &instance - working.instances.data() ) + ".step" );
            if( !boardExporter( instance, source.string(), reporter ) ) return false;
        }
        Handle( TDocStd_Document ) sourceDocument = new TDocStd_Document( "AssemblySource" );
        STEPCAFControl_Reader reader;
        reader.SetColorMode( true ); reader.SetNameMode( true );
        if( reader.ReadFile( source.string().c_str() ) != IFSelect_RetDone || !reader.Transfer( sourceDocument ) ) return false;
        Handle( XCAFDoc_ShapeTool ) sourceShapes = XCAFDoc_DocumentTool::ShapeTool( sourceDocument->Main() );
        TDF_LabelSequence roots;
        sourceShapes->GetFreeShapes( roots );
        if( roots.IsEmpty() ) return false;
        for( Standard_Integer root = 1; root <= roots.Length(); ++root )
        {
            TDF_LabelDataMap copied;
            const TDF_Label label = XCAFDoc_Editor::CloneShapeLabel(
                    roots.Value( root ), sourceShapes, destinationShapes, copied );
            if( label.IsNull() ) return false;
            for( TDF_LabelDataMap::Iterator entry( copied ); entry.More(); entry.Next() )
                XCAFDoc_Editor::CloneMetaData( entry.Key(), entry.Value(), nullptr,
                                               true, true, true, false, true );
            const TDF_Label component = destinationShapes->AddComponent(
                    assemblyLabel, label, TopLoc_Location( InstanceTransform( instance ) ) );
            TDataStd_Name::Set( component, TCollection_ExtendedString( instance.id.c_str() ) );
        }
    }
    destinationShapes->UpdateAssemblies();
    STEPCAFControl_Writer writer;
    writer.SetColorMode( true ); writer.SetNameMode( true );
    if( !writer.Transfer( destination, STEPControl_AsIs ) ) return false;
    return writer.Write( output.c_str() ) == IFSelect_RetDone;
}

std::vector<ASSEMBLY_COLLISION> ASSEMBLY_EXPORTER::CheckCollisions( const ASSEMBLY_MANIFEST& manifest,
                                                                     REPORTER* reporter,
                                                                     const ASSEMBLY_BOARD_EXPORTER& boardExporter )
{
    std::vector<ASSEMBLY_COLLISION> result; ASSEMBLY_MANIFEST working = FlattenAssembly( manifest );
    const auto errors = working.Validate( true );
    if( !errors.empty() ) throw std::runtime_error( errors.front() );
    TEMP_DIR temp{ std::filesystem::temp_directory_path() / ( "backplane-assembly-check-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) ) };
    std::filesystem::create_directories( temp.path );
    std::vector<TopoDS_Shape> shapes; shapes.reserve( working.instances.size() );
    for( const auto& i : working.instances )
    {
        std::filesystem::path source = i.source;
        if( i.kind == "board" && boardExporter ) { source = temp.path / ( std::to_string( &i - working.instances.data() ) + ".step" ); if( !boardExporter( i, source.string(), reporter ) ) throw std::runtime_error( "Failed to export assembly board: " + i.id ); }
        TopoDS_Shape shape; if( !ReadShape( source, shape ) ) throw std::runtime_error( "Failed to read assembly geometry: " + source.string() );
        shapes.push_back( BRepBuilderAPI_Transform( shape, InstanceTransform( i ), true ).Shape() );
    }
    for( size_t a = 0; a < shapes.size(); ++a ) for( size_t b = a + 1; b < shapes.size(); ++b )
    {
        BRepExtrema_DistShapeShape distance( shapes[a], shapes[b] );
        if( !distance.IsDone() ) throw std::runtime_error( "Assembly distance calculation failed" );
        const double d = distance.Value();
        double requiredClearance = 0.0;
        for( const auto& mate : working.resolved_clearances )
            if( ( mate.first == working.instances[a].id && mate.second == working.instances[b].id )
                || ( mate.second == working.instances[a].id && mate.first == working.instances[b].id ) )
                requiredClearance = std::max( requiredClearance, mate.minimum_mm );
        bool collision = d <= requiredClearance + 1e-6;
        if( !collision ) { TopoDS_Shape common = BRepAlgoAPI_Common( shapes[a], shapes[b] ).Shape(); collision = !common.IsNull() && TopExp_Explorer( common, TopAbs_SOLID ).More(); }
        if( collision ) result.push_back( { working.instances[a].id, working.instances[b].id, d } );
    }
    return result;
}
