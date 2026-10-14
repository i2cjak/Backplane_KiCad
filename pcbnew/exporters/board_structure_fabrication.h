/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wx/string.h>
#include <math/vector2d.h>
class BOARD;
class REPORTER;

// Positive material-area Gerbers, distinct from electrical artwork. The offset
// must match the accompanying copper and drill outputs.
bool ExportBoardStructureFabrication( BOARD& aBoard, const wxString& aDirectory,
                                      const VECTOR2I& aOffset, REPORTER& aReporter );
