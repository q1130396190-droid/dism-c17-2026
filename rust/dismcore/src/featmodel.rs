//! featmodel —— Windows 功能页（Feature）的独立模型组件。
//!
//! 从 `Page_Feature.cpp` 剥离出来的**纯计算**部分：
//!   * 树形聚合状态（父节点的"全开/全关/全移除/混合"）
//!   * 权威结果与现有列表的差异比对（用于跳过无意义的整表重建）
//!
//! 不迁移的部分及原因：
//!   * DuiLib 控件创建 / 布局 / 绘制 —— C++ UI 框架对象，Rust 无渲染能力
//!   * `SetVisible` 等可见性操作 —— 本质是操作控件，不属于"计算"
//!   * DISM 会话与枚举 —— 已有 `DismApiBridge` 封装
//!
//! 聚合语义（与 C++ 原实现逐条对应）：
//!   叶子：eff = pending != -1 ? pending : (enabled ? 1 : 0)；若 eff==2（永久移除）
//!         则记 3，区分"移除"与"启用/禁用"。
//!   父：  全开→1，全关→0，全移除→3，其余→2（混合）。

use std::os::raw::c_int;

/// 聚合状态常量（与 C++ 侧 `m_vAggCache` 取值一致）。
const ST_OFF: u8 = 0;
const ST_ON: u8 = 1;
const ST_MIXED: u8 = 2;
const ST_REMOVED: u8 = 3;

/// 计算整棵树的聚合状态。
///
/// 输入（长度均为 `n`）：
///   * `pending`：-1 不变 / 0 禁用 / 1 启用 / 2 永久移除
///   * `enabled`：非 0 表示当前已启用
///   * `parent` ：父节点下标，-1 表示根
///
/// 输出：`out_agg` 每项一个字节，取值见上方 ST_*。
///
/// 返回：写入节点数；`-1` 输入非法（父下标越界、或父子构成环）。
///
/// 实现说明：不依赖"子节点下标必然大于父节点"这一隐含约定——虽然当前
/// `ApplyRawData` 确实按父先子后建立索引，但那只是构造顺序的副产品，
/// 一旦将来改成别的插入顺序，靠下标顺序的算法会静默算错。这里用显式
/// 栈做后序遍历，只依赖 parent 关系本身。
///
/// # Safety
/// 所有指针须指向调用方保证有效、可读写对应长度的内存。
#[no_mangle]
pub unsafe extern "C" fn dismcore_feat_agg(
    pending: *const c_int,
    enabled: *const u8,
    parent: *const c_int,
    n: usize,
    out_agg: *mut u8,
    out_cap: usize,
) -> isize {
    if n == 0 {
        return 0;
    }
    if pending.is_null() || enabled.is_null() || parent.is_null() || out_agg.is_null() {
        return -1;
    }
    if out_cap < n {
        return -2;
    }

    let pend = unsafe { std::slice::from_raw_parts(pending, n) };
    let en = unsafe { std::slice::from_raw_parts(enabled, n) };
    let par = unsafe { std::slice::from_raw_parts(parent, n) };

    // 建子节点邻接表；同时校验 parent 合法性。
    let mut children: Vec<Vec<usize>> = vec![Vec::new(); n];
    for i in 0..n {
        let p = par[i];
        if p < 0 {
            continue; // 根
        }
        let pu = p as usize;
        if pu >= n {
            return -1; // 父下标越界
        }
        children[pu].push(i);
    }

    let out = unsafe { std::slice::from_raw_parts_mut(out_agg, out_cap) };

    // 显式栈后序遍历：state 0=未访问 1=子已入栈待处理 2=已完成
    let mut mark = vec![0u8; n];
    let mut stack: Vec<(usize, bool)> = Vec::with_capacity(n);

    for root in 0..n {
        if par[root] >= 0 || mark[root] == 2 {
            continue; // 只从根出发；已完成的跳过
        }
        stack.push((root, false));
        while let Some((node, expanded)) = stack.pop() {
            if expanded {
                // 子节点此时都已算完，可以汇总
                if children[node].is_empty() {
                    let eff: u8 = if pend[node] != -1 {
                        // pending 只有 0/1/2 三值；2 表示永久移除
                        pend[node] as u8
                    } else if en[node] != 0 {
                        ST_ON
                    } else {
                        ST_OFF
                    };
                    out[node] = if eff == 2 { ST_REMOVED } else { eff };
                } else {
                    let total = children[node].len();
                    let mut n_on = 0usize;
                    let mut n_off = 0usize;
                    let mut n_rm = 0usize;
                    for &c in &children[node] {
                        match out[c] {
                            ST_ON => n_on += 1,
                            ST_OFF => n_off += 1,
                            ST_REMOVED => n_rm += 1,
                            _ => {}
                        }
                    }
                    out[node] = if n_on == total {
                        ST_ON
                    } else if n_off == total {
                        ST_OFF
                    } else if n_rm == total {
                        ST_REMOVED
                    } else {
                        ST_MIXED
                    };
                }
                mark[node] = 2;
                continue;
            }

            if mark[node] != 0 {
                // 已入过栈（成环或重复入栈）：环会导致父永远等不到子，直接拒绝
                return -1;
            }
            mark[node] = 1;
            stack.push((node, true));
            for &c in children[node].iter().rev() {
                if mark[c] == 1 {
                    return -1; // 检测到环
                }
                stack.push((c, false));
            }
        }
    }

    // 若仍有未完成节点，说明存在从环上无法到达根的孤立结构
    for i in 0..n {
        if mark[i] != 2 {
            return -1;
        }
    }
    n as isize
}

