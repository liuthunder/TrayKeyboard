Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class NativeMethods {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
}
'@

$resultPath = Join-Path $PSScriptRoot 'automation-dedup-result.txt'
Remove-Item $resultPath -ErrorAction SilentlyContinue

$form = New-Object System.Windows.Forms.Form
$form.Text = 'TrayKeyboard Dedup Test'
$form.Width = 300
$form.Height = 200

$textBox = New-Object System.Windows.Forms.TextBox
$textBox.Multiline = $true
$textBox.Dock = 'Fill'
$textBox.Text = 'abc'
$form.Controls.Add($textBox)

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 400

$assertTimer = New-Object System.Windows.Forms.Timer
$assertTimer.Interval = 1200

$timer.Add_Tick({
    $timer.Stop()
    [NativeMethods]::SetForegroundWindow($form.Handle) | Out-Null
    $textBox.Focus() | Out-Null
    $textBox.SelectionStart = $textBox.TextLength
    $textBox.SelectionLength = 0

    $trayWindow = [NativeMethods]::FindWindow('TrayKeyboardWindowClass', 'TrayKeyboard')
    if ($trayWindow -eq [IntPtr]::Zero) {
        Set-Content -Path $resultPath -Value 'FAIL:tray-window-not-found' -Encoding ASCII
        $form.Close()
        return
    }

    [NativeMethods]::SendMessage($trayWindow, 0x8001, [IntPtr]1002, [IntPtr]0x0201) | Out-Null
    [NativeMethods]::SendMessage($trayWindow, 0x8001, [IntPtr]1002, [IntPtr]0x0202) | Out-Null
    [NativeMethods]::SendMessage($trayWindow, 0x8001, [IntPtr]1002, [IntPtr]0x0400) | Out-Null
})

$assertTimer.Add_Tick({
    $assertTimer.Stop()
    $actual = $textBox.Text.Replace("`r", '\\r').Replace("`n", '\\n')
    if ($actual -eq 'ab') {
        Set-Content -Path $resultPath -Value ('PASS:' + $actual) -Encoding ASCII
    } else {
        Set-Content -Path $resultPath -Value ('FAIL:' + $actual) -Encoding ASCII
    }
    $form.Close()
})

$form.Add_Shown({
    $timer.Start()
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