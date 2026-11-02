import pathlib

p = pathlib.Path('DuiLib/DuiLib.vcxproj')
raw = p.read_bytes()
text = raw.decode('utf-8-sig')
NL = '\r\n'

added = []
if 'UIAutomationProvider.cpp' not in text:
    a = r'    <ClCompile Include="Utils\UIAutomation.cpp" />'
    assert text.count(a) == 1, text.count(a)
    text = text.replace(a, a + NL + r'    <ClCompile Include="Utils\UIAutomationProvider.cpp" />')
    added.append('cpp')
if 'UIAutomationProvider.h' not in text:
    b = r'    <ClInclude Include="Utils\UIAutomation.h" />'
    assert text.count(b) == 1, text.count(b)
    text = text.replace(b, b + NL + r'    <ClInclude Include="Utils\UIAutomationProvider.h" />')
    added.append('h')

if 'UIAutomationCore.lib' not in text:
    n = text.count('oleacc.lib;')
    text = text.replace('oleacc.lib;', 'oleacc.lib;UIAutomationCore.lib;')
    added.append('lib x%d' % n)

p.write_bytes(b'\xef\xbb\xbf' + text.encode('utf-8'))
print('added', added)
