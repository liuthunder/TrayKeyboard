# TrayKeyBoard
TrayKeyBoard，系统图标键盘，是一个windows效率程序。当他启动时，会显示几个图标，点击图标，相当于按下键盘上的按键。
- 回车键
- 退格键
- 删除键

## 技术栈
- C++，vs2022，cmake，cmake第三方库方案，git

## 当前实现
- Win32 隐藏窗口 + 系统托盘图标
- 三个托盘图标分别映射回车、退格、删除
- 左键点击图标发送对应按键
- 右键任意图标弹出菜单，可重复发送按键或退出程序
- 托盘图标改为本地绘制的符号图标：⏎、⌫、⌦
- 通过系统前台/焦点跟踪记住最近真实输入目标，避免托盘点击抢焦点后按键失效

## 回归验证
```powershell
powershell -ExecutionPolicy Bypass -File .\test\automation_focus_regression.ps1
```

脚本会自动创建一个文本框窗口，模拟任务栏抢走焦点，再触发托盘 Backspace 动作，期望输出 PASS:ab。

## 构建与运行
```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
.\build\Release\TrayKeyboard.exe
```


