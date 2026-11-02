#pragma once

// ============================================================
// ConfirmService.h —— [R6-03] 三个"确认/设置"弹窗的无 UI 业务：
//   * EToolKind::TK_DETAILS    详细设置：config.ini[Advanced] 5 选项读写 +
//                   LoadWofAdk(SCM)/RightClickMenu(注册表)/
//                   IntegrateRE(reagentc)/DeltaTransfer(Wof) 4 个系统副作用；
//   * EToolKind::TK_DUALPATH   双路径收集与非空校验（弹窗文案留 Page）；
//   * EToolKind::TK_DELCONFIRM 危险项清单持有与"已确认"标志。
// 控件渲染、StringMgr 文案、MessageBox 均留 ToolDialogPage。
// ============================================================

#include <string>
#include <vector>

#include "IToolService.h"

namespace toolsvc {

class CConfirmService : public IToolService
{
public:
    // 同一服务类承接 3 个相近 kind；nKind 仅用于 Kind() 如实回传。
    explicit CConfirmService(IToolServiceHost* pHost, int nKind)
        : m_pHost(pHost), m_nKind(nKind) {}

    int Kind() const override { return m_nKind; }
    void Open(const ToolOpenArgs& args) override;

    // ---- 详细设置（5 个高级开关，顺序与旧 DetailsOk kKeys 一致）----
    static constexpr int kAdvancedCount = 5;
    // 从 config.ini[Advanced] 读取当前 5 开关（未 Load 缓存时沿用进程内现状，
    // 与旧 BuildDetails 直接 GetString 的行为一致）。
    void LoadAdvancedFlags(bool vOut[kAdvancedCount]) const;
    // 写回 5 开关并执行 4 个系统副作用；日志/告警经 IToolServiceHost。
    void ApplyAdvanced(const bool vIn[kAdvancedCount]);

    // ---- 双路径选择器 ----
    // [R16][T4] 标题/标签经 Open 注入；路径本身是用户运行时输入，仍经 SetDualPaths 收集。
    const std::wstring& DualTitle() const { return m_sDualTitle; }
    const std::wstring& DualLabel1() const { return m_sDualLabel1; }
    const std::wstring& DualLabel2() const { return m_sDualLabel2; }
    void SetDualPaths(std::wstring s1, std::wstring s2)
    { m_sDual1 = std::move(s1); m_sDual2 = std::move(s2); }
    [[nodiscard]] bool DualValid() const { return !m_sDual1.empty() && !m_sDual2.empty(); }
    const std::wstring& Dual1() const { return m_sDual1; }
    const std::wstring& Dual2() const { return m_sDual2; }

    // ---- 删除确认列表 ----
    // [R16][T4] 清单与标题统一经 Open(ToolOpenArgs) 注入（旧 SetDelItems 废弃）。
    size_t DelCount() const { return m_vDel.size(); }
    const std::wstring& DelAt(size_t nIdx) const
    { static const std::wstring sEmpty; return (nIdx < m_vDel.size()) ? m_vDel[nIdx] : sEmpty; }
    const std::wstring& DelTitle() const { return m_sDelTitle; }
    void MarkConfirmed() { m_bConfirmed = true; }
    [[nodiscard]] bool Confirmed() const { return m_bConfirmed; }

private:
    // 4 个高级开关的系统副作用（逐字搬迁自旧 DetailsOk，仅替换日志/字符串边界）。
    void ApplyLoadWofAdk();
    void ApplyRightClickMenu();
    void ApplyIntegrateRE();
    void ApplyDeltaTransfer();

    IToolServiceHost*       m_pHost;
    int                     m_nKind;
    std::wstring            m_sDual1;
    std::wstring            m_sDual2;
    std::vector<std::wstring> m_vDel;
    bool                    m_bConfirmed = false;
    // [R16][T4] 弹窗文案（经 Open 注入，空=Page 用 StringMgr 默认）
    std::wstring            m_sDualTitle;
    std::wstring            m_sDualLabel1;
    std::wstring            m_sDualLabel2;
    std::wstring            m_sDelTitle;
};

} // namespace toolsvc
