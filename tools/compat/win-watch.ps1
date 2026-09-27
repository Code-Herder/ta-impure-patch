# tacompat's eyes on a Windows desktop (tools/compat/tacompat.py, `windows`).
#
# Runs as a scheduled task in the logged-on user's session, because a process started
# over SSH cannot see the desktop's windows: from there every MainWindowTitle is empty.
# For -Seconds it writes one JSON line to -Out per new window of a TotalA.exe started
# from -Folder, or of a WerFault.exe (a crash report): title, window class, size, and for
# a dialog (#32770) the text of its static controls. Once the game has run a few seconds it
# writes the modules loaded from -Folder, and a "code" event: the exe's executable sections
# read out of the live process and compared with TotalA.exe on disk, as the runs of bytes
# that differ with a little context each side, plus every module of -Folder with its extent
# and whether its file carries TADR's marker. THIS JUDGES NOTHING -- tacompat's decode_run
# decodes the runs, the same one for both platforms, so the two agree by construction
# (research/notes/compat/takeover.md, part 1). It ends with {"done": true}, earlier when the
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
using System.IO;
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
    // ---- the game's own code, out of the live process --------------------------------
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int n, out IntPtr got);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    const uint PROCESS_VM_READ = 0x0010, PROCESS_QUERY_INFORMATION = 0x0400;
    static readonly byte[] TadrMark = Encoding.ASCII.GetBytes("TADemo-MKChat");
    public static string LastWhy = null;      // set when nothing could be compared
    public static int LastSections = 0;
    public static long LastBase = 0;
    const int Ctx = 8;                        // must match tacompat.py's HOOK_CTX

    static bool Has(byte[] hay, byte[] needle) {
        for (int i = 0; i + needle.Length <= hay.Length; i++) {
            int k = 0;
            while (k < needle.Length && hay[i + k] == needle[k]) k++;
            if (k == needle.Length) return true;
        }
        return false;
    }

    public static bool IsTadr(string path) {
        try { return Has(File.ReadAllBytes(path), TadrMark); } catch { return false; }
    }

    // "run|<ctx va>|<first differing va>|<one past the last>|<memory hex>|<file hex>", the
    // records tacompat.py's decode_run reads. Nothing here decides anything.
    public static List<string> Runs(int pid, string exePath, long loadedBase) {
        var outp = new List<string>();
        LastWhy = null; LastSections = 0; LastBase = 0;
        byte[] file;
        try { file = File.ReadAllBytes(exePath); }
        catch (Exception e) { LastWhy = "the exe file could not be read: " + e.Message; return outp; }
        int pe;
        try {
            pe = BitConverter.ToInt32(file, 0x3C);
            if (file[0] != 'M' || file[1] != 'Z' || BitConverter.ToInt32(file, pe) != 0x00004550)
                { LastWhy = "the exe file is not a PE"; return outp; }
        } catch (Exception e) { LastWhy = "the exe file's header is unreadable: " + e.Message; return outp; }
        LastBase = (uint)BitConverter.ToInt32(file, pe + 24 + 28);
        // Relocated, or not the file that is loaded: every byte would differ and every
        // comparison would be noise. The same guard the Wine side makes.
        if (loadedBase != LastBase) {
            LastWhy = "the exe is loaded at 0x" + loadedBase.ToString("X8") + ", not the 0x"
                    + LastBase.ToString("X8") + " its file asks for";
            return outp;
        }
        int nsec = BitConverter.ToUInt16(file, pe + 6), opt = BitConverter.ToUInt16(file, pe + 20);
        IntPtr h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, pid);
        if (h == IntPtr.Zero) { LastWhy = "OpenProcess failed (error " + Marshal.GetLastWin32Error() + ")"; return outp; }
        try {
            for (int i = 0; i < nsec; i++) {
                int o = pe + 24 + opt + 40 * i;
                uint vsize = BitConverter.ToUInt32(file, o + 8), rva = BitConverter.ToUInt32(file, o + 12);
                uint rsize = BitConverter.ToUInt32(file, o + 16), raw = BitConverter.ToUInt32(file, o + 20);
                uint ch = BitConverter.ToUInt32(file, o + 36);
                long n = Math.Min(vsize, rsize);
                if ((ch & 0x20000020) == 0 || n == 0 || raw + n > file.Length) continue;
                long va = LastBase + rva;
                var mem = new byte[n];
                IntPtr got;
                if (!ReadProcessMemory(h, new IntPtr(va), mem, (int)n, out got) || (long)got != n) {
                    LastWhy = "ReadProcessMemory read " + (long)got + " of " + n + " bytes at 0x" + va.ToString("X8");
                    return outp;
                }
                LastSections++;
                long j = 0;
                while (j < n) {
                    if (mem[j] == file[raw + j]) { j++; continue; }
                    long lo = j;
                    while (j < n && mem[j] != file[raw + j]) j++;
                    long a = Math.Max(0, lo - Ctx), b = Math.Min(n, j + Ctx);
                    var sb = new StringBuilder("run|");
                    sb.Append((va + a).ToString("x") + "|" + (va + lo).ToString("x") + "|" + (va + j).ToString("x") + "|");
                    for (long k = a; k < b; k++) sb.Append(mem[k].ToString("x2"));
                    sb.Append("|");
                    for (long k = a; k < b; k++) sb.Append(file[raw + k].ToString("x2"));
                    outp.Add(sb.ToString());
                }
            }
            if (LastSections == 0 && LastWhy == null) LastWhy = "the exe file has no executable section";
        } finally { CloseHandle(h); }
        return outp;
    }

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

