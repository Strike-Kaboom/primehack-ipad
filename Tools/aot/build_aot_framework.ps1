# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GeneratedDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$LlvmBin = "$env:ProgramFiles\LLVM\bin"
)
$ErrorActionPreference = 'Stop'
$gen = (Resolve-Path -LiteralPath $GeneratedDir).Path
$dest = [IO.Path]::GetFullPath($OutputDir)
$include = (Resolve-Path (Join-Path $PSScriptRoot '../../Source/Core/Core/PowerPC/Aot')).Path
function Find-Tool([string]$name) {
    $path = Join-Path $LlvmBin "$name.exe"
    if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    $tool = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
    if (!$tool) { throw "Missing LLVM tool $name (looked in $LlvmBin and PATH)" }
    return $tool.Source
}
$clang = Find-Tool 'clang++'
$linker = Find-Tool 'ld64.lld'
$nm = Find-Tool 'llvm-nm'
$objdump = Find-Tool 'llvm-objdump'
$sources = @(Get-Content -LiteralPath (Join-Path $gen 'aot_sources.txt'))
if (!$sources.Count -or $sources[-1] -ne 'aot_registry.cpp') { throw 'Incomplete source manifest' }
[IO.Directory]::CreateDirectory($dest) | Out-Null
$stage = Join-Path $dest ('.aot-build-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($stage) | Out-Null
$tempBefore = $env:TEMP
$tmpBefore = $env:TMP
try {
    $keepRecovery = $false
    $env:TEMP = $env:TMP = $stage
    # PrimeHack for iPad: freestanding libcalls and a clean, replaceable framework.
    $support = @'
using size_type = __SIZE_TYPE__;
extern "C" void* memset(void* dst, int value, size_type count)
{
  auto* out = static_cast<volatile unsigned char*>(dst);
  for (size_type i = 0; i < count; ++i) out[i] = static_cast<unsigned char>(value);
  return dst;
}
extern "C" void* memcpy(void* dst, const void* src, size_type count)
{
  auto* out = static_cast<volatile unsigned char*>(dst);
  const auto* in = static_cast<const unsigned char*>(src);
  for (size_type i = 0; i < count; ++i) out[i] = in[i];
  return dst;
}
extern "C" void* memmove(void* dst, const void* src, size_type count)
{
  auto* out = static_cast<volatile unsigned char*>(dst);
  const auto* in = static_cast<const unsigned char*>(src);
  if (reinterpret_cast<__UINTPTR_TYPE__>(dst) <= reinterpret_cast<__UINTPTR_TYPE__>(src))
    for (size_type i = 0; i < count; ++i) out[i] = in[i];
  else
    while (count) { --count; out[count] = in[count]; }
  return dst;
}
extern "C" void memset_pattern16(void* dst, const void* pattern, size_type count)
{
  auto* out = static_cast<volatile unsigned char*>(dst);
  const auto* bytes = static_cast<const unsigned char*>(pattern);
  for (size_type i = 0; i < count; ++i) out[i] = bytes[i & 15];
}
extern "C" void bzero(void* dst, size_type count) { memset(dst, 0, count); }

'@
    [IO.File]::WriteAllText((Join-Path $stage 'support.cpp'), $support)
    $flags = @('-target', 'arm64-apple-ios16.0', '-mcpu=apple-m4', '-O3', '-std=c++20',
        '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-ffp-contract=off',
        '-fno-fast-math', '-frounding-math', '-fno-math-errno', '-fno-strict-aliasing',
        '-fvisibility=hidden', '-fno-stack-protector', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables', '-I', $include)
    $objects = foreach ($name in @($sources) + 'support.cpp') {
        if ($name -eq 'support.cpp') { $src = Join-Path $stage $name }
        elseif ($name -cmatch '^aot_(shard_[0-9]+|registry)\.cpp$') { $src = Join-Path $gen $name }
        else { throw "Invalid source in manifest: $name" }
        $obj = Join-Path $stage ($name + '.o')
        & $clang @flags -c $src -o $obj
        if ($LASTEXITCODE) { throw "Compilation failed for $name" }
        '"' + $obj.Replace('\', '/') + '"'
    }
    $rsp = Join-Path $stage 'objects.rsp'
    [IO.File]::WriteAllLines($rsp, [string[]]$objects, [Text.UTF8Encoding]::new($false))
    $binary = Join-Path $stage 'PrimeHackAOT'
    $id = '@rpath/PrimeHackAOT.framework/PrimeHackAOT'
    & $linker -dylib -arch arm64 -platform_version ios 16.0 26.0 -install_name $id -undefined error -exported_symbol _PHAot_GetModule "@$rsp" -o $binary
    if ($LASTEXITCODE) { throw 'Mach-O link failed' }
    $undef = @(& $nm --undefined-only $binary)
    if ($LASTEXITCODE -or $undef.Count) { throw "Unresolved symbols: $undef" }
    $exports = @(& $nm --extern-only --defined-only --just-symbol-name $binary)
    if ($LASTEXITCODE -or $exports.Count -ne 1 -or $exports[0].Trim() -cne '_PHAot_GetModule') {
        throw "Unexpected Mach-O exports: $exports"
    }
    $headers = (& $objdump --macho --private-headers $binary) -join [Environment]::NewLine
    if ($LASTEXITCODE -or !$headers.Contains($id) -or !$headers.Contains('LC_BUILD_VERSION') -or
        $headers -notmatch '(?im)^\s*minos\s+16\.0\b') { throw 'Mach-O load commands failed inspection' }
    $plist = @'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleIdentifier</key><string>org.dolphin-emu.PrimeHackAOT</string>
<key>CFBundleExecutable</key><string>PrimeHackAOT</string>
<key>CFBundleName</key><string>PrimeHackAOT</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>MinimumOSVersion</key><string>16.0</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
</dict></plist>
'@
    $framework = Join-Path $stage 'PrimeHackAOT.framework'
    [IO.Directory]::CreateDirectory($framework) | Out-Null
    [IO.File]::WriteAllText((Join-Path $framework 'Info.plist'), $plist, [Text.UTF8Encoding]::new($false))
    [IO.File]::Copy($binary, (Join-Path $framework 'PrimeHackAOT'), $true)
    $stats = Join-Path $gen 'aot_stats.json'
    # Share only counts. Reject malformed, extra or free-text fields before installing the build.
    $summary = Get-Content -LiteralPath $stats -Raw | ConvertFrom-Json
    function Assert-Count($value) {
        if (($value -isnot [long] -and $value -isnot [int]) -or $value -lt 0) {
            throw 'Stats contain an invalid count'
        }
    }
    $keys = @($summary.PSObject.Properties.Name | Sort-Object)
    if (($keys -join ',') -cne 'entries,opcodes,region_length_histogram,regions') {
        throw 'Unexpected stats fields'
    }
    Assert-Count $summary.regions
    Assert-Count $summary.entries
    foreach ($op in $summary.opcodes.PSObject.Properties) {
        if ($op.Name -cnotmatch '^[a-z][a-z0-9_]{0,31}$' -or
            (@($op.Value.PSObject.Properties.Name | Sort-Object) -join ',') -cne 'fallback,lifted') {
            throw 'Invalid opcode stats'
        }
        Assert-Count $op.Value.lifted
        Assert-Count $op.Value.fallback
    }
    foreach ($bin in $summary.region_length_histogram.PSObject.Properties) {
        if ($bin.Name -notmatch '^[1-9][0-9]*$') { throw 'Invalid region length' }
        Assert-Count $bin.Value
    }
    $stagedStats = Join-Path $stage 'aot_stats.json'
    [IO.File]::WriteAllText($stagedStats, ($summary | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    $final = Join-Path $dest 'PrimeHackAOT.framework'
    $backup = Join-Path $stage 'previous.framework'
    $statsPath = Join-Path $dest 'aot_stats.json'
    # Serialize the short publication step. File.Replace preserves the old stats if replacement fails.
    $lock = [IO.FileStream]::new((Join-Path $dest '.aot-publish.lock'), [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 1, [IO.FileOptions]::DeleteOnClose)
    try {
        if ([IO.Directory]::Exists($final)) { [IO.Directory]::Move($final, $backup) }
        try {
            [IO.Directory]::Move($framework, $final)
            if ([IO.File]::Exists($statsPath)) {
                [IO.File]::Replace($stagedStats, $statsPath, (Join-Path $stage 'previous_stats.json'))
            } else {
                [IO.File]::Move($stagedStats, $statsPath)
            }
        } catch {
            try {
                if (![IO.Directory]::Exists($framework) -and [IO.Directory]::Exists($final)) {
                    [IO.Directory]::Move($final, $framework)
                }
                if ([IO.Directory]::Exists($backup)) { [IO.Directory]::Move($backup, $final) }
            } catch {
                $keepRecovery = $true
                throw "Framework rollback failed. Recovery files are in $stage. $_"
            }
            throw
        }
    } finally { $lock.Dispose() }
    Write-Output "$final (arm64 iOS, sole export _PHAot_GetModule, zero undefined symbols)"
} finally {
    $env:TEMP = $tempBefore
    $env:TMP = $tmpBefore
    if (!$keepRecovery -and [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($stage)) -eq $dest.TrimEnd('\','/') -and
        [IO.Path]::GetFileName($stage) -match '^\.aot-build-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
