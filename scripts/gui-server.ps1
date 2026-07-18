<#
.SYNOPSIS
    Web-based GUI server for AnniAudio routing.
.DESCRIPTION
    Serves a browser UI on http://localhost:<Port> and exposes JSON APIs for
    listing endpoints, reading cable config, starting/stopping routes, and
    controlling per-route volume.
.PARAMETER Port
    TCP port to listen on (default 8080).
.EXAMPLE
    .\scripts\gui-server.ps1
    .\scripts\gui-server.ps1 -Port 9090
#>
param(
    [int]$Port = 18080
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$RouteCli = "$RepoRoot\build\bin\Release\route_cli.exe"
$HtmlFile = "$PSScriptRoot\gui.html"
$ConfigFile = "$RepoRoot\config\cables.json"

$AudioDll = "$PSScriptRoot\AnniAudio.Audio.dll"
if (Test-Path $AudioDll) {
    Add-Type -Path $AudioDll
} else {
    Write-Warning "AnniAudio.Audio.dll not found; per-app routing unavailable."
}

$AudioConfigModule = "$PSScriptRoot\modules\AudioConfig\1.0.0\AudioConfig.psd1"
if (Test-Path $AudioConfigModule) {
    Import-Module $AudioConfigModule -Force
} else {
    Write-Warning "AudioConfig module not found; endpoint and app listing unavailable."
}

if (!(Test-Path $RouteCli)) {
    Write-Error "route_cli.exe not found at $RouteCli. Run '.\cli\anniaudio.ps1 build' first."
    exit 1
}
if (!(Test-Path $HtmlFile)) {
    Write-Error "GUI HTML not found at $HtmlFile."
    exit 1
}

# Active routes table: key = routeId, value = @{ Process; Capture; Render; Volume; StartTime }
$script:routes = @{}
$script:routeLock = New-Object System.Object

function Get-RouteCliList {
    $output = & $RouteCli list 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "route_cli list failed: $output"
    }
    return $output
}

function Parse-Endpoints {
    param([string[]]$Lines)
    $endpoints = @()
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        if ($line -match '^\s*(\d+)\s+(RENDER|CAPTURE)\s+\[([^\]]+)\]\s+(.+?)\s*$') {
            $idx   = [int]$matches[1]
            $flow  = $matches[2]
            $type  = $matches[3]
            $nameRaw = $matches[4].Trim()
            $id = $null
            if ($i + 1 -lt $Lines.Count -and $Lines[$i+1] -match '^\s*(\S+)\s*$') {
                $id = $matches[1].Trim()
                $i++
            }
            $isDefault = $false
            $name = $nameRaw
            if ($name -match '^(.*?)\s+\(default\)$') {
                $name = $matches[1].Trim()
                $isDefault = $true
            }
            $endpoints += [PSCustomObject]@{
                index     = $idx
                flow      = $flow
                type      = $type
                name      = $name
                id        = $id
                isDefault = $isDefault
            }
        }
    }
    return $endpoints
}

function Get-Endpoints {
    $defaults = @{}
    try {
        $defaults[(Get-AudioDevice -DeviceType Playback -Role Console -ErrorAction SilentlyContinue).ID] = $true
        $defaults[(Get-AudioDevice -DeviceType Recording -Role Console -ErrorAction SilentlyContinue).ID] = $true
    } catch {}

    return Get-AudioDevice | ForEach-Object {
        $flow = if ($_.DeviceType -eq 'Playback') { 'RENDER' } else { 'CAPTURE' }
        [PSCustomObject]@{
            index     = 0
            flow      = $flow
            type      = $_.FormFactor
            name      = "$($_.Name) ($($_.DeviceName))"
            id        = $_.ID
            volume    = [math]::Round($_.VolumeLevel, 1)
            muted     = $_.Muted
            isDefault = $defaults.ContainsKey($_.ID)
        }
    }
}

function Get-Groups {
    # Groups are cables from config. We pair render/capture endpoints by the cable name.
    $cfg = Get-Content $ConfigFile -Raw | ConvertFrom-Json
    $endpoints = Get-Endpoints
    $groups = @()
    foreach ($cable in $cfg.cables | Where-Object { $_.enabled }) {
        $base = $cable.name
        $render = $endpoints | Where-Object { $_.flow -eq 'RENDER' -and $_.name -like "*$base*" } | Select-Object -First 1
        $capture = $endpoints | Where-Object { $_.flow -eq 'CAPTURE' -and $_.name -like "*$base*" } | Select-Object -First 1
        $groups += [PSCustomObject]@{
            id      = $cable.id
            name    = $base
            render  = $render
            capture = $capture
        }
    }
    return $groups
}

