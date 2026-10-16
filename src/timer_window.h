#pragma once
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <functional>
#include "timer_state.h"
#include "timer_placement.h"

class TimerWindow {
    friend struct TimerWindowTest;
    HWND hwnd{}, owner{}, settingsDialog{};
    ULONG_PTR graphicsToken{};
    std::wstring ini;
    RECT floatingRect{100,100,460,460};
    bool visible{}, full{true}, topmost{true};
    bool darkText{},placing{};
    UINT floatingDpi{96};
    RECT floatingWork{};
    HMONITOR displayMonitor{};
    bool dragging{};
    POINT dragOrigin{};
    RECT dragRect{};
    TimerProgressTransition progressTransition;
    double displayProgress(uint64_t now) {
        if (state.milliseconds(now)<0) { progressTransition.active=false; return 1.0; }
        return progressTransition.value(state.progress(now),now);
    }
    void restart() { state.reset(minutes,GetTickCount64()); progressTransition.active=false; }
    void adjustTime(int seconds) {
        auto now=GetTickCount64(); auto before=displayProgress(now);
        state.adjustAligned(seconds,now);
        if (state.milliseconds(now)>0) progressTransition.restart(before,now);
        else progressTransition.active=false;
        if (visible) SetTimer(hwnd,RefreshTimer,16,nullptr);
    }
    int wheelRemainder{}, color{}, fullScale{50};
    static constexpr UINT_PTR RefreshTimer=1;
    std::function<bool(UINT,UINT)> changeShortcut;
    static constexpr COLORREF Colors[]={RGB(235,55,65),RGB(34,197,94),RGB(249,115,22),RGB(217,70,239),RGB(59,130,246),RGB(255,255,255),RGB(250,204,21)};
    void text(Gdiplus::Graphics& g,const std::wstring& value,float size,Gdiplus::RectF rect,Gdiplus::Color fill,bool bold=false) {
        Gdiplus::Font font(L"Segoe UI",size,bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::StringFormat format; format.SetAlignment(Gdiplus::StringAlignmentCenter); format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush brush(fill);
        if (full) g.DrawString(value.c_str(),-1,&font,rect,&format,&brush);
        else {
            Gdiplus::FontFamily family(L"Segoe UI"); Gdiplus::GraphicsPath path;
            path.AddString(value.c_str(),-1,&family,bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,size,rect,&format);
            BYTE opposite=(fill.GetR()+fill.GetG()+fill.GetB())>384 ? 15 : 250;
            Gdiplus::Pen edge(Gdiplus::Color(225,opposite,opposite,opposite),std::max(.9f,size*.014f));
            edge.SetLineJoin(Gdiplus::LineJoinRound);g.DrawPath(&edge,&path);g.FillPath(&brush,&path);
        }
    }
    void draw(Gdiplus::Graphics& g,int width,int height) {
        using namespace Gdiplus;
        g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        if (full) g.Clear(darkText ? Color(255,245,245,245) : Color(255,16,16,16)); else g.Clear(Color(0,0,0,0));
        float w=static_cast<float>(width),h=static_cast<float>(height), size=std::min(w,h)*(full ? .78f*fullScale/100.f : .88f);
        float cx=w/2,cy=h/2-(full && style ? h*.115f : 0.f), left=cx-size/2, top=cy-size/2;
        auto now=GetTickCount64(); auto c=Colors[color];
        BYTE alpha=255;
        if (state.seconds(now)<0) alpha=static_cast<BYTE>(205+50*(.5+.5*std::sin(now*.004)));
        Color accent(alpha,GetRValue(c),GetGValue(c),GetBValue(c));
        if (!full) {
            // Near-transparent interior accepts drag input; outside stays click-through.
            SolidBrush hit(Color(1,0,0,0));
            if (!style) g.FillEllipse(&hit,left,top,size,size);
            else g.FillRectangle(&hit,left,cy-size*.33f,size,size*.66f);
        }
        float stroke=std::max(2.0f,size*.04f);
        Pen track(Color(full ? 255 : 160,55,55,55),stroke),progress(accent,stroke);
        track.SetStartCap(LineCapRound); track.SetEndCap(LineCapRound);
        progress.SetStartCap(LineCapRound); progress.SetEndCap(LineCapRound);
        auto fraction=displayProgress(now);
        if (!full) {
            BYTE opposite=darkText ? 250 : 15;
            Pen edge(Color(220,opposite,opposite,opposite),stroke+2.0f);
            edge.SetStartCap(LineCapRound);edge.SetEndCap(LineCapRound);
            if (!style) g.DrawEllipse(&edge,left,top,size,size);
            else g.DrawLine(&edge,left,cy+size*.16f,left+size,cy+size*.16f);
        }
        if (!style) {
            g.DrawEllipse(&track,left,top,size,size);
            if (state.seconds(now)<0) g.DrawEllipse(&progress,left,top,size,size);
            else if (fraction>.00001) g.DrawArc(&progress,left,top,size,size,-90,static_cast<float>(fraction*360));
        } else {
            float y=cy+size*.16f;
            g.DrawLine(&track,left,y,left+size,y);
            g.DrawLine(&progress,left,y,left+size*static_cast<float>(state.seconds(now)<0 ? 1 : fraction),y);
        }
        float digitSize=size*(style ? .30f : .235f);
        auto value=state.text(now);
        // Negative and long timers must fit without clipping.
        digitSize*=std::min(1.0f,5.0f/static_cast<float>(value.size()));
        Color mainText=darkText ? Color(255,25,25,25) : Color(255,248,247,244);
        Color secondary=darkText ? Color(255,65,65,65) : Color(255,150,150,150);
        text(g,value,digitSize,{left,cy-size*(style ? .16f : .29f),size,size*.31f},mainText,true);
        SYSTEMTIME local{}; GetLocalTime(&local); wchar_t clock[16]{};
        swprintf_s(clock,L"%02u:%02u",local.wHour,local.wMinute);
        text(g,clock,size*.12f,{left,cy+size*(style ? .23f : .035f),size,size*.16f},secondary);
        if (!state.running) text(g,L"已暂停",size*.035f,{left,cy-size*.43f,size,size*.08f},secondary);

    }
    void paint() {
        if (!hwnd || !visible) return;
        RECT r{}; GetWindowRect(hwnd,&r); int w=r.right-r.left,h=r.bottom-r.top;
        if (w<=0 || h<=0) return;
        BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
        bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
        auto screen=GetDC(nullptr),memory=CreateCompatibleDC(screen); void* pixels{};
        auto bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
        if (bitmap && memory) {
            auto old=SelectObject(memory,bitmap);
            { Gdiplus::Bitmap image(w,h,w*4,PixelFormat32bppPARGB,static_cast<BYTE*>(pixels)); Gdiplus::Graphics graphics(&image); draw(graphics,w,h); }
            POINT destination{r.left,r.top},origin{}; SIZE size{w,h}; BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd,screen,&destination,&size,memory,&origin,0,&blend,ULW_ALPHA);
            SelectObject(memory,old);
        }
        if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); ReleaseDC(nullptr,screen);
    }
    static UINT monitorDpi(HMONITOR monitor) {
        UINT x=96,y=96;
        auto library=LoadLibraryW(L"shcore.dll");
        if (library) {
            using GetMonitorDpi=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);
            auto read=reinterpret_cast<GetMonitorDpi>(GetProcAddress(library,"GetDpiForMonitor"));
            if (read && FAILED(read(monitor,0,&x,&y))) x=96;
            FreeLibrary(library);
        }
        return x;
    }
    void rememberFloating() {
        GetWindowRect(hwnd,&floatingRect);
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&info)) floatingWork=info.rcWork;
        floatingDpi=GetDpiForWindow(hwnd);
        if (!floatingDpi) floatingDpi=96;
    }
    void relocateFloating(HMONITOR target) {
        MONITORINFO info{sizeof(info)};if (!GetMonitorInfoW(target,&info)) return;
        if (floatingWork.right<=floatingWork.left) floatingWork=info.rcWork;
        UINT dpi=monitorDpi(target);
        floatingRect=timerRelocate(floatingRect,floatingWork,info.rcWork,floatingDpi,dpi);
        floatingWork=info.rcWork;floatingDpi=dpi;
    }
    void position() {
        RECT r{};
        if (full) {
            MONITORINFO info{sizeof(info)};
            if (!displayMonitor || !GetMonitorInfoW(displayMonitor,&info)) {
                displayMonitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
                if (!GetMonitorInfoW(displayMonitor,&info)) return;
            }
            r=info.rcMonitor;
        } else {
            r=floatingRect;
            MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&info)) return;
            LONG width=std::min(r.right-r.left,info.rcWork.right-info.rcWork.left),height=std::min(r.bottom-r.top,info.rcWork.bottom-info.rcWork.top);
            r.left=std::clamp(r.left,info.rcWork.left,info.rcWork.right-width);r.top=std::clamp(r.top,info.rcWork.top,info.rcWork.bottom-height);
            r.right=r.left+width;r.bottom=r.top+height;floatingRect=r;floatingWork=info.rcWork;
        }
        placing=true;
        SetWindowPos(hwnd,(full || topmost) ? HWND_TOPMOST : HWND_NOTOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
        placing=false;
    }
    void switchShape() {
        auto target=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
        if (!full) rememberFloating();
        full=!full;defaultFull=full;
        if (full) { displayMonitor=target;if (beforeFullscreen) beforeFullscreen(); }
        else relocateFloating(target);
        position();paint();save();
    }
    void editTime() {
        DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_TIMER_TIME),hwnd,editProcedure,reinterpret_cast<LPARAM>(this));
    }
    static INT_PTR CALLBACK editProcedure(HWND dialog,UINT msg,WPARAM w,LPARAM l) {
        auto t=reinterpret_cast<TimerWindow*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) { t=reinterpret_cast<TimerWindow*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); SetDlgItemInt(dialog,IDC_TIMER_MINUTES,t->minutes,FALSE); return TRUE; }
        if (msg==WM_COMMAND && LOWORD(w)==IDOK) {
            BOOL ok{}; auto value=GetDlgItemInt(dialog,IDC_TIMER_MINUTES,&ok,FALSE);
            if (!ok || value<1 || value>1440) { MessageBoxW(dialog,L"请输入 1–1440 分钟。",L"设置时长",MB_OK|MB_ICONWARNING); return TRUE; }
            t->minutes=static_cast<int>(value); t->restart(); t->save(); EndDialog(dialog,IDOK); return TRUE;
        }
        if ((msg==WM_COMMAND && LOWORD(w)==IDCANCEL) || msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        return FALSE;
    }
    void contextMenu() {
        auto popup=CreatePopupMenu();
        AppendMenuW(popup,MF_STRING|(!style ? MF_CHECKED : 0),10,L"环形倒计时");
        AppendMenuW(popup,MF_STRING|(style ? MF_CHECKED : 0),11,L"条形倒计时");
        AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
        AppendMenuW(popup,MF_STRING|(!darkText ? MF_CHECKED : 0),8,L"浅色文字");
        AppendMenuW(popup,MF_STRING|(darkText ? MF_CHECKED : 0),9,L"深色文字");
        AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
        AppendMenuW(popup,MF_STRING,4,full ? L"切换悬浮窗" : L"切换全屏");
        AppendMenuW(popup,MF_STRING,7,L"计时设置…");
        POINT p{}; GetCursorPos(&p); int action=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_NONOTIFY,p.x,p.y,0,hwnd,nullptr); DestroyMenu(popup);
        if (action==4) switchShape();
        else if (action==8 || action==9) { darkText=action==9;save(); }
        else if (action==10 || action==11) { style=action-10;save(); }
        else if (action==7) PostMessageW(owner,WM_APP+7,0,0);
        paint();
    }
    static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM w,LPARAM l) {
        auto t=reinterpret_cast<TimerWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (msg==WM_NCCREATE) { t=static_cast<TimerWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); t->hwnd=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(t)); }
        if (!t) return DefWindowProcW(window,msg,w,l);
        switch (msg) {
        case WM_TIMER:
            t->paint();
            if (t->visible && !t->progressTransition.active) SetTimer(window,RefreshTimer,100,nullptr);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(window,&ps); EndPaint(window,&ps); t->paint(); return 0; }
        case WM_MOUSEMOVE:
            if (t->dragging) {
                POINT point{}; GetCursorPos(&point);
                LONG x=t->dragRect.left+point.x-t->dragOrigin.x,y=t->dragRect.top+point.y-t->dragOrigin.y;
                t->floatingRect={x,y,x+t->dragRect.right-t->dragRect.left,y+t->dragRect.bottom-t->dragRect.top};
                SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                return 0;
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (t->dragging) { t->dragging=false; ReleaseCapture(); }
            t->editTime(); return 0;
        case WM_LBUTTONDOWN: {
            if (!t->full) { t->dragging=true; GetCursorPos(&t->dragOrigin); GetWindowRect(window,&t->dragRect); SetCapture(window); }
            t->paint(); return 0;
        }
        case WM_LBUTTONUP:
            if (t->dragging) {
                t->dragging=false; ReleaseCapture(); t->rememberFloating(); t->save();
            } return 0;
        case WM_CAPTURECHANGED: t->dragging=false; return 0;
        case WM_RBUTTONUP: t->contextMenu(); return 0;
        case WM_MOUSEWHEEL: {
            t->wheelRemainder+=GET_WHEEL_DELTA_WPARAM(w); int steps=t->wheelRemainder/WHEEL_DELTA; t->wheelRemainder%=WHEEL_DELTA;
            if (t->full) {
                t->fullScale=std::clamp(t->fullScale+steps*5,25,100);t->paint();t->save();return 0;
            }
            RECT r{}; GetWindowRect(window,&r); int dpi=static_cast<int>(GetDpiForWindow(window));
            int size=std::clamp(r.right-r.left+MulDiv(steps*24,dpi,96),static_cast<LONG>(MulDiv(180,dpi,96)),static_cast<LONG>(MulDiv(800,dpi,96)));
            t->floatingRect={r.left,r.top,r.left+size,r.top+size}; t->position(); t->paint(); t->save();
        } return 0;
        case WM_KEYDOWN:
            if (l&(static_cast<LPARAM>(1)<<30)) return 0;
            if (w==VK_UP) t->adjustTime(60); else if (w==VK_DOWN) t->adjustTime(-60);
            else if (w==VK_LEFT) t->adjustTime(-10); else if (w==VK_RIGHT) t->adjustTime(10);
            else if (!(l&(static_cast<LPARAM>(1)<<30))) {
                if (w=='A') t->switchShape();
                else if (w=='E') { t->darkText=!t->darkText;t->save(); }
                else if (w=='C') { t->style=1-t->style;t->save(); }
                else if (w==VK_SPACE) t->state.toggle(GetTickCount64());
                else if (w==VK_ESCAPE) t->hide();
                else { const wchar_t keys[]=L"RGOPBWY"; for (int i=0;i<7;++i) if (w==static_cast<WPARAM>(keys[i])) { t->color=i; t->save(); break; } }
            } t->paint(); return 0;
        case WM_DPICHANGED: {
            if (t->placing) return 0;
            if (t->full) { t->displayMonitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);t->position();t->paint();return 0; }
            auto suggested=reinterpret_cast<const RECT*>(l);if (!suggested) return 0;
            RECT r=*suggested;POINT point{};GetCursorPos(&point);
            if (t->dragging) r=timerDragDpiRect(t->dragRect,t->dragOrigin,point,r.right-r.left,r.bottom-r.top);
            t->placing=true;
            SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            t->placing=false;t->floatingRect=r;t->floatingDpi=HIWORD(w);
            MONITORINFO info{sizeof(info)};if (GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&info)) t->floatingWork=info.rcWork;
            if (t->dragging) { t->dragOrigin=point;t->dragRect=r; }
            t->paint();return 0;
        }
        case WM_DISPLAYCHANGE:
            if (t->dragging) { t->dragging=false;ReleaseCapture(); }
            t->displayMonitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
            if (!t->full) t->relocateFloating(t->displayMonitor);
            t->position();t->paint();return 0;
        case WM_CLOSE: t->hide(); return 0;
        case WM_DESTROY: KillTimer(window,RefreshTimer); return 0;
        }
        return DefWindowProcW(window,msg,w,l);
    }
    static INT_PTR CALLBACK settingsProcedure(HWND dialog,UINT msg,WPARAM w,LPARAM l) {
        auto t=reinterpret_cast<TimerWindow*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) {
            t=reinterpret_cast<TimerWindow*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); t->settingsDialog=dialog;
            BYTE flags=0; if (t->modifiers&MOD_CONTROL) flags|=HOTKEYF_CONTROL; if (t->modifiers&MOD_ALT) flags|=HOTKEYF_ALT; if (t->modifiers&MOD_SHIFT) flags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_SETHOTKEY,MAKEWORD(t->key,flags),0); SetDlgItemInt(dialog,IDC_TIMER_MINUTES,t->minutes,FALSE);
            for (auto item:{L"圆环倒计时",L"大数字 + 细进度条"}) SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));
            SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_SETCURSEL,t->style,0);
            for (auto item:{L"全屏",L"透明悬浮窗"}) SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));
            SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_SETCURSEL,t->defaultFull ? 0 : 1,0); return TRUE;
        }
        if (!t) return FALSE;
        if (msg==WM_COMMAND && LOWORD(w)==IDC_TIMER_DELETE) { SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_SETHOTKEY,0,0); return TRUE; }
        if (msg==WM_COMMAND && LOWORD(w)==IDOK) {
            BOOL ok{}; auto duration=GetDlgItemInt(dialog,IDC_TIMER_MINUTES,&ok,FALSE);
            if (!ok || duration<1 || duration>1440) { MessageBoxW(dialog,L"请输入 1–1440 分钟。",L"计时设置",MB_OK|MB_ICONWARNING); return TRUE; }
            auto value=SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_GETHOTKEY,0,0); UINT mods=0; BYTE flags=HIBYTE(value);
            if (flags&HOTKEYF_CONTROL) mods|=MOD_CONTROL; if (flags&HOTKEYF_ALT) mods|=MOD_ALT; if (flags&HOTKEYF_SHIFT) mods|=MOD_SHIFT;
            if (!t->changeShortcut(mods,LOBYTE(value))) { MessageBoxW(dialog,L"快捷键无效或已被占用，请使用不同的 Ctrl / Alt 组合键。",L"计时设置",MB_OK|MB_ICONWARNING); return TRUE; }
            auto oldMinutes=t->minutes,oldStyle=t->style; bool oldFull=t->defaultFull;
            t->minutes=static_cast<int>(duration); t->style=static_cast<int>(SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_GETCURSEL,0,0));
            t->defaultFull=SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_GETCURSEL,0,0)==0;
            if (!t->save()) { t->minutes=oldMinutes; t->style=oldStyle; t->defaultFull=oldFull; MessageBoxW(dialog,L"无法保存计时设置。",L"计时设置",MB_OK|MB_ICONWARNING); return TRUE; }
            if (!t->state.started) t->state.remaining=t->state.duration=static_cast<int64_t>(t->minutes)*60000;
            t->paint(); EndDialog(dialog,IDOK); return TRUE;
        }
        if ((msg==WM_COMMAND && LOWORD(w)==IDCANCEL) || msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        if (msg==WM_DESTROY) t->settingsDialog=nullptr;
        return FALSE;
    }
