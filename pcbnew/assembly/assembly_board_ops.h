#ifndef PCBNEW_ASSEMBLY_BOARD_OPS_H
#define PCBNEW_ASSEMBLY_BOARD_OPS_H

#include <json_common.h>

#include <string>

class PCB_EDIT_FRAME;
class BOARD;
struct ASSEMBLY_MANIFEST;

nlohmann::json AssemblyListComponents( const ASSEMBLY_MANIFEST& aManifest,
                                       const std::string& aInstanceId, BOARD* aActiveBoard = nullptr );

bool AssemblyMoveComponent( const ASSEMBLY_MANIFEST& aManifest,
                            const std::string& aInstanceId, const std::string& aReference,
                            double aXmm, double aYmm, double aRotationDeg,
                            PCB_EDIT_FRAME* aFrame = nullptr, BOARD* aActiveBoard = nullptr );

nlohmann::json AssemblyElectricalSync( const ASSEMBLY_MANIFEST& aManifest,
                                       BOARD* aActiveBoard = nullptr );

#endif
