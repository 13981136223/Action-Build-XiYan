#!/usr/bin/env bash
# 开编之前确保 drivers/kernelsu/preinit/runner/runner.c 真的在场。
# 为什么放在这里而不是 SukiSU 装完之后那一步：run 41733417741 的现场打印说明
# 「修复时两处都报已就位，开编前整个 preinit 里一个 runner.c 都没有」——
# 说明中间有人重排过树（setup.sh 之后还有步骤在动 drivers/kernelsu）。
# 补的来源优先用 KernelSU 自己的克隆（kernel_platform/KernelSU/kernel/preinit/…），
# 两种上游布局都试：tiann 的 preinit/runner，和 SukiSU 的 preinit/common/runner。
set -u
WS=${1:-"$PWD/kernel_workspace"}
KP="$WS/kernel_platform"
[ -d "$KP" ] || { echo "::error::找不到 $KP"; exit 1; }

KSU=""
for C in "$KP/KernelSU/kernel" "$KP/KernelSU" "$KP/preinit/kernel"; do
  [ -d "$C/preinit" ] && { KSU="$C"; break; }
done
echo "[源] KernelSU 克隆的 preinit = ${KSU:-没找到}"

fixed=0
for PRE in $(find -L "$KP" -type d -path '*/drivers/kernelsu/preinit' 2>/dev/null); do
  if [ -f "$PRE/runner/runner.c" ]; then
    echo "[在场] $PRE/runner/runner.c"
    continue
  fi
  SRC=""
  for C in "$PRE/common/runner" ${KSU:+$KSU/preinit/runner} ${KSU:+$KSU/preinit/common/runner}; do
    [ -f "$C/runner.c" ] && { SRC="$C"; break; }
  done
  if [ -z "$SRC" ]; then
    echo "::error::$PRE 缺 runner/runner.c，且四个来源里一个都没有（$PRE/common/runner、$KSU/preinit/runner、$KSU/preinit/common/runner）"
    exit 1
  fi
  mkdir -p "$PRE/runner"
  cp -rL "$SRC"/. "$PRE/runner"/
  [ -f "$PRE/runner/runner.c" ] || { echo "::error::从 $SRC 复制后仍缺 $PRE/runner/runner.c"; exit 1; }
  echo "[补齐] $SRC → $PRE/runner（runner.c $(wc -c < "$PRE/runner/runner.c") B）"
  fixed=$((fixed+1))
done

# 硬断：谁引用 runner 就必须能对上，不然 make 只会甩一句 No rule
miss=0
for K in $(find -L "$KP" -type f \( -name Makefile -o -name Kbuild \) -path '*/drivers/kernelsu/*' 2>/dev/null); do
  grep -qE 'runner/runner\.c|runner\.elf' "$K" || continue
  P=$(dirname "$K")
  case "$P" in */preinit) D="$P/runner/runner.c";; *) D="$P/preinit/runner/runner.c";; esac
  if [ ! -f "$D" ]; then echo "::error::$K 引用 runner 但 $D 不在"; miss=1; fi
done
[ "$miss" = "0" ] || exit 1
# 关键：make 报的是 "../../drivers/kernelsu/preinit/runner/runner.c"，而 make[5] 的工作目录是
# 输出树 $O/drivers/kernelsu/preinit —— 从那儿数 ../../ 落回 $O/drivers/...，也就是它要的是
# **out 树里**的 runner.c（run 41738160219：源树里已备好、保障步的硬断也过了，make 仍说 No rule）。
# 所以同一份 runner.c 也要铺到对应的 out 路径（out 目录此刻可以不存在，mkdir -p 建出来即可）。
for PRE in $(find -L "$KP" -type d -path '*/drivers/kernelsu/preinit' 2>/dev/null); do
  [ -f "$PRE/runner/runner.c" ] || continue
  TOP=${PRE%/drivers/kernelsu/preinit}          # 该树的根（common/ 或 kernel-6.12/）
  OUT="$TOP/out/drivers/kernelsu/preinit/runner"
  if [ -f "$OUT/runner.c" ]; then
    echo "[out 树已有] $OUT/runner.c"
  elif mkdir -p "$OUT" 2>/dev/null && cp -rL "$PRE/runner/." "$OUT/" 2>/dev/null; then
    echo "[铺到 out 树] $PRE/runner/runner.c → $OUT/runner.c"
  else
    echo "::error::铺不进 $OUT"
    exit 1
  fi
done

# 最后一条兜底，且不依赖我猜 O= 是哪个目录：把那条规则里的相对依赖改写成绝对形式。
# make 报 `No rule to make target '../../drivers/kernelsu/preinit/runner/runner.c'`，
# 而源树、out 树两处我都铺了文件、硬断也过了它照样报 —— 说明 `../../` 是从某个我猜不到的
# 输出目录（Kleaf/bazel 的 execroot）数上去的。改成 `$(srctree)/drivers/...` 就跟 O= 无关了。
for K in $(find -L "$KP" -type f \( -name Makefile -o -name Kbuild \) -path '*/drivers/kernelsu/*' 2>/dev/null); do
  grep -q '\.\./\.\./drivers/kernelsu/preinit/' "$K" || continue
  sed -i 's#\.\./\.\./drivers/kernelsu/preinit/#$(srctree)/drivers/kernelsu/preinit/#g' "$K"
  grep -q 'srctree)/drivers/kernelsu/preinit/' "$K" || { echo "::error::$K 改写失败"; exit 1; }
  echo "[依赖改绝对] $K → \$(srctree)/drivers/kernelsu/preinit/…（$(grep -c 'srctree)/drivers/kernelsu/preinit/' "$K") 处）"
done

echo "ENSURE_PREINIT: 补齐 $fixed 处，引用与文件全部对得上"
