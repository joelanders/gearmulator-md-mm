"""MSVC validation of the assertion-only DSP change; contains no private inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import struct
import subprocess
import time
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--phase', choices=['build', 'test'], required=True)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--build-root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if platform.system() != 'Windows':
    raise SystemExit('This orchestration requires actual Windows/MSVC.')
runtime = args.runtime.resolve()
root = args.build_root.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
root.mkdir(parents=True, exist_ok=True)
driver = Path(__file__).resolve().parent
base = '8c919d2b390fbe08fc6b26ab881cd80b427ec952'
fixed = '61a486d620fd6404e2cab4a75febb26c9472cf03'
sources = {'baseline': root/'baseline-source', 'fixed': runtime/'source/dsp56300'}

def checked(command, label, timeout=1800):
    command = list(map(str, command))
    started = time.monotonic()
    print('START', label, flush=True)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='replace', timeout=timeout)
    (output/(label+'.log')).write_text(result.stdout, encoding='utf-8')
    with (output/'commands.jsonl').open('a', encoding='utf-8') as log:
        log.write(json.dumps(dict(label=label, command=command, exit_code=result.returncode,
                                 seconds=time.monotonic()-started))+'\n')
    print('DONE', label, result.returncode, round(time.monotonic()-started, 2), 'seconds', flush=True)
    if result.returncode:
        print(result.stdout[-12000:], flush=True)
        raise RuntimeError(label+' failed')
    return result.stdout

def git(path, *arguments):
    return subprocess.check_output(['git', '-C', str(path), *arguments], text=True).strip()

def executable(key, name, configuration='Release'):
    matches = list((root/key).rglob(name+'.exe'))
    matches = [p for p in matches if configuration in p.parts]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one {configuration} {name} for {key}: {matches}')
    return matches[0]

def section_hash(path, wanted=b'.text'):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe+4] != b'PE\0\0': raise RuntimeError('Not a PE binary')
    count = struct.unpack_from('<H', data, pe+6)[0]
    optional_size = struct.unpack_from('<H', data, pe+20)[0]
    for index in range(count):
        entry = pe+24+optional_size+40*index
        if data[entry:entry+8].rstrip(b'\0') == wanted:
            size, offset = struct.unpack_from('<II', data, entry+16)
            return hashlib.sha256(data[offset:offset+size]).hexdigest()
    raise RuntimeError('Missing PE text section')

if args.phase == 'build':
    if git(sources['fixed'], 'rev-parse', 'HEAD') != fixed:
        raise RuntimeError('Unexpected candidate DSP revision')
    changed = git(sources['fixed'], 'diff', '--name-only', base, fixed).splitlines()
    if changed != ['source/dsp56kBase/dspassert.h']:
        raise RuntimeError('The DSP comparison contains additional changes: '+str(changed))
    if sources['baseline'].exists():
        raise RuntimeError('Use a fresh baseline build directory')
    checked(['git','clone','--shared','--no-checkout',sources['fixed'],sources['baseline']], 'clone-baseline')
    checked(['git','-C',sources['baseline'],'checkout','--detach',base], 'checkout-baseline')
    checked(['git','-C',sources['baseline'],'submodule','update','--init','source/asmjit'], 'baseline-asmjit')
    for key, source in sources.items():
        checked(['cmake','-S',driver,'-B',root/key,'-G','Ninja Multi-Config',
                 '-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl',
                 '-DCMAKE_VS_PLATFORM_NAME=x64','-DGEARMULATOR_MSVC_EMBED_DEBUG_INFO=ON',
                 '-DCMAKE_C_COMPILER_LAUNCHER=sccache','-DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
                 '-DRUNTIME_SOURCE='+str(runtime),'-DDSP_SOURCE='+str(source)], 'configure-'+key)
        for config in ['Release','Debug']:
            targets = ['dspAssertContract','dsp56kTestRunner','sharedAudioBufferTest',
                       'sharedAudioReducerTest','ringBufferTest','semaphoreTest']
            if config == 'Release': targets.append('dspCorePerf')
            checked(['cmake','--build',root/key,'--config',config,'--parallel','4','--target',*targets],
                    'build-'+key+'-'+config, timeout=2400)
            shutil.copy2(root/key/('compiler-'+config+'.txt'),output/(key+'-'+config+'-compiler.txt'))
    receipt = dict(parent=git(runtime,'rev-parse','HEAD'),
        source_commits={k:git(v,'rev-parse','HEAD') for k,v in sources.items()},
        asmjit_commits={k:git(v/'source/asmjit','rev-parse','HEAD') for k,v in sources.items()},
        host=dict(system=platform.platform(), processor=platform.processor(), cpus=os.cpu_count(),
                  identifier=os.environ.get('PROCESSOR_IDENTIFIER'), cl=shutil.which('cl')),
        github_run_id=os.environ.get('GITHUB_RUN_ID'), github_run_attempt=os.environ.get('GITHUB_RUN_ATTEMPT'),
        binaries={})
    if len(set(receipt['asmjit_commits'].values())) != 1:
        raise RuntimeError('AsmJit revisions differ')
    for key in sources:
        binary = executable(key,'dspCorePerf')
        receipt['binaries'][key] = dict(path=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            text_sha256=section_hash(binary))
    (output/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    raise SystemExit(0)

receipt = json.loads((output/'build-receipt.json').read_text())
if receipt['parent'] != git(runtime,'rev-parse','HEAD'):
    raise RuntimeError('Source changed between build and test')
for key, binary in receipt['binaries'].items():
    if hashlib.sha256(Path(binary['path']).read_bytes()).hexdigest() != binary['sha256']:
        raise RuntimeError('Benchmark binary changed: '+key)

tests = []
for key in sources:
    for config in ['Release','Debug']:
        xml = output/('tests-'+key+'-'+config+'.xml')
        checked(['ctest','--test-dir',root/key,'-C',config,'--output-on-failure','--timeout','600',
                 '--output-junit',xml,'-R',
                 '^(dspAssertContract|dsp56300_unitTests|sharedAudioBufferTest|sharedAudioReducerTest|ringBufferTest|semaphoreTest)$'],
                'test-'+key+'-'+config,timeout=900)
        suite = ET.parse(xml).getroot().attrib
        if int(suite['tests']) != 6 or any(int(suite.get(k,0)) for k in ['failures','skipped','disabled']):
            raise RuntimeError('Incomplete DSP test coverage: '+str(suite))
        tests.append(dict(variant=key,config=config,**suite))

def measure(key, program, mode, iterations, label):
    text = checked([receipt['binaries'][key]['path'],program,mode,str(iterations)],label,timeout=120)
    results = [json.loads(line[len('@RESULT '):]) for line in text.splitlines() if line.startswith('@RESULT ')]
    if len(results) != 1: raise RuntimeError('Expected one benchmark result')
    result = results[0]
    if result['iterations'] != iterations or result['program'] != program or result['mode'] != mode:
        raise RuntimeError('Benchmark arguments/result mismatch')
    if result['wall_ns'] <= 0 or result['instructions'] <= 0 or (mode != 'interpreter' and result['cycles'] <= 0):
        raise RuntimeError('Benchmark performed no work')
    return dict(variant=key, **result)

# Fixed work is calibrated on the baseline, then shared by every A/B observation.
order = ['baseline','fixed','fixed','baseline']*3
rows, comparisons = [], []
fields = ['program','mode','iterations','cycles','instructions','checksum','pc']
for program in ['alu','memory']:
    for mode in ['jit','bounded','interpreter']:
        pilot = measure('baseline',program,mode,100000,'calibrate-'+program+'-'+mode)
        iterations = max(10000,min(500000000,math.ceil(100000*2_000_000_000/pilot['wall_ns'])))
        group = []
        for index,key in enumerate(order):
            result = measure(key,program,mode,iterations,f'perf-{program}-{mode}-{index}-{key}')
            if group and any(result[f] != group[0][f] for f in fields):
                raise RuntimeError('DSP state/cycle mismatch: '+json.dumps(result))
            if result['cpu_ns'] <= 0: raise RuntimeError('No CPU timing resolution')
            result['observation'] = index
            group.append(result); rows.append(result)
            with (output/'measurements.jsonl').open('a',encoding='utf-8') as log:
                log.write(json.dumps(result)+'\n')
        summary = dict(program=program,mode=mode,iterations=iterations)
        for metric in ['cpu_ns','wall_ns']:
            values = {k:[r[metric] for r in group if r['variant']==k] for k in sources}
            summary[metric] = {k:dict(min=min(v),median=statistics.median(v),max=max(v)) for k,v in values.items()}
            summary[metric]['change_percent'] = 100*(statistics.median(values['fixed'])/statistics.median(values['baseline'])-1)
        comparisons.append(summary)
        print('COMPARISON',json.dumps(summary),flush=True)

report = dict(build=receipt, tests=tests, measurements=len(rows),order=order,
    behavior_fields=fields, behavior_mismatches=0, comparisons=comparisons,
    same_text_section=receipt['binaries']['baseline']['text_sha256']==receipt['binaries']['fixed']['text_sha256'],
    caveats=['Synthetic core programs, not MD/MM audio or a firmware benchmark.',
             'Shared hosted runner; timing is descriptive and is not a real-time guarantee.',
             'Apple ThinLTO/LLVM PGO options are disabled on Windows; MSVC PGO is not tested here.'])
(output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('FINAL_DSP_ASSERT_VALIDATION',json.dumps(report),flush=True)
