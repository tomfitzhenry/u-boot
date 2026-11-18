#!/usr/bin/env bash
# Unit and integration tests for loom/weave.sh and git-loom on U-Boot branches.
set -euo pipefail

WEAVE_SH="$(cd "$(dirname "$0")" && pwd)/weave.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export GIT_AUTHOR_NAME="Test Bot"
export GIT_AUTHOR_EMAIL="bot@example.com"
export GIT_COMMITTER_NAME="Test Bot"
export GIT_COMMITTER_EMAIL="bot@example.com"

echo "=== 1. Setting up synthetic U-Boot git repo ==="
git init -q -b main "$WORK/repo"
cd "$WORK/repo"
git config loom.changeId false

mkdir -p drivers/video drivers/phy board/test
echo "hdmi-v1" > drivers/video/dw_hdmi.c
echo "phy-v1" > drivers/phy/phy.c
echo "board-v1" > board/test/board.c
git add .
git commit -q -m "Prepare v2026.07"
git tag v2026.07

# Feature branch 1: drv/video/avi (will be merged upstream in v2026.10)
git checkout -q -b drv/video/avi v2026.07
echo "hdmi-v2-avi" >> drivers/video/dw_hdmi.c
git commit -q -a -m "video: dw-hdmi: send AVI infoframe

Signed-off-by: Test Bot <bot@example.com>"

# Feature branch 2 + stacked branch 3: drv/phy/test -> board/test/board
git checkout -q -b drv/phy/test v2026.07
echo "phy-v2" >> drivers/phy/phy.c
git commit -q -a -m "phy: test: add usb2 phy

Signed-off-by: Test Bot <bot@example.com>"

git checkout -q -b board/test/board drv/phy/test
echo "board-v2-with-phy" >> board/test/board.c
git commit -q -a -m "board: test: add test board

Signed-off-by: Test Bot <bot@example.com>"

# local-ci branch
git checkout -q -b local-ci v2026.07
mkdir -p loom
cp "$WEAVE_SH" loom/weave.sh
chmod +x loom/weave.sh
cat > loom/loom.toml << 'EOF'
base = "v2026.07"
integration_branch = "lab/integration"

[[branch]]
name = "drv/video/avi"
upstream = true

[[branch]]
name = "drv/phy/test"
upstream = true

[[branch]]
name = "board/test/board"
stacked_on = "drv/phy/test"
upstream = true

[[branch]]
name = "local-ci"
upstream = false
EOF
git add loom
git commit -q -m "ci: add loom manifest"

echo "=== 2. Testing weave.sh verify and apply ==="
./loom/weave.sh verify
./loom/weave.sh apply

git checkout -q lab/integration
grep -q "hdmi-v2-avi" drivers/video/dw_hdmi.c
grep -q "phy-v2" drivers/phy/phy.c
grep -q "board-v2-with-phy" board/test/board.c
test -f loom/loom.toml

echo "=== 3. Testing missing Signed-off-by rejection ==="
git checkout -q -b bad/no-sob v2026.07
echo "bad" >> drivers/phy/phy.c
git commit -q -a -m "phy: missing signoff"
cat > "$WORK/bad-sob.toml" << 'EOF'
base = "v2026.07"
integration_branch = "lab/integration"
[[branch]]
name = "bad/no-sob"
upstream = true
EOF
if ./loom/weave.sh verify --manifest "$WORK/bad-sob.toml" 2>/dev/null; then
  echo "FAIL: expected branch without Signed-off-by to fail verify" >&2
  exit 1
fi

echo "=== 4. Testing non-upstream file rejection on upstreamable branch ==="
git checkout -q -b bad/touches-ci v2026.07
echo "bad" > CLAUDE.md
git add CLAUDE.md
git commit -q -m "docs: accidental agent file

Signed-off-by: Test Bot <bot@example.com>"
cat > "$WORK/bad-ci.toml" << 'EOF'
base = "v2026.07"
integration_branch = "lab/integration"
[[branch]]
name = "bad/touches-ci"
upstream = true
EOF
if ./loom/weave.sh verify --manifest "$WORK/bad-ci.toml" 2>/dev/null; then
  echo "FAIL: expected upstream branch touching CLAUDE.md to fail verify" >&2
  exit 1
fi

echo "=== 5. Testing conflict detection ==="
git checkout -q -b bad/conflict v2026.07
echo "hdmi-conflicting-change" >> drivers/video/dw_hdmi.c
git commit -q -a -m "video: conflict

Signed-off-by: Test Bot <bot@example.com>"
if ./loom/weave.sh verify --manifest "$WORK/repo/loom/loom.toml" --extra-branch bad/conflict 2>/dev/null; then
  echo "FAIL: expected conflicting extra branch to fail verify" >&2
  exit 1
fi

echo "=== 6. Testing rebase with upstreamed branch auto-drop ==="
git checkout -q main
# Simulate upstream U-Boot merging drv/video/avi in v2026.10!
echo "hdmi-v2-avi" >> drivers/video/dw_hdmi.c
git commit -q -a -m "video: dw-hdmi: send AVI infoframe (merged upstream)"
echo "unrelated-upstream-change" > README
git add README
git commit -q -m "Prepare v2026.10"
git tag v2026.10

git checkout -q local-ci
./loom/weave.sh rebase v2026.10

if grep -q "drv/video/avi" loom/loom.toml; then
  echo "FAIL: expected upstreamed branch drv/video/avi to be dropped from loom.toml" >&2
  exit 1
fi
grep -q 'base = "v2026.10"' loom/loom.toml

git checkout -q lab/integration
git merge-base --is-ancestor v2026.10 lab/integration
grep -q "unrelated-upstream-change" README
grep -q "hdmi-v2-avi" drivers/video/dw_hdmi.c
grep -q "phy-v2" drivers/phy/phy.c
grep -q "board-v2-with-phy" board/test/board.c

echo "ALL U-BOOT LOOM TESTS PASSED"
