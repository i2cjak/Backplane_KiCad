/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#ifndef KICAD_CROSS_PROBE_CLIENT_H
#define KICAD_CROSS_PROBE_CLIENT_H

#include <map>
#include <mutex>
#include <string>

#include <kicommon.h>
#include <kiway.h>

#include <api/common/commands/cross_probe_commands.pb.h>


/** Routes cross-probe API messages between standalone KiCad instances. */
class KICOMMON_API CROSS_PROBE_CLIENT
{
public:
    static bool SendToFrame( FRAME_T aTarget, const google::protobuf::Message& aRequest );

    static void AnnounceToPrimary( FRAME_T aFrameType );

    static void RegisterPeer( FRAME_T aFrameType, const std::string& aSocketPath );

    static bool IsOnStandardSocketPath();

private:
    static std::mutex s_mutex;
    static std::map<FRAME_T, std::string> s_peers;
};

#endif // KICAD_CROSS_PROBE_CLIENT_H
