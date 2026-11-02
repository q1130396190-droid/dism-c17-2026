import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

block = r'''
    // -------- C10: is SIMD actually worth it on the layered fixup? --------
    // C9 says the scalar per-pixel pass costs ~4ms/frame at full HD. Haswell and
    // everything after (4th gen Core, 2013+) has AVX2, so a 32-byte kernel could
    // in principle cut that. This measures the real ceiling before recommending
    // anything: if the memory traffic dominates, vectorising buys little and the
    // recommendation would be wrong.
    {
        const int W2 = 1920, H2 = 1080;
        CBitmapDC buf(W2, H2);
        if (buf.pBits) {
            const int nPix = W2 * H2;
            const int ROUNDS = 20;

            // reset helper: mostly-opaque pixels with a few zero-alpha ones, which
            // is exactly the shape the framework loop has to fix up.
            auto reset = [&]() {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                for (int i = 0; i < nPix; ++i)
                    p[i] = ((i & 0xFF) == 0) ? (0x00123456u) : 0xFF203040u;
            };

            // scalar, exactly as the framework writes it
            reset();
            auto t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                for (int i = 0; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            auto t1 = clk::now();
            const double scalarMs = Ms(t0, t1) / ROUNDS;

            // SSE2 floor: 4 pixels, branch-free, the operation is an OR when the
            // pixel is non-zero, which is what the branch computes anyway.
            reset();
            t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                int i = 0;
                for (; i + 4 <= nPix; i += 4) {
                    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
                    __m128i rgb = _mm_and_si128(v, _mm_set1_epi32(0x00FFFFFF));
                    __m128i nonZero = _mm_cmpeq_epi32(rgb, _mm_setzero_si128());
                    // nonZero lanes are "all ones" where rgb == 0 => keep alpha as is
                    __m128i withAlpha = _mm_or_si128(v, _mm_set1_epi32(static_cast<int>(0xFF000000u)));
                    __m128i out = _mm_blendv_epi8(withAlpha, v, nonZero);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i), out);
                }
                for (; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            t1 = clk::now();
            const double sseMs = Ms(t0, t1) / ROUNDS;

#if defined(__AVX2__)
            // AVX2 kernel: only compiled when the build actually targets AVX2, so
            // this number is reported honestly rather than assumed.
            reset();
            t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                int i = 0;
                const __m256i vAlpha = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
                const __m256i vRgb   = _mm256_set1_epi32(0x00FFFFFF);
                for (; i + 8 <= nPix; i += 8) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
                    __m256i rgb = _mm256_and_si256(v, vRgb);
                    __m256i isZero = _mm256_cmpeq_epi32(rgb, _mm256_setzero_si256());
                    __m256i withAlpha = _mm256_or_si256(v, vAlpha);
                    __m256i out = _mm256_blendv_epi8(withAlpha, v, isZero);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), out);
                }
                for (; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            t1 = clk::now();
            const double avxMs = Ms(t0, t1) / ROUNDS;
            ::printf("      %-42s %8.3f ms/frame\n", "C10 avx2   verify-fixup", avxMs);
            ::printf("      speedup vs scalar: %.2fx (SSE2: %.2fx)\n\n",
                     avxMs > 0 ? scalarMs / avxMs : 0.0, sseMs > 0 ? scalarMs / sseMs : 0.0);
#else
            ::printf("      %-42s %8.3f ms/frame\n", "C10 sse2   verify-fixup", sseMs);
            ::printf("      speedup vs scalar: %.2fx  (AVX2 not enabled in this build)\n\n",
                     sseMs > 0 ? scalarMs / sseMs : 0.0);
#endif

            ::printf("      %-42s %8.3f ms/frame\n", "C10 scalar verify-fixup", scalarMs);
            Chk("C10 simd clears the bar", sseMs < scalarMs,
                "vectorised fixup faster than scalar");
        }
    }
'''

anchor = '    // -------- C7: composite -- a dense list page --------'
text = text.replace(anchor, block.lstrip('\n') + chr(10) + anchor, 1)
text = text.replace('#include <gdiplus.h>', '#include <gdiplus.h>\n#include <immintrin.h>', 1)

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('inserted C10')
