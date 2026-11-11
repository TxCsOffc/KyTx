[CmdletBinding()]
param(
    [ValidateSet('Launch','Arm','ArmMaterializer','Status')] [string] $Action = 'Launch',
    [ValidateRange(1,10000)] [int] $FrameCount = 120,
    [ValidateRange(1,10000)] [int] $CpuDetailEvery = 16,
    [switch] $FineCpu,
    [switch] $Lifetime,
    [switch] $DescriptorGather,
    [switch] $FlatSrt,
    [switch] $CompiledSrt,
    [ValidateSet('off','shadow','on')] [string] $CompiledSrtInitialMode = 'off',
    [switch] $BdaSync,
    [ValidateSet('off','shadow','on')] [string] $BdaSyncInitialMode = 'off',
    [string] $BdaSyncControlFile = 'D:\PS5\ufc5-bda-sync.control',
    [string] $CompiledSrtControlFile = 'D:\PS5\ufc5-compiled-srt.control',
    [switch] $CmaskPrecheck,
    [switch] $NoCmaskSkip,
    [switch] $Output1080p,
    [switch] $VramAttribution,
    [switch] $BdaSuballoc,
    [switch] $VramEvents,
    [switch] $ShaderStorage,
    [switch] $NoFunctionLdsBound,
    [ValidateRange(0,100000)] [int] $PipelineBudget = 0,
    [ValidatePattern('^(all|[0-9a-fA-F]{16})$')] [string] $FlatSrtShader = 'd3dcf81c43080fd0',
    [string] $FlatSrtControlFile = 'D:\PS5\ufc5-flat-srt.control',
    [switch] $MaterializerDiagnostics,
    [ValidateRange(1,120)] [int] $MaterializerSeconds = 20,
    [string] $MaterializerControlFile = 'D:\PS5\ufc5-materializer-profile.control',
    [ValidatePattern('^[0-9a-fA-F]{16}$')] [string] $DescriptorShader = 'd3dcf81c43080fd0',
    [string] $DescriptorControlFile = 'D:\PS5\ufc5-descriptor-gather.control',
    [ValidateRange(0,4294967295)] [Nullable[long]] $StartFrame,
    [switch] $Build,
    [string] $BuildDirectory,
    [string] $BinDirectory = 'D:\PS5\Emulators\KytyPS5-Bin',
    [string] $GameDirectory = 'D:\PS5\Games\UFC5',
    [string] $OutputDirectory = 'D:\PS5\ufc5-profiles',
    [string] $ControlFile = 'D:\PS5\ufc5-production-profile.control'
)
$ErrorActionPreference = 'Stop'
if ($DescriptorGather -and $FlatSrt) { throw 'Run descriptor gather and FlatSRT as separate experiments.' }
if ($CompiledSrt -and ($DescriptorGather -or $FlatSrt)) { throw 'Run compiled SRT separately from descriptor gather and FlatSRT.' }
if ($BdaSync -and ($DescriptorGather -or $FlatSrt)) { throw 'Run BDA sync separately from descriptor gather and FlatSRT.' }
if ($FlatSrt) { $FlatSrtShader = $FlatSrtShader.ToLowerInvariant() }
if ($Action -eq 'ArmMaterializer') {
    Set-Content -LiteralPath $MaterializerControlFile -Value 'on' -Encoding Ascii
    Write-Output "Materializer window armed: $MaterializerControlFile. Stops automatically after its launch-configured duration."
    return
}
if ($Action -eq 'Arm') {
    Set-Content -LiteralPath $ControlFile -Value 'on' -Encoding Ascii
    Write-Output "Capture armed. The producer will consume this request and capture its configured window: $ControlFile"
    return
}
if ($Action -eq 'Status') {
    if (Test-Path -LiteralPath $ControlFile) { Get-Content -LiteralPath $ControlFile }
    Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,Responding
    Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.cpu.csv' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 4 FullName,Length,LastWriteTime
    return
}
if (Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue) {
    throw 'Kyty is running. Close it before launching the profiling build.'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo '_Build\windows' }
