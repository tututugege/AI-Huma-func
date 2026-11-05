#!/usr/bin/env python3
"""Build rootfs_spec_<size>.cpio = rootfs.cpio + overlay (SPECint binaries, inputs,
expected outputs, auto-run script).  Linux accepts concatenated newc archives.

usage: mk_testimg.py [test|train] [bench ...]
Commands mirror each benchmark's Spec/object.pm invoke().
"""
import os, re, shutil, subprocess, sys

ROOT = os.environ.get("LLM4TRACE", "/Users/ruyaoh/personal/llm4trace")
CPU = f"{ROOT}/spec2006/benchspec/CPU2006"
BIN = os.environ.get("SPECBIN", f"{ROOT}/linux_rv32/spec/bin")  # SPEC executables
BASE = f"{ROOT}/linux_rv32/rootfs.cpio"
SIZE = sys.argv[1] if len(sys.argv) > 1 else "test"
ONLY = sys.argv[2:]
STAGE = os.environ.get("STAGE", "/private/tmp/spec_stage")
OUTCPIO = os.environ.get("OUTCPIO", f"{ROOT}/linux_rv32/rootfs_spec_{SIZE}.cpio")

EXE = {"400.perlbench": "perlbench", "401.bzip2": "bzip2", "403.gcc": "gcc",
       "429.mcf": "mcf", "445.gobmk": "gobmk", "456.hmmer": "hmmer",
       "458.sjeng": "sjeng", "462.libquantum": "libquantum", "464.h264ref": "h264ref",
       "471.omnetpp": "omnetpp", "473.astar": "astar", "483.xalancbmk": "Xalan"}


def read(p):
    return open(p).read()


def cmds(b, d):
    """-> list of (args, stdin_or_None, stdout_name, compare_name)"""
    ls = sorted(os.listdir(d))
    out = []
    if b == "400.perlbench":
        for f in ls:
            m = re.match(r"(.*)\.(t|pl)$", f)
            if not m or m.group(2) == "t":
                continue
            n = m.group(1)
            if re.search(r"splitmail|perfect|diffmail|checkspam", n):
                if not os.path.exists(f"{d}/{n}.in"):
                    continue
                for line in read(f"{d}/{n}.in").splitlines():
                    if line.startswith("#") or not line.strip():
                        continue
                    p = line.split()
                    o = f"{n}." + ".".join(p) + ".out"
                    out.append((["-I./lib", f] + p, None, o, o))
            else:
                inp = f"{n}.in" if os.path.exists(f"{d}/{n}.in") else None
                out.append((["-I.", "-I./lib", f], inp, f"{n}.out", f"{n}.out"))
    elif b == "401.bzip2":
        for line in read(f"{d}/control").splitlines():
            p = line.split()
            if len(p) == 2 and not line.startswith("#") and int(p[1]) > 0:
                out.append((p, None, f"{p[0]}.out", f"{p[0]}.out"))
    elif b == "403.gcc":
        for f in ls:
            if f.endswith(".i"):
                n = f[:-2]
                out.append(([f, "-o", f"{n}.s"], None, f"{n}.out", f"{n}.s"))
    elif b == "429.mcf":
        for f in ls:
            if f.endswith(".in"):
                n = f[:-3]
                out.append(([f], None, f"{n}.out", f"{n}.out"))
    elif b == "445.gobmk":
        for f in ls:
            if f.endswith(".tst"):
                n = f[:-4]
                out.append((["--quiet", "--mode", "gtp"], f, f"{n}.out", f"{n}.out"))
    elif b == "456.hmmer":
        spec = {"bombesin": "--fixed 0 --mean 325 --num 45000 --sd 200 --seed 0",
                "leng100": "--fixed 0 --mean 425 --num 85000 --sd 300 --seed 0",
                "retro": "--fixed 0 --mean 500 --num 500000 --sd 350 --seed 0"}
        for f in ls:
            m = re.match(r"(.*)\.hmm$", f)
            if not m:
                continue
            n = m.group(1)
            if n in spec:
                out.append((spec[n].split() + [f], None, f"{n}.out", f"{n}.out"))
            elif n == "nph3":
                out.append(([f, "swiss41"], None, f"{n}.out", f"{n}.out"))
    elif b == "458.sjeng":
        for f in ls:
            if f.endswith(".txt"):
                out.append(([f], None, f[:-4] + ".out", f[:-4] + ".out"))
    elif b == "462.libquantum":
        for line in read(f"{d}/control").splitlines():
            p = line.split()
            if p:
                out.append((p[1:], None, f"{p[0]}.out", f"{p[0]}.out"))
    elif b == "464.h264ref":
        for f in ls:
            m = re.match(r"(.*)encoder_(.*)\.cfg$", f)
            if m:
                o = f"{m.group(1)}{m.group(2)}_encodelog.out"
                out.append((["-d", f], None, o, o))
    elif b == "471.omnetpp":
        for f in ls:
            if f.endswith(".ini"):
                out.append(([f], None, "omnetpp.log", "omnetpp.log"))
    elif b == "473.astar":
        for f in ls:
            if f.endswith(".cfg"):
                out.append(([f], None, f[:-4] + ".out", f[:-4] + ".out"))
    elif b == "483.xalancbmk":
        for f in ls:
            m = re.match(r"(.*)\.lst$", f)
            if m:
                for x in read(f"{d}/{f}").split():
                    out.append((["-v", x, "xalanc.xsl"], None, f"{m.group(1)}.out", f"{m.group(1)}.out"))
    return out


