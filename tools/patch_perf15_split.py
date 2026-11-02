import pathlib

p = pathlib.Path('DuiLib/Core/UIDlgBuilder.cpp')
b = p.read_bytes()
pathlib.Path('DuiLib/Core/UIDlgBuilder.cpp.bak_perf15').write_bytes(b)

# --- 1) depth guard right after the anonymous namespace closes (LF-terminated)
a1 = b'} // namespace\n'
assert b.count(a1) == 1
guard = (a1
         + b'\r\n'
           b'namespace {\r\n'
           b'// [PERF-15] The split markers belong to the outermost skin build only;\r\n'
           b'// nested sub-skin builds previously printed 18 untimed lines.\r\n'
           b'thread_local int t_nDlgBuildDepth = 0;\r\n'
           b'struct CDlgBuildDepthGuard {\r\n'
           b'\tCDlgBuildDepthGuard() { ++t_nDlgBuildDepth; }\r\n'
           b'\t~CDlgBuildDepthGuard() { --t_nDlgBuildDepth; }\r\n'
           b'};\r\n'
           b'} // namespace\r\n')
b = b.replace(a1, guard)

# --- 2) drop the noisy per-sub-skin marker
old_marker = (b'\t\t// [PERF-15] Split "create+skin" into its two real halves: skin parsing\r\n'
              b'\t\t// (already finished when we reach here) and tree construction.\r\n'
              b'\t\tif( ::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0 ) {\r\n'
              b'\t\t\t::fputs("[perf] dlg xml-parse-ready", stdout); ::fputc(10, stdout); ::fflush(stdout);\r\n'
              b'\t\t}\r\n'
              b'\r\n')
assert b.count(old_marker) == 1, b.count(old_marker)
b = b.replace(old_marker, b'')

# --- 3) timing start + depth guard on the outer Create
sig = (b'\tCControlUI* CDialogBuilder::Create(STRINGorID xml, LPCTSTR type, IDialogBuilderCallback* pCallback, \r\n'
       b'\t\tCPaintManagerUI* pManager, CControlUI* pParent)\r\n'
       b'\t{\r\n')
assert b.count(sig) == 1, b.count(sig)
b = b.replace(sig, sig +
              b'\t\t// [PERF-15] Skin parsing (this function) and control-tree build (the\r\n'
              b'\t\t// delegated Create below) used to be one opaque "create+skin" number.\r\n'
              b'\t\tconst bool bPerfTop = (::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0)\r\n'
              b'\t\t\t&& (t_nDlgBuildDepth == 0);\r\n'
              b'\t\tconst ULONGLONG ullParseT0 = bPerfTop ? ::GetTickCount64() : 0;\r\n'
              b'\t\tCDlgBuildDepthGuard dlgDepthGuard;\r\n')

# --- 4) emit the parse timing before delegating to the tree builder
ret = b'\t\treturn Create(pCallback, pManager, pParent);\r\n'
assert b.count(ret) == 1, b.count(ret)
b = b.replace(ret,
              b'\t\tif (bPerfTop) {\r\n'
              b'\t\t\tchar szP[96] = {};\r\n'
              b'\t\t\t::sprintf_s(szP, "[perf] dlg xml-parse=%llums",\r\n'
              b'\t\t\t            (unsigned long long)(::GetTickCount64() - ullParseT0));\r\n'
              b'\t\t\t::fputs(szP, stdout); ::fputc(10, stdout); ::fflush(stdout);\r\n'
              b'\t\t}\r\n'
              + ret)

p.write_bytes(b)
print('written bytes', len(b))
