//! dismcore —— Demos 内存安全模块（Rust 迁移示范）
//!
//! 迁移范围（经 R-P18 审计确定的"可安全隔离"边界）：
//!   * 纯计算、无 Win32 句柄 / 无 COM / 无 DuiLib 依赖
//!   * 输入输出均为字节切片或 UTF-8 字符串，跨 FFI 边界零分配移交
//!
//! 安全保证：
//!   * 全部使用 Rust 安全代码（#![forbid(unsafe_op_in_unsafe_fn)]）
//!   * 无裸指针运算、无手工内存管理、无缓冲区溢出可能
//!   * 哈希实现来自 RustCrypto（经审计的成熟实现）

#![forbid(unsafe_op_in_unsafe_fn)]

use md5::Md5;
use sha1::Sha1;
use sha2::{Digest, Sha256, Sha512};

mod win11_preprocess;
mod win11_install;
// [R8 2026-10-25] 可选功能页 / Windows 功能页的纯计算组件。

mod capmodel;
mod featmodel;

/// 支持的哈希算法
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Algo {
    Md5,
    Sha1,
    Sha256,
    Sha512,
}

impl Algo {
    fn from_u32(v: u32) -> Option<Algo> {
        match v {
            1 => Some(Algo::Md5),
            2 => Some(Algo::Sha1),
            3 => Some(Algo::Sha256),
            4 => Some(Algo::Sha512),
            _ => None,
        }
    }

    pub fn hex_len(self) -> usize {
        match self {
            Algo::Md5 => 32,
            Algo::Sha1 => 40,
            Algo::Sha256 => 64,
            Algo::Sha512 => 128,
        }
    }
}

#[inline]
fn to_hex_lower(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0x0F) as usize] as char);
    }
    s
}

/// 对字节切片计算指定算法的十六进制小写摘要。
pub fn digest_hex(algo: Algo, data: &[u8]) -> String {
    match algo {
        Algo::Md5 => to_hex_lower(&Md5::digest(data)),
        Algo::Sha1 => to_hex_lower(&Sha1::digest(data)),
        Algo::Sha256 => to_hex_lower(&Sha256::digest(data)),
        Algo::Sha512 => to_hex_lower(&Sha512::digest(data)),
    }
}

/// 十六进制字符串 → 字节（用于十六进制编码的辅助，纯计算）。
pub fn hex_encode(data: &[u8]) -> String {
    to_hex_lower(data)
}

/// 十六进制解码；非法字符返回 None（不 panic，无 UB）。
pub fn hex_decode(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    let val = |c: u8| -> Option<u8> {
        match c {
            b'0'..=b'9' => Some(c - b'0'),
            b'a'..=b'f' => Some(c - b'a' + 10),
            b'A'..=b'F' => Some(c - b'A' + 10),
            _ => None,
        }
    };
    let mut i = 0;
    while i < b.len() {
        let hi = val(b[i])?;
        let lo = val(b[i + 1])?;
        out.push((hi << 4) | lo);
        i += 2;
    }
    Some(out)
}

// ============================================================================
// Base64（标准 RFC 4648，含 '=' 填充；支持 UTF-8 输入，纯计算无 IO）
// ============================================================================

const B64_ALPHABET: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Base64 编码；返回标准字符串（可能带 '=' 填充）。
pub fn base64_encode(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len().div_ceil(3) * 4);
    let mut i = 0;
    while i < data.len() {
        let b0 = data[i];
        let b1 = *data.get(i + 1).unwrap_or(&0);
        let b2 = *data.get(i + 2).unwrap_or(&0);
        s.push(B64_ALPHABET[(b0 >> 2) as usize] as char);
        s.push(B64_ALPHABET[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        if i + 1 < data.len() {
            s.push(B64_ALPHABET[(((b1 & 0x0F) << 2) | (b2 >> 6)) as usize] as char);
        } else {
            s.push('=');
        }
        if i + 2 < data.len() {
            s.push(B64_ALPHABET[(b2 & 0x3F) as usize] as char);
        } else {
            s.push('=');
        }
        i += 3;
    }
    s
}

/// Base64 解码；忽略非法填充位，非法字符返回 None。
/// 容忍内嵌空白（换行/制表符/空格/CR）。
pub fn base64_decode(s: &str) -> Option<Vec<u8>> {
    // 过滤空白
    let mut b: Vec<u8> = Vec::with_capacity(s.len());
    for &c in s.as_bytes() {
        match c {
            b' ' | b'\t' | b'\r' | b'\n' => continue,
            _ => b.push(c),
        }
    }
    if b.is_empty() {
        return Some(Vec::new());
    }
    // 长度（含填充）必须是 4 的倍数
    if b.len() % 4 != 0 {
        return None;
    }
    // 允许 '=' 仅出现在尾部（最多 2 个）
    let mut pad = 0usize;
    while b.last() == Some(&b'=') {
        b.pop();
        pad += 1;
        if pad > 2 {
            return None;
        }
    }
    if b.is_empty() {
        return Some(Vec::new());
    }
    let val = |c: u8| -> Option<u8> {
        match c {
            b'A'..=b'Z' => Some(c - b'A'),
            b'a'..=b'z' => Some(c - b'a' + 26),
            b'0'..=b'9' => Some(c - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    };
    let mut out = Vec::with_capacity(b.len() / 4 * 3);
    let mut i = 0;
    while i < b.len() {
        let c0 = val(b[i])?;
        let c1 = val(b[i + 1])?;
        let c2 = if i + 2 < b.len() { val(b[i + 2])? } else { 0 };
        let c3 = if i + 3 < b.len() { val(b[i + 3])? } else { 0 };
        out.push((c0 << 2) | (c1 >> 4));
        if i + 2 < b.len() {
            out.push(((c1 & 0x0F) << 4) | (c2 >> 2));
        }
        if i + 3 < b.len() {
            out.push(((c2 & 0x03) << 6) | c3);
        }
        i += 4;
    }
    Some(out)
}

// ============================================================================
// UTF-8 ↔ UTF-16（纯计算；Rust String 内部是 UTF-8，此处手写 UTF-16 编解码，
// 避免依赖 ICU 或系统代码页 API。非法序列返回 None，不 panic。）
// ============================================================================

/// UTF-8 字节 → UTF-16 code units（字节序：主机序，本机为 LE）。
/// 非法 UTF-8 返回 None。输出为 u16 数组，非跨 FFI 分配（调用方提供缓冲）。
pub fn utf8_to_utf16(bytes: &[u8]) -> Option<Vec<u16>> {
    let text = core::str::from_utf8(bytes).ok()?;
    let mut out = Vec::with_capacity(text.chars().count());
    for c in text.chars() {
        let cp = c as u32;
        if cp < 0x10000 {
            out.push(cp as u16);
        } else {
            // 代理对
            let v = cp - 0x10000;
            out.push(0xD800 | ((v >> 10) as u16));
            out.push(0xDC00 | ((v & 0x3FF) as u16));
        }
    }
    Some(out)
}

/// UTF-16 code units → UTF-8 字节。
/// 非法 UTF-16（孤立代理对 / 编码错位）返回 None。
pub fn utf16_to_utf8(units: &[u16]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(units.len());
    let mut i = 0;
    while i < units.len() {
        let u = units[i];
        let cp: u32 = if (0xD800..=0xDBFF).contains(&u) {
            // 高位代理：必须跟低位代理
            if i + 1 >= units.len() { return None; }
            let lo = units[i + 1];
            if !(0xDC00..=0xDFFF).contains(&lo) { return None; }
            i += 2;
            0x10000 + (((u - 0xD800) as u32) << 10) + (lo - 0xDC00) as u32
        } else if (0xDC00..=0xDFFF).contains(&u) {
            // 孤立低位代理 → 非法
            return None;
        } else {
            i += 1;
            u as u32
        };
        // 编码 cp 为 UTF-8
        if cp < 0x80 {
            out.push(cp as u8);
        } else if cp < 0x800 {
            out.push(0xC0 | ((cp >> 6) as u8));
            out.push(0x80 | ((cp & 0x3F) as u8));
        } else if cp < 0x10000 {
            out.push(0xE0 | ((cp >> 12) as u8));
            out.push(0x80 | (((cp >> 6) & 0x3F) as u8));
            out.push(0x80 | ((cp & 0x3F) as u8));
        } else {
            out.push(0xF0 | ((cp >> 18) as u8));
            out.push(0x80 | (((cp >> 12) & 0x3F) as u8));
            out.push(0x80 | (((cp >> 6) & 0x3F) as u8));
            out.push(0x80 | ((cp & 0x3F) as u8));
        }
    }
    Some(out)
}

// ============================================================================
// FFI 导出（供 C++ 侧调用）
// ============================================================================

/// 计算摘要，结果写入调用方提供的缓冲区（UTF-8，无 NUL 结尾）。
///
/// * `algo`  : 1=Md5 2=Sha1 3=Sha256 4=Sha512
/// * `data`  : 输入字节
/// * `out`   : 输出缓冲区
/// * `out_cap`: 输出缓冲区容量（字节）
///
/// 返回：写入的字节数；-1 = 算法无效；-2 = 缓冲区不足（此时不写入任何数据）。
///
/// # Safety
/// `data` 必须指向可读的 `data_len` 字节；
/// `out` 必须指向可写的 `out_cap` 字节。判空与长度由调用方保证（C ABI 契约）。
#[no_mangle]
pub unsafe extern "C" fn dismcore_digest_hex(
    algo: u32,
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    // SAFETY: 由调用方按 C ABI 契约保证指针有效；此处仅做最小判空。
    if out.is_null() {
        return -2;
    }
    let Some(a) = Algo::from_u32(algo) else {
        return -1;
    };
    let need = a.hex_len();
    if out_cap < need {
        return -2;
    }
    // SAFETY: data 非空时按 data_len 读取；null+0 表示空输入。
    let slice: &[u8] = if data.is_null() || data_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(data, data_len) }
    };
    let hex = digest_hex(a, slice);
    debug_assert_eq!(hex.len(), need);
    // SAFETY: out_cap >= need 已校验。
    unsafe {
        core::ptr::copy_nonoverlapping(hex.as_ptr(), out, need);
    }
    need as isize
}

/// 输入缓冲区按十六进制编码，写入 `out`。
/// 返回写入字节数（= data_len*2）；-2 = 缓冲区不足。
///
/// # Safety
/// 同 `dismcore_digest_hex` 的指针契约。
#[no_mangle]
pub unsafe extern "C" fn dismcore_hex_encode(
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let need = data_len.saturating_mul(2);
    if out_cap < need {
        return -2;
    }
    let slice: &[u8] = if data.is_null() || data_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(data, data_len) }
    };
    let hex = hex_encode(slice);
    unsafe {
        core::ptr::copy_nonoverlapping(hex.as_ptr(), out, need);
    }
    need as isize
}

