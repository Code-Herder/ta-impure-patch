#!/usr/bin/env python3
"""The whole recipe from one config file: fetch -> train -> eval -> export -> bake -> report.

    python -m unditherer.pipeline --config unditherer/configs/shipped.json --dry-run
    python -m unditherer.pipeline --config unditherer/configs/shipped.json --stages fetch
    python -m unditherer.pipeline --config unditherer/configs/shipped.json            # train, eval, export, report
    python -m unditherer.pipeline --config unditherer/configs/shipped.json --experiments full,full-mc --force
    python -m unditherer.pipeline --config unditherer/configs/shipped.json --stages bake

Experiments are named entries in the config; each becomes `runs/<name>/` with
best.pt, last.pt and log.json (the recipe it ran with).  `init` names another
experiment whose best.pt seeds a fine-tune.  A finished experiment (best.pt
present) is skipped unless --force.  Everything the pipeline does is appended to
`runs/pipeline.jsonl`, and `report` turns the runs into `runs/results.md`.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .paths import CONFIGS, MODELS, REPO, data_root

STAGES = ("fetch", "train", "eval", "export", "bake", "report")
DEFAULT_STAGES = ("train", "eval", "export", "report")
TRAIN_KEYS = {"depth", "ch", "steps", "lr", "batch", "patch", "workers", "val_every", "val_n", "val_seed",
              "split_seed", "seed", "deterministic", "mean_weight", "mean_block", "weight_decay",
              "baseline_preset", "device", "scale_min", "scale_max", "jitter_p", "sat_min", "sat_max",
              "hue_p", "vivid_p", "dither_modes", "ordered_amp_min", "ordered_amp_max", "margin"}


class Pipeline:
    def __init__(self, config, python=None, dry_run=False, force=False, parallel=False, quiet=False):
        self.cfg = config
        self.python = python or sys.executable
        self.dry, self.force, self.parallel, self.quiet = dry_run, force, parallel, quiet
        self.corpus = Path(config.get("corpus") or data_root()).expanduser()
        self.runs = Path(config.get("runs") or self.corpus / "runs").expanduser()
        self.models_dir = Path(config.get("models_dir") or MODELS).expanduser()
        self.experiments = dict(config.get("experiments", {}))
        self.dead_ends = dict(config.get("dead_ends", {}))

    # ---- helpers ----------------------------------------------------------
    def log(self, **rec):
        rec = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), **rec}
        if not self.dry:
            self.runs.mkdir(parents=True, exist_ok=True)
            with open(self.runs / "pipeline.jsonl", "a") as fh:
                fh.write(json.dumps(rec) + "\n")
        if not self.quiet:
            print("pipeline:", json.dumps(rec), flush=True)

    def run(self, argv, cwd=None):
        cmd = [str(a) for a in argv]
        if not self.quiet or self.dry:
            print(("DRY  " if self.dry else "RUN  ") + " ".join(cmd), flush=True)
        if self.dry:
            return 0
        return subprocess.run(cmd, cwd=cwd or REPO).returncode

    def experiment(self, name):
        if name in self.experiments:
            return self.experiments[name]
        if name in self.dead_ends:
            return self.dead_ends[name]
        raise KeyError(f"unknown experiment {name!r}; config has {sorted(self.experiments)} "
                       f"(dead ends: {sorted(self.dead_ends)})")

    def resolved(self, name):
        """The experiment with `common` defaults and its init chain's depth/ch folded in."""
        exp = dict(self.cfg.get("common", {}))
        chain = []
        cur = self.experiment(name)
        while cur is not None:
            chain.append(cur)
            cur = self.experiment(cur["init"]) if cur.get("init") else None
        for e in reversed(chain):
            exp.update({k: v for k, v in e.items() if k in ("depth", "ch")})
        exp.update(self.experiment(name))
        return exp

    def order(self, names):
        """Dependency levels: every init before the experiments that use it."""
        levels, done = [], set()
        pending = list(names)
        while pending:
            ready = [n for n in pending if not self.experiment(n).get("init") or self.experiment(n)["init"] in done
                     or self.experiment(n)["init"] not in pending]
            if not ready:
                raise ValueError(f"circular init among {pending}")
            levels.append(ready)
            done.update(ready)
            pending = [n for n in pending if n not in ready]
        return levels

    def train_argv(self, name):
        exp = self.resolved(name)
        argv = [self.python, "-m", "unditherer.train", "--corpus", self.corpus, "--out", self.runs / name]
        for k, v in exp.items():
            if k not in TRAIN_KEYS or v is None:
                continue
            flag = "--" + k.replace("_", "-")
            if isinstance(v, bool):
                if v:
                    argv.append(flag)
            else:
                argv += [flag, v]
        if exp.get("init"):
            argv += ["--init", self.runs / exp["init"] / "best.pt"]
        return argv

    # ---- stages -----------------------------------------------------------
    def stage_fetch(self, names=None):
        f = self.cfg.get("fetch", {})
        argv = [self.python, "-m", "unditherer.fetch_data", "--out", self.corpus]
        if f.get("mode", "manifest") == "discover":
            argv.append("--discover")
        elif f.get("manifest"):
            argv += ["--from-manifest", REPO / f["manifest"]]
        if f.get("sources"):
            argv += ["--sources", f["sources"]]
        if f.get("limit"):
            argv += ["--limit", f["limit"]]
        rc = self.run(argv)
        self.log(stage="fetch", rc=rc)
        return rc

    def stage_train(self, names=None):
        names = names or list(self.experiments)
        rc_all = 0
        for level in self.order(names):
            procs = []
            for name in level:
                best = self.runs / name / "best.pt"
                if best.exists() and not self.force:
                    if not self.quiet:
                        print(f"skip {name}: {best} exists (use --force to retrain)")
                    continue
                argv = self.train_argv(name)
                init = self.resolved(name).get("init")
                if init and not (self.runs / init / "best.pt").exists() and not self.dry:
                    print(f"error: {name} needs runs/{init}/best.pt, which is missing", file=sys.stderr)
                    rc_all = rc_all or 1
                    continue
                self.log(stage="train", experiment=name, argv=[str(a) for a in argv])
                if self.parallel and len(level) > 1 and not self.dry:
                    print("RUN  " + " ".join(str(a) for a in argv), flush=True)
                    procs.append((name, subprocess.Popen([str(a) for a in argv], cwd=REPO)))
                else:
                    rc = self.run(argv)
                    self.log(stage="train", experiment=name, rc=rc)
                    rc_all = rc_all or rc
            for name, p in procs:
                rc = p.wait()
                self.log(stage="train", experiment=name, rc=rc)
                rc_all = rc_all or rc
        return rc_all

    def stage_eval(self, names=None):
        names = names or list(self.experiments)
        ev = self.cfg.get("eval", {})
        specs = [(n, self.runs / n / "best.pt") for n in names if (self.runs / n / "best.pt").exists()]
        if self.dry:
            print(f"DRY  evaluate {[n for n, _ in specs]} on {ev.get('n', 512)} patches seed {ev.get('seed', 1234)}")
            return 0
        if not specs:
            print("eval: nothing to evaluate (no best.pt found)", file=sys.stderr)
            return 1
        from .evaluate import evaluate_models
        res = evaluate_models([str(p) for _, p in specs], self.corpus, ev.get("patch", 96), ev.get("n", 512),
                              ev.get("seed", 1234), ev.get("device", "auto"),
                              tuple(ev.get("presets", "tuned").split(",")), verbose=not self.quiet)
        out_path = self.runs / "eval.json"
        merged = json.loads(out_path.read_text()) if out_path.exists() else {}
        merged.update({k: v for k, v in res.items() if not isinstance(v, dict)})
        merged.setdefault("experiments", {})
        for n, p in specs:
            merged["experiments"][n] = {**res[str(p)], "checkpoint": str(p),
                                        "evaluated": time.strftime("%Y-%m-%d %H:%M:%S")}
        out_path.write_text(json.dumps(merged, indent=1))
        self.log(stage="eval", experiments=[n for n, _ in specs], out=str(out_path))
        return 0

    def stage_export(self, names=None):
        mapping = self.cfg.get("export", {})
        if names:
            mapping = {m: e for m, e in mapping.items() if e in names or m in names}
        if self.dry:
            for m, e in mapping.items():
                print(f"DRY  export runs/{e}/best.pt -> {self.models_dir / (m + '.pt')} + .onnx")
            return 0
        rc = 0
        for model_name, exp in mapping.items():
            try:
                info = export_model(self.runs / exp, model_name, self.models_dir, self.runs / "eval.json",
                                    recipe=self.resolved(exp), quiet=self.quiet)
                self.log(stage="export", model=model_name, experiment=exp, **{k: v for k, v in info.items() if k != "recipe"})
            except FileNotFoundError as e:
                print(f"export {model_name}: {e}", file=sys.stderr)
                rc = 1
        return rc

    def stage_bake(self, names=None):
        b = self.cfg.get("bake", {})
        shots = REPO / b.get("shots", "research/notes/assets/undither/shots")
        out = REPO / b.get("out", "research/notes/assets/undither/learned")
        model = b.get("model", "full")
        argv = [self.python, "-m", "unditherer.infer", "--model", model, "--in", shots, "--out", out]
        rc = self.run(argv)
        self.log(stage="bake", model=model, out=str(out), rc=rc)
        if rc == 0 and b.get("prep", True):
            rc = self.run([self.python, REPO / "tools" / "undither" / "prep.py", "--no-thumbs"])
            self.log(stage="bake", step="prep", rc=rc)
        if rc == 0 and b.get("wiki", True):
            rc = self.run([self.python, "build_wiki.py"], cwd=REPO / "research")
            self.log(stage="bake", step="wiki", rc=rc)
        return rc

    def stage_report(self, names=None):
        md = results_markdown(self.runs, self.experiments, self.dead_ends, self.models_dir)
        if self.dry:
            print(md)
            return 0
        (self.runs / "results.md").write_text(md)
        if not self.quiet:
            print(md)
        self.log(stage="report", out=str(self.runs / "results.md"))
        return 0


