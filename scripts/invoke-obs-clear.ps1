param([Parameter(Mandatory)][int]$ObsProcessId)
$ErrorActionPreference = 'Stop'
$process = Get-Process -Id $ObsProcessId
if ($process.ProcessName -ne 'obs64') { throw 'Target is not OBS' }
$hostRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $process.Path))
if (!(Test-Path -LiteralPath (Join-Path $hostRoot '.owcs-compat-test'))) {
  throw 'Only invoke the clear button in a marked disposable OBS test host'
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ObsTestWindows {
  delegate bool Callback(IntPtr window, IntPtr argument);
  [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr argument);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
  public static IntPtr[] ForProcess(int processId) {
    var handles = new List<IntPtr>();
    EnumWindows((window, _) => {
      uint pid; GetWindowThreadProcessId(window, out pid);
      if (pid == processId) handles.Add(window);
      return true;
    }, IntPtr.Zero);
    return handles.ToArray();
  }
}
'@
$buttonsOnly = [System.Windows.Automation.PropertyCondition]::new(
  [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
for ($attempt = 0; $attempt -lt 2; $attempt++) {
  foreach ($handle in [ObsTestWindows]::ForProcess($ObsProcessId)) {
    $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
    if (!$window) { continue }
    # Keep the disposable dock and its menu offscreen, without activation.
    if ($window.Current.Name -like 'OWCS *' -or $window.Current.Name -like 'OBS *') {
      [void][ObsTestWindows]::SetWindowPos($handle, [IntPtr]::Zero, -32000, -32000, 1000, 800, 0x14)
    }
    $buttons = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonsOnly)
    foreach ($button in $buttons) {
      if ($button.Current.AutomationId -notmatch '(^|\.)owcsClearData$') { continue }
      $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
      Write-Output 'CLEAR_INVOKED'
      exit 0
    }
  }
  if ($attempt -ne 0) { break }
  # A newly registered dock starts hidden. Enable it through the real OBS menu
  # so Qt exposes its accessibility tree (native ShowWindow alone cannot).
  foreach ($handle in [ObsTestWindows]::ForProcess($ObsProcessId)) {
    $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
    if (!$window -or $window.Current.Name -notlike 'OBS *') { continue }
    $items = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($item in $items) {
      if ($item.Current.ControlType -ne [System.Windows.Automation.ControlType]::MenuItem -or
          ($item.Current.Name -notlike '*(D)' -and $item.Current.Name -ne 'Docks')) { continue }
      $item.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
      $children = $item.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
      foreach ($child in $children) {
        if ($child.Current.Name -like 'OWCS *') {
          $child.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        }
      }
    }
  }
}
throw 'Clear data button was not found in the isolated OBS instance'
