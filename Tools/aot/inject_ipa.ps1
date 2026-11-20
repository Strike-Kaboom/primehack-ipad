# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputIpa,
    [Parameter(Mandatory)][string]$Framework,
    [Parameter(Mandatory)][string]$OutputIpa,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
# PrimeHack for iPad: resolve a Python 3 interpreter when the launcher is absent.
if (!$Python) {
    foreach ($name in @('py', 'python3', 'python')) {
        foreach ($app in @(Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) {
            if ($app.Source -like '*\Microsoft\WindowsApps\*') { continue }
            $probe = @()
            if ($name -eq 'py') { $probe += '-3' }
            $major = & $app.Source @probe -c 'import sys; print(sys.version_info[0])'
            if ($LASTEXITCODE -eq 0 -and "$major" -eq '3') { $Python = $app.Source; break }
        }
        if ($Python) { break }
    }
    if (!$Python) {
        $candidates = @("$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe",
                        "$env:APPDATA\uv\python\cpython-3.*-windows-x86_64-none\python.exe")
        $found = Get-Item -Path $candidates -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { $Python = $found.FullName }
    }
    if (!$Python) { throw 'Python 3 is required. Supply its full path with -Python.' }
}
$arguments = @()
if ([IO.Path]::GetFileNameWithoutExtension($Python) -eq 'py') { $arguments += '-3' }
$arguments += @('-I', '-B', '-', $InputIpa, $Framework, $OutputIpa)
@'
import copy
import os
from pathlib import Path
import re
import shutil
import sys
import struct
import tempfile
import zipfile

source, framework, output = map(lambda p: Path(p).resolve(), sys.argv[1:])
if output == source or output.exists():
    raise SystemExit("OutputIpa must be a new file")
if framework.name != "PrimeHackAOT.framework":
    raise SystemExit("Expected PrimeHackAOT.framework")
for name in ("Info.plist", "PrimeHackAOT"):
    if not (framework/name).is_file():
        raise SystemExit(f"Missing framework file {name}")

def without_zip64(extra):
    kept = bytearray()
    while extra:
        tag, length = struct.unpack_from("<HH", extra)
        if length + 4 > len(extra):
            raise ValueError("Truncated ZIP extra field")
        if tag != 1:
            kept.extend(extra[:4+length])
        extra = extra[4+length:]
    return bytes(kept)

def copy_range(src, dst, start, end):
    src.seek(start)
    remaining = end - start
    while remaining:
        data = src.read(min(1024*1024, remaining))
        if not data:
            raise ValueError("Truncated ZIP member")
        dst.write(data)
        remaining -= len(data)

output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=".aot-ipa-", dir=output.parent) as tmp:
    staged = Path(tmp)/"output.zip"
    with zipfile.ZipFile(source) as archive, source.open("rb") as raw:
        apps = {m[1] for n in archive.namelist() if (m := re.fullmatch(r"(Payload/[^/]+\.app)/Info\.plist", n))}
        if len(apps) != 1:
            raise SystemExit("Expected one top-level Payload/*.app/Info.plist")
        prefix = apps.pop() + "/Frameworks/PrimeHackAOT.framework/"
        members = sorted(archive.infolist(), key=lambda z: z.header_offset)
        # Preserve local headers, compressed bytes, descriptors, UNIX modes and symlink payloads.
        # zipfile regenerates the central directory with the updated offsets, including ZIP64.
        with zipfile.ZipFile(staged, "w", allowZip64=True) as dest:
            dest.comment = archive.comment
            for i, original in enumerate(members):
                if original.filename == prefix[:-1] or original.filename.startswith(prefix):
                    continue
                info = copy.copy(original)
                info.extra = without_zip64(info.extra)
                info.header_offset = dest.fp.tell()
                end = members[i+1].header_offset if i+1 < len(members) else archive.start_dir
                copy_range(raw, dest.fp, original.header_offset, end)
                dest.filelist.append(info)
                dest.NameToInfo[info.filename] = info
            dest.start_dir = dest.fp.tell()
            for folder in (prefix.rsplit("PrimeHackAOT.framework/", 1)[0], prefix):
                if folder not in dest.NameToInfo:
                    info = zipfile.ZipInfo(folder)
                    info.create_system = 3
                    info.external_attr = (0o40755 << 16) | 0x10
                    dest.writestr(info, b"")
            for path in (framework/"Info.plist", framework/"PrimeHackAOT"):
                if path.is_symlink():
                    raise ValueError(f"Expected a flat iOS framework; found symlink {path.name}")
                if path.is_dir():
                    continue
                name = prefix + path.relative_to(framework).as_posix()
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (0o100755 if path.name == "PrimeHackAOT" else 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with path.open("rb") as data, dest.open(info, "w", force_zip64=path.stat().st_size >= (1<<31)) as member:
                    shutil.copyfileobj(data, member)
    # Windows rename fails if a concurrent operation already created the output.
    if output.exists():
        raise FileExistsError(output)
    staged.rename(output)
print(output)
'@ | & $Python @arguments
if ($LASTEXITCODE) { throw "IPA injection failed ($LASTEXITCODE)" }
