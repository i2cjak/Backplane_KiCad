/*
 * Fork-only physical structure for flex and rigid-flex boards.
 *
 * This model deliberately contains no PCB artwork.  Artwork remains in the
 * native, flat KiCad board and this model is persisted in the companion file.
 */
#pragma once

#include <json_common.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <math/vector2d.h>
#include <math/vector3.h>

class SHAPE_POLY_SET;

enum class BOARD_STRUCTURE_STACK_KIND
{
    RIGID,
    FLEX
};

struct BOARD_STRUCTURE_MATERIAL
{
    std::string id;
    std::string category;
    std::string material;
    int64_t thickness_nm = 0;
    int64_t min_thickness_nm = 0;
    int64_t max_thickness_nm = 0;
};

struct BOARD_STRUCTURE_STACK
{
    std::string id;
    std::string name;
    BOARD_STRUCTURE_STACK_KIND kind = BOARD_STRUCTURE_STACK_KIND::RIGID;
    // Global KiCad copper layer IDs, in physical order, preserving identity
    // when a regional stack omits a layer.
    std::vector<int> copper_layers;
    std::vector<BOARD_STRUCTURE_MATERIAL> materials;
    struct LAYER_INSTANCE
    {
        std::string material_id;
        int global_copper_layer = -1;
        int64_t z_offset_nm = 0;
    };
    std::vector<LAYER_INSTANCE> layers;
    std::string continuity_id;
};

struct BOARD_STRUCTURE_POLYGON
{
    std::vector<VECTOR2I> outline;
    std::vector<std::vector<VECTOR2I>> holes;
};

struct BOARD_STRUCTURE_REGION
{
    std::string id;
    std::string name;
    std::string stack_id;
    BOARD_STRUCTURE_POLYGON shape;
};

struct BOARD_STRUCTURE_BOUNDARY
{
    std::string id;
    std::vector<VECTOR2I> polyline;
};

struct BOARD_STRUCTURE_REGION_SEED
{
    std::string region_id;
    VECTOR2I point;
};

struct BOARD_STRUCTURE_STIFFENER
{
    std::string id;
    std::string region_id;
    BOARD_STRUCTURE_POLYGON shape;
    std::string material;
    std::string adhesive;
    int64_t thickness_nm = 0;
    int64_t min_thickness_nm = 0;
    int64_t max_thickness_nm = 0;
    int64_t adhesive_thickness_nm = 0;
    bool top = true;
    std::string role;
    std::vector<std::string> support_targets;
};

struct BOARD_STRUCTURE_COVERLAY
{
    std::string id;
    std::string region_id;
    BOARD_STRUCTURE_POLYGON coverage;
    std::vector<BOARD_STRUCTURE_POLYGON> openings;
    std::string material;
    std::string adhesive;
    int64_t thickness_nm = 0;
    int64_t adhesive_thickness_nm = 0;
    bool top = true;
    int64_t registration_tolerance_nm = 0;
};

struct BOARD_STRUCTURE_BEND
{
    std::string id;
    std::string region_id;
    VECTOR2I start;
    VECTOR2I end;
    double angle_degrees = 0.0;
    double inner_radius_mm = 0.0;
    double neutral_axis_offset_mm = -1.0;
    bool moving_side_left = true;
    int sequence = 0;
};

struct BOARD_STRUCTURE_VALIDATION
{
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool IsValid() const { return errors.empty(); }
};

class BOARD_STRUCTURE
{
public:
    static constexpr int SCHEMA_VERSION = 1;

    std::vector<BOARD_STRUCTURE_STACK> stacks;
    std::vector<BOARD_STRUCTURE_REGION> regions;
    // Companion-only Edge.Cuts partition records.  These are never written to
    // the native board file or interpreted as additional board edges.
    std::vector<BOARD_STRUCTURE_BOUNDARY> boundaries;
    std::vector<BOARD_STRUCTURE_REGION_SEED> region_seeds;
    std::vector<BOARD_STRUCTURE_STIFFENER> stiffeners;
    std::vector<BOARD_STRUCTURE_COVERLAY> coverlays;
    std::vector<BOARD_STRUCTURE_BEND> bends;

    // Unknown top-level fields survive a load/save cycle for forward compatibility.
    nlohmann::json unknown = nlohmann::json::object();

    void Clear();
    bool Empty() const;
    nlohmann::json ToJson() const;
    void FromJson( const nlohmann::json& aJson );
    BOARD_STRUCTURE_VALIDATION Validate() const;

    // Rebuild base regions by applying the declared internal boundaries to a
    // native Edge.Cuts outline.  The operation is atomic: malformed or
    // dangling boundaries leave all existing regions untouched.
    bool DerivePartition( const SHAPE_POLY_SET& aOutline, std::string* aError = nullptr );

    const BOARD_STRUCTURE_REGION* RegionAt( const VECTOR2I& aPoint ) const;
    const BOARD_STRUCTURE_STACK* StackForRegion( const BOARD_STRUCTURE_REGION& aRegion ) const;
    std::vector<const BOARD_STRUCTURE_REGION*> RegionsIntersecting( const BOARD_STRUCTURE_POLYGON& aShape ) const;

    // Common center of the physical flex stack used by the bend sequence.
    double BendDatumZMm() const;

    static double BendStripLengthMm( const BOARD_STRUCTURE_BEND& aBend, double aThicknessMm = 0.0 );

    // Fold a flat board point into the bend's local cylindrical frame.  The
    // returned coordinates are millimetres in the board mechanical frame;
    // electrical artwork remains in its original flat coordinates.
    static VECTOR3D FoldPointMm( const BOARD_STRUCTURE_BEND& aBend,
                                 const VECTOR3D& aFlatPoint, double aThicknessMm = 0.0 );

    // Apply all bends in developed sequence order.  Hinge side tests use the
    // original board coordinates while each later hinge frame is transported
    // through the preceding folds.
    static VECTOR3D FoldPointMm( const std::vector<BOARD_STRUCTURE_BEND>& aBends,
                                 const VECTOR3D& aFlatPoint, double aThicknessMm = 0.0,
                                 double aBendDatumZ = 0.0 );

    // Test the complete base/adhesive/reinforcement tolerance interval against
    // a connector's required mating interval.
    static bool StiffenerFits( double aBaseMinMm, double aBaseMaxMm,
                               double aAdhesiveMinMm, double aAdhesiveMaxMm,
                               double aStiffenerMinMm, double aStiffenerMaxMm,
                               double aRequiredMinMm, double aRequiredMaxMm );
private:
    void ParseJson( const nlohmann::json& aJson );
    nlohmann::json m_original = nlohmann::json::object();

};
