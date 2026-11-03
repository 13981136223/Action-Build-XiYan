#!/usr/bin/env bash
# KernelSU LSM 修复脚本的闭环取证：用上游真文件在本地假树里复现「三种副本」的形状。
# 上一版就是因为只允许命中 1 份，被树里 3 份副本（common/、KernelSU 克隆、.source_date_epoch_dir/）
# 直接打死，害得 ace_6t / ace5_race / ace5_ultra 三轮全挂（run 41670147343 等）。
set -euo pipefail
WS="${1:-$PWD}"
SB=$(mktemp -d); trap 'rm -rf "$SB"' EXIT
SRC=https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/builtin/kernel/hook/lsm_hook.c

say() { echo "$*"; }
die() { echo "::error::$*"; exit 1; }

mkdir -p "$SB/kernel_platform/common/drivers/kernelsu/hook" \
         "$SB/kernel_platform/KernelSU/kernel/hook" \
         "$SB/kernel_platform/.source_date_epoch_dir/drivers/kernelsu/hook"
curl -fsSL "$SRC" -o "$SB/real.c" || die "取不到上游 lsm_hook.c"
for d in kernel_platform/common/drivers/kernelsu/hook kernel_platform/KernelSU/kernel/hook kernel_platform/.source_date_epoch_dir/drivers/kernelsu/hook; do
  cp "$SB/real.c" "$SB/$d/lsm_hook.c"
done

grep -q 'ksu_lsm_id' "$SB/kernel_platform/common/drivers/kernelsu/hook/lsm_hook.c" \
  && die "对照组失效：没跑修复脚本就已经有 ksu_lsm_id"
say "对照组 OK：三份副本都还是上游原样（裸 \"ksu\" 调用）"

python3 "$WS/scripts/ksu_lsmid_fix.py" "$SB/kernel_platform" || die "修复脚本 rc=$?"

for d in kernel_platform/common/drivers/kernelsu/hook kernel_platform/.source_date_epoch_dir/drivers/kernelsu/hook; do
  F="$SB/$d/lsm_hook.c"
  grep -q 'static struct lsm_id ksu_lsm_id' "$F" || die "$d 缺声明"
  [ "$(grep -c 'ARRAY_SIZE(ksu_hooks), &ksu_lsm_id);' "$F")" = "1" ] || die "$d 传参次数不对"
done
say "正组 OK：参与编译的两份都已带上 &ksu_lsm_id 分支"

# 反向：锚点被改坏时必须拒绝通过（不许静默跳过）
printf '%s\n' 'void __init ksu_lsm_hook_init(void)' '{' '    security_add_hooks(a, b, "ksu");' '    security_add_hooks(a, b, "ksu");' > "$SB/bad.c"
mkdir -p "$SB/badtree/drivers/kernelsu/hook"; cp "$SB/bad.c" "$SB/badtree/drivers/kernelsu/hook/lsm_hook.c"
if python3 "$WS/scripts/ksu_lsmid_fix.py" "$SB/badtree" >/dev/null 2>&1; then
  die "反向测失败：锚点命中 2 次却被当成通过"
fi
say "反向测 OK：锚点不唯一会被点名并退出"

# 幂等：再跑一次不许重复注入
python3 "$WS/scripts/ksu_lsmid_fix.py" "$SB/kernel_platform" >/dev/null || die "二次运行失败"
[ "$(grep -c 'static struct lsm_id ksu_lsm_id' "$SB/kernel_platform/common/drivers/kernelsu/hook/lsm_hook.c")" = "1" ] \
  || die "幂等失败：声明被重复注入"
say "VERIFIED_LSMID: 三副本形状、正组、反向、幂等全过"
