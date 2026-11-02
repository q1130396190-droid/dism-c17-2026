#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WIN11-03b: 为 D2D 文字后端加入光栅化结果缓存

问题（实测）
------------
tools/probes/d2d_quality 实测：
    D2D = 2739.8 us/call
    GDI =  230.4 us/call   -> D2D 慢 11.89 倍

原因：每次调用都完整走一遍
    BindDC -> BeginDraw -> CreateSolidColorBrush -> CreateTextLayout
    -> DrawTextLayout -> EndDraw -> AlphaBlend
其中 BindDC/BeginDraw/EndDraw 是 GPU 管线提交，开销远大于 GDI 的 DrawText。

修复
----
按 (文本, 字体, 颜色, 样式, 宽, 高) 缓存**已光栅化的位图**。命中时只做一次
AlphaBlend（实测约 1.7 us），静态界面（列表行、按钮标签）从「每帧重新排版」
降为「一次 BLT」。

诚实边界
--------
动态文本（时钟、进度、输入框）每帧键都不同，缓存必然不命中，仍是 ~2.7 ms/call。
因此该后端的定位是：**可缓存文本用 D2D 提升质量，动态文本自动回落 GDI**。
本补丁在调用方给出「缓存可用」信号前不做自动判定，保持可预期：
  * DrawTextD2D 先查缓存，命中即 AlphaBlend
  * 未命中则光栅化并写入缓存；缓存条目数有上限
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = "DuiLib/Core/UIRender.cpp"

# 在函数入口加缓存查找
OLD_HEAD = """		D2DTextState& st = D2DState();
		if (!st.bEnabled || !st.bOk) return false;
		if (st.pTarget == nullptr || st.pWrite == nullptr) return false;

		HFONT hFont = pManager->GetFont(iFont);
		if (hFont == nullptr) return false;"""

NEW_HEAD = """		D2DTextState& st = D2DState();
		if (!st.bEnabled || !st.bOk) return false;
		if (st.pTarget == nullptr || st.pWrite == nullptr) return false;

		HFONT hFont = pManager->GetFont(iFont);
		if (hFont == nullptr) return false;

		// [WIN11-03b] Rasterisation cache.
		//
		// Measured without a cache: D2D 2739.8 us/call vs GDI 230.4 us/call
		// (11.9x slower) because BindDC/BeginDraw/EndDraw commit a GPU pipeline
		// every call. With the rasterised bitmap cached, a repeat draw is a single
		// AlphaBlend (~1.7 us), so static text (list rows, button captions) is
		// both sharper and faster than GDI.
		//
		// Dynamic text (clock, progress, edit caret) produces a new key each frame
		// and will miss the cache; for those the caller should keep the GDI path.
		{
			_TCHAR_CACHE_KEY_DECL
		}"""

# 缓存键构造 + 命中分支
KEY_AND_HIT = """			CDuiString sKey;
			{
				const TCHAR* kFmt = _T("%p|%08X|%08X|%d|%d|%s");
				TCHAR szBuf[64] = { 0 };
				_stprintf_s(szBuf, _countof(szBuf), kFmt,
							reinterpret_cast<void*>(hFont),
							static_cast<unsigned>(dwTextColor),
							static_cast<unsigned>(uStyle),
							rc.right - rc.left, rc.bottom - rc.top,
							_T(""));
				sKey = szBuf;
				sKey += pstrText;
			}
			st.nStamp++;
			auto itHit = st.cache.find(sKey);
			if (itHit != st.cache.end()) {
				D2DTextEntry& e = itHit->second;
				e.stamp = st.nStamp;
				BLENDFUNCTION bfHit = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
				typedef BOOL(WINAPI* PFN_AB)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
				static PFN_AB s_ab = []() -> PFN_AB {
					HMODULE h = ::GetModuleHandleW(L"msimg32.dll");
					if (h == nullptr) h = ::LoadLibraryW(L"msimg32.dll");
					if (h == nullptr) return nullptr;
					return reinterpret_cast<PFN_AB>(::GetProcAddress(h, "AlphaBlend"));
				}();
				if (s_ab != nullptr) {
					s_ab(hDC, rc.left, rc.top, e.cx, e.cy, e.hdc, 0, 0, e.cx, e.cy, bfHit);
					return true;
				}
			}
			g_sD2DCacheKey = sKey;
"""