/// 统计两份 (系统名, 启用态) 列表的差异条数。
///
/// 用途：权威枚举结果到达时，先判断"是否真的变了"，没变就不重建列表
/// （避免列表跳变、滚动复位、展开态丢失）。
///
/// 输入：两份 `\0` 分隔的 UTF-8 系统名缓冲 + 两份 `u8` 启用态数组，
///       长度必须与各自名称条数一致。
///
/// 返回：变化的条目数（新增 / 删除 / 状态翻转都计 1）；
///       `-1` 输入非法；`-2` 输出缓冲不足。
///
/// # Safety
/// 所有指针须指向调用方保证有效、可读写对应长度的内存。
#[no_mangle]
pub unsafe extern "C" fn dismcore_feat_diff(
    old_sys: *const u8,
    old_sys_len: usize,
    old_en: *const u8,
    old_n: usize,
    new_sys: *const u8,
    new_sys_len: usize,
    new_en: *const u8,
    new_n: usize,
    out_changed: *mut u32,
) -> isize {
    if out_changed.is_null() {
        return -1;
    }
    let split = |buf: *const u8, len: usize| -> Vec<String> {
        if buf.is_null() || len == 0 {
            return Vec::new();
        }
        unsafe { std::slice::from_raw_parts(buf, len) }
            .split(|&b| b == 0)
            .map(|s| String::from_utf8_lossy(s).into_owned())
            .collect()
    };

    let old_names = split(old_sys, old_sys_len);
    let new_names = split(new_sys, new_sys_len);
    if old_names.len() != old_n || new_names.len() != new_n {
        return -1;
    }
    if (old_n > 0 && old_en.is_null()) || (new_n > 0 && new_en.is_null()) {
        return -1;
    }

    let old_states: &[u8] = if old_n == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(old_en, old_n) }
    };
    let new_states: &[u8] = if new_n == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(new_en, new_n) }
    };

    let mut old_map: std::collections::HashMap<&str, u8> =
        std::collections::HashMap::with_capacity(old_names.len());
    for i in 0..old_names.len() {
        old_map.insert(old_names[i].as_str(), old_states[i]);
    }

    let mut changed: u32 = 0;
    let mut seen: std::collections::HashSet<&str> = std::collections::HashSet::new();
    for i in 0..new_names.len() {
        let key = new_names[i].as_str();
        seen.insert(key);
        match old_map.get(key) {
            Some(&st) => {
                if st != new_states[i] {
                    changed += 1; // 状态翻转
                }
            }
            None => changed += 1, // 新增
        }
    }
    // 删除：旧有新无
    for k in old_map.keys() {
        if !seen.contains(k) {
            changed += 1;
        }
    }

    unsafe { *out_changed = changed; }
    0
}

