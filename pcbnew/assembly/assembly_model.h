/* Assembly manifest and validation model. */
#ifndef PCBNEW_ASSEMBLY_MODEL_H
#define PCBNEW_ASSEMBLY_MODEL_H

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ASSEMBLY_TRANSFORM
{
    std::array<double, 3> translation{ 0.0, 0.0, 0.0 }; // mm
    std::array<double, 3> rotation{ 0.0, 0.0, 0.0 };    // degrees, XYZ
};

struct ASSEMBLY_AXIS
{
    std::array<double, 3> direction{ 0.0, 0.0, 1.0 };
};

struct ASSEMBLY_BOUNDS
{
    std::array<double, 3> minimum{ 0.0, 0.0, 0.0 };
    std::array<double, 3> maximum{ 0.0, 0.0, 0.0 };
    bool valid = false;
};

struct ASSEMBLY_INSTANCE
{
    std::string id;
    std::string source;
    std::string kind = "board"; // board, part, or assembly
    ASSEMBLY_TRANSFORM transform;
    ASSEMBLY_BOUNDS bounds; // optional source-space broad-phase bounds, mm
    std::map<std::string, std::string> connector_pins;
    bool folded = false;
    // Unknown per-instance keys are retained for forward-compatible round trips.
    nlohmann::json extra_json = nlohmann::json::object();
};

struct ASSEMBLY_MATE
{
    std::string id;
    std::string type = "point"; // point or axis
    std::string first_instance;
    std::string second_instance;
    std::array<double, 3> first{ 0.0, 0.0, 0.0 };
    std::array<double, 3> second{ 0.0, 0.0, 0.0 };
    double clearance_mm = 0.0;
    std::array<double, 3> first_axis{ 0.0, 0.0, 1.0 };
    std::array<double, 3> second_axis{ 0.0, 0.0, 1.0 };
    bool opposing = false;
    nlohmann::json extra_json = nlohmann::json::object();
};

struct ASSEMBLY_CLEARANCE
{
    std::string first;
    std::string second;
    double minimum_mm = 0.0;
};

struct ASSEMBLY_MANIFEST
{
    int schema = 1;
    std::string name;
    std::string anchor;
    std::vector<ASSEMBLY_INSTANCE> instances;
    std::vector<ASSEMBLY_MATE> mates;
    std::filesystem::path base_directory;
    // Derived leaf-pair constraints, populated only by FlattenAssembly.
    std::vector<ASSEMBLY_CLEARANCE> resolved_clearances;
    bool flattened = false;
    nlohmann::json electrical = nlohmann::json::object();
    nlohmann::json extra_json = nlohmann::json::object();

    static ASSEMBLY_MANIFEST Load( const std::filesystem::path& aPath );
    void Save( const std::filesystem::path& aPath ) const;
    nlohmann::json ToJson() const;
    static ASSEMBLY_MANIFEST FromJson( const nlohmann::json& aJson );

    std::vector<std::string> Validate( bool aRequireSources = false ) const;
};

/** Resolve directed point/axis mates; the first instance defines the reference frame. */
bool ApplyAssemblyMates( ASSEMBLY_MANIFEST& aManifest );

/** Resolve mates and recursively flatten nested assembly instances. */
ASSEMBLY_MANIFEST FlattenAssembly( const ASSEMBLY_MANIFEST& aManifest );

struct ASSEMBLY_COLLISION
{
    std::string first;
    std::string second;
    double clearance_mm = 0.0;
};

std::vector<ASSEMBLY_COLLISION> CheckAssemblyBounds( const ASSEMBLY_MANIFEST& aManifest,
                                                      double aRequiredClearanceMm = 0.0 );

#endif
