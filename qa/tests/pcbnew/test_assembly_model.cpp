#include <boost/test/unit_test.hpp>

#include <assembly/assembly_model.h>

#include <fstream>
#include <filesystem>
#include <cmath>

BOOST_AUTO_TEST_SUITE( BackplaneAssembly )

BOOST_AUTO_TEST_CASE( ManifestRoundTripAndValidation )
{
    ASSEMBLY_MANIFEST m;
    m.name = "two boards"; m.anchor = "main";
    ASSEMBLY_INSTANCE a; a.id = "main"; a.source = "main.kicad_pcb"; a.bounds = { { 0, 0, 0 }, { 10, 10, 1 }, true };
    ASSEMBLY_INSTANCE b; b.id = "daughter"; b.source = "daughter.step"; b.kind = "part";
    b.transform.translation = { 9.5, 0, 0 }; b.bounds = { { 0, 0, 0 }, { 10, 10, 1 }, true };
    m.instances = { a, b };
    m.mates.push_back( { "m1", "point", "main", "daughter", { 1, 2, 3 }, { 4, 5, 6 }, 0.2 } );
    const ASSEMBLY_MANIFEST copy = ASSEMBLY_MANIFEST::FromJson( m.ToJson() );
    BOOST_REQUIRE( copy.Validate().empty() );
    BOOST_CHECK_EQUAL( copy.instances[1].kind, "part" );
    BOOST_CHECK_EQUAL( CheckAssemblyBounds( copy, 0.0 ).size(), 1U );
}

BOOST_AUTO_TEST_CASE( BadReferencesAreReported )
{
    ASSEMBLY_MANIFEST m; m.anchor = "missing";
    m.instances.push_back( { "a", "x.step" } );
    m.mates.push_back( { "m", "axis", "a", "b" } );
    const auto errors = m.Validate();
    BOOST_CHECK( errors.size() >= 2 );
}

BOOST_AUTO_TEST_CASE( PointMateMovesSecondInstance )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    m.mates.push_back( { "join", "point", "a", "b", { 10, 20, 3 }, { 1, 2, 0 } } );
    BOOST_REQUIRE( ApplyAssemblyMates( m ) );
    BOOST_CHECK_CLOSE( m.instances[1].transform.translation[0], 9.0, 1e-9 );
    BOOST_CHECK_CLOSE( m.instances[1].transform.translation[1], 18.0, 1e-9 );
    BOOST_CHECK_CLOSE( m.instances[1].transform.translation[2], 3.0, 1e-9 );
}

BOOST_AUTO_TEST_CASE( PointMateUsesWorldCoordinatesAndIsIdempotent )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    m.instances[0].transform.translation = { 50, 20, 0 };
    m.instances[0].transform.rotation = { 0, 0, 90 };
    m.instances[1].transform.translation = { -100, -100, 50 };
    m.instances[1].transform.rotation = { 0, 0, 90 };
    m.mates.push_back( { "join", "point", "a", "b", { 10, 0, 0 }, { 2, 0, 0 } } );
    BOOST_REQUIRE( ApplyAssemblyMates( m ) );
    BOOST_CHECK_CLOSE( m.instances[1].transform.translation[0], 50.0, 1e-9 );
    BOOST_CHECK_CLOSE( m.instances[1].transform.translation[1], 28.0, 1e-9 );
    const auto firstResult = m.ToJson();
    BOOST_REQUIRE( ApplyAssemblyMates( m ) );
    BOOST_CHECK( m.ToJson() == firstResult );
}

BOOST_AUTO_TEST_CASE( CyclicMatesAreRejected )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    m.mates = { { "ab", "point", "a", "b" }, { "ba", "point", "b", "a" } };
    BOOST_CHECK( !ApplyAssemblyMates( m ) );
}

BOOST_AUTO_TEST_CASE( AxisMateAlignsAndRejectsZeroAxis )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    ASSEMBLY_MATE mate{ "axis", "axis", "a", "b" };
    mate.first_axis = { 0, 0, 1 }; mate.second_axis = { 1, 0, 0 };
    m.mates = { mate };
    BOOST_REQUIRE( ApplyAssemblyMates( m ) );
    BOOST_CHECK_CLOSE( std::abs( m.instances[1].transform.rotation[1] ), 90.0, 1e-6 );

    mate.second_axis = { 0, 0, 0 }; m.mates = { mate };
    BOOST_CHECK( !ApplyAssemblyMates( m ) );
}

BOOST_AUTO_TEST_CASE( AxisMateAntiparallelIsStable )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    ASSEMBLY_MATE mate{ "axis", "axis", "a", "b" };
    mate.first_axis = { 0, 0, 1 }; mate.second_axis = { 0, 0, -1 };
    m.mates = { mate };
    BOOST_REQUIRE( ApplyAssemblyMates( m ) );
    BOOST_CHECK( std::isfinite( m.instances[1].transform.rotation[0] ) );
    BOOST_CHECK( std::isfinite( m.instances[1].transform.rotation[1] ) );
    BOOST_CHECK( std::isfinite( m.instances[1].transform.rotation[2] ) );
}

