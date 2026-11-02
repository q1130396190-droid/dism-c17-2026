// Contrast calibration sheet: render every theme surface with known text on it.
//
// Why this exists
// ---------------
// Two earlier attempts to measure contrast from ordinary page captures both produced
// wrong answers, because they had to GUESS which pixels were text:
//   * heuristic glyph detection counted 1px borders and icon art as text -> 16/16 pages
//     "failing";
//   * anchoring on known backgrounds still could not separate a glyph stroke from an
//     anti-aliased boundary, and a deliberately painted low-contrast block went
//     unreported.
//
// Guessing is the problem. This sheet removes the guess: every cell is a solid known
// surface with one known text colour, at a position computed from the cell index. The
// measurement tool then reads the exact centre of each cell and compares the two known
// colours. No classification, so no false verdicts either way.
//
// Layout: COLS x ROWS cells, each CELL_W x CELL_H, starting at (MARGIN, MARGIN).
// Cell (col,row) shows PAIRS[row*COLS+col]. The same list is printed to the log so the
// measuring tool never has to re-derive it.
#include "ContrastSheet.h"

#include "ThemeTokens.h"
#include "UIlib.h"

namespace contrastsht
{

// Each pair is (surface token, text token). The set is chosen so that every text token
// that appears on a filled surface in the real UI is exercised at least once.
struct Pair { tokens::EToken bg; tokens::EToken fg; };
static const Pair PAIRS[] = {
    { tokens::EToken::Surface,      tokens::EToken::Ink },
    { tokens::EToken::Surface,      tokens::EToken::InkTertiary },
    { tokens::EToken::Surface,      tokens::EToken::InkMuted },
    { tokens::EToken::Surface,      tokens::EToken::AccentText },
    { tokens::EToken::SurfaceAlt,   tokens::EToken::Ink },
    { tokens::EToken::SurfaceAlt,   tokens::EToken::InkTertiary },
    { tokens::EToken::SurfaceAlt,   tokens::EToken::InkMuted },
    { tokens::EToken::SurfaceAlt,   tokens::EToken::AccentText },
    { tokens::EToken::Accent,       tokens::EToken::OnAccent },
    { tokens::EToken::HeaderBand,   tokens::EToken::OnHeaderBand },
    { tokens::EToken::WarnBg,       tokens::EToken::Warn },
    { tokens::EToken::Surface,      tokens::EToken::Danger },
    { tokens::EToken::Surface,      tokens::EToken::Success },
    { tokens::EToken::Surface,      tokens::EToken::Warn },
    { tokens::EToken::SurfaceHover, tokens::EToken::Ink },
    { tokens::EToken::SurfaceSunk,  tokens::EToken::Ink },
};
constexpr int PAIR_COUNT = static_cast<int>(sizeof(PAIRS) / sizeof(PAIRS[0]));

DuiLib::CControlUI* Build()
{
    const int COLS = 4;
    const int CELL_W = 300;
    const int CELL_H = 90;
    const int MARGIN = 10;

    DuiLib::CVerticalLayoutUI* pRoot = new DuiLib::CVerticalLayoutUI();
    pRoot->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pRoot->SetInset(RECT{ 0, 0, 0, 0 });

    for (int r = 0; r * COLS < PAIR_COUNT; ++r)
    {
        DuiLib::CHorizontalLayoutUI* pRow = new DuiLib::CHorizontalLayoutUI();
        pRow->SetFixedHeight(CELL_H);
        pRow->SetChildPadding(0);

        for (int c = 0; c < COLS; ++c)
        {
            const int idx = r * COLS + c;
            DuiLib::CContainerUI* pCell = new DuiLib::CContainerUI();
            pCell->SetFixedWidth(CELL_W);
            pCell->SetFixedHeight(CELL_H);

            if (idx < PAIR_COUNT)
            {
                pCell->SetBkColor(tokens::Token(PAIRS[idx].bg));

                // The label is a fixed token so the sheet itself never drifts; the point
                // is to measure the PAIR, not to render meaningful words.
                DuiLib::CTextUI* pText = new DuiLib::CTextUI();
                pText->SetText(L"Ag 0O");
                pText->SetFont(2);
                pText->SetTextColor(tokens::Token(PAIRS[idx].fg));
                pText->SetAttribute(L"align", L"center");
                pText->SetAttribute(L"valign", L"vcenter");
                pText->SetFixedWidth(CELL_W);
                pText->SetFixedHeight(CELL_H);
                pCell->Add(pText);
            }
            pRow->Add(pCell);
        }
        pRoot->Add(pRow);
    }
    return pRoot;
}

int CellRect(int index, int* px, int* py, int* pw, int* ph)
{
    const int COLS = 4;
    const int CELL_W = 300;
    const int CELL_H = 90;
    const int MARGIN = 10;

    if (index < 0 || index >= PAIR_COUNT) return 0;
    const int r = index / COLS;
    const int c = index % COLS;
    *px = MARGIN + c * CELL_W;
    *py = MARGIN + r * CELL_H;
    *pw = CELL_W;
    *ph = CELL_H;
    return 1;
}

int PairCount() { return PAIR_COUNT; }

void Describe(char* buf, int cbBuf, int index)
{
    if (index < 0 || index >= PAIR_COUNT) { if (cbBuf > 0) buf[0] = 0; return; }
    // Only the numeric tokens are emitted: the measuring side maps ids back through the
    // EToken enum order, so a renamed token does not break the pairing.
    ::sprintf_s(buf, static_cast<size_t>(cbBuf), "%d,%d,%d",
                index,
                static_cast<int>(PAIRS[index].bg),
                static_cast<int>(PAIRS[index].fg));
}

}   // namespace contrastsht