function Get-Apps {
    if (-not (Test-Path $AudioConfigModule)) { return @() }
    try {
        $seen = @{}
        $sessions = Get-AudioDevice | Get-AudioSession | Where-Object { $_.ProcessId -ne 0 }
        $result = foreach ($s in $sessions) {
            $appId = [int]$s.ProcessId
            if ($seen.ContainsKey($appId)) { continue }
            $seen[$appId] = $true
            $proc = $null
            try { $proc = Get-Process -Id $appId -ErrorAction SilentlyContinue } catch {}
            $name = if ($s.DisplayName) { $s.DisplayName } elseif ($proc) { $proc.ProcessName } else { "PID $appId" }
            [PSCustomObject]@{
                ProcessId      = $appId
                ProcessName    = $name
                DisplayName    = $s.DisplayName
                DeviceId       = $s.OutputDeviceId
                Volume         = [math]::Round($s.VolumeLevel, 1)
                Muted          = $s.Muted
            }
        }
        return $result
    } catch {
        Write-Warning "Get-Apps failed: $_"
        return @()
    }
}

function Set-AppOutput([int]$processId, [string]$deviceId) {
    if (-not (Test-Path $AudioDll)) { throw "Audio helper DLL not loaded" }
    $result = [AnniAudio.AudioPolicyHelper]::SetAppEndpoint($processId, $deviceId)
    if ($result -ne 'OK') { throw $result }
}

function Set-EndpointVolume([string]$deviceId, [int]$volume) {
    if (-not (Test-Path $AudioConfigModule)) { throw "AudioConfig module not loaded" }
    Set-AudioDevice -Device $deviceId -VolumeLevel $volume -ErrorAction Stop
}

function Start-RouteProcess {
    param([string]$Capture, [string]$Render, [int]$Volume)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $RouteCli
    $psi.Arguments = "route `"$Capture`" `"$Render`" $Volume"
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $started = $proc.Start()
    if (!$started) { throw "Failed to start route process." }

    # Give it a moment to fail fast (bad device name, etc.)
    Start-Sleep -Milliseconds 250
    if ($proc.HasExited -and $proc.ExitCode -ne 0) {
        throw "route_cli exited immediately with code $($proc.ExitCode)"
    }
    return $proc
}

function Write-JsonResponse {
    param($Context, $Object, $StatusCode = 200)
    $json = $Object | ConvertTo-Json -Depth 5
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $Context.Response.StatusCode = $StatusCode
    $Context.Response.ContentType = "application/json"
    $Context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    $Context.Response.Close()
}

function Write-HtmlResponse {
    param($Context, $Html)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Html)
    $Context.Response.StatusCode = 200
    $Context.Response.ContentType = "text/html"
    $Context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    $Context.Response.Close()
}

function Write-TextResponse {
    param($Context, $Text, $StatusCode = 200)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $Context.Response.StatusCode = $StatusCode
    $Context.Response.ContentType = "text/plain"
    $Context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    $Context.Response.Close()
}

function Get-RequestBody {
    param($Request)
    $reader = New-Object System.IO.StreamReader($Request.InputStream, $Request.ContentEncoding)
    $body = $reader.ReadToEnd()
    $reader.Dispose()
    if ([string]::IsNullOrWhiteSpace($body)) { return $null }
    try { return $body | ConvertFrom-Json } catch { return $null }
}

function Stop-AllRoutes {
    $script:routes.Values | ForEach-Object {
        try {
            if (!$_.Process.HasExited) {
                $_.Process.StandardInput.WriteLine("q")
                $_.Process.WaitForExit(2000)
                if (!$_.Process.HasExited) { $_.Process.Kill() }
            }
        } catch {}
    }
    $script:routes.Clear()
}

# Register cleanup on Ctrl+C / exit
try {
    $listener = New-Object System.Net.HttpListener
    $listener.Prefixes.Add("http://localhost:$Port/")
    $listener.Start()
} catch {
    Write-Error "Failed to start HTTP listener on port $Port`: $_"
    exit 1
}

$localUrl = "http://localhost:$Port/"
Write-Host "AnniAudio GUI server running at $localUrl" -ForegroundColor Green
Write-Host "Press Ctrl+C in this window to stop." -ForegroundColor Yellow

