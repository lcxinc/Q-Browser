[CmdletBinding()]
param(
    [string]$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64',
    [string]$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64',
    [string]$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe',
    [string]$BuildDirectory = '',
    [string]$DeploymentDirectory = '',
    [string]$TrustedRoot = '',
    [switch]$Clean,
    [bool]$RunAcceptance = $true,
    [string]$PrepareManualState = '',
    [ValidateSet('', 'BeforePublish')]
    [string]$FailureInjection = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('QBrowser.Task18.FileIdentity' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace QBrowser.Task18 {
  public static class FileIdentity {
    [StructLayout(LayoutKind.Sequential)] struct Info {
      public uint attributes; public System.Runtime.InteropServices.ComTypes.FILETIME creation;
      public System.Runtime.InteropServices.ComTypes.FILETIME access;
      public System.Runtime.InteropServices.ComTypes.FILETIME write;
      public uint volume; public uint sizeHigh; public uint sizeLow; public uint links;
      public uint indexHigh; public uint indexLow;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Info info);
    public static string Read(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      using (var handle = CreateFileW(native, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        Info info; if (!GetFileInformationByHandle(handle, out info))
          throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        return info.volume.ToString("x8") + ":" + info.indexHigh.ToString("x8") + info.indexLow.ToString("x8");
      }
    }
  }
  public static class ReparseDirectory {
    const uint MountPointTag = 0xA0000003;
    const uint OpenExisting = 3;
    const uint OpenReparsePoint = 0x00200000;
    const uint BackupSemantics = 0x02000000;
    const uint GetReparsePoint = 0x000900A8;
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
      IntPtr input, uint inputLength, byte[] output, uint outputLength,
      out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool RemoveDirectoryW(string path);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CreateSymbolicLinkW(string link, string target, uint flags);
    static string Native(string path) {
      string full = System.IO.Path.GetFullPath(path);
      return full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                     : @"\\?\" + full;
    }
    public static uint ReadTag(string path) {
      string full = System.IO.Path.GetFullPath(path);
      using (var handle = CreateFileW(Native(full), 0, 7, IntPtr.Zero,
                                     OpenExisting, OpenReparsePoint | BackupSemantics,
                                     IntPtr.Zero)) {
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        byte[] buffer = new byte[16384]; uint returned;
        if (!DeviceIoControl(handle, GetReparsePoint, IntPtr.Zero, 0,
                             buffer, (uint)buffer.Length, out returned, IntPtr.Zero))
          throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        if (returned < 8) throw new InvalidOperationException("Invalid reparse buffer: " + full);
        return BitConverter.ToUInt32(buffer, 0);
      }
    }
    public static void RemoveVerifiedMountPoint(string path) {
      string full = System.IO.Path.GetFullPath(path);
      if (ReadTag(full) != MountPointTag)
        throw new InvalidOperationException("Not a mount-point reparse entry: " + full);
      if (!RemoveDirectoryW(Native(full)))
        throw new Win32Exception(Marshal.GetLastWin32Error(), full);
    }
    public static void CreateFileSymbolicLink(string link, string target) {
      string linkFull = System.IO.Path.GetFullPath(link);
      string targetFull = System.IO.Path.GetFullPath(target);
      // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE keeps the adversarial
      // test runnable from Windows PowerShell 5.1 when Developer Mode allows
      // the same unelevated link creation used by modern PowerShell.
      if (!CreateSymbolicLinkW(Native(linkFull), Native(targetFull), 2))
        throw new Win32Exception(Marshal.GetLastWin32Error(), linkFull);
    }
  }
  public static class ProcessControl {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(
      uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr handle);
    [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr handle);
    static void Apply(int processId, bool suspend) {
      IntPtr handle = OpenProcess(0x0800, false, processId);
      if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
      try {
        int status = suspend ? NtSuspendProcess(handle) : NtResumeProcess(handle);
        if (status != 0) throw new Win32Exception("NTSTATUS 0x" + status.ToString("x8"));
      } finally { CloseHandle(handle); }
    }
    public static void Suspend(int processId) { Apply(processId, true); }
    public static void Resume(int processId) { Apply(processId, false); }
  }
  public sealed class DirectoryLease : IDisposable {
    readonly SafeFileHandle handle;
    public DirectoryLease(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      // Holding DELETE access while deliberately omitting FILE_SHARE_DELETE makes
      // later rename/delete opens conflict for the lifetime of this handle.
      handle = CreateFileW(native, 0x00010000, 3, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
      if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    public static int ProbeDenyDeleteLease(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      using (var probe = CreateFileW(native, 0x00010000, 3, IntPtr.Zero, 3,
                                     0x02000000, IntPtr.Zero)) {
        return probe.IsInvalid ? Marshal.GetLastWin32Error() : 0;
      }
    }
    public void Dispose() { handle.Dispose(); }
  }
  public sealed class ProcessLease : IDisposable {
    readonly SafeFileHandle handle;
    public int ProcessId { get; private set; }
    [DllImport("kernel32.dll", SetLastError=true)] static extern SafeFileHandle OpenProcess(
      uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] static extern uint GetProcessId(SafeFileHandle handle);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(
      SafeFileHandle handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateProcess(
      SafeFileHandle handle, uint exitCode);
    [DllImport("ntdll.dll")] static extern int NtSuspendProcess(SafeFileHandle handle);
    [DllImport("ntdll.dll")] static extern int NtResumeProcess(SafeFileHandle handle);
    public ProcessLease(int processId) {
      handle = OpenProcess(0x00100000 | 0x0800 | 0x1000 | 0x0001, false, processId);
      if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
      ProcessId = checked((int)GetProcessId(handle));
      if (ProcessId != processId) throw new Win32Exception("Process identity changed");
    }
    public bool IsAlive { get { return WaitForSingleObject(handle, 0) == 0x102; } }
    public void Suspend() { if (!IsAlive || NtSuspendProcess(handle) != 0) throw new Win32Exception("Suspend failed"); }
    public void Resume() { if (IsAlive && NtResumeProcess(handle) != 0) throw new Win32Exception("Resume failed"); }
    public void Terminate(uint code) { if (!IsAlive || !TerminateProcess(handle, code)) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    public void Dispose() { handle.Dispose(); }
  }
  public static class NativeAutomation {
    const int InputMouse = 0, InputKeyboard = 1;
    const uint KeyUp = 0x0002, Unicode = 0x0004;
    const uint LeftDown = 0x0002, LeftUp = 0x0004;
    const uint SrcCopy = 0x00CC0020;
    const uint WmCommand = 0x0111, WmClose = 0x0010, WmSetText = 0x000C;
    const uint BmClick = 0x00F5;
    const int IdOk = 1, IdCancel = 2, GaRoot = 2;
    [StructLayout(LayoutKind.Sequential)] struct Point { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] struct MouseInput {
      public int dx, dy; public uint mouseData, flags, time; public IntPtr extra;
    }
    [StructLayout(LayoutKind.Sequential)] struct KeyboardInput {
      public ushort virtualKey, scan; public uint flags, time; public IntPtr extra;
    }
    [StructLayout(LayoutKind.Explicit)] struct InputUnion {
      [FieldOffset(0)] public MouseInput mouse;
      [FieldOffset(0)] public KeyboardInput keyboard;
    }
    [StructLayout(LayoutKind.Sequential)] struct Input {
      public int type; public InputUnion value;
    }
    [StructLayout(LayoutKind.Sequential)] struct GuiThreadInfo {
      public uint size, flags; public IntPtr active, focus, capture, menuOwner,
        moveSize, caret; public Rect caretRect;
    }
    [StructLayout(LayoutKind.Sequential)] struct BitmapInfoHeader {
      public uint size; public int width, height; public ushort planes, bitCount;
      public uint compression, imageSize; public int xPels, yPels;
      public uint used, important;
    }
    [StructLayout(LayoutKind.Sequential)] struct BitmapInfo {
      public BitmapInfoHeader header; public uint colors;
    }
    delegate bool EnumWindow(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(
      IntPtr parent, EnumWindow callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(
      EnumWindow callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(
      IntPtr window, out uint processId);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr window);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool AttachThreadInput(
      uint first, uint second, bool attach);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(
      uint thread, ref GuiThreadInfo information);
    [DllImport("user32.dll")] static extern bool ClientToScreen(
      IntPtr window, ref Point point);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(
      uint count, Input[] inputs, int size);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(
      IntPtr window, System.Text.StringBuilder value, int maximum);
    [DllImport("user32.dll")] static extern bool PostMessageW(
      IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(
      IntPtr window, uint message, IntPtr wParam, string lParam);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("user32.dll", SetLastError=true)] static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll", SetLastError=true)] static extern bool CloseClipboard();
    [DllImport("user32.dll", SetLastError=true)] static extern bool EmptyClipboard();
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SetClipboardData(
      uint format, IntPtr memory);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr GlobalAlloc(
      uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr GlobalLock(IntPtr memory);
    [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr memory);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalFree(IntPtr memory);
    [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(
      IntPtr dc, int width, int height);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr value);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr destination,
      int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY,
      uint operation);
    [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc, IntPtr bitmap,
      uint start, uint lines, byte[] bits, ref BitmapInfo information, uint usage);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr value);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);

    static bool Send(Input[] inputs) {
      return inputs.Length > 0 && SendInput((uint)inputs.Length, inputs,
        Marshal.SizeOf(typeof(Input))) == inputs.Length;
    }
    public static IntPtr FindWorkerWindow(IntPtr host, int processId) {
      IntPtr found = IntPtr.Zero;
      EnumChildWindows(host, delegate(IntPtr candidate, IntPtr ignored) {
        uint pid; GetWindowThreadProcessId(candidate, out pid);
        Rect rect;
        if (pid == (uint)processId && IsWindowVisible(candidate)
            && GetClientRect(candidate, out rect)
            && rect.right - rect.left >= 900 && rect.bottom - rect.top >= 600) {
          found = candidate; return false;
        }
        return true;
      }, IntPtr.Zero);
      return found;
    }
    public static int ClientWidth(IntPtr window) {
      Rect rect;
      return IsWindow(window) && GetClientRect(window, out rect)
        ? rect.right - rect.left : -1;
    }
    public static int ClientHeight(IntPtr window) {
      Rect rect;
      return IsWindow(window) && GetClientRect(window, out rect)
        ? rect.bottom - rect.top : -1;
    }
    public static bool FocusWorker(IntPtr worker) {
      if (worker == IntPtr.Zero || !IsWindow(worker)) return false;
      IntPtr host = GetAncestor(worker, GaRoot);
      IntPtr foreground = GetForegroundWindow();
      uint ignored, current = GetCurrentThreadId();
      uint foregroundThread = foreground == IntPtr.Zero ? 0
        : GetWindowThreadProcessId(foreground, out ignored);
      uint workerThread = GetWindowThreadProcessId(worker, out ignored);
      bool attachedForeground = foregroundThread != 0 && foregroundThread != current
        && AttachThreadInput(current, foregroundThread, true);
      bool attachedWorker = workerThread != 0 && workerThread != current
        && AttachThreadInput(current, workerThread, true);
      BringWindowToTop(host); SetForegroundWindow(host); SetActiveWindow(host);
      SetFocus(worker);
      if (attachedWorker) AttachThreadInput(current, workerThread, false);
      if (attachedForeground) AttachThreadInput(current, foregroundThread, false);
      GuiThreadInfo information = new GuiThreadInfo();
      information.size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo));
      return GetAncestor(GetForegroundWindow(), GaRoot) == host
        && workerThread != 0 && GetGUIThreadInfo(workerThread, ref information)
        && information.focus != IntPtr.Zero
        && (information.focus == worker || IsChild(worker, information.focus));
    }
    public static bool Click(IntPtr worker, int x, int y) {
      if (!FocusWorker(worker)) return false;
      Point point = new Point(); point.x = x; point.y = y;
      if (!ClientToScreen(worker, ref point) || !SetCursorPos(point.x, point.y)) return false;
      Input down = new Input(); down.type = InputMouse; down.value.mouse.flags = LeftDown;
      Input up = new Input(); up.type = InputMouse; up.value.mouse.flags = LeftUp;
      return Send(new Input[] { down, up });
    }
    public static bool SendUnicodeText(IntPtr worker, string text) {
      foreach (char character in text) {
        if (!FocusWorker(worker)) return false;
        Input down = new Input(); down.type = InputKeyboard;
        down.value.keyboard.scan = character; down.value.keyboard.flags = Unicode;
        Input up = down; up.value.keyboard.flags = Unicode | KeyUp;
        if (!Send(new Input[] { down, up })) return false;
        System.Threading.Thread.Sleep(5);
      }
      return true;
    }
    public static bool SendKey(IntPtr worker, ushort key) {
      if (!FocusWorker(worker)) return false;
      Input down = new Input(); down.type = InputKeyboard;
      down.value.keyboard.virtualKey = key;
      Input up = down; up.value.keyboard.flags = KeyUp;
      return Send(new Input[] { down, up });
    }
    static bool IsDialog(IntPtr window, int processId) {
      uint pid; GetWindowThreadProcessId(window, out pid);
      System.Text.StringBuilder name = new System.Text.StringBuilder(32);
      return pid == (uint)processId && IsWindowVisible(window)
        && GetClassNameW(window, name, name.Capacity) > 0
        && name.ToString() == "#32770";
    }
    public static IntPtr FindFileDialog(int processId) {
      IntPtr found = IntPtr.Zero;
      EnumWindows(delegate(IntPtr candidate, IntPtr ignored) {
        if (IsDialog(candidate, processId)) { found = candidate; return false; }
        return true;
      }, IntPtr.Zero);
      return found;
    }
    public static int CancelFileDialogs(int processId) {
      int count = 0;
      EnumWindows(delegate(IntPtr candidate, IntPtr ignored) {
        if (IsDialog(candidate, processId)) {
          count++;
          PostMessageW(candidate, WmCommand, new IntPtr(IdCancel), IntPtr.Zero);
          PostMessageW(candidate, WmClose, IntPtr.Zero, IntPtr.Zero);
        }
        return true;
      }, IntPtr.Zero);
      return count;
    }
    public static int FileDialogCount(int processId) {
      int count = 0;
      EnumWindows(delegate(IntPtr candidate, IntPtr ignored) {
        if (IsDialog(candidate, processId)) count++;
        return true;
      }, IntPtr.Zero);
      return count;
    }
    public static bool AcceptFileDialog(int processId, string path) {
      IntPtr dialog = FindFileDialog(processId);
      if (dialog == IntPtr.Zero || !SetForegroundWindow(dialog)) return false;
      // Alt+N selects the native file-name editor regardless of locale.
      Input altDown = new Input(); altDown.type = InputKeyboard;
      altDown.value.keyboard.virtualKey = 0x12;
      Input nDown = new Input(); nDown.type = InputKeyboard;
      nDown.value.keyboard.virtualKey = 0x4e;
      Input nUp = nDown; nUp.value.keyboard.flags = KeyUp;
      Input altUp = altDown; altUp.value.keyboard.flags = KeyUp;
      if (!Send(new Input[] { altDown, nDown, nUp, altUp })) return false;
      System.Threading.Thread.Sleep(100);
      uint ignored; uint thread = GetWindowThreadProcessId(dialog, out ignored);
      GuiThreadInfo information = new GuiThreadInfo();
      information.size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo));
      if (!GetGUIThreadInfo(thread, ref information) || information.focus == IntPtr.Zero)
        return false;
      if (SendMessageW(information.focus, WmSetText, IntPtr.Zero, path) == IntPtr.Zero)
        return false;
      IntPtr outer = GetAncestor(information.focus, GaRoot);
      IntPtr accept = GetDlgItem(outer, IdOk);
      return accept != IntPtr.Zero
        && PostMessageW(accept, BmClick, IntPtr.Zero, IntPtr.Zero);
    }
    public static byte[] CaptureClient(IntPtr window) {
      Rect rect; if (!GetClientRect(window, out rect)) return null;
      int width = rect.right - rect.left, height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return null;
      Point origin = new Point();
      if (!ClientToScreen(window, ref origin)) return null;
      IntPtr source = GetDC(IntPtr.Zero), memory = IntPtr.Zero, bitmap = IntPtr.Zero,
        previous = IntPtr.Zero;
      try {
        if (source == IntPtr.Zero) return null;
        memory = CreateCompatibleDC(source);
        bitmap = CreateCompatibleBitmap(source, width, height);
        if (memory == IntPtr.Zero || bitmap == IntPtr.Zero) return null;
        previous = SelectObject(memory, bitmap);
        if (!BitBlt(memory, 0, 0, width, height, source, origin.x, origin.y, SrcCopy))
          return null;
        BitmapInfo information = new BitmapInfo();
        information.header.size = (uint)Marshal.SizeOf(typeof(BitmapInfoHeader));
        information.header.width = width; information.header.height = -height;
        information.header.planes = 1; information.header.bitCount = 32;
        byte[] bytes = new byte[checked(width * height * 4)];
        return GetDIBits(memory, bitmap, 0, (uint)height, bytes,
          ref information, 0) == height ? bytes : null;
      } finally {
        if (previous != IntPtr.Zero && memory != IntPtr.Zero) SelectObject(memory, previous);
        if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
        if (memory != IntPtr.Zero) DeleteDC(memory);
        if (source != IntPtr.Zero) ReleaseDC(IntPtr.Zero, source);
      }
    }
    public static int DifferentPixels(byte[] before, byte[] after) {
      if (before == null || after == null || before.Length != after.Length) return -1;
      int different = 0;
      for (int index = 0; index < before.Length; index += 16) {
        if (before[index] != after[index] || before[index + 1] != after[index + 1]
            || before[index + 2] != after[index + 2]) different++;
      }
      return different;
    }
    public static bool IsBluePixel(IntPtr window, int x, int y) {
      Point point = new Point(); point.x = x; point.y = y;
      if (!ClientToScreen(window, ref point)) return false;
      IntPtr dc = GetDC(IntPtr.Zero);
      if (dc == IntPtr.Zero) return false;
      try {
        uint color = GetPixel(dc, point.x, point.y);
        if (color == 0xffffffff) return false;
        int red = (int)(color & 0xff), green = (int)((color >> 8) & 0xff),
          blue = (int)((color >> 16) & 0xff);
        return blue > 140 && red < 100 && green < 150;
      } finally { ReleaseDC(IntPtr.Zero, dc); }
    }
    public static int DarkPixels(IntPtr window, int x, int y, int width, int height) {
      Point origin = new Point(); origin.x = x; origin.y = y;
      if (!ClientToScreen(window, ref origin)) return -1;
      IntPtr dc = GetDC(IntPtr.Zero); if (dc == IntPtr.Zero) return -1;
      try {
        int dark = 0;
        for (int row = origin.y; row < origin.y + height; row++) {
          for (int column = origin.x; column < origin.x + width; column++) {
            uint color = GetPixel(dc, column, row);
            if (color != 0xffffffff && (color & 0xff) < 100
                && ((color >> 8) & 0xff) < 100
                && ((color >> 16) & 0xff) < 100) dark++;
          }
        }
        return dark;
      } finally { ReleaseDC(IntPtr.Zero, dc); }
    }
    public static bool SetClipboardText(string text) {
      if (text == null || !OpenClipboard(IntPtr.Zero)) return false;
      IntPtr memory = IntPtr.Zero;
      try {
        if (!EmptyClipboard()) return false;
        byte[] bytes = System.Text.Encoding.Unicode.GetBytes(text + "\0");
        memory = GlobalAlloc(0x0002, new UIntPtr((uint)bytes.Length));
        if (memory == IntPtr.Zero) return false;
        IntPtr target = GlobalLock(memory);
        if (target == IntPtr.Zero) return false;
        try { Marshal.Copy(bytes, 0, target, bytes.Length); }
        finally { GlobalUnlock(memory); }
        if (SetClipboardData(13, memory) == IntPtr.Zero) return false;
        memory = IntPtr.Zero;
        return true;
      } finally {
        if (memory != IntPtr.Zero) GlobalFree(memory);
        CloseClipboard();
      }
    }
  }
}
'@
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoBuild = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
$localAppData = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData)
if ([string]::IsNullOrWhiteSpace($TrustedRoot)) {
    $TrustedRoot = Join-Path $localAppData 'QBrowserTask18'
}
$trusted = [IO.Path]::GetFullPath($TrustedRoot)
$trustedBuildRoot = Join-Path $trusted 'build'
$trustedWorkRoot = Join-Path $trusted 'work'
$trustedControlRoot = Join-Path $trusted 'control'
$sourceStage = Join-Path $trustedWorkRoot '.task18-release-source'
$defaultBuild = [IO.Path]::GetFullPath((Join-Path $trustedBuildRoot 'release'))
$defaultAcceptanceBuild = [IO.Path]::GetFullPath(
    (Join-Path $trustedBuildRoot 'release-acceptance'))
$defaultDeployment = [IO.Path]::GetFullPath((Join-Path $trusted 'release-deploy'))
$packageOutput = [IO.Path]::GetFullPath((Join-Path $trusted 'release-package'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = $defaultBuild }
if ([string]::IsNullOrWhiteSpace($DeploymentDirectory)) {
    $DeploymentDirectory = $defaultDeployment
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
$deployment = [IO.Path]::GetFullPath($DeploymentDirectory)
$ownedMarkerName = '.qbrowser-task18-owned'
$ownedMarkerText = "Q-BROWSER TASK18 OWNED v1`n"
$releaseMarkerText = "Q-BROWSER TASK18 RELEASE v1`n"

function Assert-ChildPath([string]$Path, [string]$Parent, [string]$Label) {
    $parentPrefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath($Path)
    if (-not $candidate.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must remain below $Parent"
    }
}

function Assert-NoReparseAncestor([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not (Test-Path -LiteralPath $candidate)) {
        $parent = [IO.Directory]::GetParent($candidate)
        if ($null -eq $parent) { throw "No existing ancestor for $Path" }
        $candidate = $parent.FullName
    }
    $item = Get-Item -LiteralPath $candidate -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse ancestor is forbidden: $($item.FullName)"
        }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}

function Get-PathIdentity([string]$Path) {
    return [QBrowser.Task18.FileIdentity]::Read($Path)
}

function Remove-VerifiedJunctionEntry([string]$Link, [string]$OwnedRoot,
        [string]$ExpectedTarget, [string]$Sentinel) {
    $linkFull = [IO.Path]::GetFullPath($Link)
    $ownedFull = [IO.Path]::GetFullPath($OwnedRoot)
    $expectedFull = (Resolve-Path -LiteralPath $ExpectedTarget -ErrorAction Stop).Path
    $sentinelFull = (Resolve-Path -LiteralPath $Sentinel -ErrorAction Stop).Path
    Assert-ChildPath $linkFull $ownedFull 'Junction entry'
    Assert-ChildPath $sentinelFull $expectedFull 'Junction target sentinel'
    Assert-NoReparseAncestor $ownedFull
    Assert-NoReparseAncestor (Split-Path -Parent $linkFull)
    $ownedIdentity = Get-PathIdentity $ownedFull
    $targetIdentity = Get-PathIdentity $expectedFull
    $sentinelIdentity = Get-PathIdentity $sentinelFull
    $sentinelHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinelFull).Hash
    $item = Get-Item -LiteralPath $linkFull -Force -ErrorAction Stop
    $targets = @($item.Target | ForEach-Object {
        (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($_)) -ErrorAction Stop).Path
    })
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -or
        $item.LinkType -ne 'Junction' -or $targets.Count -ne 1 -or
        -not $targets[0].Equals($expectedFull,
            [StringComparison]::OrdinalIgnoreCase) -or
        [QBrowser.Task18.ReparseDirectory]::ReadTag($linkFull) -ne
            [uint32]2684354563) {
        throw "Refusing to remove unexpected junction entry: $linkFull"
    }
    if ((Get-PathIdentity $ownedFull) -ne $ownedIdentity -or
        (Get-PathIdentity $expectedFull) -ne $targetIdentity -or
        (Get-PathIdentity $sentinelFull) -ne $sentinelIdentity -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinelFull).Hash -ne
            $sentinelHash) {
        throw "Junction ownership/target changed before removal: $linkFull"
    }
    [QBrowser.Task18.ReparseDirectory]::RemoveVerifiedMountPoint($linkFull)
    if (Test-Path -LiteralPath $linkFull -ErrorAction SilentlyContinue) {
        throw "Verified junction entry still exists after removal: $linkFull"
    }
    if ((Get-PathIdentity $ownedFull) -ne $ownedIdentity -or
        (Get-PathIdentity $expectedFull) -ne $targetIdentity -or
        (Get-PathIdentity $sentinelFull) -ne $sentinelIdentity -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinelFull).Hash -ne
            $sentinelHash) {
        throw "Junction target/sentinel changed during removal: $expectedFull"
    }
}

