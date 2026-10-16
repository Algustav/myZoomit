#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <functional>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/settings.h"
#include "../src/timer_window.h"
#include "../src/settings_tabs.h"

static void require(bool condition,const char* message) {
    if (!condition) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
struct TimerWindowTest {
    static void dark(TimerWindow& t) { t.darkText=true; }
    static void monitors(TimerWindow& t) {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor); return TRUE;
        },reinterpret_cast<LPARAM>(&monitors));
        require(!monitors.empty(),"display layout available");
        for (auto monitor:monitors) {
            t.full=false;t.relocateFloating(monitor);t.position();
            require(MonitorFromWindow(t.hwnd,MONITOR_DEFAULTTONEAREST)==monitor,"floating placement reaches each connected monitor");
            t.full=true;t.displayMonitor=monitor;t.position();
            RECT actual{};GetWindowRect(t.hwnd,&actual);MONITORINFO info{sizeof(info)};GetMonitorInfoW(monitor,&info);
            require(EqualRect(&actual,&info.rcMonitor)!=FALSE,"fullscreen placement matches selected monitor");
        }
        std::printf("PASS: native placement on %zu connected monitor(s)\n",monitors.size());
    }
    static void contrast(TimerWindow& t) {
        Gdiplus::Bitmap board(1440,720,PixelFormat32bppPARGB);Gdiplus::Graphics canvas(&board);
        canvas.Clear(Gdiplus::Color(255,255,255,255));Gdiplus::SolidBrush black(Gdiplus::Color(255,16,16,16));
        canvas.FillRectangle(&black,360,0,360,720);canvas.FillRectangle(&black,1080,0,360,720);
        t.full=false;
        for (int row=0;row<2;++row) for (int col=0;col<4;++col) {
            t.style=row;t.darkText=col>=2;
            Gdiplus::Bitmap tile(360,360,PixelFormat32bppPARGB);
            {Gdiplus::Graphics g(&tile);t.draw(g,360,360);}
            canvas.DrawImage(&tile,col*360,row*360);
        }
        UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);
        auto codecs=static_cast<Gdiplus::ImageCodecInfo*>(std::malloc(size));Gdiplus::GetImageEncoders(count,size,codecs);
        bool saved=false;
        for (UINT i=0;i<count;++i) if (wcscmp(codecs[i].MimeType,L"image/png")==0) saved=board.Save(L"build\\timer-contrast-preview.png",&codecs[i].Clsid,nullptr)==Gdiplus::Ok;
        std::free(codecs);require(saved,"contrast preview output");
    }
    static void dpi(TimerWindow& t) {
        t.full=false;
        RECT suggested{1700,100,2240,640};
        SendMessageW(t.hwnd,WM_DPICHANGED,MAKEWPARAM(144,144),reinterpret_cast<LPARAM>(&suggested));
        RECT actual{};GetWindowRect(t.hwnd,&actual);
        require(EqualRect(&actual,&suggested)!=FALSE,"DPI change accepts new monitor rectangle instead of old saved position");
        SendMessageW(t.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(180,180));
        POINT cursor{};GetCursorPos(&cursor);
        RECT expected=timerDragDpiRect(t.dragRect,t.dragOrigin,cursor,360,360);
        RECT next{1700,100,2060,460};
        SendMessageW(t.hwnd,WM_DPICHANGED,MAKEWPARAM(96,96),reinterpret_cast<LPARAM>(&next));
        GetWindowRect(t.hwnd,&actual);
        require(EqualRect(&actual,&expected)!=FALSE && t.dragging,"DPI change during drag preserves grab location");
        SendMessageW(t.hwnd,WM_MOUSEMOVE,MK_LBUTTON,0);
        GetWindowRect(t.hwnd,&actual);
        require(EqualRect(&actual,&expected)!=FALSE,"next drag move does not jump to old DPI origin");
        SendMessageW(t.hwnd,WM_LBUTTONUP,0,0);
    }
    static void drag(TimerWindow& t) {
        t.full=false;t.position();
        RECT before{},after{};GetWindowRect(t.hwnd,&before);
        SendMessageW(t.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(180,180));
        require(t.dragging,"single click on digits begins drag without editing");
        t.dragOrigin.x-=40;t.dragOrigin.y-=25;
        SendMessageW(t.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(220,205));
        GetWindowRect(t.hwnd,&after);
        require(after.left==before.left+40 && after.top==before.top+25,"drag moves floating timer");
        SendMessageW(t.hwnd,WM_LBUTTONUP,0,MAKELPARAM(220,205));
        require(!t.dragging && t.floatingRect.left==after.left,"release ends drag and retains position");
    }
    static void settings(TimerWindow& t) {
        auto dialog=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_TIMER_SETTINGS),nullptr,TimerWindow::settingsProcedure,reinterpret_cast<LPARAM>(&t));
        require(dialog!=nullptr,"timer settings resource creates all controls");
        require(SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_GETCOUNT,0,0)==2,"both styles available in settings");
        require(SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_GETCOUNT,0,0)==2,"both window shapes available in settings");
        require(LOBYTE(SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_GETHOTKEY,0,0))=='3',"default timer shortcut");
        DestroyWindow(dialog);
        auto before=t.state.remaining;bool running=t.state.running;
        t.full=true;t.switchShape();
        require(!t.full && !t.defaultFull && t.state.remaining==before && t.state.running==running,"fullscreen to floating preserves timer");
        t.switchShape();
        require(t.full && t.defaultFull && t.state.remaining==before,"floating to fullscreen preserves timer");
    }
    static void input(TimerWindow& t) {
        t.state.remaining=60000;t.state.duration=60000;t.state.running=false;
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_UP,0);
        require(t.state.seconds(GetTickCount64())==120,"up adds one minute");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_DOWN,0);
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_LEFT,0);
        require(t.state.seconds(GetTickCount64())==50,"down/left subtract one minute/ten seconds");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_RIGHT,0);
        require(t.state.seconds(GetTickCount64())==60 && !t.state.running,"right adds ten seconds and preserves pause");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_RIGHT,static_cast<LPARAM>(1)<<30);
        require(t.state.seconds(GetTickCount64())==60,"held arrow repeat cannot apply a second step");
        t.state.remaining=-20000;t.state.anchor=GetTickCount64()-150;t.state.running=true;
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_RIGHT,0);
        require(t.state.remaining==-10000,"running overtime increases by ten seconds despite fractional elapsed time");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_UP,0);
        require(t.state.remaining==0,"running overtime can increase to zero by minute alignment");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_UP,0);
        require(t.state.remaining==60000,"running overtime can return to positive countdown");
        t.state.running=false;
        auto oldStyle=t.style;bool oldDark=t.darkText,oldFull=t.full;
        SendMessageW(t.hwnd,WM_KEYDOWN,'C',0);
        require(t.style==1-oldStyle,"C toggles timer style");
        SendMessageW(t.hwnd,WM_KEYDOWN,'E',0);
        require(t.darkText!=oldDark,"E toggles text theme");
        SendMessageW(t.hwnd,WM_KEYDOWN,'A',0);
        require(t.full!=oldFull,"A toggles fullscreen");
        SendMessageW(t.hwnd,WM_KEYDOWN,'A',0);
        SendMessageW(t.hwnd,WM_KEYDOWN,'C',0);SendMessageW(t.hwnd,WM_KEYDOWN,'E',0);
        t.full=true;t.fullScale=50;
        SendMessageW(t.hwnd,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
        require(t.fullScale==55,"fullscreen wheel scales by five percent");
        SendMessageW(t.hwnd,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
        require(t.fullScale==50,"fullscreen wheel scales down");
        t.full=oldFull;
        SendMessageW(t.hwnd,WM_KEYDOWN,'G',0);
        require(t.color==1,"annotation color shortcut mapping");
        SendMessageW(t.hwnd,WM_KEYDOWN,'R',0);
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_SPACE,0);
        require(t.state.running,"space resumes");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_SPACE,static_cast<LPARAM>(1)<<30);
        require(t.state.running,"space auto-repeat does not toggle twice");
        SendMessageW(t.hwnd,WM_KEYDOWN,VK_SPACE,0);
        for (bool full:{true,false}) {
            t.full=full;t.visible=true;t.minutes=7;t.state.reset(1,GetTickCount64());
            t.state.adjust(-120,GetTickCount64());t.progressTransition.active=true;
            SendMessageW(t.hwnd,WM_KEYDOWN,VK_ESCAPE,0);
            require(!t.visible && !t.state.running && !t.state.started && t.state.remaining==420000 && t.state.duration==420000 && !t.progressTransition.active,"Escape exits both modes and resets to default duration");
        }
        t.minutes=10;
    }
    static void render(TimerWindow& t,const wchar_t* path,int w,int h,bool full,int style) {
        t.full=full;t.style=style;
        t.state.remaining=522000;t.state.duration=600000;t.state.running=false;
        t.progressTransition.active=false;
        Gdiplus::Bitmap bitmap(w,h,PixelFormat32bppPARGB);
        { Gdiplus::Graphics g(&bitmap);t.draw(g,w,h); }
        Gdiplus::Color pixel;bitmap.GetPixel(0,0,&pixel);
        require(pixel.GetA()==(full ? 255 : 0),"floating exterior must remain transparent");
        UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);
        auto codecs=static_cast<Gdiplus::ImageCodecInfo*>(std::malloc(size));
        Gdiplus::GetImageEncoders(count,size,codecs);
        bool saved=false;
        for (UINT i=0;i<count;++i) if (wcscmp(codecs[i].MimeType,L"image/png")==0) saved=bitmap.Save(path,&codecs[i].Clsid,nullptr)==Gdiplus::Ok;
        std::free(codecs);require(saved,"save native renderer preview");
    }
};
int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS|ICC_BAR_CLASSES|ICC_LINK_CLASS|ICC_TAB_CLASSES};
    require(InitCommonControlsEx(&controls)!=FALSE,"common controls available");
    auto settings=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),nullptr,[](HWND,UINT,WPARAM,LPARAM)->INT_PTR {return FALSE;},0);
    require(settings!=nullptr,"tabbed settings resource creates");
    alignSettingsControls(settings);
    auto controlRect=[&](int id) { RECT r{};GetWindowRect(GetDlgItem(settings,id),&r);MapWindowPoints(nullptr,settings,reinterpret_cast<POINT*>(&r),2);return r; };
    auto inputLeft=controlRect(IDC_SNAPSHOT_HOTKEY).left;
    for (int id:{IDC_TIMER_HOTKEY,IDC_HOTKEY,IDC_TIMER_MINUTES,IDC_TIMER_STYLE,IDC_TIMER_SHAPE})
        require(controlRect(id).left==inputLeft,"all settings inputs share one left edge");
    for (int id:{IDC_SNAPSHOT_ZOOM,IDC_DEFAULT_WIDTH}) {
        RECT channel{};SendDlgItemMessageW(settings,id,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        require(controlRect(id).left+channel.left==inputLeft,"visible slider track aligns with other inputs");
    }
    require(!GetDlgItem(settings,IDC_ZOOM_MIN) && !GetDlgItem(settings,IDC_WIDTH_MAX),"slider endpoint labels removed");
    SetDlgItemInt(settings,IDC_TIMER_MINUTES,17,FALSE);
    SendDlgItemMessageW(settings,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('7',HOTKEYF_CONTROL),0);
    for (int page:{0,1,2,3,0}) {
        selectSettingsTab(settings,page);
        auto shown=[&](int id) {return (GetWindowLongPtrW(GetDlgItem(settings,id),GWL_STYLE)&WS_VISIBLE)!=0;};
        require(shown(IDC_SNAPSHOT_HOTKEY)==(page==0) && shown(IDC_HOTKEY)==(page==1) && shown(IDC_TIMER_HOTKEY)==(page==2),"only selected page inputs visible");
        require(GetDlgItemInt(settings,IDC_TIMER_MINUTES,nullptr,FALSE)==17,"tab switching preserves draft values");
        require(LOBYTE(SendDlgItemMessageW(settings,IDC_HOTKEY,HKM_GETHOTKEY,0,0))=='7',"tab switching preserves draft shortcut");
        require(shown(IDC_ABOUT_GROUP)==(page==3) && shown(IDC_INSTRUCTIONS)==(page!=3),"about page replaces instructions without other page controls");
        if (page==3) {
            for (int id:{IDC_ABOUT_TEXT,IDC_PROJECT_LINK,IDC_CONTACT_LINK,IDC_ABOUT_LICENSE,IDC_CREDITS_LINK,IDC_COFFEE_IMAGE,IDC_COFFEE_CAPTION,IDC_ABOUT_AUTHOR})
                require(GetDlgItem(settings,id)!=nullptr && shown(id),"all about content controls available");
            continue;
        }
        auto label=GetDlgItem(settings,IDC_INSTRUCTIONS);RECT area{};GetClientRect(label,&area);
        wchar_t help[2048]{};GetWindowTextW(label,help,2048);auto dc=GetDC(label);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(label,WM_GETFONT,0,0)));
        RECT measured{0,0,area.right,0};DrawTextW(dc,help,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(label,dc);
        require(measured.bottom<=area.bottom,"page instructions fit without clipping");
    }
    auto about=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_ABOUT),nullptr,[](HWND,UINT,WPARAM,LPARAM)->INT_PTR {return FALSE;},0);
    require(about!=nullptr,"original about dialog remains available");
    for (int id:{IDC_PROJECT_LINK,IDC_CONTACT_LINK,IDC_CREDITS_LINK,IDC_COFFEE_CAPTION,IDC_ABOUT_AUTHOR}) {
        wchar_t original[512]{},copied[512]{};GetDlgItemTextW(about,id,original,512);GetDlgItemTextW(settings,id,copied,512);
        require(wcscmp(original,copied)==0,"about page preserves original links and text");
    }
    DestroyWindow(about);
    DestroyWindow(settings);
    CountdownState s;s.reset(1,1000);
    require(s.text(1000)==L"01:00","initial duration");
    require(s.text(61000)==L"00:00","exact zero");
    require(s.text(62000)==L"- 00:01","negative overtime");
    require(s.text(122000)==L"- 01:01","overtime minute rollover");
    require(s.progress(62000)==0,"overtime progress bounded");
    s.toggle(62500);require(s.milliseconds(1000000)==-1500,"pause snapshots monotonic elapsed time");
    s.adjust(60,2000000);require(!s.running && s.milliseconds(3000000)==58500,"adjust while paused across zero");
    s.toggle(3000000);require(s.milliseconds(3001500)==57000,"resume excludes paused interval");
    s.adjust(-10,3001500);require(s.milliseconds(3001500)==47000 && s.running,"adjustment preserves running state");
    s.reset(1440,0);require(s.text(86401000)==L"- 00:01","long elapsed intervals do not drift");
    s.reset(1,0);s.adjust(-600,0);require(s.text(0)==L"- 09:00" && s.duration>=1000,"adjustment into overtime");
    s.reset(4,0);require(s.progress(120000)==.5,"four minute timer half elapsed");
    s.adjust(240,120000);require(s.seconds(120000)==360 && s.progress(120000)==1,"adjusted six minutes becomes new full cycle");
    require(s.progress(300000)==.5,"three minutes later new cycle is half elapsed");
    s.toggle(300000);s.adjust(-60,400000);
    require(!s.running && s.progress(500000)==1 && s.seconds(500000)==120,"shortening paused timer rebases without resuming");
    s.adjust(-180,500000);s.adjust(120,500000);
    require(s.progress(500000)==1 && s.seconds(500000)==60,"overtime adjusted positive begins new cycle");
    s.remaining=-59500;s.running=false;s.adjust(60,500000);
    require(s.progress(500000)==1 && s.duration==500,"subsecond positive remainder still starts full");
    for (auto step:{60,-60,-10,10}) {
        s.remaining=336500;s.running=false;
        s.adjustAligned(step,500000);
        auto expected=step==60 ? 360 : step==-60 ? 240 : step==-10 ? 320 : 340;
        require(s.milliseconds(500000)==expected*1000 && !s.running,"arrow adjustment floors to whole unit after stepping");
        require(s.progress(500000)==1,"aligned adjustment rebases progress");
    }
    s.remaining=-5000;s.adjustAligned(-10,500000);
    require(s.milliseconds(500000)==-20000,"negative adjustment floors across ten-second boundary");
    s.adjustAligned(60,500000);require(s.milliseconds(500000)==0,"aligned overtime adjustment can reach exact zero");
    TimerProgressTransition transition;transition.restart(.5,1000);
    require(transition.value(1,1000)==.5,"progress transition starts without jump");
    require(transition.value(1,1075)==.75,"progress transition midpoint");
    require(transition.value(1,1150)==1 && !transition.active,"progress transition finishes at 150ms");
    RECT source{0,0,1920,1040},saved{780,340,1140,700},leftMonitor{-2560,-200,0,1240};
    RECT moved=timerRelocate(saved,source,leftMonitor,96,144);
    require(moved.left==-1550 && moved.top==250 && moved.right-moved.left==540,"monitor relocation handles negative origins and preserves logical size");
    RECT rightMonitor{1920,0,3840,1040};
    RECT right=timerRelocate(saved,source,rightMonitor,96,96);
    require(right.left==2700 && right.top==340,"second screen restores relative position");
    RECT fallback{0,0,1280,720};RECT restored=timerRelocate(moved,leftMonitor,fallback,144,96);
    require(restored.left==460 && restored.top==180,"disconnected monitor restores visible placement");
    DeleteFileW(L"build\\timer-test.ini");
    { TimerWindow timer;timer.initialize(nullptr,L"build\\timer-test.ini");
      TimerWindowTest::input(timer);
      TimerWindowTest::settings(timer);
      TimerWindowTest::drag(timer);
      TimerWindowTest::dpi(timer);
      TimerWindowTest::monitors(timer);
      TimerWindowTest::render(timer,L"build\\timer-ring-preview.png",1280,720,true,0);
      TimerWindowTest::render(timer,L"build\\timer-bar-preview.png",1280,720,true,1);
      TimerWindowTest::render(timer,L"build\\timer-floating-preview.png",360,360,false,0);
      TimerWindowTest::dark(timer);
      TimerWindowTest::render(timer,L"build\\timer-floating-dark-preview.png",360,360,false,0);
      TimerWindowTest::render(timer,L"build\\timer-floating-dark-bar-preview.png",360,360,false,1);
      TimerWindowTest::contrast(timer);
    }
    std::puts("PASS: zero/overtime, pause/resume, adjustments, long gaps, ring/bar rendering and floating alpha");
}