# -Depth: the code event nests modules and runs, and ConvertTo-Json truncates past 2.
function Emit($obj) { Add-Content -LiteralPath $Out -Value ($obj | ConvertTo-Json -Compress -Depth 8) -Encoding UTF8 }
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
        $ext = @()
        try {
            $own = @($game[0].Modules | Where-Object { $_.FileName.StartsWith($Folder + '\', [StringComparison]::OrdinalIgnoreCase) })
            $mods = @($own | ForEach-Object { $_.FileName })
            # Every module of the folder but the exe and Impure: a target inside one is what
            # a hook looks like. ModuleMemorySize is the SizeOfImage the loader used.
            $ext = @($own | Where-Object { $_.ModuleName -ne 'TotalA.exe' -and $_.ModuleName -ne 'ddraw.dll' } |
                ForEach-Object { [ordered]@{ name = $_.ModuleName; base = [int64]$_.BaseAddress
                                             size = [int64]$_.ModuleMemorySize
                                             tadr = [CompatWin]::IsTadr($_.FileName) } })
        } catch { }
        Emit ([ordered]@{ t = $t; modules = $mods })
        $runs = @()
        $why = 'the game was gone before its code could be read'
        $sections = 0
        $imgbase = 0
        try {
            $runs = @([CompatWin]::Runs($game[0].Id, (Join-Path $Folder 'TotalA.exe'),
                                        [int64]$game[0].MainModule.BaseAddress))
            $why = [CompatWin]::LastWhy
            $sections = [CompatWin]::LastSections
            $imgbase = [CompatWin]::LastBase
        } catch { $why = 'reading the game''s code threw: ' + $_.Exception.Message }
        Emit ([ordered]@{ t = $t; code = [ordered]@{ why = $why; sections = $sections
                                                     base = $imgbase; modules = $ext; runs = $runs } })
        $modulesDone = $true
    }
    if ($game.Count -eq 0 -and ($everSeen -or $t -ge 10)) { Emit ([ordered]@{ t = $t; exited = $true }); break }
    Start-Sleep -Milliseconds 700
}
Emit ([ordered]@{ t = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1); done = $true })
