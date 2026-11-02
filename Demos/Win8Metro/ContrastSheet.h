#pragma once
// Contrast calibration sheet: see ContrastSheet.cpp for why it exists.
//
// The sheet is built as an independent control tree and rendered off-screen, so it never
// touches the live UI. It exists to make contrast MEASURABLE rather than guessed: every
// cell holds one known surface colour with one known text colour at a computed position.

#include "UIlib.h"

namespace contrastsht
{

// Build a fresh control tree laid out as COLS x ROWS cells. Caller owns it.
DuiLib::CControlUI* Build();

// Rectangle of cell `index` in the rendered bitmap. Returns 0 for out-of-range.
int CellRect(int index, int* px, int* py, int* pw, int* ph);

// Number of cells the sheet contains.
int PairCount();

// Write "index,bgToken,fgToken" for one cell. Token ids are EToken enum values, so the
// measuring side can map them back without depending on token spelling.
void Describe(char* buf, int cbBuf, int index);

}   // namespace contrastsht
