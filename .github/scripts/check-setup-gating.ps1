# Parse-check windows/setup.ps1 and exercise its disc-ID gating for every disc
# the package accepts -- by extracting the SHIPPED lines out of the file and
# evaluating them, never by re-typing the logic here, so the check cannot pass
# against a copy that has drifted from what ships.
#
#   pwsh -NoProfile -File check-setup-gating.ps1      # setup.ps1 must sit alongside
#
# Run it on real Windows PowerShell 5.1 when possible; a portable pwsh 7.x is
# necessary but not sufficient, since 5.1 is the parser players actually use.
# Every assertion here has been mutation-tested: granting a disc a flag it has
# not earned, or giving PAL the US hook addresses, makes it fail by name.
$ErrorActionPreference = 'Stop'
# Any error the extracted blocks throw fails the check by name; without this a
# terminating error could end the run with nothing but a red line to show for it.
trap { Write-Host "ERROR: $_"; exit 1 }
$file = Join-Path $PSScriptRoot 'setup.ps1'
# The shipped blocks read $Here (setup.ps1's own folder) for the per-disc
# profiles; setup.ps1 sits alongside this script, so its folder is this one.
$Here = $PSScriptRoot
if (-not (Test-Path -LiteralPath $file)) { Write-Host "MISSING $file"; exit 2 }

# 1. Full-file parse
$errors = $null
$tokens = $null
[System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors -and $errors.Count -gt 0) {
    Write-Host "PARSE ERRORS: $($errors.Count)"
    foreach ($e in $errors) { Write-Host ("  line {0}: {1}" -f $e.Extent.StartLineNumber, $e.Message) }
    exit 1
}
Write-Host "PARSE OK  ($($tokens.Count) tokens, PowerShell $($PSVersionTable.PSVersion))"

# 2. Exercise the gating blocks, extracted from the file itself
$text = Get-Content -LiteralPath $file -Raw
$lcBlock = [regex]::Match($text, '(?s)\$LeaderCases = @\(\).*?(?=& \(Join-Path \$Here)').Value
$pfBlock = [regex]::Match($text, '(?s)\$PsqFast = @\(\).*?(?=& \$Cmake)').Value
if (-not $lcBlock) { Write-Host "could not extract the LeaderCases block"; exit 3 }
if (-not $pfBlock) { Write-Host "could not extract the PsqFast block"; exit 3 }

Write-Host ""
Write-Host ("{0,-8} {1,-62} {2}" -f 'disc', 'recompiler flags', 'cmake')
Write-Host ("-" * 100)
foreach ($DiscId in @('GRSEAF', 'GRSJAF', 'GRSPAF', 'GRSEPS', 'GQSE5D')) {
    $LeaderCases = @(); $PsqFast = @()
    Invoke-Expression $lcBlock
    Invoke-Expression $pfBlock
    $lc = if ($LeaderCases.Count) { $LeaderCases -join ' ' } else { '(none)' }
    $pf = if ($PsqFast.Count) { $PsqFast -join ' ' } else { '(none)' }
    if ($lc.Length -gt 60) { $lc = $lc.Substring(0, 57) + '...' }
    Write-Host ("{0,-8} {1,-62} {2}" -f $DiscId, $lc, $pf)
}

