/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <import_export.h>
#include <api/common/types/embedded_files.pb.h>

class EMBEDDED_FILES;

namespace kiapi::board
{
void PackEmbeddedFiles( common::types::EmbeddedFiles& aOutput, const EMBEDDED_FILES& aFiles );
bool UnpackEmbeddedFiles( EMBEDDED_FILES& aOutput, const common::types::EmbeddedFiles& aProto );
}
