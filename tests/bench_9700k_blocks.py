#!/usr/bin/env python3
# lz4 : default block settings (-B7, independent blocks) vs -B5 -BD (and -B5 as control) : compression and decompression
# temporary : not for merge
# usage : python3 tests/bench_9700k_blocks.py /path/to/silesia.tar [workdir]   (ROUNDS=11 by default)
# builds lz4 from this clone (CC is honored), uses the default nb of threads, and writes results.txt in workdir
import glob, os, shlex, shutil, statistics, subprocess, sys, time

if len(sys.argv) < 2:
    sys.exit("usage : python3 %s /path/to/silesia.tar [workdir]" % sys.argv[0])
SIL = os.path.abspath(sys.argv[1])
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "bench_9700k_blocks")
ROUNDS = int(os.environ.get("ROUNDS", "11"))
FBROUNDS = int(os.environ.get("FBROUNDS", "21"))
LZ4 = os.path.join(WORK, "lz4")
LEVELS = ["-1", "-9"]
VARIANTS = ["", " -B5 -BD", " -B5"]   # default, linked 64 KB blocks, independent 64 KB blocks (control)
SETS = [l + v for l in LEVELS for v in VARIANTS]
PIPE, DEVNULL = subprocess.PIPE, subprocess.DEVNULL

os.makedirs(WORK, exist_ok=True)
LOG = open(os.path.join(WORK, "results.txt"), "w")
def out(s=""):
    print(s, flush=True)
    LOG.write(s + "\n"); LOG.flush()

def tag(s): return s.replace(" ", "").replace("-", "")