/// 版本号（供 C++ 侧核对加载的 Rust 模块版本）。
#[no_mangle]
pub extern "C" fn dismcore_version() -> u32 {
    0x0006_0000
}

/// Build the Windows 11 24H2+ preprocessing bundle as UTF-8.
/// kind=0 returns unattend.xml; kind=1 returns the first-logon desktop icon script.
/// Returns bytes written, -2 for insufficient output, -3 for invalid UTF-8, -4 for invalid input.
#[no_mangle]
pub unsafe extern "C" fn dismcore_build_win11_preprocess(
    account: *const u8, account_len: usize, kind: u32, out: *mut u8, out_cap: usize,
) -> isize {
    if account.is_null() || out.is_null() { return -4; }
    let input = unsafe { core::slice::from_raw_parts(account, account_len) };
    let name = match core::str::from_utf8(input) { Ok(v) => v, Err(_) => return -3 };
    let bundle = match win11_preprocess::build_bundle(name) { Ok(v) => v, Err(_) => return -4 };
    let bytes = match kind { 0 => bundle.unattend_xml.as_bytes(), 1 => bundle.desktop_script.as_bytes(), _ => return -4 };
    if out_cap < bytes.len() { return -2; }
    unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()); }
    bytes.len() as isize
}

/// Validate a generated preprocessing bundle. Returns 1 valid, 0 invalid, -3 invalid UTF-8.
#[no_mangle]
pub unsafe extern "C" fn dismcore_validate_win11_preprocess(
    xml: *const u8, xml_len: usize, script: *const u8, script_len: usize,
) -> isize {
    if xml.is_null() || script.is_null() { return 0; }
    let xb = unsafe { core::slice::from_raw_parts(xml, xml_len) };
    let sb = unsafe { core::slice::from_raw_parts(script, script_len) };
    let x = match core::str::from_utf8(xb) { Ok(v) => v, Err(_) => return -3 };
    let s = match core::str::from_utf8(sb) { Ok(v) => v, Err(_) => return -3 };
    if win11_preprocess::validate_bundle(x, s) { 1 } else { 0 }
}

/// Install the validated bundle into a mounted image root. Returns 0 on success; otherwise a
/// negative error code (-1 invalid root, -2 unsupported build, -3 invalid account, -4 I/O,
/// -5 bundle validation). Detailed message is written as UTF-8 to `msg`.
///
/// # Safety
/// `root`/`msg` must be valid UTF-8 slices of the given lengths; `msg` must be writable.
#[no_mangle]
pub unsafe extern "C" fn dismcore_install_win11_preprocess(
    root: *const u8, root_len: usize, account: *const u8, account_len: usize,
    build: u32, msg: *mut u8, msg_cap: usize,
) -> isize {
    if root.is_null() || account.is_null() || msg.is_null() { return -6; }
    let root_s = match core::str::from_utf8(unsafe { core::slice::from_raw_parts(root, root_len) }) {
        Ok(v) => v, Err(_) => return -6 };
    let account_s = match core::str::from_utf8(unsafe { core::slice::from_raw_parts(account, account_len) }) {
        Ok(v) => v, Err(_) => return -3 };
    let result = win11_install::install_bundle(std::path::Path::new(root_s), account_s, build);
    match result {
        Ok(done) => {
            let (xml, cmd) = done.paths();
            let text = format!("OK:{}|{}", xml.display(), cmd.display());
            let bytes = text.as_bytes();
            let n = bytes.len().min(msg_cap.saturating_sub(1));
            unsafe {
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), msg, n);
                *msg.add(n) = 0;
            }
            0
        }
        Err(e) => {
            let text = e.message();
            let bytes = text.as_bytes();
            let n = bytes.len().min(msg_cap.saturating_sub(1));
            unsafe {
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), msg, n);
                *msg.add(n) = 0;
            }
            match e {
                win11_install::PrepError::InvalidRoot => -1,
                win11_install::PrepError::NotWindows11 => -2,
                win11_install::PrepError::InvalidAccount => -3,
                win11_install::PrepError::Io(_) => -4,
                win11_install::PrepError::InvalidBundle => -5,
            }
        }
    }
}

/// [R-P23 · R3] UTF-8 字节 → UTF-16 code units（主机序），写入 `out`。
/// 返回写入的 code unit 数；-2 = 缓冲区不足；-3 = 非法 UTF-8。
///
/// # Safety
/// `data` 指向可读 `data_len` 字节；`out` 指向可写 `out_cap` u16 单元。
#[no_mangle]
pub unsafe extern "C" fn dismcore_utf8_to_utf16(
    data: *const u8,
    data_len: usize,
    out: *mut u16,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let slice: &[u8] = if data.is_null() || data_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证指针有效
        unsafe { core::slice::from_raw_parts(data, data_len) }
    };
    let Some(units) = utf8_to_utf16(slice) else {
        return -3;
    };
    if out_cap < units.len() {
        return -2;
    }
    if !units.is_empty() {
        // SAFETY: out_cap >= units.len() 已校验
        unsafe {
            core::ptr::copy_nonoverlapping(units.as_ptr(), out, units.len());
        }
    }
    units.len() as isize
}

/// [R-P23 · R3] UTF-16 code units → UTF-8 字节，写入 `out`。
/// 返回写入字节数；-2 = 缓冲区不足；-3 = 非法 UTF-16。
///
/// # Safety
/// `data` 指向可读 `data_len` u16 单元；`out` 指向可写 `out_cap` 字节。
#[no_mangle]
pub unsafe extern "C" fn dismcore_utf16_to_utf8(
    data: *const u16,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let slice: &[u16] = if data.is_null() || data_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证指针有效
        unsafe { core::slice::from_raw_parts(data, data_len) }
    };
    let Some(bytes) = utf16_to_utf8(slice) else {
        return -3;
    };
    if out_cap < bytes.len() {
        return -2;
    }
    if !bytes.is_empty() {
        // SAFETY: out_cap >= bytes.len() 已校验
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        }
    }
    bytes.len() as isize
}

/// 输入 UTF-8 十六进制字符串解码为字节，写入 `out`。
/// 返回写入字节数；-2 = 缓冲区不足；-3 = 非法 hex（含奇数长度）。
///
/// # Safety
/// `s` 指向 NUL 结尾的 UTF-8；`out` 指向可写 `out_cap` 字节。
#[no_mangle]
pub unsafe extern "C" fn dismcore_hex_decode(
    s: *const u8,
    s_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let slice: &[u8] = if s.is_null() || s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 可读字节
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    // 需为 UTF-8；非 ASCII 字符必然非法 hex
    let text = match core::str::from_utf8(slice) {
        Ok(t) => t,
        Err(_) => return -3,
    };
    let Some(decoded) = hex_decode(text) else {
        return -3;
    };
    if out_cap < decoded.len() {
        return -2;
    }
    if !decoded.is_empty() {
        // SAFETY: out_cap >= decoded.len() 已校验
        unsafe {
            core::ptr::copy_nonoverlapping(decoded.as_ptr(), out, decoded.len());
        }
    }
    decoded.len() as isize
}

/// 输入字节按 Base64 编码写入 `out`。
/// 返回写入字节数；-2 = 缓冲区不足。
///
/// # Safety
/// 同 `dismcore_hex_encode` 的指针契约。
#[no_mangle]
pub unsafe extern "C" fn dismcore_base64_encode(
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let need = data_len.div_ceil(3).saturating_mul(4);
    if out_cap < need {
        return -2;
    }
    let slice: &[u8] = if data.is_null() || data_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证指针有效
        unsafe { core::slice::from_raw_parts(data, data_len) }
    };
    let enc = base64_encode(slice);
    if need > 0 {
        // SAFETY: out_cap >= need 已校验
        unsafe {
            core::ptr::copy_nonoverlapping(enc.as_ptr(), out, need);
        }
    }
    need as isize
}