# --------------------------------------------------------------------------- export

def export_model(run_dir, model_name, models_dir, eval_json=None, recipe=None, quiet=False):
    """runs/<exp>/best.pt -> models/<name>.pt (+ provenance) and models/<name>.onnx; updates models.json."""
    import torch
    from .infer import export_onnx
    from .model import Restorer, count_params
    run_dir, models_dir = Path(run_dir), Path(models_dir)
    best = run_dir / "best.pt"
    if not best.exists():
        raise FileNotFoundError(f"{best} does not exist")
    models_dir.mkdir(parents=True, exist_ok=True)
    ck = torch.load(best, map_location="cpu", weights_only=False)
    log = json.loads((run_dir / "log.json").read_text()) if (run_dir / "log.json").exists() else {}
    ev = json.loads(Path(eval_json).read_text()) if eval_json and Path(eval_json).exists() else {}
    eval_entry = (ev.get("experiments") or {}).get(run_dir.name) or ev.get(str(best)) or {}
    m = Restorer(ck["depth"], ck["ch"])
    m.load_state_dict(ck["model"])
    prov = {"name": model_name, "source_run": run_dir.name, "trained": log.get("finished") or log.get("started"),
            "eval_psnr": eval_entry.get("psnr"), "eval_set": {k: ev.get(k) for k in ("n", "patch", "seed") if k in ev},
            "params": count_params(m)}
    ck_out = {k: v for k, v in ck.items() if k != "opt"}
    ck_out.update({k: v for k, v in prov.items() if v is not None})
    ck_out["recipe"] = {k: v for k, v in (recipe or {}).items()}
    ck_out["aug"] = ck.get("aug") or log.get("aug")
    pt_path = models_dir / f"{model_name}.pt"
    torch.save(ck_out, pt_path)
    onnx_info = export_onnx(best, models_dir / f"{model_name}.onnx",
                            meta={k: v for k, v in prov.items() if v is not None and k != "eval_set"})
    entry = {"name": model_name, "source_run": run_dir.name, "depth": ck["depth"], "ch": ck["ch"],
             "params": count_params(m), "step": ck.get("step"), "train_val_psnr": ck.get("val_psnr"),
             "eval_psnr": eval_entry.get("psnr"), "eval_set": prov["eval_set"], "trained": prov["trained"],
             "recipe": ck_out["recipe"], "aug": ck_out["aug"], "onnx_max_abs_diff": onnx_info["max_abs_diff_vs_torch"],
             "files": {pt_path.name: pt_path.stat().st_size, Path(onnx_info["onnx"]).name: onnx_info["bytes"]},
             "exported": time.strftime("%Y-%m-%d %H:%M:%S")}
    mj = models_dir / "models.json"
    models = json.loads(mj.read_text()) if mj.exists() else {}
    models[model_name] = entry
    mj.write_text(json.dumps(models, indent=1))
    if not quiet:
        print(f"export {model_name}: {pt_path.name} {pt_path.stat().st_size / 1e6:.2f} MB, "
              f"{Path(onnx_info['onnx']).name} {onnx_info['bytes'] / 1e6:.2f} MB, "
              f"onnx parity {onnx_info['max_abs_diff_vs_torch']}")
    return entry


