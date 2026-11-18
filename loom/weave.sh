#!/usr/bin/env bash
# Deterministic git-loom integration and rebase manager for U-Boot feature branches.
#
# Commands:
#   loom/weave.sh verify [--manifest PATH] [--extra-branch REF]
#     Validate loom.toml, check branch hygiene (linear history, Signed-off-by,
#     no CI/Nix/agent files on upstreamable branches), and test-weave all
#     branches onto base in a temporary worktree using git-loom.
#
#   loom/weave.sh apply [--manifest PATH] [--extra-branch REF]
#     Verify and weave all branches from loom.toml into `lab/integration`
#     using git-loom.
#
#   loom/weave.sh rebase [NEW_BASE_REF] [--manifest PATH]
#     Rebase all feature branches in loom.toml onto NEW_BASE_REF (or the latest
#     fetched upstream release tag on `main`), automatically drop feature
#     branches whose commits have all been applied upstream (`git cherry`),
#     update loom.toml on `local-ci`, and re-weave `lab/integration`.
#
#   loom/weave.sh status
#     Show the current git-loom branch topology on `lab/integration`.

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
MANIFEST="${LOOM_MANIFEST:-$REPO_ROOT/loom/loom.toml}"

if ! command -v git-loom >/dev/null 2>&1; then
  echo "error: git-loom is not in PATH" >&2
  exit 1
fi

exec python3 - "$REPO_ROOT" "$MANIFEST" "$@" << 'PYEOF'
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path


def run(cmd, cwd=None, check=True, capture=True):
    res = subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if check and res.returncode != 0:
        msg = f"Command failed ({res.returncode}): {' '.join(cmd)}"
        if capture:
            if res.stdout:
                msg += f"\n--- stdout ---\n{res.stdout.strip()}"
            if res.stderr:
                msg += f"\n--- stderr ---\n{res.stderr.strip()}"
        raise RuntimeError(msg)
    return res.stdout.strip() if capture and res.stdout else ""


def ref_exists(repo, ref):
    res = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
        cwd=repo,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return res.returncode == 0


def ensure_local_branch(repo, name, remote="origin"):
    if ref_exists(repo, f"refs/heads/{name}"):
        return
    if ref_exists(repo, f"refs/remotes/{remote}/{name}"):
        run(["git", "branch", "--track", name, f"{remote}/{name}"], cwd=repo)
        return
    if ref_exists(repo, name):
        return
    raise RuntimeError(f"Required ref or branch '{name}' does not exist locally or on '{remote}'")


def load_manifest(path):
    with open(path, "rb") as f:
        data = tomllib.load(f)
    base = data.get("base")
    integration = data.get("integration_branch", "lab/integration")
    mirror = data.get("mirror_branch", "")
    upstream_url = data.get("upstream_url", "https://github.com/u-boot/u-boot.git")
    upstream_branch = data.get("upstream_branch", "main")
    branches = data.get("branch", [])
    if not base or not branches:
        raise RuntimeError(f"Invalid manifest {path}: missing 'base' or '[[branch]]' entries")
    seen = set()
    for b in branches:
        name = b.get("name")
        if not name:
            raise RuntimeError(f"Manifest entry missing 'name': {b}")
        if name in seen:
            raise RuntimeError(f"Duplicate branch '{name}' in {path}")
        seen.add(name)
    for b in branches:
        parent = b.get("stacked_on")
        if parent and parent not in seen:
            raise RuntimeError(f"Branch '{b['name']}' is stacked_on unknown branch '{parent}'")
    return {
        "base": base,
        "integration_branch": integration,
        "mirror_branch": mirror,
        "upstream_url": upstream_url,
        "upstream_branch": upstream_branch,
        "branches": branches,
    }


def topo_sort_branches(branches):
    by_name = {b["name"]: b for b in branches}
    visited = {}
    ordered = []

    def visit(name):
        state = visited.get(name, 0)
        if state == 1:
            raise RuntimeError(f"Cycle detected in stacked_on chain involving '{name}'")
        if state == 2:
            return
        visited[name] = 1
        parent = by_name[name].get("stacked_on")
        if parent:
            visit(parent)
        visited[name] = 2
        ordered.append(by_name[name])

    for b in branches:
        visit(b["name"])
    return ordered


def leaf_branches(branches):
    parents = {b["stacked_on"] for b in branches if b.get("stacked_on")}
    return [b for b in branches if b["name"] not in parents]


