// [GATE-COV] Minimal stand-in for Demos/Win8Metro/stdafx.h.
//
// Why this exists: OfflineCbsService.cpp opens with #include "stdafx.h", and the
// real one pulls in UIlib.h plus `using namespace DuiLib`. The service itself has
// zero DuiLib references (verified: 0 occurrences of DuiLib/CDuiString/UIlib), so
// linking the whole framework PCH chain just to compile it would import a
// dependency the translation unit does not actually have.
//
// This shim supplies the same non-framework prelude the real PCH provides, and
// nothing else. If the TU ever starts using DuiLib types, the build fails here
// rather than silently linking the framework -- which is the signal we want:
// at that point the file has grown a real dependency and the gate decision in
// untestable_probes.txt must be revisited.
#pragma once

#include "targetver.h"

#include <windows.h>
#include <cstdlib>
#include <memory.h>

#include "FormatW.h"
