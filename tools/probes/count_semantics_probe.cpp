// ============================================================
// count_semantics_probe.cpp -- 证伪探针辅助函数的判据语义
//
// 背景
// ----
// 前四轮里出现过四次「判据语义与调用方预期不一致」：
//   * CountNonBg 把页面自身白底当成内容（空列表与有数据同值）
//   * CountDark 用 r+g+b<380，哨兵绿 0x00FF00 和=255 被当成文字
//   * inset 属性归属搞错，探针伪造出一个不存在的排版缺陷
//   * CountNonBgInRect 直接用 rc.left 索引，负坐标越界
//
// 这些都不是产品 bug，而是**测量工具自己的 bug**，而且都会让报告变绿。
// 本轮把 CountDistinctColors 拉出来单独验证：
//
//  它用 4096 个哈希桶统计 (color*2654435761)>>20 & 0xFFF，返回"被占用的桶数"，
//  而不是真实的不同颜色数。桶数远小于 2^24，理论上会碰撞。
//
// 要回答的问题是：**碰撞会不会让"空白页"判据失效？**
//   判据是 nColors > 4 才算"有内容"，空白页只有 1 种颜色。
//   如果碰撞把 5 种颜色压到 1 种，就会把有内容判成空白（假阴性）。
//
// 本探针用已知颜色数的合成图直接验证，不靠推理。
// ============================================================

#include <windows.h>
#include <cstdio>
#include <set>
#include <vector>
#include <cstdint>

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* name, bool ok, const char* detail)
{
    if (ok) { ++g_nPass; ::printf("[PASS] %s | %s\n", name, detail); }
    else    { ++g_nFail; ::printf("[FAIL] %s | %s\n", name, detail); }
    ::fflush(stdout);
}

// 与 render_page_probe.cpp 里完全相同的实现（逐字复制，避免"测的不是同一份")
int CountDistinctColorsImpl(const DWORD* p, int w, int h)
{
    if (p == nullptr) return 0;
    unsigned char seen[4096] = {};
    int n = 0;
    const int nStep = (w * h > 200000) ? 3 : 1;
    for (int i = 0; i < w * h; i += nStep)
    {
        const DWORD c = p[i] & 0x00FFFFFF;
        const int key = static_cast<int>((c * 2654435761u) >> 20) & 0xFFF;
        if (!seen[key]) { seen[key] = 1; ++n; }
    }
    return n;
}

int TrueDistinct(const DWORD* p, int w, int h)
{
    std::set<DWORD> s;
    for (int i = 0; i < w * h; ++i) s.insert(p[i] & 0x00FFFFFF);
    return static_cast<int>(s.size());
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    const int W = 400, H = 400;

    // ---- 用例 1：单色（空白页的真实形态）----
    {
        std::vector<DWORD> v(W * H, 0x00FFFFFF);
        const int nHash = CountDistinctColorsImpl(v.data(), W, H);
        const int nTrue = TrueDistinct(v.data(), W, H);
        char buf[128];
        ::sprintf_s(buf, "hash=%d true=%d", nHash, nTrue);
        Chk("单色图：哈希计数 == 真实颜色数", nHash == nTrue, buf);
        Chk("单色图：哈希计数 <= 4（空白页判据成立）", nHash <= 4, buf);
    }

    // ---- 用例 2：5 种不同颜色（刚好越过 >4 阈值）----
    {
        std::vector<DWORD> v(W * H, 0x00FFFFFF);
        const DWORD kCols[5] = { 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00333333, 0x00AAAAAA };
        for (int i = 0; i < W * H; ++i) v[i] = kCols[i % 5];
        const int nHash = CountDistinctColorsImpl(v.data(), W, H);
        const int nTrue = TrueDistinct(v.data(), W, H);
        char buf[128];
        ::sprintf_s(buf, "hash=%d true=%d", nHash, nTrue);
        Chk("5 色图：哈希计数 >= 5（不会被误判成空白）", nHash >= 5, buf);
    }

    // ---- 用例 3：大量颜色，测碰撞导致的低估幅度 ----
    {
        std::vector<DWORD> v(W * H);
        for (int i = 0; i < W * H; ++i)
            v[i] = 0x00000000 | (static_cast<DWORD>(i) & 0x00FFFFFF);
        const int nHash = CountDistinctColorsImpl(v.data(), W, H);
        const int nTrue = TrueDistinct(v.data(), W, H);
        char buf[160];
        ::sprintf_s(buf, "hash=%d true=%d 误差=%.1f%%",
                    nHash, nTrue, (nTrue > 0) ? (100.0 * (nTrue - nHash) / nTrue) : 0.0);
        // 哈希桶上限 4096，颜色数远大于它时必然饱和，这是设计使然
        Chk("大色彩量：哈希计数 ≤ 4096（桶上限，符合设计）", nHash <= 4096, buf);
        Chk("大色彩量：哈希计数仍远超空白阈值 4", nHash > 4, buf);
    }

    // ---- 用例 4：判据的真实用途——区分"空白"与"有内容" ----
    {
        std::vector<DWORD> blank(W * H, 0x00FFFFFF);

        std::vector<DWORD> content(W * H, 0x00FFFFFF);
        // 模拟抗锯齿文字：白底上画出一批中间灰
        for (int i = 0; i < W * H / 100; ++i)
            content[i] = 0x00000000 | (static_cast<DWORD>(0x40 + (i % 0x80)) << 16)
                                  | (static_cast<DWORD>(0x40 + (i % 0x80)) << 8)
                                  |  (0x40 + (i % 0x80));

        const int nBlank = CountDistinctColorsImpl(blank.data(), W, H);
        const int nContent = CountDistinctColorsImpl(content.data(), W, H);
        char buf[160];
        ::sprintf_s(buf, "blank=%d content=%d", nBlank, nContent);
        Chk("判据有效性：空白图 ≤4 且内容图 >4（分类正确）",
            nBlank <= 4 && nContent > 4, buf);
    }

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
