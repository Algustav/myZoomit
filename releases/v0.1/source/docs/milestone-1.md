# MyZoomIt 首个里程碑设计

状态：原型已实现，Release 构建与桌面输入、透明呈现测试通过；真机手写及多屏兼容性待验证。见 milestone-1-verification.md。

## 目标与边界

在 Windows 11 x64 上，用全局快捷键进入当前显示器的透明标注层，用 Windows Ink 顺滑书写，按 Esc 隐藏标注层并返回原应用。窗口背景保持实时，不冻结屏幕。

本阶段包含单实例、托盘入口、快捷键、原生压感笔、鼠标与手指绘制、撤销上一笔、清空、可靠退出。暂不包含缩放、形状、荧光笔、重做、截图、设置页和自动启动。多屏只在呼出时选择鼠标所在屏幕，不支持跨屏连续书写。

## 使用流程

1. 启动后只显示托盘图标；菜单提供“开始标注”“清空笔迹”“退出”。
2. 按 Ctrl+2，记录前台窗口，选择鼠标所在显示器，显示并激活覆盖层。再次按同一快捷键等同退出标注。快捷键可配置留到后续设置页。
3. 使用红色圆头笔书写，默认宽度 12 个逻辑像素，启用压感和系统曲线拟合。Ctrl+滚轮每格调节 1，范围 1–24，只影响新笔画。鼠标绘制固定线宽；按用户试用反馈开启手指触摸绘制。
4. 按 C 清空当前会话笔迹，按 Esc 隐藏覆盖层，关闭其输入接收，并尝试恢复之前的前台窗口。
5. Ctrl+Z 撤销上一笔，可连续撤销。每次结束标注都清空全部墨迹，重新呼出从空画布开始。退出程序不保存笔迹。

标注状态由小型“标注中 · Esc 返回 · C 清空”提示表示，避开主要书写区域。提示不抢输入。没有主窗口、登录或网络请求。

## 状态与异常

状态为待机、进入中、标注、退出中。进入/退出过程禁止重入；快捷键注册使用 MOD_NOREPEAT。

- 快捷键被占用：明确提示冲突，托盘入口仍可使用；不静默更换快捷键。
- Ink 初始化失败：保持可退出，显示错误阶段与 HRESULT；不将鼠标模拟笔迹冒充 Windows Ink。
- 失去前台焦点：隐藏覆盖层，避免留在其他应用上截获输入。
- 显示器断开、分辨率或 DPI 变化、会话锁定：结束标注，重建资源后再允许呼出。
- 原前台窗口已关闭或系统拒绝焦点恢复：仍隐藏覆盖层，不反复强制抢焦点。
- 第二实例启动：通知已有实例呼出，随后退出。
- 托盘随 Explorer 重启丢失：处理 TaskbarCreated，重建图标。

## 技术方案

采用 C++20、Win32、C++/WinRT 和 Windows 自带 Windows.UI.Input.Inking，不依赖实验性 WinUI Ink 控件。首版通过系统 XAML Islands 托管 InkCanvas，由系统负责输入路由与图形合成；暂不手工管理 DirectComposition 墨迹视觉树。

模块：AppHost（消息循环、单实例、托盘）；HotkeyController（注册与冲突）；OverlayWindow（窗口与焦点）；InkEngine（InkDesktopHost、InkPresenter、笔迹）；DisplayContext（显示器与坐标）。

覆盖层为无边框置顶工具窗口，不显示任务栏按钮，使用合成透明背景。透明显示与输入命中分开设计：标注时整个客户区必须接收笔输入，包括完全透明的空白处；待机时直接隐藏窗口。不能仅用 WS_EX_TRANSPARENT 切换来假定鼠标和触控笔穿透行为相同。

实现调整：使用 WindowsXamlManager 和 DesktopWindowXamlSource 托管 InkCanvas。通过微软公开文档中的 IXamlSourceTransparency 将宿主背景设为透明，原生无边框窗口使用 WS_EX_NOREDIRECTIONBITMAP。调整减少手工输入路由风险，但增加系统 XAML 常驻资源开销；具体待机 CPU、内存和呼出延迟仍待测量。

