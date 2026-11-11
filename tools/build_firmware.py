#!/usr/bin/env python3
"""Build the original ZIP projects with reviewed firmware-overrides applied.

Requires Python 3, arm-none-eabi GCC/binutils and newlib-nano. Source workspaces
are generated under build/; edit firmware-overrides/, not generated files.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    subprocess.run([str(x) for x in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, help='directory with arm-none-eabi-gcc')
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    toolchain = args.toolchain
    if toolchain is None and shutil.which('arm-none-eabi-gcc') is None:
        candidate = Path('/workspace/firmware-dev/toolchain/usr/bin')
        if (candidate / 'arm-none-eabi-gcc').exists():
            toolchain = candidate
    if toolchain:
        os.environ['PATH'] = str(toolchain.resolve()) + os.pathsep + os.environ['PATH']
    if shutil.which('arm-none-eabi-gcc') is None:
        parser.error('install ARM GNU toolchain with newlib-nano, or pass --toolchain')
    run(['arm-none-eabi-gcc', '--version'])
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    # Never overwrite an earlier extracted workspace; retain sources for ELF debug.
    workspace = Path(tempfile.mkdtemp(prefix='sources-', dir=build))
    projects = [('Master.zip', 'Master', 'Master'),
                ('relay32 (2).zip', 'relay32 (2)', 'relay32')]
    for archive, original_name, project in projects:
        with zipfile.ZipFile(ROOT / archive) as z:
            if z.testzip() is not None:
                raise RuntimeError(f'corrupt archive: {archive}')
            for member in z.infolist():
                path = Path(member.filename)
                if path.is_absolute() or '..' in path.parts:
                    raise RuntimeError(f'unsafe archive path: {member.filename}')
                if path.parts and path.parts[0] != original_name:
                    raise RuntimeError(f'unexpected archive root: {member.filename}')
                if 'Debug' in path.parts:
                    continue  # Rebuild from source; no archived object files.
                z.extract(member, workspace)
        src = workspace / original_name
        if original_name != project:
            src.rename(workspace / project)
            src = workspace / project
        shutil.copytree(ROOT / 'firmware-overrides' / project, src, dirs_exist_ok=True)
        targets = [(project, None)] if project == 'Master' else [
            (f'relay32-node{i}', i) for i in range(1, 4)]
        for target, node in targets:
            # A separate importable project for each slave prevents default-ID mistakes.
            target_src = src
            if node is not None:
                target_src = workspace / target
                shutil.copytree(src, target_src)
                config = target_src / 'Core/Inc/app_config.h'
                config.write_text(config.read_text().replace('#define NODE_ID 1U',
                                                            f'#define NODE_ID {node}U'))
            out = build / target
            out.mkdir(exist_ok=True)
            flags = ['-std=gnu11', '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft',
                     '-g3', '-O0', '-ffunction-sections', '-fdata-sections', '-Wall',
                     '-fstack-usage', '-DDEBUG', '-DUSE_HAL_DRIVER', '-DSTM32F103xB',
                     '--specs=nano.specs']
            if node is not None:
                flags.append(f'-DNODE_ID={node}')
            for inc in ['Core/Inc', 'Drivers/STM32F1xx_HAL_Driver/Inc',
                        'Drivers/STM32F1xx_HAL_Driver/Inc/Legacy', 'Drivers/CMSIS/Include',
                        'Drivers/CMSIS/Device/ST/STM32F1xx/Include']:
                flags.append('-I' + str(target_src / inc))
            sources = sorted((target_src / 'Core/Src').glob('*.c')) + sorted(
                (target_src / 'Drivers/STM32F1xx_HAL_Driver/Src').glob('*.c')) + sorted(
                (target_src / 'Core/Startup').glob('*.s'))

            def compile_one(source):
                obj = out / (source.stem + '.o')
                # Legacy Cube-generated sysmem.c relied on a transitive include.
                compat = ['-include', 'sys/types.h'] if source.name == 'sysmem.c' else []
                run(['arm-none-eabi-gcc', *flags, *compat, '-c', source, '-o', obj])
                return obj

            print(f'Building {target}', flush=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                objects = list(pool.map(compile_one, sources))
            elf = out / 'firmware.elf'
            run(['arm-none-eabi-gcc', '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft',
                 *objects, '-T' + str(target_src / 'STM32F103C8TX_FLASH.ld'),
                 '--specs=nosys.specs', '--specs=nano.specs', '-Wl,--gc-sections',
                 '-Wl,-Map=' + str(out / 'firmware.map'), '-Wl,--start-group',
                 '-lc', '-lm', '-Wl,--end-group', '-o', elf])
            run(['arm-none-eabi-objcopy', '-O', 'binary', elf, out / 'firmware.bin'])
            run(['arm-none-eabi-objcopy', '-O', 'ihex', elf, out / 'firmware.hex'])
            run(['arm-none-eabi-size', elf])
            with zipfile.ZipFile(out / f'{target}-source.zip', 'w', zipfile.ZIP_DEFLATED) as z:
                for file in sorted(target_src.rglob('*')):
                    if file.is_file():
                        z.write(file, Path(target) / file.relative_to(target_src))
    print(f'All four targets built. Outputs: {build}')


if __name__ == '__main__':
    main()
