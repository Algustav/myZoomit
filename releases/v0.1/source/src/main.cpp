#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include "settings.h"
#include <dwmapi.h>
#include <wtsapi32.h>
#include <inspectable.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.Inking.h>
#include <winrt/Windows.UI.Input.Inking.Core.h>
#include <cmath>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <fstream>
#include <string>
#include <algorithm>
#include <array>

using namespace winrt;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Hosting;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Input::Inking;
namespace XamlInput = Windows::UI::Xaml::Input;
constexpr wchar_t ClassName[] = L"MyZoomIt.Overlay.v1";
constexpr UINT TrayMessage = WM_APP + 1, ActivateMessage = WM_APP + 2;
struct PenColor { WORD key; Color value; const wchar_t* name; };
constexpr std::array<PenColor,7> Palette{{
    {'R',{255,235,55,65},L"红色"},
    {'G',{255,34,197,94},L"绿色"},
    {'O',{255,249,115,22},L"橙色"},
    {'P',{255,217,70,239},L"粉紫色"},
    {'B',{255,59,130,246},L"蓝色"},
    {'W',{255,255,255,255},L"白色"},
    {'Y',{255,250,204,21},L"黄色"}
}};

// Documented Windows.UI.Xaml interface; not declared by the SDK projection.
MIDL_INTERFACE("06636c29-5a17-458d-8ea2-2422d997a922")
IXamlSourceTransparency : ::IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBackgroundTransparent(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBackgroundTransparent(boolean value) = 0;
};

class App {
    HWND window{};
    HWND island{};
    HWND previous{};
    HWND testBackground{};
    HANDLE testDuplicate{};
    POINT testOrigin{};
    HMONITOR monitor{};
    bool active{}, changing{}, hotkey{}, selfTest{};
    UINT taskbarCreated{};
    WindowsXamlManager manager{nullptr};
    DesktopWindowXamlSource source{nullptr};
    com_ptr<IDesktopWindowXamlSourceNative2> native;
    InkCanvas canvas{nullptr};
    InkPresenter presenter{nullptr};




