种子缓存目录（seed）
====================

本目录用于放置"出厂枚举快照"，让全新机器首次进入页面时也能秒开列表，
不必等 3~8 秒的 DISM 枚举。

适用页面：可选功能（Capability）、Windows 功能（Feature）。


一、放什么文件
--------------
  seed_cap.dat    可选功能种子（对应"可选功能"页）
  seed_feat.dat   Windows 功能种子（对应"Windows 功能"页）

两个文件都是可选的：缺哪个就哪个页面走正常枚举，不影响其他功能。


二、文件格式
------------
两个文件格式相同，都是 UTF-8 + CRLF 的纯文本，三部分：

  第 1 行  指纹：  SEED<TAB><OSBUILD>|<SKU>|<LANG>
  第 2 行  数据头：CAPCACHE<TAB><epoch>      （seed_cap.dat）
                   FEATCACHE<TAB><epoch>      （seed_feat.dat）
  第 3 行起 条目：  与磁盘缓存同构的 TSV


三、指纹怎么取（关键）
----------------------
指纹只能从"将来要运行它"的那台机器上取，猜是猜不出来的。
取法：在本机命令行执行

  Win8Metro.exe /DumpSeedFingerprint

它会直接输出该机器应写入的首行，例如：

  SEED	19045|0|2052

把这一整行原样作为种子文件第 1 行（连同 SEED 与后面的制表符）。

字段含义：
  OSBUILD  OS 主 build 号，如 19045 / 26100
  SKU      0 = 客户端(WinNT)，1 = 服务器(ServerNT / LanManNT)
  LANG     UI 语言 LANGID 十进制，如 2052 = zh-CN，1033 = en-US


四、指纹不匹配会怎样
--------------------
加载端会**直接拒绝**该种子，页面回到正常枚举（转圈 → 枚举完成）。

这是有意设计：功能/能力清单与 OS build、SKU、UI 语言强相关，
指纹不符时若仍然渲染，会出现"幻影条目"（列着实际不存在的功能）
或漏项。宁可慢一点，也不能显示错的。


五、种子与磁盘缓存的区别
------------------------
  磁盘缓存  %LOCALAPPDATA%\DismTool_Clone\feat_enum_cache.dat / cap_enum_cache.dat
            - 有 3600s TTL，过期即失效
            - 由本机枚举结果自动写入，天然匹配本机
            - 命中后**短路**后台枚举（视为新鲜）

  种子缓存  本目录，随包分发、只读
            - 无 TTL（出厂数据不会"过期"）
            - 受指纹严格约束
            - 命中后**仍会**发起后台枚举做校正（种子可能滞后于实机补丁）

所以：种子解决"首次进页等待"，不解决"数据永远最新"。


六、怎么生成种子文件
--------------------
[R84] 现在有自动生成器了：`/MakeSeed`。它会跑一次真实枚举，按加载端格式
写好两个种子文件，**并立刻用加载端自己的 Load*SeedCache 回读验证**——
格式不对会直接报错，不会留下"生成了但读不了"的文件。

  1. 以管理员身份运行（枚举需要 DISM 权限）：
       Win8Metro.exe /MakeSeed
  2. 看结果（控制台无输出时读文件）：
       %TEMP%\win8metro_make_seed.txt
     关注这几行必须是 1：
       seed.feat.reload.ok=1
       seed.cap.reload.ok=1
       seed.stamp.ok=1
       seed.ok=1
  3. 生成物直接落在本目录：
       seed_feat.dat / seed_cap.dat / seed_stamp.txt

若失败，文件里会写明 `seed.init.hr=0x...`：
  0x800702E4 = 未提权；其它码见 DISM API 文档。

### 什么时候要重新生成

种子是**某一时刻的快照**。装/卸过 Windows 功能或可选功能后，种子里就是旧状态。
[R85] 加了 `seed_stamp.txt` 记录生成时的系统状态：

    filemaps=<时间戳>|cbseq=<序号>

页面加载种子时会比对这两个信号（读取实测 0ms）：
  - 一致   → 种子当作新鲜，**跳过枚举**（首屏实测 72ms）
  - 不一致 → 判为过期，**仍然枚举**（首屏约 1162ms，但不会显示错的勾选状态）

所以**忘了重新生成不会显示错误状态**，只会退化成慢一些。
两个信号的选择有实测依据（装一个 TFTP 功能前后对比）：
  - `WinSxS\FileMaps` 目录时间戳  —— 变了（采用）
  - CBS `NextExecutionSequence`   —— 变了（采用）
  - `WinSxS` 目录本身时间戳       —— **没变（不采用，只看它会漏判）**



七、体积与上限
--------------
正常种子 ≤ 40 KB。加载端设 4 MB 上限，仅用于防御损坏或误放的大文件。


八、验证是否生效
----------------
打包后在新机器上运行，查看调试输出（OutputDebugString）：

  [seed] cap  seed loaded (fingerprint matched)     命中
  [seed] cap  seed fingerprint mismatch, ignored    指纹不符（被拒）
  [seed] feat seed loaded (fingerprint matched)     命中
  [seed] feat seed fingerprint mismatch, ignored    指纹不符（被拒）

页面日志里也会出现：
  [capability] seed cache hit (first run), refreshing in background

九、为什么必须随包分发（[R87]）
------------------------------
没有种子的机器会走完整枚举，首屏约 1162ms；有新鲜种子时只要 72ms。
**差的 1090ms 全部在 DISM 枚举上，不是本程序能优化的。**

所以打包时必须把本目录 4 个文件全部带上：

    seed_feat.dat   功能枚举结果
    seed_cap.dat    可选功能枚举结果
    seed_stamp.txt  生成时的系统状态快照（缺了就无法判断新鲜度）
    README.txt      本文档

`tools/package_portable.ps1` 末尾有校验：包里带了 Win8Metro.exe
但缺 seed 目录或 seed_stamp.txt 时会打出告警，并给出修复命令。
该检查只告警不阻断——没有种子的构建是合法的，只是慢。

常见退化场景：把 bin64\Win8Metro.exe 单独拷给别人（不带 seed 目录），
对方首屏就会从 72ms 变成 1162ms，且没有任何报错提示。
