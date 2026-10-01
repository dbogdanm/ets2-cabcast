# CabCast installer: finds Euro Truck Simulator 2, installs ReShade (with add-on support) if needed,
# the CabCast add-on and scrcpy (phone mirroring + adb). Run "Install CabCast.bat".
#   -Uninstall      remove CabCast (and ReShade, if this installer put it there)
#   -GameDir <dir>  use this bin\win_x64 folder instead of searching
param([switch]$Uninstall, [string]$GameDir, [switch]$NoPause)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ReShadeVersion = '6.8.0'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c }
function Step($t) { Write-Host ''; Write-Host "== $t" -ForegroundColor Cyan }
function Done { if (-not $NoPause) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null } }

function Find-Game {
    $roots = @()
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam', 'HKCU:\SOFTWARE\Valve\Steam') {
        try { $p = (Get-ItemProperty $k -ErrorAction Stop); if ($p.InstallPath) { $roots += $p.InstallPath }; if ($p.SteamPath) { $roots += $p.SteamPath } } catch {}
    }
    $libs = @()
    foreach ($r in $roots | Select-Object -Unique) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' } }
    }
    foreach ($l in $libs | Select-Object -Unique) {
        $d = Join-Path $l 'steamapps\common\Euro Truck Simulator 2\bin\win_x64'
        if (Test-Path (Join-Path $d 'eurotrucks2.exe')) { return (Resolve-Path $d).Path }
    }
    return $null
}

function Get-ReShadeDll($dir) {
    foreach ($n in 'dxgi.dll', 'd3d11.dll') {
        $f = Join-Path $dir $n
        if ((Test-Path $f) -and ((Get-Item $f).VersionInfo.ProductName -eq 'ReShade')) { return $f }
    }
    return $null
}

function Wait-GameClosed {
    if (Get-Process eurotrucks2 -ErrorAction SilentlyContinue) {
        Say 'Euro Truck Simulator 2 is running - close it to continue (waiting)...' Yellow
        while (Get-Process eurotrucks2 -ErrorAction SilentlyContinue) { Start-Sleep 2 }
        Start-Sleep 2
    }
}

