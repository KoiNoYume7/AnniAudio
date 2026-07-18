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
    [int]$Port = 8080
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$RouteCli = "$RepoRoot\build\bin\Release\route_cli.exe"
$HtmlFile = "$PSScriptRoot\gui.html"
$ConfigFile = "$RepoRoot\config\cables.json"

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
    foreach ($line in $Lines) {
        if ($line -match '^\s*(\d+)\s+(RENDER|CAPTURE)\s+\[([^\]]+)\]\s+(.+?)\s*$') {
            $idx   = [int]$matches[1]
            $flow  = $matches[2]
            $type  = $matches[3]
            $nameRaw = $matches[4].Trim()
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
                isDefault = $isDefault
            }
        }
    }
    return $endpoints
}

function Get-Endpoints {
    $lines = Get-RouteCliList
    return Parse-Endpoints -Lines $lines
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

$guiHtml = Get-Content $HtmlFile -Raw

try {
    while ($listener.IsListening) {
        $ctx = $listener.GetContext()
        $req = $ctx.Request
        $path = $req.Url.AbsolutePath
        $method = $req.HttpMethod

        try {
            switch ($path) {
                '/' {
                    Write-HtmlResponse -Context $ctx -Html $guiHtml
                    continue
                }
                '/api/endpoints' {
                    $eps = Get-Endpoints
                    Write-JsonResponse -Context $ctx -Object @{ endpoints = $eps }
                    continue
                }
                '/api/groups' {
                    $groups = Get-Groups
                    Write-JsonResponse -Context $ctx -Object @{ groups = $groups }
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
