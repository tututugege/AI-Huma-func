#!/usr/bin/env python3
"""Cross-build SPEC CPU2006 SPECint for rv32 Linux (no C/F/D), static glibc.  Runs inside the Buildroot VM.

ARCH selects the ISA (default rv32ima + Zba/Zbb/Zbs = "rv32imab"); the static glibc itself stays rv32ima.

usage: build_spec.py <bench> [<bench> ...]     (e.g. 429.mcf; 'all-c' = C benches)
Reads sources from $SPEC/<bench>/{src,Spec/object.pm}, writes $OUT/<exename>.
"""
import os, re, subprocess, sys

HOME = os.path.expanduser("~")
SPEC = os.environ.get("SPEC", f"{HOME}/spec")
OUT = os.environ.get("OUT", f"{HOME}/spec_out")
BUILD = os.environ.get("BUILD", f"{HOME}/spec_build")
TC = os.environ.get("TC", f"{HOME}/br13/host/bin/riscv32-buildroot-linux-gnu-")  # GCC 13.3: GCC 12.4 ICEs on rv32 Zbs
JOBS = os.environ.get("JOBS", "8")

ARCH = os.environ.get("ARCH", "-march=rv32ima_zicsr_zifencei_zba_zbb_zbs -mabi=ilp32")
# base flags; per-benchmark overrides below
CFLAGS = f"-O2 {ARCH} -std=gnu89 -fsigned-char -fcommon -fno-strict-aliasing -w -DSPEC_CPU -DNDEBUG"
CXXFLAGS = f"-O2 {ARCH} -std=gnu++98 -fsigned-char -fpermissive -fno-strict-aliasing -w -DSPEC_CPU -DNDEBUG"

# name -> (extra flags, extra link libs)
PER = {
    "400.perlbench": ("-DPERL_CORE -DSPEC_CPU_LINUX -I.", "-lm"),
    "401.bzip2": ("", ""),
    "403.gcc": ("-DSPEC_CPU_LINUX -I.", "-lm"),
    "429.mcf": ("-DWANT_STDC_PROTO", "-lm"),
    "445.gobmk": ("-DHAVE_CONFIG_H -I. -I.. -I../include -I./include -Iinclude -Iengine -Isgf -Iutils -Iinterface -Ipatterns", "-lm"),
    "456.hmmer": ("", "-lm"),
    "458.sjeng": ("", ""),
    "462.libquantum": ("-DSPEC_CPU_LINUX", "-lm"),
    "464.h264ref": ("-DSPEC_CPU_LINUX", "-lm"),
    "471.omnetpp": ("-I. -Iomnet_include -Ilibs/envir", "-lm"),
    "473.astar": ("-DSPEC_CPU_LITTLE_ENDIAN", "-lm"),
    "483.xalancbmk": ("-DSPEC_CPU_LINUX -DAPP_NO_THREADS -DXALAN_INMEM_MSG_LOADER -I. -Ixercesc -Ixercesc/dom -Ixercesc/dom/impl -Ixercesc/sax -Ixercesc/util/MsgLoaders/InMemory -Ixercesc/util/Transcoders/Iconv -Ixalanc/include -DPROJ_XMLPARSER -DPROJ_XMLUTIL -DPROJ_PARSERS -DPROJ_SAX4C -DPROJ_SAX2 -DPROJ_DOM -DPROJ_VALIDATORS -DXML_USE_INMEM_MESSAGELOADER -DXML_USE_INMEM_MESSAGELOADER", "-lm"),
}
STD_OVERRIDE = {"462.libquantum": "-std=gnu99", "464.h264ref": "-std=gnu99"}
CXX = {"471.omnetpp", "473.astar", "483.xalancbmk"}
ALL_C = [b for b in PER if b not in CXX]


def parse(bench):
    txt = open(f"{SPEC}/{bench}/Spec/object.pm").read()
    txt = re.sub(r"(?m)^\s*#.*$", "", txt)
    exe = re.search(r"\$exename\s*=\s*'([^']+)'", txt).group(1)
    m = re.search(r"@sources\s*=(.*?);", txt, re.S)
    srcs = re.findall(r"[\w./+-]+\.(?:c|cc|cpp)\b", m.group(1))
    return exe, srcs


def build(bench):
    exe, srcs = parse(bench)
    src = f"{SPEC}/{bench}/src"
    bd = f"{BUILD}/{bench}"
    os.makedirs(bd, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    is_cxx = bench in CXX
    cc = TC + ("g++" if is_cxx else "gcc")
    flags = CXXFLAGS if is_cxx else CFLAGS
    if bench in STD_OVERRIDE:
        flags = re.sub(r"-std=\S+", STD_OVERRIDE[bench], flags)
    extra, libs = PER[bench]
    objs = [re.sub(r"\.(c|cc|cpp)$", ".o", s).replace("/", "__") for s in srcs]
    mk = [f"CC={cc}", f"FLAGS={flags} {extra}", ""]
    mk.append(f"all: {OUT}/{exe}\n")
    mk.append(f"{OUT}/{exe}: " + " ".join(f"{bd}/{o}" for o in objs))
    mk.append(f"\t$(CC) -static $(FLAGS) -o $@ $^ {libs}\n")
    for s, o in zip(srcs, objs):
        mk.append(f"{bd}/{o}: {src}/{s}")
        mk.append(f"\t$(CC) $(FLAGS) -c -o $@ $< 2> $@.err || (cat $@.err; false)\n")
    mkf = f"{bd}/Makefile"
    open(mkf, "w").write("\n".join(mk))
    print(f"== {bench}: {len(srcs)} sources -> {exe}", flush=True)
    r = subprocess.run(["make", "-k", "-j", JOBS, "-C", src, "-f", mkf], capture_output=True, text=True)
    open(f"{bd}/build.log", "w").write(r.stdout + r.stderr)
    ok = r.returncode == 0 and os.path.exists(f"{OUT}/{exe}")
    print(f"   {'OK' if ok else 'FAIL'}  (log: {bd}/build.log)", flush=True)
    return ok


if __name__ == "__main__":
    args = sys.argv[1:] or ["all-c"]
    benches = []
    for a in args:
        benches += ALL_C if a == "all-c" else [a]
    res = {b: build(b) for b in benches}
    sys.exit(0 if all(res.values()) else 1)
