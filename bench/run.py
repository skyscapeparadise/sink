#!/usr/bin/env python3
"""sink's benchmark runner: builds, runs, checks, and does the statistics.

    python3 bench/run.py run                 # full suite -> bench/results/<date>-<commit>.json + .md
    python3 bench/run.py run --quick         # fewer samples, for a sanity check (not for publishing)
    python3 bench/run.py compare A.json B.json
    python3 bench/run.py ab HEAD WORKTREE    # interleaved A/B between two source trees
    python3 bench/run.py aa                  # A/A control: is this machine quiet enough to trust?
    python3 bench/run.py claims [results.json]   # re-check bench/claims.json
    python3 bench/run.py bless [results.json]    # accept new output fingerprints
    python3 bench/run.py compare-terms bench/results/term-*.json

What makes a number from here one you can stand behind:

  Inputs are pinned. Workload files must match bench/workloads.sha256.

  Binaries match their source. The suite builds into build-bench/ and
  rebuilds from clean whenever the *content* of the sources changes. make
  decides by mtime, and a restored file with an old mtime has fooled it here
  before (a benchmark that contradicted arithmetic turned out to be a stale
  binary).

  Output is checked, not just timed. Each metric fingerprints what it
  produced; a fingerprint that differs from bench/expected.json fails the
  run. Faster-because-it-did-less is caught, not reported. Cross-checks: the
  pty path must leave the same grid as parsing the same bytes in memory, and
  vte_bench must end on the same screen as sink's parser for a comparison
  with it to count as like-for-like.

  The machine is recorded and gated. Power source, Low Power Mode, load,
  thermal state and the busiest processes go in every results file; a run on
  battery or under load is refused unless --allow-noisy, and marked if so.

  Statistics, not anecdotes. Every raw sample is saved. Summaries are medians
  with a 95% bootstrap confidence interval (seeded, so re-running the
  analysis on the same file gives the same interval). A difference is only
  called a difference when the interval for the ratio excludes "no change"
  and the change is at least 1%; otherwise it's reported as none detected.
  Comparisons that matter should be interleaved A/B runs (`ab`), which share
  thermal and background conditions; the `aa` control shows what this
  machine's noise floor actually is.

  Claims are executable. bench/claims.json states what sink's README is
  allowed to say, each as a threshold on a metric. `claims` re-checks them
  against the conservative end of the confidence interval and fails loudly.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RESULTS = os.path.join(HERE, "results")
EXPECTED = os.path.join(HERE, "expected.json")
CLAIMS = os.path.join(HERE, "claims.json")
WORKLOADS = os.path.join(HERE, "workloads")
BUILD = os.path.join(ROOT, "build-bench")
AB_ROOT = os.path.join(ROOT, "build-ab")
TARGETS = ["sink_bench", "pty_bench", "render_bench"]

# Samples per metric. Quick runs are for checking things work, not for
# publishing; results files say which they were.
FULL = {"sink_bench": 20, "pty_bench": 10, "render_bench": 15, "vte_bench": 20}
QUICK = {"sink_bench": 5, "pty_bench": 3, "render_bench": 4, "vte_bench": 5}

MIN_EFFECT = 0.01  # differences under 1% are not reported as differences
MIN_N = 8          # fewer samples a side than this and no verdict is given
BOOT = 2000        # bootstrap resamples


# --- small helpers -----------------------------------------------------------

def sh(cmd, cwd=ROOT, check=True, capture=True):
    r = subprocess.run(cmd, cwd=cwd, text=True, capture_output=capture)
    if check and r.returncode != 0:
        out = (r.stdout or "") + (r.stderr or "")
        raise SystemExit(f"command failed ({r.returncode}): {' '.join(cmd)}\n{out[-4000:]}")
    return r


def out(cmd, cwd=ROOT):
    try:
        return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True).stdout.strip()
    except OSError:
        return ""


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def mad(xs):
    m = median(xs)
    return median([abs(x - m) for x in xs])


def boot_median_ci(xs, seed):
    """95% percentile-bootstrap interval for the median."""
    if len(xs) < 3:
        return (min(xs), max(xs)) if xs else (float("nan"), float("nan"))
    rng = random.Random(seed)
    n = len(xs)
    meds = sorted(median([xs[rng.randrange(n)] for _ in range(n)]) for _ in range(BOOT))
    return meds[int(0.025 * BOOT)], meds[int(0.975 * BOOT) - 1]


def boot_ratio_ci(a, b, seed):
    """95% bootstrap interval for median(b) / median(a), resampling each side."""
    rng = random.Random(seed)
    na, nb = len(a), len(b)
    rs = []
    for _ in range(BOOT):
        ma = median([a[rng.randrange(na)] for _ in range(na)])
        mb = median([b[rng.randrange(nb)] for _ in range(nb)])
        rs.append(mb / ma if ma else float("nan"))
    rs.sort()
    return rs[int(0.025 * BOOT)], rs[int(0.975 * BOOT) - 1]


# Two-sided 95% critical values of Student's t, by degrees of freedom.
T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262,
        10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110,
        18: 2.101, 19: 2.093, 20: 2.086, 25: 2.060, 30: 2.042}


def t975(df):
    return T975.get(df) or T975[max(k for k in T975 if k <= df)]


def paired_ratio_ci(ratios):
    """95% t-interval for the geometric-mean ratio, from one ratio per round.

    Samples within a round share that round's conditions (thermal state,
    whatever else the machine was doing), so treating them as independent
    understates the noise -- the A/A control showed intervals that excluded
    1.0 between identical binaries when it did. One paired observation per
    round, and the spread between rounds, is the honest unit."""
    import math
    logs = [math.log(r) for r in ratios if r > 0]
    n = len(logs)
    if n < 2:
        return (float("nan"), float("nan")), float("nan")
    mean = sum(logs) / n
    sd = (sum((x - mean) ** 2 for x in logs) / (n - 1)) ** 0.5
    half = t975(n - 1) * sd / n ** 0.5
    return (math.exp(mean - half), math.exp(mean + half)), math.exp(mean)


def seed_for(metric_id):
    return int(hashlib.sha256(metric_id.encode()).hexdigest()[:8], 16)


def summarize(m):
    xs = m["samples"]
    lo, hi = boot_median_ci(xs, seed_for(m["id"]))
    med = median(xs)
    return {"n": len(xs), "median": med, "ci95": [lo, hi], "min": min(xs) if xs else None,
            "max": max(xs) if xs else None, "mad": mad(xs) if xs else None,
            "rel_ci": (hi - lo) / med if med else None}


def verdict(ratio_ci, better, floor=MIN_EFFECT):
    """Compare B against A. ratio = median(B)/median(A). `floor` is the
    smallest relative change that counts: MIN_EFFECT, or for an A/B, what
    two builds of the same source were seen to differ by (see cmd_ab)."""
    lo, hi = ratio_ci
    floor = max(floor, MIN_EFFECT)
    if lo > 1 + floor or hi < 1 / (1 + floor):
        up = lo > 1
        good = up if better == "higher" else not up
        return "better" if good else "worse"
    return "no detectable difference"


def fmt(x, unit=""):
    if x is None or x != x:
        return "-"
    a = abs(x)
    s = f"{x:.0f}" if a >= 1000 else f"{x:.1f}" if a >= 100 else f"{x:.2f}" if a >= 1 else f"{x:.3f}"
    return s + (" " + unit if unit else "")


# --- inputs and build ----------------------------------------------------------

def check_workloads():
    r = sh([sys.executable, os.path.join(HERE, "gen_workloads.py"), "--check"], check=False)
    if r.returncode != 0:
        print(r.stdout)
        raise SystemExit("workloads don't match bench/workloads.sha256. "
                         "Run python3 bench/gen_workloads.py, and if it still fails, the generator changed.")


def source_digest(src_root):
    """Hash of everything the bench binaries are built from, by content."""
    h = hashlib.sha256()
    paths = []
    for top in ["src", "bench", "cmake", "shell-integration"]:
        base = os.path.join(src_root, top)
        for dirpath, dirnames, filenames in os.walk(base, followlinks=False):
            dirnames[:] = sorted(d for d in dirnames if d not in ("results", "workloads", "target", "__pycache__"))
            for f in sorted(filenames):
                if f.endswith((".cpp", ".hpp", ".h", ".mm", ".c", ".frag", ".cmake", ".zsh", ".bash", ".zshenv",
                               ".inc", ".rs", ".toml")) or f == ".zshenv":
                    paths.append(os.path.join(dirpath, f))
    paths.append(os.path.join(src_root, "CMakeLists.txt"))
    for p in sorted(paths):
        if os.path.isfile(p):
            h.update(os.path.relpath(p, src_root).encode() + b"\0")
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def build(src_root, build_dir, log=print):
    """Configures (Release) and builds the bench targets, from clean whenever
    the sources' content differs from what was last built here."""
    digest = source_digest(src_root)
    stamp = os.path.join(build_dir, ".bench-source-digest")
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        log(f"configuring {os.path.relpath(build_dir, ROOT)} (Release)")
        sh(["cmake", "-S", src_root, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"])
    cache = open(os.path.join(build_dir, "CMakeCache.txt")).read()
    bt = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.M)
    if not bt or bt.group(1) != "Release":
        raise SystemExit(f"{build_dir} is not a Release build; benchmarks of a debug build mean nothing")
    clean = not os.path.exists(stamp) or open(stamp).read().strip() != digest
    log(f"building {', '.join(TARGETS)}" + (" from clean (sources changed)" if clean else ""))
    cmd = ["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4), "--target", *TARGETS]
    if clean:
        cmd.insert(3, "--clean-first")
    sh(cmd)
    with open(stamp, "w") as f:
        f.write(digest)
    flags = ""
    fm = os.path.join(build_dir, "CMakeFiles", "sink_bench.dir", "flags.make")
    if os.path.exists(fm):
        m = re.search(r"^CXX_FLAGS = (.*)$", open(fm).read(), re.M)
        flags = m.group(1).strip() if m else ""
    return {"source_digest": digest, "build_type": "Release", "cxx_flags": flags,
            "compiler": out(["c++", "--version"]).splitlines()[0] if out(["c++", "--version"]) else ""}


def build_vte(log=print):
    if not shutil.which("cargo"):
        log("cargo not found: skipping vte_bench")
        return None
    log("building vte_bench (cargo --release)")
    sh(["cargo", "build", "--release", "--quiet"], cwd=os.path.join(HERE, "vte_bench"))
    return os.path.join(HERE, "vte_bench", "target", "release", "vte_bench")


# --- environment -----------------------------------------------------------------

def environment():
    env = {
        "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "host_cpu": out(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "cores": os.cpu_count(),
        "perf_cores": out(["sysctl", "-n", "hw.perflevel0.physicalcpu"]),
        "memory_gb": round(int(out(["sysctl", "-n", "hw.memsize"]) or 0) / 2**30, 1),
        "os": f"macOS {platform.mac_ver()[0]} ({out(['sw_vers', '-buildVersion'])})",
        "model": out(["sysctl", "-n", "hw.model"]),
    }
    batt = out(["pmset", "-g", "batt"])
    env["power"] = "AC" if "AC Power" in batt else "battery" if "Battery Power" in batt else "unknown"
    pm = out(["pmset", "-g"])
    m = re.search(r"lowpowermode\s+(\d)", pm)
    env["low_power_mode"] = bool(m and m.group(1) == "1")
    env["thermal"] = " ".join(out(["pmset", "-g", "therm"]).split())
    env["load_avg"] = [round(x, 2) for x in os.getloadavg()]
    procs = out(["ps", "-Ao", "pcpu=,comm=", "-r"]).splitlines()[:6]
    env["busiest_processes"] = [" ".join(p.split()) for p in procs]
    env["git_commit"] = out(["git", "rev-parse", "HEAD"])
    status = out(["git", "status", "--porcelain", "--untracked-files=no"])
    env["git_dirty"] = bool(status)
    if status:
        env["git_diff_sha256"] = hashlib.sha256(out(["git", "diff", "HEAD"]).encode()).hexdigest()
    return env


def noise_problems(env):
    probs = []
    if env["power"] == "battery":
        probs.append("on battery power")
    if env["low_power_mode"]:
        probs.append("Low Power Mode is on")
    if env["load_avg"][0] > 3.0:
        probs.append(f"1-minute load average is {env['load_avg'][0]}")
    if "CPU_Speed_Limit" in env["thermal"] and "CPU_Speed_Limit = 100" not in env["thermal"]:
        probs.append(f"thermal throttling ({env['thermal']})")
    return probs


# --- running ----------------------------------------------------------------------

def run_bench(binary, args, samples, log=print):
    name = os.path.basename(binary)
    log(f"running {name}")
    r = subprocess.run([binary, "--json", "--samples", str(samples), *args], cwd=ROOT,
                       text=True, capture_output=True)
    try:
        doc = json.loads(r.stdout)
    except json.JSONDecodeError:
        raise SystemExit(f"{name} did not produce JSON (exit {r.returncode}):\n{r.stderr[-3000:]}")
    doc["exit_code"] = r.returncode
    doc["stderr"] = r.stderr[-3000:]
    return doc


def run_suite(build_dir, which, samples, vte=None, snapshots=None, only=None, log=print):
    docs = []
    extra = ["--only", only] if only else []
    if "parser" in which:
        docs.append(run_bench(os.path.join(build_dir, "sink_bench"), extra + [WORKLOADS], samples["sink_bench"], log))
    if "pty" in which:
        docs.append(run_bench(os.path.join(build_dir, "pty_bench"), extra + [WORKLOADS], samples["pty_bench"], log))
    if "render" in which:
        snap = ["--snapshots", snapshots] if snapshots else []
        docs.append(run_bench(os.path.join(build_dir, "render_bench"), extra + snap + [WORKLOADS],
                              samples["render_bench"], log))
    if "vte" in which and vte:
        docs.append(run_bench(vte, [WORKLOADS], samples["vte_bench"], log))
    return docs


def all_metrics(docs):
    return {m["id"]: m for d in docs for m in d["metrics"]}


def check(docs, expected):
    """Fingerprints against the blessed ones, and the cross-checks."""
    checks = []
    for d in docs:
        if d.get("exit_code"):
            checks.append({"level": "fail", "what": f"{d['bench']} exited {d['exit_code']}",
                           "detail": d.get("stderr", "").strip()[-500:]})
    ms = all_metrics(docs)
    exp = expected.get("fingerprints", {})
    exp_px = expected.get("pixels", {})
    for mid, m in ms.items():
        fp = m.get("fingerprint")
        if fp is not None:
            if mid not in exp:
                checks.append({"level": "warn", "what": f"{mid}: no blessed fingerprint yet", "detail": fp})
            elif exp[mid] != fp:
                checks.append({"level": "fail", "what": f"{mid}: output changed",
                               "detail": f"expected {exp[mid]}, got {fp}. If the change in behaviour is intended, "
                                         "run `bench/run.py bless` and commit bench/expected.json with it."})
        px = m.get("info", {}).get("pixels")
        if px and px != "nondeterministic" and mid.endswith(".frame"):
            if mid in exp_px and exp_px[mid] != px:
                checks.append({"level": "warn", "what": f"{mid}: rendered pixels changed",
                               "detail": "compare the snapshot PNGs if this wasn't expected; a driver or OS "
                                         "update can also do this"})
    # The pty path must end where in-memory parsing of the same bytes does.
    for mid, m in ms.items():
        same = m.get("info", {}).get("same_as")
        if same and same in ms and m.get("fingerprint") != ms[same].get("fingerprint"):
            checks.append({"level": "fail", "what": f"{mid}: grid differs from {same}",
                           "detail": "the same bytes through the pty left a different grid than in memory"})
    # vte comparisons only count when both ended on the same screen.
    for mid, m in ms.items():
        if mid.startswith("vte."):
            core = "parser_core." + mid[4:]
            if core in ms:
                a = ms[core]["info"].get("screen_text")
                b = m["info"].get("screen_text")
                checks.append({"level": "ok" if a == b else "warn",
                               "what": f"{mid} vs {core}: " + ("same final screen" if a == b else
                                       "different final screens -- not a like-for-like comparison"),
                               "detail": f"{a} / {b}"})
    return checks


def load_json(path, default=None):
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return default if default is not None else {}


def results_name(env, quick):
    date = env["date"][:10]
    sha = env["git_commit"][:7] or "nogit"
    return f"{date}-{sha}{'-dirty' if env['git_dirty'] else ''}{'-quick' if quick else ''}"


def write_report(res, path_md):
    env = res["environment"]
    lines = [f"# sink benchmark results: {os.path.basename(path_md)[:-3]}", ""]
    lines.append(f"- Machine: {env['host_cpu']} ({env['model']}), {env['cores']} cores, {env['memory_gb']} GB, {env['os']}")
    lines.append(f"- Commit: `{env['git_commit'][:12]}`" + (" with uncommitted changes" if env["git_dirty"] else ""))
    lines.append(f"- Power: {env['power']}, Low Power Mode {'on' if env['low_power_mode'] else 'off'}, "
                 f"load {env['load_avg']}")
    lines.append(f"- Build: {res['build'].get('compiler', '')}; `{res['build'].get('cxx_flags', '')}`")
    if res.get("quick"):
        lines.append("- **Quick run: too few samples to publish from.**")
    if res.get("noise"):
        lines.append(f"- **Noisy machine:** {', '.join(res['noise'])}")
    lines.append("")
    fails = [c for c in res["checks"] if c["level"] == "fail"]
    warns = [c for c in res["checks"] if c["level"] == "warn"]
    lines.append(f"Checks: {len(fails)} failed, {len(warns)} warnings, "
                 f"{len([c for c in res['checks'] if c['level'] == 'ok'])} ok.")
    for c in fails + warns:
        lines.append(f"- {c['level'].upper()}: {c['what']}")
    lines.append("")
    groups = {}
    for d in res["benches"]:
        for m in d["metrics"]:
            groups.setdefault(d["bench"], []).append(m)
    for bench, ms in groups.items():
        lines.append(f"## {bench}")
        lines.append("")
        cfg = next(d["config"] for d in res["benches"] if d["bench"] == bench)
        lines.append("Config: " + ", ".join(f"{k}={v}" for k, v in cfg.items()))
        lines.append("")
        lines.append("| metric | median | 95% CI | n | unit |")
        lines.append("|---|---:|---:|---:|---|")
        for m in ms:
            s = res["stats"][m["id"]]
            lines.append(f"| {m['label']} | {fmt(s['median'])} | {fmt(s['ci95'][0])} - {fmt(s['ci95'][1])} "
                         f"| {s['n']} | {m['unit']} |")
        lines.append("")
    with open(path_md, "w") as f:
        f.write("\n".join(lines) + "\n")


def print_summary(res):
    print()
    print(f"{'metric':<44} {'median':>10} {'95% CI':>21}  unit")
    for d in res["benches"]:
        for m in d["metrics"]:
            s = res["stats"][m["id"]]
            ci = f"{fmt(s['ci95'][0])} - {fmt(s['ci95'][1])}"
            print(f"{m['id']:<44} {fmt(s['median']):>10} {ci:>21}  {m['unit']}")
    print()
    for c in res["checks"]:
        if c["level"] != "ok":
            print(f"{c['level'].upper():5} {c['what']}")
    nf = len([c for c in res["checks"] if c["level"] == "fail"])
    print(f"\n{nf} check(s) failed." if nf else "\nAll output checks passed.")


def cmd_run(args):
    check_workloads()
    env = environment()
    noise = noise_problems(env)
    if noise and not args.allow_noisy:
        raise SystemExit("Not a quiet machine: " + "; ".join(noise) +
                         ". Fix that, or pass --allow-noisy (the results will be marked).")
    binfo = build(ROOT, BUILD)
    which = args.bench.split(",")
    vte = build_vte() if "vte" in which else None
    samples = QUICK if args.quick else FULL
    name = args.name or results_name(env, args.quick)
    os.makedirs(RESULTS, exist_ok=True)
    snapshots = os.path.join(RESULTS, name + "-frames") if "render" in which else None
    if snapshots:
        os.makedirs(snapshots, exist_ok=True)
    docs = run_suite(BUILD, which, samples, vte=vte, snapshots=snapshots, only=args.only)
    env_after = {"load_avg_after": [round(x, 2) for x in os.getloadavg()]}
    res = {"suite": 1, "name": name, "quick": args.quick, "environment": {**env, **env_after}, "build": binfo,
           "noise": noise, "benches": docs,
           "stats": {m["id"]: summarize(m) for d in docs for m in d["metrics"]}}
    res["checks"] = check(docs, load_json(EXPECTED))
    path = os.path.join(RESULTS, name + ".json")
    with open(path, "w") as f:
        json.dump(res, f, indent=1)
    write_report(res, path[:-5] + ".md")
    print_summary(res)
    print(f"wrote {os.path.relpath(path, ROOT)} and .md" + (f", frames in {os.path.relpath(snapshots, ROOT)}/" if snapshots else ""))
    claims_ok = evaluate_claims(res, quiet=False) if os.path.exists(CLAIMS) and not args.only else True
    failed = any(c["level"] == "fail" for c in res["checks"])
    sys.exit(1 if failed or not claims_ok else 0)


# --- comparing -----------------------------------------------------------------------

def compare_metric_sets(a_ms, b_ms, a_label="A", b_label="B"):
    rows = []
    for mid in a_ms:
        if mid not in b_ms:
            continue
        a, b = a_ms[mid], b_ms[mid]
        lo, hi = boot_ratio_ci(a["samples"], b["samples"], seed_for(mid))
        ratio = median(b["samples"]) / median(a["samples"]) if median(a["samples"]) else float("nan")
        same_work = a.get("fingerprint") == b.get("fingerprint")
        rows.append({"id": mid, "unit": a["unit"], "better": a["better"], "a": median(a["samples"]),
                     "b": median(b["samples"]), "ratio": ratio, "ci": [lo, hi],
                     "verdict": verdict((lo, hi), a["better"]) if min(len(a["samples"]), len(b["samples"])) >= MIN_N
                     else f"too few samples (<{MIN_N} a side) to say", "same_output": same_work})
    print(f"\n{'metric':<40} {a_label:>10} {b_label:>10} {'B/A':>7} {'95% CI':>15}  verdict")
    for r in rows:
        ci = f"{r['ci'][0]:.3f}-{r['ci'][1]:.3f}"
        note = "" if r["same_output"] else "  [different output: not the same work]"
        print(f"{r['id']:<40} {fmt(r['a']):>10} {fmt(r['b']):>10} {r['ratio']:>7.3f} {ci:>15}  {r['verdict']}{note}")
    return rows


def noise_floors(env):
    """Per-metric build-to-build noise from the latest A/A' control on this
    machine: how far from 1.0 the interval reached between two builds of the
    same source. Empty if there is none."""
    if not os.path.isdir(RESULTS):
        return {}, None
    files = sorted((f for f in os.listdir(RESULTS) if f.endswith("-aa.json")),
                   key=lambda f: os.path.getmtime(os.path.join(RESULTS, f)))
    for f in reversed(files):
        doc = load_json(os.path.join(RESULTS, f))
        if doc.get("environment", {}).get("host_cpu") != env["host_cpu"] or not doc.get("separate_builds"):
            continue
        floors = {}
        for r in doc["rows"]:
            lo, hi = r["ci"]
            if lo == lo and hi == hi and lo > 0:
                floors[r["id"]] = max(hi - 1, 1 / lo - 1)
        return floors, f
    return {}, None


def compare_paired(a_ms, b_ms, a_rounds, b_rounds, a_label, b_label, floors=None):
    """Interleaved A/B: one ratio per round, judged by paired_ratio_ci."""
    floors = floors or {}
    rows = []
    for mid in a_ms:
        if mid not in b_ms or mid not in a_rounds or mid not in b_rounds:
            continue
        ra, rb = a_rounds[mid], b_rounds[mid]
        ratios = [y / x for x, y in zip(ra, rb) if x]
        (lo, hi), gm = paired_ratio_ci(ratios)
        a, b = a_ms[mid], b_ms[mid]
        enough = len(ratios) >= 4
        floor = floors.get(mid, MIN_EFFECT)
        rows.append({"id": mid, "unit": a["unit"], "better": a["better"], "a": median(a["samples"]),
                     "b": median(b["samples"]), "ratio": gm, "ci": [lo, hi], "rounds": len(ratios),
                     "floor": floor,
                     "verdict": verdict((lo, hi), a["better"], floor) if enough else "too few rounds (<4) to say",
                     "same_output": a.get("fingerprint") == b.get("fingerprint")})
    print(f"\n{'metric':<40} {a_label:>10} {b_label:>10} {'B/A':>7} {'95% CI':>15} {'floor':>6}  verdict")
    for r in rows:
        ci = f"{r['ci'][0]:.3f}-{r['ci'][1]:.3f}"
        note = "" if r["same_output"] else "  [different output: not the same work]"
        print(f"{r['id']:<40} {fmt(r['a']):>10} {fmt(r['b']):>10} {r['ratio']:>7.3f} {ci:>15} "
              f"{100 * r['floor']:>5.1f}%  {r['verdict']}{note}")
    print("\nOne paired ratio per interleaved round. 'floor' is the smallest change that counts for that metric: "
          "what two builds of the same source differed by in the latest `aa` run (at least 1%).")
    return rows


def cmd_compare(args):
    a, b = load_json(args.a), load_json(args.b)
    ea, eb = a.get("environment", {}), b.get("environment", {})
    if ea.get("host_cpu") != eb.get("host_cpu"):
        print(f"WARNING: different machines ({ea.get('host_cpu')} vs {eb.get('host_cpu')}). "
              "Ratios across machines measure the machines too.")
    print("Not interleaved: these were measured at different times, so drift between the two sessions is in the "
          "ratios. For a change you want to claim, use `bench/run.py ab`.")
    compare_metric_sets(all_metrics(a["benches"]), all_metrics(b["benches"]),
                        os.path.basename(args.a)[:10], os.path.basename(args.b)[:10])


# --- interleaved A/B ----------------------------------------------------------------

def prepare_tree(spec, label, log=print):
    """A source tree for `spec`: WORKTREE (as it is now), dir:PATH (that
    directory's src/), or a git revision (its src/). Everything but src/ --
    the benchmarks themselves, CMakeLists, tests -- is the current tree's, so
    both sides are measured by the same harness."""
    if spec == "WORKTREE":
        return ROOT
    os.makedirs(AB_ROOT, exist_ok=True)
    tree = os.path.join(AB_ROOT, label + "-src")
    if os.path.exists(tree):
        shutil.rmtree(tree)
    os.makedirs(tree)
    files = out(["git", "ls-files", "-co", "--exclude-standard"]).splitlines()
    for f in files:
        if f.startswith(("src/", "vendor/", "bench/results/", "backgrounds/", "releases/")):
            continue
        srcp = os.path.join(ROOT, f)
        if not os.path.isfile(srcp):
            continue
        dst = os.path.join(tree, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(srcp, dst)
    for link in ["vendor", "backgrounds"]:
        os.symlink(os.path.join(ROOT, link), os.path.join(tree, link))
    os.symlink(WORKLOADS, os.path.join(tree, "bench", "workloads"))
    if spec.startswith("dir:"):
        shutil.copytree(os.path.join(spec[4:], "src"), os.path.join(tree, "src"))
    else:
        rev = out(["git", "rev-parse", "--verify", spec + "^{commit}"])
        if not rev:
            raise SystemExit(f"not a revision: {spec}")
        log(f"{label}: src/ from {spec} ({rev[:10]})")
        archive = subprocess.run(["git", "archive", rev, "src"], cwd=ROOT, capture_output=True, check=True).stdout
        subprocess.run(["tar", "-x", "-C", tree], input=archive, check=True)
    return tree


def cmd_ab(args, control=False):
    check_workloads()
    env = environment()
    noise = noise_problems(env)
    if noise and not args.allow_noisy:
        raise SystemExit("Not a quiet machine: " + "; ".join(noise) + ". Pass --allow-noisy to run anyway.")
    which = args.bench.split(",")
    if control:
        # Two separate builds of the same source, in different directories,
        # exactly as a real A/B gets. Reusing one binary for both sides would
        # only measure run-to-run noise, and miss the larger effect: code
        # layout differs from build to build, and that alone moved untouched
        # metrics by ~3% in an A/B here.
        specs = [("A", "WORKTREE"), ("A2", "dir:" + ROOT)]
    else:
        specs = [("A", args.a), ("B", args.b)]
    builds = {}
    for label, spec in specs:
        tree = prepare_tree(spec, label)
        bdir = BUILD if tree == ROOT else os.path.join(AB_ROOT, label)
        build(tree, bdir)
        builds[label] = bdir
    per_round = {k: max(2, v // args.rounds) for k, v in (QUICK if args.quick else FULL).items()}
    pooled = {label: {} for label, _ in specs}
    per_round_medians = {label: {} for label, _ in specs}  # label -> id -> [median per round]
    order = [label for label, _ in specs]
    for r in range(args.rounds):
        # ABBA: alternate who goes first, so slow drift lands on both sides equally.
        seq = order if r % 2 == 0 else order[::-1]
        for label in seq:
            print(f"round {r + 1}/{args.rounds}: {label}")
            docs = run_suite(builds[label], which, per_round, only=args.only, log=lambda *_: None)
            for d in docs:
                if d.get("exit_code"):
                    print(f"  {label}: {d['bench']} exited {d['exit_code']}: {d.get('stderr', '')[-300:]}")
                for m in d["metrics"]:
                    p = pooled[label].setdefault(m["id"], {**m, "samples": []})
                    p["samples"].extend(m["samples"])
                    per_round_medians[label].setdefault(m["id"], []).append(median(m["samples"]))
                    if p.get("fingerprint") != m.get("fingerprint"):
                        p["fingerprint"] = "inconsistent"
    a, b = order
    floors, floor_file = ({}, None) if control else noise_floors(env)
    if not control:
        if floor_file:
            print(f"\nNoise floors from {floor_file}")
        else:
            print("\nNo A/A' control on file for this machine, so every metric uses a 1% floor. Build-to-build "
                  "noise is often larger than that: run `bench/run.py aa` first for verdicts you can rely on.")
    rows = compare_paired(pooled[a], pooled[b], per_round_medians[a], per_round_medians[b], a, b, floors)
    res = {"suite": 1, "kind": "aa" if control else "ab", "separate_builds": True, "a": specs[0][1], "b": specs[1][1],
           "rounds": args.rounds, "environment": env, "noise": noise, "rows": rows,
           "pooled": {k: list(v.values()) for k, v in pooled.items()}}
    os.makedirs(RESULTS, exist_ok=True)
    tag = "aa" if control else f"ab-{re.sub(r'[^A-Za-z0-9]+', '_', specs[0][1])}-vs-{re.sub(r'[^A-Za-z0-9]+', '_', specs[1][1])}"
    path = os.path.join(RESULTS, f"{env['date'][:10]}-{tag}.json")
    with open(path, "w") as f:
        json.dump(res, f, indent=1)
    print(f"\nwrote {os.path.relpath(path, ROOT)}")
    if control:
        diffs = [r for r in rows if r["verdict"] != "no detectable difference"]
        if diffs:
            print(f"\nTwo builds of the same source 'differed' on {len(diffs)} metric(s):")
            for r in diffs:
                print(f"  {r['id']}: {r['ratio']:.3f}  (95% CI {r['ci'][0]:.3f}-{r['ci'][1]:.3f})")
            print("That is code layout, not code. `ab` now treats changes smaller than these as noise for those "
                  "metrics; every metric's floor is saved in this file.")
        else:
            print("\nTwo builds of the same source showed no detectable difference on any metric.")


# --- terminals --------------------------------------------------------------------------

def cmd_compare_terms(args):
    docs = [load_json(p) for p in args.files]
    sizes = {(d["config"]["cols"], d["config"]["rows"]) for d in docs}
    if len(sizes) > 1:
        print("Window sizes differ:")
        for p, d in zip(args.files, docs):
            print(f"  {d['config']['terminal']}: {d['config']['cols']}x{d['config']['rows']}  ({p})")
        raise SystemExit("Throughput scales with cell count; resize the windows to match and re-run.")
    machines = {d.get("machine", {}).get("cpu") for d in docs}
    if len(machines) > 1:
        print(f"WARNING: results from different machines: {machines}")
    names = [d["config"]["terminal"] for d in docs]
    ids = []
    for d in docs:
        for m in d["metrics"]:
            if m["id"] not in ids:
                ids.append(m["id"])
    cols, rows = sizes.pop()
    print(f"\n{cols}x{rows} cells. Medians, with 95% bootstrap intervals.\n")
    print(f"{'':<20}" + "".join(f"{n:>26}" for n in names))
    for mid in ids:
        line = f"{mid[5:]:<20}"
        unit = ""
        for d in docs:
            m = next((m for m in d["metrics"] if m["id"] == mid), None)
            if not m:
                line += f"{'-':>26}"
                continue
            unit = m["unit"]
            lo, hi = boot_median_ci(m["samples"], seed_for(mid))
            line += f"{fmt(median(m['samples'])):>10} ({fmt(lo)}-{fmt(hi)})".rjust(26)
        print(line + f"  {unit}")
    print("\nCursor after each workload (row;col) -- terminals given the same bytes at the same size should agree:")
    for mid in ids:
        if mid == "term.query_latency":
            continue
        seen = {}
        for d in docs:
            m = next((m for m in d["metrics"] if m["id"] == mid), None)
            if m:
                seen[d["config"]["terminal"]] = ",".join(m["info"].get("cursor", []))
        word = "only one" if len(seen) < 2 else "agree" if len(set(seen.values())) == 1 else "DISAGREE"
        print(f"  {mid[5:]:<16} {word}: " + ", ".join(f"{k}={v}" for k, v in seen.items()))
    print("\nThroughput here is bytes consumed per second, not frames drawn: see bench/termbench.py.")


# --- claims and blessing --------------------------------------------------------------------

def evaluate_claims(res, quiet=False):
    claims = load_json(CLAIMS, {"claims": []})["claims"]
    stats = res["stats"]
    cpu = res["environment"]["host_cpu"]
    ok = True
    if not quiet:
        print(f"\nClaims ({os.path.relpath(CLAIMS, ROOT)}), judged on the conservative end of each 95% interval:")
    for c in claims:
        if c.get("machine") and c["machine"] not in cpu:
            if not quiet:
                print(f"  SKIP  {c['claim']}  (stated for {c['machine']}, this is {cpu})")
            continue
        if "ratio" in c:
            # median(first) / median(second), with a bootstrap interval.
            ms = all_metrics(res["benches"])
            num, den = c["ratio"]
            if num not in ms or den not in ms:
                if not quiet:
                    print(f"  ----  {c['claim']}  ({num} or {den} not in this run)")
                continue
            lo, hi = boot_ratio_ci(ms[den]["samples"], ms[num]["samples"], seed_for(num + "/" + den))
            c = {**c, "metric": f"{num} / {den}"}
        else:
            s = stats.get(c["metric"])
            if not s:
                if not quiet:
                    print(f"  ----  {c['claim']}  (metric {c['metric']} not in this run)")
                continue
            lo, hi = s["ci95"]
        if c["op"] == ">=":
            passed, edge = lo >= c["value"], lo
        else:
            passed, edge = hi <= c["value"], hi
        ok &= passed
        if not quiet:
            print(f"  {'PASS' if passed else 'FAIL'}  {c['claim']}  ({c['metric']} {c['op']} {c['value']}; "
                  f"interval edge {fmt(edge)})")
    return ok


def latest_results():
    files = sorted((f for f in os.listdir(RESULTS) if f.endswith(".json") and f[:4].isdigit()
                    and "-ab-" not in f and not f.endswith("-aa.json")),
                   key=lambda f: os.path.getmtime(os.path.join(RESULTS, f)))
    if not files:
        raise SystemExit("no results yet: run `bench/run.py run` first")
    return os.path.join(RESULTS, files[-1])


def cmd_claims(args):
    path = args.results or latest_results()
    res = load_json(path)
    print(f"results: {os.path.relpath(path, ROOT)}")
    if res.get("quick"):
        print("note: a quick run; intervals are wide, so claims are harder to pass")
    sys.exit(0 if evaluate_claims(res) else 1)


def cmd_bless(args):
    path = args.results or latest_results()
    res = load_json(path)
    if any(c["level"] == "fail" and "exited" in c["what"] for c in res["checks"]):
        raise SystemExit("that run had a benchmark exit with an error; fix it before blessing its output")
    exp = load_json(EXPECTED, {"fingerprints": {}, "pixels": {}})
    exp.setdefault("fingerprints", {})
    exp.setdefault("pixels", {})
    changed = 0
    for m in all_metrics(res["benches"]).values():
        fp = m.get("fingerprint")
        if fp is not None and exp["fingerprints"].get(m["id"]) != fp:
            print(f"  {m['id']}: {exp['fingerprints'].get(m['id'])} -> {fp}")
            exp["fingerprints"][m["id"]] = fp
            changed += 1
        px = m.get("info", {}).get("pixels")
        if px and px != "nondeterministic" and m["id"].endswith(".frame"):
            exp["pixels"][m["id"]] = px
    exp["blessed_from"] = os.path.basename(path)
    exp["machine"] = res["environment"]["host_cpu"]
    with open(EXPECTED, "w") as f:
        json.dump(exp, f, indent=1, sort_keys=True)
    print(f"{changed} fingerprint(s) updated in {os.path.relpath(EXPECTED, ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--bench", default="parser,pty,render,vte",
                       help="comma-separated: parser, pty, render, vte (default: all)")
        p.add_argument("--quick", action="store_true", help="fewer samples; for checking, not publishing")
        p.add_argument("--allow-noisy", action="store_true", help="run on battery / under load (results are marked)")
        p.add_argument("--only", help="only metrics whose id contains this")

    p = sub.add_parser("run", help="run the suite and save results")
    common(p)
    p.add_argument("--name", help="results file name (default: <date>-<commit>)")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("compare", help="compare two results files (not interleaved)")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("ab", help="interleaved A/B: WORKTREE, a git revision, or dir:PATH")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--rounds", type=int, default=6, help="interleaved rounds; each is one paired observation")
    common(p)
    p.set_defaults(fn=lambda a: cmd_ab(a))

    p = sub.add_parser("aa", help="A/A control on the working tree: measures the noise floor")
    p.add_argument("--rounds", type=int, default=6)
    common(p)
    p.set_defaults(fn=lambda a: cmd_ab(a, control=True))

    p = sub.add_parser("claims", help="check bench/claims.json against a results file")
    p.add_argument("results", nargs="?")
    p.set_defaults(fn=cmd_claims)

    p = sub.add_parser("bless", help="accept a results file's output fingerprints as expected")
    p.add_argument("results", nargs="?")
    p.set_defaults(fn=cmd_bless)

    p = sub.add_parser("compare-terms", help="compare termbench results from different terminals")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_compare_terms)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