function Get-ProcessEnvironmentState([string]$Name) {
    return [pscustomobject]@{
        Exists = Test-Path -LiteralPath "Env:$Name"
        Value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    }
}

function Clear-ProcessEnvironmentValue([string]$Name) {
    # On this .NET runtime SetEnvironmentVariable(name, $null, Process) leaves
    # a present empty variable. Qt tests presence for its sandbox kill switch,
    # so use the environment provider to remove the entry from the native block.
    Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
}

function Restore-ProcessEnvironmentState([string]$Name, $State) {
    if ($State.Exists) {
        [Environment]::SetEnvironmentVariable($Name, [string]$State.Value, 'Process')
    }
    else { Clear-ProcessEnvironmentValue $Name }
}

function Test-ProcessEnvironmentState([string]$Name, $State) {
    $exists = Test-Path -LiteralPath "Env:$Name"
    if ($exists -ne [bool]$State.Exists) { return $false }
    if (-not $exists) { return $true }
    return [Environment]::GetEnvironmentVariable($Name, 'Process') -ceq
        [string]$State.Value
}

function New-OwnedDirectory([string]$Path) {
    Assert-NoReparseAncestor $Path
    if (Test-Path -LiteralPath $Path) { throw "Owned path already exists: $Path" }
    New-Item -ItemType Directory -Path $Path | Out-Null
    [IO.File]::WriteAllText((Join-Path $Path $ownedMarkerName), $ownedMarkerText,
        [Text.UTF8Encoding]::new($false))
}

function Remove-OwnedTree([string]$Path, [string]$ExactAllowedPath,
        [string]$MarkerName = $ownedMarkerName,
        [string]$MarkerText = $ownedMarkerText) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.Equals([IO.Path]::GetFullPath($ExactAllowedPath),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a non-owned Task18 path: $full"
    }
    if (-not (Test-Path -LiteralPath $full)) { return }
    Assert-NoReparseAncestor $full
    $root = Get-Item -LiteralPath $full -Force
    if (-not $root.PSIsContainer -or
        ($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Owned cleanup root is not a plain directory: $full"
    }
    $identity = Get-PathIdentity $full
    $marker = Join-Path $full $MarkerName
    $actualMarker = if (Test-Path -LiteralPath $marker -PathType Leaf) {
        (Get-Content -LiteralPath $marker -Raw) -replace "`r`n", "`n"
    } else { '' }
    if ($actualMarker -ne ($MarkerText -replace "`r`n", "`n")) {
        throw "Owned cleanup marker is absent or invalid: $full"
    }
    $entries = @(Get-ChildItem -LiteralPath $full -Force -Recurse)
    $entryIdentities = @{}
    foreach ($entry in $entries) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Owned cleanup tree contains a reparse point: $($entry.FullName)"
        }
        $entryIdentities[$entry.FullName] = Get-PathIdentity $entry.FullName
    }
    foreach ($file in @($entries | Where-Object {
            -not $_.PSIsContainer -and
            -not $_.FullName.Equals($marker, [StringComparison]::OrdinalIgnoreCase) } |
            Sort-Object { $_.FullName.Length } -Descending)) {
        if ((Get-PathIdentity $full) -ne $identity) {
            throw "Owned cleanup root identity changed: $full"
        }
        $current = Get-Item -LiteralPath $file.FullName -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-PathIdentity $file.FullName) -ne $entryIdentities[$file.FullName]) {
            throw "Cleanup member changed identity: $($file.FullName)"
        }
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            continue
        }
        catch {
            $memberDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
            $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $file.FullName, '/remove:d', '*S-1-1-0')
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $file.FullName, '/inheritance:r', '/grant:r',
                "${currentName}:F", '*S-1-5-18:F')
            $afterAcl = Get-Item -LiteralPath $file.FullName -Force
            if (($afterAcl.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                (Get-PathIdentity $file.FullName) -ne $entryIdentities[$file.FullName] -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash -ne
                    $memberDigest) {
                throw "Cleanup member identity/content changed during ACL recovery: $($file.FullName)"
            }
            [IO.File]::SetAttributes($file.FullName,
                $current.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
        }
        Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
    }
    foreach ($directory in @($entries | Where-Object { $_.PSIsContainer } |
            Sort-Object { $_.FullName.Length } -Descending)) {
        if ((Get-PathIdentity $full) -ne $identity) {
            throw "Owned cleanup root identity changed: $full"
        }
        $current = Get-Item -LiteralPath $directory.FullName -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-PathIdentity $directory.FullName) -ne
                $entryIdentities[$directory.FullName]) {
            throw "Cleanup member changed identity: $($directory.FullName)"
        }
        $memberIdentity = $entryIdentities[$directory.FullName]
        try {
            Remove-Item -LiteralPath $directory.FullName -Force -ErrorAction Stop
        }
        catch {
            $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $directory.FullName, '/remove:d', '*S-1-1-0')
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $directory.FullName, '/inheritance:r', '/grant:r',
                "${currentName}:F", '*S-1-5-18:F')
            $afterAcl = Get-Item -LiteralPath $directory.FullName -Force
            if (($afterAcl.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                (Get-PathIdentity $directory.FullName) -ne $memberIdentity -or
                (Get-PathIdentity $full) -ne $identity) {
                throw "Cleanup directory identity changed during ACL recovery: $($directory.FullName)"
            }
            Remove-Item -LiteralPath $directory.FullName -Force -ErrorAction Stop
        }
    }
    if ((Get-PathIdentity $full) -ne $identity) {
        throw "Owned cleanup root identity changed before removal: $full"
    }
    if ((Get-PathIdentity $marker) -ne $entryIdentities[$marker]) {
        throw "Owned cleanup marker identity changed: $marker"
    }
    Remove-Item -LiteralPath $marker -Force -ErrorAction Stop
    Remove-Item -LiteralPath $full -Force
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Invoke-Captured([string]$Program, [string[]]$Arguments) {
    $quoted = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' '
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Program
    $startInfo.Arguments = $quoted
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.WorkingDirectory = (Get-Location).Path
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output = $stdout.Result + $stderr.Result
    }
}

