#!/usr/bin/env python3
"""Offline AArch64 register-use gate. Does not load or modify a KPM."""
import argparse
import hashlib
import io
import json
import re
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile

REG = re.compile(r"(?<![A-Za-z0-9_])(?:[xw]18|[vqdsbh][0-9]+)(?![A-Za-z0-9_])")


def inspect_code(code, address=0):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    findings = []
    consumed = 0
    for ins in md.disasm(code, address):
        consumed += ins.size
        regs = sorted(set(REG.findall(ins.op_str)))
        if regs:
            findings.append({"offset": hex(ins.address), "instruction":
                             f"{ins.mnemonic} {ins.op_str}", "registers": regs})
    return findings, consumed


def audit(path):
    data = path.read_bytes()
    elf = ELFFile(io.BytesIO(data))
    if elf['e_machine'] != 'EM_AARCH64' or elf.elfclass != 64:
        raise ValueError('expected ELF64 AArch64')
    symtab = elf.get_section_by_name('.symtab')
    if symtab is None:
        raise ValueError('symbol table required for function-boundary audit')
    found, incomplete, functions, undefined = [], [], 0, set()
    seen = set()
    for sym in symtab.iter_symbols():
        idx = sym['st_shndx']
        if idx == 'SHN_UNDEF' and sym.name:
            undefined.add(sym.name)
        if sym['st_info']['type'] != 'STT_FUNC' or not isinstance(idx, int):
            continue
        sec = elf.get_section(idx)
        size = sym['st_size']
        if not size or not (sec['sh_flags'] & 4):
            continue
        start = sym['st_value'] - sec['sh_addr']
        key = (idx, start, size)
        if key in seen:
            continue
        seen.add(key)
        raw = sec.data()
        if start < 0 or start + size > len(raw):
            raise ValueError(f'invalid function bounds: {sym.name}')
        functions += 1
        hits, consumed = inspect_code(raw[start:start+size], sym['st_value'])
        for hit in hits:
            found.append({"function": sym.name, "section": sec.name, **hit})
        if consumed != size:
            incomplete.append({"function": sym.name, "size": size, "decoded": consumed})
    mapping_regions = 0
    if not functions:
        # llvm-strip can remove functions but retain ELF AArch64 $x/$d mappings.
        for idx, sec in enumerate(elf.iter_sections()):
            if not (sec['sh_flags'] & 4) or not sec['sh_size']:
                continue
            markers = sorted((s['st_value'] - sec['sh_addr'], s.name[1])
                             for s in symtab.iter_symbols()
                             if s['st_shndx'] == idx and
                             (s.name == '$x' or s.name.startswith('$x.') or
                              s.name == '$d' or s.name.startswith('$d.')))
            if not markers or markers[0][0] != 0:
                raise ValueError(f'no complete code/data mapping: {sec.name}')
            raw = sec.data()
            for pos, (start, kind) in enumerate(markers):
                end = markers[pos+1][0] if pos+1 < len(markers) else len(raw)
                if start < 0 or end > len(raw) or end < start:
                    raise ValueError('invalid code/data mapping')
                if kind != 'x' or start == end:
                    continue
                mapping_regions += 1
                hits, consumed = inspect_code(raw[start:end], sec['sh_addr'] + start)
                for hit in hits:
                    found.append({'function': '<stripped>', 'section': sec.name, **hit})
                if consumed != end-start:
                    incomplete.append({'section': sec.name, 'offset': start,
                                       'size': end-start, 'decoded': consumed})
        if not mapping_regions:
            raise ValueError('no function or mapping-symbol code boundaries; audit incomplete')
    return {"artifact": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
            "functions_checked": functions, "mapping_regions_checked": mapping_regions,
            "forbidden_register_instructions": found,
            "incomplete_functions": incomplete, "undefined_symbols": sorted(undefined),
            "pass": not found and not incomplete,
            "scope": "Register-use gate only; not a runtime stability or ABI certification."}


def selftest():
    # mov x18,x0; movi v0.4s,#0; mov x0,x1; ret
    bad_x18 = bytes.fromhex('f20300aa')
    bad_simd = bytes.fromhex('0004004f')
    good = bytes.fromhex('e00301aac0035fd6')
    assert inspect_code(bad_x18)[0][0]['registers'] == ['x18']
    assert inspect_code(bad_simd)[0][0]['registers'] == ['v0']
    assert inspect_code(good) == ([], len(good))
    print('PASS: x18 rejected; SIMD rejected; GPR/ret accepted')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('artifact', nargs='?', type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        selftest()
    if args.artifact:
        result = audit(args.artifact)
        text = json.dumps(result, indent=2, ensure_ascii=True) + '\n'
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding='utf-8')
            print(f"functions={result['functions_checked']} hits={len(result['forbidden_register_instructions'])} "
                  f"incomplete={len(result['incomplete_functions'])} pass={result['pass']}")
        else:
            print(text)
        raise SystemExit(0 if result['pass'] else 1)
    if not args.selftest:
        ap.error('artifact or --selftest is required')