try {
    while ($listener.IsListening) {
        $ctx = $listener.GetContext()
        $req = $ctx.Request
        $path = $req.Url.AbsolutePath
        $method = $req.HttpMethod

        try {
            switch ($path) {
                '/' {
                    try {
                        $html = Get-Content $HtmlFile -Raw -ErrorAction Stop
                        Write-HtmlResponse -Context $ctx -Html $html
                    } catch {
                        Write-TextResponse -Context $ctx -Text "GUI HTML not found." -StatusCode 500
                    }
                    continue
                }
                '/api/endpoints' {
                    $eps = @(Get-Endpoints)
                    Write-JsonResponse -Context $ctx -Object @{ endpoints = $eps }
                    continue
                }
                '/api/groups' {
                    $groups = @(Get-Groups)
                    Write-JsonResponse -Context $ctx -Object @{ groups = $groups }
                    continue
                }
                '/api/apps' {
                    $apps = @(Get-Apps)
                    Write-JsonResponse -Context $ctx -Object @{ apps = $apps }
                    continue
                }
                '/api/routes' {
                    $list = @()
                    foreach ($kv in $script:routes.GetEnumerator()) {
                        $r = $kv.Value
                        $list += [PSCustomObject]@{
                            id        = $kv.Key
                            capture   = $r.Capture
                            render    = $r.Render
                            volume    = $r.Volume
                            running   = !$r.Process.HasExited
                            startTime = $r.StartTime
                        }
                    }
                    Write-JsonResponse -Context $ctx -Object @{ routes = $list }
                    continue
                }
                '/api/route/start' {
                    if ($method -ne 'POST') {
                        Write-TextResponse -Context $ctx -Text "POST required" -StatusCode 405
                        continue
                    }
                    $body = Get-RequestBody -Request $req
                    if (!$body -or !$body.capture -or !$body.render) {
                        Write-TextResponse -Context $ctx -Text "Missing capture or render" -StatusCode 400
                        continue
                    }
                    $vol = 100
                    if ($body.volume -and $body.volume -match '^\d+$') { $vol = [int]$body.volume }
                    if ($vol -lt 0) { $vol = 0 }
                    if ($vol -gt 200) { $vol = 200 }

                    $proc = Start-RouteProcess -Capture $body.capture -Render $body.render -Volume $vol
                    $id = [Guid]::NewGuid().ToString()
                    $script:routes[$id] = [PSCustomObject]@{
                        Process   = $proc
                        Capture   = $body.capture
                        Render    = $body.render
                        Volume    = $vol
                        StartTime = [DateTime]::Now.ToString("o")
                    }
                    Write-JsonResponse -Context $ctx -Object @{ id = $id; capture = $body.capture; render = $body.render; volume = $vol }
                    continue
                }
                '/api/route/stop' {
                    if ($method -ne 'POST') {
                        Write-TextResponse -Context $ctx -Text "POST required" -StatusCode 405
                        continue
                    }
                    $body = Get-RequestBody -Request $req
                    if (!$body -or !$body.id -or !$script:routes.ContainsKey($body.id)) {
                        Write-TextResponse -Context $ctx -Text "Unknown route id" -StatusCode 404
                        continue
                    }
                    $r = $script:routes[$body.id]
                    try {
                        if (!$r.Process.HasExited) {
                            $r.Process.StandardInput.WriteLine("q")
                            $r.Process.WaitForExit(2000)
                            if (!$r.Process.HasExited) { $r.Process.Kill() }
                        }
                    } catch {}
                    $script:routes.Remove($body.id)
                    Write-JsonResponse -Context $ctx -Object @{ stopped = $true }
                    continue
                }
                '/api/route/volume' {
                    if ($method -ne 'POST') {
                        Write-TextResponse -Context $ctx -Text "POST required" -StatusCode 405
                        continue
                    }
                    $body = Get-RequestBody -Request $req
                    if (!$body -or !$body.id -or !$script:routes.ContainsKey($body.id)) {
                        Write-TextResponse -Context $ctx -Text "Unknown route id" -StatusCode 404
                        continue
                    }
                    $vol = 100
                    if ($body.volume -and $body.volume -match '^\d+$') { $vol = [int]$body.volume }
                    if ($vol -lt 0) { $vol = 0 }
                    if ($vol -gt 200) { $vol = 200 }
                    $r = $script:routes[$body.id]
                    if (!$r.Process.HasExited) {
                        $r.Process.StandardInput.WriteLine("v $vol")
                        $r.Volume = $vol
                    }
                    Write-JsonResponse -Context $ctx -Object @{ id = $body.id; volume = $vol }
                    continue
                }
                '/api/route/app' {
                    if ($method -ne 'POST') {
                        Write-TextResponse -Context $ctx -Text "POST required" -StatusCode 405
                        continue
                    }
                    $body = Get-RequestBody -Request $req
                    if (!$body -or !$body.processId -or !$body.deviceId) {
                        Write-TextResponse -Context $ctx -Text "Missing processId or deviceId" -StatusCode 400
                        continue
                    }
                    Set-AppOutput -processId ([int]$body.processId) -deviceId $body.deviceId
                    Write-JsonResponse -Context $ctx -Object @{ ok = $true }
                    continue
                }
                '/api/volume/endpoint' {
                    if ($method -ne 'POST') {
                        Write-TextResponse -Context $ctx -Text "POST required" -StatusCode 405
                        continue
                    }
                    $body = Get-RequestBody -Request $req
                    if (!$body -or !$body.id -or $null -eq $body.volume) {
                        Write-TextResponse -Context $ctx -Text "Missing id or volume" -StatusCode 400
                        continue
                    }
                    $vol = [int]$body.volume
                    if ($vol -lt 0) { $vol = 0 }
                    if ($vol -gt 100) { $vol = 100 }
                    Set-EndpointVolume -deviceId $body.id -volume $vol
                    Write-JsonResponse -Context $ctx -Object @{ id = $body.id; volume = $vol }
                    continue
                }
                default {
                    Write-TextResponse -Context $ctx -Text "Not found" -StatusCode 404
                    continue
                }
            }
        } catch {
            Write-TextResponse -Context $ctx -Text "Server error: $_" -StatusCode 500
        }
    }
} finally {
    Stop-AllRoutes
    $listener.Stop()
    $listener.Close()
}
