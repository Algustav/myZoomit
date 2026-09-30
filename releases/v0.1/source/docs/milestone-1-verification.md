# 首个里程碑验收记录

日期：2026-09-30。产物：build/MyZoomIt.exe（x64 Release）。

最新状态：按用户要求暂时回退速度压感与鼠标起笔修正。显式设置 UseVelocityBasedPressure=false，移除 CoreWetStrokeUpdateSource 起笔压力处理；触控笔真实压感保持开启。颜色快捷键、Ctrl+Z、Ctrl+滚轮、Ctrl+2、触摸、退出清空和会话画笔设置记忆保留。

回退版最终完整回归返回码 0。基础宽度 12 时，慢速鼠标线和快速鼠标线实测均为 12px；停顿后起笔笔头和笔身均为 12px。七色快捷键和再次进入后的颜色/笔宽记忆通过。以下速度模式与起笔修正记录属于此前版本历史，不代表当前启用状态。

## 已完成

Ctrl+2 呼出当前屏幕透明标注层；原生 InkCanvas 采集和显示笔迹；Pen + Mouse + Touch 输入；红色压感笔默认宽度从 3 加粗到 6；Ctrl+滚轮调节宽度（每格 1，范围 1–24，提示条显示当前值）；C 清空；Esc 隐藏与焦点恢复；托盘菜单、单实例、热键冲突提示、会话锁定与显示变化退出、Explorer 重启后恢复托盘入口。

使用系统 XAML Islands 托管原生 InkCanvas；无需新增运行框架安装。已支持 Ctrl+Z 撤销上一笔，每次结束标注清空全部墨迹。未实现缩放、重做、持久保存或设置页。

## 验证证据

scripts/build.cmd：MSVC /W4、C++20、/O2、静态 C++ 运行库，编译链接成功，没有编译警告。原始日志位于 build/build.log。

build/MyZoomIt.exe --self-test：通过实际 SendInput 触发热键、鼠标及 Ctrl+滚轮，通过系统 InjectTouchInput 注入触摸笔画，不通过手工插入 InkStroke 冒充输入。测试背景使用独立蓝色窗口，通过屏幕区域捕获验证背景与红色笔迹同时可见。最终返回码 0，日志如下：

```
hotkey-registered=1 error=0
hotkey-overlay-visible=1
mouse-strokes=1
desktop-background-visible=1
rendered-red-ink-visible=1
clear-remaining=0
hidden-and-input-disabled=1
previous-focus-restored=1
50-toggle-cycles=1
display-change-exits=1
focus-loss-exits=1
second-instance-activates-existing=1
ctrl-wheel-increases-width=1
width-minimum=1
width-maximum=24
touch-injection-initialized=1 error=0
injected-touch-strokes=1
two-strokes-before-undo=2
ctrl-z-leaves-one-stroke=1
exit-clears-ink=1
reopen-empty=1
undo-empty-safe=1
result=0
```

原始日志：build/self-test.log。测试区域截图：build/self-test.bmp（600×400，只捕获蓝色测试窗口内部）。截图已人工查看，蓝色背景与红色笔迹均可见。

显示变化测试发送 WM_DISPLAYCHANGE，验证事件处理路径；没有实际拔插显示器。失焦测试切换到测试窗口。50 次循环验证可见性和模式状态，不等同于长时间资源泄漏检测。

## 待真机验收

### 原生圆珠笔速度模式验证（2026-09-30）

启用 InkDrawingAttributes.ModelerAttributes.UseVelocityBasedPressure=true，保留 IgnorePressure=false，使用默认圆珠笔笔刷；调节基础笔宽时保留速度模式。没有自定义速度或模拟压力算法。

以 SendInput 系统鼠标移动事件生成两条横线，慢速约 100 屏幕像素/秒、快速约 1000 像素/秒；基础宽度设为 12。截取测试窗口内部，测量两笔共同经过的 50 列区域中红色像素的平均覆盖宽度：慢速 22.64 像素、快速 20.00 像素。实际值包含本机 DPI 和抗锯齿影响，不代表基础笔宽的直接单位换算。原始鼠标 InkPoint.Pressure 均为 0.5，但系统渲染宽度明显不同，确认原生模型实际生效。

