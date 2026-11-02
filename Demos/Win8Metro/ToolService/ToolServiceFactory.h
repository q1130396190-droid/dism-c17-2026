#pragma once

// ============================================================
// ToolServiceFactory.h —— [R6]/[R16][T5] 按弹窗 kind 装配服务束
//
//   CreateToolServices(kind, host) 返回该弹窗所需的全部服务：
//     * pSvc —— 弹窗业务服务（LogDepGraph/List/Confirm），
//               无业务服务的 kind（映像/向导类）为空；
//     * pImg —— wimgapi 映像/ISO 服务，仅需要它的 kind 创建
//               （CAPTURE/APPLY/IMAGEX/ISO/ESDWIZ/MOUNT/PECFG）；
//     * pWiz —— 六大向导业务服务，仅需要它的 kind 创建
//               （ACCOUNT/BOOTFIX/APPLY/MOUNT/PECFG）。
//   [R16][T5] 取代旧 CreateToolService（6 kind 业务服务）与
//   Page 构造期无条件创建 Iso/Wizard 两个重型服务的旧通路。
// ============================================================

#include <memory>

#include "IToolService.h"
#include "IIsoImageService.h"
#include "IWizardService.h"

namespace toolsvc {

class IToolServiceHost;

struct TServiceBundle
{
    std::unique_ptr<IToolService>      pSvc;  // 弹窗业务服务（可空）
    std::shared_ptr<IIsoImageService>  pImg;  // [R16][T2] shared_ptr：ESD 线程持拥有型副本
    std::unique_ptr<IWizardService>    pWiz;  // 向导业务服务（可空）
};

TServiceBundle CreateToolServices(int nKind, IToolServiceHost* pHost);

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
