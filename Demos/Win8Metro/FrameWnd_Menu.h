#pragma once

// ============================================================
// FrameWnd_Menu  —— [R14-01] ShowPopupMenu 菜单表驱动拆解
//
// 原 754 行 ShowPopupMenu（FrameWnd.cpp L3754-4507）按职责拆成：
//   ShowPopupMenu        薄壳：CreatePopupMenu + 4 分支 Build + Track + Dispatch
//   BuildMenuFile        文件(F)：1101/1102/1103/1104/1105/1106/1107 + WIM 转换子菜单
//   BuildMenuRecover     恢复功能(R)：1201-1208
//   BuildMenuOption      选项(O)：1301 + 操作完成后子菜单 + 语言子菜单
//   BuildMenuHelp        帮助(H)：1401-1406
//   TrackMenuAndDispatch TrackPopupMenu + DestroyMenu + 分发
//   DispatchMenuCmd      [R16-T7] kMenuCmdTab 表驱动分发（收编 1101-1406，
//                        命令 ID 符号化为 enum EMenuCmd，cc<10）
//   MenuCmd*             case 内联逻辑抽取（1107/1112/1202/1207/1208/1320+）
//
// 语义/顺序与原函数逐一对应（2026-10-13 ShowPopupMenu 基线）。
// 语言表 kLangMenu 与 LangCodeToIndex / IsLangPackAvailable 一并迁入本模块，
// FrameWnd.cpp 原调用处改用 FrameWnd_Menu_* 导出接口。
// ============================================================

#include "stdafx.h"
#include <string>

// 语言代码 → kLangMenu 下标（未命中回退 0=zh-CN）
int  FrameWnd_Menu_LangCodeToIndex(const std::wstring& sCode);

// 语言包是否可用（zh-CN 内置回退表恒可用；其余需 lang\<code>.dll 真实存在）
[[nodiscard]] bool FrameWnd_Menu_IsLangPackAvailable(const std::wstring& sCode);

// 语言表项数 / 代码 / endonym（BuildMenuOption 用）
int  FrameWnd_Menu_LangCount();
const wchar_t* FrameWnd_Menu_LangCodeAt(int nIdx);
const wchar_t* FrameWnd_Menu_LangEndonymAt(int nIdx);