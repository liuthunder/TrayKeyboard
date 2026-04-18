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

## 构建与运行
```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
.\build\Release\TrayKeyboard.exe
```


