# build_android.ps1 — кросс-сборка под Android с Windows-хоста.
# На самом устройстве/в Linux пользуйтесь Makefile — этот скрипт нужен только
# потому, что GNU make на Windows обычно нет.
#
#   .\build_android.ps1                      # все ABI, бинарник + libamplitude.so
#   .\build_android.ps1 -Abi arm64-v8a       # только одна архитектура
#   .\build_android.ps1 -Ndk D:\android-ndk -Api 31

param(
    [string]   $Ndk = 'F:\Projects\Toolchains\AndroidNDK',
    [int]      $Api = 31,
    [string[]] $Abi = @('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin  = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
if (-not (Test-Path $bin)) { throw "NDK не найден: $bin" }

$triples = @{
    'arm64-v8a'   = 'aarch64-linux-android'
    'armeabi-v7a' = 'armv7a-linux-androideabi'
    'x86_64'      = 'x86_64-linux-android'
    'x86'         = 'i686-linux-android'
}
$abiFlags = @{
    'arm64-v8a'   = @()
    'armeabi-v7a' = @('-march=armv7-a', '-mfpu=neon', '-mfloat-abi=softfp')
    'x86_64'      = @('-mssse3', '-msse4.1')
    'x86'         = @('-mssse3', '-msse4.1')
}

$core = @(
    "$root\src\decoder_open.cpp",
    "$root\src\decoder_minimp3.cpp",
    "$root\src\decoder_mediandk.cpp"
)
$common = @('-O3', '-std=c++11', '-Wall', '-Wextra', '-fno-rtti',
            '-ffunction-sections', '-fdata-sections', "-I$root\src")
$link   = @('-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-lmediandk', '-lm')

$failed = @()
foreach ($a in $Abi) {
    $cxx = Join-Path $bin "$($triples[$a])$Api-clang++.cmd"
    if (-not (Test-Path $cxx)) { throw "нет компилятора для $a ($cxx)" }
    $out = Join-Path $root "build\$a"
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    # 1. CLI-бинарник (исключения не нужны — экономим размер)
    & $cxx @common @($abiFlags[$a]) '-fno-exceptions' '-fPIE' `
        "$root\src\amplitude.cpp" @core `
        '-o' "$out\amplitude" '-pie' '-static-libstdc++' @link
    if ($LASTEXITCODE -ne 0) { $failed += "$a/бинарник"; continue }

    # 2. libamplitude.so для JNI (исключения нужны: bad_alloc -> OutOfMemoryError)
    & $cxx @common @($abiFlags[$a]) '-fPIC' '-shared' `
        "$root\android\amplitude_jni.cpp" @core `
        '-o' "$out\libamplitude.so" '-static-libstdc++' @link
    if ($LASTEXITCODE -ne 0) { $failed += "$a/so"; continue }

    $b = (Get-Item "$out\amplitude").Length
    $s = (Get-Item "$out\libamplitude.so").Length
    "{0,-13} amplitude {1,8} Б   libamplitude.so {2,8} Б" -f $a, $b, $s
}

if ($failed.Count) { throw "не собралось: $($failed -join ', ')" }
"`nГотово. Артефакты в $root\build\<abi>\"
