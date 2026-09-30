# 最小开发环境验证

日期：2026-09-30

安装：独立 Build Tools 2022，版本 17.14.41。安装器来自微软官方下载入口，Authenticode 签名 Valid，签名方 Microsoft Corporation。

显式选择的组件：

- Microsoft.VisualStudio.Component.VC.Tools.x86.x64
- Microsoft.VisualStudio.Component.Windows11SDK.26100

由微软安装器自动补齐必要依赖；未选择完整工作负载、Visual Studio IDE 或 CMake。使用 --nocache 避免保留已安装组件的大型下载缓存；安装器未自动重启，退出码 0。

安装位置：C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools

SDK 目录版本：10.0.26100.0

验证结果：

1. scripts/check-environment.ps1：Ready=True。
2. scripts/verify-toolchain.cmd：使用 x64 工具链编译 C++20 检查程序，链接 windowsapp、ole32、d3d11、dcomp 成功。
3. 检查程序创建 InkDrawingAttributes 并启用压感，激活 InkDesktopHost 成功，返回码 0。

尚未验证：透明覆盖层的输入路由、DirectComposition 画布呈现、实际触控笔压感与延迟。以上需要下一阶段原型及真机验收。
