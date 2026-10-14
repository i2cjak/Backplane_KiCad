/* Geometry regression tests for the Backplane rigid-flex evaluator. */
#include <qa_utils/wx_utils/unit_test_utils.h>
#include <boost/test/unit_test.hpp>

#include <backplane_rigid_flex_geometry.h>

using namespace BACKPLANE_RIGID_FLEX;

BOOST_AUTO_TEST_SUITE( BackplaneRigidFlexGeometry )

BOOST_AUTO_TEST_CASE( CylindricalBendPreservesDevelopedLength )
{
    BEND_SPEC bend;
    bend.innerRadius = 2.0;
    bend.angleRadians = 3.14159265358979323846 / 2.0;

    const double width = bendStripWidth( bend, 0.2 );
    const VECTOR3D tangent = transformPoint( bend, 0.2, VECTOR3D( 0.0, width, 0.0 ) );
    const double neutralRadius = bendNeutralRadius( bend, 0.2 );

    BOOST_CHECK_CLOSE( tangent.y, neutralRadius, 1e-8 );
    BOOST_CHECK_CLOSE( tangent.z, neutralRadius, 1e-8 );
    BOOST_CHECK_SMALL( tangent.x, 1e-12 );

    // The downstream island is rigid: two points retain their separation.
    const VECTOR3D p1 = transformPoint( bend, 0.2, VECTOR3D( 0.0, width + 3.0, 0.0 ) );
    const VECTOR3D p2 = transformPoint( bend, 0.2, VECTOR3D( 4.0, width + 3.0, 0.0 ) );
    BOOST_CHECK_CLOSE( std::hypot( p2.x - p1.x, p2.y - p1.y ), 4.0, 1e-8 );
}

BOOST_AUTO_TEST_CASE( NegativeSideAndAngle )
{
    BEND_SPEC bend;
    bend.innerRadius = 1.0;
    bend.angleRadians = -3.14159265358979323846 / 2.0;
    bend.movingSide = -1;
    const double width = bendStripWidth( bend, 0.0 );
    const VECTOR3D result = transformPoint( bend, 0.0, VECTOR3D( 0.0, -width, 0.0 ) );
    BOOST_CHECK_CLOSE( result.y, -1.0, 1e-8 );
    BOOST_CHECK_CLOSE( result.z, -1.0, 1e-8 );
}

BOOST_AUTO_TEST_CASE( StiffenerUsesFullToleranceInterval )
{
    BOOST_CHECK( stiffenerFits( 0.14, 0.16, 0.02, 0.03, 0.12, 0.12, 0.28, 0.32 ) );
    BOOST_CHECK( !stiffenerFits( 0.14, 0.16, 0.02, 0.03, 0.10, 0.12, 0.28, 0.32 ) );
}

BOOST_AUTO_TEST_CASE( PartialFoldPreservesMaterialAndRotatesThickness )
{
    BEND_SPEC bend;
    bend.innerRadius = 2.0;
    bend.angleRadians = 3.14159265358979323846;
    bend.foldFraction = 0.5;
    const double width = bendStripWidth( bend, 0.2 );
    const VECTOR3D neutral = transformPoint( bend, 0.2, { 4.0, width, 0.0 } );
    // Halfway through a 180-degree bend the same strip forms a 90-degree arc
    // with twice the final neutral radius.
    BOOST_CHECK_CLOSE( neutral.x, 4.0, 1e-8 );
    BOOST_CHECK_CLOSE( neutral.y, 4.2, 1e-8 );
    BOOST_CHECK_CLOSE( neutral.z, 4.2, 1e-8 );
    const VECTOR3D surface = transformPoint( bend, 0.2, { 4.0, width, 0.1 } );
    BOOST_CHECK_CLOSE( surface.y, 4.1, 1e-8 );
    BOOST_CHECK_CLOSE( surface.z, 4.2, 1e-8 );
    double length = 0.0;
    VECTOR3D previous = transformPoint( bend, 0.2, { 0.0, 0.0, 0.0 } );
    for( int i = 1; i <= 10000; ++i )
    {
        const VECTOR3D p = transformPoint( bend, 0.2, { 0.0, width * i / 10000, 0.0 } );
        length += std::sqrt( (p.x-previous.x)*(p.x-previous.x)
                            + (p.y-previous.y)*(p.y-previous.y)
                            + (p.z-previous.z)*(p.z-previous.z) );
        previous = p;
    }
    BOOST_CHECK_CLOSE( length, width, 1e-5 );
    bend.foldFraction = 0.0;
    const VECTOR3D flat = transformPoint( bend, 0.2, { 4.0, width, 0.1 } );
    BOOST_CHECK_CLOSE( flat.y, width, 1e-8 );
    BOOST_CHECK_CLOSE( flat.z, 0.1, 1e-8 );
}

