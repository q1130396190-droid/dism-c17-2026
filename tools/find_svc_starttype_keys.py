import pathlib
import re

src = pathlib.Path('Demos/Win8Metro/StringManager.cpp').read_text(encoding='utf-8')

def dec(s):
    return re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), s)

for m in re.finditer(r'\{ "([\w.]+)",\s*L"([^"]*)"', src):
    v = dec(m.group(2))
    if any(k in v for k in ('\u81ea\u52a8', '\u624b\u52a8', '\u7981\u7528')):
        print(m.group(1), '=', v[:50])
