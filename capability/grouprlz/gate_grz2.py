#!/usr/bin/env python3
"""Independent binding gate for GRZ2_GROUPED.

Parses the STREAM/GROUP/END wire directly from the format spec -- it does not link the codec
or trust its own reporting -- and checks the eight binding invariants:

  1 complete exact replay
  2 encode(P) is byte-identical to the prefix of encode(P||S) through P's last complete group
  3 strict full decode vs deliberate prefix decode
  4 rejection at group-end+1 and inside a payload
  5 absolute byte zero usable as a COPY source, and same-group anchor visibility
  6 forced ADD-cap rollback with a deterministic retry
  7 repeated ring wrap with exact group and whole-stream digests
  8 a late oversized TU grows the ring, preserves prior history, then wraps exactly

usage: gate_grz2.py <grz2g binary> <workdir> <manifest>
"""
import os, struct, subprocess, sys, hashlib, tempfile, shutil

GRZ, WORK, MANIFEST = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(WORK, exist_ok=True)
MAGIC_STREAM, MAGIC_GROUP, MAGIC_END = 0x335A5247, 0x46505247, 0x444E4547
STREAM_HDR = 4 + 4 + 1 + 4 + 4 + 4 + 1 + 1 + 1 + 8 * 6
END_LEN = 4 + 8 * 4
results = []


def run(args, expect_ok=True):
    r = subprocess.run(args, capture_output=True)
    if expect_ok and r.returncode != 0:
        raise RuntimeError("cmd failed: %s\n%s" % (" ".join(args), r.stderr.decode()[:400]))
    return r


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def parse(path):
    """Return (groups, end) where groups[i] = dict(start=frame offset, end=offset past frame)."""
    d = open(path, "rb").read()
    if struct.unpack_from("<I", d, 0)[0] != MAGIC_STREAM:
        raise RuntimeError("bad stream magic")
    o = STREAM_HDR
    groups, end = [], None
    while o < len(d):
        mg = struct.unpack_from("<I", d, o)[0]
        if mg == MAGIC_END:
            end = dict(start=o, raw=struct.unpack_from("<Q", d, o + 4)[0],
                       groups=struct.unpack_from("<Q", d, o + 12)[0])
            o += END_LEN
            break
        if mg != MAGIC_GROUP:
            raise RuntimeError("bad group magic at %d" % o)
        p = o + 4
        gidx, gstart, gout, hbase, hext = struct.unpack_from("<QQQQQ", d, p); p += 40
        ntu = struct.unpack_from("<I", d, p)[0]; p += 4
        p += 8 * ntu
        p += 1 + 4                       # offmode + be[4]
        p += 8 * 4                       # rawsz
        csz = struct.unpack_from("<QQQQ", d, p); p += 8 * 4
        nlb = struct.unpack_from("<I", d, p)[0]; p += 4
        p += 9 * nlb
        p += 8                           # group digest
        p += sum(csz)
        groups.append(dict(idx=gidx, start=o, end=p, gout=gout, tu=ntu))
        o = p
    if o != len(d) and end is None:
        raise RuntimeError("trailing bytes")
    return groups, end


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("%-46s %s %s" % (name, "PASS" if ok else "FAIL", detail))


def cat_tus(n, out):
    paths = open(MANIFEST).read().split()[:n]
    with open(out, "wb") as f:
        for q in paths:
            f.write(open(q, "rb").read())
    return out


OPT = ["-K", "256", "-s", "5", "-l", "4", "-k", "1", "-b", "8", "-j", "8",
       "--gtu", "112", "--graw", "512", "--gadd", "128", "--hist", "2048"]

# ---- fixtures ------------------------------------------------------------------------
NP, NS = 224, 200
P = cat_tus(NP, WORK + "/P.ii")
PS = cat_tus(NP + NS, WORK + "/PS.ii")
for name, src, n in (("P", P, NP), ("PS", PS, NP + NS)):
    m = WORK + "/m%s.txt" % name
    open(m, "w").write("\n".join(open(MANIFEST).read().split()[:n]) + "\n")
    run([GRZ, "tu", m, WORK + "/%s.tu" % name])

