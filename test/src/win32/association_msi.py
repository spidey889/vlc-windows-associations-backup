#!/usr/bin/env python3
# Copyright (C) 2026 VideoLAN and its authors
# Copyright (C) 2026 Vinit Rajpurohit (spidey889)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and run the opt-in native MSI association fixture; never install VLC.

On Windows x64, with Python 3, a MinGW C compiler and WiX 3:
  python association_msi.py NEW-OUTPUT-DIRECTORY --wix WIX-BIN [--cc gcc]

Like association_installer.py, this uses the production installer authoring with
a harmless payload. The package cannot be installed. Only costing, conditions,
property changes and the mock association action are invoked, without elevation.
Finish events are interpreted from the compiled MSI table using native MSI APIs;
this is not an interactive wizard or a real Windows-default acceptance test.
Run from an ordinary host shell if a sandbox blocks MSI's child process.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--wix', required=True, type=Path)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source = repo/'extras/package/win32/msi'
output = args.output.resolve()
output.mkdir()  # Never reuse a directory that could contain a real vlc.exe.
payload = output/'payload'
payload.mkdir()
config = output/'extras/package/win32/msi'
config.mkdir(parents=True)

product = (source/'product.wxs').read_text()
product = product.replace('<Icon Id="vlc.ico"',
    '<Condition Message="Test fixture only; installation is disabled.">0</Condition>\n'
    '        <Icon Id="vlc.ico"')
assert 'installation is disabled' in product
(output/'product.wxs').write_text(product)
for name in ['extensions.wxs', 'LICENSE.rtf', 'bannrbmp.bmp']:
    shutil.copyfile(source/name, output/name)
for name in re.findall(r'Source="\$\(var.VlcInstallPrefix\)([^"/\\]+)"', product):
    (payload/name).write_bytes(b'fixture')
shutil.copyfile(repo/'share/icons/vlc.ico', payload/'vlc.ico')
subprocess.run([args.cc, '-Wall', '-Wextra', '-Werror', '-municode',
    str(Path(__file__).with_suffix('.c')), '-o', str(payload/'vlc.exe')], check=True)

text = (source/'config.wxi.in').read_text()
values = dict(VERSION_MAJOR='4', VERSION_MINOR='0', VERSION_REVISION='0',
    VERSION_EXTRA='0', PREFIX_PATH=str(payload)+'\\', HAVE_NLS_TRUE='#',
    BUILD_SKINS_TRUE='#', WINDOWS_ARCH='x64')
for key, value in values.items():
    text = text.replace('@'+key+'@', value)
assert not re.search(r'@\w+@', text)
(config/'config.wxi').write_text(text)
(output/'groups.wxs').write_text(
    '<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi"><Fragment>'
    '<ComponentGroup Id="CompPluginsGroup"/><ComponentGroup Id="CompLuaGroup"/>'
    '</Fragment></Wix>')
subprocess.run([sys.executable, str(source/'msi-associations.py'),
    str(source/'extensions.wxs'), str(config/'associations.wxi')], check=True)
wix = args.wix.resolve()
subprocess.run([str(wix/'candle.exe'), '-nologo', '-arch', 'x64', '-I'+str(output),
    '-ext', str(wix/'WixUtilExtension.dll'), '-out', str(output)+'\\',
    str(output/'product.wxs'), str(output/'extensions.wxs'), str(output/'groups.wxs')], check=True)
subprocess.run([str(wix/'light.exe'), '-nologo', '-sval', '-spdb', '-cultures:en-us',
    '-ext', str(wix/'WixUIExtension.dll'), '-ext', str(wix/'WixUtilExtension.dll'),
    '-out', str(output/'fixture.msi'), *map(str, output.glob('*.wixobj'))], check=True)
subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    str(Path(__file__).with_suffix('.ps1')), str(output)], check=True)