function Invoke-TrackedBuild([string[]]$Arguments) {
    $quoted = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' '
    # Windows PowerShell 5.1's Start-Process -PassThru can return a Process
    # whose ExitCode stays null even after WaitForExit.  Constructing the
    # process directly retains the native handle and gives an authoritative
    # exit code while we track only this invocation's descendants.
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $CMake
    $startInfo.Arguments = $quoted
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WorkingDirectory = (Get-Location).Path
    $process = [Diagnostics.Process]::Start($startInfo)
    $descendantIds = [Collections.Generic.HashSet[int]]::new()
    [void]$descendantIds.Add($process.Id)
    $ownedMsBuild = @{}
    do {
        $process.Refresh()
        $all = @(Get-CimInstance Win32_Process)
        $changed = $true
        while ($changed) {
            $changed = $false
            foreach ($candidate in $all) {
                if ($descendantIds.Contains([int]$candidate.ParentProcessId) -and
                    $descendantIds.Add([int]$candidate.ProcessId)) {
                    $changed = $true
                }
            }
        }
        foreach ($candidate in $all | Where-Object {
                $_.Name -ieq 'MSBuild.exe' -and
                $descendantIds.Contains([int]$_.ProcessId) }) {
            $identity = "$([int]$candidate.ProcessId)|$($candidate.CreationDate.ToUniversalTime().Ticks)"
            $ownedMsBuild[$identity] = $true
        }
        if (-not $process.HasExited) { Start-Sleep -Milliseconds 100 }
    } while (-not $process.HasExited)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "$CMake build failed with exit code $($process.ExitCode)"
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $remaining = @()
        foreach ($identity in $ownedMsBuild.Keys) {
            $parts = $identity -split '\|'
            $current = Get-CimInstance Win32_Process -Filter "ProcessId=$($parts[0])" `
                -ErrorAction SilentlyContinue
            if ($null -ne $current -and
                $current.CreationDate.ToUniversalTime().Ticks -eq [long]$parts[1]) {
                $remaining += $identity
            }
        }
        if ($remaining.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    } while ($timer.ElapsedMilliseconds -lt 30000)
    if ($remaining.Count -ne 0) {
        throw "Owned MSBuild nodes survived /nr:false: $($remaining -join ',')"
    }
    Write-Output "MSBUILD_NODE_REUSE_DISABLED=PASS ownedNodes=$($ownedMsBuild.Count) residue=0"
}

function Protect-Path([string]$Path, [switch]$Container) {
    Assert-NoReparseAncestor $Path
    $item = Get-Item -LiteralPath $Path -Force
    if ($Container -and -not $item.PSIsContainer) { throw "Expected directory: $Path" }
    if (-not $Container -and $item.PSIsContainer) { throw "Expected file: $Path" }
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $existingAcl = Get-Acl -LiteralPath $Path
    $trustedSids = @($current.Value, $system.Value)
    $identities = @($existingAcl.GetAccessRules($true, $false,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -notin $trustedSids } |
        ForEach-Object { $_.IdentityReference.Value } | Sort-Object -Unique)
    foreach ($identity in $identities) {
        Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
            $Path, '/remove:g', "*$identity")
    }
    $grants = if ($Container) {
        @("${currentName}:(OI)(CI)F", '*S-1-5-18:(OI)(CI)F')
    } else { @("${currentName}:F", '*S-1-5-18:F') }
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" `
        (@($Path, '/inheritance:r', '/grant:r') + $grants)
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $Path, '/setowner', $currentName)
    $verified = Get-Acl -LiteralPath $Path
    $allowed = @($current.Value, $system.Value)
    $ownerSid = ([Security.Principal.NTAccount]$verified.Owner).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    if (-not $verified.AreAccessRulesProtected -or $ownerSid -ne $current.Value -or
        @($verified.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -notin $allowed }).Count -ne 0) {
        throw "ACL sanitization failed: $Path"
    }
}

function Assert-ProtectedPath([string]$Path) {
    Assert-NoReparseAncestor $Path
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $trusted = @($current, 'S-1-5-18')
    $acl = Get-Acl -LiteralPath $Path
    $owner = ([Security.Principal.NTAccount]$acl.Owner).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    $untrustedAllow = @($acl.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
            $_.AccessControlType -eq 'Allow' -and
            $_.IdentityReference.Value -notin $trusted })
    if (-not $acl.AreAccessRulesProtected -or $owner -ne $current -or
        $untrustedAllow.Count -ne 0) {
        throw "Unsafe ACL: $Path"
    }
}

function Get-TrustedSidValues {
    $values = @(
        [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
        'S-1-5-18', 'S-1-5-32-544')
    try {
        $values += ([Security.Principal.NTAccount]'NT SERVICE\TrustedInstaller').Translate(
            [Security.Principal.SecurityIdentifier]).Value
    } catch {}
    return @($values | Sort-Object -Unique)
}

function Assert-TrustedAncestorChain([string]$Path, [string]$ManagedRoot = '') {
    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not (Test-Path -LiteralPath $candidate)) {
        $parent = [IO.Directory]::GetParent($candidate)
        if ($null -eq $parent) { throw "No existing trusted ancestor for $Path" }
        $candidate = $parent.FullName
    }
    $managed = if ([string]::IsNullOrWhiteSpace($ManagedRoot)) { '' }
        else { [IO.Path]::GetFullPath($ManagedRoot).TrimEnd('\') }
    $trustedSids = @(Get-TrustedSidValues)
    $replaceMask = [long]([Security.AccessControl.FileSystemRights]::Delete) -bor
        [long]([Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles) -bor
        [long]([Security.AccessControl.FileSystemRights]::ChangePermissions) -bor
        [long]([Security.AccessControl.FileSystemRights]::TakeOwnership)
    $writeMask = $replaceMask -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteData) -bor
        [long]([Security.AccessControl.FileSystemRights]::AppendData) -bor
        [long]([Security.AccessControl.FileSystemRights]::CreateFiles) -bor
        [long]([Security.AccessControl.FileSystemRights]::CreateDirectories) -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteAttributes) -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteExtendedAttributes)
    $item = Get-Item -LiteralPath $candidate -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Trusted ancestor is a reparse point: $($item.FullName)"
        }
        $acl = Get-Acl -LiteralPath $item.FullName
        $owner = ([Security.Principal.NTAccount]$acl.Owner).Translate(
            [Security.Principal.SecurityIdentifier]).Value
        if ($owner -notin $trustedSids) {
            throw "Trusted ancestor has an untrusted owner: $($item.FullName)"
        }
        $insideManaged = -not [string]::IsNullOrEmpty($managed) -and
            ($item.FullName.TrimEnd('\').Equals($managed,
                [StringComparison]::OrdinalIgnoreCase) -or
             $item.FullName.StartsWith($managed + '\',
                [StringComparison]::OrdinalIgnoreCase))
        if ($insideManaged -and -not $acl.AreAccessRulesProtected) {
            throw "Managed ancestor DACL is not protected: $($item.FullName)"
        }
        foreach ($rule in $acl.GetAccessRules($true, $true,
                [Security.Principal.SecurityIdentifier])) {
            if ($rule.AccessControlType -ne 'Allow' -or
                $rule.IdentityReference.Value -in $trustedSids -or
                ($rule.PropagationFlags -band
                    [Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) {
                continue
            }
            $effectiveMask = if ($insideManaged) { $writeMask } else { $replaceMask }
            if (([long]$rule.FileSystemRights -band $effectiveMask) -ne 0) {
                throw "Untrusted ancestor replacement/write ACE: $($item.FullName)"
            }
        }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}

function Assert-StableTrustedPath([string]$Path, [string]$Identity) {
    Assert-TrustedAncestorChain $Path $trusted
    if ((Get-PathIdentity $Path) -ne $Identity) {
        throw "Trusted path identity changed: $Path"
    }
}

function New-DirectoryLease([string]$Path) {
    $lease = [QBrowser.Task18.DirectoryLease]::new($Path)
    [void]$script:operationLeases.Add($lease)
    return $lease
}

function Dispose-OperationLeases {
    for ($index = $script:operationLeases.Count - 1; $index -ge 0; --$index) {
        $script:operationLeases[$index].Dispose()
    }
    $script:operationLeases.Clear()
}

function Assert-PlainTree([string]$Path) {
    Assert-NoReparseAncestor $Path
    foreach ($entry in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse member is forbidden: $($entry.FullName)"
        }
    }
}

function New-TrustedSourceSnapshot([string]$Destination) {
    New-OwnedDirectory $Destination
    Protect-Path $Destination -Container
    $lease = New-DirectoryLease $Destination
    $files = @(& git -C $repo ls-files)
    if ($LASTEXITCODE -ne 0 -or $files.Count -eq 0) {
        throw 'Unable to enumerate tracked Release source inputs.'
    }
    foreach ($relative in $files) {
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') {
            throw "Unsafe tracked source path: $relative"
        }
        $source = Join-Path $repo $relative
        $sourceItem = Get-Item -LiteralPath $source -Force
        if ($sourceItem.PSIsContainer -or
            ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Tracked Release source is not a plain file: $relative"
        }
        $target = Join-Path $Destination $relative
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
        Copy-Item -LiteralPath $source -Destination $target
    }
    Protect-Path $Destination -Container
    Assert-PlainTree $Destination
    Assert-TrustedAncestorChain $Destination $trusted
    return [pscustomobject]@{
        Lease = $lease
        Identity = Get-PathIdentity $Destination
        Count = $files.Count
    }
}

function Test-PrivatePem([string]$Path) {
    $pattern = [regex]::new(
        '(?m)^[ \t]*-----BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----')
    $reader = [IO.StreamReader]::new($Path, [Text.Encoding]::ASCII, $false, 65536)
    try {
        $buffer = [char[]]::new(65536); $carry = ''
        while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $text = $carry + [string]::new($buffer, 0, $count)
            if ($pattern.IsMatch($text)) { return $true }
            $carry = if ($text.Length -gt 128) {
                $text.Substring($text.Length - 128)
            } else { $text }
        }
        return $false
    }
    finally { $reader.Dispose() }
}

function Get-RawSecurityDescriptorHex([string]$Path) {
    $acl = Get-Acl -LiteralPath $Path
    $bytes = $acl.GetSecurityDescriptorBinaryForm()
    # BitConverter is available in Windows PowerShell 5.1/.NET Framework;
    # Convert.ToHexString is not.
    return [BitConverter]::ToString($bytes).Replace('-', '')
}

function Get-DeploymentSnapshot([string]$Root) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $items = @((Get-Item -LiteralPath $rootPath -Force)) +
        @(Get-ChildItem -LiteralPath $rootPath -Recurse -Force |
            Sort-Object FullName)
    $snapshot = foreach ($item in $items) {
        $relative = if ($item.FullName.Equals($rootPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            '.'
        }
        else { $item.FullName.Substring($rootPath.Length + 1).Replace('\', '/') }
        $acl = Get-RawSecurityDescriptorHex $item.FullName
        if ($item.PSIsContainer) { "D|$relative|$acl" }
        else {
            $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash
            "F|$relative|$($item.Length)|$digest|$acl"
        }
    }
    return ($snapshot -join "`n")
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs, [string]$Failure) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $TimeoutMs) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw $Failure
}

