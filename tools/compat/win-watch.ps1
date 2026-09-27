# tacompat's eyes on a Windows desktop (tools/compat/tacompat.py, `windows`).
#
# Runs as a scheduled task in the logged-on user's session, because a process started
# over SSH cannot see the desktop's windows: from there every MainWindowTitle is empty.
# For -Seconds it writes one JSON line to -Out per new window of a TotalA.exe started
# from -Folder, or of a WerFault.exe (a crash report): title, window class, size, and for
# a dialog (#32770) the text of its static controls. Once the game has run a few seconds
# it writes the modules loaded from -Folder. It ends with {"done": true}, earlier when the
# game has exited.
param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Seconds = 40
)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class CompatWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    static string Text(IntPtr h) { var s = new StringBuilder(2048); GetWindowText(h, s, 2048); return s.ToString(); }
    static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static List<string[]> Windows(uint[] pids) {
        var r = new List<string[]>();
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (Array.IndexOf(pids, p) < 0 || !IsWindowVisible(h)) return true;
            RECT rc; GetWindowRect(h, out rc);
            string cls = Cls(h), body = "";
            if (cls == "#32770") {
                var parts = new List<string>();
                EnumChildWindows(h, (c, x) => { if (Cls(c) == "Static") { var t = Text(c); if (t.Length > 0) parts.Add(t); } return true; }, IntPtr.Zero);
                body = String.Join("\n", parts);
            }
            r.Add(new string[] { p.ToString(), Text(h), cls, (rc.R - rc.L) + "x" + (rc.B - rc.T), body });
            return true;
        }, IntPtr.Zero);
        return r;
    }
}
"@

function Emit($obj) { Add-Content -LiteralPath $Out -Value ($obj | ConvertTo-Json -Compress) -Encoding UTF8 }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null

$t0 = Get-Date
$seen = @{}
$modulesDone = $false
$everSeen = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $t = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $game = @(Get-Process TotalA -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Folder + '\', [StringComparison]::OrdinalIgnoreCase) })
    $wer = @(Get-Process WerFault -ErrorAction SilentlyContinue)
    if ($game.Count -gt 0) { $everSeen = $true }
    $pids = [uint32[]]@(@($game | ForEach-Object { $_.Id }) + @($wer | ForEach-Object { $_.Id }))
    foreach ($w in [CompatWin]::Windows($pids)) {
        $key = $w[1] + '|' + $w[2] + '|' + $w[4]
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            $isWer = @($wer | Where-Object { $_.Id -eq [int]$w[0] }).Count -gt 0
            Emit ([ordered]@{ t = $t; pid = [int]$w[0]; title = $w[1]; class = $w[2]; size = $w[3]; text = $w[4]; werfault = $isWer })
        }
    }
    if (-not $modulesDone -and $game.Count -gt 0 -and $t -ge 6) {
        $mods = @()
        try { $mods = @($game[0].Modules | Where-Object { $_.FileName.StartsWith($Folder + '\', [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { $_.FileName }) } catch { }
        Emit ([ordered]@{ t = $t; modules = $mods })
        $modulesDone = $true
    }
    if ($game.Count -eq 0 -and ($everSeen -or $t -ge 10)) { Emit ([ordered]@{ t = $t; exited = $true }); break }
    Start-Sleep -Milliseconds 700
}
Emit ([ordered]@{ t = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1); done = $true })
