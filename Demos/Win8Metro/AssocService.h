#pragma once

// ============================================================
// AssocService —— S8（v2 P2c）
// 文件关联查询/设置/清除/重置（Windows 10+ UserChoice 注册表方案），
// 以及关联页用到的 dism 导入/导出/移除参数执行。
// 纯 Win32（经 Registry / ProcessRunner），无 UI 控件依赖；
// 实现文件不使用预编译头、不 include UIlib.h / StringManager.h。
//
// 函数体与原 CFrameWnd::SetUserFileAssociation 等逐字一致
//（物理搬迁，零逻辑改动）。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <vector>
#include <utility>

// [R37 Patch D] 文件关联写入结果码（原 CFrameWnd::EAssocWriteResult）
enum class EAssocWriteResult : int
{
    ASSOC_WRITE_OK = 0,
    ASSOC_WRITE_NO_PERMISSION = -1,
    ASSOC_WRITE_INVALID_PROGID = -2,
    ASSOC_WRITE_IASSOC_FAIL = -3,
    ASSOC_WRITE_REG_FAIL = -4,
    ASSOC_WRITE_VERIFY_FAIL = -5,
};

// [R94] 一个用 SHAssocEnumHandlers 枚举到的关联处理器
struct AssocHandlerEntry
{
    AssocHandlerEntry() : bRecommended(false) {}
    DuiLib::CDuiString sName;      // IAssocHandler::GetUIName
    DuiLib::CDuiString sIconPath;  // IAssocHandler::GetIconLocation
    int                nIconIndex;
    bool               bRecommended;  // IsRecommended
};

// [R97] 一个"打开方式"ProgId 候选（含显示名与命令行）
struct AssocProgidEntry
{
    AssocProgidEntry() : bRecommended(false) {}
    DuiLib::CDuiString sProgId;     // ProgId 名（如 "Notepad.exe" 或 "txtfile"）
    DuiLib::CDuiString sDisplay;    // 显示名
    DuiLib::CDuiString sCommand;    // open\command 命令行
    bool               bRecommended;  // 是否当前默认
};

class AssocService
{
public:
    // 查询扩展名的当前 ProgId 与 open\command 命令行（原样摘自 QueryAssociation）。
    // sExtIn 为空返回 false；查不到时 out 参数保持空串。
    static bool QueryProgIdAndCommand(const DuiLib::CDuiString& sExtIn,
                                     DuiLib::CDuiString& sProgId,
                                     DuiLib::CDuiString& sCommand);

    // 写入 HKCU\...\FileExts\.ext\UserChoice\ProgId，回读校验
    // [R99] 组合写路径：同时维护 FileExts\OpenWithProgids + OpenWithList
    static EAssocWriteResult SetUserFileAssociation(const DuiLib::CDuiString& sExt,
                                                  const DuiLib::CDuiString& sProgId);
    static EAssocWriteResult ClearUserFileAssociation(const DuiLib::CDuiString& sExt);
    static EAssocWriteResult ResetAssocToDefault(const DuiLib::CDuiString& sExt);
    static bool VerifyUserFileAssociation(const DuiLib::CDuiString& sExt,
                                       const DuiLib::CDuiString& sProgId);
    static int  ApplyAssocBatch(std::vector<std::pair<DuiLib::CDuiString,DuiLib::CDuiString>>& vPairs);

    // [R99] FileExts 组合写路径：UserChoice\ProgId + OpenWithProgids + OpenWithList
    // 返回 EAssocWriteResult::ASSOC_WRITE_OK 表示三处均写成功；否则返回首个失败码。
    static EAssocWriteResult SetUserFileAssociationEx(const DuiLib::CDuiString& sExt,
                                                     const DuiLib::CDuiString& sProgId);