FORBIDDEN_UPSTREAM_PATHS = (
    ".github/",
    "loom/",
    "flake.nix",
    "flake.lock",
    "shell.nix",
    "README.md",
    "CLAUDE.md",
    "PLAN.md",
    "DESIGN.md",
)


def verify_branch_hygiene(repo, cfg):
    base = cfg["base"]
    if not ref_exists(repo, base):
        ensure_local_branch(repo, base)
    ordered = topo_sort_branches(cfg["branches"])
    for b in ordered:
        name = b["name"]
        ensure_local_branch(repo, name)
        parent = b.get("stacked_on") or base
        anc = subprocess.run(
            ["git", "merge-base", "--is-ancestor", parent, name],
            cwd=repo,
        )
        if anc.returncode != 0:
            raise RuntimeError(
                f"Branch '{name}' does not descend from its parent '{parent}'"
            )
        merges = run(["git", "rev-list", "--merges", f"{parent}..{name}"], cwd=repo)
        if merges:
            raise RuntimeError(
                f"Branch '{name}' contains merge commits in {parent}..{name}:\n{merges}"
            )
        commits_raw = run(["git", "rev-list", "--reverse", f"{parent}..{name}"], cwd=repo)
        commits = [c for c in commits_raw.splitlines() if c]
        if not commits:
            raise RuntimeError(
                f"Branch '{name}' has 0 commits on top of '{parent}' (empty branch)"
            )
        if b.get("upstream", True):
            for c in commits:
                body = run(["git", "show", "-s", "--format=%B", c], cwd=repo)
                subject = body.splitlines()[0] if body else c
                if not re.search(r"^Signed-off-by:\s+.+<.+>", body, re.MULTILINE):
                    raise RuntimeError(
                        f"Upstreamable branch '{name}' commit {c[:12]} ('{subject}') "
                        f"is missing a 'Signed-off-by:' trailer"
                    )
                files = run(
                    ["git", "diff-tree", "--no-commit-id", "--name-only", "-r", c],
                    cwd=repo,
                ).splitlines()
                for path in files:
                    if any(
                        path == fp or path.startswith(fp)
                        for fp in FORBIDDEN_UPSTREAM_PATHS
                    ):
                        raise RuntimeError(
                            f"Upstreamable branch '{name}' commit {c[:12]} touches "
                            f"non-upstream file '{path}'"
                        )
    print(f"Verified hygiene of {len(ordered)} branches on base '{base}'.")


