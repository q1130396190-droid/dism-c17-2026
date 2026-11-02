// ============================================================
// ListService.cpp —— [R6-02] 超链接列表条目/目标校验
// 行为等价基线：ToolDialog.cpp(2026-10-13) 7821-7830 LinkListOpen。
// ============================================================

#include <string>

#include "ListService.h"

namespace toolsvc {

void CListService::Open(const ToolOpenArgs& args)
{
    m_vItems = args.vLinks;
    m_sTitle = args.sLinkTitle;   // [R16][T4] 标题随 args 注入
}

bool CListService::LabelAt(int nIdx, std::wstring& sLabel) const
{
    if (nIdx < 0 || nIdx >= static_cast<int>(m_vItems.size())) return false;
    sLabel = m_vItems[nIdx].first;
    return true;
}

bool CListService::TargetAt(int nIdx, std::wstring& sTarget) const
{
    if (nIdx < 0 || nIdx >= static_cast<int>(m_vItems.size())) return false;
    if (m_vItems[nIdx].second.empty()) return false;
    sTarget = m_vItems[nIdx].second;
    return true;
}

bool CListService::IsHttp(const std::wstring& sTarget)
{
    return sTarget.compare(0, 7, L"http://") == 0 ||
           sTarget.compare(0, 8, L"https://") == 0;
}

} // namespace toolsvc
// [C++17 R3] 文件级内联常量（C++17 inline variables，ODR 安全）：
