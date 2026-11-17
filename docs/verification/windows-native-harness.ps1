$ErrorActionPreference = 'Stop'
$serial = $null
$results = @()
$directory = Join-Path $env:SystemDrive 'libipc-tests'
New-Item -ItemType Directory -Force $directory | Out-Null
$log = Join-Path $directory 'results.log'
function Report([string]$line) {
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    if ($null -ne $script:serial) { $script:serial.WriteLine($line) }
}
foreach ($port in @('COM2','COM1')) {
    try {
        $serial = New-Object System.IO.Ports.SerialPort $port,115200,'None',8,'One'
        $serial.Open()
        break
    } catch {
        Add-Content -LiteralPath $log -Value $_.ToString()
        $serial = $null
    }
}
try {
    Report 'LIBIPC_NATIVE_BEGIN'
    $version = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Report ($version.ProductName + '; build=' + $version.CurrentBuildNumber + '.' + $version.UBR)
    Report ('Kernel=' + [System.Environment]::OSVersion.Version.ToString())
    $bin = Join-Path $directory 'bin'
    New-Item -ItemType Directory -Force $bin | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ipc_echo.exe') -Destination $bin
    Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.exe' | Copy-Item -Destination $bin -Force
    Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.dll' | Copy-Item -Destination $bin -Force
    $env:PATH = $bin + ';' + $env:PATH
    $peer = Join-Path $env:SystemDrive 'home\lily\code\libipc\build-win'
    New-Item -ItemType Directory -Force $peer | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin 'ipc_echo.exe') -Destination $peer
    & subst.exe Z: ($env:SystemDrive + '\')
    Start-Sleep -Seconds 20
    function Run-Test([string]$name, [string[]]$arguments) {
        Report ('TEST_BEGIN ' + $name + ' ' + ($arguments -join ' '))
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo.FileName = Join-Path $bin $name
        $process.StartInfo.WorkingDirectory = $bin
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.Arguments = (($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
        [void]$process.Start()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $code = 124
        if ($process.WaitForExit(30000)) { $code = $process.ExitCode }
        else { $process.Kill(); $process.WaitForExit() }
        Report $output.GetAwaiter().GetResult()
        Report $errors.GetAwaiter().GetResult()
        Report ('TEST_END ' + $name + ' exit=' + $code)
        $script:results += @{name=$name; arguments=$arguments; exit_code=$code}
        $process.Dispose()
    }
    Run-Test 'ipc_test.exe' @()
    Run-Test 'ipc_crash_test.exe' @()
    Run-Test 'ipc_behavior_shared.exe' @()
    Run-Test 'ipc_behavior_static.exe' @()
    Run-Test 'ipc_framing_test.exe' @()
    Run-Test 'ipc_cpp_test.exe' @()
    Run-Test 'ipc_cpp_test.exe' @((Join-Path $bin 'ipc_echo.exe'))
    Run-Test 'rust-static.exe' @('--test-threads=1')
    Run-Test 'rust-shared.exe' @('--test-threads=1')
    Run-Test 'rust-sys-static.exe' @('--test-threads=1')
    Run-Test 'rust-sys-shared.exe' @('--test-threads=1')
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $directory 'results.json') -Encoding UTF8
    $failed = @($results | Where-Object { $_.exit_code -ne 0 }).Count
    Report ('LIBIPC_NATIVE_END failed=' + $failed + ' commands=' + $results.Count)
} catch {
    Report ('LIBIPC_NATIVE_ERROR ' + $_.ToString())
} finally {
    if ($null -ne $serial -and $serial.IsOpen) { $serial.Close() }
    & shutdown.exe /s /t 0
}
