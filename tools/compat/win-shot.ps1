# The game window's client area from the screen's device context, saved as a PNG: what GDI's fallback
# shows a player, which the engine-surface screenshot cannot say. Runs in the logged-on user's session
# (Win.screen): a service session has no desktop. It reads a Vulkan swapchain as black on the Windows test
# setup, as PrintWindow does, and desktop duplication gets no frame at all, so only GDI's picture is judged.
param(
    [Parameter(Mandatory = $true)][string]$Folder,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
trap { Set-Content -LiteralPath "$Out.err" -Value "$_"; exit 1 }
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class CompatShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public static void Save(IntPtr hwnd, string path) {
        SetProcessDPIAware();
        RECT r; POINT o = new POINT();
        if (!GetClientRect(hwnd, out r) || !ClientToScreen(hwnd, ref o) || r.R <= 0 || r.B <= 0)
            throw new Exception("the game window has no client area");
        using (var b = new Bitmap(r.R, r.B, PixelFormat.Format24bppRgb))
        using (var g = Graphics.FromImage(b)) {
            g.CopyFromScreen(o.X, o.Y, 0, 0, new Size(r.R, r.B));
            b.Save(path, ImageFormat.Png);
        }
    }
}
"@
$p = Get-Process TotalA -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($Folder, [StringComparison]::OrdinalIgnoreCase) -and $_.MainWindowHandle -ne 0 } |
    Select-Object -First 1
if (-not $p) { throw "no game window for $Folder" }
$tmp = "$Out.tmp"
[CompatShot]::Save($p.MainWindowHandle, $tmp)
Move-Item -Force -LiteralPath $tmp -Destination $Out