# 3. The invariants that matter
Write-Host ""
$failed = 0
foreach ($DiscId in @('GRSEAF', 'GRSJAF', 'GRSPAF', 'GRSEPS', 'GQSE5D', 'ZZZZZZ')) {
    $LeaderCases = @(); $PsqFast = @()
    Invoke-Expression $lcBlock
    Invoke-Expression $pfBlock
    $hasLC = $LeaderCases -contains '--leader-cases'
    $lcCount = ($LeaderCases | Where-Object { $_ -eq '--leader-cases' }).Count
    $hasDirect = $LeaderCases -contains '--direct-calls'
    $hasSelf = $LeaderCases -contains '--self-calls'
    $hasMem = $PsqFast -contains '-DMODULE_MEM_FAST=ON'
    $bad = @()
    if ($lcCount -gt 1) { $bad += "--leader-cases passed $lcCount times" }
    # The timing-changing flags are allowed only on discs that have been
    # PLAYTESTED: US, then Plus, JP and PAL on 2026-09-16. Any disc added here
    # without a playtest is the mistake this list exists to prevent.
    $playtested = 'GRSEAF', 'GRSEPS', 'GRSJAF', 'GRSPAF'
    if ($hasDirect -and -not ($playtested -contains $DiscId)) { $bad += 'direct-calls on an unplaytested disc' }
    if ($hasSelf -and -not ($playtested -contains $DiscId)) { $bad += 'self-calls on an unplaytested disc' }
    # JP takes the flags but must take NO --dispatch-pc: on that disc the US
    # literals are not entry points at all, so passing them would only make the
    # wrong code dispatcher-reachable.
    $dpCount = ($LeaderCases | Where-Object { $_ -eq '--dispatch-pc' }).Count
    if ($DiscId -eq 'GRSJAF' -and $dpCount -ne 0) { $bad += "JP passed $dpCount --dispatch-pc" }
    if (('GRSEAF', 'GRSEPS', 'GRSPAF' -contains $DiscId) -and $dpCount -ne 6) { $bad += "$DiscId passed $dpCount --dispatch-pc, expected 6" }
    # PAL must pass its OWN addresses, never the US list.
    if ($DiscId -eq 'GRSPAF' -and ($LeaderCases -contains '0x8020C1E8')) { $bad += 'PAL passed the US hook addresses' }
    if ($DiscId -eq 'GRSPAF' -and -not ($LeaderCases -contains '0x80213938')) { $bad += 'PAL is missing its own hook addresses' }
    if ($hasLC -ne $hasMem) { $bad += 'leader-cases and MEM_FAST disagree' }
    if (('GRSJAF', 'GRSPAF', 'GRSEPS' -contains $DiscId) -and -not $hasLC) { $bad += "$DiscId did not get leader-cases" }
    if (('GRSEPS', 'GRSJAF', 'GRSPAF' -contains $DiscId) -and -not ($hasDirect -and $hasSelf)) { $bad += "$DiscId lost its playtested flags" }
    # An unrecognised disc must get nothing at all: every lever depends on a
    # profile trained for THAT disc, and there is none for a disc we know nothing
    # about. This is the case that would silently ship a stale-profile build.
    if (('GQSE5D', 'ZZZZZZ' -contains $DiscId) -and ($hasLC -or $hasMem -or $hasDirect -or $hasSelf)) {
        $bad += "unknown disc $DiscId got a lever"
    }
    if ($bad.Count) { Write-Host ("FAIL {0}: {1}" -f $DiscId, ($bad -join '; ')); $failed++ }
    else { Write-Host ("ok   {0}" -f $DiscId) }
}

# 4. The guests' code sites: only with -Spawn or -Heihachi, only on the US disc, and the same fifteen
# setup.sh passes (nine in gate group 1, six in group 2). A guest build without them keeps the guests
# off; a plain build WITH them costs every player cycles.
# setup.sh is compared when it sits alongside (copy it next to setup.ps1); without it only the counts are checked.
$setupSh = Join-Path $PSScriptRoot 'setup.sh'
foreach ($case in @(@('GRSEAF', '', ''), @('GRSEAF', 'x', ''), @('GRSEAF', '', 'p'), @('GRSEAF', 'x', 'p'),
                    @('GRSPAF', 'x', 'p'), @('GRSJAF', 'x', ''), @('GRSEPS', '', 'p'), @('ZZZZZZ', 'x', 'p'))) {
    $DiscId = $case[0]; $Spawn = $case[1]; $Heihachi = $case[2]
    $LeaderCases = @()
    Invoke-Expression $lcBlock
    $sites = @($LeaderCases | Where-Object { $_ -match '^0x[0-9A-Fa-f]{8}@[12]$' })
    $g1 = @($sites | Where-Object { $_ -like '*@1' }).Count
    $g2 = @($sites | Where-Object { $_ -like '*@2' }).Count
    $want = ($DiscId -eq 'GRSEAF') -and ($Spawn -or $Heihachi)
    $label = "{0} spawn={1} heihachi={2}" -f $DiscId, [bool]$Spawn, [bool]$Heihachi
    $bad = @()
    if ($want -and ($g1 -ne 9 -or $g2 -ne 6)) { $bad += "expected 9+6 guest sites, got $g1+$g2" }
    if (-not $want -and $sites.Count) { $bad += "$($sites.Count) guest sites on a build that must have none" }
    if ($want -and (Test-Path -LiteralPath $setupSh)) {
        $shText = Get-Content -LiteralPath $setupSh -Raw
        $shSites = @([regex]::Matches($shText, '--gated-pc (0x[0-9A-Fa-f]{8}@[12])') | ForEach-Object { $_.Groups[1].Value })
        if (Compare-Object $shSites $sites) { $bad += 'guest sites differ from setup.sh' }
    }
    if ($bad.Count) { Write-Host ("FAIL {0}: {1}" -f $label, ($bad -join '; ')); $failed++ }
    else { Write-Host ("ok   {0}" -f $label) }
}
$Spawn = ''; $Heihachi = ''
if ($failed) { Write-Host "$failed disc(s) failed"; exit 1 }
exit 0
