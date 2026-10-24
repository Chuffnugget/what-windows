# Build-All owns execution. These helpers only record stages and summarise logs.
function New-BuildReport([string]$Root, [string[]]$Architectures, [bool]$ValidateOnly) {
    $run = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6)
    $directory = Join-Path $Root "build/reports/$run"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $stages = [Collections.Generic.List[object]]::new()
    $keys = @('Prerequisites', 'Signing')
    if (!$ValidateOnly) {
        $keys = @('Preflight') + $keys
        foreach ($architecture in $Architectures) { $keys += "$architecture Build", "$architecture Packaging" }
    }
    foreach ($key in $keys) {
        $stages.Add([ordered]@{ name=$key; status='Not run'; started=$null; seconds=0 })
    }
    return @{
        root=$Root; directory=$directory; log=(Join-Path $directory 'Build-All.log')
        started=[DateTime]::UtcNow; version='Unknown'; status='Failed'; current=$null
        architectures=$Architectures; validateOnly=$ValidateOnly; stages=$stages
        tools=[Collections.Generic.List[object]]::new()
        outputs=[Collections.Generic.List[object]]::new()
    }
}

function Complete-BuildReportStage($Report, [string]$Status) {
    if (!$Report.current) { return }
    $Report.current.status = $Status
    $Report.current.seconds = [math]::Round(([DateTime]::UtcNow - $Report.current.started).TotalSeconds, 1)
    $Report.current = $null
}

function Set-BuildReportStage($Report, [string]$Name) {
    Complete-BuildReportStage $Report 'Passed'
    $Report.current = $Report.stages | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    $Report.current.status = 'Running'
    $Report.current.started = [DateTime]::UtcNow
    # This marker associates compiler output with its architecture in the summary.
    Write-Host "WHAT build stage: $Name"
}

# Classify known paths and message formats; leave unfamiliar diagnostics visible
# as Other rather than claiming that every build warning belongs to WHAT.
function Get-BuildDiagnosticSource([string]$Message, [string]$Stage) {
    if ($Message -match '(?i)[\\/]_deps[\\/]yaml_cpp-src[\\/]') { return 'yaml-cpp' }
    if ($Message -match '(?i)[\\/]dependencies[\\/]openssl-[^\\/]+[\\/]' -or
        ($Stage -eq 'Prerequisites' -and $Message -match '^(?:apps|crypto|ssl|providers|engines|include)[\\/].+\(\d+\): (?:fatal )?(?:warning|error) C\d+:')) {
        return 'OpenSSL'
    }
    if ($Message -match '(?i)[\\/](?:_deps|dependencies)[\\/]') { return 'Other dependency' }
    # Qt 6.11.2 tracks canceled asynchronous controls in QTBUG-139552. Keep the
    # warning and its test name, but identify its upstream source in the summary.
    if ($Message -match '^QWARN\s*:.*There are still.*items in the process of being created at engine destruction\.') { return 'Qt' }
    if ($Message -match '^(?:QWARN|QFATAL|FAIL!|SKIP)\s*:|^WARNING:\s+\w+Tests::') { return 'Tests' }
    if ($Message -match '^CMake\b') { return 'Build configuration' }
    if ($Message -match '(?i)ARM64.*(?:not runtime-tested|runtime test|runtime-test)') { return 'Release validation' }
    if ($Message -match '(?i)(?:^|[\\/])(?:src|qml|tests)[\\/].*\(\d+(?:,\d+)?\):') { return 'WHAT' }
    return 'Other'
}

