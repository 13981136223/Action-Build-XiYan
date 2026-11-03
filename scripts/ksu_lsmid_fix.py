#!/usr/bin/env python3
"""KernelSU 的 security_add_hooks() 在 6.12 换了第三参数类型（const char* → const struct lsm_id*）。
上游 SukiSU 还写着 "ksu"，6.12 线上会报 incompatible pointer types（run 41663264213 就挂在这）。
这里只在锚点唯一时改，改完自检；不唯一就退出 2，不猜。

安全性靠版本条件编译：>= 6.12 传 &ksu_lsm_id，< 6.12 保持原样，所以这条修复对 6.1/6.6 是无害的。
用法：python3 ksu_lsmid_fix.py <kernel_platform 目录>
"""
import os
import sys

CALL_OLD = '    security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");'
DEF_NEW = ('#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)\n'
           'static struct lsm_id ksu_lsm_id = {\n'
           '\t.name = "ksu",\n'
           '};\n'
           '#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)\n\n')
CALL_NEW = ('#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)\n'
            '    security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), &ksu_lsm_id);\n'
            '#else\n'
            '    security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");\n'
            '#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)\n')
FUNC_ANCHOR = 'void __init ksu_lsm_hook_init(void)\n{\n'


def find_file(root):
    hits = []
    # followlinks=True：MTK 是单仓布局，kernel_platform/common 是指向 ../kernel-<ver> 的符号链接，
    # 不跟随就永远找不到文件（同一形状的坑已经在 susfs.c 那次踩过）。
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        if 'lsm_hook.c' in filenames:
            p = os.path.join(dirpath, 'lsm_hook.c')
            if os.path.basename(dirpath) == 'hook' and 'kernelsu' in dirpath.lower():
                hits.append(p)
    return hits


def main():
    if len(sys.argv) != 2:
        print('用法: ksu_lsmid_fix.py <kernel_platform 目录>')
        return 2
    hits = find_file(sys.argv[1])
    if len(hits) != 1:
        print('::error::lsm_hook.c 命中 %d 个（应 1）：%s' % (len(hits), ' '.join(hits)))
        return 2
    p = hits[0]
    with open(p, encoding='utf-8', errors='surrogateescape') as f:
        s = f.read()

    if 'ksu_lsm_id' in s:
        print('KSU_LSMID_FIX: 已修过，跳过 ' + p)
        return 0

    for a, why in ((CALL_OLD, '裸字符串调用行'), (FUNC_ANCHOR, 'ksu_lsm_hook_init 函数头')):
        n = s.count(a)
        if n != 1:
            print('::error::%s 锚点命中 %d 次（应 1）：%s' % (p, n, why))
            return 2

    s = s.replace(FUNC_ANCHOR, DEF_NEW + FUNC_ANCHOR, 1)
    s = s.replace(CALL_OLD, CALL_NEW.rstrip('\n') + '\n', 1)
    with open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)

    # 自检：声明/传参/旧写法三者都要对得上，且不能出现「声明没了但还在传 &ksu_lsm_id」
    # 自检：声明在场、6.12 分支传的是指针、且只传一次；#else 里保留原写法是预期的，
    # 所以不能用「"ksu"); 只出现一次」这种脆判据（它在 #else 里还在）。
    if ('static struct lsm_id ksu_lsm_id' not in s
            or s.count('ARRAY_SIZE(ksu_hooks), &ksu_lsm_id);') != 1):
        print('::error::改完自检没过（声明缺失或 &ksu_lsm_id 传参次数不对）')
        return 2
    print('KSU_LSMID_FIX: 已改 ' + p + '（6.12 传 &ksu_lsm_id，<6.12 保持原样）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
