"""Separate the C++23 static-library flavour from its DLL counterpart.

Measured defect (found by the config-matrix gate): SDebug23 and SRelease23 both emit
lib\\cpp23\\x64\\DuiLibCxx23.lib, and SDebugA23/SReleaseA23 both emit the ACxx23 variant.
Same directory, same TargetName -> a Debug static library silently overwrites the Release
one, or the reverse, depending on build order. The four 23-variants also lacked the
_static naming the non-23 static configurations now use, and they declared no
<ImportLibrary>, so lib.exe fell back to <OutDir>\\<TargetName>.lib -- i.e. straight into
lib\\cpp23, the directory the DLL import libraries would use for that flavour.

Fix, consistent with the earlier non-23 fix:
  * static 23 configs get TargetName suffix _static / _static_d and keep A for MBCS
  * static 23 configs get their own OutDir: Lib\\Static\\cpp23\\<platform>
  * explicit ImportLibrary in the same directory so lib.exe cannot fall back into the
    DLL import-library directory
"""

import re
import xml.etree.ElementTree as ET

P = r"DuiLib\DuiLib.vcxproj"
BS = chr(92)

# configuration -> TargetName suffix (ProjectName/A already present in the template)
STATIC23 = {
    "SDebug23":    ("_static_dCxx23",  "SDebug23"),
    "SDebugA23":   ("A_static_dCxx23", "SDebugA23"),
    "SRelease23":  ("_staticCxx23",    "SRelease23"),
    "SReleaseA23": ("A_staticCxx23",   "SReleaseA23"),
}

NEW_OUTDIR = ("<OutDir Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\">"
              "$(SolutionDir)Lib" + BS + "Static" + BS + "cpp23" + BS + "$(Platform)" + BS + "</OutDir>")


def main():
    b = open(P, "rb").read()
    bom = b[:3] == b"\xef\xbb\xbf"
    t = b.decode("utf-8-sig" if bom else "utf-8")

    for cfg, (suffix, _dir) in STATIC23.items():
        cond = "'$(Configuration)|$(Platform)'=='%s|x64'" % cfg

        # --- OutDir ---
        pat = re.compile(r"<OutDir Condition=\"%s\">[^<]*</OutDir>" % re.escape(cond))
        if pat.search(t):
            t = pat.sub(lambda m: NEW_OUTDIR % cfg, t)
        else:
            raise SystemExit("%s: OutDir element not found" % cfg)

        # --- TargetName ---
        tn_pat = re.compile(r"<TargetName Condition=\"%s\">[^<]*</TargetName>" % re.escape(cond))
        t = tn_pat.sub(lambda m: "<TargetName Condition=\"%s\">$(ProjectName)%s</TargetName>"
                                  % (cond, suffix), t)

        # --- ImportLibrary: must be explicit and inside the static directory ---
        il = ("<ImportLibrary>$(SolutionDir)Lib" + BS + "Static" + BS + "cpp23" + BS
              + "$(Platform)" + BS + "$(ProjectName)" + suffix + ".lib</ImportLibrary>")
        # locate the config's ItemDefinitionGroup and inject into its Lib element
        grp = re.compile(r"(<ItemDefinitionGroup Condition=\"%s\">)(.*?)(</ItemDefinitionGroup>)"
                         % re.escape(cond), re.S)

        def add_lib(m):
            body = m.group(2)
            if "<ImportLibrary>" in body:
                body = re.sub(r"<ImportLibrary>[^<]*</ImportLibrary>", il, body)
            elif "<Lib>" in body:
                body = body.replace("<Lib>", "<Lib>\n      " + il, 1)
            else:
                body = body + "\n    <Lib>\n      " + il + "\n    </Lib>\n  "
            return m.group(1) + body + m.group(3)

        t, n = grp.subn(add_lib, t)
        if n != 1:
            raise SystemExit("%s: expected 1 ItemDefinitionGroup, matched %d" % (cfg, n))

    data = t.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    open(P, "wb").write(data)

    ET.parse(P)
    print("patched; XML OK; NUL=%d" % open(P, "rb").read().count(b"\x00"))
    for line in t.splitlines():
        if any(k in line for k in ("SDebug23", "SDebugA23", "SRelease23", "SReleaseA23")):
            if "OutDir" in line or "TargetName" in line or "ImportLibrary" in line:
                print("   ", line.strip())


if __name__ == "__main__":
    main()
