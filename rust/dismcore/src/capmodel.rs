//! capmodel —— 可选功能页（Capability）的独立模型组件。
//!
//! 从 `Page_Capability.cpp` 剥离出来的**纯计算**部分：
//!   * 过滤匹配（关键字 + 安装状态）
//!   * 排序（已安装优先，其次按名称）
//!
//! 不迁移的部分及原因（如实说明，避免误以为整页已转 Rust）：
//!   * DuiLib 控件树创建 / 布局 / 绘制 —— 属 C++ UI 框架对象，Rust 侧无渲染能力
//!   * DISM 会话与 API 调用 —— 已有 `DismApiBridge` 封装，重复实现无收益
//!   * 消息泵与定时器 —— Win32 窗口消息循环，与 UI 线程绑定
//!
//! 设计：跨 FFI 只传扁平缓冲（UTF-8 / u32 / u8），不传结构体、不共享所有权，
//! 避免 ABI 对齐与生命周期问题。

use std::os::raw::c_char;

/// 把以 `\0` 分隔的 UTF-8 缓冲切成字符串切片。
///
/// 安全前提：调用方保证 `buf[..len]` 是合法 UTF-8 且可读。
/// 无效 UTF-8 时退化为 lossy 转换，不 panic —— 跨 FFI 不该因输入脏数据崩溃。
fn split_nul(buf: &[u8]) -> Vec<&str> {
    buf.split(|&b| b == 0)
        .map(|s| std::str::from_utf8(s).unwrap_or(""))
        .collect()
}

/// 判断单个条目是否可见。
///
/// 规则（与 C++ 侧原实现逐条对应）：
///   * `show_all == 0` 时，仅 `state == "Installed"` 可见；
///   * `filter` 非空时，`identity` 或 `display` 任一包含该子串（大小写不敏感）即可见。
#[inline]
fn item_visible(state: &str, identity: &str, display: &str, show_all: bool, filter: &str) -> bool {
    let installed = state.eq_ignore_ascii_case("Installed");
    if !show_all && !installed {
        return false;
    }
    if filter.is_empty() {
        return true;
    }
    identity.to_lowercase().contains(filter) || display.to_lowercase().contains(filter)
}

/// 计算每个条目的可见性位图。
///
/// 输入：
///   * `states` / `identities` / `displays`：三段 `\0` 分隔的 UTF-8 缓冲，长度须一致；
///   * `show_all`：非 0 表示显示全部；
///   * `filter`：已小写的过滤词（空 = 不过滤）。
///
/// 输出：`out_visible` 每字节对应一个条目（1 可见 / 0 隐藏）。
///
/// 返回：写出的条目数；`-1` 输入非法；`-2` 输出缓冲不足。
///
/// # Safety
/// 所有指针须指向调用方保证有效、可读写对应长度的内存。
#[no_mangle]
pub unsafe extern "C" fn dismcore_cap_visibility(
    states: *const u8,
    states_len: usize,
    identities: *const u8,
    identities_len: usize,
    displays: *const u8,
    displays_len: usize,
    show_all: u32,
    filter: *const u8,
    filter_len: usize,
    out_visible: *mut u8,
    out_cap: usize,
) -> isize {
    if states.is_null() || identities.is_null() || displays.is_null() || out_visible.is_null() {
        return -1;
    }
    let st = split_nul(unsafe { std::slice::from_raw_parts(states, states_len) });
    let id = split_nul(unsafe { std::slice::from_raw_parts(identities, identities_len) });
    let dp = split_nul(unsafe { std::slice::from_raw_parts(displays, displays_len) });

    // 三段长度必须一致：不一致说明调用方拼装出错，直接拒绝而不是"取最短"糊过去。
    let n = st.len();
    if id.len() != n || dp.len() != n {
        return -1;
    }
    if out_cap < n {
        return -2;
    }

    let filter_raw = if filter.is_null() || filter_len == 0 {
        String::new()
    } else {
        std::str::from_utf8(unsafe { std::slice::from_raw_parts(filter, filter_len) })
            .unwrap_or("")
            .to_lowercase()
    };

    let out = unsafe { std::slice::from_raw_parts_mut(out_visible, out_cap) };
    for i in 0..n {
        out[i] = u8::from(item_visible(st[i], id[i], dp[i], show_all != 0, &filter_raw));
    }
    n as isize
}

