#pragma once

// ============================================================
// FontSupport.h —— 字体可用性探测与回退
//
// 为什么需要（实测证据）：
//   Main.xml 用 6 个 <Font name="Microsoft YaHei"> 定义全部界面字体。
//   但实测本机（Win11 25H2 26200）**没有** "Microsoft YaHei"，
//   只有 "Microsoft YaHei UI" —— 也就是说整个界面的字体一直在走
//   GDI 的隐式回退，回退到哪个字体由系统决定，不可控也不可复现。
//
//   这不是理论风险：中文界面在缺 YaHei 的系统（精简版、部分语言包、
//   Windows Server 桌面体验未装）上同样会落到"随便一个 CJK 字体"，
//   笔画粗细与字宽都会和设计稿不一致。
//
// 做法：
//   只在**启动时探测一次**，把 XML 里的逻辑字体名解析成"本机真实可用"
//   的字体名，再用它重建字体表。不改变布局与字号，只修字体族。
//
// Win10 兼容：
//   全部走 EnumFontFamiliesEx / CreateFontIndirect + GetTextFace，
//   Win7 起就有；不依赖任何新 API，也不需要版本判断。
// ============================================================

#include <windows.h>

namespace fontsup
{

// 探测某个字体族名在本机是否真实存在。
//   判据：CreateFontIndirect 后 GetTextFace 返回的名字与请求名一致
//   （不区分大小写）。这比 EnumFontFamiliesEx 更可靠 ——
//   后者在有字体替换/链接时也会返回邻居字体。
[[nodiscard]] bool IsFontAvailable(const wchar_t* pszFaceName);

// 把逻辑名解析成本机可用的实际字体名：
//   1) 请求名本身可用 → 原样返回
//   2) 否则按候选表逐个探测，返回第一个可用
//   3) 全部不可用 → 返回空串（调用方保留原值，交给 GDI 回退）
//
// 候选表由调用方按"语义"提供，例如中文界面正文字体传
// {"Microsoft YaHei", "Microsoft YaHei UI", "Microsoft JhengHei UI", "SimSun"}。
[[nodiscard]] const wchar_t* ResolveFirstAvailable(const wchar_t* const* ppCandidates,
                                                   int nCount);

// 界面用三组语义字体：正文 / 符号 / 等宽。
//   返回的指针指向进程级静态存储，生命周期到进程结束。
[[nodiscard]] const wchar_t* UiTextFace();     // 中文正文
[[nodiscard]] const wchar_t* UiSymbolFace();   // 勾选/箭头等符号
[[nodiscard]] const wchar_t* UiMonoFace();     // 路径、日志

}   // namespace fontsup