run([GRZ, "enc", P, WORK + "/P.grz", "-m", "g2", "-u", WORK + "/P.tu"] + OPT)
run([GRZ, "enc", PS, WORK + "/PS.grz", "-m", "g2", "-u", WORK + "/PS.tu"] + OPT)
gP, eP = parse(WORK + "/P.grz")
gPS, ePS = parse(WORK + "/PS.grz")

# ---- 1 complete exact replay ----------------------------------------------------------
run([GRZ, "dec", WORK + "/PS.grz", WORK + "/full.ii", "-j", "1"])
check("1 complete exact replay", sha(WORK + "/full.ii") == sha(PS))

# ---- 2 suffix independence (complete wires, not a cut of one file) ---------------------
keep = len(gP) - 1                      # P's last group is closed by end-of-input
cut = gP[keep - 1]["end"]
a = open(WORK + "/P.grz", "rb").read()[:cut]
b = open(WORK + "/PS.grz", "rb").read()[:cut]
check("2 encode(P) prefix == encode(P||S) prefix", a == b,
      "through group %d (%d bytes, TU %d)" % (keep - 1, cut, sum(g["tu"] for g in gP[:keep])))

# ---- 3 strict full decode vs deliberate prefix decode ----------------------------------
open(WORK + "/cut.grz", "wb").write(open(WORK + "/PS.grz", "rb").read()[:cut])
r_full = run([GRZ, "dec", WORK + "/cut.grz", WORK + "/x.ii", "-j", "1"], expect_ok=False)
r_pre = run([GRZ, "decprefix", WORK + "/cut.grz", WORK + "/pre.ii", "-j", "1"], expect_ok=False)
exp = cat_tus(sum(g["tu"] for g in gP[:keep]), WORK + "/expect.ii")
check("3 strict dec rejects a prefix wire", r_full.returncode != 0)
check("3 decprefix accepts + replays it", r_pre.returncode == 0 and sha(WORK + "/pre.ii") == sha(exp))

# ---- 4 rejection at group-end+1 and inside a payload -----------------------------------
raw = open(WORK + "/PS.grz", "rb").read()
open(WORK + "/p1.grz", "wb").write(raw[:cut + 1])
mid = (gPS[keep - 1]["start"] + gPS[keep - 1]["end"]) // 2
open(WORK + "/pm.grz", "wb").write(raw[:mid])
check("4 decprefix rejects group-end+1",
      run([GRZ, "decprefix", WORK + "/p1.grz", WORK + "/y.ii", "-j", "1"], False).returncode != 0)
check("4 decprefix rejects mid-payload",
      run([GRZ, "decprefix", WORK + "/pm.grz", WORK + "/y.ii", "-j", "1"], False).returncode != 0)
check("4 full dec rejects both",
      run([GRZ, "dec", WORK + "/p1.grz", WORK + "/y.ii", "-j", "1"], False).returncode != 0 and
      run([GRZ, "dec", WORK + "/pm.grz", WORK + "/y.ii", "-j", "1"], False).returncode != 0)

# ---- 5 byte zero as a COPY source + same-group anchor visibility -----------------------
blk = bytes((i * 37 + (i >> 3) * 11) & 0xFF for i in range(4096))
z = WORK + "/zero.ii"
with open(z, "wb") as f:
    f.write(blk); f.write(os.urandom(1 << 20)); f.write(blk); f.write(os.urandom(1 << 16))
open(WORK + "/zero.mf", "w").write(z + "\n")
run([GRZ, "tu", WORK + "/zero.mf", WORK + "/zero.tu"])
r = run([GRZ, "enc", z, WORK + "/zero.grz", "-m", "g1", "-u", WORK + "/zero.tu",
         "-K", "64", "-s", "0", "-l", "4", "-k", "1", "-b", "8", "-j", "1"])
src0 = int(r.stderr.decode().split("src0=")[1].split()[0])
run([GRZ, "dec", WORK + "/zero.grz", WORK + "/zero.out", "-j", "1"])
check("5 byte-zero usable as a COPY source", src0 > 0, "src0=%d" % src0)
check("5 same-group repeat matched + exact",
      sha(WORK + "/zero.out") == sha(z) and len(parse(WORK + "/zero.grz")[0]) == 1)

