import pathlib

p = pathlib.Path('DuiLib/Utils/WinImplBase.cpp')
b = p.read_bytes()
pathlib.Path('DuiLib/Utils/WinImplBase.cpp.bak_perf17').write_bytes(b)

# 1) time m_pm.Init
init_anchor = b'\t\tm_pm.Init(m_hWnd, GetManagerName());\r\n'
if b.count(init_anchor) != 1:
    raise SystemExit('init anchor count=%d' % b.count(init_anchor))
b = b.replace(init_anchor,
              b'\t\tconst BOOL bPerfProbe = (::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0);\r\n'
              b'\t\tconst ULONGLONG ullPmInitT0 = ::GetTickCount64();\r\n'
              + init_anchor +
              b'\t\tif (bPerfProbe) {\r\n'
              b'\t\t\tchar szI[96] = {};\r\n'
              b'\t\t\t::sprintf_s(szI, "[perf] pm init=%llums",\r\n'
              b'\t\t\t            (unsigned long long)(::GetTickCount64() - ullPmInitT0));\r\n'
              b'\t\t\t::fputs(szI, stdout); ::fputc(10, stdout); ::fflush(stdout);\r\n'
              b'\t\t}\r\n')

# 2) remove the now-duplicate bPerfProbe declaration lower down
old_decl = (b'\t\t// [PERF-15] Instrument the two halves of skin loading. "create+skin"\r\n'
            b'\t\t// measured 171-250ms as one opaque step; this makes the split visible\r\n'
            b'\t\t// without adding any per-node cost (one GetTickCount64 per phase).\r\n'
            b'\t\tconst BOOL bPerfProbe = (::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0);\r\n')
if b.count(old_decl) != 1:
    raise SystemExit('dup decl count=%d' % b.count(old_decl))
b = b.replace(old_decl,
              b'\t\t// [PERF-15] Skin tree build (XML parse is timed inside CDialogBuilder).\r\n')

# 3) split AttachDialog and InitWindow -- anchors avoid GBK comment bytes
att_anchor = b'\t\tm_pm.AttachDialog(pRoot);\r\n'
if b.count(att_anchor) != 1:
    raise SystemExit('attach count=%d' % b.count(att_anchor))
b = b.replace(att_anchor,
              b'\t\tconst ULONGLONG ullAttachT0 = ::GetTickCount64();\r\n'
              b'\t\tm_pm.AttachDialog(pRoot);\r\n'
              b'\t\tif (bPerfProbe) {\r\n'
              b'\t\t\tchar szA[96] = {};\r\n'
              b'\t\t\t::sprintf_s(szA, "[perf] attach=%llums",\r\n'
              b'\t\t\t            (unsigned long long)(::GetTickCount64() - ullAttachT0));\r\n'
              b'\t\t\t::fputs(szA, stdout); ::fputc(10, stdout); ::fflush(stdout);\r\n'
              b'\t\t}\r\n')

init_win = b'\t\tInitWindow();\r\n'
if b.count(init_win) != 1:
    raise SystemExit('initwindow count=%d' % b.count(init_win))
b = b.replace(init_win,
              b'\t\tconst ULONGLONG ullInitT0 = ::GetTickCount64();\r\n'
              b'\t\tInitWindow();\r\n'
              b'\t\tif (bPerfProbe) {\r\n'
              b'\t\t\tchar szW[96] = {};\r\n'
              b'\t\t\t::sprintf_s(szW, "[perf] initwindow=%llums",\r\n'
              b'\t\t\t            (unsigned long long)(::GetTickCount64() - ullInitT0));\r\n'
              b'\t\t\t::fputs(szW, stdout); ::fputc(10, stdout); ::fflush(stdout);\r\n'
              b'\t\t}\r\n')

p.write_bytes(b)
print('written bytes', len(b))