function Write-BuildReport($Report, [string]$Failure) {
    $finished = [DateTime]::UtcNow
    $diagnostics = [ordered]@{}
    $testTotals = [ordered]@{}
    $logs = [Collections.Generic.List[object]]::new()
    $logs.Add(@{ path=$Report.log; stage='Prerequisites' })
    $seenLogs = @{}
    # Qt suppresses routine warnings in its live output. Also scan each full test
    # log announced by Invoke-QtTests, including when that test aborts the build.
    for ($index = 0; $index -lt $logs.Count; $index++) {
        $entry = $logs[$index]
        if (!(Test-Path -LiteralPath $entry.path) -or $seenLogs.ContainsKey($entry.path)) { continue }
        $seenLogs[$entry.path] = $true
        $stage = $entry.stage
        $testName = Split-Path $entry.path -Leaf
        $lineNumber = 0
        foreach ($raw in [IO.File]::ReadLines($entry.path)) {
            $lineNumber++
            $line = ($raw -replace '\x1b\[[0-9;]*m', '').Trim() -replace '^>\s*', ''
            if ($line -match '^WHAT build stage: (.+)$') { $stage = $Matches[1]; continue }
            if ($line -match '^Running (.+)\. Full log: (.+)$') {
                $testName = $Matches[1]
                $logs.Add(@{ path=$Matches[2]; stage=$stage })
                continue
            }
            if ($index -eq 0 -and $line -match '^Totals:') { $testTotals["$stage / $testName"] = $line }
            $level = $null
            if ($line -match '(?i)\b(?:fatal\s+)?error(?:\s+[A-Z]+\d+)?\s*:|^(?:FAIL!|QFATAL|FAILED:|Exception:|CMake Error\b|ninja: build stopped)') {
                $level = 'Error'
            } elseif ($line -match '(?i)\bwarning(?:\s+[A-Z]+\d+)?\s*:|^QWARN\b|^CMake (?:Deprecation )?Warning\b') {
                $level = 'Warning'
            } elseif ($line -match '^SKIP\s*:') {
                $level = 'Skipped test'
            }
            if (!$level) { continue }
            $message = $line -replace '^WARNING:\s*', ''
            $key = "$level|$stage|$message"
            if (!$diagnostics.Contains($key)) {
                $source = Get-BuildDiagnosticSource $line $stage
                $diagnostics[$key] = [ordered]@{ level=$level; source=$source; stage=$stage; message=$message; log=$entry.path; line=$lineNumber; mentions=0 }
            }
            $diagnostics[$key].mentions++
        }
    }
    if ($Failure) {
        $diagnostics['Build failure'] = [ordered]@{ level='Error'; source='Build configuration'; stage='Build failure'; message=$Failure; log=$Report.log; line=0; mentions=1 }
    }
    $issues = @($diagnostics.Values)
    $errorCount = @($issues | Where-Object { $_.level -eq 'Error' }).Count
    $warningCount = @($issues | Where-Object { $_.level -eq 'Warning' }).Count
    $sourceCounts = @(foreach ($source in @('WHAT', 'Tests', 'Build configuration', 'Release validation', 'OpenSSL', 'yaml-cpp', 'Qt', 'Other dependency', 'Other')) {
        $matching = @($issues | Where-Object { $_.source -eq $source })
        if ($matching.Count -or $source -eq 'WHAT') {
            [ordered]@{
                source=$source
                errors=@($matching | Where-Object { $_.level -eq 'Error' }).Count
                warnings=@($matching | Where-Object { $_.level -eq 'Warning' }).Count
                skippedTests=@($matching | Where-Object { $_.level -eq 'Skipped test' }).Count
            }
        }
    })
    $summary = [ordered]@{
        schema=1; version=$Report.version; status=$Report.status
        started=$Report.started.ToString('o'); finished=$finished.ToString('o')
        seconds=[math]::Round(($finished - $Report.started).TotalSeconds, 1)
        architectures=$Report.architectures; validateOnly=$Report.validateOnly
        stages=@($Report.stages.ToArray()); tools=@($Report.tools.ToArray()); outputs=@($Report.outputs.ToArray())
        errors=$errorCount; warnings=$warningCount; diagnosticSources=$sourceCounts; diagnostics=$issues; testTotals=$testTotals
        logs=@($seenLogs.Keys | Sort-Object)
    }
    $jsonPath = Join-Path $Report.directory 'compilation-report.json'
    $markdownPath = Join-Path $Report.directory 'compilation-report.md'
    $summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('# WHAT compilation report')
    $lines.Add('')
    $lines.Add("Result: **$($summary.status)** | Version: $($summary.version) | Elapsed: $($summary.seconds) seconds")
    $lines.Add("Started (UTC): $($summary.started) | Architectures: $($Report.architectures -join ', ')")
    $lines.Add("Unique diagnostic messages: $errorCount errors, $warningCount warnings. Repeated log mentions are grouped.")
    $lines.Add('Counts cover detected diagnostics, not source-code defects. Full logs retain multiline details and other output.')
    $lines.Add('')
    $lines.Add('| Diagnostic source | Errors | Warnings | Skipped tests |')
    $lines.Add('| --- | ---: | ---: | ---: |')
    foreach ($source in $sourceCounts) {
        $lines.Add("| $($source.source) | $($source.errors) | $($source.warnings) | $($source.skippedTests) |")
    }
    $lines.Add('')
    $lines.Add('Dependency diagnostics are counted and retained below. WHAT counts cover its own source files; test and build-tool messages have separate rows.')
    $lines.Add('')
    $lines.Add('| Stage | Result | Seconds |')
    $lines.Add('| --- | --- | ---: |')
    foreach ($stage in $Report.stages) { $lines.Add("| $($stage.name) | $($stage.status) | $($stage.seconds) |") }
    $lines.Add('')
    $lines.Add('## Toolchains')
    $lines.Add('')
    foreach ($tool in $Report.tools) { $lines.Add("- $($tool.architecture): Qt $($tool.qt); MSVC $($tool.compilerVersion)") }
    $lines.Add('')
    $lines.Add('## Outputs and runtime tests')
    $lines.Add('')
    if (!$Report.outputs.Count) { $lines.Add('No build stage completed. Check the failed-stage log for partial output.') }
    foreach ($output in $Report.outputs) {
        $lines.Add("- $($output.architecture): runtime tests **$($output.runtimeTests)**; staging: $($output.release)")
        if ($output.installer) { $lines.Add("  Installer: $($output.installer)") }
        if ($output.portable) { $lines.Add("  Portable ZIP: $($output.portable)") }
    }
    if ($testTotals.Count) {
        $lines.Add('')
        $lines.Add('## Test totals')
        $lines.Add('')
        foreach ($name in $testTotals.Keys) { $lines.Add("- ${name}: $($testTotals[$name])") }
    }
    foreach ($level in @('Error', 'Warning', 'Skipped test')) {
        $lines.Add('')
        $lines.Add("## $level messages")
        $lines.Add('')
        $matching = @($issues | Where-Object { $_.level -eq $level })
        if (!$matching.Count) { $lines.Add('None detected.') }
        foreach ($issue in $matching) {
            $lines.Add("**$($issue.source) / $($issue.stage)** ($($issue.mentions) log mentions)")
            $lines.Add('')
            $lines.Add('```text')
            $lines.Add($issue.message)
            $lines.Add('```')
            $logUri = ([uri][IO.Path]::GetFullPath($issue.log)).AbsoluteUri
            $lines.Add("[Full log]($logUri), line $($issue.line)")
            $lines.Add('')
        }
    }
    $lines.Add('## Full logs')
    $lines.Add('')
    foreach ($log in $summary.logs) { $lines.Add("- [$([IO.Path]::GetFileName($log))]($(([uri][IO.Path]::GetFullPath($log)).AbsoluteUri))") }
    $lines | Set-Content -LiteralPath $markdownPath -Encoding UTF8
    Copy-Item -LiteralPath $markdownPath -Destination (Join-Path $Report.root 'build/compilation-report.md') -Force
    Copy-Item -LiteralPath $jsonPath -Destination (Join-Path $Report.root 'build/compilation-report.json') -Force
    Write-Host "Build result: $($summary.status). Diagnostic messages: $errorCount errors, $warningCount warnings."
    foreach ($source in $sourceCounts) {
        if ($source.errors -or $source.warnings -or $source.source -eq 'WHAT') {
            Write-Host "  $($source.source): $($source.errors) errors, $($source.warnings) warnings."
        }
    }
    Write-Host "Compilation report: $markdownPath"
}
