<#
.SYNOPSIS
    End-to-end tests for the TLS file transfer server and client.

.DESCRIPTION
    Assumes the project has already been built, then exercises it against a
    real server process over a real TLS connection:

      * command line handling and exit codes
      * a clean transfer, compared byte for byte
      * resuming an interrupted transfer
      * rejecting a file whose contents no longer match
      * several clients transferring at the same time
      * two clients competing for the same destination file
      * certificate verification, including the cases that must fail

.PARAMETER BuildDir
    Directory holding the built FTP.exe (default: ./build).

.PARAMETER Port
    Port the test server listens on (default: 9100).

.EXAMPLE
    pwsh tests/run_tests.ps1 -BuildDir build
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [int]$Port = 9100,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $BuildDir 'FTP.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    $exe = Join-Path (Resolve-Path $BuildDir) 'FTP.exe'
}
if (-not (Test-Path -LiteralPath $exe)) {
    throw "FTP.exe not found in '$BuildDir'. Build the project first."
}
$buildDirFull = Split-Path -Parent $exe

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("ftp-tests-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$serverOut = Join-Path $work 'server.stdout'
$serverErr = Join-Path $work 'server.stderr'
$server = $null
$script:passed = 0
$script:failed = 0
$script:failures = @()

# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

function New-RandomFile {
    param([string]$Path, [int]$Megabytes)
    $buffer = New-Object byte[] (4MB)
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $fs = [System.IO.File]::Create($Path)
    for ($i = 0; $i -lt [math]::Ceiling($Megabytes / 4); $i++) {
        $rng.GetBytes($buffer)
        $fs.Write($buffer, 0, $buffer.Length)
    }
    $fs.Close()
}

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
}

function Invoke-Ftp {
    <# Runs FTP.exe to completion and returns its exit code plus output. #>
    param([string[]]$Arguments)
    $o = Join-Path $work ([guid]::NewGuid().ToString('N') + '.out')
    $e = Join-Path $work ([guid]::NewGuid().ToString('N') + '.err')
    $start = @{
        FilePath               = $exe
        PassThru               = $true
        NoNewWindow            = $true
        Wait                   = $true
        RedirectStandardOutput = $o
        RedirectStandardError  = $e
    }
    # Start-Process rejects an empty collection, so it is only passed when set.
    if ($Arguments -and $Arguments.Count -gt 0) { $start['ArgumentList'] = $Arguments }
    $p = Start-Process @start
    $result = @{
        Code = $p.ExitCode
        Out  = (Get-Content -LiteralPath $o -Raw -ErrorAction SilentlyContinue)
        Err  = (Get-Content -LiteralPath $e -Raw -ErrorAction SilentlyContinue)
    }
    Remove-Item -LiteralPath $o, $e -Force -ErrorAction SilentlyContinue
    $result
}

function Start-FtpClient {
    <# Starts FTP.exe in the background; returns the process object. #>
    param([string[]]$Arguments, [string]$Tag)
    $o = Join-Path $work "$Tag.out"
    $e = Join-Path $work "$Tag.err"
    $p = Start-Process -FilePath $exe -ArgumentList $Arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput $o -RedirectStandardError $e
    # Touching .Handle makes the object cache the process handle, without which
    # .ExitCode comes back empty for a process that has already finished.
    $null = $p.Handle
    $p
}