/// 版本标记：逻辑变更时递增，C++ 侧可断言与头文件一致。
#[no_mangle]
pub extern "C" fn dismcore_featmodel_version() -> u32 {
    0x0001_0000
}

#[cfg(test)]
mod tests {
    use super::*;

    fn agg(pending: &[i32], enabled: &[u8], parent: &[i32]) -> Vec<u8> {
        let n = pending.len();
        let mut out = vec![9u8; n];
        let rc = unsafe {
            dismcore_feat_agg(
                pending.as_ptr(), enabled.as_ptr(), parent.as_ptr(),
                n, out.as_mut_ptr(), out.len(),
            )
        };
        assert_eq!(rc, n as isize);
        out
    }

    #[test]
    fn leaf_states_follow_enabled_and_pending() {
        // 0: 已启用；1: 未启用；2: pending=2（永久移除）
        let out = agg(&[-1, -1, 2], &[1, 0, 0], &[-1, -1, -1]);
        assert_eq!(out, vec![ST_ON, ST_OFF, ST_REMOVED]);
    }

    #[test]
    fn parent_all_on_is_on() {
        // 父 0，子 1、2 都启用
        let out = agg(&[-1, -1, -1], &[0, 1, 1], &[-1, 0, 0]);
        assert_eq!(out[0], ST_ON);
    }

    #[test]
    fn parent_all_off_is_off() {
        let out = agg(&[-1, -1, -1], &[0, 0, 0], &[-1, 0, 0]);
        assert_eq!(out[0], ST_OFF);
    }

    #[test]
    fn parent_mixed_is_mixed() {
        let out = agg(&[-1, -1, -1], &[0, 1, 0], &[-1, 0, 0]);
        assert_eq!(out[0], ST_MIXED);
    }

    #[test]
    fn aggregation_independent_of_child_index_order() {
        // 子节点下标小于父节点：若实现依赖"子>父"的隐含约定，这里会算错
        // 0、1 是子，2 是父
        let out = agg(&[-1, -1, -1], &[1, 1, 0], &[2, 2, -1]);
        assert_eq!(out[2], ST_ON, "父应由子节点聚合得出，与下标顺序无关");
    }

    #[test]
    fn rejects_out_of_range_parent() {
        let n = 1usize;
        let mut out = [0u8; 1];
        let rc = unsafe {
            dismcore_feat_agg([-1].as_ptr(), [0u8].as_ptr(), [5].as_ptr(), n, out.as_mut_ptr(), 1)
        };
        assert_eq!(rc, -1);
    }

    fn diff(old: &[(&str, u8)], new: &[(&str, u8)]) -> u32 {
        let build = |v: &[(&str, u8)]| -> (Vec<u8>, Vec<u8>) {
            let mut names = Vec::new();
            let mut st = Vec::new();
            for (i, (n, s)) in v.iter().enumerate() {
                if i > 0 {
                    names.push(0);
                }
                names.extend_from_slice(n.as_bytes());
                st.push(*s);
            }
            (names, st)
        };
        let (on, os) = build(old);
        let (nn, ns) = build(new);
        let mut changed = 0u32;
        let rc = unsafe {
            dismcore_feat_diff(
                on.as_ptr(), on.len(), os.as_ptr(), os.len(),
                nn.as_ptr(), nn.len(), ns.as_ptr(), ns.len(),
                &mut changed,
            )
        };
        assert_eq!(rc, 0);
        changed
    }

    #[test]
    fn diff_identical_is_zero() {
        assert_eq!(diff(&[("A", 1), ("B", 0)], &[("A", 1), ("B", 0)]), 0);
    }

    #[test]
    fn diff_counts_flip_add_remove() {
        // A 翻转、B 删除、C 新增 => 3
        assert_eq!(diff(&[("A", 0), ("B", 0)], &[("A", 1), ("C", 0)]), 3);
    }
}