    TextBlock hintText{nullptr};
    float penWidth{12};
    size_t colorIndex{};
    int wheelRemainder{};
    unsigned testStep{};
    int testResult{1};
    std::ofstream log;
    HWND settingsWindow{};
    UINT hotkeyModifiers{MOD_CONTROL}, hotkeyKey{'2'};
    int hotkeyId{1};
    std::wstring settingsPath;
    std::wstring shortcutText() const {
        std::wstring result;
        if (hotkeyModifiers & MOD_CONTROL) result+=L"Ctrl+";
        if (hotkeyModifiers & MOD_ALT) result+=L"Alt+";
        if (hotkeyModifiers & MOD_SHIFT) result+=L"Shift+";
        wchar_t name[64]{};
        GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(hotkeyKey,MAPVK_VK_TO_VSC)<<16),name,64);
        return result+name;
    }
    static bool validShortcut(UINT mods, UINT code) {
        return (mods & (MOD_CONTROL|MOD_ALT)) && !(mods & ~(MOD_CONTROL|MOD_ALT|MOD_SHIFT)) &&
            code>0 && code<256 && code!=VK_F12 && code!=VK_CONTROL && code!=VK_SHIFT &&
            code!=VK_MENU && code!=VK_LWIN && code!=VK_RWIN;
    }
    void loadSettings() {
        if (selfTest) {
            wchar_t path[MAX_PATH]{};
            GetFullPathNameW(L"build\\settings-test.ini",MAX_PATH,path,nullptr);
            settingsPath=path; return;
        }
        wchar_t folder[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,folder))) return;
        settingsPath=std::wstring(folder)+L"\\MyZoomIt\\settings.ini";
        auto mods=GetPrivateProfileIntW(L"Hotkey",L"Modifiers",MOD_CONTROL,settingsPath.c_str());
        auto code=GetPrivateProfileIntW(L"Hotkey",L"Key",'2',settingsPath.c_str());
        if (validShortcut(mods,code)) { hotkeyModifiers=mods; hotkeyKey=code; }
    }
    bool applyShortcut(UINT mods, UINT code, std::wstring& error) {
        if (!validShortcut(mods,code)) { error=L"请选择包含 Ctrl 或 Alt 的组合键；F12 为系统保留键。"; return false; }
        if (mods==hotkeyModifiers && code==hotkeyKey && hotkey) return true;
        int next=hotkeyId==1 ? 2 : 1;
        if (!RegisterHotKey(window,next,mods|MOD_NOREPEAT,code)) {
            error=L"该快捷键已被占用或不可用，请选择其他组合键。"; return false;
        }
        auto parent=settingsPath.substr(0,settingsPath.find_last_of(L"\\"));
        CreateDirectoryW(parent.c_str(),nullptr);
        auto section=L"Modifiers="+std::to_wstring(mods)+L'\0'+L"Key="+std::to_wstring(code)+L'\0';
        if (settingsPath.empty() || !WritePrivateProfileSectionW(L"Hotkey",section.c_str(),settingsPath.c_str())) {
            UnregisterHotKey(window,next); error=L"无法保存设置，原快捷键仍然有效。"; return false;
        }
        if (hotkey) UnregisterHotKey(window,hotkeyId);
        hotkeyId=next; hotkey=true; hotkeyModifiers=mods; hotkeyKey=code;
        NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; icon.uFlags=NIF_TIP;
        auto tip=L"MyZoomIt · "+shortcutText()+L" 标注";
        wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE); Shell_NotifyIconW(NIM_MODIFY,&icon);
        return true;
    }
    static INT_PTR CALLBACK settingsProcedure(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
        auto app=reinterpret_cast<App*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) {
            app=reinterpret_cast<App*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); app->settingsWindow=dialog;
            BYTE flags=0;
            if (app->hotkeyModifiers & MOD_CONTROL) flags|=HOTKEYF_CONTROL;
            if (app->hotkeyModifiers & MOD_ALT) flags|=HOTKEYF_ALT;
            if (app->hotkeyModifiers & MOD_SHIFT) flags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD(app->hotkeyKey,flags),0);
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETRULES,HKCOMB_NONE|HKCOMB_S,HOTKEYF_CONTROL);
            return TRUE;
        }
        if (!app) return FALSE;
        if (msg==WM_COMMAND) {
            if (LOWORD(w)==IDC_RESET) {
                SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('2',HOTKEYF_CONTROL),0);
                SetDlgItemTextW(dialog,IDC_ERROR,L""); return TRUE;
            }
            if (LOWORD(w)==IDOK) {
                auto value=SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_GETHOTKEY,0,0);
                UINT mods=0; auto flags=HIBYTE(value);
                if (flags & HOTKEYF_CONTROL) mods|=MOD_CONTROL;
                if (flags & HOTKEYF_ALT) mods|=MOD_ALT;
                if (flags & HOTKEYF_SHIFT) mods|=MOD_SHIFT;
                std::wstring error;
                if (!app->applyShortcut(mods,LOBYTE(value),error)) {
                    SetDlgItemTextW(dialog,IDC_ERROR,error.c_str()); return TRUE;
                }
                EndDialog(dialog,IDOK); return TRUE;
            }
            if (LOWORD(w)==IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
        }
        if (msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        if (msg==WM_DESTROY) app->settingsWindow=nullptr;
        return FALSE;
    }
    void settings() {
        if (settingsWindow) { SetForegroundWindow(settingsWindow); return; }
        leave();
        DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),nullptr,settingsProcedure,reinterpret_cast<LPARAM>(this));
    }

    void updateHint() {
        hintText.Text(std::wstring(L"标注中  ·  ")+Palette[colorIndex].name+L"  ·  Ctrl+滚轮 粗细 "+
            std::to_wstring(static_cast<int>(penWidth))+L"  ·  Ctrl+Z 撤销  ·  C 清空  ·  Esc 返回");
    }
    bool setColor(WORD keyCode) {
        for (size_t i=0;i<Palette.size();++i) {
            if (Palette[i].key!=keyCode) continue;
            colorIndex=i;
            auto attributes=presenter.CopyDefaultDrawingAttributes();
            attributes.Color(Palette[i].value);
            presenter.UpdateDefaultDrawingAttributes(attributes);
            updateHint(); return true;
        }
        return false;
    }

    void updateWidth(float width) {
        penWidth=std::clamp(width,1.0f,24.0f);
        auto attributes=presenter.CopyDefaultDrawingAttributes();
        attributes.Size({penWidth,penWidth});
        attributes.ModelerAttributes().UseVelocityBasedPressure(false);
        presenter.UpdateDefaultDrawingAttributes(attributes);
        updateHint();
    }
    void wheel(int delta) {
        wheelRemainder+=delta;
        auto steps=wheelRemainder/WHEEL_DELTA;
        wheelRemainder%=WHEEL_DELTA;
        if (steps) updateWidth(penWidth+static_cast<float>(steps));
    }
    static void mouseWheel(int delta) {
        INPUT input{}; input.type=INPUT_MOUSE;
        input.mi.dwFlags=MOUSEEVENTF_WHEEL; input.mi.mouseData=static_cast<DWORD>(delta);
        SendInput(1,&input,sizeof(input));
    }
    static void mouseMove(int x,int y) {
        INPUT input{}; input.type=INPUT_MOUSE;
        input.mi.dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        auto width=GetSystemMetrics(SM_CXVIRTUALSCREEN), height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
        input.mi.dx=static_cast<LONG>((static_cast<long long>(x-GetSystemMetrics(SM_XVIRTUALSCREEN))*65536+32768)/width);
        input.mi.dy=static_cast<LONG>((static_cast<long long>(y-GetSystemMetrics(SM_YVIRTUALSCREEN))*65536+32768)/height);
        SendInput(1,&input,sizeof(input));
    }
    void touch(DWORD flags, int offset) {
        POINTER_TOUCH_INFO contact{};
        contact.pointerInfo.pointerType=PT_TOUCH; contact.pointerInfo.pointerId=0;
        contact.pointerInfo.pointerFlags=flags;
        contact.pointerInfo.ptPixelLocation={testOrigin.x+300+offset,testOrigin.y+300};
        auto p=contact.pointerInfo.ptPixelLocation;
        contact.rcContact={p.x-2,p.y-2,p.x+2,p.y+2};
        contact.touchMask=TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        contact.pressure=512; contact.orientation=90;
        if (!InjectTouchInput(1,&contact)) {
            log << "touch-injection-error=" << GetLastError() << "\n";
            testResult=1;
        }
    }

    static void key(WORD value, bool up = false) {
        INPUT input{}; input.type=INPUT_KEYBOARD; input.ki.wVk=value;
        input.ki.dwFlags=up ? KEYEVENTF_KEYUP : 0;
        SendInput(1,&input,sizeof(input));
    }

    void tray() {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = window; icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        auto tip=L"MyZoomIt · "+shortcutText()+L" 标注";
        wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &icon);
    }
    void clear() { if (presenter) presenter.StrokeContainer().Clear(); }
    void undo() {
        auto container=presenter.StrokeContainer();
        auto strokes=container.GetStrokes();
        if (!strokes.Size()) return;
        for (auto const& stroke : strokes) stroke.Selected(false);
        strokes.GetAt(strokes.Size()-1).Selected(true);
        container.DeleteSelected();
    }
    void leave(bool restore = true) {
        if (!active || changing) return;
        changing = true;
        presenter.IsInputEnabled(false);
        ShowWindow(window, SW_HIDE);
        active = false;
        clear();
        if (restore && IsWindow(previous)) SetForegroundWindow(previous);
        changing = false;
    }
    void enter() {
        if (settingsWindow) { SetForegroundWindow(settingsWindow); return; }
        if (changing) return;
        if (active) { leave(); return; }
        changing = true;
        previous = GetForegroundWindow();
        POINT point{}; GetCursorPos(&point);
        auto next = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        if (next != monitor) clear();
        monitor = next;
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) { changing = false; return; }
        auto r = info.rcMonitor;
        try {
            SetWindowPos(window, HWND_TOPMOST, r.left, r.top, r.right-r.left, r.bottom-r.top, 0);
            presenter.IsInputEnabled(true);
            active = true;
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            SetFocus(island);
            changing = false;
        } catch (...) {
            changing = false;
            ShowWindow(window,SW_HIDE); active=false;
            throw;
        }
    }
    void menu() {
        auto popup = CreatePopupMenu();
        auto label=L"开始 / 结束标注\t"+shortcutText();
        AppendMenuW(popup, MF_STRING, 1, label.c_str());
        AppendMenuW(popup, MF_STRING, 2, L"清空笔迹");
        AppendMenuW(popup, MF_STRING, 4, L"设置…");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 3, L"退出");
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(window);
        auto command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY,
            point.x, point.y, 0, window, nullptr);
        DestroyMenu(popup);
        if (command == 1) enter();
        else if (command == 2) clear();
        else if (command == 3) PostMessageW(window, WM_CLOSE, 0, 0);
        else if (command == 4) settings();
        PostMessageW(window, WM_NULL, 0, 0);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) noexcept {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            app->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(hwnd, message, w, l);
        try { return app->message(message, w, l); }
        catch (hresult_error const& e) {
            app->leave(false);
            auto text = std::wstring(L"Windows Ink 操作失败：") + e.message().c_str();
            MessageBoxW(nullptr, text.c_str(), L"MyZoomIt", MB_OK | MB_ICONERROR);
            return 0;
        } catch (...) { app->leave(false); return 0; }
    }
    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        if (taskbarCreated && msg == taskbarCreated) { tray(); return 0; }
        switch (msg) {
        case ActivateMessage: if (!active) enter(); return 0;
        case WM_HOTKEY: if (static_cast<int>(w)==hotkeyId) enter(); return 0;
        case WM_SIZE:
            if (island) SetWindowPos(island, nullptr, 0, 0, LOWORD(l), HIWORD(l), SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) leave(false); return 0;
        case WM_DISPLAYCHANGE: case WM_DPICHANGED: leave(false); monitor = nullptr; clear(); return 0;
        case WM_WTSSESSION_CHANGE: if (w == WTS_SESSION_LOCK) leave(false); return 0;
        case TrayMessage:
            if (l == WM_LBUTTONDBLCLK) enter();
            else if (l == WM_RBUTTONUP) menu();
            return 0;
        case WM_TIMER: if (selfTest) test(); return 0;
        case WM_CLOSE: if (settingsWindow) EndDialog(settingsWindow,IDCANCEL); leave(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            UnregisterHotKey(window, hotkeyId);
            WTSUnRegisterSessionNotification(window);
            { NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; Shell_NotifyIconW(NIM_DELETE, &icon); }
            PostQuitMessage(selfTest ? testResult : 0); return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }
    void test() {
        // Uses actual desktop mouse input; only runs with explicit --self-test.
        if (testStep == 0) {
            POINT point{}; GetCursorPos(&point);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&info);
            testOrigin={info.rcMonitor.left,info.rcMonitor.top};
            WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"MyZoomIt.TestBackground";
            wc.hbrBackground=CreateSolidBrush(RGB(20,80,130));
            RegisterClassW(&wc);
            testBackground=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"MyZoomIt test",WS_POPUP | WS_VISIBLE,
                testOrigin.x+100,testOrigin.y+100,600,400,nullptr,nullptr,wc.hInstance,nullptr);
            SetForegroundWindow(testBackground);
            mouseMove(testOrigin.x+240,testOrigin.y+240);
            key(VK_CONTROL); key('2'); key('2',true); key(VK_CONTROL,true);
        } else if (testStep == 1) {
            log << "hotkey-overlay-visible=" << (active && IsWindowVisible(window)) << "\n";
        } else if (testStep == 3) {
            // Allow first-show XAML layout/input attachment to settle before injection.
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            SendInput(1, &input, sizeof(input));
        } else if (testStep < 10) {
            RECT r{}; GetWindowRect(window, &r);
            mouseMove(r.left + 240 + static_cast<int>(testStep)*20, r.top + 240 + static_cast<int>(testStep)*8);
        } else if (testStep == 10) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            SendInput(1, &input, sizeof(input));
        } else if (testStep == 13) {
            auto strokes = presenter.StrokeContainer().GetStrokes().Size();
            log << "mouse-strokes=" << strokes << "\n";
            auto dc=GetDC(nullptr);
            auto memory=CreateCompatibleDC(dc);
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=600; bitmap.bmiHeader.biHeight=-400;
            bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32; bitmap.bmiHeader.biCompression=BI_RGB;
            void* pixels{};
            auto image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
            if (!image || !pixels) throw hresult_error(E_OUTOFMEMORY);
            auto old=SelectObject(memory,image);
            BitBlt(memory,0,0,600,400,dc,testOrigin.x+100,testOrigin.y+100,SRCCOPY | CAPTUREBLT);
            auto pixel=[pixels](int x,int y) {
                auto value=static_cast<DWORD*>(pixels)[(y-100)*600+x-100];
                return RGB((value>>16)&255,(value>>8)&255,value&255);
            };
            bool transparent=pixel(500,400)==RGB(20,80,130);
            bool inkVisible=false;
            for (int y=235;y<325;++y) for (int x=235;x<445;++x) {
                auto color=pixel(x,y);
                if (GetRValue(color)>180 && GetGValue(color)<100 && GetBValue(color)<110) inkVisible=true;
            }
            BITMAPFILEHEADER file{}; file.bfType=0x4D42;
            file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+600*400*4;
            std::ofstream capture("build/self-test.bmp",std::ios::binary);
            capture.write(reinterpret_cast<char*>(&file),sizeof(file));
            capture.write(reinterpret_cast<char*>(&bitmap.bmiHeader),sizeof(BITMAPINFOHEADER));
            capture.write(static_cast<char*>(pixels),600*400*4);
            SelectObject(memory,old); DeleteObject(image); DeleteDC(memory);
            ReleaseDC(nullptr,dc);
            log << "desktop-background-visible=" << transparent << "\n";
            log << "rendered-red-ink-visible=" << inkVisible << "\n";
            testResult=strokes>0 && transparent && inkVisible && hotkey ? 0 : 1;
            key('C'); key('C',true);
        } else if (testStep == 15) {
            auto remaining = presenter.StrokeContainer().GetStrokes().Size();
            log << "clear-remaining=" << remaining << "\n";
            if (remaining!=0) testResult=1;
            key(VK_ESCAPE); key(VK_ESCAPE,true);
        } else if (testStep == 17) {
            bool hidden = !IsWindowVisible(window) && !presenter.IsInputEnabled();
            log << "hidden-and-input-disabled=" << hidden << "\n";
            bool focusRestored=GetForegroundWindow()==previous;
            log << "previous-focus-restored=" << focusRestored << "\n";
            bool cycles=true;
            for (int i=0; i<50; ++i) { enter(); cycles &= active && IsWindowVisible(window); leave(); cycles &= !active && !IsWindowVisible(window); }
            log << "50-toggle-cycles=" << cycles << "\n";
            if (!hidden || !cycles || !focusRestored) testResult=1;
            enter();
            SendMessageW(window,WM_DISPLAYCHANGE,0,0);
            bool displayExit=!active && !IsWindowVisible(window);
            log << "display-change-exits=" << displayExit << "\n";
            enter(); SetForegroundWindow(testBackground);
            bool lostFocusExit=!active && !IsWindowVisible(window);
            log << "focus-loss-exits=" << lostFocusExit << "\n";
            if (!displayExit || !lostFocusExit) testResult=1;
            wchar_t executable[MAX_PATH]{};
            GetModuleFileNameW(nullptr,executable,MAX_PATH);
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            if (CreateProcessW(executable,nullptr,nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) {
                testDuplicate=process.hProcess; CloseHandle(process.hThread);
            } else testResult=1;
        } else if (testStep == 22) {
            DWORD exitCode=STILL_ACTIVE;
            if (testDuplicate) { GetExitCodeProcess(testDuplicate,&exitCode); CloseHandle(testDuplicate); }
            bool singleInstance=exitCode==0 && active && IsWindowVisible(window);
            log << "second-instance-activates-existing=" << singleInstance << "\n";
            if (!singleInstance) testResult=1;
            key(VK_CONTROL); mouseWheel(2*WHEEL_DELTA);
        } else if (testStep == 24) {
            bool resized=penWidth==14 && presenter.CopyDefaultDrawingAttributes().Size().Width==14;
            log << "ctrl-wheel-increases-width=" << resized << "\n";
            if (!resized) testResult=1;
            mouseWheel(-30*WHEEL_DELTA);
        } else if (testStep == 26) {
            log << "width-minimum=" << penWidth << "\n";
            if (penWidth!=1) testResult=1;
            mouseWheel(40*WHEEL_DELTA);
        } else if (testStep == 28) {
            log << "width-maximum=" << penWidth << "\n";
            if (penWidth!=24) testResult=1;
            key(VK_CONTROL,true); updateWidth(12); clear();
            auto initialized=InitializeTouchInjection(1,TOUCH_FEEDBACK_NONE);
            log << "touch-injection-initialized=" << initialized << " error=" << GetLastError() << "\n";
            if (!initialized) testResult=1;
            touch(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,0);
        } else if (testStep == 29 || testStep == 30) {
            touch(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,static_cast<int>(testStep-28)*30);
        } else if (testStep == 31) {
            touch(POINTER_FLAG_UP,60);
        } else if (testStep == 34) {
            auto touchStrokes=presenter.StrokeContainer().GetStrokes().Size();
            log << "injected-touch-strokes=" << touchStrokes << "\n";
            if (touchStrokes==0) testResult=1;
            touch(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,0);
        } else if (testStep == 35) {
            touch(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,30);
        } else if (testStep == 36) {
            touch(POINTER_FLAG_UP,30);
        } else if (testStep == 39) {
            auto count=presenter.StrokeContainer().GetStrokes().Size();
            log << "two-strokes-before-undo=" << count << "\n";
            if (count!=2) testResult=1;
            key(VK_CONTROL); key('Z'); key('Z',true); key(VK_CONTROL,true);
        } else if (testStep == 41) {
            auto count=presenter.StrokeContainer().GetStrokes().Size();
            log << "ctrl-z-leaves-one-stroke=" << count << "\n";
            if (count!=1) testResult=1;
            key(VK_ESCAPE); key(VK_ESCAPE,true);
        } else if (testStep == 43) {
            bool exitCleared=!active && presenter.StrokeContainer().GetStrokes().Size()==0;
            log << "exit-clears-ink=" << exitCleared << "\n";
            if (!exitCleared) testResult=1;
            enter();
            bool reopenedEmpty=presenter.StrokeContainer().GetStrokes().Size()==0;
            log << "reopen-empty=" << reopenedEmpty << "\n";
            if (!reopenedEmpty) testResult=1;
            key(VK_CONTROL); key('Z'); key('Z',true); key(VK_CONTROL,true);
        } else if (testStep == 45) {
            log << "undo-empty-safe=" << (presenter.StrokeContainer().GetStrokes().Size()==0) << "\n";
            auto enabled=presenter.CopyDefaultDrawingAttributes().ModelerAttributes().UseVelocityBasedPressure();
            log << "velocity-pressure-disabled=" << !enabled << "\n";
            if (enabled) testResult=1;
            updateWidth(12);
            mouseMove(testOrigin.x+220,testOrigin.y+350);
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            SendInput(1,&input,sizeof(input));
        } else if (testStep>45 && testStep<66) {
            mouseMove(testOrigin.x+220+static_cast<int>(testStep-45)*10,testOrigin.y+350);
        } else if (testStep==66) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            SendInput(1,&input,sizeof(input));
        } else if (testStep==69) {
            mouseMove(testOrigin.x+220,testOrigin.y+450);
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            SendInput(1,&input,sizeof(input));
        } else if (testStep==70 || testStep==71) {
            mouseMove(testOrigin.x+220+static_cast<int>(testStep-69)*100,testOrigin.y+450);
        } else if (testStep==72) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            SendInput(1,&input,sizeof(input));
        } else if (testStep==76) {
            auto strokes=presenter.StrokeContainer().GetStrokes();
            log << "velocity-test-strokes=" << strokes.Size() << "\n";
            if (strokes.Size()!=2) testResult=1;
            for (uint32_t i=0;i<strokes.Size();++i) {
                auto points=strokes.GetAt(i).GetInkPoints();
                float total=0;
                for (auto const& point : points) total+=point.Pressure();
                log << (i==0 ? "slow" : "fast") << "-average-pressure=" << (points.Size() ? total/points.Size() : 0) << "\n";
            }
            auto dc=GetDC(nullptr); auto memory=CreateCompatibleDC(dc);
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=600; bitmap.bmiHeader.biHeight=-400;
            bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32;
            void* pixels{}; auto image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
            if (!image || !pixels) throw hresult_error(E_OUTOFMEMORY);
            auto old=SelectObject(memory,image);
            BitBlt(memory,0,0,600,400,dc,testOrigin.x+100,testOrigin.y+100,SRCCOPY | CAPTUREBLT);
            auto thickness=[pixels](int centerY) {
                int count=0;
                for (int x=250;x<300;++x) for (int y=centerY-25;y<centerY+25;++y) {
                    auto p=static_cast<DWORD*>(pixels)[(y-100)*600+x-100];
                    if (((p>>16)&255)>180 && ((p>>8)&255)<100 && (p&255)<110) ++count;
                }
                return count/50.0f;
            };
            auto slow=thickness(350), fast=thickness(450);
            log << "slow-rendered-width-px=" << slow << "\nfast-rendered-width-px=" << fast << "\n";
            bool constant=slow>0 && fast>0 && std::abs(slow-fast)<=1.0f;
            log << "mouse-width-independent-of-speed=" << constant << "\n";
            if (!constant) testResult=1;
            BITMAPFILEHEADER file{}; file.bfType=0x4D42;
            file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+600*400*4;
            std::ofstream capture("build/velocity-test.bmp",std::ios::binary);
            capture.write(reinterpret_cast<char*>(&file),sizeof(file));
            capture.write(reinterpret_cast<char*>(&bitmap.bmiHeader),sizeof(BITMAPINFOHEADER));
            capture.write(static_cast<char*>(pixels),600*400*4);
            SelectObject(memory,old); DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr,dc);
        } else if (testStep>=77 && testStep<=90) {
            auto index=(testStep-77)/2;
            if ((testStep-77)%2==0) { key(Palette[index].key); key(Palette[index].key,true); }
            else {
                auto attributes=presenter.CopyDefaultDrawingAttributes();
                bool correct=attributes.Color()==Palette[index].value && attributes.Size().Width==12 &&
                    !attributes.ModelerAttributes().UseVelocityBasedPressure();
                log << "color-shortcut-" << static_cast<char>(Palette[index].key) << "=" << correct << "\n";
                if (!correct) testResult=1;
            }
        } else if (testStep==92) {
            key('P'); key('P',true);
            key(VK_CONTROL); mouseWheel(-3*WHEEL_DELTA); key(VK_CONTROL,true);
        } else if (testStep==94) {
            key(VK_ESCAPE); key(VK_ESCAPE,true);
        } else if (testStep==96) {
            enter();
            auto attributes=presenter.CopyDefaultDrawingAttributes();
            bool remembered=attributes.Color()==Palette[3].value && attributes.Size().Width==9 &&
                !attributes.ModelerAttributes().UseVelocityBasedPressure() && !attributes.IgnorePressure();
            log << "pen-settings-preserved-on-reentry=" << remembered << "\n";
            if (!remembered) testResult=1;
            clear(); setColor('R'); updateWidth(12);
        } else if (testStep==98) {
            mouseMove(testOrigin.x+220,testOrigin.y+400);
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            SendInput(1,&input,sizeof(input));
        } else if (testStep>=103 && testStep<=105) {
            mouseMove(testOrigin.x+220+static_cast<int>(testStep-102)*70,testOrigin.y+400);
        } else if (testStep==106) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            SendInput(1,&input,sizeof(input));
        } else if (testStep==110) {
            auto dc=GetDC(nullptr); auto memory=CreateCompatibleDC(dc);
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=600; bitmap.bmiHeader.biHeight=-400;
            bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32;
            void* pixels{}; auto image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
            if (!image || !pixels) throw hresult_error(E_OUTOFMEMORY);
            auto old=SelectObject(memory,image);
            BitBlt(memory,0,0,600,400,dc,testOrigin.x+100,testOrigin.y+100,SRCCOPY | CAPTUREBLT);
            auto maxWidth=[pixels](int begin,int end) {
                int maximum=0;
                for (int x=begin;x<end;++x) {
                    int count=0;
                    for (int y=365;y<435;++y) {
                        auto p=static_cast<DWORD*>(pixels)[(y-100)*600+x-100];
                        if (((p>>16)&255)>180 && ((p>>8)&255)<100 && (p&255)<110) ++count;
                    }
                    maximum=std::max(maximum,count);
                }
                return maximum;
            };
            auto head=maxWidth(210,245),body=maxWidth(300,360);
            log << "held-mouse-head-width=" << head << "\nheld-mouse-body-width=" << body << "\n";
            bool smooth=head>0 && body>0 && head<=body*1.2f;
            log << "held-mouse-start-no-ball=" << smooth << "\n";
            if (!smooth) testResult=1;
            BITMAPFILEHEADER file{}; file.bfType=0x4D42;
            file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+600*400*4;
            std::ofstream capture("build/mouse-start-test.bmp",std::ios::binary);
            capture.write(reinterpret_cast<char*>(&file),sizeof(file));
            capture.write(reinterpret_cast<char*>(&bitmap.bmiHeader),sizeof(BITMAPINFOHEADER));
            capture.write(static_cast<char*>(pixels),600*400*4);
            SelectObject(memory,old); DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr,dc);
            leave();
            std::wstring error;
            bool changed=applyShortcut(MOD_CONTROL|MOD_ALT,'9',error);
            bool saved=GetPrivateProfileIntW(L"Hotkey",L"Key",0,settingsPath.c_str())=='9';
            bool oldFree=RegisterHotKey(window,20,MOD_CONTROL|MOD_NOREPEAT,'2')!=FALSE;
            if (oldFree) UnregisterHotKey(window,20);
            bool occupied=RegisterHotKey(window,21,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'8')!=FALSE;
            bool rejected=occupied && !applyShortcut(MOD_CONTROL|MOD_ALT,'8',error) && hotkeyKey=='9';
            if (occupied) UnregisterHotKey(window,21);
            bool restored=applyShortcut(MOD_CONTROL,'2',error);
            log << "settings-change-save-old-released=" << (changed && saved && oldFree) << "\nsettings-conflict-preserves-old=" << rejected << "\nsettings-restore-default=" << restored << "\n";
            if (!(changed && saved && oldFree && rejected && restored)) testResult=1;
            // Modal settings uses its own message loop; the existing timer verifies controls and cancellation.
            testStep=111;
            settings();
            bool cancelled=hotkeyKey=='2' && settingsWindow==nullptr;
            log << "settings-dialog-cancel-keeps-shortcut=" << cancelled << "\n";
            if (!cancelled) testResult=1;
            log << "result=" << testResult << "\n"; log.flush();
            DestroyWindow(testBackground);
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (testStep == 112 && settingsWindow) {
            bool loaded=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('2',HOTKEYF_CONTROL);
            SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('7',HOTKEYF_ALT),0);
            SendMessageW(settingsWindow,WM_COMMAND,IDC_RESET,0);
            bool reset=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('2',HOTKEYF_CONTROL);
            log << "settings-dialog-load-reset=" << (loaded && reset) << "\n";
            if (!(loaded && reset)) testResult=1;
            SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('7',HOTKEYF_ALT),0);
            PostMessageW(settingsWindow,WM_COMMAND,IDCANCEL,0);
        }
        ++testStep;
    }
