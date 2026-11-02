# Unusable verification tools -- kept for the record, do not wire into any gate

Both scripts tried to measure text contrast from rendered pixels and both returned wrong
verdicts. They are archived rather than deleted so the reasoning is not lost, and they
are excluded from every automated path because a measurement tool that reports a wrong
number is worse than no tool at all.

## audit_contrast.py

Reported **16/16 pages** as failing. Cause: it classified glyph pixels heuristically, and
a 1px border or a colourful icon satisfies the same test as a text stroke.
Measured false pair: `#ABABAB on #FFFFFF ratio=2.30` -- that is the Border token, not text.

## contrast_known_bg.py

Anchored on known surface colours instead of guessing glyphs, which was the right idea,
but the both-sides-must-be-background rule combined with a sampling stride of 3 made the
foreground set empty in practice: a deliberately painted low-contrast text block was
still reported as 0/16 pages failing. Switching to per-pixel scanning ran 7 minutes
without producing a verdict.

## What replaced them

`tools/mig/check_token_contrast.py` computes contrast from the token VALUES parsed out of
ThemeTokens.cpp. The pairing set is small and fully known, so no pixels need to be
interpreted -- which removes the entire class of failure above. It found a real defect the
pixel tools never did: light-theme `Success #2DA44E` at 3.22:1 on white.

A third attempt, an isolated calibration sheet with known cell geometry, also failed: a
hand-built control tree has no paint manager, so text was never drawn (0 non-white pixels
of 451,400). See section 4.8 of the handover document.
