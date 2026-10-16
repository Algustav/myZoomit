#pragma once
#include "settings.h"
#include <utility>

inline void selectSettingsTab(HWND dialog,int page) {
    const int zoom[]={IDC_ZOOM_HOTKEY_LABEL,IDC_SNAPSHOT_HOTKEY,IDC_SNAPSHOT_DELETE,IDC_ZOOM_LABEL,IDC_SNAPSHOT_ZOOM,IDC_SNAPSHOT_ZOOM_VALUE};
    const int draw[]={IDC_DRAW_HOTKEY_LABEL,IDC_HOTKEY,IDC_DELETE_HOTKEY,IDC_WIDTH_LABEL,IDC_DEFAULT_WIDTH,IDC_DEFAULT_WIDTH_VALUE};
    const int timer[]={IDC_TIMER_HOTKEY_LABEL,IDC_TIMER_HOTKEY,IDC_TIMER_DELETE,IDC_TIMER_MINUTES_LABEL,IDC_TIMER_MINUTES,IDC_TIMER_STYLE_LABEL,IDC_TIMER_STYLE,IDC_TIMER_SHAPE_LABEL,IDC_TIMER_SHAPE};
    for (int id:zoom) ShowWindow(GetDlgItem(dialog,id),page==0 ? SW_SHOW : SW_HIDE);
    for (int id:draw) ShowWindow(GetDlgItem(dialog,id),page==1 ? SW_SHOW : SW_HIDE);
    for (int id:timer) ShowWindow(GetDlgItem(dialog,id),page==2 ? SW_SHOW : SW_HIDE);
    const int about[]={IDC_ABOUT_GROUP,IDC_ABOUT_TEXT,IDC_PROJECT_LINK,IDC_CONTACT_LINK,IDC_ABOUT_LICENSE,IDC_CREDITS_LINK,IDC_COFFEE_IMAGE,IDC_COFFEE_CAPTION,IDC_ABOUT_AUTHOR};
    for (int id:about) ShowWindow(GetDlgItem(dialog,id),page==3 ? SW_SHOW : SW_HIDE);
    for (int id:{IDC_INSTRUCTION_GROUP,IDC_INSTRUCTIONS}) ShowWindow(GetDlgItem(dialog,id),page==3 ? SW_HIDE : SW_SHOW);
    if (page==3) { InvalidateRect(dialog,nullptr,TRUE);return; }
    const wchar_t* help[]={
        L"- 快捷键触发缩放模式后，可以用鼠标滚轮缩放\r\n"
        L"- 按鼠标左键或触摸屏幕进入批注模式\r\n"
        L"- 按鼠标右键回到缩放模式，保留笔迹\r\n"
        L"- 按 Esc 退出缩放模式",
        L"- 用鼠标、触控笔或触摸屏幕绘制，均支持压感效果\r\n"
        L"- 按键 1～5 切换标注的图形模式：\r\n"
        L"  1 手写，2 矩形，3 椭圆，4 直线，5 箭头\r\n"
        L"- 也可配合修饰键快速绘制图形：\r\n"
        L"  Ctrl 矩形，Tab 椭圆，Shift 直线，Ctrl+Shift 箭头\r\n"
        L"- 按键切换笔迹颜色和书写功能，共七种颜色：\r\n"
        L"  R 红色，G 绿色，B 蓝色，O 橙色，Y 黄色，P 紫色，W 白色\r\n"
        L"  E 橡皮擦，C 清空笔迹，K：黑屏可模拟黑板书写\r\n"
        L"- Ctrl+滚轮：调整笔迹粗细（4～24 px，默认值可设置）\r\n"
        L"- Ctrl+Z：撤销上一笔画或图形\r\n"
        L"- Ctrl+Y：恢复撤销的操作",
        L"- 全屏/浮窗两种模式，按 A 键切换，浮窗模式下可拖动\r\n"
        L"- 深/浅两种配色，按 E 切换\r\n"
        L"- 环形/条形两种样式，按 C 切换\r\n"
        L"- 方向键 ↑ / ↓：增减分钟，← / →：增减十秒\r\n"
        L"- 空格：暂停/继续\r\n"
        L"- Esc 退出后倒计时重置"};
    SetDlgItemTextW(dialog,IDC_INSTRUCTIONS,help[page]);
    RECT group{22,page==2 ? 132 : 100,306,258};MapDialogRect(dialog,&group);
    SetWindowPos(GetDlgItem(dialog,IDC_INSTRUCTION_GROUP),nullptr,group.left,group.top,group.right-group.left,group.bottom-group.top,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT text{30,page==2 ? 146 : 114,298,252};MapDialogRect(dialog,&text);
    SetWindowPos(GetDlgItem(dialog,IDC_INSTRUCTIONS),nullptr,text.left,text.top,text.right-text.left,text.bottom-text.top,SWP_NOZORDER|SWP_NOACTIVATE);
    InvalidateRect(dialog,nullptr,TRUE);
}

inline void alignSettingsControls(HWND dialog) {
    auto rect=[&](int id) { RECT r{};GetWindowRect(GetDlgItem(dialog,id),&r);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&r),2);return r; };
    int inputLeft=rect(IDC_SNAPSHOT_HOTKEY).left;
    for (int id:{IDC_SNAPSHOT_ZOOM,IDC_DEFAULT_WIDTH}) {
        RECT channel{};SendDlgItemMessageW(dialog,id,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        auto r=rect(id);SetWindowPos(GetDlgItem(dialog,id),nullptr,inputLeft-channel.left,r.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        int center=r.top+(channel.top+channel.bottom)/2;
        int label=id==IDC_SNAPSHOT_ZOOM ? IDC_ZOOM_LABEL : IDC_WIDTH_LABEL;
        int value=id==IDC_SNAPSHOT_ZOOM ? IDC_SNAPSHOT_ZOOM_VALUE : IDC_DEFAULT_WIDTH_VALUE;
        for (int target:{label,value}) { auto bounds=rect(target);SetWindowPos(GetDlgItem(dialog,target),nullptr,bounds.left,center-(bounds.bottom-bounds.top)/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); }
    }
    for (auto pair:{std::pair{IDC_TIMER_MINUTES,IDC_TIMER_MINUTES_LABEL},std::pair{IDC_TIMER_STYLE,IDC_TIMER_STYLE_LABEL},std::pair{IDC_TIMER_SHAPE,IDC_TIMER_SHAPE_LABEL}}) {
        auto input=rect(pair.first),label=rect(pair.second);
        SetWindowPos(GetDlgItem(dialog,pair.first),nullptr,inputLeft,input.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(dialog,pair.second),nullptr,label.left,(input.top+input.bottom-label.bottom+label.top)/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
}
