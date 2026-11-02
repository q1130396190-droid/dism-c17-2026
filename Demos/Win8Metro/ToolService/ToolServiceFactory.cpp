// ============================================================
// ToolServiceFactory.cpp —— [R6]/[R16][T5] kind -> 服务束创建点。
// 覆盖全部 15 kind：
//   * 6 个弹窗业务服务（LogDepGraph/List/Confirm，R6-02/03 已下沉）；
//   * IsoImage/Wizard 两个重型服务按 kind 惰性装配（R6-04/05 迁入工厂），
//     不需要它们的 kind 不再创建（日志/依赖图弹窗不再持有 ISO 挂载槽）。
// ============================================================

#include "ToolServiceFactory.h"
#include "IToolServiceHost.h"
#include "LogDepGraphService.h"
#include "ListService.h"
#include "ConfirmService.h"

#include <memory>

namespace toolsvc {

[[nodiscard]] TServiceBundle CreateToolServices(int nKind, IToolServiceHost* pHost)
{
    TServiceBundle b;
    switch (nKind)
    {
    case EToolKind::TK_LOGVIEW:
    case EToolKind::TK_DEPGRAPH:
        // [C++17 阶段3] 所有权移交调用方（pSvc 为 unique_ptr，随束析构释放，无裸 delete）
        b.pSvc = std::make_unique<CLogDepGraphService>(pHost);
        break;
    case EToolKind::TK_LINKLIST:
        b.pSvc = std::make_unique<CListService>(pHost);
        break;
    case EToolKind::TK_DETAILS:
    case EToolKind::TK_DUALPATH:
    case EToolKind::TK_DELCONFIRM:
        b.pSvc = std::make_unique<CConfirmService>(pHost, nKind);
        break;

    case EToolKind::TK_CAPTURE:
    case EToolKind::TK_IMAGEX:
    case EToolKind::TK_ISO:
    case EToolKind::TK_ESDWIZ:
        // 仅映像语义弹窗创建 wimgapi 作业工厂/ISO 挂载槽/ESD 解密原语
        b.pImg = CreateIsoImageService();
        break;

    case EToolKind::TK_ACCOUNT:
    case EToolKind::TK_BOOTFIX:
        // Acct*/Bf* 路径不触碰映像服务（m_pIso 仅为历史保留依赖）
        b.pWiz = CreateWizardService(nullptr, pHost);
        break;

    case EToolKind::TK_APPLY:
    case EToolKind::TK_MOUNT:
    case EToolKind::TK_PECFG:
        // [R16][T5] Apply 作业线程经 IWizardService::ApplyRunJob 执行，
        // 需同时装配映像服务（挂载槽/作业工厂）与向导服务。
        b.pImg = CreateIsoImageService();
        b.pWiz = CreateWizardService(b.pImg.get(), pHost);
        break;

    default:
        // 未知 kind：空束（Page 走默认渲染，与旧行为一致）
        break;
    }
    return b;
}

} // namespace toolsvc