def main():
    if os.path.exists(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(f"{STAGE}/spec")
    run = ["#!/bin/sh", f"echo '=== SPECint {SIZE} run start ==='", "cd /spec"]
    for b in EXE:
        if ONLY and b not in ONLY:
            continue
        bd = f"{STAGE}/spec/{b}"
        os.makedirs(bd)
        for sub in ("all", SIZE):
            src = f"{CPU}/{b}/data/{sub}/input"
            if os.path.isdir(src):
                subprocess.run(["cp", "-R", f"{src}/.", bd], check=True)
        exp = f"{CPU}/{b}/data/{SIZE}/output"
        os.makedirs(f"{bd}/expected")
        if os.path.isdir(exp):
            subprocess.run(["cp", "-R", f"{exp}/.", f"{bd}/expected"], check=True)
        shutil.copy(f"{BIN}/{EXE[b]}", f"{bd}/{EXE[b]}")
        os.chmod(f"{bd}/{EXE[b]}", 0o755)
        cl = cmds(b, bd)
        for args, stdin, so, cmpn in cl:
            q = " ".join(f"'{a}'" for a in args)
            redir = (f"< '{stdin}' " if stdin else "") + f"> '{so}' 2> '{so}.err'"
            run += [
                f"cd /spec/{b}",
                "t0=$(cut -d. -f1 /proc/uptime)",
                f"./{EXE[b]} {q} {redir}; rc=$?",
                "t1=$(cut -d. -f1 /proc/uptime)",
                f"echo \"RUN {b} {cmpn} rc=$rc secs=$((t1-t0))\"",
                "cd /spec",
            ]
        run += [
            f"cd /spec/{b}",
            "for f in expected/*; do n=${f#expected/}; "
            "if [ -f \"$n\" ] && cmp -s \"$f\" \"$n\"; then echo \"CHECK " + b + " $n MATCH\"; "
            "else echo \"CHECK " + b + " $n DIFF\"" + ("; echo \"@@FILE " + b + " $n\"; base64 \"$n\" 2>&1; echo \"@@END\"" if SIZE != "ref" else "") + "; fi; done",
            "cd /spec",
        ]
    run += ["sync", "sleep 3", "echo '=== SPECint run done ==='"]
    open(f"{STAGE}/spec/run_all.sh", "w").write("\n".join(run) + "\n")
    os.chmod(f"{STAGE}/spec/run_all.sh", 0o755)
    os.makedirs(f"{STAGE}/etc/init.d")
    open(f"{STAGE}/etc/init.d/S99spec", "w").write(
        "#!/bin/sh\ncase \"$1\" in start)\n  /spec/run_all.sh > /dev/console 2>&1\n  poweroff -f ;;\nesac\n")
    os.chmod(f"{STAGE}/etc/init.d/S99spec", 0o755)
    ov = f"{STAGE}.cpio"
    subprocess.run(["bsdtar", "--format", "newc", "--uid", "0", "--gid", "0", "-cf", ov, "-C", STAGE, "."], check=True)
    with open(OUTCPIO, "wb") as o:
        for p in (BASE, ov):
            with open(p, "rb") as i:
                shutil.copyfileobj(i, o)
    print(f"{OUTCPIO}: {os.path.getsize(OUTCPIO)/1e6:.1f} MB; commands: {sum(1 for l in run if l.startswith('./'))}")


main()
