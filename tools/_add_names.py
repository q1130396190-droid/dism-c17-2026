import re, os, glob

dir_path = r"d:\DuiLib_Ultimate-master\bin64\skin\Classic"
counter = 0

for fpath in glob.glob(os.path.join(dir_path, "**", "*.xml"), recursive=True):
    with open(fpath, "r", encoding="utf-8") as f:
        content = f.read()
    orig = content

    def add_name(m):
        global counter
        tag = m.group(1)
        spaces = m.group(2)
        counter += 1
        return f'<{tag} name="cl_{counter}"{spaces}'

    content = re.sub(r'<(HorizontalLayout|VerticalLayout|TabLayout)(\s+)(?!name=)', add_name, content)

    if content != orig:
        with open(fpath, "w", encoding="utf-8", newline="") as f:
            f.write(content)
        print(f"  patched: {os.path.basename(fpath)}")

print(f"\nTotal unnamed containers named: {counter}")
