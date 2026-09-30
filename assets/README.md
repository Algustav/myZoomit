# 应用图标

`logo-original.png` 是用户于 2026-09-30 明确指定的霓虹彩色铅笔原图。
`logo.png` 使用内置 imagegen 去掉外围白底及外部阴影，保留深色圆角底板、铅笔和笔迹，外部为透明区域。
`myzoomit.ico` 从处理后的 PNG 转换，包含 16、20、24、32、40、48、64、96、128、256 像素尺寸，用于 Windows 可执行文件、托盘和设置窗口。无需附带外部图片即可运行应用。

处理提示：Remove ONLY the white/off-white background and external drop shadow surrounding the dark rounded-square tile. Make that outer area fully transparent, including the rounded outer corners. Preserve the entire dark navy rounded-square tile and the neon gradient cyan-blue-purple-pink-orange pencil and sweeping underline EXACTLY, including their existing shape, angle, positioning, internal glow and colors. No redesign, no new elements, no text. Crop the transparent padding close to the tile's outer bounds so the existing rounded-square tile fills a square image, centered, with no white fringe or external shadow. Keep a clean antialiased transparent outline. This is background removal and trimming only.

2026-09-30：再次使用内置 imagegen 裁去外侧透明留白。处理后的深色底板不透明边界约为 (1, 0) 至 (1253, 1253)，1254×1254 画布基本占满；只保留圆角区域透明。上一版保留为 logo-before-trim.png。编辑提示：Trim the transparent exterior padding tightly to the outer bounds of the existing dark rounded-square tile so the flat portions of the tile left, right, top and bottom touch the image edges. Keep the rounded corners transparent. No additional padding. Preserve the exact existing neon pencil, underline, dark tile, colors, glow, proportions and orientation.
