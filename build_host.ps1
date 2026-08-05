# build_host.ps1 — сборка и прогон на Windows-хосте (MSVC), только MP3-путь.
# GNU make под Windows обычно нет, поэтому Makefile здесь не годится.
#
#   .\build_host.ps1            # собрать всё и прогнать юнит-тесты
#   .\build_host.ps1 -Tests     # только юнит-тесты
#   .\build_host.ps1 -Func      # юнит-тесты + функциональные (нужны фикстуры)

param([switch]$Tests, [switch]$Func)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $build | Out-Null

# vcvars64.bat зовёт vswhere без пути, поэтому каталог установщика нужен в PATH.
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
if (-not (Test-Path (Join-Path $installer 'vswhere.exe'))) {
    throw "не найден vswhere.exe в $installer — нужны Visual Studio Build Tools"
}
$env:PATH = "$installer;$env:PATH"
$vsPath = & (Join-Path $installer 'vswhere.exe') -latest -products * -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "не найден vcvars64.bat: $vcvars" }

function Invoke-Cl([string]$what, [string]$cmdline) {
    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $cmdline"
    if ($LASTEXITCODE -ne 0) { throw "не собралось: $what" }
}

function Invoke-Test([string]$exe) {
    & (Join-Path $build $exe)
    if ($LASTEXITCODE -ne 0) { throw "тесты провалены: $exe" }
}

Invoke-Cl 'test_envelope' 'cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /EHsc /I src /Fe:build\test_envelope.exe /Fo:build\ tests\test_envelope.cpp'
Invoke-Cl 'test_sniff'    'cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /EHsc /I src /Fe:build\test_sniff.exe    /Fo:build\ tests\test_sniff.cpp'
Invoke-Test 'test_envelope.exe'
Invoke-Test 'test_sniff.exe'

if ($Tests) { exit 0 }

Invoke-Cl 'amplitude.exe' ('cl /nologo /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /EHsc /I src /Fe:build\amplitude.exe /Fo:build\ ' +
                           'src\amplitude.cpp src\decoder_open.cpp src\decoder_minimp3.cpp src\decoder_mediandk.cpp')
Write-Host "OK: build\amplitude.exe"

if ($Func) {
    # python3 на Windows обычно оказывается заглушкой Microsoft Store: она есть
    # в PATH, печатает «Python» и возвращает 9009. Берём первый интерпретатор,
    # который действительно отвечает на --version.
    $py = $null
    foreach ($cand in 'python3', 'python', 'py') {
        if (-not (Get-Command $cand -ErrorAction SilentlyContinue)) { continue }
        # Перенаправление делает cmd: если гасить stderr средствами PowerShell,
        # вывод заглушки станет ошибкой и утащит за собой весь скрипт.
        cmd /c "$cand --version >nul 2>&1"
        if ($LASTEXITCODE -eq 0) { $py = $cand; break }
    }
    if (-not $py) { throw 'не найден рабочий python — функциональные тесты не запустить' }

    & $py tests\run_tests.py build\amplitude.exe
    if ($LASTEXITCODE -ne 0) { throw 'функциональные тесты провалены' }
}
