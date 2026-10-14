#ifndef PCBNEW_ASSEMBLY_EXPORTER_H
#define PCBNEW_ASSEMBLY_EXPORTER_H

#include "assembly_model.h"
#include <functional>
#include <string>
#include <vector>

class REPORTER;
class TopoDS_Shape;
struct ASSEMBLY_COLLISION;

using ASSEMBLY_BOARD_EXPORTER = std::function<bool( const ASSEMBLY_INSTANCE&, const std::string&, REPORTER* )>;

/** Aggregate individually exported board/part STEP files into one assembly STEP. */
class ASSEMBLY_EXPORTER
{
public:
    static bool Export( const ASSEMBLY_MANIFEST& aManifest, const std::string& aOutput,
                        REPORTER* aReporter = nullptr,
                        const ASSEMBLY_BOARD_EXPORTER& aBoardExporter = {} );
    static std::vector<ASSEMBLY_COLLISION> CheckCollisions( const ASSEMBLY_MANIFEST& aManifest,
                                                             REPORTER* aReporter = nullptr,
                                                             const ASSEMBLY_BOARD_EXPORTER& aBoardExporter = {} );
};

#endif