public:
    int run(HINSTANCE instance, bool testing) {
        selfTest=testing;
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS}; InitCommonControlsEx(&controls);
        loadSettings();
        if (testing) log.open("build/self-test.log");
        WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=instance;
        wc.lpszClassName=ClassName; wc.hCursor=LoadCursorW(nullptr, IDC_CROSS);
        if (!RegisterClassW(&wc)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        window=CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            ClassName, L"MyZoomIt", WS_POPUP, 0,0,800,600,nullptr,nullptr,instance,this);
        if (!window) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        MARGINS margins{-1,-1,-1,-1}; check_hresult(DwmExtendFrameIntoClientArea(window, &margins));
        DWORD preference=1; DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
        manager=WindowsXamlManager::InitializeForCurrentThread();
        auto transparency=Window::Current().as<IXamlSourceTransparency>();
        check_hresult(transparency->put_IsBackgroundTransparent(true));
        source=DesktopWindowXamlSource(); native=source.as<IDesktopWindowXamlSourceNative2>();
        check_hresult(native->AttachToWindow(window));
        check_hresult(native->get_WindowHandle(&island));
        Grid root;
        root.Background(SolidColorBrush(Color{0,0,0,0}));
        canvas=InkCanvas(); presenter=canvas.InkPresenter();
        presenter.InputDeviceTypes(Windows::UI::Core::CoreInputDeviceTypes::Pen | Windows::UI::Core::CoreInputDeviceTypes::Mouse | Windows::UI::Core::CoreInputDeviceTypes::Touch);
        InkDrawingAttributes attributes;
        attributes.Color(Palette[colorIndex].value); attributes.Size({penWidth,penWidth});
        attributes.IgnorePressure(false); attributes.FitToCurve(true);
        attributes.ModelerAttributes().UseVelocityBasedPressure(false);
        presenter.UpdateDefaultDrawingAttributes(attributes); presenter.IsInputEnabled(false);
        presenter.StrokesCollected([this](auto const&, auto const&) { if (!active) clear(); });
        root.Children().Append(canvas);
        root.AddHandler(UIElement::PointerWheelChangedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (active && (args.KeyModifiers() & Windows::System::VirtualKeyModifiers::Control) != Windows::System::VirtualKeyModifiers::None) {
                    wheel(args.GetCurrentPoint(canvas).Properties().MouseWheelDelta());
                    args.Handled(true);
                }
            })),true);
        Border hint;
        hint.Background(SolidColorBrush(Color{235,30,33,39})); hint.CornerRadius({8,8,8,8});
        hint.Padding({14,8,14,8}); hint.Margin({16,16,16,16});
        hint.HorizontalAlignment(HorizontalAlignment::Left); hint.VerticalAlignment(VerticalAlignment::Top);
        hint.IsHitTestVisible(false);
        hintText=TextBlock();
        hintText.Foreground(SolidColorBrush(Color{255,255,255,255})); hintText.FontSize(14);
        hint.Child(hintText); root.Children().Append(hint); updateWidth(penWidth);
        source.Content(root);
        SetWindowPos(island,nullptr,0,0,800,600,SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated"); tray();
        hotkey=RegisterHotKey(window,hotkeyId,hotkeyModifiers | MOD_NOREPEAT,hotkeyKey) != FALSE;
        if (testing) log << "hotkey-registered=" << hotkey << " error=" << GetLastError() << "\n";
        WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
        if (!hotkey && !testing) MessageBoxW(nullptr,(shortcutText()+L" 已被占用。请通过托盘开始标注，或在设置中修改快捷键。").c_str(),L"MyZoomIt",MB_OK | MB_ICONWARNING);
        if (testing) SetTimer(window,1,100,nullptr);
        MSG msg{};
        int status;
        while ((status=GetMessageW(&msg,nullptr,0,0)) > 0) {
            if (active && msg.message == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL)&0x8000)) {
                wheel(GET_WHEEL_DELTA_WPARAM(msg.wParam)); continue;
            }
            if (active && msg.message == WM_KEYDOWN && (msg.hwnd == window || IsChild(window,msg.hwnd))) {
                if (msg.wParam == VK_ESCAPE) { leave(); continue; }
                if (msg.wParam == 'Z' && (GetKeyState(VK_CONTROL)&0x8000)) { undo(); continue; }
                if (!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && setColor(static_cast<WORD>(msg.wParam))) continue;
                if (msg.wParam == 'C') { clear(); continue; }
            }
            BOOL handled=FALSE; check_hresult(native->PreTranslateMessage(&msg,&handled));
            if (!handled) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        presenter=nullptr; canvas=nullptr; hintText=nullptr; source.Content(nullptr); native=nullptr;
        source.Close(); source=nullptr; manager.Close(); manager=nullptr;
        return status==-1 ? 1 : static_cast<int>(msg.wParam);
    }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto mutex=CreateMutexW(nullptr,FALSE,L"Local\\MyZoomIt.SingleInstance.v1");
    if (!mutex) return 1;
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        auto existing=FindWindowW(ClassName,nullptr);
        if (existing) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(existing,ActivateMessage,0,0); }
        CloseHandle(mutex); return 0;
    }
    int result=1;
    try {
        init_apartment(apartment_type::single_threaded);
        App app; result=app.run(instance,std::wstring(command)==L"--self-test");
    } catch (hresult_error const& e) {
        wchar_t code[32]{}; swprintf_s(code,L"\nHRESULT: 0x%08X",static_cast<unsigned>(e.code().value));
        auto text=std::wstring(L"初始化 Windows Ink 失败：\n") + e.message().c_str() + code;
        MessageBoxW(nullptr,text.c_str(),L"MyZoomIt",MB_OK | MB_ICONERROR);
    }
    CloseHandle(mutex); return result;
}