# --------------------------------------------------------------------------- report

def results_markdown(runs, experiments, dead_ends=None, models_dir=None):
    runs = Path(runs)
    ev = json.loads((runs / "eval.json").read_text()) if (runs / "eval.json").exists() else {}
    ev_exp = ev.get("experiments", {})
    rows = ["| experiment | net | params | steps | peak lr | mean-w | init | best train-val dB | fixed-set dB | min |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    names = list(experiments) + [n for n in (dead_ends or {}) if (runs / n / "log.json").exists()]
    for n in names:
        lp = runs / n / "log.json"
        if not lp.exists():
            rows.append(f"| {n} | | | | | | | not run | | |")
            continue
        j = json.loads(lp.read_text())
        a = j.get("args", {})
        init = Path(a["init"]).parent.name if a.get("init") else ""
        fixed = ev_exp.get(n, {}).get("psnr")
        note = " (dead end)" if dead_ends and n in dead_ends else ""
        elapsed = j.get("elapsed_s") or (j["log"][-1].get("elapsed_s", 0) if j.get("log") else 0)
        rows.append(f"| {n}{note} | {a.get('depth')}x{a.get('ch')} | {j.get('params', 0):,} | {a.get('steps')} | "
                    f"{a.get('lr')} | {a.get('mean_weight', 0)} | {init} | {j.get('best_val_psnr', float('nan')):.2f} | "
                    f"{'' if fixed is None else f'{fixed:.2f}'} | {elapsed / 60:.0f} |")
    head = [f"# Results ({time.strftime('%Y-%m-%d')})", ""]
    if ev:
        base = ", ".join(f"{k.replace('bilateral_', '')} bilateral {v:.2f} dB" for k, v in ev.items()
                         if k.startswith("bilateral_"))
        head += [f"Fixed evaluation set: {ev.get('n')} patches of {ev.get('patch')}², seed {ev.get('seed')}. "
                 f"Dithered input {ev.get('input', float('nan')):.2f} dB; {base}.", ""]
    head.append("Best train-val is each run's own validation set (changes with the augmentation); "
                "fixed-set is `unditherer.evaluate`, comparable across runs.")
    head.append("")
    tail = []
    mj = Path(models_dir or MODELS) / "models.json"
    if mj.exists():
        tail += ["", "## Shipped models", "", "| model | from run | net | params | fixed-set dB | onnx parity |", "|---|---|---|---|---|---|"]
        for name, m in json.loads(mj.read_text()).items():
            e = m.get("eval_psnr")
            tail.append(f"| {name} | {m.get('source_run')} | {m['depth']}x{m['ch']} | {m['params']:,} | "
                        f"{'' if e is None else f'{e:.2f}'} | {m.get('onnx_max_abs_diff')} |")
    return "\n".join(head + rows + tail) + "\n"


# --------------------------------------------------------------------------- entry

def load_config(path):
    path = Path(path)
    if not path.exists() and (CONFIGS / f"{path}.json").exists():
        path = CONFIGS / f"{path}.json"
    cfg = json.loads(path.read_text())
    cfg["_path"] = str(path)
    return cfg


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", "-c", default="shipped", help="config JSON path, or a name in unditherer/configs/")
    ap.add_argument("--stages", default=",".join(DEFAULT_STAGES), help=f"comma list from {', '.join(STAGES)}")
    ap.add_argument("--experiments", "-e", help="comma list of experiments (default: all in the config)")
    ap.add_argument("--force", action="store_true", help="retrain experiments whose best.pt exists")
    ap.add_argument("--parallel", action="store_true", help="run independent experiments concurrently")
    ap.add_argument("--dry-run", "-n", action="store_true", help="print what would run")
    ap.add_argument("--python", help="interpreter for subprocesses (default: this one)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    cfg = load_config(args.config)
    stages = [s.strip() for s in args.stages.split(",") if s.strip()]
    bad = [s for s in stages if s not in STAGES]
    if bad:
        ap.error(f"unknown stage(s) {bad}; choose from {STAGES}")
    names = [n.strip() for n in args.experiments.split(",")] if args.experiments else None
    p = Pipeline(cfg, args.python, args.dry_run, args.force, args.parallel, args.quiet)
    if names:
        for n in names:
            p.experiment(n)          # fail early on typos
    print(f"config {cfg['_path']}: corpus {p.corpus}, runs {p.runs}, models {p.models_dir}")
    rc = 0
    for s in stages:
        rc = getattr(p, f"stage_{s}")(names)
        if rc:
            print(f"stage {s} failed (rc {rc})", file=sys.stderr)
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
