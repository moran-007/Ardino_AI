$ErrorActionPreference = 'Stop'
$testOutput = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $testOutput -Force
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../offline_wake.cpp') -Raw
$options = [regex]::Match($source, '(?s)constexpr ModelOption MODEL_OPTIONS\[\] = \{.*?\n\};').Value
$lookup = [regex]::Match($source, '(?s)const ModelOption \*findOption\(const String &id\) \{.*?\n\}').Value
if (-not $options -or -not $lookup) { throw 'Could not read existing WakeNet model lookup.' }
$code = @"
#include <string>
#include <cstdio>
using String = std::string;
struct ModelOption { const char *id; const char *label; };
$options
$lookup
int main() {
  if (!findOption("wn9_nihaoxiaozhi_tts")) return 2;
  if (!findOption("小默小默")) {
    std::puts("RED: current real wake-model lookup rejects arbitrary user wake label 小默小默; no enrollment exists.");
    return 1;
  }
  return 0;
}
"@
$file = Join-Path $testOutput 'baseline.cpp'
[IO.File]::WriteAllText($file, $code, [Text.UTF8Encoding]::new($false))
& 'D:/msys64/ucrt64/bin/g++.exe' '-std=c++17' $file '-o' (Join-Path $testOutput 'baseline.exe')
if ($LASTEXITCODE -ne 0) { throw 'Baseline fixture compilation failed.' }
& (Join-Path $testOutput 'baseline.exe')
if ($LASTEXITCODE -ne 1) { throw 'Expected meaningful baseline rejection was not observed.' }
