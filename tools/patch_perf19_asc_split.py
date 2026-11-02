import pathlib

p = pathlib.Path('Demos/Win8Metro/FrameWnd.cpp')
raw = p.read_bytes()
pathlib.Path('Demos/Win8Metro/FrameWnd.cpp.bak_perf19').write_bytes(raw)
text = raw.decode('utf-8-sig')

# The file uses blank-line-separated statements (LF, double-spaced source).
probe = ('    if( ::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0 )\n'
         '    {\n'
         '        char szA[112] = {};\n'
         '        ::sprintf_s(szA, "[perf]     asc:%-12s %llums", TAGHERE,\n'
         '                    (unsigned long long)(::GetTickCount64() - g_ullAscT0));\n'
         '        ::fputs(szA, stdout); ::fputc(10, stdout); ::fflush(stdout);\n'
         '        g_ullAscT0 = ::GetTickCount64();\n'
         '    }\n\n')


def inject(anchor, tag):
    global text
    assert text.count(anchor) == 1, (tag, text.count(anchor))
    text = text.replace(anchor, probe.replace("TAGHERE", chr(34)+tag+chr(34)) + anchor)


start = '    CDuiString sSkinDir = CPaintManagerUI::GetResourcePath();'
assert text.count(start) == 1
text = text.replace(start,
                    '    static ULONGLONG g_ullAscT0 = ::GetTickCount64();\n'
                    '    g_ullAscT0 = ::GetTickCount64();\n'
                    + start)

inject('    if (bWantWin11 && m_pStartupCheck->report.uWin11Effects != 0)',
       'check-run')
inject('    if (m_pStartupCheck->report.bD2DTextAvailable)',
       'apply-effects')
inject('    if (!startupcheck::IsFontFamilyAvailable(L"Segoe Fluent Icons"))',
       'd2d-text')
inject('        const theme::EMode eMode = theme::SystemMode();',
       'font-fallback')
inject('    ReportStartupCheckToUi(sReport.c_str());',
       'theme-apply')

p.write_bytes(b'\xef\xbb\xbf' + text.encode('utf-8'))
print('ok')
