#pragma once
// Control-tree geometry dump. See GeometryDump.cpp for why this replaced pixel probing.

#include <string>
#include "UIlib.h"

namespace geomdump
{

// Write the laid-out tree to `pszPath`. Returns false when the file cannot be created.
// `pOut` optionally receives the same text, so a caller can log a summary without
// re-reading the file.
// IMPORTANT: call only after layout, or GetPos() returns default rectangles.
bool DumpTree(DuiLib::CControlUI* pRoot, const wchar_t* pszPath, std::string* pOut = nullptr);

// Number of controls in the tree; used by callers to prove the dump is not empty.
int CountNodes(DuiLib::CControlUI* pRoot);

}   // namespace geomdump
