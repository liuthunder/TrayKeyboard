# TrayKeyboard

TrayKeyboard 是一个 Windows 托盘按键工具。程序启动后会在系统托盘里显示三个图标，点击图标即可向当前输入目标发送按键。

支持的按键：
- 回车
- 退格
- 删除

## 适用场景

- 触屏设备上补充常用编辑按键
- 鼠标操作为主时快速输入 Enter、Backspace、Delete
- 需要常驻后台的轻量辅助工具

## 使用方法

1. 启动 TrayKeyboard。
2. 程序会最小化到系统托盘，不显示主窗口。
3. 左键点击托盘图标即可发送对应按键。
4. 右键点击任意托盘图标可以打开菜单，执行重复发送、切换“开机自启动”或退出程序。

## 运行方式

如果你下载的是发布版：

1. 下载发布页中的 TrayKeyboard-win64.zip。
2. 解压后运行 TrayKeyboard.exe。

如果你需要自行编译：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
.\build\Release\TrayKeyboard.exe
```

## 使用说明

- TrayKeyboard 会尽量把按键发回最近的真实输入目标。
- 托盘图标分别代表 Enter、Backspace、Delete。
- 退出程序请使用托盘右键菜单中的退出项。

## 开发与设计文档

技术实现、设计取舍、回归验证和替代方案已经移到 [docs/设计文档.md](docs/设计文档.md)。


