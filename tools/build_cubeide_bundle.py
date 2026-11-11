#!/usr/bin/env python3
"""Create four independent CubeIDE projects and compile Debug/Release with ARM GCC.

The metadata is checked here; CubeIDE itself and an ST-LINK are not required by
this builder. Actual IDE import and hardware programming need the user's machine.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'com.st.stm32cube.ide.mcu.'
TARGETS = [('LoRa_Master', 'Master.zip', 'Master', 'Master', None)] + [
    (f'LoRa_Node{i}', 'relay32 (2).zip', 'relay32 (2)', 'relay32', i) for i in range(1, 4)]


def write_xml(path, tree):
    ET.indent(tree, space='  ')
    data = ET.tostring(tree.getroot(), encoding='UTF-8', xml_declaration=True)
    if path.name == '.cproject':
        # CDT uses this processing instruction to identify its storage format.
        data = data.replace(b'?>', b'?>\n<?fileVersion 4.0.0?>', 1)
    path.write_bytes(data)


def set_option(parent, superclass, value, value_type, name):
    option = next((e for e in parent.findall('option')
                   if e.get('superClass') == superclass), None)
    if option is None:
        option = ET.SubElement(parent, 'option', {
            'id': superclass + '.' + name, 'superClass': superclass,
            'valueType': value_type})
    option.set('value', value)
    return option


def configure_project(project, name, node_id):
    description = ET.parse(project / '.project')
    description.find('name').text = name
    # These customized applications are maintained as C projects. Keep the old
    # CubeMX input separately; generating from it would overwrite application code.
    natures = description.find('natures')
    for nature in list(natures):
        if nature.text == PREFIX + 'MCUCubeProjectNature':
            natures.remove(nature)
    write_xml(project / '.project', description)

    cproject = ET.parse(project / '.cproject')
    ids = {e.get('id'): e.get('id') + '.' + name for e in cproject.iter()
           if e.tag != 'extension' and e.get('id', '').startswith(PREFIX)}
    for e in cproject.iter():
        for key, value in list(e.attrib.items()):
            if value in ids:
                e.set(key, ids[value])
            elif key == 'instanceId':
                e.set(key, ';'.join(ids.get(p, p) for p in value.split(';')))
    for config in cproject.findall('.//configuration'):
        profile = config.get('name')
        chain = config.find('folderInfo/toolChain')
        for option in list(chain.findall('option')):
            if option.get('superClass') == PREFIX + 'option.internal.toolchain.version':
                chain.remove(option)
        for conversion in ['converthex', 'convertbinary']:
            set_option(chain, PREFIX + 'gnu.managedbuild.option.' + conversion,
                       'true', 'boolean', name + '.' + profile)
        builder = chain.find('builder')
        builder.set('buildPath', '${workspace_loc:/' + name + '}/' + profile)
        compiler = next(t for t in chain.findall('tool') if t.get('superClass') ==
                        PREFIX + 'gnu.managedbuild.tool.c.compiler')
        dialect = PREFIX + 'gnu.managedbuild.tool.c.compiler.option.dialect.std'
        set_option(compiler, dialect, dialect + '.value.gnu11',
                   'enumerated', name + '.' + profile)
        if node_id is not None:
            symbols = next(o for o in compiler.findall('option')
                           if o.get('valueType') == 'definedSymbols')
            for symbol in list(symbols):
                if symbol.get('value', '').startswith('NODE_ID='):
                    symbols.remove(symbol)
            ET.SubElement(symbols, 'listOptionValue',
                          {'builtIn': 'false', 'value': f'NODE_ID={node_id}'})
    for element in cproject.findall('.//project'):
        element.set('id', name + '.cubeide.project')
        element.set('name', name)
    write_xml(project / '.cproject', cproject)

    for settings in (project / '.settings').glob('*.xml'):
        tree = ET.parse(settings)
        for e in tree.iter():
            for key, value in list(e.attrib.items()):
                if value in ids:
                    e.set(key, ids[value])
                elif key == 'instanceId':
                    # Scanner discovery embeds configuration/tool IDs separated
                    # by semicolons rather than referencing one ID directly.
                    e.set(key, ';'.join(ids.get(p, p) for p in value.split(';')))
        write_xml(settings, tree)

    debug = next(c.get('id') for c in cproject.findall('.//configuration')
                 if c.get('name') == 'Debug')
    for launch_file in list(project.glob('*.launch')):
        launch = ET.parse(launch_file)
        for e in list(launch.getroot()):
            key = e.get('key', '')
            if key == PREFIX + 'debug.stlink.log_file':
                launch.getroot().remove(e)
            elif key == PREFIX + 'debug.launch.loadList':
                data = json.loads(e.get('value'))
                for item in data['fItems']:
                    item['fProjectName'] = name
                    item['fPath'] = f'Debug/{name}.elf'
                e.set('value', json.dumps(data, separators=(',', ':')))
            elif key == 'org.eclipse.cdt.launch.PROJECT_ATTR':
                e.set('value', name)
            elif key == 'org.eclipse.cdt.launch.PROGRAM_NAME':
                e.set('value', f'Debug/{name}.elf')
            elif key == 'org.eclipse.cdt.launch.PROJECT_BUILD_CONFIG_ID_ATTR':
                e.set('value', debug)
            elif key == 'org.eclipse.debug.core.MAPPED_RESOURCE_PATHS':
                for entry in e:
                    entry.set('value', '/' + name)
            elif key == PREFIX + 'debug.launch.swv_trace_hclk':
                e.set('value', '72000000')
        launch_file.unlink()
        write_xml(project / (name + ' Debug.launch'), launch)

    if node_id is not None:
        config = project / 'Core/Inc/app_config.h'
        text, count = re.subn(r'#define NODE_ID \d+U', f'#define NODE_ID {node_id}U',
                             config.read_text())
        if count != 1:
            raise RuntimeError('expected exactly one NODE_ID default')
        config.write_text(text)
    for ioc in list(project.glob('*.ioc')):
        reference = project / 'Reference'
        reference.mkdir(exist_ok=True)
        ioc.rename(reference / ioc.name)


def prepare_projects(destination):
    destination.mkdir(parents=True, exist_ok=False)
    result = []
    for name, archive, original, overlay, node_id in TARGETS:
        project = destination / name
        project.mkdir()
        with zipfile.ZipFile(ROOT / archive) as z:
            if z.testzip() is not None:
                raise RuntimeError(f'corrupt ZIP: {archive}')
            for item in z.infolist():
                p = Path(item.filename)
                if p.is_absolute() or '..' in p.parts or not p.parts or p.parts[0] != original:
                    raise RuntimeError(f'unsafe ZIP path: {item.filename}')
                if item.is_dir() or 'Debug' in p.parts or 'Release' in p.parts:
                    continue
                target = project.joinpath(*p.parts[1:])
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(z.read(item))
        shutil.copytree(ROOT / 'firmware-overrides' / overlay, project, dirs_exist_ok=True)
        configure_project(project, name, node_id)
        result.append(project)
    return result


def compiler_flags(project, profile):
    tree = ET.parse(project / '.cproject')
    config = next(c for c in tree.findall('.//configuration') if c.get('name') == profile)
    compiler = next(t for t in config.findall('.//tool')
                    if t.get('superClass') == PREFIX + 'gnu.managedbuild.tool.c.compiler')
    flags = ['-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-Wall',
             '-ffunction-sections', '-fdata-sections', '--specs=nano.specs']
    for option in compiler.findall('option'):
        kind = option.get('valueType')
        if kind == 'definedSymbols':
            flags.extend('-D' + e.get('value') for e in option)
        elif kind == 'includePath':
            for entry in option:
                relative = entry.get('value')
                path = (project / profile / relative).resolve()
                if not path.is_dir() or not path.is_relative_to(project.resolve()):
                    raise RuntimeError(f'bad include path: {relative}')
                flags.append('-I' + str(path))
        elif option.get('superClass', '').endswith('.dialect.std'):
            flags.append('-std=' + option.get('value').rsplit('.', 1)[1])
    if '-std=gnu11' not in flags:
        raise RuntimeError('GNU11 must be explicitly selected')
    flags += ['-g3', '-O0'] if profile == 'Debug' else ['-g0', '-Os']
    return flags


def build_project(project, profile, output, jobs):
    output.mkdir(parents=True)
    flags = compiler_flags(project, profile)
    sources = sorted((project / 'Core/Src').glob('*.c')) + sorted(
        (project / 'Drivers/STM32F1xx_HAL_Driver/Src').glob('*.c')) + sorted(
        (project / 'Core/Startup').glob('*.s'))
    commands = []

    def compile_one(source):
        obj = output / (source.stem + '.o')
        cmd = ['arm-none-eabi-gcc', *flags, '-c', str(source), '-o', str(obj)]
        subprocess.run(cmd, check=True)
        return obj, cmd

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        compiled = list(pool.map(compile_one, sources))
    objects = [str(obj) for obj, cmd in compiled]
    commands.extend(cmd for obj, cmd in compiled)
    elf = output / (project.name + '.elf')
    link = ['arm-none-eabi-gcc', '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft',
            *objects, '-T' + str(project / 'STM32F103C8TX_FLASH.ld'),
            '--specs=nosys.specs', '--specs=nano.specs', '-Wl,--gc-sections',
            '-Wl,-Map=' + str(output / (project.name + '.map')),
            '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group', '-o', str(elf)]
    subprocess.run(link, check=True)
    commands.append(link)
    for extension, fmt in [('hex', 'ihex'), ('bin', 'binary')]:
        subprocess.run(['arm-none-eabi-objcopy', '-O', fmt, str(elf),
                        str(output / (project.name + '.' + extension))], check=True)
    subprocess.run(['arm-none-eabi-size', str(elf)], check=True)
    (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    toolchain = args.toolchain
    if toolchain is None and shutil.which('arm-none-eabi-gcc') is None:
        toolchain = Path('/workspace/firmware-dev/toolchain/usr/bin')
    if toolchain:
        os.environ['PATH'] = str(toolchain.resolve()) + os.pathsep + os.environ['PATH']
    if shutil.which('arm-none-eabi-gcc') is None:
        parser.error('ARM GCC/newlib-nano required; pass --toolchain /path/to/bin')
    version = subprocess.check_output(['arm-none-eabi-gcc', '--version'], text=True).splitlines()[0]
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='cubeide-', dir=build))
    bundle = work / 'LoRa-STM32CubeIDE'
    projects = prepare_projects(bundle / 'Projects')
    shutil.copyfile(ROOT / 'CUBEIDE_README.md', bundle / 'README.md')
    shutil.copyfile(ROOT / 'LORA_DIAGNOSIS.md', bundle / 'LORA_DIAGNOSIS.md')
    validation = {'toolchain': version, 'cubeide_gui_tested': False,
                  'hardware_tested': False, 'builds': []}
    for project in projects:
        for profile in ['Debug', 'Release']:
            print('Building', project.name, profile, flush=True)
            output = work / 'validation' / project.name / profile
            build_project(project, profile, output, args.jobs)
            validation['builds'].append({'project': project.name, 'configuration': profile,
                                         'status': 'passed'})
            if profile == 'Debug':
                prebuilt = bundle / 'Prebuilt' / project.name
                prebuilt.mkdir(parents=True)
                for extension in ['hex', 'bin']:
                    file = output / (project.name + '.' + extension)
                    shutil.copyfile(file, prebuilt / file.name)
    (bundle / 'VALIDATION.json').write_text(json.dumps(validation, indent=2) + '\n')
    archive = ROOT / 'downloads/LoRa-STM32CubeIDE.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for file in sorted(bundle.rglob('*')):
            if file.is_file():
                z.write(file, file.relative_to(work))
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    print('Validated eight builds. Bundle:', archive, 'SHA256:', checksum, flush=True)
    print('Sources and command logs retained in:', work)


if __name__ == '__main__':
    main()
