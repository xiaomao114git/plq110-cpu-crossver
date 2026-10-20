#!/usr/bin/env python3
"""Host regression checks; never executes an Android kernel module."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def function(source, name):
    match = re.search(r'^static [^;\n]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise ValueError('function not found: ' + name)
    start = source.index('{', match.start())
    depth = 0
    for i in range(start, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():i+1]
    raise ValueError('unterminated function')


PREAMBLE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define HOOK_NO_ERR 0
#define HOOK_CHAIN_FULL 4090
typedef int hook_err_t;
struct filename { const char *name; };
static int ki_enabled = 1, ki_nuids = 1, ki_ndirs_want = 1;
static int uid_matches = 1, path_calls, uid_calls;
static int ki_uid_hit(void) { ++uid_calls; return uid_matches; }
static int ki_path_is_target(const char *p) { ++path_calls; return p[0] == '/'; }
struct ki_wrap_registration { void *address; void *before; void *after; };
static struct ki_wrap_registration ki_wraps[8];
static int ki_nwraps, register_calls, next_rc, remove_calls;
static struct ki_wrap_registration removed[8];
static hook_err_t hook_wrap(void *addr, int32_t argc, void *before, void *after, void *udata)
{
    (void)addr; (void)argc; (void)before; (void)after; (void)udata;
    ++register_calls; return next_rc;
}
static void hook_unwrap(void *addr, void *before, void *after)
{
    if (remove_calls >= 8) abort();
    removed[remove_calls++] = (struct ki_wrap_registration){addr, before, after};
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } ++checks; } while (0)
'''

MAIN = r'''
int main(void)
{
    int checks = 0;
    struct filename valid = {"/fixture"}, empty = {0};
    CHECK(ki_should_block(0) == 0);
    CHECK(ki_should_block((void *)(intptr_t)-1) == 0);
    CHECK(ki_should_block((void *)(intptr_t)-2) == 0);
    CHECK(ki_should_block((void *)(intptr_t)-4095) == 0);
    CHECK(uid_calls == 0 && path_calls == 0);
    CHECK(ki_should_block(&empty) == 0);
    empty.name = (void *)(intptr_t)-22;
    CHECK(ki_should_block(&empty) == 0);
    CHECK(path_calls == 0);
    uid_matches = 0;
    CHECK(ki_should_block((void *)(uintptr_t)1) == 0);
    CHECK(path_calls == 0);
    uid_matches = 1;
    CHECK(ki_should_block(&valid) == 1);
    ki_enabled = 0;
    CHECK(ki_should_block((void *)(uintptr_t)1) == 0);
    ki_enabled = 1; ki_nuids = 0;
    CHECK(ki_should_block((void *)(uintptr_t)1) == 0);
    ki_nuids = 1; ki_ndirs_want = 0;
    CHECK(ki_should_block((void *)(uintptr_t)1) == 0);

    next_rc = -4093;
    CHECK(ki_register_wrap((void *)1, 3, (void *)101, 0, 0) == next_rc);
    CHECK(ki_nwraps == 0);
    ki_unregister_wraps();
    CHECK(remove_calls == 0);
    next_rc = 0;
    for (int i = 0; i < 8; ++i)
        CHECK(ki_register_wrap((void *)(uintptr_t)(i+1), 3,
              (void *)(uintptr_t)(101+i), 0, 0) == 0);
    CHECK(ki_nwraps == 8);
    int calls = register_calls;
    CHECK(ki_register_wrap((void *)99, 3, (void *)999, 0, 0) == -HOOK_CHAIN_FULL);
    CHECK(register_calls == calls);
    ki_unregister_wraps();
    CHECK(ki_nwraps == 0 && remove_calls == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(removed[i].address == (void *)(uintptr_t)(8-i));
        CHECK(removed[i].before == (void *)(uintptr_t)(108-i));
        CHECK(ki_wraps[i].address == 0 && ki_wraps[i].before == 0);
    }
    ki_unregister_wraps();
    CHECK(remove_calls == 8);
    printf("PASS: %d host checks; error pointers, non-target early return, registration failure, capacity, reverse cleanup, repeated cleanup\n", checks);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser()
    compiler = p.add_mutually_exclusive_group(required=True)
    compiler.add_argument('--zig', type=Path)
    compiler.add_argument('--cc')
    p.add_argument('--source', type=Path, default=Path('spoof_suite/src/kpm/ki_guard_kpm.c'))
    p.add_argument('--output-dir', type=Path, default=Path('diagnostics/kpm-host-tests'))
    a = p.parse_args()
    source = a.source.read_text(encoding='utf-8')
    for fallback in ('path_openat', 'path_lookupat'):
        if f'kallsyms_lookup_name("{fallback}")' in source:
            raise AssertionError('incompatible ABI fallback still present: ' + fallback)
    if 'ki_unregister_wraps();' not in function(source, 'ki_init'):
        raise AssertionError('missing init-failure cleanup')
    output = a.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    harness = output / 'stability_harness.c'
    exe = output / 'stability_harness.exe'
    harness.write_text(PREAMBLE + '\n'.join(function(source, n) for n in
        ('ki_should_block', 'ki_register_wrap', 'ki_unregister_wraps')) + MAIN, encoding='utf-8')
    command = [str(a.zig.resolve()), 'cc'] if a.zig else [a.cc]
    command += ['-O1', '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(exe)]
    subprocess.run(command, check=True, timeout=180)
    result = subprocess.run([str(exe)], check=True, timeout=15, stdout=subprocess.PIPE, text=True)
    print(result.stdout, end='')
    (output / 'result.json').write_text(json.dumps({
        'source_sha256': hashlib.sha256(a.source.read_bytes()).hexdigest(),
        'status': 'pass', 'output': result.stdout.strip(),
        'scope': 'User-space mock tests only; no live hook, concurrency, runtime ABI, or device stability coverage.'
    }, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