$script:operationLeases = [Collections.Generic.List[IDisposable]]::new()
$script:workerLeases = [Collections.Generic.List[IDisposable]]::new()
Assert-ChildPath $build $trusted 'BuildDirectory'
Assert-ChildPath $deployment $trusted 'DeploymentDirectory'
Assert-NoReparseAncestor $build
Assert-NoReparseAncestor $deployment
if ($build.Equals($deployment, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and DeploymentDirectory must be distinct.'
}
if ($Clean -and
    (-not $build.Equals($defaultBuild, [StringComparison]::OrdinalIgnoreCase) -or
     -not $deployment.Equals($defaultDeployment, [StringComparison]::OrdinalIgnoreCase))) {
    throw '-Clean is restricted to the exact trusted Task18 build/release paths.'
}

$deployScript = Join-Path $trustedControlRoot 'Deploy.cmake'
if ((Test-Path -LiteralPath $deployment) -and -not $Clean) {
    # Verification of an authoritative release is deliberately read-only. In
    # particular, do not call Protect-Path/icacls here: an already accepted
    # deployment must either pass exactly as published or fail closed.
    if (-not (Test-Path -LiteralPath $deployScript -PathType Leaf)) {
        throw "Trusted deployment verifier is missing: $deployScript"
    }
    Assert-NoReparseAncestor $trusted
    Assert-ProtectedPath $trusted
    Assert-ProtectedPath $trustedControlRoot
    Assert-ProtectedPath $deployScript
    Assert-TrustedAncestorChain $deployment $trusted
    [void](New-DirectoryLease $trusted)
    [void](New-DirectoryLease $trustedControlRoot)
    $deploymentIdentity = Get-PathIdentity $deployment
    $deploymentLease = New-DirectoryLease $deployment
    $before = Get-DeploymentSnapshot $deployment
    $verifierBefore = "$(Get-PathIdentity $deployScript)|" +
        "$(Get-RawSecurityDescriptorHex $deployScript)|" +
        (Get-FileHash -Algorithm SHA256 -LiteralPath $deployScript).Hash
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment", '-P', $deployScript)
    $after = Get-DeploymentSnapshot $deployment
    $verifierAfter = "$(Get-PathIdentity $deployScript)|" +
        "$(Get-RawSecurityDescriptorHex $deployScript)|" +
        (Get-FileHash -Algorithm SHA256 -LiteralPath $deployScript).Hash
    if ($before -ne $after -or $verifierBefore -ne $verifierAfter) {
        throw 'Read-only deployment verification changed deployment/verifier hash or ACL state.'
    }
    Assert-StableTrustedPath $deployment $deploymentIdentity
    Write-Output "Verified existing accepted deployment without mutation: $deployment"
    Dispose-OperationLeases
    return
}

Assert-TrustedAncestorChain $localAppData
Assert-NoReparseAncestor $trusted
New-Item -ItemType Directory -Path $trusted -Force | Out-Null
[void](New-DirectoryLease $trusted)
Protect-Path $trusted -Container
foreach ($managedParent in @($trustedBuildRoot, $trustedWorkRoot, $trustedControlRoot)) {
    New-Item -ItemType Directory -Path $managedParent -Force | Out-Null
    [void](New-DirectoryLease $managedParent)
    Protect-Path $managedParent -Container
}
Assert-TrustedAncestorChain $trusted $trusted
$trustedIdentity = Get-PathIdentity $trusted

if (-not [string]::IsNullOrWhiteSpace($PrepareManualState)) {
    $manualState = [IO.Path]::GetFullPath($PrepareManualState)
    $expectedManualState = [IO.Path]::GetFullPath(
        (Join-Path $trusted 'manual-deployed-smoke'))
    if (-not $manualState.Equals($expectedManualState,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manual state is restricted to $expectedManualState"
    }
    Assert-NoReparseAncestor $manualState
    New-Item -ItemType Directory -Path $manualState -Force | Out-Null
    Protect-Path $manualState -Container
    Assert-PlainTree $manualState
    foreach ($name in @('package-store','sandbox-temp','telemetry','storage')) {
        $directory = Join-Path $manualState $name
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Protect-Path $directory -Container
    }
    Assert-PlainTree $manualState
    Assert-TrustedAncestorChain $manualState $trusted
    Write-Output "Protected manual deployment state prepared: $manualState"
    Dispose-OperationLeases
    return
}

if ($Clean) {
    Remove-OwnedTree $build $defaultBuild
    Remove-OwnedTree $defaultAcceptanceBuild $defaultAcceptanceBuild
    Remove-OwnedTree $deployment $defaultDeployment '.qbrowser-release-root' $releaseMarkerText
    Remove-OwnedTree $packageOutput $packageOutput
    Remove-OwnedTree $sourceStage $sourceStage
}
elseif (Test-Path -LiteralPath $build) {
    throw "Build output already exists; inspect it before using -Clean: $build"
}

$runId = [Guid]::NewGuid().ToString('N')
$taskTemp = Join-Path $trustedWorkRoot ".task18-release-temp-$runId"
$staging = Join-Path $trustedWorkRoot ".task18-release-deploy-$runId"
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousPath = $env:PATH
$previousSourceDateEpoch = Get-ProcessEnvironmentState 'SOURCE_DATE_EPOCH'
$loaderEnvironmentNames = @('QML_IMPORT_PATH','QML2_IMPORT_PATH','QT_PLUGIN_PATH',
    'QT_QPA_PLATFORM_PLUGIN_PATH','QTWEBENGINEPROCESS_PATH',
    'QTWEBENGINE_RESOURCES_PATH','QTWEBENGINE_LOCALES_PATH',
    'QTWEBENGINE_DICTIONARIES_PATH','QTWEBENGINE_CHROMIUM_FLAGS',
    'QTWEBENGINE_DISABLE_SANDBOX','QTWEBENGINE_REMOTE_DEBUGGING',
    'OPENSSL_CONF','OPENSSL_MODULES','QTDIR',
    'QT_ROOT_DIR','Qt6_DIR','CMAKE_PREFIX_PATH')
$previousLoaderEnvironment = @{}
foreach ($name in $loaderEnvironmentNames) {
    $previousLoaderEnvironment[$name] = Get-ProcessEnvironmentState $name
}
$previousMsBuildNodeReuse = Get-ProcessEnvironmentState 'MSBUILDDISABLENODEREUSE'
$primaryFailure = $null
try {
    New-OwnedDirectory $taskTemp
    New-OwnedDirectory $staging
    Protect-Path $taskTemp -Container
    Protect-Path $staging -Container
    $taskTempLease = New-DirectoryLease $taskTemp
    $stagingLease = New-DirectoryLease $staging
    $taskTempIdentity = Get-PathIdentity $taskTemp
    $stagingIdentity = Get-PathIdentity $staging
    # Release configure/build, the deployment-only E2E, and the production
    # acceptance suite must all run independently of caller-controlled Qt/OpenSSL
    # loader state. Keep the exact caller snapshot above and restore it in finally.
    foreach ($name in $loaderEnvironmentNames) {
        Clear-ProcessEnvironmentValue $name
    }
    $node = (Get-Command node.exe -ErrorAction Stop).Source
    $env:TEMP = $taskTemp
    $env:TMP = $taskTemp
    $env:SOURCE_DATE_EPOCH = '946684800'
    $env:QTEST_FUNCTION_TIMEOUT = '900000'
    $env:MSBUILDDISABLENODEREUSE = '1'

function New-SignedUpdatePackage([string]$Version, [string]$Destination,
        [string]$MainQml = '', [switch]$ClipboardReadWithGesture) {
    $packageWork = Join-Path $taskTemp "package-work-$Version"
    $source = Join-Path $packageWork 'source'
    New-Item -ItemType Directory -Path $packageWork | Out-Null
    New-Item -ItemType Directory -Path $source | Out-Null
    $pilotSource = Join-Path $sourceStage 'packages\pilot'
    Assert-PlainTree $pilotSource
    foreach ($entry in Get-ChildItem -LiteralPath $pilotSource -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Pilot source contains a reparse point: $($entry.FullName)"
        }
        Copy-Item -LiteralPath $entry.FullName -Destination $source -Recurse
    }
    $manifestPath = Join-Path $source 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.version = $Version
    if ($ClipboardReadWithGesture) {
        $manifest.permissions.clipboardRead = 'user-gesture'
    }
    [IO.File]::WriteAllText($manifestPath,
        ($manifest | ConvertTo-Json -Depth 20 -Compress), [Text.UTF8Encoding]::new($false))
    if (-not [string]::IsNullOrEmpty($MainQml)) {
        [IO.File]::WriteAllText((Join-Path $source 'qml\Main.qml'), $MainQml,
            [Text.UTF8Encoding]::new($false))
    }
    # The task temp ancestor is deny-delete leased. Let qbrowser-package own the
    # exact unleased child root it uses for atomic output staging, separate from
    # its immutable source-tree root.
    $unsigned = Join-Path $packageWork "$Version.unsigned.qapkg"
    $cli = Join-Path $staging 'host\qbrowser-package.exe'
    $privateKey = Join-Path $trusted 'signing\private.pem'
    Invoke-Checked $cli @('pack', '--source', $source, '--output', $unsigned)
    Invoke-Checked $cli @('sign', '--package', $unsigned, '--private-key',
        $privateKey, '--output', $Destination)
    $inspection = & $cli inspect --package $Destination `
        --public-key (Join-Path $staging 'trust\dev-public.pem')
    if ($LASTEXITCODE -ne 0) { throw "Update $Version inspection failed." }
    $value = $inspection | ConvertFrom-Json
    if (-not $value.verified -or $value.appId -ne 'com.qbrowser.pilot' -or
        $value.version -ne $Version) {
        throw "Update $Version has unexpected signed identity."
    }
}

function Get-DeployedProcesses([string]$Name, [string]$ExpectedPath) {
    if ([string]::IsNullOrWhiteSpace($ExpectedPath)) {
        throw "Expected deployed process path is empty for $Name"
    }
    $normalized = [IO.Path]::GetFullPath($ExpectedPath)
    return @(Get-CimInstance Win32_Process -Filter "Name='$Name'" |
        Where-Object {
            if ([string]::IsNullOrWhiteSpace([string]$_.ExecutablePath)) { $false }
            else { try {
                [IO.Path]::GetFullPath([string]$_.ExecutablePath).Equals(
                    $normalized, [StringComparison]::OrdinalIgnoreCase)
            }
            catch { $false } }
        })
}

function Get-DeployedWorkerIdentity($Process) {
    return "$([int]$Process.ProcessId)|$($Process.CreationDate.ToUniversalTime().Ticks)"
}

function Assert-CurrentDeployedWorker($Worker) {
    if ($null -eq $Worker -or -not $Worker.Lease.IsAlive) {
        throw 'Bound deployed Worker process is no longer alive.'
    }
    $expected = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $current = @(Get-DeployedProcesses 'qbrowser-worker.exe' $expected)
    $matches = @($current | Where-Object {
        (Get-DeployedWorkerIdentity $_) -eq $Worker.Identity })
    if ($current.Count -ne 1 -or $matches.Count -ne 1) {
        throw 'Bound deployed Worker is no longer the unique current generation.'
    }
}

function Wait-DeployedWorker([string[]]$ExcludedIdentities = @()) {
    $expected = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $script:observedWorker = $null
    Wait-Until {
        $candidate = @(Get-DeployedProcesses 'qbrowser-worker.exe' $expected |
            Where-Object { (Get-DeployedWorkerIdentity $_) -notin $ExcludedIdentities }) |
            Sort-Object CreationDate -Descending | Select-Object -First 1
        if ($null -ne $candidate) {
            $lease = $null
            try {
                $lease = [QBrowser.Task18.ProcessLease]::new([int]$candidate.ProcessId)
                $bound = [pscustomobject]@{
                    ProcessId = [int]$candidate.ProcessId
                    CommandLine = [string]$candidate.CommandLine
                    CreationDate = $candidate.CreationDate
                    Identity = Get-DeployedWorkerIdentity $candidate
                    Lease = $lease
                }
                Assert-CurrentDeployedWorker $bound
                [void]$script:workerLeases.Add($lease)
                $script:observedWorker = $bound
                return $true
            }
            catch { if ($null -ne $lease) { $lease.Dispose() }; return $false }
        }
        return $false
    } 30000 'The deployed LPAC Worker did not start from the staged runtime.'
    return $script:observedWorker
}

function Start-DeployedHost([string]$MockOrigin, [string]$Store,
        [string]$Sandbox, [string]$Telemetry, [string]$Storage,
        [string]$InstallPackage,
        [int]$HealthWindowMs) {
    $arguments = @('--package-mode', "--mock-origin=$MockOrigin",
        '--app-id=com.qbrowser.pilot',
        "--trusted-public-key=$(Join-Path $staging 'trust\dev-public.pem')",
        "--package-store=$Store", "--sandbox-temp=$Sandbox",
        "--runtime-root=$(Join-Path $staging 'runtime')",
        "--worker-executable=$(Join-Path $staging 'runtime\qbrowser-worker.exe')",
        "--telemetry-directory=$Telemetry", "--storage-directory=$Storage",
        "--health-window-ms=$HealthWindowMs",
        '--heartbeat-timeout-ms=10000')
    if (-not [string]::IsNullOrEmpty($InstallPackage)) {
        $arguments += "--install-package=$InstallPackage"
    }
    $quoted = ($arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    $process = Start-Process -FilePath (Join-Path $staging 'host\qbrowser-host.exe') `
        -ArgumentList $quoted -PassThru
    Wait-Until { $process.Refresh(); $process.HasExited -or
        $process.MainWindowHandle -ne [IntPtr]::Zero } 30000 'Deployed Host window did not appear.'
    if ($process.HasExited) { throw "Deployed Host exited early: $($process.ExitCode)" }
    return $process
}

function Wait-Telemetry([string]$Telemetry, [string]$Pattern, [int]$TimeoutMs = 30000) {
    $file = Join-Path $Telemetry 'events.jsonl'
    Wait-Until { (Test-Path -LiteralPath $file -PathType Leaf) -and
        (Get-Content -LiteralPath $file -Raw) -match $Pattern } $TimeoutMs `
        "Telemetry did not contain required event: $Pattern"
}

function Wait-MockRequest([string]$MockOutput, [string]$Method,
        [string]$Target, [int]$TimeoutMs = 15000) {
    Wait-Until {
        if (-not (Test-Path -LiteralPath $MockOutput -PathType Leaf)) {
            return $false
        }
        foreach ($line in Get-Content -LiteralPath $MockOutput -ErrorAction SilentlyContinue) {
            try { $message = $line | ConvertFrom-Json } catch { continue }
            if ($null -ne $message.PSObject.Properties['request'] -and
                [string]$message.request.method -eq $Method -and
                [string]$message.request.target -eq $Target) {
                return $true
            }
        }
        return $false
    } $TimeoutMs "Mock API did not observe exact request: $Method $Target"
}

function Get-MockRequestCount([string]$MockOutput, [string]$Method,
        [string]$Target) {
    if (-not (Test-Path -LiteralPath $MockOutput -PathType Leaf)) { return 0 }
    $count = 0
    foreach ($line in Get-Content -LiteralPath $MockOutput -ErrorAction SilentlyContinue) {
        try { $message = $line | ConvertFrom-Json } catch { continue }
        if ($null -ne $message.PSObject.Properties['request'] -and
            [string]$message.request.method -eq $Method -and
            [string]$message.request.target -eq $Target) {
            ++$count
        }
    }
    return $count
}

function Wait-MockRequestAfter([string]$MockOutput, [string]$Method,
        [string]$Target, [int]$Before, [int]$TimeoutMs = 15000) {
    Wait-Until {
        (Get-MockRequestCount $MockOutput $Method $Target) -gt $Before
    } $TimeoutMs "Mock API did not observe a new exact request: $Method $Target"
}

function Get-RouteAckCount([string]$Telemetry, [string]$Template) {
    $eventFile = Join-Path $Telemetry 'events.jsonl'
    if (-not (Test-Path -LiteralPath $eventFile -PathType Leaf)) { return 0 }
    $pattern = '"phase":"worker","code":"completed".*' +
        '"routeTemplate":"' + [regex]::Escape($Template) + '".*"queueDepth":0'
    return @([regex]::Matches((Get-Content -LiteralPath $eventFile -Raw),
        $pattern)).Count
}

function New-DeployedHostAutomation([Diagnostics.Process]$Process) {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $Process.Refresh()
    $root = [System.Windows.Automation.AutomationElement]::FromHandle(
        [IntPtr]$Process.MainWindowHandle)
    if ($null -eq $root) { throw 'Windows UI Automation could not open the Host window.' }
    $editCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Edit)
    $address = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        $editCondition)
    $goCondition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, 'Go'))
    $go = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        $goCondition)
    if ($null -eq $address -or $null -eq $go) {
        throw 'Host navigation controls are unavailable to Windows UI Automation.'
    }
    return [pscustomobject]@{
        Root = $root
        Address = [System.Windows.Automation.ValuePattern]$address.GetCurrentPattern(
            [System.Windows.Automation.ValuePattern]::Pattern)
        Go = [System.Windows.Automation.InvokePattern]$go.GetCurrentPattern(
            [System.Windows.Automation.InvokePattern]::Pattern)
    }
}

function Invoke-DeployedNavigation($Automation, [string]$Route,
        [string]$Telemetry, [string]$Template) {
    $before = Get-RouteAckCount $Telemetry $Template
    $Automation.Address.SetValue($Route)
    $Automation.Go.Invoke()
    Wait-Until { $Automation.Address.Current.Value -eq $Route } 10000 `
        "Host address did not accept route $Route"
    Wait-Until { (Get-RouteAckCount $Telemetry $Template) -gt $before } 15000 `
        "Worker did not acknowledge normalized route with pending=0: $Route"
}

function Get-DeployedWorkerWindow([Diagnostics.Process]$HostProcess, $Worker) {
    $script:deployedWorkerWindow = [IntPtr]::Zero
    Wait-Until {
        $HostProcess.Refresh()
        $candidate = [QBrowser.Task18.NativeAutomation]::FindWorkerWindow(
            [IntPtr]$HostProcess.MainWindowHandle, [int]$Worker.ProcessId)
        if ($candidate -eq [IntPtr]::Zero) { return $false }
        $script:deployedWorkerWindow = $candidate
        return $true
    } 10000 'The deployed Worker native child window was not found.'
    return $script:deployedWorkerWindow
}

function Get-StorageValue([string]$Storage, [string]$Key) {
    foreach ($file in @(Get-ChildItem -LiteralPath $Storage -File -Filter '*.json' `
            -ErrorAction SilentlyContinue)) {
        try { $values = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json }
        catch { continue }
        $property = $values.PSObject.Properties[$Key]
        if ($null -ne $property) { return [string]$property.Value }
    }
    return $null
}