if ($Build) {
    & cmake --build $BuildDirectory --target kyty_emulator -j 8
    if ($LASTEXITCODE -ne 0) { throw 'Profiling build failed.' }
}
$source = Join-Path $BuildDirectory 'kyty_emulator.exe'
if (-not (Test-Path -LiteralPath $source)) { throw 'Executable missing. Use -Build.' }
if (-not (Test-Path -LiteralPath $GameDirectory)) { throw "Game directory missing: $GameDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$exe = Join-Path $BinDirectory 'kyty_emulator.production-profile.exe'
Copy-Item -LiteralPath $source -Destination $exe -Force
$base = Join-Path $OutputDirectory ('production-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.csv')
$names = @('KYTY_GPU_TIMING_CSV','KYTY_GPU_TIMING_CONTROL_FILE','KYTY_GPU_PROFILE_CONTROL_FILE',
           'KYTY_GPU_PROFILE_START_FRAME','KYTY_GPU_PROFILE_FRAME_COUNT','KYTY_GPU_PROFILE_CPU_DETAIL_EVERY','KYTY_VIDEO_OUT_1080P',
           'KYTY_DETILE_CAPTURE_DIR','KYTY_DETILE_TRACE','KYTY_SPARSE_BDA','KYTY_VRAM_TEST_FPS')
$names += @('KYTY_GPU_PROFILE_FINE_CPU','KYTY_GPU_PROFILE_LIFETIME')
$names += @('KYTY_DESCRIPTOR_GATHER_SHADER','KYTY_DESCRIPTOR_GATHER_CONTROL_FILE','KYTY_DESCRIPTOR_GATHER_CSV')
$names += @('KYTY_FLAT_SRT_SHADER','KYTY_FLAT_SRT_CONTROL_FILE','KYTY_FLAT_SRT_CSV')
$names += @('KYTY_COMPILED_SRT_CONTROL_FILE','KYTY_COMPILED_SRT_CSV')
$names += @('KYTY_BDA_SYNC_CONTROL_FILE','KYTY_BDA_SYNC_CSV')
$names += @('KYTY_CMASK_PRECHECK_CSV','KYTY_CMASK_SKIP_IMPOSSIBLE','KYTY_VRAM_ATTRIBUTION_CSV','KYTY_BDA_SUBALLOC','KYTY_VRAM_EVENTS','KYTY_PIPELINE_BUDGET','KYTY_SHADER_STORAGE_LOG','KYTY_FUNCTION_LDS_BOUND')
$names += @('KYTY_MATERIALIZER_PROFILE_CONTROL_FILE','KYTY_MATERIALIZER_PROFILE_CSV','KYTY_MATERIALIZER_PROFILE_SECONDS')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:KYTY_GPU_TIMING_CSV = $base
    $env:KYTY_GPU_PROFILE_FRAME_COUNT = "$FrameCount"
    $env:KYTY_GPU_PROFILE_CPU_DETAIL_EVERY = "$CpuDetailEvery"
    $env:KYTY_GPU_PROFILE_FINE_CPU = if ($FineCpu) { '1' } else { '0' }
    $env:KYTY_GPU_PROFILE_LIFETIME = if ($Lifetime) { '1' } else { '0' }
    if ($CompiledSrt) {
        Set-Content -LiteralPath $CompiledSrtControlFile -Value $CompiledSrtInitialMode -Encoding Ascii
        $env:KYTY_COMPILED_SRT_CONTROL_FILE = $CompiledSrtControlFile
        $env:KYTY_COMPILED_SRT_CSV = $base + '.compiled-srt.csv'
    } else {
        foreach ($name in @('KYTY_COMPILED_SRT_CONTROL_FILE','KYTY_COMPILED_SRT_CSV')) {
            [Environment]::SetEnvironmentVariable($name,$null,'Process')
        }
    }
    if ($BdaSync) {
        Set-Content -LiteralPath $BdaSyncControlFile -Value $BdaSyncInitialMode -Encoding Ascii
        $env:KYTY_BDA_SYNC_CONTROL_FILE = $BdaSyncControlFile
        $env:KYTY_BDA_SYNC_CSV = $base + '.bda-sync.csv'
    } else {
        foreach ($name in @('KYTY_BDA_SYNC_CONTROL_FILE','KYTY_BDA_SYNC_CSV')) {
            [Environment]::SetEnvironmentVariable($name,$null,'Process')
        }
    }
    if ($CmaskPrecheck) {
        $env:KYTY_CMASK_PRECHECK_CSV = $base + '.cmask-precheck.csv'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_CMASK_PRECHECK_CSV',$null,'Process')
    }
    if ($VramAttribution) {
        $env:KYTY_VRAM_ATTRIBUTION_CSV = $base + '.vram-attribution.csv'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_VRAM_ATTRIBUTION_CSV',$null,'Process')
    }
    if ($VramEvents) {
        $env:KYTY_VRAM_EVENTS = '1'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_VRAM_EVENTS',$null,'Process')
    }
    if ($ShaderStorage) {
        $env:KYTY_SHADER_STORAGE_LOG = $base + '.shader-storage.txt'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_SHADER_STORAGE_LOG',$null,'Process')
    }
    if ($NoFunctionLdsBound) {
        $env:KYTY_FUNCTION_LDS_BOUND = '0'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_FUNCTION_LDS_BOUND',$null,'Process')
    }
    if ($PipelineBudget -gt 0) {
        $env:KYTY_PIPELINE_BUDGET = "$PipelineBudget"
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_PIPELINE_BUDGET',$null,'Process')
    }
    if ($BdaSuballoc) {
        $env:KYTY_BDA_SUBALLOC = '1'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_BDA_SUBALLOC',$null,'Process')
    }
    if (-not $NoCmaskSkip) {
        $env:KYTY_CMASK_SKIP_IMPOSSIBLE = '1'
    } else {
        [Environment]::SetEnvironmentVariable('KYTY_CMASK_SKIP_IMPOSSIBLE',$null,'Process')
    }
    if ($FlatSrt) {
        Set-Content -LiteralPath $FlatSrtControlFile -Value 'off' -Encoding Ascii
        $env:KYTY_FLAT_SRT_SHADER = $FlatSrtShader
        $env:KYTY_FLAT_SRT_CONTROL_FILE = $FlatSrtControlFile
        $env:KYTY_FLAT_SRT_CSV = $base + '.flat-srt.csv'
    } else {
        foreach ($name in @('KYTY_FLAT_SRT_SHADER','KYTY_FLAT_SRT_CONTROL_FILE','KYTY_FLAT_SRT_CSV')) {
            [Environment]::SetEnvironmentVariable($name,$null,'Process')
        }
    }
    if ($MaterializerDiagnostics) {
        Set-Content -LiteralPath $MaterializerControlFile -Value 'off' -Encoding Ascii
        $env:KYTY_MATERIALIZER_PROFILE_CONTROL_FILE = $MaterializerControlFile
        $env:KYTY_MATERIALIZER_PROFILE_CSV = $base + '.materializer.csv'
        $env:KYTY_MATERIALIZER_PROFILE_SECONDS = "$MaterializerSeconds"
    } else {
        foreach ($name in @('KYTY_MATERIALIZER_PROFILE_CONTROL_FILE','KYTY_MATERIALIZER_PROFILE_CSV','KYTY_MATERIALIZER_PROFILE_SECONDS')) {
            [Environment]::SetEnvironmentVariable($name,$null,'Process')
        }
    }
    if ($DescriptorGather) {
        Set-Content -LiteralPath $DescriptorControlFile -Value 'off' -Encoding Ascii
        $env:KYTY_DESCRIPTOR_GATHER_SHADER = $DescriptorShader
        $env:KYTY_DESCRIPTOR_GATHER_CONTROL_FILE = $DescriptorControlFile
        $env:KYTY_DESCRIPTOR_GATHER_CSV = $base + '.descriptor.csv'
    } else {
        foreach ($name in @('KYTY_DESCRIPTOR_GATHER_SHADER','KYTY_DESCRIPTOR_GATHER_CONTROL_FILE','KYTY_DESCRIPTOR_GATHER_CSV')) {
            [Environment]::SetEnvironmentVariable($name,$null,'Process')
        }
    }
    $env:KYTY_VIDEO_OUT_1080P = if ($Output1080p) { '1' } else { '0' }
    $env:KYTY_VRAM_TEST_FPS = '1'
    foreach ($name in @('KYTY_GPU_TIMING_CONTROL_FILE','KYTY_DETILE_CAPTURE_DIR','KYTY_DETILE_TRACE')) {
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    if ($null -ne $StartFrame) {
        $env:KYTY_GPU_PROFILE_START_FRAME = "$StartFrame"
        [Environment]::SetEnvironmentVariable('KYTY_GPU_PROFILE_CONTROL_FILE',$null,'Process')
    } else {
        Set-Content -LiteralPath $ControlFile -Value 'off' -Encoding Ascii
        $env:KYTY_GPU_PROFILE_CONTROL_FILE = $ControlFile
        [Environment]::SetEnvironmentVariable('KYTY_GPU_PROFILE_START_FRAME',$null,'Process')
    }
    if (-not $previous['KYTY_SPARSE_BDA']) { $env:KYTY_SPARSE_BDA = '1' }
    $launchArgs = @('--game',('"' + $GameDirectory + '"'),'--printf-direction','Silent')
    $process = Start-Process -PassThru -WindowStyle Hidden -FilePath $exe -WorkingDirectory $BinDirectory `
        -ArgumentList $launchArgs -RedirectStandardOutput ($base + '.stdout.txt') -RedirectStandardError ($base + '.stderr.txt')
    $manifest = [ordered]@{ executable=$exe; sha256=(Get-FileHash -LiteralPath $exe).Hash;
        pid=$process.Id; started=(Get-Date).ToString('o'); trace=$base; frame_count=$FrameCount;
        start_frame=$StartFrame; cpu_detail_every=$CpuDetailEvery; control_file=$ControlFile; native_output=-not $Output1080p;
        fine_cpu=[bool]$FineCpu; lifetime=[bool]$Lifetime;
        fine_cpu_root_every=32;
        descriptor_gather=[bool]$DescriptorGather; descriptor_shader=$DescriptorShader;
        descriptor_control_file=if ($DescriptorGather) { $DescriptorControlFile } else { $null };
        flat_srt=[bool]$FlatSrt; flat_srt_shader=$FlatSrtShader;
        flat_srt_control_file=if ($FlatSrt) { $FlatSrtControlFile } else { $null };
        compiled_srt=[bool]$CompiledSrt;
        compiled_srt_initial_mode=if ($CompiledSrt) { $CompiledSrtInitialMode } else { $null };
        compiled_srt_control_file=if ($CompiledSrt) { $CompiledSrtControlFile } else { $null };
        bda_sync=[bool]$BdaSync;
        bda_sync_initial_mode=if ($BdaSync) { $BdaSyncInitialMode } else { $null };
        bda_sync_control_file=if ($BdaSync) { $BdaSyncControlFile } else { $null };
        cmask_precheck_csv=if ($CmaskPrecheck) { $base + '.cmask-precheck.csv' } else { $null };
        cmask_skip_impossible=-not $NoCmaskSkip;
        bda_suballoc=[bool]$BdaSuballoc; vram_events=[bool]$VramEvents; shader_storage=[bool]$ShaderStorage; function_lds_bound=-not $NoFunctionLdsBound; pipeline_budget=$PipelineBudget;
        vram_attribution_csv=if ($VramAttribution) { $base + '.vram-attribution.csv' } else { $null };
        materializer_diagnostics=[bool]$MaterializerDiagnostics; materializer_seconds=$MaterializerSeconds;
        materializer_control_file=if ($MaterializerDiagnostics) { $MaterializerControlFile } else { $null };
        sparse_bda=$env:KYTY_SPARSE_BDA; prior_environment=$previous;
        kyty_environment=@(Get-ChildItem Env:KYTY* | Select-Object Name,Value);
        fps_log_start_bytes=if (Test-Path 'D:\PS5\fps-pressure-test.txt') { (Get-Item 'D:\PS5\fps-pressure-test.txt').Length } else { 0 } }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($base + '.manifest.json') -Encoding UTF8
    Write-Output "Profiling build running: PID $($process.Id)"
    Write-Output "Trace: $base"
    Write-Output "Window: $FrameCount guest SuspendPoint epochs (not guaranteed display frame numbers)."
    if ($CompiledSrt) {
        Write-Output "Compiled SRT starts $CompiledSrtInitialMode. Control: $CompiledSrtControlFile"
        Write-Output 'Leave the detailed production profiler unarmed for the initial off/on timing comparison.'
    } elseif ($null -eq $StartFrame) { Write-Output 'Drive to the paused fight, then run this script with -Action Arm. Capture stops automatically.' }
    if ($BdaSync) { Write-Output "BDA sync starts $BdaSyncInitialMode. Control: $BdaSyncControlFile" }
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
}
