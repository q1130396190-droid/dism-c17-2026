#pragma once

// ============================================================
// ProgramCacheService —— 程序和功能页首屏缓存
//
// 进入页面时不再同步扫描三个 Uninstall 注册表源：先用上次结果
// 秒开首屏，再由后台线程重新枚举并替换。缓存仅用于展示；
// 卸载/修复仍走原有实时路径。
// ============================================================

#include "ProgramService.h"
#include "Utils/Utils.h"
#include <vector>
#include <string>

class ProgramCacheService
{
public:
    // [R27] [[nodiscard]]：缓存读写在磁盘故障/权限不足时可能失败。
    // 忽略返回值会让"写缓存失败"变成静默缺陷——下次进页仍读到旧列表，
    // 表现为"卸载了程序但列表没变"，很难归因。
    [[nodiscard]] static bool Load(std::vector<ProgEntry>& vOut);
    [[nodiscard]] static bool Save(const std::vector<ProgEntry>& vIn);
    static void Invalidate();
};
