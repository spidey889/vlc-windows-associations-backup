#!/usr/bin/env python3
# Copyright (C) 2026 Vinit Rajpurohit (spidey889)
# SPDX-License-Identifier: GPL-2.0-or-later

"""Build harmless native tests for the actual NSIS adapter; never run VLC.

Usage: python3 association_installer.py /path/to/test-output [win64|amd64]
Compile only. Run association-installer-test.exe on Windows without /S overrides;
it is asInvoker, has no pages/payload, and writes results beside itself. The only
real system calls are a read-only elevation query and process-local registry view
selection. External reads, registration and ExecWait are mocked before compiling.
Run from an ordinary host shell if an AppContainer sandbox rejects the NSIS
fixed-base stub; do not change Windows mitigation or UAC settings.
"""
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

def registry_arguments(arguments):
    # NSIS quoted arguments may contain whitespace and $\\" escapes.
    return re.findall(r'"(?:\$\\.|[^"\n])*"|\'[^\'\n]*\'|[^\s;]+', arguments)


def check_machine_class_registration(source, name):
    # HKCR merges the elevated account's classes. All installer-owned handlers
    # and their cleanup must address the machine hive even with per-user shadows.
    for operation, arguments in re.findall(
            r'^\s*((?:Read|Write|Delete)Reg\w+) ([^;\n]+)', source, re.M):
        tokens = registry_arguments(arguments)
        if operation.startswith('Read'):
            tokens = tokens[1:]
        if tokens[0].startswith('/'):
            tokens = tokens[1:]
        root, key = tokens[:2]
        key = key.strip('"\'')
        if root == 'HKCR' or key.startswith(('Software\\Classes\\', 'VLC',
                'Applications\\vlc.exe', 'AudioCD\\', 'DVD\\', '$R0', '${EXT}')):
            assert root == 'HKLM' and key.startswith('Software\\Classes\\'), \
                f'{name}: class registration/cleanup must use HKLM Software\\Classes: {operation} {arguments}'


repo = Path(__file__).resolve().parents[3]
nsis = repo/'extras/package/win32/NSIS'
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
helper = (nsis/'helpers/associations.nsh').read_text()
assert helper.count('  ExecWait ') == 1
helper = helper.replace('  ExecWait ', '  StrCpy $MockCommand ')
helper = helper.replace("$AssociationExtensions' $1", "$AssociationExtensions'\n  Call MockExecWait")
assert '  ExecWait ' not in helper
helper = helper.replace('  Call AssociationCommandIsElevated', '  StrCpy $0 $MockElevated')
helper = re.sub(r'  DetailPrint (.*)', r'  StrCpy $MockDetail \1', helper)
markers = [f'  ReadRegStr $0 HKLM "${{PRODUCT_DIR_REGKEY}}" "{name}"'
           for name in ['InstallDir','Version']]
markers += ['  ReadRegStr $0 ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString"']
for marker in markers:
    assert helper.count(marker) == 1
    helper = helper.replace(marker, '  Call MockReadMarker')
helper = helper.replace('  IfFileExists "$INSTDIR\\vlc.exe" 0 association_registration_failed',
    '  ${If} $RegistrationFailure == 1\n    Goto association_registration_failed\n  ${EndIf}')
for index,line in enumerate(['  ReadRegStr $0 HKLM "${PRODUCT_DIR_REGKEY}" ""',
    '  ReadRegStr $0 HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\vlc.exe" ""'],2):
    assert helper.count(line) == 1
    helper = helper.replace(line,
        f'  StrCpy $0 "$INSTDIR\\vlc.exe"\n  ${{If}} $RegistrationFailure == {index}\n'
        f'    StrCpy $0 "wrong executable"\n  ${{ElseIf}} $RegistrationFailure == {index+2}\n'
        '    SetErrors\n  ${EndIf}')
assert not re.search(r'^\s*(ExecWait|ReadReg|WriteReg|DeleteReg)',helper,re.M)
(output/'mock-associations.nsh').write_text(helper)

extensions = (nsis/'helpers/extensions.nsh').read_text()
installer = (nsis/'vlc.win32.nsi.in').read_text()
for name, source in [('extensions.nsh', extensions), ('vlc.win32.nsi.in', installer)]:
    check_machine_class_registration(source, name)
print('Machine-wide class registration and cleanup checks passed')
msi = nsis.parent/'msi'
compatibility = output/'msi-compat.nsh'
subprocess.run([sys.executable, str(msi/'msi-associations.py'), str(msi/'extensions.wxs'),
                str(compatibility), '--nsis-extensions', str(nsis/'helpers/extensions.nsh')],
               check=True)
