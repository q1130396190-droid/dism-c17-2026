"""Make the solution buildable in every configuration it advertises.

Measured problem: DuiLib.sln offers 16 solution configurations, but each Demos project
declared only Debug|x64 and Release|x64. Building e.g. DebugA|x64 therefore fails with
MSB8013 ("this project does not contain configuration DebugA|x64") rather than skipping --
so 14 of the 16 advertised configurations could not build the solution at all.

Design decision: the Demos projects stay minimal (2 real configurations). The solution
maps every *variant* onto one of those two, chosen by what the variant means:

    Debug*   -> Debug      (debug CRT, no optimisation)
    Release* -> Release    (release CRT, optimised)
    S*       -> Release    (the S prefix denotes a static-library flavour of the framework;
                            an application links the DLL, so there is nothing static to
                            link against -- Release is the meaningful counterpart)

This keeps one artifact per flavour, avoids six near-identical copies of each project's
settings, and removes the silent MSB8013 class of failure. Every mapping gets a matching
.Build.0 line so the project is actually built, not merely activated.
"""
import re

SLN = r"DuiLib.sln"
DEMOS = ["ActivationKit", "CBSHost", "NCleaner", "Win8Metro"]


def target_for(sol_cfg: str) -> str:
    """Map a solution configuration variant onto a Demos project configuration."""
    return "Debug|x64" if sol_cfg.startswith("Debug") else "Release|x64"


def main() -> None:
    b = open(SLN, "rb").read()
    bom = b[:3] == b"\xef\xbb\xbf"
    text = b.decode("utf-8-sig" if bom else "utf-8")

    sol_cfgs = re.findall(r"^\t\t(\w+\|x64) = ", text, re.M)
    if len(sol_cfgs) != 16:
        raise SystemExit("unexpected solution configuration count: %d" % len(sol_cfgs))

    guid_of = {}
    for name, _path, guid in re.findall(r'= "([^"]+)", "([^"]+)", "\{([^}]+)\}"', text):
        guid_of[name] = guid

    changed = 0
    for name in DEMOS:
        g = guid_of[name]
        for sol_cfg in sol_cfgs:
            want = target_for(sol_cfg)
            pattern = re.compile(
                r"(\{%s\}\.%s\.ActiveCfg = )(\S+)" % (re.escape(g), re.escape(sol_cfg)))
            text, n1 = pattern.subn(lambda m: m.group(1) + want, text)
            pattern_b = re.compile(
                r"(\{%s\}\.%s\.Build\.0 = )(\S+)" % (re.escape(g), re.escape(sol_cfg)))
            text, n2 = pattern_b.subn(lambda m: m.group(1) + want, text)
            if n1 != 1 or n2 != 1:
                raise SystemExit(
                    "%s %s: expected 1 ActiveCfg and 1 Build.0, got %d/%d"
                    % (name, sol_cfg, n1, n2))
            changed += 1

    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    open(SLN, "wb").write(data)
    print("remapped %d solution-configuration entries across %d projects" % (changed, len(DEMOS)))

    # verify: every advertised solution configuration now targets a real project config
    verify = open(SLN, "rb").read().decode("utf-8-sig")
    for name in DEMOS:
        g = guid_of[name]
        pairs = dict(re.findall(
            r"\{%s\}\.(\w+\|x64)\.ActiveCfg = (\S+)" % re.escape(g), verify))
        bad = [k for k, v in pairs.items() if v not in ("Debug|x64", "Release|x64")]
        print("  %-14s entries=%d out-of-range=%d" % (name, len(pairs), len(bad)))


if __name__ == "__main__":
    main()