    // [R115-1] HKCR 层写路径：新建扩展名注册项 + OpenWithProgids 候选 + 默认值。
    // 覆盖 HKCR\.ext(默认 ProgId) 与 HKCR\.ext\OpenWithProgids(推荐候选)，
    // 与 FileExts\UserChoice 组合，形成"默认 + 候选"三层完整写路径。
    static EAssocWriteResult RegisterExtKey(const DuiLib::CDuiString& sExt,
                                            const DuiLib::CDuiString& sProgId);

    // [R115-1] HKCR 层写路径：删除扩展名注册项（RegDeleteTree 递归删 HKCR\.ext 与
    // 关联的 FileExts UserChoice/OpenWithProgids/OpenWithList），走确认后回滚。
    static EAssocWriteResult DeleteExtKey(const DuiLib::CDuiString& sExt);

    // [R115-1] HKCR 层写路径：恢复系统默认（删除 UserChoice + HKCR 注册项 + 广播）。
    static EAssocWriteResult RestoreExtDefault(const DuiLib::CDuiString& sExt);

    // [P1-A9] 默认应用关联原生维护（替代 dism /Import|/Export|
    // /Remove-DefaultAppAssociations 外部进程）。
    // 导出：遍历 HKLM/HKCU Classes 扩展名与 URL 协议，经
    //   IApplicationAssociationRegistration::QueryCurrentDefault 取当前
    //   默认 ProgId，AssocQueryString 取应用名，生成标准
    //   <DefaultAssociations> XML（UTF-8 BOM）。
    // 导入：把 XML 路径写入系统策略键
    //   HKLM\SOFTWARE\Policies\Microsoft\Windows\System
    //   !DefaultAssociationsConfiguration（企业部署同款机制）；
    //   离线映像（sOfflineRoot 非空，如 "D:\\"）先把 XML 复制到
    //   <root>\\Windows\\System32\\DefaultAssociations.xml，策略值指向
    //   C:\\Windows\\System32\\DefaultAssociations.xml，注册表经
    //   DataEngine 离线蜂巢会话写入。
    // 移除：删除该策略值（离线同样经蜂巢会话）。
    // 返回 0 = 成功；非 0 = 错误码，sErr 回传可读说明。
    static DWORD ExportDefaultAssociations(const DuiLib::CDuiString& sXmlPath,
                                           const DuiLib::CDuiString& sOfflineRoot,
                                           DuiLib::CDuiString& sErr);
    static DWORD ImportDefaultAssociations(const DuiLib::CDuiString& sXmlPath,
                                           const DuiLib::CDuiString& sOfflineRoot,
                                           DuiLib::CDuiString& sErr);
    static DWORD RemoveDefaultAssociations(const DuiLib::CDuiString& sOfflineRoot,
                                           DuiLib::CDuiString& sErr);

    // [R94] SHAssocEnumHandlers 枚举 .ext 的关联处理器（Shell API 直连）。
    // 返回：0 成功；1 参数非法；2 CoInitialize 失败；3 SHAssocEnumHandlers 失败；
    //       4 枚举失败。
    static int EnumAssocHandlers(const DuiLib::CDuiString& sExt,
                                std::vector<AssocHandlerEntry>& vOut,
                                DWORD& dwErrOut);

    // [R97] OpenWithProgids —— 枚举"打开方式"ProgId 列表（概念名闭合）
    // 真实路径：AssocQueryString(ASSOCSTR_PROGID) 取当前默认 + HKCR\Applications
    // 枚举全部注册应用 + IApplicationAssociationRegistration::QueryAppIsDefault
    // 判断默认。返回 >=0 为命中数，负值为错误码。
    static int EnumOpenWithProgids(const DuiLib::CDuiString& sExt,
                                   std::vector<AssocProgidEntry>& vOut,
                                   DWORD& dwErrOut);

    // [R97] 触发系统"打开方式"对话框（ShellExecuteEx + "OpenWith" verb）
    // 返回 true 表示已触发，false 表示触发失败。
    static bool OpenWithDialog(const DuiLib::CDuiString& sExt);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