/// 生成排序后的索引序列：已安装优先，其次按显示名（大小写不敏感）升序。
///
/// 输入同 [`dismcore_cap_visibility`]（不需要 `displays` 时传 identity 即可）。
/// 输出：`out_order` 每项一个 `u32` 下标。
///
/// 返回：写出的条目数；`-1` 输入非法；`-2` 输出缓冲不足。
///
/// # Safety
/// 同 [`dismcore_cap_visibility`]。
#[no_mangle]
pub unsafe extern "C" fn dismcore_cap_sort_order(
    states: *const u8,
    states_len: usize,
    displays: *const u8,
    displays_len: usize,
    out_order: *mut u32,
    out_cap: usize,
) -> isize {
    if states.is_null() || displays.is_null() || out_order.is_null() {
        return -1;
    }
    let st = split_nul(unsafe { std::slice::from_raw_parts(states, states_len) });
    let dp = split_nul(unsafe { std::slice::from_raw_parts(displays, displays_len) });
    let n = st.len();
    if dp.len() != n {
        return -1;
    }
    if out_cap < n {
        return -2;
    }

    let mut order: Vec<u32> = (0..n as u32).collect();
    order.sort_by(|&a, &b| {
        let ia = st[a as usize].eq_ignore_ascii_case("Installed");
        let ib = st[b as usize].eq_ignore_ascii_case("Installed");
        // 已安装的排前面
        ib.cmp(&ia)
            .then_with(|| dp[a as usize].to_lowercase().cmp(&dp[b as usize].to_lowercase()))
    });

    let out = unsafe { std::slice::from_raw_parts_mut(out_order, out_cap) };
    out[..n].copy_from_slice(&order);
    n as isize
}

/// 版本标记：随模型逻辑变更递增，C++ 侧启动时可断言与头文件声明一致。
#[no_mangle]
pub extern "C" fn dismcore_capmodel_version() -> u32 {
    0x0001_0000
}

/// 供 C++ 侧探测 ABI 是否就绪的哨兵（返回固定字符串长度）。
#[no_mangle]
pub extern "C" fn dismcore_capmodel_abi(_unused: *const c_char) -> u32 {
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn nul(items: &[&str]) -> Vec<u8> {
        let mut v = Vec::new();
        for (i, s) in items.iter().enumerate() {
            if i > 0 {
                v.push(0);
            }
            v.extend_from_slice(s.as_bytes());
        }
        v
    }

    #[test]
    fn installed_only_when_not_show_all() {
        let st = nul(&["Installed", "Not Present"]);
        let id = nul(&["A", "B"]);
        let dp = nul(&["a", "b"]);
        let mut out = [9u8; 2];
        let n = unsafe {
            dismcore_cap_visibility(
                st.as_ptr(), st.len(), id.as_ptr(), id.len(), dp.as_ptr(), dp.len(),
                0, std::ptr::null(), 0, out.as_mut_ptr(), out.len(),
            )
        };
        assert_eq!(n, 2);
        assert_eq!(out, [1, 0]);
    }

    #[test]
    fn filter_matches_identity_or_display_case_insensitive() {
        let st = nul(&["Installed", "Installed"]);
        let id = nul(&["OpenSSH.Client", "X"]);
        let dp = nul(&["x", "远程桌面"]);
        let f = "openssh".to_lowercase();
        let mut out = [9u8; 2];
        let n = unsafe {
            dismcore_cap_visibility(
                st.as_ptr(), st.len(), id.as_ptr(), id.len(), dp.as_ptr(), dp.len(),
                1, f.as_ptr(), f.len(), out.as_mut_ptr(), out.len(),
            )
        };
        assert_eq!(n, 2);
        assert_eq!(out, [1, 0]);
    }

    #[test]
    fn rejects_mismatched_lengths() {
        let st = nul(&["A", "B"]);
        let id = nul(&["only-one"]);
        let dp = nul(&["a", "b"]);
        let mut out = [0u8; 2];
        let n = unsafe {
            dismcore_cap_visibility(
                st.as_ptr(), st.len(), id.as_ptr(), id.len(), dp.as_ptr(), dp.len(),
                0, std::ptr::null(), 0, out.as_mut_ptr(), out.len(),
            )
        };
        assert_eq!(n, -1);
    }

    #[test]
    fn sort_puts_installed_first_then_by_name() {
        let st = nul(&["Not Present", "Installed", "Installed"]);
        let dp = nul(&["Zeta", "Beta", "Alpha"]);
        let mut out = [0u32; 3];
        let n = unsafe {
            dismcore_cap_sort_order(st.as_ptr(), st.len(), dp.as_ptr(), dp.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(n, 3);
        // 已安装的 Alpha(2)、Beta(1) 在前，未安装 Zeta(0) 在后
        assert_eq!(&out[..3], &[2, 1, 0]);
    }
}
