"""Package the local portable release with its build and validation record. Run from any working directory."""
from pathlib import Path
import hashlib
import shutil
import zipfile

root = Path(__file__).resolve().parent.parent
version = (root / 'VERSION.txt').read_text().strip()
release = root / 'releases' / f'v{version}'
if release.exists():
    raise SystemExit(f'Release already exists; preserve the frozen snapshot: {release}')
for path in (root / 'build' / 'MyZoomIt.exe', root / 'build' / 'build.log', root / 'docs' / f'release-v{version}.md', root / 'build' / 'timer-test.log'):
    if not path.is_file():
        raise SystemExit(f'Missing release input: {path}')

bundle_name = f'MyZoomIt-v{version}-win-x64'
bundle = release / bundle_name
bundle.mkdir(parents=True)
shutil.copy2(root / 'build' / 'MyZoomIt.exe', bundle / 'MyZoomIt.exe')
(bundle / 'settings.ini').write_text('[Hotkey]\nModifiers=2\nKey=50\n[Snapshot]\nModifiers=2\nKey=49\nZoom=200\n[UI]\nShowHint=0\n[Pen]\nDefaultWidth=8\nWidth=8\nColor=0\nTool=0\n[Timer]\nModifiers=2\nKey=51\nMinutes=10\nStyle=0\nFullscreen=1\nTopmost=1\nColor=0\nDarkText=0\nFullScale=50\n', encoding='utf-8')
shutil.copy2(root / 'LICENSE', bundle / 'LICENSE.txt')
shutil.copy2(root / 'third_party' / 'perfect-freehand' / 'LICENSE', bundle / 'perfect-freehand-LICENSE.txt')
(bundle / 'VERSION.txt').write_text(version + '\n', encoding='utf-8')
(bundle / '使用说明.txt').write_text(f'''MyZoomIt v{version} 便携版

适用 Windows 11 x64。无需安装，解压到可写文件夹，双击 MyZoomIt.exe。
程序常驻系统托盘；可能位于隐藏图标区。

Ctrl+1：截图缩放。默认 200%，移动鼠标浏览，滚轮以 25% 缩放。
首次单击或触控进入标注，抬起后再次接触才开始绘制；右键返回浏览并保留笔迹。
Ctrl+2：实时标注。鼠标、触控笔、手指均可绘制，支持压感。
Esc 或再次按模式快捷键退出，最终可见标注画面自动复制到剪贴板。
Ctrl+滚轮：调整笔宽 4–24 px，默认 8 px。
1–5：手写、矩形、椭圆、直线、箭头。
Ctrl 矩形 / Tab 椭圆 / Shift 直线 / Ctrl+Shift 箭头；松开恢复手写。
R 红 / G 绿 / B 蓝 / O 橙 / Y 黄 / P 紫 / W 白。
E 橡皮擦，C 清空，K 黑板；Ctrl+Z 撤销，Ctrl+Y 恢复。
笔或手指输入时隐藏光标，移动鼠标后恢复。

Ctrl+3：倒计时与当前时钟，默认 10 分钟。
A 全屏/透明浮窗，E 深浅色，C 环形/条形，空格暂停/继续。
↑/↓ 增减一分钟，←/→ 增减十秒；先增减再向下对齐到整分或整十秒。
到零后继续负计时。Esc 或再次按 Ctrl+3 退出后重置为默认时长。
浮窗可单击拖动；双击设置时长并重新开始；滚轮调整显示大小。
R/G/B/O/Y/P/W 切换强调色。右键可选样式、文字主题、全屏和计时设置。
全屏默认显示比例 50%，滚轮每格 5%，范围 25%–100%。
优先在鼠标所在屏幕呼出，支持扩展屏幕和跨屏拖动。

托盘“设置”含缩放、标注、倒计时、关于四页。
快捷键须为 Ctrl/Alt 组合，Backspace/Delete 清空；保存后停用对应快捷键。
设置保存在程序旁的 settings.ini，复制整个文件夹即可携带配置。
开机自启和鼠标强调是当前电脑的系统设置，不会随文件夹自动启用。
升级前退出旧程序，保留自己的 settings.ini，再替换 MyZoomIt.exe。
''', encoding='utf-8-sig')

verification = release / 'verification'
verification.mkdir()
shutil.copy2(root / 'build' / 'build.log', verification / 'build.log')
shutil.copy2(root / 'build' / 'timer-test.log', verification / 'timer-test.log')
(verification / 'validation.txt').write_text('Release build completed. Native timer/state, settings resources, tab visibility/draft preservation, About content and geometry regression checks passed. Interactive behavior was reviewed by the user during development; physical pen/touch and every monitor layout were not exhaustively retested for packaging.\n', encoding='utf-8')
shutil.copy2(root / 'docs' / f'release-v{version}.md', release / 'README.md')

with zipfile.ZipFile(release / f'{bundle_name}.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(bundle.iterdir()):
        archive.write(path, f'{bundle_name}/{path.name}')
with zipfile.ZipFile(release / f'MyZoomIt-v{version}-source.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    paths = [root / 'README.md', root / 'VERSION.txt', root / '.gitignore', root / '.gitattributes', root / 'LICENSE']
    for directory in ('src', 'scripts', 'docs', 'assets', 'third_party', 'tests'):
        paths.extend(p for p in (root / directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    for path in sorted(paths):
        archive.write(path, f'MyZoomIt-v{version}-source/{path.relative_to(root).as_posix()}')
hashes = []
for path in [*sorted(bundle.iterdir()), *sorted(release.glob('*.zip'))]:
    hashes.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(release).as_posix()}')
(release / 'SHA256SUMS.txt').write_text('\n'.join(hashes) + '\n', encoding='utf-8')
print(release)
print('\n'.join(hashes))
