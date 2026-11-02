//! Windows 11 image preprocessing: build gate, bundle validation, atomic payload install.
//!
//! C++ only loads the offline SOFTWARE hive and passes the real CurrentBuildNumber.
//! Every preprocessing decision and file write remains in Rust.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

pub const MIN_BUILD: u32 = 26100;

#[derive(Debug)]
pub enum PrepError {
    InvalidRoot,
    NotWindows11,
    InvalidAccount,
    Io(String),
    InvalidBundle,
}

impl PrepError {
    pub fn message(&self) -> String {
        match self {
            PrepError::InvalidRoot => "所选目录不是已挂载的 Windows 映像根目录。".into(),
            PrepError::NotWindows11 => format!("仅支持 Windows 11 build {MIN_BUILD}（24H2）及以上版本。"),
            PrepError::InvalidAccount => "本地管理员账户名无效。".into(),
            PrepError::Io(m) => format!("文件操作失败：{m}"),
            PrepError::InvalidBundle => "生成的预处理包校验失败。".into(),
        }
    }
}

pub fn atomic_write(target: &Path, bytes: &[u8]) -> Result<(), PrepError> {
    let parent = target.parent().ok_or_else(|| PrepError::Io("invalid target".into()))?;
    fs::create_dir_all(parent).map_err(|e| PrepError::Io(e.to_string()))?;
    let mut tmp_name = target.file_name().unwrap_or_default().to_os_string();
    tmp_name.push(".tmp");
    let tmp = parent.join(tmp_name);
    {
        let mut f = fs::File::create(&tmp).map_err(|e| PrepError::Io(e.to_string()))?;
        f.write_all(bytes).map_err(|e| PrepError::Io(e.to_string()))?;
        f.sync_all().map_err(|e| PrepError::Io(e.to_string()))?;
    }
    // Windows rename does not replace an existing file; remove only after the temp file is durable.
    if target.exists() {
        if let Err(e) = fs::remove_file(target) {
            let _ = fs::remove_file(&tmp);
            return Err(PrepError::Io(e.to_string()));
        }
    }
    if let Err(e) = fs::rename(&tmp, target) {
        let _ = fs::remove_file(&tmp);
        return Err(PrepError::Io(e.to_string()));
    }
    Ok(())
}

pub struct Installed {
    pub xml_path: PathBuf,
    pub cmd_path: PathBuf,
}

impl Installed {
    pub fn paths(&self) -> (&Path, &Path) { (self.xml_path.as_path(), self.cmd_path.as_path()) }
}

pub fn install_bundle(root: &Path, account: &str, build: u32) -> Result<Installed, PrepError> {
    if build < MIN_BUILD { return Err(PrepError::NotWindows11); }
    if !root.join("Windows/System32/ntoskrnl.exe").is_file() {
        return Err(PrepError::InvalidRoot);
    }
    let bundle = super::win11_preprocess::build_bundle(account).map_err(|_| PrepError::InvalidAccount)?;
    if !super::win11_preprocess::validate_bundle(&bundle.unattend_xml, &bundle.desktop_script) {
        return Err(PrepError::InvalidBundle);
    }
    let xml_path = root.join("Windows/Panther/unattend.xml");
    let cmd_path = root.join("Windows/Setup/Scripts/Win11DesktopIcons.cmd");
    atomic_write(&xml_path, bundle.unattend_xml.as_bytes())?;
    atomic_write(&cmd_path, bundle.desktop_script.as_bytes())?;

    let read_back_xml = fs::read(&xml_path).map_err(|e| PrepError::Io(e.to_string()))?;
    let read_back_cmd = fs::read(&cmd_path).map_err(|e| PrepError::Io(e.to_string()))?;
    if !super::win11_preprocess::validate_bundle(
        &String::from_utf8_lossy(&read_back_xml),
        &String::from_utf8_lossy(&read_back_cmd),
    ) {
        return Err(PrepError::InvalidBundle);
    }
    Ok(Installed { xml_path, cmd_path })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn rejects_old_build_before_touching_disk() {
        let d = std::env::temp_dir().join("dismcore_old_build");
        let _ = fs::create_dir_all(&d);
        assert!(matches!(install_bundle(&d, "ImageAdmin", 22631), Err(PrepError::NotWindows11)));
    }
    #[test] fn rejects_missing_image_root() {
        let d = std::env::temp_dir().join("dismcore_missing_image");
        assert!(matches!(install_bundle(&d, "ImageAdmin", 26100), Err(PrepError::InvalidRoot)));
    }
}
