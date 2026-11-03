#!/usr/bin/env bash
# android16-6.12 SUSFS 适配器的独立取证脚本。
# 现场来源：OnePlusOSS/android_kernel_common_oneplus_sm8845 @ oneplus/sm8845_b_16.0.0_ace_6t
# （Ace 6T 的真源码），补丁来源与构建步骤一致：gitlab simonpunk/susfs4ksu gki-android16-6.12-dev。
# 思路：先在真树上按 CI 的 fuzz 复现「4 个 .rej + hooks.c 有调用无声明」，再让适配器补，
# 补完必须能点名每一条声明/钩子；对照组（不打适配器）必须缺，否则这个测本身是废的。
set -euo pipefail

SRC_REPO=OnePlusOSS/android_kernel_common_oneplus_sm8845
SRC_REF=oneplus/sm8845_b_16.0.0_ace_6t
SUSFS_REF=gki-android16-6.12-dev
KVER=6.12
ACK=android16
FILES=(fs/exec.c fs/proc/base.c fs/proc/task_mmu.c security/selinux/hooks.c)

WS="${1:-$PWD}"
SB=$(mktemp -d)
trap 'rm -rf "$SB"' EXIT

say() { echo "$*"; }
die() { echo "::error::$*"; exit 1; }

mkdir -p "$SB/fs/proc" "$SB/security/selinux"
for f in "${FILES[@]}"; do
  curl -fsSL "https://raw.githubusercontent.com/$SRC_REPO/$SRC_REF/$f" -o "$SB/$f" \
    || die "取不到 $SRC_REPO 的 $f"
  [ -s "$SB/$f" ] || die "$f 取回是空的"
done
curl -fsSL "https://gitlab.com/simonpunk/susfs4ksu/-/raw/$SUSFS_REF/kernel_patches/50_add_susfs_in_gki-$ACK-$KVER.patch" \
  -o "$SB/full.patch" || die "取不到 susfs 补丁"
[ -s "$SB/full.patch" ] || die "susfs 补丁是空的"
python3 "$WS/scripts/split_patch_by_file.py" "$SB/full.patch" "$SB/mini.patch" "${FILES[@]}" || die "裁剪补丁失败"

cd "$SB"
rc=0
patch -p1 -F 0 -i mini.patch >patch.out 2>&1 || rc=$?
say "patch rc=$rc FAILED行数=$(grep -c 'FAILED' patch.out || true) rej=$(find . -name '*.rej' | wc -l)"

# --- 对照组：不打适配器时，hooks.c 必须「有调用、没声明」，且 .rej 在场 ---
grep -q 'susfs_security_context_to_sid(' security/selinux/hooks.c \
  || die "对照组失效：真树里没有 susfs_security_context_to_sid 调用，现场与 run 41657781119 不一致"
if grep -q 'extern int susfs_security_context_to_sid' security/selinux/hooks.c; then
  die "对照组失效：没打适配器就已经有声明，这个测证明不了任何事"
fi
say "对照组 OK：hooks.c 有调用无声明（这正是编译挂的那一条）"

# --- 正组：跑适配器 ---
python3 "$WS/scripts/susfs612_adapter.py" . || die "适配器 rc=$?（锚点不唯一或自检没过）"

need() { # need <file> <字面串> <说明>
  python3 - "$1" "$2" "$3" <<'PYE'
import sys
p, needle, why = sys.argv[1], sys.argv[2], sys.argv[3]
body = open(p, encoding='utf-8', errors='surrogateescape').read()
n = body.count(needle)
if n != 1:
    print("::error::%s 里 %s 命中 %d 次（应 1）：%s" % (p, why, n, why))
    sys.exit(1)
PYE
}

need security/selinux/hooks.c 'extern int susfs_security_context_to_sid(const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp);' 'susfs sid 查询声明'
need security/selinux/hooks.c 'extern bool ksu_selinux_hide_running __read_mostly;' 'KernelSU selinux 开关声明'
need fs/exec.c '#include <linux/susfs_def.h>' 'exec.c susfs 头'
need fs/proc/base.c '#include <linux/susfs_def.h>' 'base.c susfs 头'
need fs/proc/task_mmu.c 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))
			return 0;' 'show_smap 早退'
need fs/proc/task_mmu.c 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))
			continue;' 'rollup 循环跳过'

python3 - <<'PYE'
import sys
body = open('fs/proc/task_mmu.c', encoding='utf-8', errors='surrogateescape').read()
i = body.find('clear_refs_write')
j = body.find('static ssize_t clear_refs_write', 1)
k = j if j > 0 else i
seg = body[k:k + 3000]
if 'SUSFS_IS_INODE_SUS_MAP' in seg:
    print('::error::钩子落进了 clear_refs_write —— 插错函数，会改掉 clear_refs 行为')
    sys.exit(1)
print('clear_refs_write 未被污染')
PYE
[ $? -eq 0 ] || die "clear_refs 污染检查没过"

# --- 幂等：再跑一次不许重复注入 ---
python3 "$WS/scripts/susfs612_adapter.py" . || die "适配器二次运行 rc=$?"
n=$(grep -c 'extern int susfs_security_context_to_sid' security/selinux/hooks.c)
[ "$n" = "1" ] || die "二次运行后声明变成 $n 条（幂等失败）"

say "VERIFIED_612_ADAPTER: 4 文件 6 处钩子全部到位、clear_refs 未污染、幂等通过"