# build
ncpu = os.sysconf("SC_NPROCESSORS_ONLN")   # as lz4
subprocess.run(["make", "-C", REPO, "-s", "-j%d" % ncpu, "lz4"], check=True, stdout=DEVNULL)
shutil.copy(os.path.join(REPO, "lz4"), LZ4)
cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), "?")
cc = os.environ.get("CC", "cc")
ccv = subprocess.run([cc, "--version"], stdout=PIPE, text=True).stdout.splitlines()[0]
out("== %s" % subprocess.run(["git", "-C", REPO, "log", "-1", "--format=%h %s"], stdout=PIPE, text=True).stdout.strip())
out("cpu : %s ; %d cpus ; lz4 default threads : %d" % (cpu, ncpu, max(1, ncpu - 1 - ncpu // 4)))
out("compiler : %s ; kernel %s ; rounds %d" % (ccv, os.uname().release, ROUNDS))
t_start = time.time()

# inputs : silesia, 200 files of 1 MB, 50 files of 4 MB
def split(n, size):
    d = os.path.join(WORK, "s%dm" % (size >> 20))
    if not os.path.isdir(d):
        os.mkdir(d)
        with open(SIL, "rb") as f:
            for i in range(n):
                with open(os.path.join(d, "f%03d" % i), "wb") as g: g.write(f.read(size))
    return sorted(glob.glob(os.path.join(d, "f*")))
SMALL = {"s1m": split(200, 1 << 20), "s4m": split(50, 4 << 20)}
subprocess.run(["cat", SIL], stdout=DEVNULL)

def run(args, src=None, sink=False):
    """lz4 args ; src : stdin from cat src (else none) ; sink : stdout to cat (else /dev/null)
       returns (elapsed ms, cpu ms) of lz4"""
    cat = subprocess.Popen(["cat", src], stdout=PIPE) if src else None
    t0 = time.perf_counter()
    lz = subprocess.Popen([LZ4] + args, stdin=cat.stdout if cat else DEVNULL, stdout=PIPE if sink else DEVNULL)
    if cat: cat.stdout.close()
    snk = None
    if sink:
        snk = subprocess.Popen(["cat"], stdin=lz.stdout, stdout=DEVNULL)
        lz.stdout.close()
    _, st, ru = os.wait4(lz.pid, 0)
    t1 = time.perf_counter()
    lz.returncode = os.WEXITSTATUS(st) if os.WIFEXITED(st) else 1
    for p in (cat, snk):
        if p: p.wait()
    if lz.returncode: sys.exit("error : lz4 " + " ".join(args))
    return ((t1 - t0) * 1e3, (ru.ru_utime + ru.ru_stime) * 1e3)

def bench(kind, mode, sets, argsf):
    """argsf(set, mode) -> (args, src, sink) ; rounds interleave the sets"""
    R = {s: [] for s in sets}
    for _ in range(ROUNDS):
        for s in sets:
            a, src, sink = argsf(s, mode)
            R[s].append(run(a, src, sink))
    for s in sets:
        el, c = (statistics.median(x) for x in zip(*R[s]))
        out("%-10s %-4s %-12s elapsed %7.1f ms   cpu %7.1f ms" % (kind, mode, s, el, c))

def firstbyte(args, src):
    fb, tot = [], []
    for _ in range(FBROUNDS):
        t0 = time.perf_counter()
        cat = subprocess.Popen(["cat", src], stdout=PIPE)
        lz = subprocess.Popen([LZ4] + args, stdin=cat.stdout, stdout=PIPE, bufsize=0)
        cat.stdout.close()
        lz.stdout.read(1)
        t1 = time.perf_counter()
        while lz.stdout.read(1 << 20): pass
        t2 = time.perf_counter()
        lz.wait(); cat.wait()
        fb.append((t1 - t0) * 1e3); tot.append((t2 - t0) * 1e3)
    return statistics.median(fb), statistics.median(tot)

# compressed inputs for decompression, sizes and round trip
out("== sizes (silesia) and round trip")
for s in SETS:
    t = tag(s)
    frame = os.path.join(WORK, "sil%s.lz4" % t)
    subprocess.run([LZ4, "-q", "-f"] + s.split() + [SIL, frame], check=True)
    for k, files in SMALL.items():
        d = os.path.join(WORK, k + t)
        if not os.path.isdir(d):
            os.mkdir(d)
            for f in files:
                subprocess.run([LZ4, "-q"] + s.split() + [f, os.path.join(d, os.path.basename(f) + ".lz4")], check=True)
    ok = subprocess.run("%s -q -d -c %s | cmp -s - %s" % (LZ4, shlex.quote(frame), shlex.quote(SIL)), shell=True).returncode == 0
    out("%-12s %10d bytes   %s" % (s, os.path.getsize(frame), "ok" if ok else "ROUND TRIP FAILED"))
    if not ok: sys.exit(1)

def cargs(s, mode):
    o = ["-q"] + s.split()
    if mode == "f2n": return o + ["-c", SIL], None, False
    if mode == "p2n": return o + ["-c"], SIL, False
    if mode == "p2p": return o + ["-c"], SIL, True
    return o + ["-m", "-c"] + SMALL[mode], None, False

def dargs(s, mode):
    t = tag(s)
    frame = os.path.join(WORK, "sil%s.lz4" % t)
    if mode == "f2n": return ["-q", "-d", "-c", frame], None, False
    if mode == "p2n": return ["-q", "-d", "-c"], frame, False
    if mode == "f2p": return ["-q", "-d", "-c", frame], None, True
    return ["-q", "-d", "-m", "-c"] + sorted(glob.glob(os.path.join(WORK, mode + t, "*.lz4"))), None, False

out("== compression : median of %d (f2n file->null, p2n pipe->null, p2p pipe->pipe, s1m 200 x 1 MB, s4m 50 x 4 MB)" % ROUNDS)
for l in LEVELS:
    for mode in ("f2n", "p2n", "p2p", "s1m", "s4m"):
        bench("compress", mode, [l + v for v in VARIANTS], cargs)
out("== decompression : median of %d (f2p file->pipe)" % ROUNDS)
for l in LEVELS:
    for mode in ("f2n", "p2n", "f2p", "s1m", "s4m"):
        bench("decompress", mode, [l + v for v in VARIANTS], dargs)
out("== first output byte, input from cat : median of %d" % FBROUNDS)
for s in SETS:
    fb, tot = firstbyte(["-q"] + s.split() + ["-c"], SIL)
    out("compress   %-12s first byte %7.1f ms   end %7.1f ms" % (s, fb, tot))
for s in SETS:
    fb, tot = firstbyte(["-q", "-d", "-c"], os.path.join(WORK, "sil%s.lz4" % tag(s)))
    out("decompress %-12s first byte %7.1f ms   end %7.1f ms" % (s, fb, tot))
out("== done in %.0f s ; results in %s" % (time.time() - t_start, os.path.join(WORK, "results.txt")))