function Wait-StorageValue([string]$Storage, [string]$Key, [string]$Expected,
        [int]$TimeoutMs = 15000) {
    Wait-Until { (Get-StorageValue $Storage $Key) -eq $Expected } $TimeoutMs `
        "Storage did not persist exact value: $Key=$Expected"
}

function Invoke-DeployedPilotBusinessAcceptance([Diagnostics.Process]$HostProcess,
        $Worker, [string]$Telemetry, [string]$MockOutput, [string]$Storage,
        [string]$SelectedFile) {
    $automation = New-DeployedHostAutomation $HostProcess

    $loginBefore = Get-MockRequestCount $MockOutput 'POST' '/api/login'
    $dashboardBefore = Get-MockRequestCount $MockOutput 'GET' '/api/dashboard'
    Invoke-DeployedNavigation $automation 'app://pilot/login' $Telemetry '/login'
    $window = Get-DeployedWorkerWindow $HostProcess $Worker
    $clientWidth = [QBrowser.Task18.NativeAutomation]::ClientWidth($window)
    $clientHeight = [QBrowser.Task18.NativeAutomation]::ClientHeight($window)
    if ($clientWidth -lt 900 -or $clientHeight -lt 600) {
        throw "Deployed Worker client geometry is invalid: ${clientWidth}x${clientHeight}"
    }
    $scaleX = $clientWidth / 1100.0
    $scaleY = $clientHeight / 720.0
    $loginX = [int](550 * $scaleX)
    $loginY = [int](308 * $scaleY)
    $orderStatusX = [int](383 * $scaleX)
    $orderStatusY = [int](634 * $scaleY)
    $orderRegionX = [int](340 * $scaleX)
    $orderRegionY = [int](610 * $scaleY)
    $orderRegionWidth = [int](95 * $scaleX)
    $orderRegionHeight = [int](48 * $scaleY)
    $customerRegionX = [int](520 * $scaleX)
    $customerRegionY = [int](130 * $scaleY)
    $customerRegionWidth = [int](300 * $scaleX)
    $customerRegionHeight = [int](90 * $scaleY)
    $customerListX = [int](650 * $scaleX)
    $customerListY = [int](170 * $scaleY)
    $customerDetailX = [int](900 * $scaleX)
    $customerDetailY = [int](634 * $scaleY)
    $settingsX = [int](432 * $scaleX)
    $settingsY = [int](140 * $scaleY)
    $fileReadyX = [int](280 * $scaleX)
    $fileControlX = [int](312 * $scaleX)
    $fileControlY = [int](127 * $scaleY)
    $focused = $false
    1..3 | ForEach-Object {
        if (-not $focused) {
            $focused = [QBrowser.Task18.NativeAutomation]::Click(
                $window, $loginX, $loginY)
        }
    }
    if (-not $focused -or
        -not [QBrowser.Task18.NativeAutomation]::SendUnicodeText(
            $window, 'pilot@example.com') -or
        -not [QBrowser.Task18.NativeAutomation]::SendKey($window, 0x09) -or
        -not [QBrowser.Task18.NativeAutomation]::SendUnicodeText(
            $window, 'pilot-pass') -or
        -not [QBrowser.Task18.NativeAutomation]::SendKey($window, 0x09) -or
        -not [QBrowser.Task18.NativeAutomation]::SendKey($window, 0x0d)) {
        throw 'Real deployed login keyboard automation failed.'
    }
    Wait-MockRequestAfter $MockOutput 'POST' '/api/login' $loginBefore
    Wait-MockRequestAfter $MockOutput 'GET' '/api/dashboard' $dashboardBefore
    Write-Output 'DEPLOYMENT_BUSINESS_LOGIN=PASS realInput=1 successor=/dashboard'

    $orderGetBefore = Get-MockRequestCount $MockOutput 'GET' '/api/orders/ORD-1001'
    $orderPatchBefore = Get-MockRequestCount $MockOutput 'PATCH' '/api/orders/ORD-1001'
    Invoke-DeployedNavigation $automation 'app://pilot/orders/ORD-1001' `
        $Telemetry '/orders/:id'
    Wait-MockRequestAfter $MockOutput 'GET' '/api/orders/ORD-1001' $orderGetBefore
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::DarkPixels(
            $window, $orderRegionX, $orderRegionY,
            $orderRegionWidth, $orderRegionHeight) -ge 30
    } 10000 'Loaded order did not render an enabled processing mutation control.'
    if (-not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $orderStatusX, $orderStatusY)) {
        throw 'Real deployed order status click failed.'
    }
    Wait-MockRequestAfter $MockOutput 'PATCH' '/api/orders/ORD-1001' $orderPatchBefore
    Write-Output 'DEPLOYMENT_BUSINESS_ORDER=PASS GET=>PATCH status=processing'

    $customersBefore = Get-MockRequestCount $MockOutput 'GET' `
        '/api/customers?page=1&pageSize=20&query='
    $customerBefore = Get-MockRequestCount $MockOutput 'GET' '/api/customers/CUS-001'
    Invoke-DeployedNavigation $automation 'app://pilot/customers' $Telemetry '/customers'
    Wait-MockRequestAfter $MockOutput 'GET' `
        '/api/customers?page=1&pageSize=20&query=' $customersBefore
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::DarkPixels(
            $window, $customerRegionX, $customerRegionY,
            $customerRegionWidth, $customerRegionHeight) -ge 30
    } 10000 'Loaded customer list did not render a selectable entity.'
    if (-not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $customerListX, $customerListY) -or
        -not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $customerDetailX, $customerDetailY)) {
        throw 'Real deployed customer successor clicks failed.'
    }
    Wait-MockRequestAfter $MockOutput 'GET' '/api/customers/CUS-001' $customerBefore
    Wait-Until { (Get-RouteAckCount $Telemetry '/customers/:id') -gt 0 } 15000 `
        'Customer list response was not consumed into the detail successor route.'
    Write-Output 'DEPLOYMENT_BUSINESS_CUSTOMERS=PASS list=>detail CUS-001'

    Invoke-DeployedNavigation $automation 'app://pilot/settings' $Telemetry '/settings'
    if (-not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $settingsX, $settingsY)) {
        throw 'Real deployed dark-theme click failed.'
    }
    Wait-StorageValue $Storage 'theme' 'dark'
    Write-Output 'DEPLOYMENT_BUSINESS_STORAGE_SET=PASS theme=dark'

    Invoke-DeployedNavigation $automation 'app://pilot/files' $Telemetry '/files'
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::IsBluePixel(
            $window, $fileReadyX, $fileControlY)
    } 10000 'Deployed file control did not render.'
    if (-not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $fileControlX, $fileControlY)) {
        throw 'Real deployed file cancel click failed.'
    }
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::FileDialogCount($HostProcess.Id) -gt 0
    } 5000 'Native file cancel dialog did not open.'
    if ([QBrowser.Task18.NativeAutomation]::CancelFileDialogs($HostProcess.Id) -le 0) {
        throw 'Native file cancel dialog was not cancelled.'
    }
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::FileDialogCount($HostProcess.Id) -eq 0
    } 5000 'Native file cancel dialog remained open.'
    $beforeFile = [QBrowser.Task18.NativeAutomation]::CaptureClient($window)
    if ($null -eq $beforeFile -or
        -not [QBrowser.Task18.NativeAutomation]::Click(
            $window, $fileControlX, $fileControlY)) {
        throw 'Real deployed file success click failed.'
    }
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::FileDialogCount($HostProcess.Id) -gt 0
    } 5000 'Native file success dialog did not open.'
    if (-not [QBrowser.Task18.NativeAutomation]::AcceptFileDialog(
            $HostProcess.Id, $SelectedFile)) {
        throw 'Native file success dialog automation failed.'
    }
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::FileDialogCount($HostProcess.Id) -eq 0
    } 5000 'Native file success dialog remained open.'
    Wait-Until {
        $afterFile = [QBrowser.Task18.NativeAutomation]::CaptureClient($window)
        [QBrowser.Task18.NativeAutomation]::DifferentPixels(
            $beforeFile, $afterFile) -gt 2000
    } 10000 'Pilot QML did not consume the selected file metadata response.'
    Write-Output 'DEPLOYMENT_BUSINESS_FILE=PASS cancel=1 success=1 consumed=1'
}

function Assert-DeployedStoragePersistence([Diagnostics.Process]$HostProcess, $Worker,
        [string]$Telemetry, [string]$Storage) {
    $automation = New-DeployedHostAutomation $HostProcess
    Invoke-DeployedNavigation $automation 'app://pilot/settings' $Telemetry '/settings'
    $window = Get-DeployedWorkerWindow $HostProcess $Worker
    # Light is enabled only after the restarted Worker consumes the persisted
    # dark value. One non-retried side-effect click must persist the successor.
    if (-not [QBrowser.Task18.NativeAutomation]::Click($window, 328, 140)) {
        throw 'Real deployed persisted-theme successor click failed.'
    }
    Wait-StorageValue $Storage 'theme' 'light'
    Write-Output 'DEPLOYMENT_BUSINESS_STORAGE_RESTART=PASS dark=>restart=>light'
}

function Invoke-DeployedClipboardAcceptance([Diagnostics.Process]$HostProcess, $Worker,
        [string]$Telemetry, [string]$Storage) {
    $automation = New-DeployedHostAutomation $HostProcess
    Invoke-DeployedNavigation $automation 'app://pilot/dashboard' $Telemetry '/dashboard'
    $window = Get-DeployedWorkerWindow $HostProcess $Worker
    $clientWidth = [QBrowser.Task18.NativeAutomation]::ClientWidth($window)
    $clientHeight = [QBrowser.Task18.NativeAutomation]::ClientHeight($window)
    if ($clientWidth -lt 900 -or $clientHeight -lt 600) {
        throw "Deployed clipboard Worker geometry is invalid: ${clientWidth}x${clientHeight}"
    }
    $centerX = [int]($clientWidth / 2)
    $centerY = [int]($clientHeight / 2)
    Wait-StorageValue $Storage 'clipboard-no-gesture' 'clipboard.gesture_required'
    Wait-StorageValue $Storage 'clipboard-undeclared' 'capability.denied'
    Wait-Until {
        [QBrowser.Task18.NativeAutomation]::IsBluePixel($window, $centerX, $centerY)
    } 10000 'Deployed clipboard gesture control did not render.'
    if (-not [QBrowser.Task18.NativeAutomation]::SetClipboardText('gesture-canary') -or
        -not [QBrowser.Task18.NativeAutomation]::Click($window, $centerX, $centerY)) {
        throw 'Real deployed clipboard gesture input failed.'
    }
    Wait-StorageValue $Storage 'clipboard-gesture' `
        'first-ok:clipboard.gesture_required'
    Write-Output 'DEPLOYMENT_BUSINESS_CLIPBOARD=PASS expired=denied undeclared=denied gesture=once replay=denied'
}

