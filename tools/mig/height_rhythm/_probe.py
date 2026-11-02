import os, re, collections
root = r"D:\DuiLib_Ultimate-master\bin64\skin\Win8Metro"
tag = re.compile(r"<\s*([A-Za-z_][A-Za-z0-9_]*)")
c = collections.Counter()
for dp, dn, fn in os.walk(root):
    for f in fn:
        if f.lower().endswith(".xml"):
            p = os.path.join(dp, f)
            with open(p, "rb") as fh:
                s = fh.read().decode("utf-8", "replace")
            for m in tag.finditer(s):
                c[m.group(1)] += 1
for k, v in c.most_common(60):
    print(f"{v:5d}  {k}")
