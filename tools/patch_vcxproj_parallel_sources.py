import pathlib

p = pathlib.Path('Demos/Win8Metro/Win8Metro.vcxproj')
raw = p.read_bytes()
text = raw.decode('utf-8-sig')
NL = '\r\n'

if 'StartupService_Query.h' not in text:
    hdr = '<ClInclude Include="StartupService.h" />'
    assert text.count(hdr) == 1
    text = text.replace(hdr, hdr + NL
                        + '    <ClInclude Include="StartupService_Query.h" />' + NL
                        + '    <ClInclude Include="AssocQuery_Parallel.h" />')

if 'StartupService_Query.cpp' not in text:
    pch_line = ('      <PrecompiledHeader Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'">'
                'NotUsing</PrecompiledHeader>')
    target = pch_line + NL + '    </ClCompile>' + NL + '    <ClCompile Include="StartupService.cpp">'
    assert text.count(target) == 1, text.count(target)
    block = ''
    for src in ('StartupService_Query.cpp', 'AssocQuery_Parallel.cpp'):
        block += (
            '    <ClCompile Include="%s">' % src + NL +
            '      <PrecompiledHeader Condition="\'$(Configuration)|$(Platform)\'==\'Debug|Win32\'">'
            'NotUsing</PrecompiledHeader>' + NL +
            '      <PrecompiledHeader Condition="\'$(Configuration)|$(Platform)\'==\'Release|Win32\'">'
            'NotUsing</PrecompiledHeader>' + NL +
            '      <PrecompiledHeader Condition="\'$(Configuration)|$(Platform)\'==\'Debug|x64\'">'
            'NotUsing</PrecompiledHeader>' + NL +
            '      <PrecompiledHeader Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'">'
            'NotUsing</PrecompiledHeader>' + NL +
            '    </ClCompile>' + NL)
    text = text.replace(target, pch_line + NL + '    </ClCompile>' + NL + block
                        + '    <ClCompile Include="StartupService.cpp">')

p.write_bytes(b'\xef\xbb\xbf' + text.encode('utf-8'))
print('ok')