compatibility_source = compatibility.read_text()
assert not re.search(r'^\s*(ReadReg|WriteReg|DeleteReg|Exec|File)', compatibility_source, re.M)

# Exercise the real helper's relative generated include as staged by both builds.
# -PPO only preprocesses; the unmocked registration instructions are never run.
helper_stage = output/'helpers'
helper_stage.mkdir(exist_ok=True)
(helper_stage/'extensions.nsh').write_text(extensions)
(helper_stage/'msi-compat.nsh').write_text(compatibility_source)
result = subprocess.run(['makensis', '-NOCD', '-PPO', '-'], cwd=output,
                        input='!include LogicLib.nsh\n!include WinVer.nsh\n!include "helpers/extensions.nsh"\n',
                        text=True, capture_output=True)
if result.returncode:
    print(result.stderr)
    raise SystemExit(result.returncode)
print('Staged NSIS generated include preprocessing passed')

# Preprocess the real checkbox call with both build flags, not a mirrored guard.
skin_ui = '\n'.join(line for line in installer.splitlines()
                    if '!insertmacro MacroSkinExtensions AssociateSkinExtensionSection' in line)
assert skin_ui, 'Missing skin association checkbox call'
skin_macro = re.search(r'!macro MacroSkinExtensions _action\n.*?!macroend', extensions, re.S)[0]
skin_extensions = re.findall(r'!insertmacro \$\{_action\} \w+ "([^"\n]+)"', skin_macro)
for flag, expected in [('', skin_extensions), ('#', [])]:
    source = ('!macro AssociateSkinExtensionSection TYPE EXT\n'
              'Section "${EXT}"\nSectionEnd\n!macroend\n' + skin_macro + '\n' +
              skin_ui.replace('@BUILD_SKINS_TRUE@', flag) + '\n')
    result = subprocess.run(['makensis', '-NOCD', '-PPO', '-'], input=source,
                            text=True, capture_output=True, check=True)
    visible = re.findall(r'^Section "([^"\n]+)"$', result.stdout, re.M)
    assert visible == expected, f'Skin association checkboxes: expected {expected}, got {visible}'
print('Skin association checkbox preprocessing passed: skins enabled and disabled')
start = extensions.index('!macro AssociateExtensionSection')
end = extensions.index('!macro RegisterExtensionMacro')
sections = extensions[start:end].replace('Call AssociateExtension', 'Call MockAssociateExtension')
assert not re.search(r'^\s*(WriteReg|DeleteReg|Exec|RegDLL|UnRegDLL)',sections,re.M)

# Capture the actual registration/cleanup operations in memory, including their
# conditional branches. Never substitute a second copy of the compatibility list.
def mock_registry_operation(match):
    operation, arguments = match.groups()
    tokens = registry_arguments(arguments)
    if operation == 'ReadRegStr':
        assert len(tokens) == 4
        target, *tokens = tokens
        tail = '\n  Pop ' + target
    else:
        assert len(tokens) == {'WriteRegStr': 4, 'DeleteRegKey': 2, 'DeleteRegValue': 3}[operation]
        tail = ''
    return ''.join('  Push ' + token + '\n' for token in tokens) + \
        '  Call Mock' + operation + tail

registration = compatibility_source + '\n'
for original, mocked in [('RegisterExtension', 'TestRegisterExtension'),
                          ('RegisterSkinExtension', 'TestRegisterSkinExtension'),
                         ('un.RegisterExtension', 'TestUnRegisterExtension')]:
    body = re.search(r'Function ' + re.escape(original) + r'\n(.*?)FunctionEnd',
                     extensions, re.S)[1]
    body = re.sub(r'(?m)^[ \t]+(WriteRegStr|ReadRegStr|DeleteRegKey|DeleteRegValue) ([^;\n]+)(?:;[^\n]*)?$',
                  mock_registry_operation, body)
    assert not re.search(r'^\s*(ReadReg|WriteReg|DeleteReg|Exec)', body, re.M)
    registration += 'Function ' + mocked + '\n' + body + 'FunctionEnd\n'

# Use the production comparator: mocked results missed real multi-view upgrade
# eligibility interactions. This function has no registry/process operations.
utils = (nsis/'helpers/utils.nsh').read_text()
compare = re.search(r'Function VersionCompare\n.*?FunctionEnd', utils, re.S)[0] + '\n'
assert not re.search(r'^\s*(ReadReg|WriteReg|DeleteReg|Exec)', compare, re.M)
registration += compare