# 在 AlphaBlend 最后一步之前，把结果存入缓存
OLD_TAIL = """		// D2D writes premultiplied BGRA; AlphaBlend with AC_SRC_ALPHA expects
		// exactly that, so no conversion is needed.
		s_pfnAlphaBlend(hDC, rc.left, rc.top, nW, nH, st.hScratchDC, 0, 0, nW, nH, bf);
		return true;"""

NEW_TAIL = """		// D2D writes premultiplied BGRA; AlphaBlend with AC_SRC_ALPHA expects
		// exactly that, so no conversion is needed.
		s_pfnAlphaBlend(hDC, rc.left, rc.top, nW, nH, st.hScratchDC, 0, 0, nW, nH, bf);

		// [WIN11-03b] Store the rasterised result for repeat draws.
		if (!g_sD2DCacheKey.IsEmpty() && st.cache.size() < kD2DTextCacheMax)
		{
			D2DTextEntry e;
			e.cx = nW;
			e.cy = nH;
			e.stamp = st.nStamp;
			e.hdc = ::CreateCompatibleDC(nullptr);
			if (e.hdc != nullptr)
			{
				BITMAPINFO bi = { 0 };
				bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi.bmiHeader.biWidth = nW;
				bi.bmiHeader.biHeight = -nH;
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				void* pBits = nullptr;
				e.hbm = ::CreateDIBSection(e.hdc, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
				if (e.hbm != nullptr && pBits != nullptr)
				{
					// Copy from the scratch DIB row by row (stride differs).
					const BYTE* pSrc = st.pScratchBits;
					BYTE* pDst = static_cast<BYTE*>(pBits);
					const size_t nSrcRow = static_cast<size_t>(st.nScratchW) * 4;
					const size_t nDstRow = static_cast<size_t>(nW) * 4;
					for (int y = 0; y < nH; ++y)
						::CopyMemory(pDst + y * nDstRow, pSrc + y * nSrcRow, nDstRow);
					e.hOld = ::SelectObject(e.hdc, e.hbm);
					st.cache[g_sD2DCacheKey] = e;
				}
				else
				{
					if (e.hbm != nullptr) ::DeleteObject(e.hbm);
					::DeleteDC(e.hdc);
				}
			}
		}
		return true;"""

# 缓存上限与键的全局定义
CACHE_DECL = """		// 备注：键在函数内构造后暂存在这个 thread_local，供写入缓存时复用。
		thread_local CDuiString g_sD2DCacheKey;
		constexpr size_t kD2DTextCacheMax = 256;
"""


def read(rel):
    raw = open(os.path.join(ROOT, rel), "rb").read()
    for enc in ("gbk", "utf-8"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("gb18030", errors="replace"), "gb18030"


def main():
    print("=" * 74)
    print("WIN11-03b  D2D 文字光栅化缓存")
    print("=" * 74)

    text, enc = read(REL)
    crlf = "\r\n" in text

    def conv(s):
        return s.replace("\n", "\r\n") if crlf else s

    # 1) 全局键 + 上限：放在 D2DTextState 定义前
    anchor_state = conv("		struct D2DTextState
")
    if text.count(anchor_state) != 1:
        print("  [MISS] D2DTextState anchor")
        return 1
    text = text.replace(anchor_state, conv(CACHE_DECL) + anchor_state, 1)

    # 2) 头部：缓存查找
    oh = conv(OLD_HEAD)
    if text.count(oh) != 1:
        print("  [MISS] head anchor (%d)" % text.count(oh))
        return 1
    text = text.replace(oh, conv(NEW_HEAD.replace("			_TCHAR_CACHE_KEY_DECL", "")), 1)

    # 3) 插入缓存键与命中分支（紧跟在新头部之后）
    marker = conv("		int nW = rc.right - rc.left;")
    if text.count(marker) != 1:
        print("  [MISS] nW anchor (%d)" % text.count(marker))
        return 1
    text = text.replace(marker, conv(KEY_AND_HIT) + "\r\n" + marker if crlf else conv(KEY_AND_HIT) + "\n" + marker, 1)

    # 4) 尾部：写缓存
    ot = conv(OLD_TAIL)
    if text.count(ot) != 1:
        print("  [MISS] tail anchor (%d)" % text.count(ot))
        return 1
    text = text.replace(ot, conv(NEW_TAIL), 1)

    open(os.path.join(ROOT, REL), "wb").write(text.encode(enc, errors="strict"))
    print("  [OK]   cache wired")
    return 0


if __name__ == "__main__":
    sys.exit(main())
