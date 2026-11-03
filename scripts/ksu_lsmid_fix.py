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
            '#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)')
FUNC_ANCHOR = 'void __init ksu_lsm_hook_init(void)\n{\n'


def find_files(root):
    """树里 lsm_hook.c 可能有多份副本，实测 run 41670147343 命中 3 份：
       common/drivers/kernelsu/hook/（参与编译的那份）、
       kernel_platform/KernelSU/kernel/hook/（setup.sh 用的克隆）、
       .source_date_epoch_dir/drivers/kernelsu/hook/（构建期另一份树拷贝）。
       只挑 drivers/kernelsu/hook 这条路径（不挑克隆），按 realpath 去重后逐个改 ——
       猜「哪份才生效」不如都改：动作幂等，漏一份才是真炸。
       MTK 单仓布局里 common 是指向 ../kernel-<ver> 的符号链接，os.walk 要 followlinks=True。
    """
    hits, seen = [], set()
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        if 'lsm_hook.c' not in filenames:
            continue
        if '/drivers/kernelsu/hook' not in dirpath.replace(os.sep, '/'):
            continue
        p = os.path.realpath(os.path.join(dirpath, 'lsm_hook.c'))
        if p not in seen:
            seen.add(p)
            hits.append(p)
    return hits


def patch_one(p):
    with open(p, encoding='utf-8', errors='surrogateescape') as f:
        s = f.read()
    if 'ksu_lsm_id' in s:
        return 'skip'
    for a, why in ((CALL_OLD, '裸字符串调用行'), (FUNC_ANCHOR, 'ksu_lsm_hook_init 函数头')):
        n = s.count(a)
        if n != 1:
            print('::error::%s 锚点命中 %d 次（应 1）：%s' % (p, n, why))
            return 'fail'
    s = s.replace(FUNC_ANCHOR, DEF_NEW + FUNC_ANCHOR, 1)
    s = s.replace(CALL_OLD, CALL_NEW + '\n', 1)
    # 自检：声明在场、6.12 分支传的是指针且只传一次。#else 里保留原写法是预期的，
    # 所以不能用「"ksu"); 只出现一次」这种脆判据（它在 #else 里还在）。
    if ('static struct lsm_id ksu_lsm_id' not in s
            or s.count('ARRAY_SIZE(ksu_hooks), &ksu_lsm_id);') != 1):
        print('::error::%s 改完自检没过（声明缺失或 &ksu_lsm_id 传参次数不对）' % p)
        return 'fail'
    with open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)
    return 'ok'


def main():
    if len(sys.argv) != 2:
        print('用法: ksu_lsmid_fix.py <kernel_platform 目录>')
        return 2
    hits = find_files(sys.argv[1])
    if not hits:
        print('::error::树里找不到任何 drivers/kernelsu/hook/lsm_hook.c —— SukiSU 没进树？')
        return 2
    done = []
    for p in hits:
        r = patch_one(p)
        if r == 'fail':
            return 2
        if r == 'ok':
            done.append(p)
    print('KSU_LSMID_FIX: 扫到 %d 份、改动 %d 份，其余已是修后状态' % (len(hits), len(done)))

    # 硬断：改完再走一遍树，任何一份 drivers/kernelsu/hook/lsm_hook.c 都不许还留着裸 "ksu" 调用。
    # 早先用「路径里必须有 /common/ 或 /kernel-」认参与编译的那份，实测 6.1 线上只扫到 1 份且两条都不匹配
    # （run 41683212606 就是被自己的判定打死的）—— 逐份复查比猜哪份生效可靠。
    again = find_files(sys.argv[1])
    for (p2 in again) {
        f = again[p2]
        body = open(f, encoding='utf-8', errors='surrogateescape').read()
        if 'ksu_lsm_id' not in body:
            print('::error::%s 仍是裸 "ksu" 调用，没被修到' % f)
            return 2
    }
    print('KSU_LSMID_FIX: 复查 ' + str(len(again)) + ' 份，全部已带 &ksu_lsm_id 分支')
    return 0


if __name__ == '__main__':
    sys.exit(main())