# ---- 6 forced ADD-cap rollback + deterministic retry -----------------------------------
r = run([GRZ, "enc", P, WORK + "/add.grz", "-m", "g2", "-u", WORK + "/P.tu",
         "-K", "256", "-s", "5", "-l", "4", "-k", "1", "-b", "8", "-j", "8",
         "--gtu", "112", "--graw", "512", "--gadd", "4", "--hist", "512",
         "--retry-test", "1", "--curve", WORK + "/add.tsv"])
rf = int(r.stderr.decode().split("retry_fail=")[1].split()[0])
closed = [l.split("\t")[9].strip() for l in open(WORK + "/add.tsv").read().splitlines()[1:]]
run([GRZ, "dec", WORK + "/add.grz", WORK + "/add.out", "-j", "1"])
check("6 ADD cap fires + retry deterministic",
      rf == 0 and "add" in closed and sha(WORK + "/add.out") == sha(P),
      "retry_fail=%d add_groups=%d" % (rf, closed.count("add")))

# ---- 7 repeated ring wrap --------------------------------------------------------------
w = cat_tus(220, WORK + "/wrap.ii")
open(WORK + "/wrap.mf", "w").write("\n".join(open(MANIFEST).read().split()[:220]) + "\n")
run([GRZ, "tu", WORK + "/wrap.mf", WORK + "/wrap.tu"])
run([GRZ, "enc", w, WORK + "/wrap.grz", "-m", "g2", "-u", WORK + "/wrap.tu",
     "-K", "256", "-s", "5", "-l", "4", "-k", "1", "-b", "8", "-j", "8",
     "--gtu", "112", "--graw", "2", "--gadd", "128", "--hist", "1"])
r = run([GRZ, "dec", WORK + "/wrap.grz", WORK + "/wrap.out", "-j", "1"], expect_ok=False)
ng = len(parse(WORK + "/wrap.grz")[0])
check("7 repeated ring wrap, digests + replay",
      r.returncode == 0 and sha(WORK + "/wrap.out") == sha(w),
      "%d groups, 1 MiB history over %.0f MiB" % (ng, os.path.getsize(w) / 1048576))

# ---- 8 late oversized TU: grow after history exists, then wrap -------------------------
seed = bytes((i * 37 + (i >> 5) * 11 + (i >> 13) * 7) & 0xff for i in range(1 << 20))
late_tus = [
    seed[:700000],
    seed[:700000] + seed * 4,
    (seed[:700000] + seed * 4)[-700000:] + seed * 3,
]
late_raw = WORK + "/late-grow.ii"
late_map = WORK + "/late-grow.tu"
offsets = [0]
with open(late_raw, "wb") as f:
    for tu in late_tus:
        f.write(tu)
        offsets.append(offsets[-1] + len(tu))
with open(late_map, "wb") as f:
    for offset in offsets:
        f.write(struct.pack("<Q", offset))
late_wire = WORK + "/late-grow.grz"
r_enc = run([GRZ, "enc", late_raw, late_wire, "-m", "g2", "-u", late_map,
             "-K", "64", "-s", "0", "-t", "20", "-l", "4", "-k", "1",
             "-b", "1", "-j", "2", "--gtu", "1", "--graw", "1",
             "--gadd", "16", "--hist", "1", "--retry-test", "1"])
r_dec = run([GRZ, "dec", late_wire, WORK + "/late-grow.out", "-j", "1"])
enc_diag = r_enc.stderr.decode()
src0 = int(enc_diag.split("src0=")[1].split()[0])
oversize = int(enc_diag.split("oversize_tu=")[1].split()[0])
retry_fail = int(enc_diag.split("retry_fail=")[1].split()[0])
dec_fields = r_dec.stdout.decode().strip().splitlines()[-1].split("\t")
final_ring = int(dec_fields[4])
late_groups = parse(late_wire)[0]
check("8 late oversized grow preserves + wraps",
      sha(WORK + "/late-grow.out") == sha(late_raw) and
      len(late_groups) == 3 and late_groups[1]["gout"] > (1 << 20) and
      final_ring > (2 << 20) and src0 > 0 and oversize > 0 and retry_fail == 0,
      "%d groups, ring %.0f MiB, src0=%d, oversized=%d, retry_fail=%d" %
      (len(late_groups), final_ring / 1048576, src0, oversize, retry_fail))

n_fail = sum(1 for _, ok, _ in results if not ok)
print("\n%d/%d checks PASS" % (len(results) - n_fail, len(results)))
sys.exit(1 if n_fail else 0)