# Reuse the real copy-error guards, mocking only File extraction. The rest of
# the installer logging is intentionally outside these focused guard tests.
# Allow comments/blank lines before the error check, but no instructions.
copy_source = (nsis/'helpers/install.nsh').read_text()
copy_guards = re.findall(r'  ClearErrors\n(?:  SetOutPath [^\n]+\n)?  File [^\n]+\n'
                        r'(?:[ \t]*(?:;[^\n]*)?\n)*  \$\{If\} \$\{Errors\}\n'
                        r'    StrCpy \$AssociationSkipDefaults 1\n  \$\{EndIf\}\n',copy_source)
assert len(copy_guards)==3
copy_tests = ''
for index,guard in enumerate(copy_guards):
    guard = re.sub(r'^  SetOutPath .*$',
                   '  ${{If}} $MockPathError == 1\n    SetErrors\n  ${{EndIf}}',guard,flags=re.M)
    guard = re.sub(r'^  File .*$', '  ${{If}} $MockCopyError == 1\n    SetErrors\n  ${{EndIf}}',guard,flags=re.M)
    guard = guard.replace('${{','${').replace('}}','}')
    copy_tests += f'!macro TestCopyGuard{index}\n'+guard+'!macroend\n'

start = extensions.index('!macro MacroAudioExtensions')
end = extensions.index('; Generic function for adding the context menu')
inventory = extensions[start:end]
assert sections.count('Call AppendAssociationExtension') == 3
# SectionGroup begin/end count as NSIS sections too; the assertion section is last.
count = len(re.findall(r'!insertmacro \$\{_action\} \w+ "',inventory)) + 6
all_types = []
for name in ['Audio','Video','Other','Skin','Unassociated']:
    macro = re.search(r'!macro Macro'+name+r'Extensions _action(.*?)!macroend',inventory,re.S)
    assert macro
    all_types += re.findall(r'!insertmacro \$\{_action\} (\w+) "([^"\n]+)"',macro[1])
all_extensions = [extension for _, extension in all_types]
assert len(all_extensions) == count - 6

# Derive expected coverage independently from the two actual inventories. The
# compiled fixture executes the production predicate for every positive/negative.
msi_ids = {item.get('Id') for item in ET.parse(msi/'extensions.wxs').getroot().iter()
           if item.tag.endswith('}ProgId')}
shared = {extension for _, extension in all_types if 'VLC'+extension+'.Document' in msi_ids}
assert '.mid' in shared and '.bin' not in shared
compatibility_checks = '!macro CheckAllMsiCompatibility\n'
for group, extension in all_types:
    if extension in shared:
        compatibility_checks += '  !insertmacro CheckMsiCompatibility %s "%s" "%s"\n' % \
                                (group, extension, extension[1:].upper())
    else:
        compatibility_checks += '  !insertmacro CheckNoMsiCompatibility "%s"\n' % extension
for extension in ['.bin', '.dsf-other']:
    compatibility_checks += '  !insertmacro CheckNoMsiCompatibility "%s"\n' % extension
compatibility_checks += '!macroend\n'
registration += compatibility_checks
print('Compatibility coverage: %d shared types; .mid retained, .bin excluded' % len(shared))
(output/'mock-extensions.nsh').write_text(registration+copy_tests+sections+inventory+
    f'\n!define __ASSOCIATION_SECTION_COUNT {count}\n'+
    f'!define __ASSOCIATION_EXTENSION_COUNT {len(all_extensions)}\n'+
    '!define __ASSOCIATION_ALL_EXTENSIONS "'+','.join(all_extensions)+'"\n')
fixture = Path(__file__).with_suffix('.nsi')
args = ['makensis','-NOCD','-V3','-DASSOCIATION_TEST_HELPER='+str(output/'mock-associations.nsh'),
        '-DASSOCIATION_TEST_EXTENSIONS='+str(output/'mock-extensions.nsh'),str(fixture)]
if len(sys.argv) > 2:
    assert sys.argv[2] in ['win64','amd64']
    args.insert(1, '-DVLC_64BIT_INSTALLER')
    if sys.argv[2] == 'amd64':
        args.insert(1, '-XTarget amd64-unicode')
# win64 deliberately retains the x86 NSIS stub used by the real VLC installer.
with (output/'compile.log').open('w') as log:
    result = subprocess.run(args,cwd=output,stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'compile.log').read_text()[-5000:])
    raise SystemExit(result.returncode)
print('Harmless NSIS adapter fixture compiled: '+str(output/'association-installer-test.exe'))
