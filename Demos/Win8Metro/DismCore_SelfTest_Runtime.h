#pragma once

// ============================================================
// DismCore_SelfTest_Runtime.h —— R132 运行期服务层自检入口
//
// 与 DismCore_SelfTest.cpp 的关系：
//   后者测纯函数（MD5/Base64/ParseVersion/seed 指纹），本头声明的是
//   「服务层真实往返」——会碰注册表与文件系统的那些。
//
// 契约：
//   * 只用 HKCU\Software\Dism++_SelfTest_R132 与 %TEMP%，结束即清理
//   * 不写 HKCR，不做需要管理员权限的操作
//   * 不触发崩溃自重启、不依赖 UI
// ============================================================

// SelfTestCtx 定义在 DismCore_SelfTest.cpp 内（匿名命名空间之外），
// 这里前向声明不足以使用其成员，故改为以函数指针回调方式解耦：
//   调用方传入一个「判定一项」的回调，本模块不依赖 SelfTestCtx 的布局。
using SelfTestChkFn = void (*)(void* pCtx, bool ok, const char* pszName);

// 运行期服务层自检。pCtx / pChk 由调用方提供（通常包装 SelfTestCtx::Chk）。
// 返回实际判定项数。
int SelfTestRuntimeServices(void* pCtx, SelfTestChkFn pChk);