function Stop-OwnedHost([Diagnostics.Process]$Process) {
    if ($Process.HasExited) { return }
    $started = [Diagnostics.Stopwatch]::StartNew()
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $processCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $Process.Id)
    $window = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
        [System.Windows.Automation.TreeScope]::Children, $processCondition)
    if ($null -eq $window) {
        throw "Exact deployed Host top-level window is unavailable: $($Process.Id)"
    }
    $windowPattern = [System.Windows.Automation.WindowPattern]$window.GetCurrentPattern(
        [System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    if ($Process.WaitForExit(60000)) {
        Write-Output "DEPLOYED_HOST_CLEAN_EXIT_MS=$($started.ElapsedMilliseconds)"
        return
    }
    $Process.Refresh()
    $expectedHost = [IO.Path]::GetFullPath((Join-Path $staging 'host\qbrowser-host.exe'))
    if ($Process.HasExited) { return }
    if ([string]::IsNullOrWhiteSpace($Process.Path) -or
        -not [IO.Path]::GetFullPath($Process.Path).Equals(
            $expectedHost, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Host PID identity changed while closing: $($Process.Id)"
    }
    Stop-Process -Id $Process.Id -Force
    if (-not $Process.WaitForExit(10000)) {
        throw "Exact deployed Host remained after forced test cleanup: $($Process.Id)"
    }
}

function Get-AclTreeSnapshot([string[]]$Roots) {
    $lines = foreach ($rootPath in $Roots) {
        $rootFull = [IO.Path]::GetFullPath($rootPath).TrimEnd('\')
        foreach ($item in @((Get-Item -LiteralPath $rootFull -Force)) +
                @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force |
                    Sort-Object FullName)) {
            $relative = $item.FullName.Substring($rootFull.Length).TrimStart('\').Replace('\','/')
            "$rootFull|$relative|$(Get-RawSecurityDescriptorHex $item.FullName)"
        }
    }
    return ($lines -join "`n")
}

function Assert-AclLeaseRestored([string]$Before, [string[]]$Roots, [string]$Label,
        [switch]$AllowAdditional, [switch]$IgnoreActivationLockLifecycle) {
    $after = Get-AclTreeSnapshot $Roots
    if ($AllowAdditional) {
        $afterLines = @($after -split "`n")
        foreach ($line in @($Before -split "`n")) {
            if ($IgnoreActivationLockLifecycle -and
                $line -match '\|apps/com\.qbrowser\.pilot/\.activation\.lock\|') {
                continue
            }
            if ($line -notin $afterLines) {
                throw "$Label changed a pre-existing ACL lease: $line"
            }
        }
    }
    elseif ($after -ne $Before) {
        $beforeLines = @($Before -split "`n")
        $afterLines = @($after -split "`n")
        foreach ($line in @($beforeLines | Where-Object { $_ -notin $afterLines } |
                Select-Object -First 10)) {
            Write-Output "ACL_LEASE_MISSING=$line"
        }
        foreach ($line in @($afterLines | Where-Object { $_ -notin $beforeLines } |
                Select-Object -First 10)) {
            Write-Output "ACL_LEASE_ADDED=$line"
        }
        throw "$Label ACL lease was not restored exactly."
    }
    if ($after -match 'S-1-15-2-') { throw "$Label retained an AppContainer SID ACE." }
}

function Invoke-DeployedRouteAcceptance([Diagnostics.Process]$AppProcess,
        [string]$Telemetry, $NegativeWorker = $null) {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    if (-not ('QBrowser.Task18.AsyncAutomation' -as [type])) {
        Add-Type -ReferencedAssemblies @(
            [System.Windows.Automation.AutomationElement].Assembly.Location,
            [System.Windows.Automation.InvokePattern].Assembly.Location,
            [System.Windows.Automation.AutomationPattern].Assembly.Location) -TypeDefinition @'
using System.Threading.Tasks;
using System.Windows.Automation;
namespace QBrowser.Task18 {
  public static class AsyncAutomation {
    public static Task InvokeAsync(AutomationElement element) {
      return Task.Run(() => {
        var pattern = (InvokePattern)element.GetCurrentPattern(InvokePattern.Pattern);
        pattern.Invoke();
      });
    }
  }
}
'@
    }
    $root = [System.Windows.Automation.AutomationElement]::FromHandle(
        [IntPtr]$AppProcess.MainWindowHandle)
    if ($null -eq $root) { throw 'Windows UI Automation could not open the Host window.' }
    $editCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Edit)
    $address = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        $editCondition)
    $goCondition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, 'Go'))
    $go = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $goCondition)
    if ($null -eq $address -or $null -eq $go) {
        throw 'Host navigation controls are unavailable to Windows UI Automation.'
    }
    $value = [System.Windows.Automation.ValuePattern]$address.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern)
    $invoke = [System.Windows.Automation.InvokePattern]$go.GetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern)
    $routes = @('app://pilot/login', 'app://pilot/dashboard',
        'app://pilot/orders', 'app://pilot/orders/ORD-1001',
        'app://pilot/orders/ORD-1001/edit', 'app://pilot/customers',
        'app://pilot/customers/CUS-001', 'app://pilot/files',
        'app://pilot/settings', 'app://pilot/web/help')
    $workerPath = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $templates = @{
        'app://pilot/login'='/login'; 'app://pilot/dashboard'='/dashboard'
        'app://pilot/orders'='/orders'; 'app://pilot/orders/ORD-1001'='/orders/:id'
        'app://pilot/orders/ORD-1001/edit'='/orders/:id/edit'
        'app://pilot/customers'='/customers'
        'app://pilot/customers/CUS-001'='/customers/:id'
        'app://pilot/files'='/files'; 'app://pilot/settings'='/settings'
    }
    $eventFile = Join-Path $Telemetry 'events.jsonl'
    if ($null -ne $NegativeWorker) {
        $negativeRoute = 'app://pilot/settings'
        $negativePattern = '"phase":"worker","code":"completed".*' +
            '"routeTemplate":"/settings".*"queueDepth":0'
        $before = if (Test-Path $eventFile) {
            @([regex]::Matches((Get-Content $eventFile -Raw), $negativePattern)).Count
        } else { 0 }
        Assert-CurrentDeployedWorker $NegativeWorker
        $NegativeWorker.Lease.Suspend()
        $missingAckDetected = $false
        $invokeTask = $null
        try {
            try {
                $value.SetValue($negativeRoute)
                $invokeTask = [QBrowser.Task18.AsyncAutomation]::InvokeAsync($go)
                try {
                    Wait-Until {
                        (Test-Path $eventFile) -and
                        @([regex]::Matches((Get-Content $eventFile -Raw), $negativePattern)).Count `
                            -gt $before
                    } 2000 'Injected ignored route produced no acknowledgement.'
                }
                catch { $missingAckDetected = $true }
            }
            catch {
                if ($_.Exception.ToString() -notmatch 'Operation timed out|0x80131505') {
                    throw
                }
                $missingAckDetected = $true
            }
        }
        finally {
            if (-not $NegativeWorker.Lease.IsAlive) {
                throw 'Negative ACK Worker generation restarted during injection.'
            }
            $NegativeWorker.Lease.Resume()
            Assert-CurrentDeployedWorker $NegativeWorker
            if ($null -ne $invokeTask -and -not $invokeTask.Wait(10000)) {
                throw 'Negative ACK UI Automation invocation did not finish after resume.'
            }
            if ($null -ne $invokeTask -and $invokeTask.IsFaulted) {
                throw $invokeTask.Exception
            }
        }
        if (-not $missingAckDetected) {
            throw 'Negative route injection was incorrectly accepted.'
        }
        Wait-Until {
            (Test-Path $eventFile) -and
            @([regex]::Matches((Get-Content $eventFile -Raw), $negativePattern)).Count `
                -gt $before
        } 15000 'Resumed Worker did not drain the negative route generation.'
        Write-Output 'DEPLOYMENT_ROUTE_NEGATIVE=PASS ignoredWorkerAck=rejected'
        return
    }
    foreach ($route in $routes) {
        $template = $templates[$route]
        $ackPattern = if ($null -ne $template) {
            '"phase":"worker","code":"completed".*"routeTemplate":"' +
                [regex]::Escape($template) + '".*"queueDepth":0'
        } else { $null }
        $ackBefore = if ($null -ne $ackPattern -and (Test-Path $eventFile)) {
            @([regex]::Matches((Get-Content $eventFile -Raw), $ackPattern)).Count
        } else { 0 }
        $value.SetValue($route)
        $invoke.Invoke()
        Wait-Until { $value.Current.Value -eq $route } 10000 `
            "Host address did not accept route $route"
        $surfaceName = if ($route -eq 'app://pilot/web/help') {
            'Q-Browser Pilot Help'
        } else { 'qbrowser-worker' }
        $nameCondition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $surfaceName)
        Wait-Until { $null -ne $root.FindFirst(
                [System.Windows.Automation.TreeScope]::Descendants, $nameCondition) } `
            30000 "Deployed route did not expose '$surfaceName': $route"
        if ($AppProcess.HasExited) { throw "Host exited while navigating $route" }
        if ($route -ne 'app://pilot/web/help' -and
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw "LPAC Worker was not alive for route $route"
        }
        if ($null -ne $ackPattern) {
            Wait-Until {
                (Test-Path $eventFile) -and
                @([regex]::Matches((Get-Content $eventFile -Raw), $ackPattern)).Count `
                    -gt $ackBefore
            } 15000 "Worker did not acknowledge normalized route with pending=0: $route"
        }
    }
    $helperPath = Join-Path $staging 'host\QtWebEngineProcess.exe'
    Wait-Until { @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $helperPath).Count -gt 0 } `
        15000 'The WebEngine route did not start the deployed helper.'
    $helper = @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $helperPath)
    foreach ($process in $helper) {
        Write-Output "DEPLOYED_WEBENGINE_COMMANDLINE=$($process.CommandLine)"
        if ($process.CommandLine -match [regex]::Escape((Join-Path $repo 'packages') + '\') -or
            $process.CommandLine -match [regex]::Escape($build + '\') -or
            $process.CommandLine -match [regex]::Escape((Join-Path $QtRoot 'bin') + '\') -or
            $process.CommandLine -match '(?i)--no-sandbox') {
            throw 'WebEngine helper command line contains a source path or disabled sandbox.'
        }
    }
    foreach ($resource in @('qtwebengine_resources.pak','icudtl.dat')) {
        if (-not (Test-Path -LiteralPath (Join-Path $staging "host\resources\$resource") `
                -PathType Leaf)) {
            throw "WebEngine resource is not deployed beside the bound helper: $resource"
        }
    }
    $value.SetValue('app://pilot/dashboard')
    $invoke.Invoke()
}

function Invoke-DeploymentOnlyE2E {
    $mockOut = Join-Path $taskTemp 'mock-api.stdout'
    $mockErr = Join-Path $taskTemp 'mock-api.stderr'
    $mock = $null
    $initialHostProcess = $null
    $secondHost = $null
    $thirdHost = $null
    $ownedPids = [Collections.Generic.HashSet[int]]::new()
    $workerPath = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $webEnginePath = Join-Path $staging 'host\QtWebEngineProcess.exe'
    try {
        foreach ($name in $loaderEnvironmentNames) {
            $poison = if ($name -eq 'QTWEBENGINE_DISABLE_SANDBOX') { '1' }
                elseif ($name -eq 'QTWEBENGINE_CHROMIUM_FLAGS') { '--no-sandbox' }
                else { 'C:\qbrowser-poisoned-loader-path' }
            [Environment]::SetEnvironmentVariable($name, $poison, 'Process')
        }
        foreach ($name in $loaderEnvironmentNames) {
            Clear-ProcessEnvironmentValue $name
        }
        if (@($loaderEnvironmentNames | Where-Object {
                Test-Path -LiteralPath "Env:$_" }).Count -ne 0) {
            throw 'Loader/security environment poison was not cleared.'
        }
        Write-Output "DEPLOYMENT_LOADER_ENV_CLEARED=$($loaderEnvironmentNames -join ',')"
        $mock = Start-Process -FilePath $node -ArgumentList 'src/server.ts' `
            -WorkingDirectory (Join-Path $sourceStage 'tools\mock-api') -WindowStyle Hidden `
            -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr -PassThru
        [void]$ownedPids.Add($mock.Id)
        $script:mockOrigin = $null
        Wait-Until {
            $mock.Refresh()
            if ($mock.HasExited) {
                throw "Mock API exited $($mock.ExitCode): $(Get-Content $mockErr -Raw -ErrorAction SilentlyContinue)"
            }
            if (-not (Test-Path -LiteralPath $mockOut -PathType Leaf)) { return $false }
            $line = Get-Content -LiteralPath $mockOut -First 1 -ErrorAction SilentlyContinue
            if ($line) {
                try { $script:mockOrigin = ($line | ConvertFrom-Json).origin } catch { return $false }
            }
            return $script:mockOrigin -match '^http://127\.0\.0\.1:[1-9][0-9]*$'
        } 15000 "Deployed mock API did not start: $(Get-Content $mockErr -Raw -ErrorAction SilentlyContinue)"

        $state = Join-Path $taskTemp 'deployment-e2e'
        $store = Join-Path $state 'package-store'
        $sandbox = Join-Path $state 'sandbox-temp'
        $telemetry = Join-Path $state 'telemetry'
        $storage = Join-Path $state 'storage'
        New-Item -ItemType Directory -Path $store, $sandbox, $telemetry, $storage | Out-Null
        Protect-Path $store -Container
        Protect-Path $sandbox -Container
        Protect-Path $telemetry -Container
        Protect-Path $storage -Container
        $minimalPath = "$(Join-Path $staging 'host');$env:SystemRoot\System32;$env:SystemRoot"
        $env:PATH = $minimalPath
        $pilot = Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'
        $deployAclRoots = @((Join-Path $staging 'runtime'), (Join-Path $staging 'packages'))
        $deployAclBefore = Get-AclTreeSnapshot $deployAclRoots
        $initialHostProcess = Start-DeployedHost $script:mockOrigin $store $sandbox $telemetry $storage $pilot 1000
        [void]$ownedPids.Add($initialHostProcess.Id)
        $worker = Wait-DeployedWorker
        [void]$ownedPids.Add([int]$worker.ProcessId)
        if ($worker.CommandLine -match [regex]::Escape($QtRoot + '\bin') -or
            $worker.CommandLine -match [regex]::Escape((Join-Path $repo 'packages'))) {
            throw 'LPAC Worker command line contains an implicit source runtime path.'
        }
        Wait-Telemetry $telemetry '"packageVersion":"1\.0\.0","phase":"health","code":"healthy"' 30000
        $selectedFile = Join-Path $taskTemp 'deployment-selected-pilot-note.txt'
        [IO.File]::WriteAllText($selectedFile, 'pilot-file',
            [Text.UTF8Encoding]::new($false))
        Protect-Path $selectedFile
        Invoke-DeployedPilotBusinessAcceptance $initialHostProcess $worker `
            $telemetry $mockOut $storage $selectedFile
        1..10 | ForEach-Object {
            Invoke-DeployedRouteAcceptance $initialHostProcess $telemetry `
                -NegativeWorker $worker
            Write-Output "DEPLOYMENT_ROUTE_NEGATIVE_REPEAT=$_/10 identity=$($worker.Identity)"
        }
        Stop-OwnedHost $initialHostProcess
        if ($initialHostProcess.ExitCode -ne 0) {
            throw "Initial deployed Host cleanup exited $($initialHostProcess.ExitCode)."
        }
        Wait-Until { @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 } `
            15000 'Initial deployed Worker remained after Host shutdown.'
        Assert-AclLeaseRestored $deployAclBefore $deployAclRoots 'Initial Host'
        $storeAclBefore = Get-AclTreeSnapshot @($store)

        $candidateRoot = Join-Path $taskTemp 'candidate'
        New-Item -ItemType Directory -Path $candidateRoot | Out-Null
        Protect-Path $candidateRoot -Container
        $update = Join-Path $candidateRoot 'com.qbrowser.pilot-1.1.0.qapkg'
        New-SignedUpdatePackage '1.1.0' $update
        Protect-Path $update
        $env:PATH = "C:\polluted-does-not-exist;$minimalPath"
        $secondHost = Start-DeployedHost $script:mockOrigin $store $sandbox $telemetry $storage $update 60000
        [void]$ownedPids.Add($secondHost.Id)
        $firstUpdateWorker = Wait-DeployedWorker
        [void]$ownedPids.Add([int]$firstUpdateWorker.ProcessId)
        $activation = Join-Path $store 'apps\com.qbrowser.pilot\activation.json'
        Wait-Until {
            (Test-Path -LiteralPath $activation -PathType Leaf) -and
            ((Get-Content -LiteralPath $activation -Raw | ConvertFrom-Json).current `
                -like 'versions/1.1.0-*')
        } 30000 'Signed 1.1.0 update was not activated.'
        Start-Sleep -Seconds 5
        if ($secondHost.HasExited -or
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Signed 1.1.0 candidate was not stably running before crash injection.'
        }
        Assert-CurrentDeployedWorker $firstUpdateWorker
        $firstUpdateWorker.Lease.Terminate(88)
        Wait-Until { -not $firstUpdateWorker.Lease.IsAlive } 10000 `
            'First crashed Worker generation did not exit.'
        $restartedWorker = Wait-DeployedWorker @($firstUpdateWorker.Identity)
        [void]$ownedPids.Add([int]$restartedWorker.ProcessId)
        Wait-Telemetry $telemetry '"packageVersion":"1\.1\.0","phase":"worker","code":"restarted"' 30000
        if (((Get-Content -LiteralPath $activation -Raw | ConvertFrom-Json).current `
                -notlike 'versions/1.1.0-*')) {
            throw 'The first crash did not retain the signed 1.1.0 candidate.'
        }
        Start-Sleep -Seconds 5
        if ($secondHost.HasExited -or
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Restarted 1.1.0 Worker was not stable before the second crash.'
        }
        Assert-CurrentDeployedWorker $restartedWorker
        $restartedWorker.Lease.Terminate(89)
        Wait-Until { -not $restartedWorker.Lease.IsAlive } 10000 `
            'Second crashed Worker generation did not exit.'
        Wait-Telemetry $telemetry '"packageVersion":"1\.0\.0","phase":"rollback","code":"recovered"' 60000
        Wait-Until { ((Get-Content -LiteralPath $activation -Raw |
                ConvertFrom-Json).current -like 'versions/1.0.0-*') } 30000 `
            'Double crash did not recover the 1.0.0 last-known-good package.'
        $recoveryWorker = Wait-DeployedWorker @(
            $firstUpdateWorker.Identity, $restartedWorker.Identity)
        [void]$ownedPids.Add([int]$recoveryWorker.ProcessId)
        if ($secondHost.HasExited) { throw 'Host exited during Worker crash recovery.' }
        Start-Sleep -Seconds 5
        if (@(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Recovered 1.0.0 Worker was not stable before route acceptance.'
        }
        Assert-DeployedStoragePersistence $secondHost $recoveryWorker $telemetry $storage
        Invoke-DeployedRouteAcceptance $secondHost $telemetry
        foreach ($request in @(
                [pscustomobject]@{ Method = 'GET'; Target = '/api/dashboard' }
                [pscustomobject]@{ Method = 'GET'; Target = '/api/orders/ORD-1001' }
                [pscustomobject]@{ Method = 'GET'; Target = '/api/customers?page=1&pageSize=20&query=' }
                [pscustomobject]@{ Method = 'GET'; Target = '/api/customers/CUS-001' })) {
            Wait-MockRequest $mockOut $request.Method $request.Target
            Write-Output "DEPLOYMENT_CAPABILITY_NETWORK=$($request.Method) $($request.Target)"
        }
        Stop-OwnedHost $secondHost
        if ($secondHost.ExitCode -ne 0) {
            throw "Updated deployed Host cleanup exited $($secondHost.ExitCode)."
        }
        Wait-Until {
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 -and
            @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $webEnginePath).Count -eq 0
        } 20000 'Deployed Worker/WebEngine processes remained after acceptance.'
        $clipboardQml = @'
import QtQuick
Rectangle {
    id: root
    width: 1100; height: 720
    color: "#f8fafc"
    property string noGestureId: ""
    property string noGestureStoreId: ""
    property string undeclaredId: ""
    property string undeclaredStoreId: ""
    property string firstId: ""
    property string replayId: ""
    property bool ready: false
    property bool firstOk: false
    Component.onCompleted:
        root.noGestureId = Runtime.invoke("clipboard", "read", {})
    Rectangle {
        anchors.centerIn: parent
        width: 240; height: 96; radius: 8
        color: root.ready ? "#2563eb" : "#64748b"
        MouseArea {
            anchors.fill: parent
            enabled: root.ready
            onClicked: root.firstId = Runtime.invoke("clipboard", "read", {})
        }
    }
    Connections {
        target: Runtime
        function onCapabilityFinished(id, response) {
            if (id === root.noGestureId) {
                root.noGestureStoreId = Runtime.invoke("storage", "set", {
                    key: "clipboard-no-gesture", value: response.error.code
                })
            } else if (id === root.noGestureStoreId) {
                root.undeclaredId = Runtime.invoke("clipboard", "write", {
                    text: "must-not-write"
                })
            } else if (id === root.undeclaredId) {
                root.undeclaredStoreId = Runtime.invoke("storage", "set", {
                    key: "clipboard-undeclared", value: response.error.code
                })
            } else if (id === root.undeclaredStoreId) {
                root.ready = response.ok === true
            } else if (id === root.firstId) {
                root.firstOk = response.ok === true
                    && response.result !== undefined
                    && response.result.text === "gesture-canary"
                root.replayId = Runtime.invoke("clipboard", "read", {})
            } else if (id === root.replayId) {
                Runtime.invoke("storage", "set", {
                    key: "clipboard-gesture",
                    value: (root.firstOk ? "first-ok:" : "first-failed:")
                        + response.error.code
                })
            }
        }
    }
}
'@
        $clipboardPackage = Join-Path $candidateRoot `
            'com.qbrowser.pilot-1.2.0-clipboard.qapkg'
        New-SignedUpdatePackage '1.2.0' $clipboardPackage `
            -MainQml $clipboardQml -ClipboardReadWithGesture
        Protect-Path $clipboardPackage
        $thirdHost = Start-DeployedHost $script:mockOrigin $store $sandbox `
            $telemetry $storage $clipboardPackage 1000
        [void]$ownedPids.Add($thirdHost.Id)
        $clipboardWorker = Wait-DeployedWorker @(
            $firstUpdateWorker.Identity, $restartedWorker.Identity,
            $recoveryWorker.Identity)
        [void]$ownedPids.Add([int]$clipboardWorker.ProcessId)
        Wait-Telemetry $telemetry `
            '"packageVersion":"1\.2\.0","phase":"health","code":"healthy"' 30000
        Invoke-DeployedClipboardAcceptance $thirdHost $clipboardWorker $telemetry $storage
        Stop-OwnedHost $thirdHost
        if ($thirdHost.ExitCode -ne 0) {
            throw "Clipboard deployed Host cleanup exited $($thirdHost.ExitCode)."
        }
        Wait-Until {
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0
        } 15000 'Clipboard deployed Worker remained after acceptance.'
        Assert-AclLeaseRestored $deployAclBefore $deployAclRoots 'Updated Host deployment'
        Assert-AclLeaseRestored $storeAclBefore @($store) 'Updated Host package store' `
            -AllowAdditional -IgnoreActivationLockLifecycle
        Write-Output 'DEPLOYMENT_E2E_OK routes=10 networkReads=4 business=login+order+customers+storage+file+clipboard webEngine=deployed update=1.1.0 rollback=1.0.0 paths=minimal+polluted'
    }
    catch {
        Write-Output "DEPLOYMENT_E2E_FAILURE=$($_.Exception.Message)"
        Write-Output "DEPLOYMENT_E2E_STACK=$($_.ScriptStackTrace)"
        if ((Get-Variable telemetry -ErrorAction SilentlyContinue) -and
            (Test-Path -LiteralPath (Join-Path $telemetry 'events.jsonl') -PathType Leaf)) {
            Write-Output 'DEPLOYMENT_E2E_TELEMETRY_TAIL_BEGIN'
            Get-Content -LiteralPath (Join-Path $telemetry 'events.jsonl') -Tail 40
            Write-Output 'DEPLOYMENT_E2E_TELEMETRY_TAIL_END'
        }
        throw
    }
    finally {
        $childDefinitions = @(
            [pscustomobject]@{ Name = 'qbrowser-worker.exe'; Path = Join-Path $staging 'runtime\qbrowser-worker.exe' }
            [pscustomobject]@{ Name = 'QtWebEngineProcess.exe'; Path = Join-Path $staging 'host\QtWebEngineProcess.exe' })
        foreach ($definition in $childDefinitions) {
            foreach ($owned in @(Get-DeployedProcesses $definition.Name $definition.Path)) {
                Stop-Process -Id ([int]$owned.ProcessId) -Force -ErrorAction SilentlyContinue
                Wait-Until { -not (Get-Process -Id ([int]$owned.ProcessId) `
                        -ErrorAction SilentlyContinue) } 10000 `
                    "Owned child process remained: $($owned.ProcessId)"
            }
        }
        foreach ($process in @($thirdHost, $secondHost, $initialHostProcess, $mock)) {
            if ($null -ne $process -and -not $process.HasExited -and
                $ownedPids.Contains($process.Id)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                [void]$process.WaitForExit(10000)
            }
        }
        $env:PATH = $previousPath
        foreach ($name in $loaderEnvironmentNames) {
            # The release operation cleared caller-controlled loader state
            # before entering this E2E. Keep it cleared for the production
            # acceptance suite; only the outermost finally restores caller
            # state after every release check is complete.
            Clear-ProcessEnvironmentValue $name
        }
    }
}

function Invoke-AdversarialDeploymentTests {
    $key = Join-Path $staging 'trust\dev-public.pem'
    $keyBackup = Join-Path $taskTemp 'public-key-backup.pem'
    Move-Item -LiteralPath $key -Destination $keyBackup
    [QBrowser.Task18.ReparseDirectory]::CreateFileSymbolicLink($key, $keyBackup)
    try {
        $result = Invoke-Captured $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'reparse point rejected') {
            throw 'Verifier did not reject an externally linked trust key.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Force }
        Move-Item -LiteralPath $keyBackup -Destination $key
        Protect-Path $key
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_REPARSE_REJECT=PASS'

    $transitiveDependency = Join-Path $staging 'host\Qt6Qml.dll'
    $transitiveBackup = Join-Path $taskTemp 'Qt6Qml.dll.backup'
    Move-Item -LiteralPath $transitiveDependency -Destination $transitiveBackup
    try {
        $result = Invoke-Captured $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch 'missing PE dependencies') {
            throw 'Verifier did not reject a missing transitive PE dependency.'
        }
    }
    finally {
        Move-Item -LiteralPath $transitiveBackup -Destination $transitiveDependency
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Write-Output 'ADVERSARIAL_PE_CLOSURE_REJECT=PASS dependency=Qt6Qml.dll'

    $bogusDirectory = Join-Path $staging 'host\bogus-loader-directory'
    New-Item -ItemType Directory -Path $bogusDirectory | Out-Null
    Move-Item -LiteralPath $transitiveDependency `
        -Destination (Join-Path $bogusDirectory 'Qt6Qml.dll')
    try {
        $result = Invoke-Captured $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch 'missing PE dependencies') {
            throw 'Verifier accepted a dependency moved outside loader search directories.'
        }
    }
    finally {
        Move-Item -LiteralPath (Join-Path $bogusDirectory 'Qt6Qml.dll') `
            -Destination $transitiveDependency
        Remove-Item -LiteralPath $bogusDirectory -Force
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Write-Output 'ADVERSARIAL_PE_MOVED_REJECT=PASS loaderSearch=importer+closureRoot'

    $privateProbe = Join-Path $staging 'docs\private-material-probe.txt'
    $privateLabels = @('RSA PRIVATE KEY','EC PRIVATE KEY','DSA PRIVATE KEY',
        'OPENSSH PRIVATE KEY','ENCRYPTED PRIVATE KEY','PRIVATE KEY')
    foreach ($privateLabel in $privateLabels) {
        $preamble = if ($privateLabel -eq 'RSA PRIVATE KEY') { 'x' * 9000 } else { '' }
        [IO.File]::WriteAllText($privateProbe,
            "$preamble`n-----BEGIN $privateLabel-----`n",
            [Text.UTF8Encoding]::new($false))
        Protect-Path $privateProbe
        $result = Invoke-Captured $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch 'forbidden PEM material') {
            throw "Verifier did not reject PEM form: $privateLabel"
        }
    }
    Remove-Item -LiteralPath $privateProbe -Force
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_PRIVATE_PEM_REJECT=PASS preamble=9000 forms=RSA,EC,DSA,OpenSSH,PKCS8-encrypted,PKCS8-unencrypted'

    $package = Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'
    $packageBackup = Join-Path $taskTemp 'pilot-backup.qapkg'
    $manifest = Join-Path $staging 'SHA-256SUMS'
    $manifestBackup = Join-Path $taskTemp 'manifest-backup.txt'
    Copy-Item -LiteralPath $package -Destination $packageBackup
    Copy-Item -LiteralPath $manifest -Destination $manifestBackup
    $bytes = [IO.File]::ReadAllBytes($package)
    $bytes[[Math]::Min(32, $bytes.Length - 1)] = $bytes[[Math]::Min(32, $bytes.Length - 1)] -bxor 1
    [IO.File]::WriteAllBytes($package, $bytes)
    $result = Invoke-Captured $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'signature/identity') {
        throw 'Regenerated hashes blessed a tampered signed package.'
    }
    Copy-Item -LiteralPath $packageBackup -Destination $package -Force
    Copy-Item -LiteralPath $manifestBackup -Destination $manifest -Force
    Protect-Path $package
    Protect-Path $manifest
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_TAMPERED_SIGNED_PACKAGE_REJECT=PASS regeneratedHashes=true'
}

function Test-CleanupReparseDefense {
    $external = Join-Path $taskTemp 'external-sentinel'
    $junction = Join-Path $taskTemp 'owned-junction-attack'
    New-Item -ItemType Directory -Path $external | Out-Null
    [IO.File]::WriteAllText((Join-Path $external 'sentinel.txt'), 'must-survive',
        [Text.UTF8Encoding]::new($false))
    New-Item -ItemType Junction -Path $junction -Target $external | Out-Null
    $rejected = $false
    try { Remove-OwnedTree $junction $junction } catch { $rejected = $true }
    if (-not $rejected -or
        -not (Test-Path -LiteralPath (Join-Path $external 'sentinel.txt')) -or
        -not (Test-Path -LiteralPath $junction)) {
        throw 'Cleanup reparse defense mutated or accepted the external sentinel link.'
    }
    Remove-VerifiedJunctionEntry $junction $taskTemp $external `
        (Join-Path $external 'sentinel.txt')
    Write-Output 'CLEANUP_REPARSE_DEFENSE=PASS externalSentinel=unchanged'

    $longRoot = Join-Path $taskTemp 'owned-long-path-cleanup'
    New-OwnedDirectory $longRoot
    Protect-Path $longRoot -Container
    $deep = $longRoot
    while ($deep.Length -lt 280) {
        $deep = Join-Path $deep ('segment-' + ('x' * 20))
        [void][IO.Directory]::CreateDirectory('\\?\' + $deep)
    }
    [IO.File]::WriteAllText('\\?\' + (Join-Path $deep 'sentinel.txt'),
        'owned', [Text.UTF8Encoding]::new($false))
    Remove-OwnedTree $longRoot $longRoot
    if (Test-Path -LiteralPath $longRoot) {
        throw 'Long-path owned cleanup left residue.'
    }
    Write-Output "CLEANUP_LONG_PATH=PASS length=$($deep.Length) residue=0"
}

function Test-TrustedParentReplacementDefense {
    $probe = Join-Path $taskTemp 'parent-replacement-probe'
    New-OwnedDirectory $probe
    Protect-Path $probe -Container
    $identity = Get-PathIdentity $probe
    $lease = [QBrowser.Task18.DirectoryLease]::new($probe)
    $renamed = Join-Path $taskTemp 'parent-replacement-probe-renamed'
    $renameRejected = $false
    try {
        try { Move-Item -LiteralPath $probe -Destination $renamed -ErrorAction Stop }
        catch { $renameRejected = $true }
        if (-not $renameRejected -or -not (Test-Path -LiteralPath $probe) -or
            (Get-PathIdentity $probe) -ne $identity) {
            throw 'A leased trusted directory accepted rename/replacement.'
        }
    }
    finally { $lease.Dispose() }
    Remove-OwnedTree $probe $probe

    $unsafe = Join-Path $taskTemp 'unsafe-parent-probe'
    New-OwnedDirectory $unsafe
    Protect-Path $unsafe -Container
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $unsafe, '/grant', '*S-1-5-11:(OI)(CI)M')
    $unsafeRejected = $false
    try { Assert-TrustedAncestorChain $unsafe $trusted }
    catch { $unsafeRejected = $true }
    if (-not $unsafeRejected) { throw 'Untrusted parent replacement ACL was accepted.' }
    Protect-Path $unsafe -Container
    Remove-OwnedTree $unsafe $unsafe
    Write-Output 'TRUSTED_PARENT_REPLACEMENT_DEFENSE=PASS lease=rename-rejected acl=fail-closed'
}

$buildParentIdentity = Get-PathIdentity $trustedBuildRoot
    # Protection helpers intentionally invoke native ACL tools whose output is useful
    # interactively. Select only the explicit final snapshot record so native output
    # cannot change the type of this security-sensitive result.
    $sourceSnapshot = @(New-TrustedSourceSnapshot $sourceStage) |
        Select-Object -Last 1
    Assert-StableTrustedPath $sourceStage $sourceSnapshot.Identity
    $controlCandidate = Join-Path $trustedControlRoot 'Deploy.cmake.candidate'
    Copy-Item -LiteralPath (Join-Path $sourceStage 'cmake\Deploy.cmake') `
        -Destination $controlCandidate
    Protect-Path $controlCandidate
    if (Test-Path -LiteralPath $deployScript) {
        # This .NET runtime rejects a null backup path. Keep the rollback copy
        # inside the already owned task directory and remove it only after the
        # atomic replacement succeeds.
        $controlBackup = Join-Path $taskTemp 'Deploy.cmake.previous'
        [IO.File]::Replace($controlCandidate, $deployScript, $controlBackup, $true)
        Remove-Item -LiteralPath $controlBackup -Force
    } else {
        Move-Item -LiteralPath $controlCandidate -Destination $deployScript
    }
    Protect-Path $deployScript
    Write-Output "TRUSTED_SOURCE_SNAPSHOT=PASS files=$($sourceSnapshot.Count)"
    Test-CleanupReparseDefense
    Test-TrustedParentReplacementDefense
    New-OwnedDirectory $build
    Protect-Path $build -Container
    # CMake must enter this leaf, so its protected/deny-delete-leased parent is
    # the operation barrier; the leaf identity and tree are checked around every
    # build use. A DELETE lease on the leaf would block CMake's own directory open.
    $buildIdentity = Get-PathIdentity $build
    Assert-StableTrustedPath $build $buildIdentity
    $aclProbe = Join-Path $build 'acl-tamper-probe.txt'
    [IO.File]::WriteAllText($aclProbe, 'probe', [Text.UTF8Encoding]::new($false))
    Protect-Path $aclProbe
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $aclProbe, '/grant', '*S-1-5-11:R')
    $aclRejected = $false
    try { Assert-ProtectedPath $aclProbe } catch { $aclRejected = $true }
    if (-not $aclRejected) { throw 'Build input ACL tamper was accepted.' }
    Protect-Path $aclProbe
    Remove-Item -LiteralPath $aclProbe -Force
    $reparseProbe = Join-Path $build 'reparse-tamper-probe'
    New-Item -ItemType Junction -Path $reparseProbe -Target $taskTemp | Out-Null
    $reparseRejected = $false
    try { Assert-PlainTree $build } catch { $reparseRejected = $true }
    if (-not $reparseRejected) { throw 'Build input reparse tamper was accepted.' }
    Remove-VerifiedJunctionEntry $reparseProbe $build $taskTemp `
        (Join-Path $taskTemp $ownedMarkerName)
    Assert-ProtectedPath $build
    Assert-PlainTree $build
    Assert-StableTrustedPath $build $buildIdentity
    Write-Output 'BUILD_INPUT_TAMPER_DEFENSE=PASS acl=rejected reparse=rejected'
    Invoke-Checked $CMake @('-S', $sourceStage, '-B', $build,
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_PREFIX_PATH=$QtRoot", "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        '-DBUILD_TESTING=OFF', '-DQ_BROWSER_BUILD_WEBENGINE=ON')
    Invoke-TrackedBuild @('--build', $build, '--config', 'Release', '--parallel', '2',
        '--', '/nr:false')
    Assert-StableTrustedPath $build $buildIdentity
    Assert-ProtectedPath $build
    Assert-PlainTree $build
    $releaseHost = Join-Path $build 'apps\host\Release\qbrowser-host.exe'
    $hostAcl = Get-Acl -LiteralPath $releaseHost
    if (@($hostAcl.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
            $_.AccessControlType -eq 'Allow' -and
            $_.IdentityReference.Value -notin @(
                [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
                'S-1-5-18') }).Count -ne 0) {
        throw 'Generated Release Host is writable/readable by an untrusted principal.'
    }
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
    if ($cache -notmatch '(?m)^BUILD_TESTING:BOOL=OFF\r?$') {
        throw 'Release build unexpectedly enabled test code.'
    }
    $ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
    $testInventory = & $ctest --test-dir $build -C Release -N
    if ($LASTEXITCODE -ne 0 -or ($testInventory -join "`n") -notmatch 'Total Tests: 0') {
        throw 'Production Release build contains tests or CTest inventory failed.'
    }

    & (Join-Path $sourceStage 'scripts\create-dev-package.ps1') -Configuration Release `
        -BuildDirectory $build -OutputDirectory $packageOutput `
        -KeyDirectory (Join-Path $trusted 'signing') -TrustedRoot $trusted `
        -SourceDirectory $sourceStage -ParentTrustedRootLeaseHeld -Clean:$Clean
    if ($LASTEXITCODE -ne 0) { throw 'Development Pilot package creation failed.' }
    Assert-TrustedAncestorChain $packageOutput $trusted
    $packageOutputIdentity = Get-PathIdentity $packageOutput
    [void](New-DirectoryLease $packageOutput)
    Assert-StableTrustedPath $packageOutput $packageOutputIdentity

    $stagingHost = Join-Path $staging 'host'
    $stagingRuntime = Join-Path $staging 'runtime'
    New-Item -ItemType Directory -Path $stagingHost, $stagingRuntime | Out-Null
    Invoke-Checked $CMake @('--install', $build, '--config', 'Release',
        '--prefix', $stagingHost, '--component', 'Runtime')
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=ASSEMBLE',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", "-DQ_BROWSER_REPO_ROOT=$sourceStage",
        "-DQ_BROWSER_QT_ROOT=$QtRoot", "-DQ_BROWSER_OPENSSL_ROOT=$OpenSslRoot",
        "-DQ_BROWSER_PACKAGE_FILE=$(Join-Path $packageOutput 'com.qbrowser.pilot-1.0.0.qapkg')",
        "-DQ_BROWSER_PUBLIC_KEY=$(Join-Path $packageOutput 'dev-public.pem')",
        '-P', $deployScript)
    foreach ($container in @($staging, $stagingHost, $stagingRuntime,
            (Join-Path $staging 'packages'), (Join-Path $staging 'trust'))) {
        Protect-Path $container -Container
    }
    foreach ($file in @((Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'),
            (Join-Path $staging 'trust\dev-public.pem'),
            (Join-Path $staging 'SHA-256SUMS'))) {
        Protect-Path $file
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=PREVERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Assert-StableTrustedPath $staging $stagingIdentity

    $forbiddenSymbols = @('qbrowser_host_testing', 'qbrowser_archive_testing',
        'forceLifecycleQueueFullForTesting', 'retryWorkerCleanupForTesting')
    foreach ($binary in Get-ChildItem -LiteralPath $staging -Recurse -File -Filter '*.exe') {
        foreach ($symbol in $forbiddenSymbols) {
            & "$env:SystemRoot\System32\findstr.exe" /P /M /C:$symbol $binary.FullName | Out-Null
            if ($LASTEXITCODE -eq 0) {
                throw "Production binary contains test-hook surface '$symbol': $($binary.Name)"
            }
        }
    }

    Invoke-DeploymentOnlyE2E
    [IO.File]::WriteAllText((Join-Path $staging 'release-attestation.json'),
        "{`"schema`":1,`"deploymentOnlyE2E`":true,`"routeCount`":10," +
        "`"webEngine`":`"deployed`",`"signedUpdate`":`"1.1.0`"," +
        "`"rollback`":`"1.0.0`"}`n", [Text.UTF8Encoding]::new($false))
    Protect-Path (Join-Path $staging 'release-attestation.json')
    Remove-Item -LiteralPath (Join-Path $staging $ownedMarkerName) -Force
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Invoke-AdversarialDeploymentTests

    $acceptanceBuild = $defaultAcceptanceBuild
    if ($RunAcceptance) {
        New-OwnedDirectory $acceptanceBuild
        Protect-Path $acceptanceBuild -Container
        $env:PATH = $previousPath
        & (Join-Path $sourceStage 'scripts\run-acceptance.ps1') -Configuration Release `
            -BuildDirectory $acceptanceBuild `
            -QtRoot $QtRoot -OpenSslRoot $OpenSslRoot
        if ($LASTEXITCODE -ne 0) { throw 'Release acceptance failed.' }
    }

    $privateScanRoots = @($build, $packageOutput, $staging)
    if ($RunAcceptance) { $privateScanRoots += $acceptanceBuild }
    foreach ($file in $privateScanRoots | ForEach-Object {
            Get-ChildItem -LiteralPath $_ -File -Recurse -Force }) {
        if ($file.Length -gt 0 -and (Test-PrivatePem $file.FullName)) {
            throw "Build/acceptance output contains PEM private key material: $($file.FullName)"
        }
    }

    $minimalPath = "$(Join-Path $staging 'host');$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($verificationPath in @($minimalPath, "C:\polluted-does-not-exist;$minimalPath")) {
        $env:PATH = $verificationPath
        Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    }
    $env:PATH = $previousPath
    foreach ($name in $loaderEnvironmentNames) {
        Restore-ProcessEnvironmentState $name $previousLoaderEnvironment[$name]
    }
    if ($FailureInjection -eq 'BeforePublish') {
        throw 'Injected Task18 failure before atomic publication.'
    }
    if ((Get-PathIdentity $trustedBuildRoot) -ne $buildParentIdentity) {
        throw 'Trusted build root identity changed before publication.'
    }
    Assert-StableTrustedPath $staging $stagingIdentity
    Assert-StableTrustedPath $packageOutput $packageOutputIdentity
    Assert-NoReparseAncestor $deployment
    if (Test-Path -LiteralPath $deployment) {
        throw "Deployment appeared during staging; refusing to overwrite: $deployment"
    }
    $stagingLease.Dispose()
    Move-Item -LiteralPath $staging -Destination $deployment
    $deploymentIdentity = Get-PathIdentity $deployment
    $deploymentLease = New-DirectoryLease $deployment
    Assert-StableTrustedPath $deployment $deploymentIdentity
    $replacement = Join-Path $trusted 'release-deploy-substitute-probe'
    $publishedRenameRejected = $false
    try { Move-Item -LiteralPath $deployment -Destination $replacement -ErrorAction Stop }
    catch { $publishedRenameRejected = $true }
    if (-not $publishedRenameRejected -or -not (Test-Path -LiteralPath $deployment) -or
        (Get-PathIdentity $deployment) -ne $deploymentIdentity) {
        throw 'Published deployment accepted rename/replacement substitution.'
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment", '-P', $deployScript)
    Write-Output 'PUBLISHED_PARENT_REPLACEMENT_DEFENSE=PASS substitute=rejected'
    Write-Output "Q-Browser Release deployment created and accepted: $deployment"
}
catch {
    $primaryFailure = $_
    throw
}
finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:PATH = $previousPath
    Restore-ProcessEnvironmentState 'SOURCE_DATE_EPOCH' $previousSourceDateEpoch
    Restore-ProcessEnvironmentState 'MSBUILDDISABLENODEREUSE' $previousMsBuildNodeReuse
    foreach ($name in $loaderEnvironmentNames) {
        Restore-ProcessEnvironmentState $name $previousLoaderEnvironment[$name]
    }
    $cleanupFailures = @()
    try {
        foreach ($lease in $script:workerLeases) { $lease.Dispose() }
        $script:workerLeases.Clear()
        Dispose-OperationLeases
    }
    catch { $cleanupFailures += "lease cleanup: $($_.Exception.Message)" }
    try {
        if (Test-Path -LiteralPath $staging) {
            if (Test-Path -LiteralPath (Join-Path $staging '.qbrowser-release-root')) {
                Remove-OwnedTree $staging $staging '.qbrowser-release-root' $releaseMarkerText
            }
            else { Remove-OwnedTree $staging $staging }
        }
    }
    catch { $cleanupFailures += "staging cleanup: $($_.Exception.Message)" }
    try {
        if (Test-Path -LiteralPath $sourceStage) {
            Remove-OwnedTree $sourceStage $sourceStage
        }
    }
    catch { $cleanupFailures += "source snapshot cleanup: $($_.Exception.Message)" }
    try {
        if (Test-Path -LiteralPath $taskTemp) {
            Remove-OwnedTree $taskTemp $taskTemp
        }
    }
    catch { $cleanupFailures += "temporary cleanup: $($_.Exception.Message)" }
    Start-Sleep -Milliseconds 1500
    if (Test-Path -LiteralPath $taskTemp) {
        $cleanupFailures += "delayed temporary residue: $taskTemp"
    }
    foreach ($name in $loaderEnvironmentNames) {
        if (-not (Test-ProcessEnvironmentState $name $previousLoaderEnvironment[$name])) {
            $cleanupFailures += "loader environment restoration: $name"
        }
    }
    if (-not (Test-ProcessEnvironmentState 'MSBUILDDISABLENODEREUSE' `
            $previousMsBuildNodeReuse)) {
        $cleanupFailures += 'MSBuild node-reuse environment restoration'
    }
    if ($cleanupFailures.Count -ne 0) {
        $cleanupMessage = $cleanupFailures -join '; '
        if ($null -ne $primaryFailure) {
            Write-Error "Secondary cleanup failure after primary '$($primaryFailure.Exception.Message)': $cleanupMessage" -ErrorAction Continue
        }
        else { throw "Release cleanup failed: $cleanupMessage" }
    }
}
