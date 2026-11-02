import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

block = r'''
    // -------- C9: layered-window per-pixel composite --------
    // The layered path (transparent/rounded window, the Win11 default look) walks
    // the dirty rectangle and touches every pixel in scalar code after the whole
    // frame has been painted. This is the one place where the per-pixel work is
    // both unavoidable and trivially vectorisable, so its share of a frame decides
    // whether a SIMD change is worth anything at all.
    {
        const int W2 = 1920, H2 = 1080;
        CBitmapDC src(W2, H2);
        CBitmapDC dst(W2, H2);
        if (src.pBits && dst.pBits) {
            ::ZeroMemory(src.pBits, (size_t)W2 * H2 * 4);
            // Paint something so the loop does real work (opaque glyph-ish pixels
            // scattered, like text and icons on a background).
            for (int y = 0; y < H2; y += 3) {
                for (int x = 0; x < W2; x += 17) {
                    src.pBits[((size_t)y * W2 + x) * 4 + 0] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 1] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 2] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 3] = 0xFF;
                }
            }

            const int ROUNDS = 10;
            auto t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                // Exactly the loop from CPaintManagerUI::WM_PAINT's layered branch
                // (the "else" form, without a layered draw-string): force alpha on
                // pixels that are non-zero but have no alpha channel.
                DWORD* pBits = reinterpret_cast<DWORD*>(src.pBits);
                const int nPix = W2 * H2;
                for (int i = 0; i < nPix; ++i) {
                    DWORD v = pBits[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        pBits[i] = v | 0xFF000000u;
                }
            }
            auto t1 = clk::now();
            const double curMs = Ms(t0, t1) / ROUNDS;
            ::printf("      %-42s %8.3f ms/frame  (%dx%d)\n",
                     "C9 current   layered alpha fixup (scalar)", curMs, W2, H2);

            Chk("C9 layered fixup share", curMs < 12.0,
                "full-HD fixup under 12ms per frame");
        }
    }
'''

anchor = '    // -------- C7: composite -- a dense list page --------'
text = text.replace(anchor, block.lstrip('\n') + chr(10) + anchor, 1)

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('inserted C9')