BOOST_AUTO_TEST_CASE( NestedAssemblyFlattensWithQualifiedIdsAndRelativeSources )
{
    const auto root = std::filesystem::temp_directory_path() / "kicad-assembly-nested-test";
    std::filesystem::create_directories( root / "sub" );
    std::ofstream( root / "sub" / "child.backplane-assembly.json" )
            << R"({"schema":1,"instances":[{"id":"leaf","source":"leaf.step"}]})";
    ASSEMBLY_MANIFEST m; m.base_directory = root;
    m.instances.push_back( { "module", "sub/child.backplane-assembly.json", "assembly" } );
    const auto flat = FlattenAssembly( m );
    BOOST_REQUIRE_EQUAL( flat.instances.size(), 1U );
    BOOST_CHECK_EQUAL( flat.instances[0].id, "module/leaf" );
    BOOST_CHECK( std::filesystem::path( flat.instances[0].source ).is_absolute() );
    std::error_code ec; std::filesystem::remove_all( root, ec );
}

BOOST_AUTO_TEST_CASE( UnknownAssemblyKeysSurviveRoundTrip )
{
    nlohmann::json input = { { "schema", 1 }, { "future", 42 },
                             { "electrical", { { "links", nlohmann::json::array() } } },
                             { "instances", nlohmann::json::array( { { { "id", "a" },
                                                                         { "source", "a.step" },
                                                                         { "vendor_key", "kept" } } } ) },
                             { "mates", nlohmann::json::array() } };
    const auto output = ASSEMBLY_MANIFEST::FromJson( input ).ToJson();
    BOOST_CHECK_EQUAL( output.at( "future" ), 42 );
    BOOST_CHECK_EQUAL( output.at( "instances" ).at( 0 ).at( "vendor_key" ), "kept" );
    BOOST_CHECK( output.contains( "electrical" ) );
}


namespace
{
std::array<double, 3> transformedVector( std::array<double, 3> p,
                                        const std::array<double, 3>& rotation )
{
    const double radians = std::acos( -1.0 ) / 180.0;
    for( int axis = 0; axis < 3; ++axis )
    {
        int a = ( axis + 1 ) % 3, b = ( axis + 2 ) % 3;
        const double c = std::cos( rotation[axis] * radians );
        const double q = std::sin( rotation[axis] * radians );
        const double oldA = p[a], oldB = p[b];
        p[a] = c * oldA - q * oldB;
        p[b] = q * oldA + c * oldB;
    }
    return p;
}
}

BOOST_AUTO_TEST_CASE( AxisMateUsesBothRotatedFramesAndIsIdempotent )
{
    ASSEMBLY_MANIFEST m;
    m.instances = { { "a", "a.step" }, { "b", "b.step" } };
    m.instances[0].transform = { { 5, 7, 9 }, { 23, 41, 67 } };
    m.instances[1].transform = { { -3, 2, 8 }, { -19, 52, 11 } };
    ASSEMBLY_MATE mate{ "axis", "axis", "a", "b", { 3, 4, 5 }, { 8, 2, 1 } };
    mate.first_axis = { 0, 1, 0 }; mate.second_axis = { 0, 0, 1 }; mate.opposing = true;
    m.mates = { mate };
    for( int repeat = 0; repeat < 3; ++repeat )
    {
        BOOST_REQUIRE( ApplyAssemblyMates( m ) );
        const auto firstAxis = transformedVector( mate.first_axis, m.instances[0].transform.rotation );
        const auto secondAxis = transformedVector( mate.second_axis, m.instances[1].transform.rotation );
        const auto firstPoint = transformedVector( mate.first, m.instances[0].transform.rotation );
        const auto secondPoint = transformedVector( mate.second, m.instances[1].transform.rotation );
        for( int k = 0; k < 3; ++k )
        {
            BOOST_CHECK_SMALL( firstAxis[k] + secondAxis[k], 1e-8 );
            BOOST_CHECK_SMALL( firstPoint[k] + m.instances[0].transform.translation[k]
                               - secondPoint[k] - m.instances[1].transform.translation[k], 1e-8 );
        }
    }
}

BOOST_AUTO_TEST_CASE( NestedRotationCompositionAndCycles )
{
    const auto root = std::filesystem::temp_directory_path() / "kicad-assembly-transform-test";
    std::filesystem::create_directories( root / "sub" );
    ASSEMBLY_MANIFEST child;
    child.base_directory = root / "sub";
    child.instances = { { "leaf", "leaf.step", "part" } };
    child.instances[0].transform = { { 1, 2, 3 }, { 33, -24, 18 } };
    child.Save( root / "sub/child.json" );
    ASSEMBLY_MANIFEST parent;
    parent.base_directory = root;
    parent.instances = { { "module", "sub/child.json", "assembly" } };
    parent.instances[0].transform = { { 7, 9, 11 }, { 42, 53, -27 } };
    const auto flat = FlattenAssembly( parent );
    BOOST_REQUIRE_EQUAL( flat.instances.size(), 1U );
    BOOST_CHECK_EQUAL( flat.instances[0].source, ( root / "sub/leaf.step" ).string() );
    const std::array<double, 3> p{ 2, 5, 8 };
    auto expected = transformedVector( p, child.instances[0].transform.rotation );
    for( int k = 0; k < 3; ++k ) expected[k] += child.instances[0].transform.translation[k];
    expected = transformedVector( expected, parent.instances[0].transform.rotation );
    auto actual = transformedVector( p, flat.instances[0].transform.rotation );
    for( int k = 0; k < 3; ++k )
        BOOST_CHECK_SMALL( actual[k] + flat.instances[0].transform.translation[k]
                           - expected[k] - parent.instances[0].transform.translation[k], 1e-8 );
    child.instances = { { "cycle", "../sub/child.json", "assembly" } };
    child.Save( root / "sub/child.json" );
    BOOST_CHECK_THROW( FlattenAssembly( parent ), std::invalid_argument );
    std::error_code ec;
    std::filesystem::remove_all( root, ec );
}

BOOST_AUTO_TEST_SUITE_END()