function Wait-ForServerLog {
    <# Blocks until the server appends text matching a pattern, or the timeout
       expires. Only content written after this call is considered, so a match
       left behind by an earlier transfer can never be mistaken for this one. #>
    param([string]$Pattern, [int]$TimeoutSeconds = 60)
    $mark = 0
    if (Test-Path -LiteralPath $serverOut) {
        $existing = Get-Content -LiteralPath $serverOut -Raw -ErrorAction SilentlyContinue
        if ($existing) { $mark = $existing.Length }
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $serverOut) {
            $text = Get-Content -LiteralPath $serverOut -Raw -ErrorAction SilentlyContinue
            if ($text -and $text.Length -gt $mark) {
                if ($text.Substring($mark) -match $Pattern) { return $true }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Test-Case {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')
    if ($Condition) {
        $script:passed++
        Write-Host ("  PASS  " + $Name) -ForegroundColor Green
    } else {
        $script:failed++
        $script:failures += $Name
        Write-Host ("  FAIL  " + $Name) -ForegroundColor Red
        if ($Detail) { Write-Host ("        " + $Detail) -ForegroundColor DarkYellow }
    }
}

function Get-SentMegabytes {
    <# Pulls the number out of the client's "[INFO] Sent 12.34 MB" line. #>
    param([string]$Output)
    if ($Output -match '\[INFO\] Sent\s+([0-9.]+)\s+MB') {
        return [double]$Matches[1]
    }
    return $null
}

function Clear-Received {
    Get-ChildItem -LiteralPath $buildDirFull -Filter 'received_*' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# --------------------------------------------------------------------------
# server lifecycle
# --------------------------------------------------------------------------

function Start-TestServer {
    param([string]$Port)
    $p = Start-Process -FilePath $exe -ArgumentList @('server', $Port) -PassThru -NoNewWindow `
            -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr `
            -WorkingDirectory $buildDirFull
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) {
            throw "The server exited immediately: $(Get-Content $serverErr -Raw -ErrorAction SilentlyContinue)"
        }
        if ((Get-Content -LiteralPath $serverOut -Raw -ErrorAction SilentlyContinue) -match 'Listening on port') {
            return $p
        }
        Start-Sleep -Milliseconds 150
    }
    throw 'The server did not report that it is listening.'
}

try {
    # Uploads land in the build directory, so anything left over from a run that
    # was interrupted would be seen by the resume tests and change their result.
    Clear-Received
    $server = Start-TestServer -Port $Port
    Write-Host ""
    Write-Host "FTP end-to-end tests  (server: $exe on port $Port)" -ForegroundColor Cyan
    Write-Host ""

    # ----------------------------------------------------------------------
    Write-Host 'Command line handling' -ForegroundColor Cyan
    $r = Invoke-Ftp @()
    Test-Case 'no arguments prints usage and exits 1' ($r.Code -eq 1 -and $r.Err -match 'Usage')

    $r = Invoke-Ftp @('nonsense')
    Test-Case 'unknown mode exits 1' ($r.Code -eq 1)

    $r = Invoke-Ftp @('client')
    Test-Case 'client without a file exits 1' ($r.Code -eq 1)

    foreach ($bad in @('0', '70000', 'abc', '65536', '-1')) {
        $r = Invoke-Ftp @('server', $bad)
        Test-Case "invalid port '$bad' is rejected" ($r.Code -eq 1 -and $r.Err -match 'Invalid port')
    }

    $r = Invoke-Ftp @('client', (Join-Path $work 'x.bin'), '127.0.0.1', '70000')
    Test-Case 'client rejects an invalid port' ($r.Code -eq 1 -and $r.Err -match 'Invalid port')

    $r = Invoke-Ftp @('client', (Join-Path $work 'x.bin'), '127.0.0.1', $Port, '--nonsense')
    Test-Case 'unknown option is rejected' ($r.Code -eq 1)

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Clean transfer' -ForegroundColor Cyan
    Clear-Received
    $small = Join-Path $work 'small.bin'
    New-RandomFile -Path $small -Megabytes 5
    $smallHash = Get-Sha256 $small

    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port)
    Test-Case 'client exits 0' ($r.Code -eq 0)
    Test-Case 'client reports the server verified the file' ($r.Out -match 'Server verified the file')

    $stored = Join-Path $buildDirFull 'received_small.bin'
    Test-Case 'the file was stored under received_<name>' (Test-Path -LiteralPath $stored)
    Test-Case 'the stored size matches' ((Get-Item $stored).Length -eq (Get-Item $small).Length)
    Test-Case 'the stored SHA-256 matches' ((Get-Sha256 $stored) -eq $smallHash)

    $a = [System.IO.File]::ReadAllBytes($small)
    $b = [System.IO.File]::ReadAllBytes($stored)
    $identical = [System.Linq.Enumerable]::SequenceEqual([byte[]]$a, [byte[]]$b)
    Test-Case 'the stored file is byte for byte identical' $identical

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Certificate verification' -ForegroundColor Cyan
    $ca = Join-Path $buildDirFull 'server.crt'

    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port, '--ca', $ca)
    Test-Case 'the correct CA is accepted (IP subject alternative name)' ($r.Code -eq 0)

    $r = Invoke-Ftp @('client', $small, 'localhost', $Port, '--ca', $ca)
    Test-Case 'the correct CA is accepted (DNS subject alternative name)' ($r.Code -eq 0)

    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port, '--insecure')
    Test-Case '--insecure connects without verification' ($r.Code -eq 0 -and $r.Err -match 'verification is disabled')

    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port, '--ca', (Join-Path $work 'missing.crt'))
    Test-Case 'a missing CA file is reported' ($r.Code -eq 1 -and $r.Err -match 'Could not load the CA')

    # An unrelated CA must never be able to vouch for the server, even though
    # the certificate itself names the host we connected to.
    $otherCrt = Join-Path $work 'other-ca.crt'
    $otherKey = Join-Path $work 'other-ca.key'
    $otherCnf = Join-Path $work 'other-ca.cnf'
    @"
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = unrelated-test-ca
[v3]
basicConstraints = critical,CA:TRUE
"@ | Set-Content -LiteralPath $otherCnf -Encoding ascii
    $openssl = Get-Command openssl -ErrorAction SilentlyContinue
    if ($openssl) {
        # openssl writes its progress dots to stderr, which would otherwise be
        # turned into a terminating error by $ErrorActionPreference.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $openssl.Source req -x509 -newkey rsa:2048 -nodes -keyout $otherKey -out $otherCrt `
            -days 365 -config $otherCnf 2>&1 | Out-Null
        $ErrorActionPreference = $previousPreference
        if (Test-Path $otherCrt) {
            $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port, '--ca', $otherCrt)
            Test-Case 'an unrelated CA is rejected' ($r.Code -eq 1 -and ($r.Err -match 'handshake|verify'))
        } else {
            Write-Host '  SKIP  an unrelated CA is rejected (openssl unavailable)' -ForegroundColor DarkYellow
        }
    } else {
        Write-Host '  SKIP  an unrelated CA is rejected (openssl unavailable)' -ForegroundColor DarkYellow
    }

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Integrity' -ForegroundColor Cyan
    Clear-Received
    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port)
    $stored = Join-Path $buildDirFull 'received_small.bin'
    $fs = [System.IO.File]::Open($stored, 'Open', 'Write')
    $fs.Seek(1048576, 'Begin') | Out-Null
    $fs.Write([byte[]](0, 0, 0, 0, 0, 0, 0, 0), 0, 8)
    $fs.Close()
    $r = Invoke-Ftp @('client', $small, '127.0.0.1', $Port)
    Test-Case 'a tampered file is refused (exit 1)' ($r.Code -eq 1)
    Test-Case 'the client is told the transfer failed' `
        (($r.Err -match 'refused this transfer') -or ($r.Err -match 'did not accept the file')) `
        "stderr: $($r.Err)"
    Test-Case 'the server deleted the tampered file' (-not (Test-Path -LiteralPath $stored))

    Clear-Received
    $empty = Join-Path $work 'empty.bin'
    New-Item -ItemType File -Path $empty -Force | Out-Null
    $r = Invoke-Ftp @('client', $empty, '127.0.0.1', $Port)
    Test-Case 'an empty file is refused by the client' ($r.Code -eq 1)

    $r = Invoke-Ftp @('client', (Join-Path $work 'no-such-file.bin'), '127.0.0.1', $Port)
    Test-Case 'a missing source file is reported' ($r.Code -eq 1 -and $r.Err -match 'no-such-file.bin')

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Resuming' -ForegroundColor Cyan
    Clear-Received
    $big = Join-Path $work 'big.bin'
    New-RandomFile -Path $big -Megabytes 24
    $bigHash = Get-Sha256 $big
    $storedBig = Join-Path $buildDirFull 'received_big.bin'

    $r = Invoke-Ftp @('client', $big, '127.0.0.1', $Port)
    Test-Case 'the 24 MB file uploads' ($r.Code -eq 0 -and (Get-Sha256 $storedBig) -eq $bigHash)

    # Truncating the stored file is a deterministic stand-in for a transfer
    # that was cut short, and exercises exactly the same resume path.
    $truncateTo = 7MB + 12345
    $fs = [System.IO.File]::Open($storedBig, 'Open', 'Write')
    $fs.SetLength($truncateTo)
    $fs.Close()
    $r = Invoke-Ftp @('client', $big, '127.0.0.1', $Port)
    Test-Case 'a resumed transfer reports the resume offset' ($r.Out -match "Resuming from offset: $truncateTo")
    $expectedRemainder = [math]::Round(((Get-Item $big).Length - $truncateTo) / 1MB, 2)
    $actualRemainder = Get-SentMegabytes -Output $r.Out
    Test-Case "only the missing $expectedRemainder MB is sent" `
        ($null -ne $actualRemainder -and [math]::Abs($actualRemainder - $expectedRemainder) -lt 0.02) `
        "expected $expectedRemainder MB, client reported $actualRemainder MB"
    Test-Case 'the resumed file matches the source' ((Get-Sha256 $storedBig) -eq $bigHash)

    # A partial file larger than the file being uploaded cannot be ours.
    $fs = [System.IO.File]::Open($storedBig, 'Open', 'Write')
    $fs.SetLength((Get-Item $big).Length + 1MB)
    $fs.Close()
    $r = Invoke-Ftp @('client', $big, '127.0.0.1', $Port)
    Test-Case 'a stale oversized partial is discarded and the file re-uploaded' `
        ($r.Code -eq 0 -and $r.Out -match 'Resuming from offset: 0' -and (Get-Sha256 $storedBig) -eq $bigHash) `
        "exit $($r.Code); stdout: $($r.Out -replace "`r?`n", ' | '); stderr: $($r.Err)"

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Interruption' -ForegroundColor Cyan
    Clear-Received
    $huge = Join-Path $work 'huge.bin'
    New-RandomFile -Path $huge -Megabytes 400
    $hugeHash = Get-Sha256 $huge
    $storedHuge = Join-Path $buildDirFull 'received_huge.bin'

    $killed = Start-FtpClient -Arguments @('client', $huge, '127.0.0.1', $Port) -Tag 'kill'
    # Kill the moment the payload starts moving, so that most of the file is
    # still outstanding.
    $found = Wait-ForServerLog -Pattern 'Accepting received_huge\.bin' -TimeoutSeconds 120
    if (-not $killed.HasExited) { $killed.Kill() }
    Test-Case 'the client was killed mid transfer' $found

    # The server can still be draining data that was already in flight when the
    # client was killed, so wait for the stored file to stop growing before
    # measuring it. Otherwise the offset compared below can be stale.
    $partial = 0
    $stableFor = 0
    $lastSize = -1
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $size = if (Test-Path -LiteralPath $storedHuge) { (Get-Item $storedHuge).Length } else { 0 }
        if ($size -eq $lastSize -and $size -gt 0) {
            $stableFor++
            if ($stableFor -ge 6) { $partial = $size; break }   # 6 x 250 ms
        } else {
            $stableFor = 0
        }
        $lastSize = $size
        Start-Sleep -Milliseconds 250
    }
    if ($partial -eq 0 -and $lastSize -gt 0) { $partial = $lastSize }
    Test-Case "a partial file was kept ($partial of $((Get-Item $huge).Length) bytes)" `
        ($partial -gt 0 -and $partial -lt (Get-Item $huge).Length)

    $r = Invoke-Ftp @('client', $huge, '127.0.0.1', $Port)
    Test-Case 'the transfer resumes after the interruption' ($r.Code -eq 0)
    Test-Case "the resume offset matches the partial file" ($r.Out -match "Resuming from offset: $partial")
    Test-Case 'the resumed 220 MB file matches the source' ((Get-Sha256 $storedHuge) -eq $hugeHash)

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Concurrency' -ForegroundColor Cyan
    Clear-Received

    # A large transfer is started and, while it is still running, a small one is
    # sent. The small transfer can only finish first if the server is not
    # blocked by the large one.
    $slow = Join-Path $work 'slow.bin'
    New-RandomFile -Path $slow -Megabytes 220
    $slowHash = Get-Sha256 $slow
    $storedSlow = Join-Path $buildDirFull 'received_slow.bin'

    $background = Start-FtpClient -Arguments @('client', $slow, '127.0.0.1', $Port) -Tag 'slow'
    $found = Wait-ForServerLog -Pattern 'Accepting received_slow\.bin' -TimeoutSeconds 90
    Test-Case 'the long transfer is in progress' $found

    $quick = Join-Path $work 'quick.bin'
    New-RandomFile -Path $quick -Megabytes 1
    $quickHash = Get-Sha256 $quick
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $r = Invoke-Ftp @('client', $quick, '127.0.0.1', $Port)
    $watch.Stop()
    Test-Case 'a second client is served while the first is transferring' ($r.Code -eq 0)
    Test-Case 'the second transfer finished quickly' ($watch.Elapsed.TotalSeconds -lt 30)
    Test-Case 'the second file is intact' ((Get-Sha256 (Join-Path $buildDirFull 'received_quick.bin')) -eq $quickHash)
    $background.WaitForExit(300000) | Out-Null
    Test-Case 'the long transfer also completed' ((Get-Sha256 $storedSlow) -eq $slowHash)

    # Two clients must not append to the same destination file. The destination
    # is derived from the base name only, so the second client uploads a *small*
    # file that happens to share the name: it then reaches the server almost
    # immediately, instead of spending seconds hashing a large file while the
    # first transfer finishes and releases its claim.
    Clear-Received
    $contestedDir = Join-Path $work 'contested'
    New-Item -ItemType Directory -Force -Path (Join-Path $contestedDir 'a') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $contestedDir 'b') | Out-Null
    $contested = Join-Path $contestedDir 'a\contested.bin'
    New-RandomFile -Path $contested -Megabytes 220
    $contestedHash = Get-Sha256 $contested
    $contestedRival = Join-Path $contestedDir 'b\contested.bin'
    New-RandomFile -Path $contestedRival -Megabytes 2
    $storedContested = Join-Path $buildDirFull 'received_contested.bin'

    $first = Start-FtpClient -Arguments @('client', $contested, '127.0.0.1', $Port) -Tag 'contest_a'
    $found = Wait-ForServerLog -Pattern 'Accepting received_contested\.bin' -TimeoutSeconds 90
    Test-Case 'the first client took the file' $found
    $second = Invoke-Ftp @('client', $contestedRival, '127.0.0.1', $Port)
    Test-Case 'a competing client for the same file is refused' ($second.Code -eq 1)
    Test-Case 'the refusal says the file is busy' ($second.Err -match 'busy')
    $first.WaitForExit(300000) | Out-Null
    Test-Case 'the first client still finished cleanly' ((Get-Sha256 $storedContested) -eq $contestedHash)

    # Several clients at once, each with its own file.
    Clear-Received
    $hashes = @{}
    $clients = @()
    $tags = @()
    foreach ($i in 1..4) {
        $path = Join-Path $work "parallel$i.bin"
        New-RandomFile -Path $path -Megabytes 12
        $hashes["received_parallel$i.bin"] = Get-Sha256 $path
        $tag = "par$i"
        $tags += $tag
        $clients += Start-FtpClient -Arguments @('client', $path, '127.0.0.1', $Port) -Tag $tag
    }
    $allOk = $true
    $details = @()
    for ($i = 0; $i -lt $clients.Count; $i++) {
        $clients[$i].WaitForExit(300000) | Out-Null
        if ($clients[$i].ExitCode -ne 0) {
            $allOk = $false
            $err = Get-Content -LiteralPath (Join-Path $work ($tags[$i] + '.err')) -Raw -ErrorAction SilentlyContinue
            $details += "$($tags[$i]) exit $($clients[$i].ExitCode): $err"
        }
    }
    Test-Case 'four parallel uploads all succeed' $allOk ($details -join ' | ')
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $buildDirFull $name
        Test-Case "$name is intact" ((Test-Path -LiteralPath $path) -and (Get-Sha256 $path) -eq $hashes[$name])
    }

    # ----------------------------------------------------------------------
    Write-Host ''
    Write-Host 'Server without a certificate' -ForegroundColor Cyan
    $bare = Join-Path $work 'bare'
    New-Item -ItemType Directory -Force -Path $bare | Out-Null
    Copy-Item $exe $bare -Force
    Get-ChildItem $buildDirFull -Filter '*.dll' | Copy-Item -Destination $bare -Force
    $p = Start-Process -FilePath (Join-Path $bare 'FTP.exe') -ArgumentList @('server', ($Port + 1)) `
            -PassThru -NoNewWindow -Wait -RedirectStandardOutput (Join-Path $work 'bare.out') `
            -RedirectStandardError (Join-Path $work 'bare.err') -WorkingDirectory $bare
    $bareErr = Get-Content (Join-Path $work 'bare.err') -Raw -ErrorAction SilentlyContinue
    Test-Case 'the server refuses to start without a certificate' `
        ($p.ExitCode -eq 1 -and $bareErr -match 'TLS assets not found')
}
finally {
    if ($server -and -not $server.HasExited) {
        $server.Kill()
        $server.WaitForExit(10000) | Out-Null
    }
    if ($KeepArtifacts) {
        Write-Host ''
        Write-Host "Artifacts kept in $work" -ForegroundColor DarkGray
    } else {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    Clear-Received
}

Write-Host ''
Write-Host ("Passed: {0}   Failed: {1}" -f $script:passed, $script:failed) -ForegroundColor $(if ($script:failed -eq 0) { 'Green' } else { 'Red' })
if ($script:failed -gt 0) {
    Write-Host ''
    foreach ($f in $script:failures) { Write-Host ("  - " + $f) -ForegroundColor Red }
    exit 1
}
exit 0
