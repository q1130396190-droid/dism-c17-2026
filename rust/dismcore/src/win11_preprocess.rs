//! Windows 11 24H2+ offline-image preprocessing payload generator.
//! Pure Rust: no Win32 handles, registry APIs, or filesystem access cross the FFI boundary.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreprocessBundle {
    pub unattend_xml: String,
    pub desktop_script: String,
}

fn xml_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            _ => out.push(c),
        }
    }
    out
}

fn valid_account_name(name: &str) -> bool {
    let n = name.trim();
    !n.is_empty() && n.chars().count() <= 20
        && !n.chars().any(|c| c.is_control() || "\\/[]:;|=,+*?<>\"@".contains(c))
        && n != "." && n != ".."
}

pub fn build_bundle(account_name: &str) -> Result<PreprocessBundle, &'static str> {
    if !valid_account_name(account_name) { return Err("invalid account name"); }
    let account = xml_escape(account_name.trim());
    let script = r#"@echo off
setlocal
rem Apply classic desktop icons to Default User and every existing profile.
set "CLSIDKEY=Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel"
for %%G in ({20D04FE0-3AEA-1069-A2D8-08002B30309D} {59031a47-3f72-44a7-89c5-5595fe6b30ee} {F02C1A0D-BE21-4350-88B0-7367FC96EF3C} {5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}) do reg add "HKU\DefaultUser\%CLSIDKEY%" /v "%%G" /t REG_DWORD /d 0 /f >nul
for /f "tokens=*" %%S in ('reg query HKLM\SOFTWARE\Microsoft\Windows^ NT\CurrentVersion\ProfileList 2^>nul ^| findstr /r "S-1-5-21-"') do call :ApplyOne "%%S"
reg add "HKCU\%CLSIDKEY%" /v "{20D04FE0-3AEA-1069-A2D8-08002B30309D}" /t REG_DWORD /d 0 /f >nul
reg add "HKCU\%CLSIDKEY%" /v "{59031a47-3f72-44a7-89c5-5595fe6b30ee}" /t REG_DWORD /d 0 /f >nul
reg add "HKCU\%CLSIDKEY%" /v "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}" /t REG_DWORD /d 0 /f >nul
reg add "HKCU\%CLSIDKEY%" /v "{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}" /t REG_DWORD /d 0 /f >nul
exit /b 0
:ApplyOne
for /f "tokens=2,*" %%A in ('reg query %~1 /v ProfileImagePath 2^>nul ^| findstr /i ProfileImagePath') do set "P=%%B"
if not defined P exit /b 0
if not exist "%P%\NTUSER.DAT" exit /b 0
reg load HKU\DismPrepUser "%P%\NTUSER.DAT" >nul 2>&1 || exit /b 0
for %%G in ({20D04FE0-3AEA-1069-A2D8-08002B30309D} {59031a47-3f72-44a7-89c5-5595fe6b30ee} {F02C1A0D-BE21-4350-88B0-7367FC96EF3C} {5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}) do reg add "HKU\DismPrepUser\%CLSIDKEY%" /v "%%G" /t REG_DWORD /d 0 /f >nul
reg unload HKU\DismPrepUser >nul 2>&1
exit /b 0
"#.to_string();
    let xml = format!(r#"<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <OOBE>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts><LocalAccounts><LocalAccount wcm:action="add">
        <Name>{0}</Name><DisplayName>{0}</DisplayName><Description>Local administrator created by image preprocessing</Description><Group>Administrators</Group>
      </LocalAccount></LocalAccounts></UserAccounts>
      <FirstLogonCommands><SynchronousCommand wcm:action="add"><Order>1</Order><Description>Apply desktop icons for default and administrator profiles</Description><CommandLine>cmd.exe /d /c "%WINDIR%\Setup\Scripts\Win11DesktopIcons.cmd"</CommandLine></SynchronousCommand></FirstLogonCommands>
    </component>
  </settings>
</unattend>
"#, account);
    Ok(PreprocessBundle { unattend_xml: xml, desktop_script: script })
}

pub fn validate_bundle(xml: &str, script: &str) -> bool {
    let required_xml = ["<settings pass=\"oobeSystem\">", "<HideOnlineAccountScreens>true", "<HideWirelessSetupInOOBE>true", "<UserAccounts>", "<LocalAccounts>", "<Group>Administrators</Group>", "<FirstLogonCommands>", "Win11DesktopIcons.cmd", "</unattend>"];
    let required_script = ["HKU\\DefaultUser", "ProfileList", "NTUSER.DAT", "{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "{59031a47-3f72-44a7-89c5-5595fe6b30ee}", "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", "{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}"];
    required_xml.iter().all(|x| xml.contains(x)) && required_script.iter().all(|x| script.contains(x))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn builds_and_validates() { let b=build_bundle("ImageAdmin").unwrap(); assert!(validate_bundle(&b.unattend_xml,&b.desktop_script)); }
    #[test] fn escapes_account() { let b=build_bundle("bad&name").unwrap(); assert!(b.unattend_xml.contains("bad&amp;name")); }
    #[test] fn rejects_long_account() { assert!(build_bundle("abcdefghijklmnopqrstuvwxyz").is_err()); }
}
