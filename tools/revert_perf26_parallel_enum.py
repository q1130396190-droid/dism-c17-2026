import pathlib

h = pathlib.Path('Demos/Win8Metro/Page_Assoc.h')
t = h.read_text(encoding='utf-8-sig')
nl = '\r\n' if '\r\n' in t else '\n'

start = t.find('    // [PERF-26] Each source now writes into its own vector')
assert start > 0, 'header comment not found'
end = t.find('\u4e0a\u9650 1000\uff09', start)   # "上限 1000）"
assert end > start, 'hkcr line not found'
end = t.find(nl, end)
assert end > start
replacement = (
    '    void AssocBuildEnumHkcu();        // HKCU\\Explorer\\FileExts' + nl +
    '    void AssocBuildEnumHklm();        // HKLM\\SOFTWARE\\Classes' + nl +
    '    void AssocBuildEnumHkcr();        // HKCR\uff08. \u524d\u7f00\u5b50\u952e\uff0c\u4e0a\u9650 1000\uff09')
t = t[:start] + replacement + t[end:]
h.write_bytes(b'\xef\xbb\xbf' + t.encode('utf-8'))
print('header reverted')

p = pathlib.Path('Demos/Win8Metro/Page_Assoc.cpp')
s = p.read_text(encoding='utf-8-sig')
for name in ('AssocBuildEnumHkcu', 'AssocBuildEnumHklm', 'AssocBuildEnumHkcr'):
    old_sig = 'void CPageAssoc::%s(std::vector<DuiLib::CDuiString>& vOut)' % name
    assert s.count(old_sig) == 1, name
    s = s.replace(old_sig, 'void CPageAssoc::%s()' % name)
s = s.replace('vOut.push_back(szName);', 'm_vAssocExt.push_back(szName);')

marker = '    // [PERF-26] The three sources are independent registry walks'
call_start = s.find(marker)
assert call_start > 0, 'call site not found'
tail = 'm_vAssocExt.insert(m_vAssocExt.end(), vCr.begin(), vCr.end());'
tail_pos = s.find(tail, call_start)
assert tail_pos > call_start
brace_end = s.find('    }', tail_pos)
assert brace_end > tail_pos
call_end = s.find('\n', brace_end) + 1
s = s[:call_start] + (
    '    // [PERF-26 result] Running the three registry walks concurrently was tried and\n'
    '    // REVERTED: page build went 63ms -> 78ms. Each walk is small and the thread\n'
    '    // setup cost more than the overlap saved. Serial is faster here.\n'
    '    AssocBuildEnumHkcu();\n'
    '    AssocBuildEnumHklm();\n'
    '    AssocBuildEnumHkcr();\n') + s[call_end:]
s = s.replace('#include <thread>   // [PERF-26] parallel registry walks\n', '')
p.write_bytes(b'\xef\xbb\xbf' + s.encode('utf-8'))
print('cpp reverted')
