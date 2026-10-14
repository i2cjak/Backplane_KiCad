/* SPDX-License-Identifier: GPL-3.0-or-later
 * Geometry primitives shared by rigid-flex preview and exporters.  The input
 * coordinates are the board's flat XY plane in millimetres; Z is the local
 * stack direction.  This file deliberately has no wx or PCB dependencies so
 * the exact same evaluator can be used by headless exporters and tests.
 */
#ifndef BACKPLANE_RIGID_FLEX_GEOMETRY_H
#define BACKPLANE_RIGID_FLEX_GEOMETRY_H

#include <cmath>
#include <algorithm>
#include <limits>
#include <vector>

#include <math/vector2d.h>
#include <math/vector3.h>

namespace BACKPLANE_RIGID_FLEX
{

struct BEND_AXIS
{
    VECTOR2D origin{};
    VECTOR2D direction{ 1.0, 0.0 };
    // Z datum of the flat surface carrying this bend.  Zero preserves the
    // original mechanical-frame convention.
    double z = 0.0;
};

struct BEND_SPEC
{
    BEND_AXIS axis;
    double innerRadius = 0.0;
    // NaN selects the homogeneous approximation (inner radius + t/2).
    // A finite value is the fabricator supplied neutral-axis offset from the
    // inner surface and replaces that approximation.
    double neutralAxisOffset = std::numeric_limits<double>::quiet_NaN();
    double angleRadians = 0.0;
    // Animation changes curvature while retaining the target strip length.
    double foldFraction = 1.0;
    // The side of the axis containing the bend strip (+1 or -1).
    int movingSide = 1;
};

inline double bendNeutralRadius( const BEND_SPEC& aBend, double aThickness )
{
    return aBend.innerRadius + ( std::isfinite( aBend.neutralAxisOffset )
                                    ? aBend.neutralAxisOffset : aThickness * 0.5 );
}

inline double bendStripWidth( const BEND_SPEC& aBend, double aThickness )
{
    return bendNeutralRadius( aBend, aThickness ) * std::abs( aBend.angleRadians );
}

inline double bendNeutralZ( const BEND_SPEC& aBend, double aThickness )
{
    if( !std::isfinite( aBend.neutralAxisOffset ) )
        return aBend.axis.z;
    const double offsetFromCenter = aThickness * 0.5 - aBend.neutralAxisOffset;
    return aBend.axis.z + ( aBend.angleRadians < 0.0 ? -offsetFromCenter : offsetFromCenter );
}

inline bool validBend( const BEND_SPEC& aBend, double aThickness = 0.0 )
{
    const double d = std::hypot( aBend.axis.direction.x, aBend.axis.direction.y );
    return std::isfinite( d ) && d > std::numeric_limits<double>::epsilon()
        && std::isfinite( aBend.axis.origin.x ) && std::isfinite( aBend.axis.origin.y )
        && std::isfinite( aBend.axis.z )
        && std::isfinite( aThickness ) && aThickness >= 0.0
        && std::isfinite( aBend.innerRadius ) && aBend.innerRadius > 0.0
        && ( std::isnan( aBend.neutralAxisOffset )
             || ( std::isfinite( aBend.neutralAxisOffset ) && aBend.neutralAxisOffset >= 0.0 ) )
        && bendNeutralRadius( aBend, aThickness ) > 0.0
        && std::isfinite( aBend.angleRadians )
        && std::abs( aBend.angleRadians ) <= 2.0 * 3.14159265358979323846
        && std::isfinite( aBend.foldFraction ) && aBend.foldFraction >= 0.0
        && aBend.foldFraction <= 1.0
        && ( aBend.movingSide == 1 || aBend.movingSide == -1 );
}

/** Map a flat point through a single cylindrical bend.
 *
 * Points between the two tangent lines are developed onto the cylinder.  A
 * point beyond the moving tangent is carried by the rigid downstream patch.
 * Points on the fixed side stay unchanged.  The returned Z includes the
 * local stack coordinate and therefore works for both surfaces of a board.
 */
inline VECTOR3D transformPoint( const BEND_SPEC& aBend, double aThickness,
                                const VECTOR3D& aPoint )
{
    if( !validBend( aBend, aThickness ) || aBend.angleRadians == 0.0
        || aBend.foldFraction == 0.0 )
        return aPoint;
    const double dn = std::hypot( aBend.axis.direction.x, aBend.axis.direction.y );
    const double tx = aBend.axis.direction.x / dn;
    const double ty = aBend.axis.direction.y / dn;
    const double nx = -ty * aBend.movingSide;
    const double ny = tx * aBend.movingSide;
    const double dx = aPoint.x - aBend.axis.origin.x;
    const double dy = aPoint.y - aBend.axis.origin.y;
    const double along = dx * tx + dy * ty;
    const double side = dx * nx + dy * ny;
    if( side <= 0.0 )
        return aPoint;
    const double width = bendStripWidth( aBend, aThickness );
    const double k = aBend.angleRadians * aBend.foldFraction / width;
    const double u = std::min( side, width );
    const double q = k * u;
    // Stable evaluation of (1-cos(q))/k near the flat state.
    const double radial = std::abs( q ) < 1e-8 ? u * ( 1.0 - q*q/6.0 ) : std::sin( q ) / k;
    const double lift = 2.0 * std::sin( q/2.0 ) * std::sin( q/2.0 ) / k;
    const double extra = side - u;
    const double neutralZ = bendNeutralZ( aBend, aThickness );
    const double localZ = aPoint.z - neutralZ;
    const double outSide = radial + extra * std::cos( q ) - localZ * std::sin( q );
    const double outZ = neutralZ + lift + extra * std::sin( q ) + localZ * std::cos( q );
    return VECTOR3D( aBend.axis.origin.x + along * tx + outSide * nx,
                     aBend.axis.origin.y + along * ty + outSide * ny, outZ );
}

// Apply a developed bend sequence.  The side tests are intentionally made in
// the original XY frame.  For each subsequent hinge its axis and local frame
// are transported through all preceding rigid/cylindrical mappings; using the
// already deformed point with the original 2D axis would classify the second
// hinge incorrectly.
inline VECTOR3D transformPointSequence( const std::vector<BEND_SPEC>& aBends,
                                        double aThickness, const VECTOR3D& aPoint )
{
    if( aBends.empty() )
        return aPoint;

    auto flatNormal = []( const BEND_SPEC& bend )
    {
        const double d = std::hypot( bend.axis.direction.x, bend.axis.direction.y );
        const double tx = bend.axis.direction.x / d;
        const double ty = bend.axis.direction.y / d;
        return VECTOR2D( -ty * bend.movingSide, tx * bend.movingSide );
    };

    struct LOCAL_FRAME
    {
        VECTOR3D origin{}, t{}, n{}, z{};
        bool valid = false;
    };
    std::vector<LOCAL_FRAME> frames;
    frames.reserve( aBends.size() );

    auto apply = [&]( size_t index, const VECTOR3D& input, const VECTOR3D& flatInput )
    {
        const BEND_SPEC& bend = aBends[index];
        const LOCAL_FRAME& frame = frames[index];
        if( !frame.valid || !validBend( bend, aThickness ) || bend.angleRadians == 0.0
            || bend.foldFraction == 0.0 )
            return input;
        const VECTOR2D n2 = flatNormal( bend );
        const double side = ( flatInput.x - bend.axis.origin.x ) * n2.x
                          + ( flatInput.y - bend.axis.origin.y ) * n2.y;
        if( side <= 0.0 )
            return input;
        const double width = bendStripWidth( bend, aThickness );
        const VECTOR3D relative( input.x - frame.origin.x, input.y - frame.origin.y,
                                 input.z - frame.origin.z );
        const double along = relative.x*frame.t.x + relative.y*frame.t.y + relative.z*frame.t.z;
        const double zCurrent = relative.x*frame.z.x + relative.y*frame.z.y + relative.z*frame.z.z;
        const double k = bend.angleRadians * bend.foldFraction / width;
        const double u = std::min( side, width );
        const double q = k * u;
        const double radial = std::abs( q ) < 1e-8 ? u * ( 1.0 - q*q/6.0 ) : std::sin( q ) / k;
        const double lift = 2.0 * std::sin( q/2.0 ) * std::sin( q/2.0 ) / k;
        const double extra = side - u;
        const double outSide = radial + extra * std::cos( q ) - zCurrent * std::sin( q );
        const double outZ = lift + extra * std::sin( q ) + zCurrent * std::cos( q );
        return VECTOR3D( frame.origin.x + along*frame.t.x + outSide*frame.n.x + outZ*frame.z.x,
                         frame.origin.y + along*frame.t.y + outSide*frame.n.y + outZ*frame.z.y,
                         frame.origin.z + along*frame.t.z + outSide*frame.n.z + outZ*frame.z.z );
    };

    for( size_t i = 0; i < aBends.size(); ++i )
    {
        const BEND_SPEC& bend = aBends[i];
        LOCAL_FRAME frame;
        if( validBend( bend, aThickness ) && bend.angleRadians != 0.0
            && bend.foldFraction != 0.0 )
        {
            const double d = std::hypot( bend.axis.direction.x, bend.axis.direction.y );
            const double tx = bend.axis.direction.x / d;
            const double ty = bend.axis.direction.y / d;
            const VECTOR2D n2 = flatNormal( bend );
            const double neutralZ = bendNeutralZ( bend, aThickness );
            VECTOR3D axisOrigin( bend.axis.origin.x, bend.axis.origin.y, neutralZ );
            VECTOR3D axisEnd( bend.axis.origin.x + tx, bend.axis.origin.y + ty, neutralZ );
            // Use a local differential sample so a nearby earlier strip cannot
            // pull the transported normal across a different tangent frame.
            constexpr double referenceDistance = 1.0e-4;
            VECTOR3D normalPoint( bend.axis.origin.x + n2.x * referenceDistance,
                                  bend.axis.origin.y + n2.y * referenceDistance, neutralZ );
            VECTOR3D zPoint( bend.axis.origin.x, bend.axis.origin.y, neutralZ + 1.0 );
            for( size_t j = 0; j < i; ++j )
            {
                axisOrigin = apply( j, axisOrigin, VECTOR3D( bend.axis.origin.x,
                                                             bend.axis.origin.y, neutralZ ) );
                axisEnd = apply( j, axisEnd, VECTOR3D( bend.axis.origin.x + tx,
                                                        bend.axis.origin.y + ty, neutralZ ) );
                normalPoint = apply( j, normalPoint, VECTOR3D( bend.axis.origin.x
                                                                + n2.x * referenceDistance,
                                                                bend.axis.origin.y
                                                                + n2.y * referenceDistance, neutralZ ) );
                zPoint = apply( j, zPoint, VECTOR3D( bend.axis.origin.x,
                                                     bend.axis.origin.y, neutralZ + 1.0 ) );
            }
            frame.origin = axisOrigin;
            frame.t = VECTOR3D( axisEnd.x - axisOrigin.x, axisEnd.y - axisOrigin.y,
                                axisEnd.z - axisOrigin.z );
            const double tLength = std::sqrt( frame.t.x*frame.t.x + frame.t.y*frame.t.y
                                              + frame.t.z*frame.t.z );
            if( std::isfinite( tLength ) && tLength > std::numeric_limits<double>::epsilon() )
            {
                frame.t = VECTOR3D( frame.t.x/tLength, frame.t.y/tLength, frame.t.z/tLength );
                auto orthogonal = [&]( const VECTOR3D& value )
                {
                    const double projection = value.x*frame.t.x + value.y*frame.t.y + value.z*frame.t.z;
                    return VECTOR3D( value.x - frame.t.x*projection, value.y - frame.t.y*projection,
                                     value.z - frame.t.z*projection );
                };
                frame.n = orthogonal( VECTOR3D( normalPoint.x-axisOrigin.x,
                                                normalPoint.y-axisOrigin.y,
                                                normalPoint.z-axisOrigin.z ) );
                const double nLength = std::sqrt( frame.n.x*frame.n.x + frame.n.y*frame.n.y
                                                  + frame.n.z*frame.n.z );
                if( std::isfinite( nLength ) && nLength > std::numeric_limits<double>::epsilon() )
                {
                    frame.n = VECTOR3D( frame.n.x/nLength, frame.n.y/nLength, frame.n.z/nLength );
                    frame.z = orthogonal( VECTOR3D( zPoint.x-axisOrigin.x,
                                                   zPoint.y-axisOrigin.y,
                                                   zPoint.z-axisOrigin.z ) );
                    const double zn = frame.z.x*frame.n.x + frame.z.y*frame.n.y + frame.z.z*frame.n.z;
                    frame.z = VECTOR3D( frame.z.x-zn*frame.n.x, frame.z.y-zn*frame.n.y,
                                        frame.z.z-zn*frame.n.z );
                    const double zLength = std::sqrt( frame.z.x*frame.z.x + frame.z.y*frame.z.y
                                                      + frame.z.z*frame.z.z );
                    if( std::isfinite( zLength ) && zLength > std::numeric_limits<double>::epsilon() )
                    {
                        frame.z = VECTOR3D( frame.z.x/zLength, frame.z.y/zLength, frame.z.z/zLength );
                        frame.valid = true;
                    }
                }
            }
        }
        frames.push_back( frame );
    }

    VECTOR3D current = aPoint;
    for( size_t i = 0; i < aBends.size(); ++i )
        current = apply( i, current, aPoint );
    return current;
}

// A reinforcement fit is valid only when the complete tolerance interval is
// inside the connector's required interval.
inline bool stiffenerFits( double aBaseMin, double aBaseMax, double aAdhesiveMin,
                           double aAdhesiveMax, double aStiffenerMin,
                           double aStiffenerMax, double aRequiredMin,
                           double aRequiredMax )
{
    for( double value : { aBaseMin, aBaseMax, aAdhesiveMin, aAdhesiveMax,
                           aStiffenerMin, aStiffenerMax, aRequiredMin, aRequiredMax } )
        if( !std::isfinite( value ) || value < 0.0 )
            return false;
    if( aBaseMin > aBaseMax || aAdhesiveMin > aAdhesiveMax
        || aStiffenerMin > aStiffenerMax || aRequiredMin > aRequiredMax )
        return false;
    return aBaseMin + aAdhesiveMin + aStiffenerMin >= aRequiredMin
        && aBaseMax + aAdhesiveMax + aStiffenerMax <= aRequiredMax;
}

} // namespace BACKPLANE_RIGID_FLEX

#endif