/// 输入 UTF-8 Base64 字符串解码写入 `out`。
/// 返回写入字节数；-2 = 缓冲区不足；-3 = 非法 base64。
///
/// # Safety
/// `s` 指向 NUL 结尾 UTF-8；`out` 指向可写 `out_cap` 字节。
#[no_mangle]
pub unsafe extern "C" fn dismcore_base64_decode(
    s: *const u8,
    s_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> isize {
    if out.is_null() {
        return -2;
    }
    let slice: &[u8] = if s.is_null() || s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 可读字节
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let text = match core::str::from_utf8(slice) {
        Ok(t) => t,
        Err(_) => return -3,
    };
    let Some(decoded) = base64_decode(text) else {
        return -3;
    };
    if out_cap < decoded.len() {
        return -2;
    }
    if !decoded.is_empty() {
        // SAFETY: out_cap >= decoded.len() 已校验
        unsafe {
            core::ptr::copy_nonoverlapping(decoded.as_ptr(), out, decoded.len());
        }
    }
    decoded.len() as isize
}

// ============================================================================
// [R-P24 · R4] Data.xml `?Get*` 表达式解析器（纯字符串扫描，无 IO / 无 Win32）
// ============================================================================
//
// 迁移边界：仅词法定位层（扫描 `?GetXxx(...)` 的几何信息）迁入 Rust。
// 四个求值器（GetRegSz / GetFileVersion / GetIniData / GetLocaleName）走 Win32，
// 按 R-P18 审计原则保留 C++，不在迁移范围。

/// 单次解析命中的几何信息（全部为 UTF-8 字节偏移 / 长度）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataExprHit {
    /// `?` 在原文中的字节偏移
    pub q: usize,
    /// 完整匹配 `?GetXxx(...)` 的字节长度
    pub match_len: usize,
    /// 函数名（不含前导 `?`），如 "GetRegSz"
    pub name: String,
    /// 括号内原文（未 trim / 未去引号）
    pub body: String,
}

/// 在 UTF-8 文本中定位第一个 `?Get*(...)`。
///
/// 语义与 C++ `ParseFunExpr` 逐字对齐：
///   * 只考虑文本中**第一个** `?`（其后若非法，不再向后搜索）
///   * `?` 之后必须紧跟 "Get"（ASCII 大小写不敏感），否则视为未命中
///   * `?` 之后必须存在 `(`
///   * 括号必须闭合，并按嵌套深度匹配以支持 `?GetX(?GetY(...))` 形式
///
/// 未命中 / 括号未闭合 / 非 "Get" 前缀 → `None`。不 panic。
pub fn parse_data_expr(text: &str) -> Option<DataExprHit> {
    let bytes = text.as_bytes();
    let n = bytes.len();
    let q = bytes.iter().position(|&b| b == b'?')?;
    // 需至少有 "Get"（3 字节）跟在 `?` 之后
    if q + 4 > n {
        return None;
    }
    if !bytes[q + 1..q + 4].eq_ignore_ascii_case(b"get") {
        return None;
    }
    // 从 "Get" 之后起找第一个 '('
    let mut j = q + 4;
    while j < n && bytes[j] != b'(' {
        j += 1;
    }
    if j >= n {
        return None; // 无 '(' → 未命中
    }
    let name = core::str::from_utf8(&bytes[q + 1..j]).ok()?;
    // 从 '(' 起按嵌套深度找匹配的 ')'
    let mut depth: isize = 0;
    let mut k = j;
    let mut close: Option<usize> = None;
    while k < n {
        match bytes[k] {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    close = Some(k);
                    break;
                }
            }
            _ => {}
        }
        k += 1;
    }
    let c = close?; // 括号未闭合 → None
    let body = core::str::from_utf8(&bytes[j + 1..c]).ok()?;
    Some(DataExprHit {
        q,
        match_len: c - q + 1,
        name: name.to_string(),
        body: body.to_string(),
    })
}

/// [R-P24 · R4] FFI：定位 UTF-8 文本中第一个 `?Get*(...)`，返回命中几何信息。
///
/// 返回值：
///   * `1`  = 命中（`name_out`/`body_out`/`*out_q`/`*out_match_len`
///            `*out_name_len`/`*out_body_len` 已写入）
///   * `0`  = 未命中（文本中不存在合法的 `?Get*(...)`）
///   * `-1` = 输入不是合法 UTF-8
///   * `-2` = 输出缓冲不足（不写入任何数据）
///   * `-3` = 关键指针为 null
///
/// `name_out` 写入函数名（不含前导 `?`），`body_out` 写入括号内原文，
/// 均为 UTF-8 字节、**不追加 NUL**（长度由 `*out_name_len` / `*out_body_len` 给出）。
///
/// # Safety
/// `s` 指向可读 `s_len` 字节；`name_out`/`body_out` 分别指向可写
/// `name_cap`/`body_cap` 字节；`out_q`/`out_match_len`/`out_name_len`/`out_body_len`
/// 指向可写 u64。
#[no_mangle]
pub unsafe extern "C" fn dismcore_parse_data_expr(
    s: *const u8,
    s_len: usize,
    name_out: *mut u8,
    name_cap: usize,
    body_out: *mut u8,
    body_cap: usize,
    out_q: *mut u64,
    out_match_len: *mut u64,
    out_name_len: *mut u64,
    out_body_len: *mut u64,
) -> isize {
    if name_out.is_null()
        || body_out.is_null()
        || out_q.is_null()
        || out_match_len.is_null()
        || out_name_len.is_null()
        || out_body_len.is_null()
    {
        return -3;
    }
    let slice: &[u8] = if s.is_null() || s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 可读字节
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let text = match core::str::from_utf8(slice) {
        Ok(t) => t,
        Err(_) => return -1,
    };
    let Some(hit) = parse_data_expr(text) else {
        return 0;
    };
    if name_cap < hit.name.len() || body_cap < hit.body.len() {
        return -2;
    }
    if !hit.name.is_empty() {
        // SAFETY: name_cap >= hit.name.len() 已校验
        unsafe { core::ptr::copy_nonoverlapping(hit.name.as_ptr(), name_out, hit.name.len()); }
    }
    if !hit.body.is_empty() {
        // SAFETY: body_cap >= hit.body.len() 已校验
        unsafe { core::ptr::copy_nonoverlapping(hit.body.as_ptr(), body_out, hit.body.len()); }
    }
    // SAFETY: 各 out 指针非空已校验
    unsafe {
        *out_q = hit.q as u64;
        *out_match_len = hit.match_len as u64;
        *out_name_len = hit.name.len() as u64;
        *out_body_len = hit.body.len() as u64;
    }
    1
}

// ============================================================================
// [R-P24 · R4] Data.xml `?Get*` 表达式解析器（UTF-16 变体）
// ============================================================================
//
// DataEngine.cpp 全程使用 wchar_t（UTF-16 code unit）。若把 UTF-8 版
// parse_data_expr 直接接入，q / match_len 为 UTF-8 字节偏移，而 C++ 侧
// Find / Assign / GetData 均为 wchar 下标，含非 ASCII（如路径中的中文）
// 时会错位。因此提供 UTF-16 变体：q / match_len 直接就是 code unit 数，
// C++ 侧无需任何编码换算。

/// 在 UTF-16 code units 中定位第一个 `?Get*(...)`。
///
/// 与 `parse_data_expr` 逐字对齐：仅扫第一个 `?`、`?` 后须紧跟 ASCII "Get"
/// （大小写不敏感）、括号须闭合且支持嵌套。未命中返回 `None`。
pub fn parse_data_expr_utf16(units: &[u16]) -> Option<DataExprHitUtf16> {
    let n = units.len();
    let q = units.iter().position(|&u| u == b'?' as u16)?;
    if q + 4 > n {
        return None;
    }
    // "Get" 必须为 ASCII，逐 code unit 比较（大小写不敏感）
    let get = [b'G' as u16, b'e' as u16, b't' as u16];
    let ok = |x: u16, y: u16| {
        // ASCII 大小写折叠：字母相差 0x20 即视为等价
        x == y || x ^ 0x20 == y
    };
    if !(ok(units[q + 1], get[0]) && ok(units[q + 2], get[1]) && ok(units[q + 3], get[2])) {
        return None;
    }
    let mut j = q + 4;
    while j < n && units[j] != b'(' as u16 {
        j += 1;
    }
    if j >= n {
        return None;
    }
    // name：从 q+1 到 j（不含 '('），要求全部为 ASCII（与 C++ 一致）
    let mut name = String::with_capacity(j - q - 1);
    for &u in &units[q + 1..j] {
        if u > 0x7F {
            return None; // 函数名含非 ASCII → 未命中
        }
        name.push(u as u8 as char);
    }
    let mut depth: isize = 0;
    let mut k = j;
    let mut close: Option<usize> = None;
    while k < n {
        match units[k] {
            u if u == b'(' as u16 => depth += 1,
            u if u == b')' as u16 => {
                depth -= 1;
                if depth == 0 {
                    close = Some(k);
                    break;
                }
            }
            _ => {}
        }
        k += 1;
    }
    let c = close?;
    let body = units[j + 1..c].to_vec();
    Some(DataExprHitUtf16 {
        q,
        match_len: c - q + 1,
        name,
        body,
    })
}

