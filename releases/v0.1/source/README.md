# MyZoomIt

轻量、快捷键驱动的 Windows Ink 屏幕标注工具。

当前阶段：首个里程碑原型已实现，Release 构建与桌面自动验收通过；触控笔手感和多屏 DPI 等待真机验收。

## 运行

双击 `build\MyZoomIt.exe`，程序在系统托盘常驻（图标可能位于隐藏图标区）。

- **Ctrl+2**：在鼠标所在屏幕开始 / 结束标注。
- **Esc**：结束标注，恢复之前的应用焦点。
- **C**：清空笔迹。
- **Ctrl+Z**：撤销上一笔，可连续撤销；空画布时不做操作。
- **Ctrl+鼠标滚轮**：调节笔宽，范围 1–24，每格调整 1；默认 12，提示条显示当前宽度。只影响后续笔画。
- 鼠标左键、触控笔或手指：红色笔书写；触控笔保留压感。
- 当前暂时关闭速度压感与鼠标起笔修正，鼠标使用固定线宽。Ctrl+滚轮控制笔宽，触控笔仍使用真实压感。
- **R 红色、G 绿色、O 橙色、P 粉紫色、B 蓝色、W 白色、Y 黄色**：标注时按对应字母切换颜色，只影响后续笔画；提示条显示当前颜色。
- 结束标注后再次进入，保留颜色和笔宽；记忆限本次程序运行。
- 托盘右键：开始 / 结束、清空、设置、退出。设置中可修改全局快捷键、恢复默认 Ctrl+2，并查看操作说明。快捷键保存至 `%LOCALAPPDATA%\MyZoomIt\settings.ini`，重启后继续有效；冲突时保留旧快捷键。

标注期间覆盖层接收输入，不能点击底层应用；按 Esc 后恢复操作。每次结束标注（Esc、Ctrl+2、失焦或退出程序）都清空全部墨迹，重新呼出从空画布开始。没有缩放、笔迹保存或重做功能。

## 构建与验证

运行 `scripts\build.cmd` 生成 Release 程序和 `build\build.log`。没有额外包下载。

运行 `build\MyZoomIt.exe --self-test` 执行真实桌面输入测试，需先退出已有实例，并从项目根目录运行。测试约 14 秒，会短暂显示蓝色测试窗口、移动鼠标、模拟快捷键/滚轮并注入触摸输入；测试期间请勿同时操作键盘或鼠标。结果写入 `build\self-test.log`，测试区域截图写入 `build\self-test.bmp`，程序随后自动退出。返回码 0 为通过。

[验收结果与已知限制](docs/milestone-1-verification.md)

- [里程碑设计与验收清单](docs/milestone-1.md)
- 环境检查：在项目目录运行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check-environment.ps1`。此参数只作用于本次检查进程，不修改系统执行策略。

最小开发环境只选择微软 C++ Build Tools 的 MSVC x64/x86 编译工具和 Windows 11 SDK，让安装器补齐必要依赖；无需 Visual Studio IDE、CMake 或额外工作负载。运行原型使用 Windows 11 x64 自带 XAML/Ink 系统组件。

实际编译与 Windows Ink 宿主检查：运行 `scripts\verify-toolchain.cmd`。




