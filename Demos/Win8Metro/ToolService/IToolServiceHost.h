#pragma once

// ============================================================
// IToolServiceHost.h —— [R6] Service -> 宿主 环境能力窄接口
//
// 服务通过本接口访问窗口/文件对话框/批处理/日志/确认等"环境"，
// 不直接持有 CFrameWnd/C*UI/MessageBox（R7 契约）。方法签名只使用
// windows 系统类型与 STL；由 CFrameWnd（经 Page）真实实现。
//
// 语义与旧 IToolHost（ToolDialog.h）一一对应，仅把 CDuiString
// 换成 std::wstring，边界转换留在 UI 层。
//
// [R16][T6] 双宿主接口归一：本接口为主接口，由 CFrameWnd 直接实现
// （旧 CToolServiceHostBridge 转换层取消）。IToolHost 降级为 Page
// 渲染期薄适配（随 T10 评估删除）。HostNotify/HostProgress 明确为
// "服务不需要、仅 UI 使用"，保留在 IToolHost，不入本接口。
// ============================================================

#include <windows.h>
#include <string>
#include <vector>

#include "ToolTypes.h"

namespace toolsvc {

class IToolServiceHost
{
public:
    virtual ~IToolServiceHost() {}

    virtual HWND HostHwnd() = 0;
    virtual int  HostDPI() = 0;

    virtual bool HostOpenFile(std::wstring& sResult,
                              const std::wstring& sFilter,
                              const std::wstring& sTitle) = 0;
    virtual bool HostSaveFile(std::wstring& sResult,
                              const std::wstring& sFilter,
                              const std::wstring& sTitle,
                              const std::wstring& sDefName) = 0;
    virtual bool HostBrowseFolder(std::wstring& sResult,
                                  const std::wstring& sTitle) = 0;

    // dism /Get-WimInfo 真实解析（返回服务层 DTO）。
    virtual bool HostQueryWim(const std::wstring& sWim,
                              std::vector<TWimIndex>& vOut,
                              std::wstring* psErr) = 0;

    // 新建临时工作目录写批处理并（经 UAC）执行；phProcess 非空接收
    // 进程句柄（调用方 CloseHandle），空指针表示不关心。
    virtual bool HostRunBat(const std::wstring& sBatName,
                            const std::wstring& sBody,
                            HANDLE* phProcess) = 0;

    virtual void HostLog(const std::wstring& sLine, int nLevel = 0) = 0;

    // 模态提示/确认。危险确认默认按钮固定在"否"。
    virtual void HostAlert(const std::wstring& sTitle,
                           const std::wstring& sText,
                           UINT nIcon) = 0;
    virtual bool HostConfirm(const std::wstring& sTitle,
                             const std::wstring& sText) = 0;
};

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
