// ============================================================================
//  ActivationKitClient.h
//
//  主程序（Win8Metro.exe）访问 ActivationKit.dll 的客户端适配层。
//
//  R01 FrogPlugins 重构产物：原「激活备份/还原」能力从青蛙插件 vtable
//  slot[77]/[81] 剥离为独立卫星组件 ActivationKit.dll（3 个序号冻结导出）。
//  本适配层与 NCleanerClient / CbsHostClient 同构：
//    运行期 LoadLibrary + GetProcAddress，不静态链接；
//    DLL 缺失/导出不全时 IsReady()=false，UI 置灰并给出中文提示，
//    主程序启动与其余 11 个工具卡片不受影响。
//
//  生命周期：FrameWnd 构造 Init()、析构 Shutdown()。
//  线程模型：函数指针加载后只读；备份/还原由 UI 工作流同步调用。
// ============================================================================

#ifndef ACTIVATION_KIT_CLIENT_H
#define ACTIVATION_KIT_CLIENT_H

#include <windows.h>
#include <string>

namespace ActivationKitClient
{

// 加载 amd64\ActivationKit.dll（兼容主程序同目录扁平布局）并解析 3 个导出。
// DLL 缺失返回 false（pErr 可空）；此时 IsReady()=false。
bool Init(std::wstring* pErr = nullptr);

// 卸载（进程退出路径；保持模块驻留亦可，此处对称释放）。
void Shutdown();

// 是否已就绪（DLL 已加载且 3 个导出全部解析成功）。
bool IsReady();

// 取最后一次错误描述（Init 失败原因）。
const std::wstring& LastError();

// ---- 业务接口（参数语义与 ActivationKit.h 的 C ABI 一致）----

// 备份：在 sOutDir 下生成「激活备份\Backup.ini」。
long Backup(const std::wstring& sOutDir, const std::wstring& sNote,
            std::wstring* pErr = nullptr);

// 还原：读 sBaseDir\激活备份\Backup.ini 写回注册表。
long Restore(const std::wstring& sBaseDir, std::wstring* pErr = nullptr);

// 备份件是否存在且含有效 DigitalProductId。
[[nodiscard]] bool BackupExists(const std::wstring& sBaseDir);

}  // namespace ActivationKitClient

#endif  // ACTIVATION_KIT_CLIENT_H