BOOST_AUTO_TEST_CASE( RejectInvalidPhysicalInputs )
{
    BEND_SPEC bend;
    bend.innerRadius = 2.0;
    BOOST_CHECK( validBend( bend ) );
    bend.axis.direction.x = std::numeric_limits<double>::infinity();
    BOOST_CHECK( !validBend( bend ) );
    BOOST_CHECK( !stiffenerFits( 0.16, 0.14, 0.02, 0.03, 0.12, 0.12, 0.28, 0.32 ) );
    BOOST_CHECK( !stiffenerFits( -0.1, 0.14, 0.02, 0.03, 0.12, 0.12, 0.0, 1.0 ) );
}

BOOST_AUTO_TEST_CASE( InvalidBendTransformsAreSafeIdentity )
{
    BEND_SPEC bend;
    bend.innerRadius = 0.0;
    const VECTOR3D point{ 3.0, 4.0, 0.2 };

    // Exporters may encounter an incomplete staged bend.  The geometry
    // primitive must leave the point unchanged instead of producing NaNs.
    BOOST_CHECK( !validBend( bend ) );
    const VECTOR3D result = transformPoint( bend, 0.2, point );
    BOOST_CHECK_CLOSE( result.x, point.x, 1e-12 );
    BOOST_CHECK_CLOSE( result.y, point.y, 1e-12 );
    BOOST_CHECK_CLOSE( result.z, point.z, 1e-12 );

    bend.innerRadius = 2.0;
    bend.axis.direction = { 0.0, 0.0 };
    BOOST_CHECK( !validBend( bend ) );
    BOOST_CHECK( transformPoint( bend, 0.2, point ) == point );
}

BOOST_AUTO_TEST_CASE( SequentialParallelBendsTransportTheHingeFrame )
{
    BEND_SPEC first;
    first.innerRadius = 2.0;
    first.angleRadians = 3.14159265358979323846 / 2.0;
    first.axis.origin = { 0.0, 0.0 };
    first.axis.direction = { 20.0, 0.0 };

    BEND_SPEC second = first;
    second.axis.origin = { 0.0, 10.0 };
    second.angleRadians = 3.14159265358979323846 / 2.0;

    const std::vector<BEND_SPEC> sequence = { first, second };
    const double width = bendStripWidth( first, 0.0 );

    // The island between hinges receives the first bend and remains fixed on
    // the second hinge's original developed side.
    const VECTOR3D middle = transformPointSequence( sequence, 0.0, { 5.0, 5.0, 0.0 } );
    BOOST_CHECK_CLOSE( middle.y, 2.0, 1e-8 );
    BOOST_CHECK_CLOSE( middle.z, 3.858407346410207, 1e-8 );

    // The downstream island receives both folds.  The second hinge axis is
    // transported onto the first folded tangent plane rather than reused in
    // the original XY plane.
    const VECTOR3D downstream = transformPointSequence( sequence, 0.0, { 5.0, 15.0, 0.0 } );
    BOOST_CHECK_CLOSE( downstream.y, -1.858407346410207, 1e-8 );
    BOOST_CHECK_CLOSE( downstream.z, 10.858407346410207, 1e-8 );

    const VECTOR3D fixed = transformPointSequence( sequence, 0.0, { 5.0, -2.0, 0.0 } );
    BOOST_CHECK_CLOSE( fixed.y, -2.0, 1e-8 );
    BOOST_CHECK_CLOSE( fixed.z, 0.0, 1e-8 );
    BOOST_CHECK( width > 0.0 );
}

BOOST_AUTO_TEST_CASE( SequentialOppositeSignBendsCompose )
{
    BEND_SPEC first;
    first.innerRadius = 1.5;
    first.angleRadians = 3.14159265358979323846 / 2.0;
    first.axis.origin = { 0.0, 0.0 };
    first.axis.direction = { 20.0, 0.0 };
    BEND_SPEC second = first;
    second.axis.origin = { 0.0, 8.0 };
    second.angleRadians = -3.14159265358979323846 / 2.0;

    const VECTOR3D result = transformPointSequence( { first, second }, 0.0, { 4.0, 13.0, 0.0 } );
    // Opposite signs return the downstream tangent toward the first folded
    // island while retaining the developed distance between hinges.
    BOOST_CHECK_CLOSE( result.y, 5.643805509807655, 1e-8 );
    BOOST_CHECK_CLOSE( result.z, 8.643805509807655, 1e-8 );
}