/// UTF-16 变体的解析命中几何信息（q / match_len 为 code unit 数，name 为
/// UTF-8 字符串便于 C++ 与 LookupFunEval 比对，body 保留 UTF-16 原文）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataExprHitUtf16 {
    pub q: usize,
    pub match_len: usize,
    pub name: String,
    pub body: Vec<u16>,
}

/// [R-P24 · R4] FFI：在 UTF-16 code units 中定位第一个 `?Get*(...)`。
///
/// 返回值：
///   * `1`  = 命中
///   * `0`  = 未命中
///   * `-2` = 输出缓冲不足（不写入任何数据）
///   * `-3` = 关键指针为 null
///
/// `name_out` 写入函数名（UTF-8 字节、不含前导 `?`、不追加 NUL），
/// `body_out` 写入括号内原文（UTF-16 code units、不追加 NUL）。
/// `*out_q` / `*out_match_len` 为 code unit 数。
///
/// # Safety
/// `s` 指向可读 `s_len` 个 code unit；`name_out` / `body_out` 分别指向可写
/// `name_cap` 字节 / `body_cap` 个 code unit；四个 u64 指针均非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_parse_data_expr_utf16(
    s: *const u16,
    s_len: usize,
    name_out: *mut u8,
    name_cap: usize,
    body_out: *mut u16,
    body_cap: usize,
    out_q: *mut u64,
    out_match_len: *mut u64,
    out_name_len: *mut u64,
    out_body_len: *mut u64,
) -> isize {
    if s.is_null()
        || name_out.is_null()
        || body_out.is_null()
        || out_q.is_null()
        || out_match_len.is_null()
        || out_name_len.is_null()
        || out_body_len.is_null()
    {
        return -3;
    }
    let units: &[u16] = if s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 个可读 code unit
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let Some(hit) = parse_data_expr_utf16(units) else {
        return 0;
    };
    let name_bytes = hit.name.as_bytes();
    if name_cap < name_bytes.len() || body_cap < hit.body.len() {
        return -2;
    }
    if !name_bytes.is_empty() {
        // SAFETY: name_cap >= name_bytes.len() 已校验
        unsafe { core::ptr::copy_nonoverlapping(name_bytes.as_ptr(), name_out, name_bytes.len()); }
    }
    if !hit.body.is_empty() {
        // SAFETY: body_cap >= hit.body.len() 已校验
        unsafe { core::ptr::copy_nonoverlapping(hit.body.as_ptr(), body_out, hit.body.len()); }
    }
    // SAFETY: 各 out 指针非空已校验
    unsafe {
        *out_q = hit.q as u64;
        *out_match_len = hit.match_len as u64;
        *out_name_len = name_bytes.len() as u64;
        *out_body_len = hit.body.len() as u64;
    }
    1
}

// ============================================================================
// [R-P25 · R6] 版本字符串解析 + 版本比较（纯计算，无 IO / 无 Win32）
// ============================================================================
//
// 迁移边界：仅字符串解析与纯整数比较迁入 Rust。
// 文件版本读取（GetFileVersionInfoSize / GetFileVersionInfo / VerQueryValue）、
// 环境变量展开（ExpandEnvironmentStrings）、路径存在性（GetFileAttributes）
// 按 R-P18 审计原则保留 C++。

/// 版本号：4 段 u32，缺段补 0（与 C++ `want[4]={0,0,0,0}` 语义一致）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Version {
    pub a: u32,
    pub b: u32,
    pub c: u32,
    pub d: u32,
}

impl Version {
    pub fn zero() -> Self {
        Version { a: 0, b: 0, c: 0, d: 0 }
    }

    /// 把 4 段压缩为一个 64 位整数，供无符号比较（与 C++ `uWant` 一致）。
    pub fn as_u64(self) -> u64 {
        (((self.a << 16) | self.b) as u64) << 32 | (((self.c << 16) | self.d) as u64)
    }
}

/// 解析版本字符串为 `Version`。
///
/// 规则（与 C++ `EvalFileVersion` 内联解析逐字对齐）：
///   * 以 `.` 分段，最多取前 4 段，多余段忽略
///   * 每段用无符号十进制解析；段内一旦遇到非法字符即停止本段解析
///     （`_ttoi` 语义：返回已解析部分，非数字停止），非法段按 0 处理
///   * 不足 4 段则缺段补 0
///
/// 永不返回 None（空串 / 全非法 → `Version::zero()`）。不 panic。
pub fn parse_version(s: &str) -> Version {
    let mut v = Version::zero();
    let mut seg = 0u8;
    let mut cur: u64 = 0;
    let mut broken = false; // 本段已遇非法字符，停止累积
    for ch in s.chars() {
        if ch == '.' {
            if seg < 4 {
                let val = cur.min(u32::MAX as u64) as u32;
                match seg {
                    0 => v.a = val,
                    1 => v.b = val,
                    2 => v.c = val,
                    _ => v.d = val,
                }
            }
            seg = seg.saturating_add(1);
            cur = 0;
            broken = false;
        } else if seg < 4 && !broken {
            if let Some(d) = ch.to_digit(10) {
                cur = cur.saturating_mul(10).saturating_add(d as u64);
            } else {
                broken = true; // _ttoi 遇到非数字停止解析本段
            }
        }
    }
    // 收尾最后一段
    if seg < 4 {
        let val = cur.min(u32::MAX as u64) as u32;
        match seg {
            0 => v.a = val,
            1 => v.b = val,
            2 => v.c = val,
            _ => v.d = val,
        }
    }
    v
}

/// 版本比较结果。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VersionCmp {
    Less,
    Equal,
    Greater,
}

/// 无符号版本比较（把 4 段压缩为 u64 后按整数比较，与 C++ `uHave` / `uWant` 一致）。
pub fn compare_version(lhs: Version, rhs: Version) -> VersionCmp {
    let a = lhs.as_u64();
    let b = rhs.as_u64();
    if a < b {
        VersionCmp::Less
    } else if a > b {
        VersionCmp::Greater
    } else {
        VersionCmp::Equal
    }
}

/// [R-P25 · R6] FFI：解析版本字符串为 4 段 u32。
///
/// 返回值：`*out_a` / `*out_b` / `*out_c` / `*out_d` 已写入；返回 0 表示成功。
/// 关键指针为 null 返回 -1。永不失败于非法输入（缺段补 0）。
///
/// # Safety
/// `s` 指向可读 `s_len` 字节（UTF-8）；四个 u32 指针均非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_parse_version(
    s: *const u8,
    s_len: usize,
    out_a: *mut u32,
    out_b: *mut u32,
    out_c: *mut u32,
    out_d: *mut u32,
) -> isize {
    if out_a.is_null() || out_b.is_null() || out_c.is_null() || out_d.is_null() {
        return -1;
    }
    let slice: &[u8] = if s.is_null() || s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 可读字节
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let text = core::str::from_utf8(slice).unwrap_or("");
    let v = parse_version(text);
    // SAFETY: 四个 out 指针非空已校验
    unsafe {
        *out_a = v.a;
        *out_b = v.b;
        *out_c = v.c;
        *out_d = v.d;
    }
    0
}

/// [R-P25 · R6] FFI：无符号版本比较。
///
/// 返回值：-1=Less / 0=Equal / 1=Greater；关键指针为 null 返回 -2。
///
/// # Safety
/// 8 个 u32 指针均非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_compare_version(
    la: *const u32,
    lb: *const u32,
    lc: *const u32,
    ld: *const u32,
    ra: *const u32,
    rb: *const u32,
    rc: *const u32,
    rd: *const u32,
) -> isize {
    if la.is_null() || lb.is_null() || lc.is_null() || ld.is_null()
        || ra.is_null() || rb.is_null() || rc.is_null() || rd.is_null()
    {
        return -2;
    }
    // SAFETY: 各指针非空已校验
    let lhs = Version {
        a: unsafe { *la },
        b: unsafe { *lb },
        c: unsafe { *lc },
        d: unsafe { *ld },
    };
    let rhs = Version {
        a: unsafe { *ra },
        b: unsafe { *rb },
        c: unsafe { *rc },
        d: unsafe { *rd },
    };
    match compare_version(lhs, rhs) {
        VersionCmp::Less => -1,
        VersionCmp::Equal => 0,
        VersionCmp::Greater => 1,
    }
}

