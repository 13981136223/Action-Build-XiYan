#!/usr/bin/env python3
"""android16-6.12 的 SUSFS 补丁适配器（多副本版）。

真树实测（源 OnePlusOSS sm8845@oneplus/sm8845_b_16.0.0_ace_6t）：
- fs/exec.c                缺 #include <linux/susfs_def.h> → susfs_is_current_proc_no_su 隐式声明
- fs/proc/base.c           缺同名 include
- security/selinux/hooks.c 缺 susfs 的两个 extern
- fs/proc/task_mmu.c       缺 show_smap 早退 与 rollup 循环内的跳过。
  rollup 真树是 `do { ... } for_each_vma(vmi, vma);`（宏=while），所以用 continue
  （跳到条件推进迭代器）；**不能**拿带花括号的 for_each_vma 当锚点 —— 那个在
  clear_refs_write 里，插错会改掉 clear_refs 的行为（这处是本地验证抓出来的）。

为什么要扫全部副本：run 41704902642 里第 29 步（补丁 + 适配器 + 落地核对）全绿，
可 Build Kernel FAST 仍报 exec.c 未声明 / task_mmu 里 vma 未声明 —— 也就是说
「我们改的那份」和「编译用的那份」不是同一份文件。KernelSU 那边已经因为同样的形状踩过
（树里 3 份 lsm_hook.c），当时的结论就是：猜哪份生效不如逐份改。

补丁一律 fuzz<=2；本脚本只按唯一锚点插入，锚点命中次数≠1 就退出 2。
用法：python3 susfs612_adapter.py <kernel_platform 目录>
"""
import os
import sys

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

# 目标文件用「路径后缀」识别，任何一棵树拷贝都算：common/、msm-kernel/、.source_date_epoch_dir/ …
TARGETS = ('fs/exec.c', 'fs/proc/base.c', 'fs/proc/task_mmu.c', 'security/selinux/hooks.c')
DONE = []


def read(p):
    with open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.read()


def write(p, s):
    with open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)


def find_copies(root):
    """返回 {后缀: [绝对路径, ...]}，按 realpath 去重；MTK 的 common 是符号链接，必须 followlinks。"""
    out = {t: [] for t in TARGETS}
    seen = {t: set() for t in TARGETS}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        norm = dirpath.replace(os.sep, '/')
        for t, rel in ((x, x) for x in TARGETS):
            if not norm.endswith('/' + os.path.dirname(rel)):
                continue
            p = os.path.join(dirpath, os.path.basename(rel))
            if not os.path.isfile(p):
                continue
            rp = os.path.realpath(p)
            if rp in seen[t]:
                continue
            seen[t].add(rp)
            out[t].append(p)
    return out


def insert_after(path, anchor, ins, marker, why):
    s = read(path)
    # 判定标记必须是「要插入的那一行」，不能只写符号名：真树里调用点是补丁的成功 hunk 带进来的，
    # 只查符号名会把「有调用、没声明」当成已完成（run 41657781119 的假绿灯就是这么来的）。
    if marker in s:
        return 'skip'
    n = s.count(anchor)
    if n != 1:
        print('::error::%s 锚点命中 %d 次（应 1），拒绝猜测（%s）' % (path, n, why))
        return 'fail'
    write(path, s.replace(anchor, anchor + ins, 1))
    return 'ok'


def patch_task_mmu(path):
    s = read(path)
    changed = []
    a1 = ('static int show_smap(struct seq_file *m, void *v)\n'
          '{\n\tstruct vm_area_struct *vma = v;\n\tstruct mem_size_stats mss = {};\n')
    g1 = 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\treturn 0;'
    if g1 not in s:
        if s.count(a1) != 1:
            print('::error::%s show_smap 锚点命中 %d 次（应 1）' % (path, s.count(a1)))
            return 'fail'
        s = s.replace(a1, a1 + SMAP_HOOK, 1)
        changed.append('show_smap')
    a2 = '\tdo {\n\t\tsmap_gather_stats(vma, &mss, 0);\n'
    g2 = 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\tcontinue;'
    if g2 not in s:
        if s.count(a2) != 1:
            print('::error::%s rollup do{ 锚点命中 %d 次（应 1）' % (path, s.count(a2)))
            return 'fail'
        s = s.replace(a2, '\tdo {\n' + ROLLUP_HOOK + '\t\tsmap_gather_stats(vma, &mss, 0);\n', 1)
        changed.append('show_smaps_rollup')
    if not changed:
        return 'skip'
    write(path, s)
    DONE.append('task_mmu(' + ','.join(changed) + ')')
    return 'ok'


def main():
    if len(sys.argv) != 2:
        print('用法: susfs612_adapter.py <kernel_platform 目录>')
        return 2
    copies = find_copies(sys.argv[1])
    total = 0
    for t in TARGETS:
        if not copies[t]:
            print('::error::树里一份 %s 都没找到（布局与预期不符），拒绝当没这回事' % t)
            return 2
        total += len(copies[t])

    simple = {
        'fs/exec.c': ('#include <linux/fdtable.h>\n', INC_EXEC, '#include <linux/susfs_def.h>', 'exec.c 用了 susfs_is_current_proc_no_su'),
        'fs/proc/base.c': ('#include <linux/cpufreq_times.h>\n', INC_BASE, '#include <linux/susfs_def.h>', 'base.c 用了 SUS_MAP / OPEN_REDIRECT 钩子'),
        'security/selinux/hooks.c': ('struct selinux_state selinux_state;\n', EXT_SELINUX,
                                     'extern int susfs_security_context_to_sid', 'selinux hooks 需要 susfs extern'),
    }
    for t, (anchor, ins, marker, why) in simple.items():
        for p in copies[t]:
            r = insert_after(p, anchor, ins, marker, why)
            if r == 'fail':
                return 2
            if r == 'ok':
                DONE.append(p.replace(os.sep, '/').split('kernel_platform/')[-1])
    for p in copies['fs/proc/task_mmu.c']:
        if patch_task_mmu(p) == 'fail':
            return 2

    # 末尾逐份复查：每一份目标文件都得带着自己那条声明/钩子（marker 用插入行，不用裸符号）
    checks = {
        'fs/exec.c': '#include <linux/susfs_def.h>',
        'fs/proc/base.c': '#include <linux/susfs_def.h>',
        'security/selinux/hooks.c': 'extern int susfs_security_context_to_sid',
        'fs/proc/task_mmu.c': 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n\t\t\treturn 0;',
    }
    for t, marker in checks.items():
        for p in copies[t]:
            if marker not in read(p):
                print('::error::适配后 %s 仍缺 %s' % (p, marker))
                return 2

    print('SUSFS_612_ADAPTER: 扫到 %d 份目标文件（exec.c %d / base.c %d / task_mmu %d / hooks %d），全部到位'
          % (total, len(copies['fs/exec.c']), len(copies['fs/proc/base.c']),
             len(copies['fs/proc/task_mmu.c']), len(copies['security/selinux/hooks.c'])))
    print('   本次实际改动：' + (', '.join(DONE) if DONE else '无（本就齐全）'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
