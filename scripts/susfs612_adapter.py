#!/usr/bin/env python3
"""android16-6.12 的 SUSFS 补丁适配器。

真树实测（源 OnePlusOSS sm8845@oneplus/sm8845_b_16.0.0_ace_6t）：
- fs/exec.c                缺 #include <linux/susfs_def.h> → susfs_is_current_proc_no_su 隐式声明
- fs/proc/base.c           缺同名 include
- security/selinux/hooks.c 缺 susfs 的两个 extern
- fs/proc/task_mmu.c       缺 show_smap 早退 与 rollup 循环内的跳过。
  rollup 真树是 `do { ... } for_each_vma(vmi, vma);`（宏=while），所以用 continue
  （跳到条件推进迭代器）；**不能**拿带花括号的 for_each_vma 当锚点 —— 那个在
  clear_refs_write 里，插错会改掉 clear_refs 的行为（这处是本地验证抓出来的）。

补丁一律 fuzz<=2；本脚本只按唯一锚点插入，锚点命中次数≠1 就退出 2。
用法：python3 susfs612_adapter.py <kernel_root>
"""
import os
import sys

DONE = []


def read(p):
    with open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.read()


def write(p, s):
    with open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)


def insert_after(root, rel, anchor, ins, marker, why):
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        print('::warning::%s 不存在，跳过（%s）' % (rel, why))
        return
    s = read(p)
    if marker in s:
        return
    n = s.count(anchor)
    if n != 1:
        print('::error::%s 锚点命中 %d 次（应 1），拒绝猜测（%s）' % (rel, n, why))
        sys.exit(2)
    write(p, s.replace(anchor, anchor + ins, 1))
    DONE.append(rel)


INC_EXEC = '#ifdef CONFIG_KSU_SUSFS\n#include <linux/susfs_def.h>\n#endif\n'
INC_BASE = ('#if defined(CONFIG_KSU_SUSFS_SUS_MAP) || defined(CONFIG_KSU_SUSFS_OPEN_REDIRECT)\n'
            '#include <linux/susfs_def.h>\n'
            '#endif // #if defined(CONFIG_KSU_SUSFS_SUS_MAP) || defined(CONFIG_KSU_SUSFS_OPEN_REDIRECT)\n')
EXT_SELINUX = ('#ifdef CONFIG_KSU_SUSFS\n'
               'extern bool ksu_selinux_hide_running __read_mostly;\n'
               'extern int susfs_security_context_to_sid(const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp);\n'
               '#endif // #ifdef CONFIG_KSU_SUSFS\n')

SMAP_HOOK = ('#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n'
             '\tif (vma->vm_file) {\n'
             '\t\tif (SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n'
             '\t\t\treturn 0;\n'
             '\t}\n'
             '#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n')
ROLLUP_HOOK = ('#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n'
               '\t\tif (vma->vm_file && SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n'
               '\t\t\tcontinue;\n'
               '#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n')


def patch_task_mmu(root):
    p = os.path.join(root, 'fs/proc/task_mmu.c')
    if not os.path.exists(p):
        print('::warning::fs/proc/task_mmu.c 不存在，跳过')
        return
    s = read(p)
    changed = []

    a1 = ('static int show_smap(struct seq_file *m, void *v)\n'
          '{\n\tstruct vm_area_struct *vma = v;\n\tstruct mem_size_stats mss = {};\n')
    if 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\treturn 0;' not in s:
        if s.count(a1) != 1:
            print('::error::task_mmu.c show_smap 锚点命中 %d 次（应 1）' % s.count(a1))
            sys.exit(2)
        s = s.replace(a1, a1 + SMAP_HOOK, 1)
        changed.append('show_smap')

    a2 = '\tdo {\n\t\tsmap_gather_stats(vma, &mss, 0);\n'
    if 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\tcontinue;' not in s:
        if s.count(a2) != 1:
            print('::error::task_mmu.c rollup do{ 锚点命中 %d 次（应 1）' % s.count(a2))
            sys.exit(2)
        s = s.replace(a2, '\tdo {\n' + ROLLUP_HOOK + '\t\tsmap_gather_stats(vma, &mss, 0);\n', 1)
        changed.append('show_smaps_rollup')

    if changed:
        write(p, s)
        DONE.append('fs/proc/task_mmu.c(' + ','.join(changed) + ')')


def main():
    if len(sys.argv) != 2:
        print('用法: susfs612_adapter.py <kernel_root>')
        return 2
    root = sys.argv[1]

    # 判定标记必须是「要插入的那一行」，不能只写符号名：真树里调用点（hooks.c:6643
    # susfs_security_context_to_sid(...)）是补丁的成功 hunk 带进来的，只查符号名会把
    # 「有调用、没声明」当成已完成，末尾自检同样被骗过（run 41657781119 就是这么挂的）。
    insert_after(root, 'fs/exec.c', '#include <linux/fdtable.h>\n', INC_EXEC,
                 '#include <linux/susfs_def.h>', 'exec.c 用了 susfs_is_current_proc_no_su')
    insert_after(root, 'fs/proc/base.c', '#include <linux/cpufreq_times.h>\n', INC_BASE,
                 '#include <linux/susfs_def.h>', 'base.c 用了 SUS_MAP / OPEN_REDIRECT 钩子')
    insert_after(root, 'security/selinux/hooks.c', 'struct selinux_state selinux_state;\n', EXT_SELINUX,
                 'extern int susfs_security_context_to_sid', 'selinux hooks 需要 susfs extern')
    patch_task_mmu(root)

    checks = [
        ('fs/exec.c', '#include <linux/susfs_def.h>'),
        ('fs/proc/base.c', '#include <linux/susfs_def.h>'),
        ('security/selinux/hooks.c', 'extern int susfs_security_context_to_sid'),
        ('security/selinux/hooks.c', 'extern bool ksu_selinux_hide_running'),
        ('fs/proc/task_mmu.c', 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\treturn 0;'),
        ('fs/proc/task_mmu.c', 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\tcontinue;'),
    ]
    gone = []
    for rel, marker in checks:
        q = os.path.join(root, rel)
        if os.path.exists(q) and marker not in read(q):
            gone.append(rel + ' 缺 ' + marker)
    print('SUSFS_612_ADAPTER: 已补 ' + (', '.join(DONE) if DONE else '无（本就齐全）'))
    if gone:
        print('::error::适配后仍缺：' + '；'.join(gone))
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
