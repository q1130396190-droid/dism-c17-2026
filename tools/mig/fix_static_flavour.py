"""Separate the static-library flavour (S*) from the DLL flavour in DuiLib.vcxproj.

Measured defect: building the solution's SDebug configuration left 60 .obj members inside
Lib\\x64\\DuiLib_d.lib -- i.e. the static library had replaced the DLL's import library.
The next DebugA solution build then failed with LNK2038 "MTd_StaticDebug does not match
MDd_DynamicDebug", because the app was linking an import library that was not one.

Two independent causes, and both had to be fixed:
  1. <TargetName> for SDebug is $(ProjectName)_d, identical to Debug. Same file name, and
     until the previous patch also the same directory, so one silently overwrote the other.
  2. <IntDir> for SDebug is Temp\\Debug\\$(Platform), identical to Debug, so the two
     configurations also shared object files. Even with distinct output names, incremental
     builds would mix objects compiled with different ConfigurationType.

Fix: give every S* configuration its own TargetName suffix (_static) and its own IntDir.
DLL import libraries keep their established names, so no consumer project changes.
"""

P = r"DuiLib\DuiLib.vcxproj"

# (configuration, TargetName value, IntDir tail)
STATIC_CFGS = [
    ("SDebug",    "$(ProjectName)_static_d",   "SDebug"),
    ("SDebugA",   "$(ProjectName)A_static_d",  "SDebugA"),
    ("SRelease",  "$(ProjectName)_static",     "SRelease"),
    ("SReleaseA", "$(ProjectName)A_static",    "SReleaseA"),
]


def main():
    b = open(P, "rb").read()
    bom = b[:3] == b"\xef\xbb\xbf"
    t = b.decode("utf-8-sig" if bom else "utf-8")

    for cfg, target, intdir in STATIC_CFGS:
        cond = "'$(Configuration)|$(Platform)'=='%s|x64'" % cfg

        # TargetName: replace an existing element if present, otherwise insert one.
        tn = "<TargetName Condition=\"%s\">" % cond
        idx = t.find(tn)
        if idx >= 0:
            end = t.find("</TargetName>", idx) + len("</TargetName>")
            t = t[:idx] + "%s%s</TargetName>" % (tn, target) + t[end:]
        else:
            anchor = "<IntDir Condition=\"%s\">" % cond
            a = t.find(anchor)
            if a < 0:
                raise SystemExit("%s: no TargetName and no IntDir anchor" % cfg)
            t = t[:a] + "%s%s</TargetName>\n    " % (tn, target) + t[a:]

        # IntDir: always rewrite to a configuration-specific directory.
        anchor = "<IntDir Condition=\"%s\">" % cond
        a = t.find(anchor)
        if a < 0:
            raise SystemExit("%s: IntDir anchor missing" % cfg)
        end = t.find("</IntDir>", a) + len("</IntDir>")
        t = t[:a] + "%s$(SolutionDir)Temp\\%s\\$(Platform)\\</IntDir>" % (anchor, intdir) + t[end:]

    data = t.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    open(P, "wb").write(data)

    import xml.etree.ElementTree as ET
    ET.parse(P)
    verify = open(P, "rb").read().decode("utf-8-sig")
    print("patched; XML OK; NUL=%d" % open(P, "rb").read().count(b"\x00"))
    for line in verify.splitlines():
        if ("TargetName Condition" in line and "S" in line) or \
           ("IntDir Condition" in line and any(c in line for c in ("SDebug", "SRelease"))):
            print("  ", line.strip())


if __name__ == "__main__":
    main()
