#pragma once

// ============================================================
// ListService.h —— [R6-02] 超链接列表(EToolKind::TK_LINKLIST) 无 UI 业务：
// 条目持有与"点击目标"校验/规范化。打开动作（ShellExecute）留
// ToolDialogPage；本服务只保证越界/空目标被拒绝。
// ============================================================

#include <string>
#include <vector>
#include <utility>

#include "IToolService.h"

namespace toolsvc {

class CListService : public IToolService
{
public:
    explicit CListService(IToolServiceHost* pHost) : m_pHost(pHost) {}

    int Kind() const override { return static_cast<int>(EToolKind::TK_LINKLIST); }
    // [R16][T4] 条目与标题统一经 args 注入（旧 SetItems 入口废弃）。
    void Open(const ToolOpenArgs& args) override;

    size_t Count() const { return m_vItems.size(); }
    // 顶部说明标题（空=Page 用 StringMgr 默认文案）。
    const std::wstring& Title() const { return m_sTitle; }

    // 取第 nIdx 条的显示标签与目标；越界返回 false。
    bool LabelAt(int nIdx, std::wstring& sLabel) const;
    // 取点击目标：越界或目标为空返回 false（Page 不执行打开）。
    [[nodiscard]] bool TargetAt(int nIdx, std::wstring& sTarget) const;

    // http(s) 外链判定（其余按本地路径处理）。
    static bool IsHttp(const std::wstring& sTarget);

private:
    IToolServiceHost* m_pHost;
    std::vector<std::pair<std::wstring, std::wstring> > m_vItems; // label,target
    std::wstring m_sTitle;  // [R16][T4] 经 Open 注入
};

} // namespace toolsvc
