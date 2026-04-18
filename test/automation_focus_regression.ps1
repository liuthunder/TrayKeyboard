Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class NativeMethods {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@

$resultPath = Join-Path $PSScriptRoot 'automation-result.txt'
Remove-Item $resultPath -ErrorAction SilentlyContinue

$form = New-Object System.Windows.Forms.Form
$form.Text = 'TrayKeyboard Automation Test'
$form.Width = 400
$form.Height = 240
$form.StartPosition = 'CenterScreen'

$textBox = New-Object System.Windows.Forms.TextBox
$textBox.Multiline = $true
$textBox.Dock = 'Fill'
$textBox.Text = 'abc'
$form.Controls.Add($textBox)

$focusTimer = New-Object System.Windows.Forms.Timer
$focusTimer.Interval = 300

$invokeTimer = New-Object System.Windows.Forms.Timer
$invokeTimer.Interval = 700

$assertTimer = New-Object System.Windows.Forms.Timer
$assertTimer.Interval = 1200

$focusTimer.Add_Tick({
    $focusTimer.Stop()
    $form.Activate() | Out-Null
    [NativeMethods]::SetForegroundWindow($form.Handle) | Out-Null
    $textBox.Focus() | Out-Null
    $textBox.SelectionStart = $textBox.TextLength
    $textBox.SelectionLength = 0
})

$invokeTimer.Add_Tick({
    $invokeTimer.Stop()

    $shellWindow = [NativeMethods]::FindWindow('Shell_TrayWnd', $null)
    if ($shellWindow -ne [IntPtr]::Zero) {
        [NativeMethods]::SetForegroundWindow($shellWindow) | Out-Null
    }

    $trayWindow = [NativeMethods]::FindWindow('TrayKeyboardWindowClass', 'TrayKeyboard')
    if ($trayWindow -eq [IntPtr]::Zero) {
        Set-Content -Path $resultPath -Value 'tray-window-not-found' -Encoding ASCII
        $form.Close()
        return
    }

    [NativeMethods]::SendMessage($trayWindow, 0x8001, [IntPtr]1002, [IntPtr]0x0201) | Out-Null
    [NativeMethods]::SendMessage($trayWindow, 0x8001, [IntPtr]1002, [IntPtr]0x0202) | Out-Null
})

$assertTimer.Add_Tick({
    $assertTimer.Stop()
    $actual = $textBox.Text.Replace("`r", '\\r').Replace("`n", '\\n')
    $focusState = $textBox.Focused
    if ($actual -eq 'ab' -and $focusState) {
        Set-Content -Path $resultPath -Value ('PASS:' + $actual + ':focus=' + $focusState) -Encoding ASCII
    } else {
        Set-Content -Path $resultPath -Value ('FAIL:' + $actual + ':focus=' + $focusState) -Encoding ASCII
    }
    $form.Close()
})

$form.Add_Shown({
    $focusTimer.Start()
    $invokeTimer.Start()
    $assertTimer.Start()
})

[System.Windows.Forms.Application]::Run($form)

if (Test-Path $resultPath) {
    $result = Get-Content $resultPath -Raw
    Write-Output $result.Trim()
    if ($result.StartsWith('PASS:')) {
        exit 0
    }
}

exit 1