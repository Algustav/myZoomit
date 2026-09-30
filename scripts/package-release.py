"""Freeze the validated local portable release. Run from any working directory."""
from pathlib import Path
import hashlib
import shutil
import zipfile

root = Path(__file__).resolve().parent.parent
version = (root / 'VERSION.txt').read_text().strip()
release = root / 'releases' / f'v{version}'
if release.exists():
    raise SystemExit(f'Release already exists; preserve the frozen snapshot: {release}')
for name in ('self-test.log', 'cursor-test.log'):
    if 'result=0' not in (root / 'build' / name).read_text():
        raise SystemExit(f'Missing passing validation: {name}')
for name in ('拷贝 A', '拷贝 B'):
    if 'result=0' not in (root / 'build' / 'portable-verification' / name / 'portable-test.log').read_text():
        raise SystemExit(f'Missing portable validation: {name}')

bundle_name = f'MyZoomIt-v{version}-win-x64'
bundle = release / bundle_name
bundle.mkdir(parents=True)
shutil.copy2(root / 'build' / 'MyZoomIt.exe', bundle / 'MyZoomIt.exe')
shutil.copy2(root / 'build' / 'settings.ini', bundle / 'settings.ini')
(bundle / 'VERSION.txt').write_text(version + '\n', encoding='utf-8')
(bundle / '使用说明.txt').write_text(f'''MyZoomIt v{version} 便携版

适用 Windows 11 x64。无需安装，解压到可写文件夹，双击 MyZoomIt.exe。
程序常驻托盘；可能位于托盘隐藏图标区。
复制完整文件夹到其他电脑，程序及 settings.ini 一起携带。

Ctrl+2：进入/退出标注；Esc：退出。
鼠标左键、触控笔、手指：绘制。触控笔支持原生压感。
进入时圆点从200缩至当前笔宽，鼠标指针隐藏，只显示圆点。
Ctrl+滚轮：调整粗细，6–24，默认12。
R 红色 / G 绿色 / O 橙色 / P 粉紫色 / B 蓝色 / W 白色 / Y 黄色。
1 手写 / 2 矩形 / 3 椭圆 / 4 直线 / 5 箭头。
按住 Ctrl 矩形 / Tab 椭圆 / Shift 直线 / Ctrl+Shift 箭头；松开恢复手写。
箭头尖端在按下处，拖动终点为箭尾。
Ctrl+Z：撤销；C：清空。退出标注清空全部墨迹，需要时用 PrtScr 截屏。

托盘右键“设置”：更改快捷键、删除快捷键、切换悬浮菜单、开机自启。
删除快捷键后保存即可停用；可用托盘菜单或双击托盘图标进入标注。
settings.ini 固定在程序旁，保存快捷键、悬浮菜单和正常退出时的画笔设置。
快捷键被其他程序占用时，从托盘修改。

开机自启是当前电脑的设置，不会随文件夹自动启用。
移动已开启自启的程序前先关闭自启，移动后需要时重新勾选。
升级时保留原 settings.ini，退出程序后替换 MyZoomIt.exe。
默认发行设置：Ctrl+2、红色、宽度12、手写，悬浮菜单关闭。
''', encoding='utf-8-sig')

verification = release / 'verification'
verification.mkdir()
for name in ('build.log', 'self-test.log', 'cursor-test.log', 'dependencies.txt'):
    shutil.copy2(root / 'build' / name, verification / name)
for index, name in enumerate(('拷贝 A', '拷贝 B'), 1):
    shutil.copy2(root / 'build' / 'portable-verification' / name / 'portable-test.log', verification / f'portable-copy-{index}.log')
shutil.copy2(root / 'docs' / 'release-v1.0.md', release / 'README.md')

with zipfile.ZipFile(release / f'{bundle_name}.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(bundle.iterdir()):
        archive.write(path, f'{bundle_name}/{path.name}')
with zipfile.ZipFile(release / f'MyZoomIt-v{version}-source.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    paths = [root / 'README.md', root / 'VERSION.txt', root / '.gitignore']
    for directory in ('src', 'scripts', 'docs', 'assets'):
        paths.extend(p for p in (root / directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    for path in sorted(paths):
        archive.write(path, f'MyZoomIt-v{version}-source/{path.relative_to(root).as_posix()}')
hashes = []
for path in [*sorted(bundle.iterdir()), *sorted(release.glob('*.zip'))]:
    hashes.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(release).as_posix()}')
(release / 'SHA256SUMS.txt').write_text('\n'.join(hashes) + '\n', encoding='utf-8')
print(release)
print('\n'.join(hashes))
