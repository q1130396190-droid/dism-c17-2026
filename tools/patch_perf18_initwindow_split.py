import pathlib

p = pathlib.Path('Demos/Win8Metro/FrameWnd.cpp')
raw = p.read_bytes()
pathlib.Path('Demos/Win8Metro/FrameWnd.cpp.bak_perf18').write_bytes(raw)
text = raw.decode('utf-8', errors='surrogateescape')
NL = '\r\n' if '\r\n' in text[:4000] else '\n'


def mark(tag):
    q = '"'
    return (
        '    if( ::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0 ) {' + NL +
        '        char szQ[96] = {};' + NL +
        '        ::sprintf_s(szQ, "[perf]   iw:%-14s %llums", ' + q + tag + q + ',' + NL +
        '                    (unsigned long long)(::GetTickCount64() - g_ullIwT0));' + NL +
        '        ::fputs(szQ, stdout); ::fputc(10, stdout); ::fflush(stdout);' + NL +
        '        g_ullIwT0 = ::GetTickCount64();' + NL +
        '    }' + NL)


def inject(before_line, tag):
    global text
    assert text.count(before_line) == 1, (tag, text.count(before_line))
    text = text.replace(before_line, mark(tag) + before_line)


anchor = '    ApplyStartupCheckAndWin11();'
assert text.count(anchor) == 1
text = text.replace(anchor,
                    '    static ULONGLONG g_ullIwT0 = ::GetTickCount64();' + NL +
                    '    g_ullIwT0 = ::GetTickCount64();' + NL +
                    anchor)

inject('    LoadSkinGlyphs();', 'privil+pe')
inject('    InitSystemInfo();', 'dataengine')
inject('    InitSessionProgress();', 'sysinfo')
inject('    InitTrayIcon();', 'sessprog')
inject('    InitTooltips();', 'trayicon')
inject('    CenterWindow();', 'tooltips')

p.write_bytes(text.encode('utf-8', errors='surrogateescape'))
print('ok, CRLF=', NL == '\r\n')