保留系统低延迟即时笔迹路径，完成笔画后由系统转为持久笔迹。第一阶段不自定义 drying、不额外叠加平滑算法，降低抬笔跳变和双重渲染风险。输入启用 Pen、Mouse 与 Touch；压感启用，笔尾擦除作为硬件能力验证项，侧键不作为本阶段通过条件。

启用 Per-Monitor DPI Awareness V2。窗口边界取显示器完整矩形（包含任务栏），统一屏幕物理像素到窗口局部及 Ink 坐标的转换；移动显示器后不复用旧坐标。没有绘制时不使用持续刷新定时器，不截图桌面。

## 最先验证的技术关口

先做单显示器原型：透明空白区域能接收触控笔，背景实时可见，原生 Ink 快速书写无明显跳变，Esc 能释放输入。若此组合不能稳定成立，先解决窗口宿主与输入路由，暂停附加功能，不换成截图背景来宣称同等体验。

## 验收步骤

| 操作 | 预期 |
|---|---|
| 启动程序，在记事本继续输入 | 无主窗口，待机不截获输入 |
| Ctrl+2 呼出 | 鼠标所在屏幕出现提示，桌面背景实时可见 |
| 在透明空白处画圈、斜线、连续中文 | 无漏笔，笔迹落点正确，抬笔无闪烁/移位 |
| 用轻重笔触连续画线 | 支持压感的设备呈现连续粗细变化 |
| 用鼠标、触控笔和手指绘制 | 三类输入均能书写；掌触行为待真机确认 |
| Ctrl+滚轮调节宽度 | 每格变化 1，最小 1、最大 24，提示条同步更新 |
| C 清空，Esc 返回 | 画布清空；原应用可以正常鼠标与键盘操作 |
| 呼出/退出重复 50 次 | 无遗留置顶窗口、失效热键或累积资源泄漏 |
| 原应用关闭、Alt+Tab、锁屏、显示器拔插 | 安全回到待机，托盘可退出 |
| 100%、150%、200% 缩放及负坐标副屏 | 覆盖范围与落笔准确，无可见偏移 |
| 第二次启动，或热键被其他应用占用 | 单实例行为正确；冲突明确提示 |

记录设备型号、笔型号、显示刷新率、DPI、系统版本与测试结果。鼠标测试不能替代触控笔手感验收。用高速录像对比 ZoomIt 和系统原生手写应用，记录跟笔距离与抬笔变化；不在测量前承诺具体端到端毫秒数。

性能目标（尚未测量）：暖启动热键到可落笔不超过 200ms；待机 60 秒平均 CPU 小于 0.5%（记录测试机）；资源在重复呼出后无持续增长。首版产物应包含 Release 可执行文件、构建日志、验收记录和已知限制。

## 环境

开发只安装独立 Visual Studio Build Tools 中的 MSVC x64/x86 工具和 Windows 11 SDK 及其必要依赖，不选择整个工作负载。首个原型使用直接编译脚本，无需 CMake。C++/WinRT 头文件可来自 SDK；若所选 SDK 不含完整投影，则在项目中固定 Microsoft.Windows.CppWinRT 依赖。无需额外 .NET SDK、UWP 工作负载或完整 Visual Studio IDE。

运行基线为 Windows 11 x64、支持 Windows Ink 的触控笔设备和可用图形驱动。首先在实际演示设备上验收；Windows 10、ARM64 和远程桌面支持放到后续兼容性阶段。

2026-09-30 环境已就绪：独立 Build Tools 2022 17.14.41 与 Windows SDK 10.0.26100.0。安装器退出码 0，无需重启。环境检查、Release 编译及桌面测试通过；已有可运行标注原型，真机手写体验待验证。

## 官方参考

- https://learn.microsoft.com/en-us/windows/apps/api-reference/interface-members/ixamlsourcetransparency-isbackgroundtransparent
- https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/xaml-islands/host-standard-control-with-xaml-islands-cpp

- https://learn.microsoft.com/en-us/windows/win32/api/inkpresenterdesktop/nn-inkpresenterdesktop-iinkdesktophost
- https://learn.microsoft.com/en-us/windows/win32/api/inkpresenterdesktop/nn-inkpresenterdesktop-iinkpresenterdesktop
- https://learn.microsoft.com/en-us/uwp/api/windows.ui.input.inking.inkpresenter

