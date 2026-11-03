#!/usr/bin/env python3
"""把一份多文件 unified diff 裁成只含指定文件的子补丁（保持 patch -p1 可用）。
用法: python3 split_patch_by_file.py <patch> <out> <path1> [path2 ...]
路径写 b 侧相对名（如 fs/exec.c），不带 a/ 前缀。
"""
import sys


def main():
    if len(sys.argv) < 4:
        print('用法: split_patch_by_file.py <patch> <out> <path...>')
        return 2
    patch, out = sys.argv[1], sys.argv[2]
    want = set(sys.argv[3:])
    keep_lines = []
    keep = False
    seen = set()
    with open(patch, encoding='utf-8', errors='surrogateescape') as f:
        for ln in f.read().split('\n'):
            if ln.startswith('--- a/'):
                p = ln[6:].strip()
                keep = p in want
                if keep:
                    seen.add(p)
                    keep_lines.append('--- ' + p)
                continue
            if ln.startswith('+++ b/'):
                if keep:
                    keep_lines.append('+++ ' + ln[4:].strip())
                continue
            if ln.startswith('diff --git '):
                continue
            if keep:
                keep_lines.append(ln)
    missing = want - seen
    with open(out, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write('\n'.join(keep_lines))
    print('裁出 %d 个文件、%d 行' % (len(seen), len(keep_lines)))
    if missing:
        print('::error::补丁里没有这些文件：%s' % ' '.join(sorted(missing)))
        return 2
    if not seen:
        print('::error::一个文件都没裁出来，补丁格式与预期不符')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