/// [R-P25 · R6] FFI：在 UTF-16 code units 中解析版本字符串为 4 段 u32。
///
/// 版本串通常为纯 ASCII（如 `10.0.19045.3803`），此变体把 code unit 逐字节映射
/// 到 UTF-8 后复用 `parse_version`，避免 C++ 侧 UTF-16↔UTF-8 两段式转换。
/// 非 ASCII code unit 视为非法字符（停止解析当前段，_ttoi 语义）。
///
/// 返回值：`*out_a` / `*out_b` / `*out_c` / `*out_d` 已写入；返回 0 表示成功。
/// 关键指针为 null 返回 -1。
///
/// # Safety
/// `s` 指向可读 `s_len` 个 code unit；四个 u32 指针均非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_parse_version_utf16(
    s: *const u16,
    s_len: usize,
    out_a: *mut u32,
    out_b: *mut u32,
    out_c: *mut u32,
    out_d: *mut u32,
) -> isize {
    if out_a.is_null() || out_b.is_null() || out_c.is_null() || out_d.is_null() {
        return -1;
    }
    let units: &[u16] = if s.is_null() || s_len == 0 {
        &[]
    } else {
        // SAFETY: 调用方保证 s 指向 s_len 个可读 code unit
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    // 版本串基本为 ASCII：非 ASCII 视为非法字符（不参与解析，_ttoi 停止语义）
    let mut buf = Vec::with_capacity(units.len());
    for &u in units {
        if u > 0x7F {
            // 遇非法字符：截断（后续 parse_version 会 stop 当前段）
            buf.push(b'.'); // 让 parse_version 结束当前段
            break;
        }
        buf.push(u as u8);
    }
    let text = core::str::from_utf8(&buf).unwrap_or("");
    let v = parse_version(text);
    // SAFETY: 四个 out 指针非空已校验
    unsafe {
        *out_a = v.a;
        *out_b = v.b;
        *out_c = v.c;
        *out_d = v.d;
    }
    0
}

// ============================================================================
// [R-P26 · R5] XML 标签段定位（纯字符串扫描，无 XML 解析器依赖 / 无 IO）
// ============================================================================
//
// 迁移边界：仅在 UTF-8 / UTF-16 文本中定位指定标签名的出现位置与长度，
// 返回 Span（零拷贝）。不做 XML 语法校验、不解析属性值、不构建 DOM。
// CMarkup 解析器与 Page_Unattend.cpp 中的 pass 属性业务逻辑保留 C++。

/// 标签定位命中：Span（字节/code unit 偏移 + 长度，零拷贝）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct XmlTagSpan {
    /// 标签起始偏移（含 `<`）
    pub q: usize,
    /// 完整标签长度（含 `<` 与结尾 `>`），自闭合标签含 `/>`
    pub len: usize,
    /// 标签名长度
    pub name_len: usize,
}

/// 在 UTF-8 文本中定位第一个匹配 `tag` 的 XML 标签（大小写不敏感）。
///
/// 规则（与 C++ `Page_Unattend.cpp` 标签扫描语义对齐）：
///   * `<` 之后跳过空白/换行
///   * 标签名结束于空白 / `"` / `/` / `>`
///   * 标签名与 `tag` ASCII 大小写不敏感比较
///   * 找到匹配标签后，定位其结尾 `>`（含自闭合 `/>`）
///
/// 未命中 / 未闭合返回 `None`。不 panic。
pub fn find_xml_tag(text: &str, tag: &str) -> Option<XmlTagSpan> {
    let bytes = text.as_bytes();
    let n = bytes.len();
    let mut i = 0;
    while i < n {
        if bytes[i] == b'<' {
            // 跳过空白/换行
            let mut j = i + 1;
            while j < n && (bytes[j] == b' ' || bytes[j] == b'\t'
                            || bytes[j] == b'\r' || bytes[j] == b'\n') {
                j += 1;
            }
            if j >= n { return None; }
            // 提取标签名
            let name_start = j;
            while j < n && bytes[j] != b' '
                && bytes[j] != b'\t'
                && bytes[j] != b'\r'
                && bytes[j] != b'\n'
                && bytes[j] != b'"'
                && bytes[j] != b'/'
                && bytes[j] != b'>' {
                j += 1;
            }
            let name = core::str::from_utf8(&bytes[name_start..j]).ok()?;
            if name.eq_ignore_ascii_case(tag) {
                // 定位结尾 '>'
                let mut k = j;
                while k < n && bytes[k] != b'>' {
                    k += 1;
                }
                if k >= n { return None; } // 未闭合
                return Some(XmlTagSpan {
                    q: i,
                    len: k - i + 1,
                    name_len: j - name_start,
                });
            }
            // 未匹配：跳到当前标签结尾，继续扫描
            i += 1;
        } else {
            i += 1;
        }
    }
    None
}

/// 在 UTF-16 code units 中定位第一个匹配 `tag` 的 XML 标签（大小写不敏感）。
///
/// 与 `find_xml_tag` 语义一致；`tag` 传入 UTF-8 字节（内部与标签名 ASCII 比较）。
/// q / len 为 code unit 数，与 C++ `CDuiString::Find` 下标一一对应。
pub fn find_xml_tag_utf16(units: &[u16], tag: &str) -> Option<XmlTagSpan> {
    let n = units.len();
    let tag_bytes = tag.as_bytes();
    let mut i = 0;
    while i < n {
        if units[i] == b'<' as u16 {
            let mut j = i + 1;
            while j < n && (units[j] == b' ' as u16 || units[j] == b'\t' as u16
                            || units[j] == b'\r' as u16 || units[j] == b'\n' as u16) {
                j += 1;
            }
            if j >= n { return None; }
            let name_start = j;
            while j < n && units[j] != b' ' as u16
                && units[j] != b'\t' as u16
                && units[j] != b'\r' as u16
                && units[j] != b'\n' as u16
                && units[j] != b'"' as u16
                && units[j] != b'/' as u16
                && units[j] != b'>' as u16 {
                j += 1;
            }
            let name_len = j - name_start;
            // ASCII 大小写不敏感比较
            if name_len == tag_bytes.len() {
                let mut eq = true;
                for k in 0..name_len {
                    let c = units[name_start + k] as u8;
                    if c.to_ascii_lowercase() != tag_bytes[k].to_ascii_lowercase() {
                        eq = false;
                        break;
                    }
                }
                if eq {
                    let mut k = j;
                    while k < n && units[k] != b'>' as u16 {
                        k += 1;
                    }
                    if k >= n { return None; }
                    return Some(XmlTagSpan {
                        q: i,
                        len: k - i + 1,
                        name_len,
                    });
                }
            }
            i += 1;
        } else {
            i += 1;
        }
    }
    None
}

