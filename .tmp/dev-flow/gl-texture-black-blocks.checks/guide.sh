#!/usr/bin/env bash
# u-guide 核验脚本（testCommand）：文案落地 + 渲染器选择逻辑零改动
set -u
SUPER=/Users/zhushanwen/Code/melonDS-android-workspace/fix-gl-texture-black-blocks
BASE=c42995caff0144f37ad9791dc9bc96d82e8a6f65
cd "$SUPER" || exit 1
FAIL=0
ok(){ echo "PASS: $1"; }
bad(){ echo "FAIL: $1"; FAIL=1; }

# 1. res/ 下渲染器指引文案落地（相对基线有新增改动）
# diff 对象 = 基线 vs 工作区：本流程禁 git 写、引擎核验后才统一提交，改动在核验时点必然未提交
if git diff --name-only "$BASE" -- app/src/main/res/ | grep -q .; then
  ok "app/src/main/res/ 相对基线有文案改动"
else
  bad "app/src/main/res/ 无改动（指引文案未落地）"
fi

# 2. README 渲染器指引节落地
if git diff --name-only "$BASE" -- README.md | grep -q .; then
  ok "README.md 相对基线有改动"
else
  bad "README.md 无改动（指引节未落地）"
fi

# 3. 渲染器选择逻辑零改动（emulator 渲染目录 + cpp 层）
stray=$(git diff --name-only "$BASE" -- app/src/main/java/me/magnum/melonds/ui/emulator/ app/src/main/cpp/)
if [ -z "$stray" ]; then
  ok "emulator 渲染目录与 cpp 层零改动"
else
  bad "渲染逻辑文件被触及: $stray"
fi

exit $FAIL