build/velocity-test.bmp 已查看：上方慢速线较粗，下方快速线较细。最终 self-test 返回码 0；热键、触摸、滚轮上下限、撤销和退出清空回归均通过。

官方接口：https://learn.microsoft.com/pt-br/UWP/api/windows.ui.input.inking.inkmodelerattributes.usevelocitybasedpressure?view=winrt-22621

### 硬件与兼容性

鼠标起笔修正（2026-09-30）：通过 CoreWetStrokeUpdateSource 在系统低延迟墨迹线程上，仅调整鼠标起笔前 16 个墨迹坐标单位的原始压力，系数从 0.35 过渡至 1；按距离而非等待时长过渡，防止按下停顿时膨胀。后续笔画仍启用原生速度压感，没有替换成自行计算速度的笔刷。通过 GetPointerType 区分鼠标，触控笔和触摸不应用这一修正。先前“没有自定义模拟压力算法”的描述仅对应原生速度模式初版，现在加入了上述局部起笔处理。

新增回归：鼠标按下停顿约 500ms，再快速横划；实测笔头与笔身红色覆盖最大宽度均为 14px，held-mouse-start-no-ball=1。截图 build/mouse-start-test.bmp。原生快慢笔宽对照仍通过；七色键、设置记忆、撤销、触摸、清空和焦点恢复完整回归最终返回码 0。桌面输入测试的首次注入仍偶有时序不稳定，重跑通过；真实硬件起笔效果待用户试用确认。

颜色与会话设置更新：R/G/O/P/B/W/Y 分别切换红、绿、橙、粉紫、蓝、白、黄；字母键只在标注模式生效。修改颜色时复制现有绘制属性，只更换颜色，保留笔宽和原生速度压感。结束标注清空墨迹，但不重置画笔设置。退出程序后不持久保存设置。

最新桌面回归返回码 0：七个颜色快捷键全部正确；选择粉紫色、通过 Ctrl+滚轮将宽度设为 9，Esc 结束并再次呼出后，粉紫色、宽度 9、速度压感及压感设置均保留。触摸、撤销、清空与速度模式测试通过。桌面输入测试需要运行期间不操作鼠标键盘，首次回归输入不稳定，重跑完整测试通过。

- 触控笔压感连续变化、笔尾擦除、笔画转角与抬笔稳定性；鼠标测试不能代替触控笔验收。
- 手指真机书写、手掌误触、侧键行为；系统模拟触摸测试不等同于全部硬件设备已验证。
- 不同缩放比例、负坐标副屏、跨显示器切换、真实拔插与锁屏恢复。
- 快捷键冲突的实际交互、Explorer 重启、全屏演示软件与屏幕共享。
- 待机 CPU/内存、200ms 呼出目标、端到端跟笔延迟、长时间运行资源增长。

## 试用步骤

1. 启动程序，打开记事本，确认正常输入。
2. Ctrl+2 呼出，先用鼠标画线，再用手指或笔快速画圈、写中文和做轻重笔触；用 Ctrl+滚轮确认粗细变化和上下限。
3. C 清空，Esc 返回，确认记事本重新接收输入。
4. 连续绘制两笔，Ctrl+Z 后只剩第一笔；Esc 结束后再次呼出，画布应为空。
5. 托盘右键“退出”。本次运行的笔迹不保存。

已知限制：本阶段运行时使用系统 XAML，常驻内存不等同于可执行文件体积；尚未完成手写体验或全部兼容性验收。程序未签名。




## 托盘设置（2026-09-30）

设置入口仅位于托盘右键菜单。使用 Windows 原生快捷键输入控件，可设置 Ctrl / Alt / Shift 组合键，恢复默认 Ctrl+2，并查看绘制、颜色、笔宽、撤销、清空和退出说明。快捷键保存在本机用户目录，保存后立即生效；快捷键冲突或配置写入失败时保留原注册。取消或关闭设置不应用修改。

Release 构建无警告，桌面自动验收退出码 0。新增验证：快捷键更换及配置读取、旧组合释放、冲突保留旧组合、恢复默认、对话框初始化及取消。原有鼠标、触摸、七色、撤销和退出清空回归均通过。测试配置独立位于 build/settings-test.ini，不修改用户配置。
