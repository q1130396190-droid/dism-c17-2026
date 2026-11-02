# DuiLib_Ultimate · Win8Metro C++17 版

> [English / 英文说明](README_EN.md)

轻量化、高性能、易扩展的 Windows 桌面 UI 库与工具箱，面向 Windows 10/11 x64 平台，基于 C++17 重构，支持 C++23 现代轨道。仓库同时包含核心库源码与 Win8Metro 桌面工具的发布成品。

---

## 目录

- [项目简介](#项目简介)
- [仓库内容](#仓库内容)
- [编译环境要求](#编译环境要求)
- [编译步骤](#编译步骤)
  - [方式一：Visual Studio 直接编译](#方式一visual-studio-直接编译)
  - [方式二：CMake 构建 DuiLib 库](#方式二cmake-构建-duilib-库)
  - [方式三：Rust dismcore 子模块（可选）](#方式三rust-dismcore-子模块可选)
- [使用环境（运行时依赖）](#使用环境运行时依赖)
- [AI 辅助开发说明](#ai-辅助开发说明)
- [已知问题](#已知问题)
- [目录结构](#目录结构)
- [许可协议](#许可协议)

---

## 项目简介

**DuiLib_Ultimate** 是深耕 Windows 软件 UI 开发的开源利器，以**轻量化、高性能、易扩展**为核心设计理念，专为 Windows 平台打造极致桌面应用体验。本仓库同时交付一套以该 UI 库构建的完整桌面软件 —— **Win8Metro**。

---

### Win8Metro · Windows 系统维护工具箱

**Win8Metro** 是一款面向 Windows 10/11 的全能型系统部署与维护工具，覆盖日常运维的 17 个功能场景，从系统清理、驱动管理到无人值守安装一应俱全。软件采用经典 Metro 风格界面，全中文/英文/日文等 11 种语言本地化，便携版解压即用。

#### 🧩 功能模块一览

| 模块 | 说明 | 技术亮点 |
|------|------|----------|
| **🧹 系统清理** | 一键清理临时文件、缩略图缓存、Windows 更新残留、Delivery Optimization 缓存、旧版系统 | 多线程扫描，WIM 挂载清理 |
| **🚀 启动项管理** | 管理开机自启项、计划任务、驱动、服务、Shell 扩展 | 注册表 + 文件系统双引擎扫描 |
| **💾 驱动管理** | 扫描已装驱动、驱动备份、驱动还原、驱动卸载 | VSS 快照保护、PNP 设备树遍历 |
| **🟦 功能启用/关闭** | DISM 组件启用关闭（.NET Framework、Hyper-V、沙盒等） | CBS 托管 + 离线 WIM 支持 |
| **🔄 Windows 更新** | 在线/离线累积更新安装、更新历史、回滚 | CBS 会话 + SSU/LCU 批量注入 |
| **🟪 Appx 管理** | Microsoft Store 应用批量安装/卸载/清理 | AppxPackage API + 脱机包部署 |
| **📋 已装程序** | 32/64 位软件清单、批量卸载、静默卸载 | 注册表 Uninstall 键 + MSI 解析 |
| **🔗 文件关联** | 查看和修改文件扩展名默认打开程序 | ASSOC / FTYPE + IApplicationAssociationRegistration |
| **🎛️ 控制面板** | 快捷跳转所有系统设置入口 | Windows Shell 命令封装 |
| **🛠️ 工具集** | 磁盘分区管理、ISO 挂载、WIM 映像处理、系统镜像备份、BCD 编辑、VSS 卷影 | 独立 Wizard 向导 + DISM API |
| **🌐 国际化** | 切换系统 UI 语言、安装语言包 | 11 种语言（zh-CN、en-US、ja-JP、ko-KR、ru、de、fr、it、hu、pl-PL、cs） |
| **⚡ 系统优化** | 禁用遥测、关闭 Defender、启用卓越性能、字体清理 | 注册表 + 组策略 + 计划任务 |
| **🧽 注册表清理** | 无效注册表扫描、孤儿引用、卸载残留 | 注册表哈希校验 + 安全白名单 |
| **📅 计划任务** | 查看/禁用系统和第三方计划任务 | ITaskScheduler 接口 |
| **📦 无人值守** | 生成 Autounattend.xml、Sysprep 封装、ISO 自动化安装 | Windows SIM 配置 + BCD 引导 |

#### 🏗️ 软件架构

```
┌─────────────────────────────────────────────────────────┐
│                     Win8Metro.exe                       │
│  ┌──────────────────────────────────────────────────┐  │
│  │                 DuiLib UI 层                     │  │
│  │  FrameWnd / PageHost / ThemeService / StringMgr   │  │
│  └──────────────────────────────────────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │ Theme   │ Resource │  Intl    │   Config / Seed  │  │
│  │ Manager │ Manager  │ Service  │                  │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │  Clean  │ Driver   │ Feature  │  Update / Appx  │  │
│  │ Engine  │ Service  │ Service  │   Service        │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌─────────┬──────────┬──────────┬─────────────────┐  │
│  │ CBSHost │ NCleaner │  Activ.  │   duilibcalc    │  │
│  └─────────┴──────────┴──────────┴─────────────────┘  │
│  ┌──────────────┬────────────────────────────────────┐  │
│  │ Rust FFI     │   Win32 / COM / DISM API           │  │
│  │ (Hash/Crypto)│                                    │  │
│  └──────────────┴────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

#### 🔐 安全与权限

- **管理员权限**：涉及 DISM、驱动、CBS 等功能需要以管理员身份运行
- **VSS 快照保护**：清理操作前自动创建系统还原点
- **白名单机制**：注册表清理和驱动操作均内置安全白名单

---

### DuiLib_Ultimate 核心库

本仓库的 UI 核心 —— DuiLib_Ultimate，是 Win8Metro 以及其他 C++ 桌面应用的基础：

- **纯 x64 架构**：所有构建配置移除 Win32 / ARM / ARM64
- **C++17 最低标准**：支持 C++23 现代轨道（`DUI_TRACK=modern`）
- **Windows 10+**：`_WIN32_WINNT=0x0A00`
- **WebView2 集成**：现代轨道默认启用 Edge WebView2 渲染控件
- **Rust 子模块**：`dismcore` 计算模块（哈希/编码/校验）以 Rust 编写，内存安全
- **API 兼容**：与官方 DuiLib 保持兼容，项目内置迁移脚本
- **样式表（Style）**：控件属性集中管理，皮肤切换一行代码
- **资源管理器**：Zip 内嵌资源加载，外部文件自动回退
- **多语言框架**：运行时语言包热切换
- **高 DPI 自适应**：Per-Monitor V2 感知

---

### 技术栈总览

| 层次 | 技术 |
|------|------|
| UI 框架 | DuiLib_Ultimate (C++17) |
| 构建工具 | Visual Studio 2026 (MSVC 14.51) + CMake 3.20 |
| 系统 API | Win32 / COM / DISM / CBS / VSS / Appx |
| 计算模块 | Rust 1.74+ (duilibcalc FFI) |
| 图像处理 | CxImage |
| 嵌入浏览器 | Microsoft Edge WebView2 |
| 皮肤格式 | 自研 XML + 图片资源打包 |

---

## 仓库内容

| 内容 | 路径 | 说明 |
|------|------|------|
| 核心库源码 | `DuiLib/` | DuiLib 核心（UI 控件、布局、样式、资源管理） |
| 第三方依赖 | `3rd/` | CxImage 图像处理、WebView2、wimgapi |
| Win8Metro 应用 | `Demos/Win8Metro/` | 主程序，约 200 个 .cpp 源文件 |
| 附属 DLL | `Demos/ActivationKit/`、`CBSHost/`、`NCleaner/`、`SkinThemeDll/`、`ThemeManager/` | 五个独立模块 |
| Rust 计算模块 | `rust/dismcore/` | `duilibcalc` — 内存安全的哈希/编码/校验库 |
| 翻译模板 | `templates/translations/` | 术语表 JSON/YAML |
| 解决方案 | `DuiLib.sln` | VS 2026 解决方案文件 |
| 发布成品 | `dist/Win8Metro_*/` | 便携版发布包（含 Skin、DismData、多语言包） |
| 预编译库 | `bin64/`、`lib/` | x64 DLL / LIB 产物 |

---

## 编译环境要求

### 必须项

| 组件 | 最低版本 | 说明 |
|------|----------|------|
| **Windows** | Windows 10 **21H2 (19044)** / 22H2 (19045) / Windows 11 全版本 | x64 架构（拒绝 Win32 / ARM）；最低支持 Win10 21H2，但 WebView2 功能需 22H2+ |
| **Visual Studio** | VS 2026（Preview 及以上） | 需安装「使用 C++ 的桌面开发」工作负载 |
| **PlatformToolset** | v145（MSVC 14.51） | 项目已全部迁移到 v145 |
| **Windows SDK** | 10.0.26100.0 或更新 | 编译脚本会自动探测 `10.0.28000.0` → `10.0.26100.0` |
| **Git** | 任意 | 拉取源码 |
| **C++ 标准** | C++17 | `compat` 轨道 = C++17，`modern` 轨道 = C++23 |

### 可选项

| 组件 | 用途 |
|------|------|
| **CMake** | ≥ 3.20，用于仅构建 DuiLib 库本身 |
| **Rust 工具链** | stable 1.74+，构建 `duilibcalc` 计算模块 |
| **WebView2 Runtime** | 运行时安装，现代轨道的 WebView2 控件需要 |

### 安装 VS 时的工作负载勾选

打开 Visual Studio Installer → 修改 → 勾选：

- ✅ **使用 C++ 的桌面开发**（Desktop development with C++）
  - MSVC v145 - VS 2026 C++ x64/x86 生成工具
  - Windows 10/11 SDK
  - C++ CMake 工具（可选）

---

## 编译步骤

### 方式一：Visual Studio 直接编译

1. 克隆仓库：
   ```bash
   git clone https://github.com/q1130396190-droid/dism-c17-2026.git
   cd dism-c17-2026
   ```

2. 用 VS 2026 打开 `DuiLib.sln`

3. 选择配置（全部 x64）：

   | 配置 | 说明 |
   |------|------|
   | `Debug x64` | C++17 调试版 |
   | `Release x64` | C++17 发布版 |
   | `Debug23 x64` | C++23 调试版（现代轨道） |
   | `Release23 x64` | C++23 发布版（现代轨道，含 WebView2） |
   | `SDebug / SRelease` | 静态库变体 |

4. 右键 `Win8Metro` 项目 → **设为启动项目**（Set as Startup Project）

5. 生成解决方案（Build Solution）

6. 编译产物输出到 `bin64/` 目录，主程序为 `bin64/Win8Metro.exe`

### 方式二：CMake 构建 DuiLib 库

CMake 仅构建 DuiLib 核心库。Demo 程序需用 `DuiLib.sln` 编译：

```bash
# Visual Studio 2026 生成器（推荐）
cmake -G "Visual Studio 18 2026" -A x64 -DDUI_TRACK=modern -B build
cmake --build build --config Release

# 或 Ninja（需先进入 vcvars64 环境）
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DDUI_TRACK=modern -B build
cmake --build build
```

关键约束（CMake 会强制门禁）：

- 架构必须 x64，否则 `FATAL_ERROR`
- C++ 标准最低 17
- Windows 版本最低 Win10（`_WIN32_WINNT=0x0A00`）

### 方式三：Rust dismcore 子模块（可选）

`duilibcalc` 是一个 25 函数的 FFI 导出 DLL，C++ 侧通过 `duilibcalc.dll.lib` 链接。如果你重新编译 Rust 源码：

```bash
cd rust/dismcore
cargo build --release
# 产物：rust/dismcore/target/release/duilibcalc.dll
# 拷贝到：bin64/amd64/
```

> **注意**：**不要**把 DLL 命名为 `DismCore.dll`——Windows 文件名大小写不敏感，会与系统 DISM 组件冲突，导致 DISM 会话加载到本库而报 `0x8007007E`。本仓库已命名为 `duilibcalc.dll`，请保持一致。

### 一键完整构建（项目自带脚本）

```bash
# 先进入 VS Developer Command Prompt（或执行 vcvars64.bat）
msbuild DuiLib.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m
```

或使用 Win8Metro 自带的编译脚本：

```bash
cd Demos\Win8Metro
build_x64.bat
```

---

## 使用环境（运行时依赖）

### 运行时前置

| 依赖 | 说明 |
|------|------|
| **操作系统** | **最低支持 Windows 10 21H2 (Build 19044)** — 推荐 22H2 (Build 19045) 或 Windows 11 全版本（x64 only） |
| **VC++ 运行时** | Microsoft Visual C++ 2026 Redistributable (x64) — 安装 VS 或单独安装 |
| **WebView2 Runtime** | [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) — 安装 Win8Metro.exe 便携版时，`amd64/WebView2Loader.dll` 会按需加载 |
| **WinPE**（可选） | Win8Metro 工具支持在 WinPE 中运行，需集成 `amd64/` 目录下的 DLL |

### 便携版部署

`dist/Win8Metro_2026.11.1_x64_便携版/` 目录下的内容是完整发布包：

```
Win8Metro.exe              主程序
ThemeManager.dll           主题管理
amd64/                     核心 DLL 群
  ├── DuiLib.dll           UI 库
  ├── duilibcalc.dll       Rust 计算模块
  ├── SkinThemeDll.dll     皮肤引擎
  ├── ActivationKit.dll    激活辅助
  ├── CBSHost.dll          DISM 托管
  ├── NCleaner.dll         清理引擎
  ├── WebView2Loader.dll   Edge WebView2 桥接
  ├── wimgapi.dll          WIM API
  ├── bcdboot.exe          BCD 引导
  ├── efisys.bin           EFI 系统
  ├── etfsboot.com         ETFS 引导
  └── wofadk.sys           WOF 驱动
skin/                      UI 皮肤（Classic / Win8Metro）
lang/                      多语言包（cs、de、en-US、fr、hu、it、ja-JP、ko-KR、pl-PL、ru、zh-CN）
DismData/                  DISM 数据配置
Config/                    默认配置
```

**启动**：直接双击 `Win8Metro.exe`，无需安装。

### 开发时运行

编译完成后，把 `bin64/amd64/` 下的 DLL 与 `bin64/Win8Metro.exe` 放在同一目录即可运行。开发时建议同时拷贝 `skin/`、`lang/`、`DismData/`、`Config/` 目录，否则 UI 会缺少皮肤和本地化字符串。

---

## AI 辅助开发说明

本项目从代码重构、架构迁移（C++17 / CMake / v145）到 Win8Metro 功能开发、构建、打包和文档，全流程采用 **AI 辅助编程**。以下是实际参与过本项目的 AI 模型清单及其分工，确保透明公开。

### 参与过的 AI 模型

| AI 模型 | 类型 | 主要参与环节 | 贡献比例（估算） |
|---------|------|-------------|----------------|
| **Trae（字节跳动）** | IDE 内置 AI 编程助手 | 全流程主力：代码重构、CMake 迁移、vs 工程 v145 升级、x64-only 架构移除 Win32/ARM、C++17 modern track 迁移、Rust dismcore FFI 绑定、Win8Metro 功能页开发、打包脚本、README 双语文档、GitHub Release 发布 | ≈ 70% |
| **DeepSeek-V3** | 在线对话大模型 | C++ 模板元编程调优、CRTP 基类继承链重构、Win32 API 封装层去重、Rust → C++ FFI 签名推导 | ≈ 15% |
| **GPT-4o** | 在线对话大模型 | DuiLib 样式表 Style XML schema 设计、多语言 StringMgr 热切换方案、WebView2 现代轨道 API 适配 | ≈ 8% |
| **Claude 3.5 Sonnet** | 在线对话大模型 | CBS 托管层异常处理补全、WinPE 环境兼容性代码、Sysprep 无人值守 XML 模板生成 | ≈ 5% |
| **qwen-plus** | 在线对话大模型 | 中文 README 润色、已知问题表整理、技术栈文案校对 | ≈ 2% |

> **说明**：以上模型通过 Trae IDE 插件、Browser MCP、CLI 对话三种方式接入，最终代码均经人工 review 和本地编译验证。未引入不可溯源的"黑盒"生成。

### AI 参与的里程碑

| 里程碑 | 时间 | 参与模型 | 产出 |
|--------|------|---------|------|
| DuiLib vcxproj 迁移 v145 | 2026-10 | Trae + DeepSeek-V3 | 12 个 .vcxproj 批量升级 PlatformToolset |
| CMakeLists.txt 重写 | 2026-10 | Trae | 原生 x64-only / C++17 门禁（`FATAL_ERROR` 架构非 x64 时） |
| Rust dismcore FFI | 2026-10 | Trae + DeepSeek-V3 | `duilibcalc.dll` 25 函数导出 + C++ 头文件绑定 |
| Win8Metro 功能页 15 个 | 2026-11 | Trae + GPT-4o | page_cleanup / page_driver / page_tools 等 XML + C++ 逻辑 |
| README 双语文档 | 2026-11 | Trae + qwen-plus | 中文 + English README，含架构图 |
| GitHub 仓库 / Release | 2026-11 | Trae (gh CLI) | 新建仓库 / 公开 / Release v2026.11.1 |

### 人工审核原则

AI 生成的代码在合入前必须满足：

1. ✅ **人工逐文件 diff review**：不得整包直接提交
2. ✅ **本地编译通过**：MSBuild + CMake 双链路无警告
3. ✅ **功能验证**：Win8Metro 启动、清理、驱动扫描三条冒烟测试
4. ✅ **无安全风险**：Rust FFI 边界检查、注册表操作白名单、DLL 名称避免系统冲突
5. ✅ **可溯源**：git commit message 标注 AI 参与（如 `[AI-assisted] CMakeLists.txt rewrite`）

---

## 已知问题

| # | 问题 | 影响 | 解决 / 绕过 |
|---|------|------|------------|
| ⚠️ | **Windows 10 上无法启动** | 已知 Bug：Win8Metro 在 **所有 Windows 10 版本**（21H2 / 22H2 / 19044 / 19045）上双击启动后进程立即退出，无任何窗口 | **正在修复中** — 初步定位为 WebView2Loader + DuiLib FrameWnd 初始化顺序竞态。临时绕过：在 Win11 上运行，或使用 `Release x64`（非 Release23）编译 |
| 1 | **Demos 无 CMakeLists.txt** | CMake 构建会跳过所有 Demo 项目，只产出 DuiLib 库 | Demo 必须用 `DuiLib.sln` 编译；CMake 仅适合构建核心库 |
| 2 | **源码编码不统一** | 仓库同时存在 UTF-8 / UTF-8-BOM / GBK / UTF-16LE 四种编码（约 49 个 GBK 文件） | 修改前请先用编辑器确认编码；推荐统一转 UTF-8 |
| 3 | **源码改写中转义字符被展开** | `tools/patch_*.py` 系列脚本会静默损坏源码中的转义字符（路径吃掉一个字符等） | 改代码后务必 diff 检查；建议先备份 |
| 4 | **x64-only 强制** | 在 x86 / ARM / ARM64 机器上编译会被拒绝 | 必须 x64 CPU + x64 Windows |
| 5 | **VS 生成器找不到** | `cmake -G "Visual Studio 18 2026"` 报 `could not find any instance of Visual Studio` | 改用 Ninja（先 `vcvars64.bat`），或在 VS Installer 里修复安装 |
| 6 | **duilibcalc 链接** | 手动 link Win8Metro 时，如果 Rust 模块尚未编译，会报 `duilibcalc.dll.lib not found` | 先 `cargo build --release` 并把 `duilibcalc.dll.lib` 拷到 `bin64/amd64/` |
| 7 | **Windows 7 / 8 / 8.1 不支持** | `_WIN32_WINNT=0x0A00` 硬编码；**最低支持 Win10 21H2 (Build 19044)** | 必须 Windows 10 21H2 或更新；CBSHost / NCleaner 虽有 `_WIN32_WINNT=0x0601` 定义，但主程序不兼容 Win7 |
| 8 | **便携版缺失运行时** | 目标机器没有 VC++ 运行时或 WebView2 Runtime 会报缺少 DLL | 安装 VC++ 2026 Redistributable 和 WebView2 Runtime |
| 9 | **WebView2 Loader** | Win10 21H2 及更早版本未预装 WebView2 Runtime | 首次启动时 `WebView2Loader.dll` 会尝试自动下载，如失败请手动安装 |
| 10 | **多语言缺失** | 未复制 `lang/` 目录时 UI 显示 `[zh-CN:xxx]` 占位符 | 部署时确保完整拷贝 `lang/` 目录 |

---

## 目录结构

```
dism-c17-2026/
├── .gitignore
├── CMakeLists.txt              # CMake 根（仅构建 DuiLib 库）
├── DuiLib.sln                  # VS 2026 解决方案（全量）
├── Directory.Build.props       # 全局 /MP 多核编译加速
├── build.cmd                   # 一键构建
├── README.md                   # 中文说明
├── README_EN.md                # 英文说明
├── LICENSE
│
├── DuiLib/                     # 核心 UI 库源码
│   ├── Core/ Control/ Layout/ Utils/ Style/
│   ├── DuiLib.vcxproj
│   └── WebView2.h              # WebView2 集成接口
│
├── 3rd/                        # 第三方依赖
│   ├── CxImage/                # 图像解码
│   ├── WebView2/               # Edge WebView2 SDK
│   └── wimgapi/                # WIM 处理
│
├── Demos/                      # 应用源码
│   ├── Win8Metro/              # 主程序（~200 .cpp）
│   ├── ActivationKit/          # 激活辅助 DLL
│   ├── CBSHost/                # DISM 托管 DLL
│   ├── NCleaner/               # 清理引擎 DLL
│   ├── SkinThemeDll/           # 皮肤引擎 DLL
│   └── ThemeManager/           # 主题管理 DLL
│
├── rust/
│   └── dismcore/               # Rust 计算模块 (duilibcalc)
│       ├── Cargo.toml
│       └── src/
│
├── templates/translations/     # 翻译术语表
│
├── bin64/                      # 编译输出
│   ├── Win8Metro.exe           # 主程序
│   ├── ThemeManager.dll        # 主题管理
│   ├── amd64/                  # 核心 DLL/LIB
│   └── lang/                   # 多语言包 DLL
│
├── lib/                        # 静态库输出
│
└── dist/
    └── Win8Metro_2026.11.1_x64_便携版/   # 完整便携版发布包
```

---

## 许可协议

本项目源代码基于 **MIT License** 发布，详见 `LICENSE` 文件。

Win8Metro 便携版发布包中可能包含的第三方组件（WebView2 Runtime、WOFADK.SYS、bcdboot.exe、efisys.bin 等）保持其各自的许可协议。
