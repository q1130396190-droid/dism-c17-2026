#pragma once

// ============================================================
// IToolService.h —— [R6] 工具箱弹窗业务服务统一生命周期接口
//
// 5 个具体服务（LogDepGraph / List / Confirm / IsoImage / Wizard）
// 各自实现强类型业务方法供 ToolDialogPage 直接调用；本接口只收敛
// 与弹窗种类无关的生命周期，便于工厂统一持有与 Close：
//
//   Open(args) —— 弹窗 Create 前注入入参（对应旧 Set* 系列）；
//   Close()    —— 弹窗销毁时的资源回收（透明挂载 ISO 卸载等，
//                 对应旧 ~CToolDialog 的 R-B80 行为，禁止回退）。
//
// 服务不创建线程、不持有控件；长耗时作业由 Page 用
// BackgroundJobs 起线程，函数体内只跑服务的无 UI 原语。
// ============================================================

#include "ToolTypes.h"

namespace toolsvc {

class IToolServiceHost;

class IToolService
{
public:
    virtual ~IToolService() {}

    // 弹窗种类（TK_*），供 Page/工厂断言服务与弹窗匹配。
    virtual int Kind() const = 0;

    // Create 前注入入参；默认实现忽略（多数弹窗无入参）。
    virtual void Open(const ToolOpenArgs& /*args*/) {}

    // 弹窗销毁时回收（卸载透明挂载的 ISO 等）。
    virtual void Close() {}
};

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
