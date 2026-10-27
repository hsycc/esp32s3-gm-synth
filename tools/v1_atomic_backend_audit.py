#!/usr/bin/env python3
"""Audit the IDF6.1 PSRAM address-dispatched backend; not a timing proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
from tools.v1_loop_target_evidence import expand_response_files
import subprocess


def checked_body(text, symbol):
    match=re.search(r'^\s*[0-9a-f]+ <'+re.escape(symbol)+r'>:\s*\n(.*?)(?=^\S.*file format|^Disassembly of|^\s*[0-9a-f]+ <|\Z)',text,re.M|re.S)
    if not match:raise ValueError('missing backend disassembly: '+symbol)
    body=match.group(1)
    instructions=[]
    for line in body.splitlines():
        m=re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2,8}\s+)+([a-z][a-z0-9_.]*)\b(.*)',line)
        if m:instructions.append((int(m[1],16),m[2],m[3]))
    if not instructions or not any(op.startswith('ret') for _,op,_ in instructions):
        raise ValueError('unrecognized backend instructions: '+symbol)
    for address,op,args in instructions:
        if op.startswith('call') or op in ('jx','ill','ill.n','break','break.n'):
            raise ValueError('call/trap in internal atomic backend: '+symbol)
        if op.startswith('b') or op=='j':
            branch=re.search(r'(?:^|[,\s])([0-9a-f]+)\s+<',args)
            if not branch or int(branch[1],16)<=address:
                raise ValueError('unbounded/unknown branch in backend: '+symbol)
    return body.strip()


def source_contract(header):
    compact=''.join(header.split())
    clauses=(
        '#define_ATOMIC_IF_NOT_EXT_RAM()\\if(!((uintptr_t)ptr>=SOC_EXTRAM_DATA_LOW&&(uintptr_t)ptr<SOC_EXTRAM_DATA_HIGH))',
        'typeold;\\_ATOMIC_HW_STUB_LOAD(n,type);\\_ATOMIC_ENTER_CRITICAL();',
        '_ATOMIC_HW_STUB_STORE(n,type);\\_ATOMIC_ENTER_CRITICAL();',
        'return__atomic_s32c1i_load_##n(ptr,memorder);',
        '__atomic_s32c1i_store_##n(ptr,value,memorder);\\return;',
    )
    if not all(part in compact for part in clauses):
        raise ValueError('IDF address dispatch changed; re-audit instead of waiving')


def audit(idf, build, sdkconfig, profile, output):
    config=sdkconfig.read_text()
    enabled='CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=y' in config.splitlines()
    psram='CONFIG_SPIRAM=y' in config.splitlines()
    if (profile=='physical') != enabled or (profile=='physical') != psram:
        raise ValueError('profile/workaround/PSRAM mismatch')
    report=dict(schema=1,profile=profile,psram=psram,workaround=enabled,
                physical_execution=False,timing_approved=False)
    if enabled:
        files=['priv_include/esp_stdatomic.h','src/stdatomic.c',
               'src/port/xtensa/stdatomic_s32c1i.c','CMakeLists.txt']
        base=idf/'components/esp_libc'
        source_contract((base/files[0]).read_text())
        report['sdk_source_sha256']={p:hashlib.sha256((base/p).read_bytes()).hexdigest() for p in files}
        commands=json.loads((build/'compile_commands.json').read_text())
        selected={}
        for row in commands:
            for suffix in ('/esp_libc/src/stdatomic.c','/esp_libc/src/port/xtensa/stdatomic_s32c1i.c'):
                if row['file'].endswith(suffix):selected[suffix]=row
        if len(selected)!=2:raise ValueError('missing SDK backend compiler commands')
        response_files={}
        args={k: expand_response_files(v.get('arguments') or shlex.split(v['command']),
                                      v['directory'],response_files) for k,v in selected.items()}
        report['response_files']=response_files
        normal=args['/esp_libc/src/stdatomic.c'];hardware=args['/esp_libc/src/port/xtensa/stdatomic_s32c1i.c']
        for values,expected in ((normal,'-mdisable-hardware-atomics'),(hardware,'-mno-disable-hardware-atomics')):
            flags=[v for v in values if v in ('-mdisable-hardware-atomics','-mno-disable-hardware-atomics')]
            if not flags or flags[-1]!=expected:raise ValueError('unexpected SDK atomic compile policy')
        compiler=Path(next(a for a in hardware if a.endswith('gcc')))
        objdump=compiler.with_name(compiler.name[:-3]+'objdump')
        if not objdump.exists():objdump=compiler.with_name('xtensa-esp-elf-objdump')
        if not objdump.exists():raise ValueError('Xtensa objdump missing')
        library=build/'esp-idf/esp_libc/libesp_libc.a'
        report['internal_bodies']={}
        for name in ('__atomic_s32c1i_load_4','__atomic_s32c1i_store_4'):
            raw=subprocess.check_output([str(objdump),'-d','--disassemble='+name,str(library)],text=True)
            report['internal_bodies'][name]=checked_body(raw,name)
        report['sdk_compile_commands']=selected
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('sdk_compile_commands','internal_bodies')},indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('idf','build','sdkconfig','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--profile',choices=('physical','qemu'),required=True)
    a=parser.parse_args();audit(a.idf,a.build,a.sdkconfig,a.profile,a.output)