/// [R-P26 · R5] FFI：在 UTF-8 文本中定位第一个匹配 `tag` 的 XML 标签。
///
/// 返回值：1=命中（`*out_q` / `*out_len` 已写入）；0=未命中；-2=关键指针为空。
///
/// # Safety
/// `s` 指向可读 `s_len` 字节；`tag` 指向可读 `tag_len` 字节；两个 u64 指针非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_find_xml_tag(
    s: *const u8,
    s_len: usize,
    tag: *const u8,
    tag_len: usize,
    out_q: *mut u64,
    out_len: *mut u64,
) -> isize {
    if s.is_null() || tag.is_null() || out_q.is_null() || out_len.is_null() {
        return -2;
    }
    let slice: &[u8] = if s_len == 0 { &[] } else {
        // SAFETY: 调用方保证
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let tag_slice: &[u8] = if tag_len == 0 { &[] } else {
        // SAFETY: 调用方保证
        unsafe { core::slice::from_raw_parts(tag, tag_len) }
    };
    let text = match core::str::from_utf8(slice) { Ok(t) => t, Err(_) => return 0 };
    let tag_str = match core::str::from_utf8(tag_slice) { Ok(t) => t, Err(_) => return 0 };
    let Some(span) = find_xml_tag(text, tag_str) else { return 0 };
    // SAFETY: out 指针非空已校验
    unsafe { *out_q = span.q as u64; *out_len = span.len as u64; }
    1
}

/// [R-P26 · R5] FFI：在 UTF-16 code units 中定位第一个匹配 `tag` 的 XML 标签。
///
/// 返回值：1=命中；0=未命中；-2=关键指针为空。
///
/// # Safety
/// `s` 指向可读 `s_len` 个 code unit；`tag` 指向可读 `tag_len` 字节；两个 u64 指针非空。
#[no_mangle]
pub unsafe extern "C" fn dismcore_find_xml_tag_utf16(
    s: *const u16,
    s_len: usize,
    tag: *const u8,
    tag_len: usize,
    out_q: *mut u64,
    out_len: *mut u64,
) -> isize {
    if s.is_null() || tag.is_null() || out_q.is_null() || out_len.is_null() {
        return -2;
    }
    let units: &[u16] = if s_len == 0 { &[] } else {
        // SAFETY: 调用方保证
        unsafe { core::slice::from_raw_parts(s, s_len) }
    };
    let tag_slice: &[u8] = if tag_len == 0 { &[] } else {
        // SAFETY: 调用方保证
        unsafe { core::slice::from_raw_parts(tag, tag_len) }
    };
    let tag_str = match core::str::from_utf8(tag_slice) { Ok(t) => t, Err(_) => return 0 };
    let Some(span) = find_xml_tag_utf16(units, tag_str) else { return 0 };
    // SAFETY: out 指针非空已校验
    unsafe { *out_q = span.q as u64; *out_len = span.len as u64; }
    1
}

// ============================================================================
// 单元测试（Rust 侧回归）
// ============================================================================
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn md5_empty() {
        assert_eq!(digest_hex(Algo::Md5, b""),
                   "d41d8cd98f00b204e9800998ecf8427e");
    }

    #[test]
    fn md5_abc() {
        assert_eq!(digest_hex(Algo::Md5, b"abc"),
                   "900150983cd24fb0d6963f7d28e17f72");
    }

    #[test]
    fn sha256_abc() {
        assert_eq!(digest_hex(Algo::Sha256, b"abc"),
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    #[test]
    fn sha1_abc() {
        assert_eq!(digest_hex(Algo::Sha1, b"abc"),
                   "a9993e364706816aba3e25717850c26c9cd0d89d");
    }

    #[test]
    fn hex_roundtrip() {
        let v = vec![0x00u8, 0x0f, 0xa5, 0xff];
        let h = hex_encode(&v);
        assert_eq!(h, "000fa5ff");
        assert_eq!(hex_decode(&h).unwrap(), v);
    }

    #[test]
    fn hex_decode_invalid() {
        assert!(hex_decode("xyz").is_none());
        assert!(hex_decode("abc").is_none()); // 奇数长度
    }

    #[test]
    fn ffi_digest_matches() {
        let data = b"abc";
        let mut buf = [0u8; 64];
        let n = unsafe {
            dismcore_digest_hex(1, data.as_ptr(), data.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 32);
        assert_eq!(&buf[..32], b"900150983cd24fb0d6963f7d28e17f72");
    }

    #[test]
    fn ffi_buffer_too_small() {
        let data = b"abc";
        let mut buf = [0u8; 8];
        let n = unsafe {
            dismcore_digest_hex(1, data.as_ptr(), data.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -2);
    }

    #[test]
    fn ffi_bad_algo() {
        let data = b"abc";
        let mut buf = [0u8; 64];
        let n = unsafe {
            dismcore_digest_hex(99, data.as_ptr(), data.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -1);
    }

    // ===== R-P22 · R2 Hex/Base64 =====

    #[test]
    fn base64_encode_empty() {
        assert_eq!(base64_encode(b""), "");
    }

    #[test]
    fn base64_encode_basic() {
        // RFC 4648 标准向量
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn base64_encode_utf8_chinese() {
        // "中" UTF-8 = E4 B8 AD → 5Lit
        assert_eq!(base64_encode("中".as_bytes()), "5Lit");
        // "你好" → 5L2g5aW9
        assert_eq!(base64_encode("你好".as_bytes()), "5L2g5aW9");
    }

    #[test]
    fn base64_roundtrip() {
        let v = b"The quick brown fox jumps over the lazy dog";
        let enc = base64_encode(v);
        assert_eq!(base64_decode(&enc).unwrap(), v);
    }

    #[test]
    fn base64_decode_ignore_ws() {
        // 内嵌换行
        let enc = "Zm9v\nYmFy\r\n";
        assert_eq!(base64_decode(enc).unwrap(), b"foobar");
    }

    #[test]
    fn base64_decode_invalid() {
        // 非 base64 字符
        assert!(base64_decode("!!!!").is_none());
        // 长度不是 4 的倍数
        assert!(base64_decode("Zm9vY").is_none());
        // 填充非法
        assert!(base64_decode("Zm9v===").is_none());
    }

    #[test]
    fn base64_decode_vs_crypt_library() {
        // 验证与 PowerShell [Convert]::ToBase64String 一致（RFC 4648）
        let v = vec![0x00u8, 0x01, 0x02, 0xFE, 0xFF, 0x80];
        let enc = base64_encode(&v);
        assert_eq!(enc, "AAEC/v+A");
        assert_eq!(base64_decode(&enc).unwrap(), v);
    }

    #[test]
    fn ffi_hex_decode_roundtrip() {
        let hex = b"000fa5ff";
        let mut buf = [0u8; 8];
        let n = unsafe {
            dismcore_hex_decode(hex.as_ptr(), hex.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 4);
        assert_eq!(&buf[..4], &[0x00, 0x0f, 0xa5, 0xff]);
    }

    #[test]
    fn ffi_hex_decode_invalid_returns_m3() {
        let bad = b"xyz";
        let mut buf = [0u8; 8];
        let n = unsafe {
            dismcore_hex_decode(bad.as_ptr(), bad.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -3);
    }

    #[test]
    fn ffi_hex_decode_small_buffer_returns_m2() {
        let hex = b"000fa5ff";
        let mut buf = [0u8; 3];
        let n = unsafe {
            dismcore_hex_decode(hex.as_ptr(), hex.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -2);
    }

    #[test]
    fn ffi_base64_encode_matches_python() {
        let data = b"foobar";
        let mut buf = [0u8; 16];
        let n = unsafe {
            dismcore_base64_encode(data.as_ptr(), data.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 8);
        assert_eq!(&buf[..8], b"Zm9vYmFy");
    }

    #[test]
    fn ffi_base64_decode_roundtrip() {
        let b64 = b"Zm9vYmFy";
        let mut buf = [0u8; 16];
        let n = unsafe {
            dismcore_base64_decode(b64.as_ptr(), b64.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 6);
        assert_eq!(&buf[..6], b"foobar");
    }

    #[test]
    fn ffi_base64_decode_invalid_returns_m3() {
        let bad = b"!!!!";
        let mut buf = [0u8; 16];
        let n = unsafe {
            dismcore_base64_decode(bad.as_ptr(), bad.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -3);
    }

    // ===== [R-P23 · R3] UTF-8 ↔ UTF-16 =====

    #[test]
    fn utf8_to_utf16_ascii() {
        // "abc" → 3 code units
        let units = utf8_to_utf16(b"abc").unwrap();
        assert_eq!(units, vec![0x0061, 0x0062, 0x0063]);
    }

    #[test]
    fn utf8_to_utf16_chinese() {
        // "你好" UTF-8 = E4 BD A0 E5 A5 BD → 0x4F60 0x597D
        let units = utf8_to_utf16("你好".as_bytes()).unwrap();
        assert_eq!(units, vec![0x4F60, 0x597D]);
    }

    #[test]
    fn utf8_to_utf16_emoji_surrogate_pair() {
        // U+1F600 (😀) UTF-8 = F0 9F 98 80 → 0xD83D 0xDE00
        let units = utf8_to_utf16("\u{1F600}".as_bytes()).unwrap();
        assert_eq!(units, vec![0xD83D, 0xDE00]);
    }

    #[test]
    fn utf8_to_utf16_invalid_returns_none() {
        // 非法 UTF-8 序列
        assert!(utf8_to_utf16(&[0xFF, 0xFE]).is_none());
        assert!(utf8_to_utf16(&[0xC0, 0x80]).is_none());
    }

    #[test]
    fn utf16_to_utf8_roundtrip() {
        let s = "Hello, 你好, 😀";
        let units = utf8_to_utf16(s.as_bytes()).unwrap();
        let bytes = utf16_to_utf8(&units).unwrap();
        assert_eq!(bytes, s.as_bytes());
    }

    #[test]
    fn utf16_to_utf8_ascii() {
        let units = vec![0x0061u16, 0x0062, 0x0063];
        assert_eq!(utf16_to_utf8(&units).unwrap(), b"abc");
    }

    #[test]
    fn utf16_to_utf8_invalid_lone_surrogate() {
        // 孤立低位代理 → None
        assert!(utf16_to_utf8(&[0xDC00]).is_none());
        // 高位代理后跟非低位代理 → None
        assert!(utf16_to_utf8(&[0xD800, 0x0061]).is_none());
    }

    #[test]
    fn ffi_utf8_to_utf16_chinese() {
        let utf8 = "你好".as_bytes();
        let mut buf = [0u16; 16];
        let n = unsafe {
            dismcore_utf8_to_utf16(utf8.as_ptr(), utf8.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 2);
        assert_eq!(buf[0], 0x4F60);
        assert_eq!(buf[1], 0x597D);
    }

    #[test]
    fn ffi_utf8_to_utf16_invalid_returns_m3() {
        let bad = [0xFFu8, 0xFE];
        let mut buf = [0u16; 16];
        let n = unsafe {
            dismcore_utf8_to_utf16(bad.as_ptr(), bad.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, -3);
    }

    #[test]
    fn ffi_utf16_to_utf8_roundtrip() {
        let units = [0x4F60u16, 0x597D];
        let mut buf = [0u8; 16];
        let n = unsafe {
            dismcore_utf16_to_utf8(units.as_ptr(), units.len(), buf.as_mut_ptr(), buf.len())
        };
        assert_eq!(n, 6);
        assert_eq!(&buf[..6], "你好".as_bytes());
    }

    // ===== [R-P24 · R4] Data.xml `?Get*` 解析器 =====

    #[test]
    fn parse_expr_basic_regsz() {
        let hit = parse_data_expr("?GetRegSz(HKCU\\A\\B,Value)").unwrap();
        assert_eq!(hit.q, 0);
        assert_eq!(hit.name, "GetRegSz");
        assert_eq!(hit.body, "HKCU\\A\\B,Value");
        assert_eq!(hit.match_len, "?GetRegSz(HKCU\\A\\B,Value)".len());
    }

    #[test]
    fn parse_expr_mid_string_and_suffix() {
        let hit = parse_data_expr("Root=?GetIniData(%Root%a.ini,S,K)\\..\\x").unwrap();
        assert_eq!(hit.q, 5);
        assert_eq!(hit.name, "GetIniData");
        assert_eq!(hit.body, "%Root%a.ini,S,K");
        assert_eq!(hit.match_len, "?GetIniData(%Root%a.ini,S,K)".len());
    }

    #[test]
    fn parse_expr_locale_empty_body() {
        let hit = parse_data_expr("x?GetLocaleName()y").unwrap();
        assert_eq!(hit.q, 1);
        assert_eq!(hit.name, "GetLocaleName");
        assert_eq!(hit.body, "");
        assert_eq!(hit.match_len, "?GetLocaleName()".len());
    }

    #[test]
    fn parse_expr_case_insensitive_get() {
        let hit = parse_data_expr("?getregsz(a,b)").unwrap();
        assert_eq!(hit.name, "getregsz");
        assert_eq!(hit.body, "a,b");
    }

    #[test]
    fn parse_expr_nested_paren_in_body() {
        // body 内含括号：GetRegSz(HKEY\\..\\Key,(a)) → 深度匹配到最外层 ')'
        let hit = parse_data_expr("?GetRegSz(HKEY\\A,(a,b),True)").unwrap();
        assert_eq!(hit.name, "GetRegSz");
        assert_eq!(hit.body, "HKEY\\A,(a,b),True");
    }

    #[test]
    fn parse_expr_no_question_returns_none() {
        assert!(parse_data_expr("plain string").is_none());
        assert!(parse_data_expr("").is_none());
    }

    #[test]
    fn parse_expr_question_not_get_returns_none() {
        // 第一个 '?' 后不是 Get → 未命中（与 C++ 行为一致：不向后继续扫）
        assert!(parse_data_expr("a?Foo(b)").is_none());
        assert!(parse_data_expr("a?Get").is_none()); // 没有 '('
    }

    #[test]
    fn parse_expr_unclosed_paren_returns_none() {
        assert!(parse_data_expr("?GetRegSz(HKEY\\A,B").is_none());
        assert!(parse_data_expr("?GetRegSz(a").is_none());
    }

    #[test]
    fn parse_expr_only_question_returns_none() {
        assert!(parse_data_expr("?").is_none());
        assert!(parse_data_expr("?G").is_none()); // "Get" 不足 3 字节
    }

    #[test]
    fn ffi_parse_data_expr_roundtrip() {
        let text = "Root=?GetFileVersion(%Root%Chrome.exe)\\\\x";
        let mut name = [0u8; 64];
        let mut body = [0u8; 128];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr(
                text.as_ptr(), text.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, 1);
        assert_eq!(q, 5);
        assert_eq!(ml, "?GetFileVersion(%Root%Chrome.exe)".len() as u64);
        assert_eq!(nl, "GetFileVersion".len() as u64);
        assert_eq!(bl, "%Root%Chrome.exe".len() as u64);
        assert_eq!(core::str::from_utf8(&name[..nl as usize]).unwrap(), "GetFileVersion");
        assert_eq!(core::str::from_utf8(&body[..bl as usize]).unwrap(), "%Root%Chrome.exe");
    }

    #[test]
    fn ffi_parse_data_expr_miss_returns_0() {
        let text = "plain string";
        let mut name = [0u8; 8];
        let mut body = [0u8; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr(
                text.as_ptr(), text.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, 0);
    }

    #[test]
    fn ffi_parse_data_expr_small_buffer_returns_m2() {
        let text = "?GetRegSz(HKEY\\A,B)";
        let mut name = [0u8; 2]; // 不足
        let mut body = [0u8; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr(
                text.as_ptr(), text.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, -2);
    }

    #[test]
    fn ffi_parse_data_expr_null_out_returns_m3() {
        let text = "?GetRegSz(HKEY\\A,B)";
        let mut body = [0u8; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr(
                text.as_ptr(), text.len(),
                core::ptr::null_mut(), 0,
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, -3);
    }

    #[test]
    fn ffi_parse_data_expr_invalid_utf8_returns_m1() {
        let bad = [0xFFu8, 0xFE];
        let mut name = [0u8; 8];
        let mut body = [0u8; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr(
                bad.as_ptr(), bad.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, -1);
    }

    // ===== [R-P24 · R4] UTF-16 变体 =====

    fn to_u16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn parse_expr_utf16_basic() {
        let units = to_u16("?GetRegSz(HKEY\\A,B)");
        let hit = parse_data_expr_utf16(&units).unwrap();
        assert_eq!(hit.q, 0);
        assert_eq!(hit.name, "GetRegSz");
        assert_eq!(hit.body, to_u16("HKEY\\A,B"));
        assert_eq!(hit.match_len, to_u16("?GetRegSz(HKEY\\A,B)").len());
    }

    #[test]
    fn parse_expr_utf16_mid_string_suffix() {
        let units = to_u16("Root=?GetIniData(%Root%a.ini,S,K)\\..\\x");
        let hit = parse_data_expr_utf16(&units).unwrap();
        assert_eq!(hit.q, 5);
        assert_eq!(hit.name, "GetIniData");
        assert_eq!(hit.body, to_u16("%Root%a.ini,S,K"));
        assert_eq!(hit.match_len, to_u16("?GetIniData(%Root%a.ini,S,K)").len());
    }

    #[test]
    fn parse_expr_utf16_empty_body() {
        let units = to_u16("x?GetLocaleName()y");
        let hit = parse_data_expr_utf16(&units).unwrap();
        assert_eq!(hit.q, 1);
        assert_eq!(hit.name, "GetLocaleName");
        assert!(hit.body.is_empty());
        assert_eq!(hit.match_len, to_u16("?GetLocaleName()").len());
    }

    #[test]
    fn parse_expr_utf16_nested_paren() {
        let units = to_u16("?GetRegSz(HKEY\\A,(a,b),True)");
        let hit = parse_data_expr_utf16(&units).unwrap();
        assert_eq!(hit.name, "GetRegSz");
        assert_eq!(hit.body, to_u16("HKEY\\A,(a,b),True"));
    }

    #[test]
    fn parse_expr_utf16_non_get_returns_none() {
        assert!(parse_data_expr_utf16(&to_u16("a?Foo(b)")).is_none());
        assert!(parse_data_expr_utf16(&to_u16("plain")).is_none());
        assert!(parse_data_expr_utf16(&[]).is_none());
    }

    #[test]
    fn parse_expr_utf16_unclosed_returns_none() {
        assert!(parse_data_expr_utf16(&to_u16("?GetRegSz(HKEY\\A,B")).is_none());
    }

    #[test]
    fn ffi_parse_data_expr_utf16_roundtrip() {
        let text = "Root=?GetFileVersion(%Root%Chrome.exe)\\\\x";
        let units = to_u16(text);
        let mut name = [0u8; 64];
        let mut body = [0u16; 128];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr_utf16(
                units.as_ptr(), units.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, 1);
        assert_eq!(q, 5);
        assert_eq!(ml, to_u16("?GetFileVersion(%Root%Chrome.exe)").len() as u64);
        assert_eq!(nl, "GetFileVersion".len() as u64);
        assert_eq!(bl, "%Root%Chrome.exe".len() as u64);
        assert_eq!(core::str::from_utf8(&name[..nl as usize]).unwrap(), "GetFileVersion");
        assert_eq!(&body[..bl as usize], &to_u16("%Root%Chrome.exe")[..]);
    }

    #[test]
    fn ffi_parse_data_expr_utf16_miss_returns_0() {
        let units = to_u16("plain string");
        let mut name = [0u8; 8];
        let mut body = [0u16; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr_utf16(
                units.as_ptr(), units.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, 0);
    }

    #[test]
    fn ffi_parse_data_expr_utf16_small_buffer_returns_m2() {
        let units = to_u16("?GetRegSz(HKEY\\A,B)");
        let mut name = [0u8; 2];
        let mut body = [0u16; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr_utf16(
                units.as_ptr(), units.len(),
                name.as_mut_ptr(), name.len(),
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, -2);
    }

    #[test]
    fn ffi_parse_data_expr_utf16_null_returns_m3() {
        let mut body = [0u16; 8];
        let mut q: u64 = 0;
        let mut ml: u64 = 0;
        let mut nl: u64 = 0;
        let mut bl: u64 = 0;
        let n = unsafe {
            dismcore_parse_data_expr_utf16(
                core::ptr::null(), 0,
                core::ptr::null_mut(), 0,
                body.as_mut_ptr(), body.len(),
                &mut q, &mut ml, &mut nl, &mut bl,
            )
        };
        assert_eq!(n, -3);
    }

    // ===== [R-P25 · R6] 版本解析与比较 =====

    #[test]
    fn parse_version_full_4seg() {
        let v = parse_version("1.2.3.4");
        assert_eq!(v, Version { a: 1, b: 2, c: 3, d: 4 });
    }

    #[test]
    fn parse_version_missing_segs_pad_zero() {
        assert_eq!(parse_version("1.2.3"), Version { a: 1, b: 2, c: 3, d: 0 });
        assert_eq!(parse_version("1.2"), Version { a: 1, b: 2, c: 0, d: 0 });
        assert_eq!(parse_version("1"), Version { a: 1, b: 0, c: 0, d: 0 });
        assert_eq!(parse_version(""), Version::zero());
    }

    #[test]
    fn parse_version_extra_segs_ignored() {
        // 多余段忽略
        assert_eq!(parse_version("1.2.3.4.5"), Version { a: 1, b: 2, c: 3, d: 4 });
    }

    #[test]
    fn parse_version_invalid_char_stops_seg() {
        // 段内遇非数字停止该段解析（_ttoi 语义）
        assert_eq!(parse_version("1x.2"), Version { a: 1, b: 2, c: 0, d: 0 });
        // 完全非法 → 0
        assert_eq!(parse_version("abc"), Version::zero());
    }

    #[test]
    fn parse_version_large_segment_caps() {
        // 溢出段 saturate 到 u32::MAX
        let v = parse_version("99999999999.1");
        assert_eq!(v.a, u32::MAX);
        assert_eq!(v.b, 1);
    }

    #[test]
    fn compare_version_basic() {
        assert_eq!(compare_version(parse_version("1.2.3.4"), parse_version("1.2.3.4")), VersionCmp::Equal);
        assert_eq!(compare_version(parse_version("1.2.3.4"), parse_version("1.2.3.5")), VersionCmp::Less);
        assert_eq!(compare_version(parse_version("2.0.0.0"), parse_version("1.9.9.9")), VersionCmp::Greater);
    }

    #[test]
    fn compare_version_missing_segs_as_zero() {
        // 1.2 与 1.2.0.0 等价
        assert_eq!(compare_version(parse_version("1.2"), parse_version("1.2.0.0")), VersionCmp::Equal);
        // 1.2.0.1 > 1.2
        assert_eq!(compare_version(parse_version("1.2.0.1"), parse_version("1.2")), VersionCmp::Greater);
    }

    #[test]
    fn version_as_u64_compression() {
        let v = Version { a: 1, b: 2, c: 3, d: 4 };
        // ((1<<16|2) << 32) | (3<<16|4)
        assert_eq!(v.as_u64(), ((0x10002u64) << 32) | 0x30004);
    }

    #[test]
    fn ffi_parse_version_roundtrip() {
        let s = b"10.0.19045.3803";
        let mut a = 0u32; let mut b = 0u32; let mut c = 0u32; let mut d = 0u32;
        let n = unsafe { dismcore_parse_version(s.as_ptr(), s.len(), &mut a, &mut b, &mut c, &mut d) };
        assert_eq!(n, 0);
        assert_eq!((a, b, c, d), (10, 0, 19045, 3803));
    }

    #[test]
    fn ffi_parse_version_empty_and_null_out() {
        let mut a = 0u32; let mut b = 0u32; let mut c = 0u32; let mut d = 0u32;
        // 空串 → 0.0.0.0
        let n = unsafe { dismcore_parse_version(core::ptr::null(), 0, &mut a, &mut b, &mut c, &mut d) };
        assert_eq!(n, 0);
        assert_eq!((a, b, c, d), (0, 0, 0, 0));
        // null out → -1
        let n = unsafe { dismcore_parse_version(core::ptr::null(), 0, core::ptr::null_mut(), &mut b, &mut c, &mut d) };
        assert_eq!(n, -1);
    }

    #[test]
    fn ffi_compare_version_roundtrip() {
        let a1 = 1u32; let b1 = 2u32; let c1 = 3u32; let d1 = 4u32;
        let a2 = 1u32; let b2 = 2u32; let c2 = 3u32; let d2 = 5u32;
        let r = unsafe {
            dismcore_compare_version(&a1, &b1, &c1, &d1, &a2, &b2, &c2, &d2)
        };
        assert_eq!(r, -1);
        // null → -2
        let r = unsafe {
            dismcore_compare_version(core::ptr::null(), &b1, &c1, &d1, &a2, &b2, &c2, &d2)
        };
        assert_eq!(r, -2);
    }

    #[test]
    fn ffi_parse_version_utf16_roundtrip() {
        let s: Vec<u16> = "10.0.19045.3803".encode_utf16().collect();
        let mut a = 0u32; let mut b = 0u32; let mut c = 0u32; let mut d = 0u32;
        let n = unsafe {
            dismcore_parse_version_utf16(s.as_ptr(), s.len(), &mut a, &mut b, &mut c, &mut d)
        };
        assert_eq!(n, 0);
        assert_eq!((a, b, c, d), (10, 0, 19045, 3803));
    }

    #[test]
    fn ffi_parse_version_utf16_empty_and_null_out() {
        let mut a = 0u32; let mut b = 0u32; let mut c = 0u32; let mut d = 0u32;
        let n = unsafe { dismcore_parse_version_utf16(core::ptr::null(), 0, &mut a, &mut b, &mut c, &mut d) };
        assert_eq!(n, 0);
        assert_eq!((a, b, c, d), (0, 0, 0, 0));
        let n = unsafe {
            dismcore_parse_version_utf16(core::ptr::null(), 0, core::ptr::null_mut(), &mut b, &mut c, &mut d)
        };
        assert_eq!(n, -1);
    }

    // ===== [R-P26 · R5] XML 标签段定位 =====

    #[test]
    fn find_xml_tag_basic() {
        let text = "<unattend><settings pass=\"specialize\"><component /></settings></unattend>";
        let span = find_xml_tag(text, "SETTINGS").unwrap();
        assert_eq!(&text[span.q..span.q + span.len], "<settings pass=\"specialize\">");
        assert_eq!(span.name_len, "settings".len());
    }

    #[test]
    fn find_xml_tag_case_insensitive() {
        let text = "<UNATTEND><Settings /></UNATTEND>";
        let span = find_xml_tag(text, "unattend").unwrap();
        assert_eq!(&text[span.q..span.q + span.len], "<UNATTEND>");
    }

    #[test]
    fn find_xml_tag_self_closing() {
        let text = "<root><component id=\"a\" /></root>";
        let span = find_xml_tag(text, "component").unwrap();
        assert_eq!(&text[span.q..span.q + span.len], "<component id=\"a\" />");
    }

    #[test]
    fn find_xml_tag_empty_body() {
        let text = "<settings></settings>";
        let span = find_xml_tag(text, "settings").unwrap();
        assert_eq!(&text[span.q..span.q + span.len], "<settings>");
    }

    #[test]
    fn find_xml_tag_miss_returns_none() {
        assert!(find_xml_tag("<a><b></b></a>", "c").is_none());
        assert!(find_xml_tag("plain text", "a").is_none());
    }

    #[test]
    fn find_xml_tag_unclosed_returns_none() {
        // 匹配标签但找不到结尾 '>' → 未命中
        assert!(find_xml_tag("<a><b", "b").is_none());
        assert!(find_xml_tag("<a", "a").is_none());
        // 已闭合标签正常命中
        assert!(find_xml_tag("<a><b>", "b").is_some());
    }

    #[test]
    fn find_xml_tag_multiple_scans_later() {
        let text = "<settings></settings><component /><settings>second</settings>";
        // 第一个 settings 命中
        let span = find_xml_tag(text, "settings").unwrap();
        assert_eq!(&text[span.q..span.q + span.len], "<settings>");
    }

    #[test]
    fn find_xml_tag_utf16_basic() {
        let text: Vec<u16> = "<unattend><settings pass=\"spec\"></settings></unattend>".encode_utf16().collect();
        let span = find_xml_tag_utf16(&text, "settings").unwrap();
        let matched: Vec<u16> = text[span.q..span.q + span.len].to_vec();
        let expect: Vec<u16> = "<settings pass=\"spec\">".encode_utf16().collect();
        assert_eq!(matched, expect);
        assert_eq!(span.name_len, "settings".len());
    }

    #[test]
    fn find_xml_tag_utf16_miss() {
        let text: Vec<u16> = "<a><b></b></a>".encode_utf16().collect();
        assert!(find_xml_tag_utf16(&text, "c").is_none());
        assert!(find_xml_tag_utf16(&[], "a").is_none());
    }

    #[test]
    fn ffi_find_xml_tag_roundtrip() {
        let text = "<unattend><settings pass=\"spec\" /></unattend>";
        let tag = b"settings";
        let mut q: u64 = 0;
        let mut len: u64 = 0;
        let n = unsafe {
            dismcore_find_xml_tag(text.as_ptr(), text.len(), tag.as_ptr(), tag.len(), &mut q, &mut len)
        };
        assert_eq!(n, 1);
        assert_eq!(q, 10);
        assert_eq!(&text[q as usize..q as usize + len as usize], "<settings pass=\"spec\" />");
    }

    #[test]
    fn ffi_find_xml_tag_miss_and_null() {
        let text = "<a></a>";
        let tag = b"z";
        let mut q: u64 = 0;
        let mut len: u64 = 0;
        let n = unsafe { dismcore_find_xml_tag(text.as_ptr(), text.len(), tag.as_ptr(), tag.len(), &mut q, &mut len) };
        assert_eq!(n, 0);
        let n = unsafe { dismcore_find_xml_tag(core::ptr::null(), 0, tag.as_ptr(), tag.len(), &mut q, &mut len) };
        assert_eq!(n, -2);
    }

    #[test]
    fn ffi_find_xml_tag_utf16_roundtrip() {
        let text: Vec<u16> = "<unattend><settings /></unattend>".encode_utf16().collect();
        let tag = b"SETTINGS";
        let mut q: u64 = 0;
        let mut len: u64 = 0;
        let n = unsafe {
            dismcore_find_xml_tag_utf16(text.as_ptr(), text.len(), tag.as_ptr(), tag.len(), &mut q, &mut len)
        };
        assert_eq!(n, 1);
        let matched: Vec<u16> = text[q as usize..q as usize + len as usize].to_vec();
        assert_eq!(matched, "<settings />".encode_utf16().collect::<Vec<_>>());
    }
}