try {
    Write-Host ''
    Write-Host '  CabCast - your phone / browser on the truck screen' -ForegroundColor White
    Write-Host '  ---------------------------------------------------'

    # ---------------------------------------------------------------- game
    Step 'Finding Euro Truck Simulator 2'
    $bin = if ($GameDir) { $GameDir } else { Find-Game }
    if (-not $bin -or -not (Test-Path $bin)) {
        Say 'Could not find the game automatically.' Yellow
        $g = Read-Host 'Paste the game folder (the one with bin\win_x64), or Enter to cancel'
        if (-not $g) { throw 'cancelled' }
        $bin = if (Test-Path (Join-Path $g 'bin\win_x64')) { Join-Path $g 'bin\win_x64' } else { $g }
        if (-not (Test-Path (Join-Path $bin 'eurotrucks2.exe'))) { throw "eurotrucks2.exe not found in $bin" }
    }
    Say "Game: $bin" Green
    $data = Join-Path $bin 'dbm_phone'
    $marker = Join-Path $data 'reshade_installed_by_dbm.txt'
    Wait-GameClosed

    # ---------------------------------------------------------------- uninstall
    if ($Uninstall) {
        Step 'Removing CabCast'
        Get-Process scrcpy, adb -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$data*" } | Stop-Process -Force -ErrorAction SilentlyContinue
        if (Test-Path $marker) {
            $rs = Get-Content $marker
            if ($rs -and (Test-Path (Join-Path $bin $rs))) { Remove-Item (Join-Path $bin $rs) -Force; Say "removed ReShade ($rs)" }
            if (Test-Path (Join-Path $bin "$rs.dbm-backup")) { Move-Item (Join-Path $bin "$rs.dbm-backup") (Join-Path $bin $rs) -Force; Say 'restored the previous ReShade' }
        }
        Remove-Item (Join-Path $bin 'dbm_phone.addon64') -Force -ErrorAction SilentlyContinue
        Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
        Say 'CabCast removed.' Green
        Done; exit 0
    }

    # ---------------------------------------------------------------- ReShade
    Step "ReShade $ReShadeVersion with add-on support"
    $tmp = Join-Path $env:TEMP 'dbm_phone_setup'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $setup = Join-Path $tmp "ReShade_Setup_${ReShadeVersion}_Addon.exe"
    if (-not (Test-Path $setup)) {
        Say 'downloading ReShade...'
        Invoke-WebRequest "https://reshade.me/downloads/ReShade_Setup_${ReShadeVersion}_Addon.exe" -OutFile $setup -UserAgent 'Mozilla/5.0' -Headers @{ Referer = 'https://reshade.me/' }
    }
    # the setup exe carries ReShade64.dll as a zip appended to it: cut the zip out (start = end-of-central-directory
    # position - central directory size - its offset), .NET can't read it inside the exe
    $bytes = [IO.File]::ReadAllBytes($setup)
    $eocd = -1
    for ($i = $bytes.Length - 22; $i -ge [Math]::Max(0, $bytes.Length - 65600); $i--) {
        if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4b -and $bytes[$i + 2] -eq 5 -and $bytes[$i + 3] -eq 6) { $eocd = $i; break }
    }
    if ($eocd -lt 0) { throw 'unexpected ReShade setup file' }
    $start = $eocd - [BitConverter]::ToUInt32($bytes, $eocd + 12) - [BitConverter]::ToUInt32($bytes, $eocd + 16)
    $payload = Join-Path $tmp 'reshade_payload.zip'
    $fs = [IO.File]::Create($payload); $fs.Write($bytes, $start, $bytes.Length - $start); $fs.Close()
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($payload)
    $newDll = Join-Path $tmp 'ReShade64.dll'
    try { $e = $zip.Entries | Where-Object { $_.FullName -eq 'ReShade64.dll' }; [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $newDll, $true) } finally { $zip.Dispose() }
    $newVer = [version](Get-Item $newDll).VersionInfo.FileVersion.Split(' ')[0]

    $cur = Get-ReShadeDll $bin
    if ($cur) {
        $curVer = [version](Get-Item $cur).VersionInfo.FileVersion.Split(' ')[0]
        $same = (Get-FileHash $cur).Hash -eq (Get-FileHash $newDll).Hash
        if ($same) { Say "ReShade $curVer (add-on version) already installed" Green }
        elseif ($curVer -gt $newVer) { Say "ReShade $curVer found (newer) - keeping it. It must be the 'with full add-on support' version." Yellow }
        else {
            # older or the build without add-ons: replace, keep a backup; settings and presets stay
            $name = Split-Path $cur -Leaf
            Copy-Item $cur (Join-Path $bin "$name.dbm-backup") -Force
            Copy-Item $newDll $cur -Force
            New-Item -ItemType Directory -Force $data | Out-Null
            if (-not (Test-Path $marker)) { Set-Content $marker $name }
            Say "ReShade $curVer updated to $newVer with add-on support (backup: $name.dbm-backup)" Green
        }
    } else {
        Copy-Item $newDll (Join-Path $bin 'dxgi.dll') -Force
        New-Item -ItemType Directory -Force $data | Out-Null
        Set-Content $marker 'dxgi.dll'
        Say "ReShade $newVer installed (dxgi.dll)" Green
    }

    # ---------------------------------------------------------------- add-on
    Step 'CabCast add-on'
    Copy-Item (Join-Path $Here 'files\dbm_phone.addon64') $bin -Force
    New-Item -ItemType Directory -Force $data | Out-Null
    Say 'dbm_phone.addon64 installed' Green

    # ---------------------------------------------------------------- scrcpy (+ adb)
    Step 'scrcpy (phone mirroring)'
    $scrDir = Join-Path $data 'scrcpy'
    if (Test-Path (Join-Path $scrDir 'scrcpy.exe')) { Say 'already installed' Green }
    else {
        Say 'downloading scrcpy...'
        $rel = Invoke-RestMethod 'https://api.github.com/repos/Genymobile/scrcpy/releases/latest' -UserAgent 'dbm-phone-installer'
        $asset = $rel.assets | Where-Object { $_.name -like 'scrcpy-win64-*.zip' } | Select-Object -First 1
        if (-not $asset) { throw 'scrcpy download not found on GitHub' }
        $sz = Join-Path $tmp $asset.name
        Invoke-WebRequest $asset.browser_download_url -OutFile $sz
        $ex = Join-Path $tmp 'scrcpy_x'
        Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive $sz $ex -Force
        $inner = Get-ChildItem $ex -Directory | Select-Object -First 1
        New-Item -ItemType Directory -Force $scrDir | Out-Null
        Copy-Item (Join-Path $inner.FullName '*') $scrDir -Recurse -Force
        Say "scrcpy $($rel.tag_name) installed" Green
    }

    # ---------------------------------------------------------------- browser
    Step 'Browser (for YouTube / Spotify / Maps on the screen)'
    $browser = $null
    foreach ($k in 'chrome.exe', 'msedge.exe') {
        foreach ($root in 'HKLM:', 'HKCU:') {
            try { $v = (Get-ItemProperty "$root\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$k" -ErrorAction Stop).'(default)'; if ($v -and (Test-Path $v)) { $browser = $v; break } } catch {}
        }
        if ($browser) { break }
    }
    if (-not $browser) { foreach ($c in "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Google\Chrome\Application\chrome.exe") { if (Test-Path $c) { $browser = $c; break } } }
    if ($browser) { Say "found: $browser" Green } else { Say 'Chrome / Edge not found - the PC browser source needs one of them (phone mode works without).' Yellow }

    # ---------------------------------------------------------------- done
    Step 'Done!'
    Say 'In the game:' White
    Say '  1. Home key opens ReShade -> Add-ons tab -> DBM CabCast'
    Say '  2. Sit in the cab, look at the navigation screen, click "Detect screen"'
    Say '  3. Pick the source: Android phone (USB) or PC browser'
    Say '  4. F10 = cursor on the screen (F10 twice = recalibrate), Esc = leave'
    Say ''
    Say 'Phone: Settings > About phone > tap "Build number" 7 times, then' White
    Say '       Settings > Developer options > USB debugging ON, plug in the cable, tap "Allow".'
    Say ''
    Say 'Uninstall: run "Uninstall CabCast.bat"'
    Done
}
catch {
    Say ''
    Say "Install failed: $($_.Exception.Message)" Red
    Done; exit 1
}
