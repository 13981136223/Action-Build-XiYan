#!/usr/bin/env bash
# 6.12 适配器打完之后的落地核对：include 顺序 + 钩子作用域。
# 为什么要单独一步：run 41696139519 里适配器自称"已补 4 个文件"，可编译还是报
#   fs/exec.c:1949: call to undeclared function 'susfs_is_current_proc_no_su'
#   fs/proc/task_mmu.c:1516: use of undeclared identifier 'vma'
# 也就是说"补过了"不等于"补对位置"。这里 30 秒内就把这两种错位点名，不用等 30 分钟编译。
set -e
D=${1:-.}
[ -f "$D/fs/exec.c" ] || { echo "::error::找不到 $D/fs/exec.c"; exit 1; }
[ -f "$D/fs/proc/task_mmu.c" ] || { echo "::error::找不到 $D/fs/proc/task_mmu.c"; exit 1; }

INC=$(grep -n '#include <linux/susfs_def.h>' "$D/fs/exec.c" | head -1 | cut -d: -f1)
USE=$(grep -n 'susfs_is_current_proc_no_su' "$D/fs/exec.c" | tail -1 | cut -d: -f1)
[ -n "$INC" ] || { echo "::error::fs/exec.c 里没有 susfs_def.h 的 include（适配器没补上或被判重跳过）"; exit 1; }
[ -n "$USE" ] || { echo "::error::fs/exec.c 里找不到 no_su 调用点，与已知真树不符"; exit 1; }
echo "exec.c：susfs_def.h 在第 $INC 行，no_su 调用在第 $USE 行"
[ "$INC" -lt "$USE" ] || { echo "::error::include($INC) 不在调用($USE) 之前，声明进不去"; exit 1; }

python3 - "$D/fs/proc/task_mmu.c" <<'PYCHK'
import re
import sys
p = sys.argv[1]
lines = open(p, encoding='utf-8', errors='surrogateescape').read().split('\n')
bad = []
for i, ln in enumerate(lines):
    if 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))' in ln:
        back = '\n'.join(lines[max(0, i - 60):i + 1])
        if not re.search(r'vm_area_struct \*vma|vma = vma_next|vma = |for_each_vma\(vmi, vma\)', back):
            bad.append(i + 1)
if bad:
    print('::error::task_mmu.c 第 %s 行的钩子上文 60 行内看不到 vma 声明（插进了错误的函数）'
          % ','.join(map(str, bad)))
    sys.exit(1)
print('task_mmu.c：钩子共 %d 处，都在能看到 vma 的作用域里'
      % sum(1 for l in lines if 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))' in l))
PYCHK

echo "CHECK612_LANDED: include 顺序与钩子作用域都对"
