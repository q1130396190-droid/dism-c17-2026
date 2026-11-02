#pragma once

// [B4-F7] EPageId 前向声明（有固定底层类型，可前置声明）；PageBase.h 完整定义由实现方 include。
enum class EPageId : int;

// ============================================================
// IPageHost.h —— [R4] 页宿主窄接口
//
// 消耦目标（审计台账 R4-01，CP-1 值持）：
//   * 原 CFrameWnd 值持 17 个具体页对象（16 页序 + Windows 功能子视图），
//     壳代码到处 m_pgXxx.ConcreteMethod()，FrameWnd.h 被迫 include 全部
//     具体页头；
//   * 页所有权/装配下沉 CPageHost（PageHost.h）后，只需要"按页序取
//     CPageBase 基类指针"的调用方（Service / 未来外部模块）依赖本接口，
//     不依赖任何具体 CPageXxx 类型。
//
// 生命周期：CPageHost 与核心壳同生命周期（壳的值成员），页对象在宿主
// 构造时一次性装配（仅保存 IPageContext&，不做调用）。
// ============================================================

class CPageBase;

class IPageHost
{
public:
    virtual ~IPageHost() {}

    // 按 content_area 页序（EPageId，0..15）取页；越界返回 nullptr。
    virtual CPageBase* PageById(int idx) = 0;
    // [B4-F7] EPageId 重载：字面量 PageById(PAGE_*) 匹配此重载，转发 int 版；
    // 动态 int（GetCurSel/表项/循环）走 int 版，语义不变。
    virtual CPageBase* PageById(EPageId id) { return PageById(static_cast<int>(id)); }
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