public:
    CountdownState state;
    UINT modifiers{MOD_CONTROL},key{'3'};
    int minutes{10},style{};
    bool defaultFull{true};
    std::function<void()> beforeFullscreen;
    void refresh() { paint(); }
    bool settingsOpen() const { return settingsDialog!=nullptr; }
    bool owns(HWND window) const { return window==hwnd || (hwnd && IsChild(hwnd,window)); }
    bool isFullscreen() const { return visible && full; }
    bool willShowFullscreen() const { return !visible && defaultFull; }
    void raiseFloating() { if (visible && !full && topmost) SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE); }
    bool save() {
        std::wstring section;
        auto add=[&](const wchar_t* name,int value){section+=name;section+=L"=";section+=std::to_wstring(value);section+=L'\0';};
        add(L"Modifiers",static_cast<int>(modifiers));add(L"Key",static_cast<int>(key));add(L"Minutes",minutes);add(L"Style",style);add(L"Fullscreen",defaultFull);add(L"Topmost",topmost);add(L"Color",color);add(L"DarkText",darkText);add(L"FullScale",fullScale);add(L"Dpi",static_cast<int>(floatingDpi));
        add(L"WorkLeft",floatingWork.left);add(L"WorkTop",floatingWork.top);add(L"WorkRight",floatingWork.right);add(L"WorkBottom",floatingWork.bottom);
        add(L"X",floatingRect.left);add(L"Y",floatingRect.top);add(L"Size",floatingRect.right-floatingRect.left);
        return WritePrivateProfileSectionW(L"Timer",section.c_str(),ini.c_str())!=FALSE;
    }
    void initialize(HWND parent,const std::wstring& path) {
        owner=parent; ini=path;
        auto read=[&](const wchar_t* name,int value){return static_cast<int>(GetPrivateProfileIntW(L"Timer",name,value,ini.c_str()));};
        modifiers=static_cast<UINT>(read(L"Modifiers",MOD_CONTROL));key=static_cast<UINT>(read(L"Key",'3'));
        minutes=std::clamp(read(L"Minutes",10),1,1440);style=std::clamp(read(L"Style",0),0,1);defaultFull=read(L"Fullscreen",1)!=0;topmost=read(L"Topmost",1)!=0;color=std::clamp(read(L"Color",0),0,6);
        fullScale=std::clamp(read(L"FullScale",50),25,100);darkText=read(L"DarkText",0)!=0;floatingDpi=static_cast<UINT>(std::clamp(read(L"Dpi",96),48,768));
        floatingWork={read(L"WorkLeft",0),read(L"WorkTop",0),read(L"WorkRight",0),read(L"WorkBottom",0)};
        int x=read(L"X",100),y=read(L"Y",100),size=std::clamp(read(L"Size",360),90,6400);floatingRect={x,y,x+size,y+size};
        if (floatingWork.right<=floatingWork.left) {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(MonitorFromRect(&floatingRect,MONITOR_DEFAULTTONEAREST),&info)) floatingWork=info.rcWork;
        }
        state.remaining=state.duration=static_cast<int64_t>(minutes)*60000;
        Gdiplus::GdiplusStartupInput startup; if (Gdiplus::GdiplusStartup(&graphicsToken,&startup,nullptr)!=Gdiplus::Ok) throw std::runtime_error("Timer graphics initialization failed");
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=procedure;wc.lpszClassName=L"MyZoomIt.Timer";wc.style=CS_DBLCLKS;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        RegisterClassW(&wc);
        hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,wc.lpszClassName,L"MyZoomIt 倒计时",WS_POPUP,100,100,360,360,nullptr,nullptr,wc.hInstance,this);
        if (!hwnd) throw std::runtime_error("Timer window initialization failed");
    }
    void hide() {
        if (!visible) return; if (!full) rememberFloating();
        if (dragging) { dragging=false; ReleaseCapture(); }
        visible=false;ShowWindow(hwnd,SW_HIDE);KillTimer(hwnd,RefreshTimer);
        state.reset(minutes,GetTickCount64());state.running=false;state.started=false;progressTransition.active=false;
        save();
    }
    void toggle() {
        if (visible) { hide(); return; }
        if (!state.started) restart();
        POINT cursor{};GetCursorPos(&cursor);displayMonitor=MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST);
        full=defaultFull;
        if (!full) relocateFloating(displayMonitor);
        if (full && beforeFullscreen) beforeFullscreen();
        visible=true; position(); paint(); ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);SetFocus(hwnd);SetTimer(hwnd,RefreshTimer,100,nullptr);
    }
    INT_PTR showSettings(HWND parent,std::function<bool(UINT,UINT)> callback) {
        if (settingsDialog) { SetForegroundWindow(settingsDialog); return IDCANCEL; }
        changeShortcut=std::move(callback);
        return DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_TIMER_SETTINGS),parent,settingsProcedure,reinterpret_cast<LPARAM>(this));
    }
    ~TimerWindow() { if (hwnd) DestroyWindow(hwnd); if (graphicsToken) Gdiplus::GdiplusShutdown(graphicsToken); }
};