def weave_in_worktree(repo, cfg, target_branch=None, extra_branches=None):
    base = cfg["base"]
    leaves = leaf_branches(cfg["branches"])
    run(["git", "config", "loom.changeId", "false"], cwd=repo)
    run(["git", "branch", "-f", "_loom_base", base], cwd=repo)

    tmp_dir = tempfile.mkdtemp(prefix="git-loom-weave-")
    tmp_branch = f"_loom_tmp_{os.getpid()}"
    try:
        run(["git", "worktree", "add", "-B", tmp_branch, tmp_dir, "_loom_base"], cwd=repo)
        run(["git", "config", f"branch.{tmp_branch}.remote", "."], cwd=tmp_dir)
        run(["git", "config", f"branch.{tmp_branch}.merge", "refs/heads/_loom_base"], cwd=tmp_dir)
        for b in leaves:
            name = b["name"]
            print(f"  weaving {name} ...")
            run(["git-loom", "merge", name], cwd=tmp_dir, capture=True)
        for eb in extra_branches or []:
            print(f"  weaving extra branch {eb} ...")
            run(["git-loom", "merge", eb], cwd=tmp_dir, capture=True)

        status_out = run(["git-loom", "status", "--all"], cwd=tmp_dir)
        woven_sha = run(["git", "rev-parse", "HEAD"], cwd=tmp_dir)

        if target_branch:
            current_branch = run(
                ["git", "symbolic-ref", "--quiet", "--short", "HEAD"],
                cwd=repo,
                check=False,
            )
            if current_branch == target_branch:
                run(["git", "reset", "--hard", woven_sha], cwd=repo)
            else:
                run(["git", "branch", "-f", target_branch, woven_sha], cwd=repo)
            run(["git", "config", f"branch.{target_branch}.remote", "."], cwd=repo)
            run(["git", "config", f"branch.{target_branch}.merge", "refs/heads/_loom_base"], cwd=repo)
            mirror = cfg.get("mirror_branch")
            if mirror:
                if current_branch == mirror:
                    run(["git", "reset", "--hard", woven_sha], cwd=repo)
                else:
                    run(["git", "branch", "-f", mirror, woven_sha], cwd=repo)
                run(["git", "config", f"branch.{mirror}.remote", "."], cwd=repo)
                run(["git", "config", f"branch.{mirror}.merge", "refs/heads/_loom_base"], cwd=repo)
        return woven_sha, status_out
    finally:
        subprocess.run(
            ["git", "worktree", "remove", "--force", tmp_dir],
            cwd=repo,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            ["git", "branch", "-D", tmp_branch],
            cwd=repo,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        shutil.rmtree(tmp_dir, ignore_errors=True)


def update_manifest_text(manifest_path, new_base, dropped_branches, reparent_map):
    text = Path(manifest_path).read_text()
    text = re.sub(
        r'^base\s*=\s*"[^"]+"',
        f'base = "{new_base}"',
        text,
        count=1,
        flags=re.MULTILINE,
    )
    if dropped_branches:
        blocks = re.split(r"(?=^\[\[branch\]\])", text, flags=re.MULTILINE)
        kept = []
        for blk in blocks:
            m = re.search(r'^name\s*=\s*"([^"]+)"', blk, flags=re.MULTILINE)
            if m and m.group(1) in dropped_branches:
                continue
            for old_p, new_p in reparent_map.items():
                if new_p:
                    blk = re.sub(
                        rf'^stacked_on\s*=\s*"{re.escape(old_p)}"',
                        f'stacked_on = "{new_p}"',
                        blk,
                        flags=re.MULTILINE,
                    )
                else:
                    blk = re.sub(
                        rf'^stacked_on\s*=\s*"{re.escape(old_p)}"\n',
                        "",
                        blk,
                        flags=re.MULTILINE,
                    )
            kept.append(blk)
        text = "".join(kept)
    Path(manifest_path).write_text(text)


def cmd_rebase(repo, manifest_path, cfg, new_base):
    old_base = cfg["base"]
    if not new_base:
        upstream_url = cfg["upstream_url"]
        upstream_branch = cfg["upstream_branch"]
        print(f"Fetching latest tags from {upstream_url} ({upstream_branch}) ...")
        run(["git", "fetch", "--tags", upstream_url, upstream_branch], cwd=repo)
        new_base = run(["git", "describe", "--tags", "--abbrev=0", "FETCH_HEAD"], cwd=repo)
    if not ref_exists(repo, new_base):
        raise RuntimeError(f"Target base ref '{new_base}' does not exist")

    old_base_sha = run(["git", "rev-parse", f"{old_base}^{{commit}}"], cwd=repo)
    new_base_sha = run(["git", "rev-parse", f"{new_base}^{{commit}}"], cwd=repo)
    print(f"Rebasing loom branches from {old_base} ({old_base_sha[:12]}) -> {new_base} ({new_base_sha[:12]})")

    ordered = topo_sort_branches(cfg["branches"])
    old_tips = {}
    for b in ordered:
        ensure_local_branch(repo, b["name"])
        old_tips[b["name"]] = run(["git", "rev-parse", b["name"]], cwd=repo)

    dropped = set()
    reparent_map = {}

    current_repo_branch = run(
        ["git", "symbolic-ref", "--quiet", "--short", "HEAD"],
        cwd=repo,
        check=False,
    )
    tmp_dir = tempfile.mkdtemp(prefix="git-loom-rebase-")
    try:
        run(["git", "worktree", "add", "--detach", tmp_dir, new_base_sha], cwd=repo)
        for b in ordered:
            name = b["name"]
            orig_parent_name = b.get("stacked_on")
            old_parent_sha = old_tips[orig_parent_name] if orig_parent_name else old_base_sha
            cur_parent_name = orig_parent_name
            while cur_parent_name in dropped:
                cur_parent_name = reparent_map.get(cur_parent_name)
            new_parent_ref = cur_parent_name if cur_parent_name else new_base_sha

            cherry_out = run(
                ["git", "cherry", new_parent_ref, old_tips[name], old_parent_sha],
                cwd=tmp_dir,
            )
            cherry_lines = [ln for ln in cherry_out.splitlines() if ln.strip()]
            plus_commits = [ln for ln in cherry_lines if ln.startswith("+ ")]

            if not plus_commits and name != "local-ci":
                print(f"  [drop] {name}: all commits already present in {new_parent_ref}")
                dropped.add(name)
                reparent_map[name] = cur_parent_name
                continue

            print(f"  [rebase] {name} onto {cur_parent_name or new_base} ...")
            run(["git", "checkout", "--detach", old_tips[name]], cwd=tmp_dir)
            run(
                ["git", "rebase", "--onto", new_parent_ref, old_parent_sha, "HEAD"],
                cwd=tmp_dir,
            )
            rebased_sha = run(["git", "rev-parse", "HEAD"], cwd=tmp_dir)
            if current_repo_branch == name:
                run(["git", "reset", "--hard", rebased_sha], cwd=repo)
            else:
                run(["git", "branch", "-f", name, rebased_sha], cwd=repo)

        if ref_exists(repo, "refs/heads/local-ci"):
            local_ci_sha = run(["git", "rev-parse", "refs/heads/local-ci"], cwd=repo)
            run(["git", "checkout", "--detach", local_ci_sha], cwd=tmp_dir)
            wt_manifest = Path(tmp_dir) / "loom" / "loom.toml"
            if wt_manifest.exists():
                update_manifest_text(wt_manifest, new_base, dropped, reparent_map)
                diff = run(["git", "status", "--porcelain", "loom/loom.toml"], cwd=tmp_dir)
                if diff:
                    run(["git", "add", "loom/loom.toml"], cwd=tmp_dir)
                    msg = f"loom: rebase manifest onto {new_base}"
                    if dropped:
                        msg += f"\n\nDropped upstreamed branches: {', '.join(sorted(dropped))}"
                    run(["git", "commit", "-m", msg], cwd=tmp_dir)
                    new_local_ci_sha = run(["git", "rev-parse", "HEAD"], cwd=tmp_dir)
                    if current_repo_branch == "local-ci":
                        run(["git", "reset", "--hard", new_local_ci_sha], cwd=repo)
                    else:
                        run(["git", "branch", "-f", "local-ci", new_local_ci_sha], cwd=repo)
                    if Path(manifest_path).exists() and current_repo_branch != "local-ci":
                        shutil.copyfile(wt_manifest, manifest_path)
    finally:
        subprocess.run(
            ["git", "worktree", "remove", "--force", tmp_dir],
            cwd=repo,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        shutil.rmtree(tmp_dir, ignore_errors=True)

    local_ci_manifest = run(["git", "show", "local-ci:loom/loom.toml"], cwd=repo)
    with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as tf:
        tf.write(local_ci_manifest)
        tf_path = tf.name
    try:
        new_cfg = load_manifest(tf_path)
        verify_branch_hygiene(repo, new_cfg)
        woven_sha, status_out = weave_in_worktree(
            repo, new_cfg, target_branch=new_cfg["integration_branch"]
        )
        print(f"Woven {new_cfg['integration_branch']} at {woven_sha[:12]}:\n{status_out}")
    finally:
        os.unlink(tf_path)


def main():
    repo = sys.argv[1]
    manifest_path = sys.argv[2]
    args = sys.argv[3:]
    if not args:
        print("usage: loom/weave.sh <verify|apply|rebase|status> [options]", file=sys.stderr)
        sys.exit(2)

    cmd = args[0]
    rest = args[1:]
    extra_branches = []
    pos = []
    i = 0
    while i < len(rest):
        if rest[i] == "--manifest":
            manifest_path = rest[i + 1]
            i += 2
        elif rest[i] == "--extra-branch":
            extra_branches.append(rest[i + 1])
            i += 2
        else:
            pos.append(rest[i])
            i += 1

    cfg = load_manifest(manifest_path)

    if cmd == "verify":
        verify_branch_hygiene(repo, cfg)
        woven_sha, status_out = weave_in_worktree(
            repo, cfg, target_branch=None, extra_branches=extra_branches
        )
        print(f"Dry-run git-loom weave succeeded ({woven_sha[:12]}):\n{status_out}")
    elif cmd == "apply":
        verify_branch_hygiene(repo, cfg)
        woven_sha, status_out = weave_in_worktree(
            repo,
            cfg,
            target_branch=cfg["integration_branch"],
            extra_branches=extra_branches,
        )
        print(
            f"Updated {cfg['integration_branch']} "
            f"(and {cfg.get('mirror_branch', '')}) -> {woven_sha[:12]}:\n{status_out}"
        )
    elif cmd == "rebase":
        new_base = pos[0] if pos else ""
        cmd_rebase(repo, manifest_path, cfg, new_base)
    elif cmd == "status":
        out = run(["git-loom", "status", "--all"], cwd=repo)
        print(out)
    else:
        raise RuntimeError(f"Unknown subcommand '{cmd}'")


if __name__ == "__main__":
    main()
PYEOF