BOOST_AUTO_TEST_CASE( NeutralOffsetPlacesTheInnerSurfaceOnTheSpecifiedRadius )
{
    BEND_SPEC positive;
    positive.innerRadius = 2.0;
    positive.neutralAxisOffset = 0.1;
    positive.angleRadians = M_PI / 2.0;
    positive.axis.direction = { 20.0, 0.0 };
    const double width = bendStripWidth( positive, 0.4 );

    // The mechanical datum is the stack centre.  For a positive bend the
    // inner surface is the +Z half of the centred stack.
    const VECTOR3D innerPositive = transformPoint( positive, 0.4,
                                                    { 0.0, width, 0.2 } );
    BOOST_CHECK_CLOSE( innerPositive.y, 2.0, 1e-8 );

    BEND_SPEC negative = positive;
    negative.angleRadians = -M_PI / 2.0;
    const VECTOR3D innerNegative = transformPoint( negative, 0.4,
                                                    { 0.0, bendStripWidth( negative, 0.4 ),
                                                      -0.2 } );
    BOOST_CHECK_CLOSE( innerNegative.y, 2.0, 1e-8 );
}

BOOST_AUTO_TEST_CASE( ShiftedAxisDatumAndSingleSequenceAgree )
{
    BEND_SPEC bend;
    bend.innerRadius = 1.7;
    bend.neutralAxisOffset = 0.1;
    bend.angleRadians = M_PI / 2.0;
    bend.axis.origin = { 2.0, -1.0 };
    bend.axis.direction = { 8.0, 3.0 };
    bend.axis.z = 4.25;
    const VECTOR3D point{ 4.0, 2.0, 4.45 };
    const VECTOR3D single = transformPoint( bend, 0.4, point );
    const VECTOR3D sequence = transformPointSequence( std::vector<BEND_SPEC>{ bend }, 0.4, point );
    BOOST_CHECK_CLOSE( sequence.x, single.x, 1e-10 );
    BOOST_CHECK_CLOSE( sequence.y, single.y, 1e-10 );
    BOOST_CHECK_CLOSE( sequence.z, single.z, 1e-10 );

    const double width = bendStripWidth( bend, 0.4 );
    const double neutralZ = bendNeutralZ( bend, 0.4 );
    const double directionLength = std::hypot( bend.axis.direction.x, bend.axis.direction.y );
    const double nx = -bend.axis.direction.y / directionLength;
    const double ny = bend.axis.direction.x / directionLength;
    const VECTOR3D flatInner{ bend.axis.origin.x + nx * width,
                              bend.axis.origin.y + ny * width, bend.axis.z + 0.2 };
    const VECTOR3D mappedInner = transformPoint( bend, 0.4, flatInner );
    BOOST_CHECK_CLOSE( ( mappedInner.x - bend.axis.origin.x ) * nx
                       + ( mappedInner.y - bend.axis.origin.y ) * ny,
                       bend.innerRadius, 1e-8 );
    BOOST_CHECK_CLOSE( neutralZ, 4.35, 1e-8 );
}

BOOST_AUTO_TEST_CASE( SequentialNeutralPlanesMeetAtTransportedHinge )
{
    BEND_SPEC first;
    first.innerRadius = 2.0;
    first.neutralAxisOffset = 0.1;
    first.angleRadians = M_PI / 2.0;
    first.axis.direction = { 20.0, 0.0 };
    BEND_SPEC second = first;
    second.axis.origin = { 0.0, 10.0 };
    second.axis.z = 0.0;

    const std::vector<BEND_SPEC> sequence = { first, second };
    const VECTOR3D hinge = { 5.0, 10.0, bendNeutralZ( first, 0.4 ) };
    const VECTOR3D beforeSecond = transformPoint( first, 0.4, hinge );
    const VECTOR3D afterSequence = transformPointSequence( sequence, 0.4, hinge );
    BOOST_CHECK_CLOSE( afterSequence.x, beforeSecond.x, 1e-8 );
    BOOST_CHECK_CLOSE( afterSequence.y, beforeSecond.y, 1e-8 );
    BOOST_CHECK_CLOSE( afterSequence.z, beforeSecond.z, 1e-8 );
}

BOOST_AUTO_TEST_CASE( NeutralAxisOverrideControlsDevelopedLength )
{
    BEND_SPEC bend;
    bend.innerRadius = 2.0;
    bend.neutralAxisOffset = 1.0;
    bend.angleRadians = 3.14159265358979323846 / 2.0;

    // A supplied neutral axis is measured from the inner surface and takes
    // precedence over the homogeneous t/2 approximation.
    BOOST_CHECK_CLOSE( bendNeutralRadius( bend, 0.2 ), 3.0, 1e-12 );
    BOOST_CHECK_CLOSE( bendStripWidth( bend, 0.2 ), 3.0 * 3.14159265358979323846 / 2.0,
                       1e-12 );
    BOOST_CHECK( !validBend( bend, -0.001 ) );
}

BOOST_AUTO_TEST_SUITE_END()
