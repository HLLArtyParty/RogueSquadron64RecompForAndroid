# Send N64-mapped keystrokes to the running game window via Win32 SendInput
# (scancodes, which is what SDL reads). The game window must exist; it is focused
# first. Keyboard input is on by default in this build, so no controller is needed.
#
#   pwsh tools/drive-input.ps1 -Keys "enter:100:1500,space:100:800"
#
# Each token is  name:holdMs:afterMs  (holdMs/afterMs optional; default 90/400).
# Default key map (matches project_keyboard_mouse_input): enter=Start, space=A(fire/confirm),
# lshift=B, lctrl=Z, e=Rtrig, q=Ltrig, wasd/arrows=stick, ijkl=C, esc.
param(
  [string]$Keys = "",
  [string]$WindowTitle = "Star Wars: Rogue Squadron 64 Recompiled"
)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class DrvIn {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KI ki; public int pad; }
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk; public ushort scan; public uint flags; public uint time; public IntPtr extra; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  public static void Key(ushort scan, bool ext, bool up){ INPUT[] a=new INPUT[1]; a[0].type=1; a[0].ki.scan=scan; a[0].ki.flags=8|(up?(uint)2:0)|(ext?(uint)1:0); SendInput(1,a,Marshal.SizeOf(typeof(INPUT))); }
  public static IntPtr Focus(string t){ IntPtr h=FindWindow(null,t); if(h!=IntPtr.Zero){ ShowWindow(h,9); SetForegroundWindow(h);} return h; }
}
"@
# scancode set 1 (scan, isExtended)
$sc = @{ 'enter'=@(0x1C,$false);'space'=@(0x39,$false);'esc'=@(0x01,$false);
  'up'=@(0x48,$true);'down'=@(0x50,$true);'left'=@(0x4B,$true);'right'=@(0x4D,$true);
  'w'=@(0x11,$false);'a'=@(0x1E,$false);'s'=@(0x1F,$false);'d'=@(0x20,$false);
  'q'=@(0x10,$false);'e'=@(0x12,$false);'i'=@(0x17,$false);'j'=@(0x24,$false);'k'=@(0x25,$false);'l'=@(0x26,$false);
  'lshift'=@(0x2A,$false);'lctrl'=@(0x1D,$false);'f1'=@(0x3B,$false);'f6'=@(0x40,$false); }

$h = [DrvIn]::Focus($WindowTitle)
if ($h -eq [IntPtr]::Zero) { Write-Output "WINDOW NOT FOUND: $WindowTitle"; exit 1 }
Start-Sleep -Milliseconds 300
foreach ($tok in ($Keys -split ',')) {
  if (-not $tok.Trim()) { continue }
  $p = $tok.Trim() -split ':'; $name = $p[0].ToLower()
  $hold  = if ($p.Count -gt 1) { [int]$p[1] } else { 90 }
  $after = if ($p.Count -gt 2) { [int]$p[2] } else { 400 }
  if (-not $sc.ContainsKey($name)) { Write-Output "UNKNOWN KEY: $name"; continue }
  $scan=[uint16]$sc[$name][0]; $ext=[bool]$sc[$name][1]
  [DrvIn]::Key($scan,$ext,$false); Start-Sleep -Milliseconds $hold; [DrvIn]::Key($scan,$ext,$true)
  Write-Output "pressed $name"; Start-Sleep -Milliseconds $after
}
