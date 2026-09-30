#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include "settings.h"
#include "version.h"
#include <dwmapi.h>
#include <wtsapi32.h>
#include <inspectable.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Input.h>
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
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.System.h>
#include <fstream>
#include <string>
#include <algorithm>
#include <array>
#include <vector>
#include <atomic>

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
constexpr UINT SettingsMessage = WM_APP + 3, SettingsReadyMessage = WM_APP + 4;
HICON appIcon(bool smallIcon) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_APP),IMAGE_ICON,
        GetSystemMetrics(smallIcon ? SM_CXSMICON : SM_CXICON),
        GetSystemMetrics(smallIcon ? SM_CYSMICON : SM_CYICON),LR_SHARED));
}
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
    Windows::UI::Input::Inking::Core::CoreInkIndependentInputSource inkInput{nullptr};
    bool cursorOnlyTest{};
    std::atomic<bool> inkCursorConfigured{};
    enum class Tool { Pen, Rectangle, Ellipse, Line, Arrow };
    Tool tool{Tool::Pen};
    Tool requestedTool{Tool::Pen};
    bool heldTool{};
    Canvas shapeSurface{nullptr};
    Windows::UI::Xaml::Shapes::Polyline preview{nullptr};
    bool drawingShape{};
    uint32_t shapePointer{};
    Windows::Foundation::Point shapeStart{}, shapeEnd{};
    InkDrawingAttributes shapeAttributes{nullptr};

    const wchar_t* toolName() const {
        switch (tool) {
        case Tool::Rectangle: return L"矩形";
        case Tool::Ellipse: return L"椭圆";
        case Tool::Line: return L"直线";
        case Tool::Arrow: return L"箭头";
        default: return L"手写";
        }
    }
    void cancelShape() {
        drawingShape=false;
        if (preview) preview.Points().Clear();
        if (shapeSurface) shapeSurface.ReleasePointerCaptures();
    }
    void setTool(Tool next) {
        cancelShape(); tool=requestedTool=next;
        shapeSurface.Visibility(tool==Tool::Pen ? Visibility::Collapsed : Visibility::Visible);
        presenter.IsInputEnabled(active && tool==Tool::Pen);
        updateHint();
    }
    void syncHeldTool() {
        bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        bool tab=(GetKeyState(VK_TAB)&0x8000)!=0;
        bool holding=ctrl || shift || tab;
        if (!holding && !heldTool) return;
        heldTool=holding;
        Tool next=ctrl && shift ? Tool::Arrow : tab ? Tool::Ellipse : ctrl ? Tool::Rectangle : shift ? Tool::Line : Tool::Pen;
        requestedTool=next;
        // Keep the current drag's geometry and capture until the pointer is released.
        if (!drawingShape && tool!=next) setTool(next);
    }
    std::vector<Windows::Foundation::Point> shapePoints() const {
        using Windows::Foundation::Point;
        auto a=shapeStart, b=shapeEnd;
        std::vector<Point> points;
        if (tool==Tool::Rectangle) return {a,{b.X,a.Y},b,{a.X,b.Y},a};
        if (tool==Tool::Ellipse) {
            float cx=(a.X+b.X)/2,cy=(a.Y+b.Y)/2;
            float rx=std::abs(b.X-a.X)/2,ry=std::abs(b.Y-a.Y)/2;
            int count=std::clamp(static_cast<int>(std::max(rx,ry)*0.8f),48,360);
            for (int i=0;i<count;++i) {
                double angle=i*6.283185307179586/count;
                points.push_back({cx+rx*static_cast<float>(std::cos(angle)),cy+ry*static_cast<float>(std::sin(angle))});
            }
            points.push_back(points.front()); return points;
        }
        if (tool==Tool::Arrow) std::swap(a,b); // Pointer-down is the arrow tip; pointer-up is the tail.
        points={a,b};
        if (tool==Tool::Arrow) {
            float dx=b.X-a.X,dy=b.Y-a.Y,length=std::hypot(dx,dy);
            if (length>0) {
                float head=std::min(length*0.45f,std::max(18.0f,shapeAttributes.Size().Width*3));
                float ux=dx/length,uy=dy/length;
                points.push_back({b.X-ux*head-uy*head*0.5f,b.Y-uy*head+ux*head*0.5f});
                points.push_back(b);
                points.push_back({b.X-ux*head+uy*head*0.5f,b.Y-uy*head-ux*head*0.5f});
            }
        }
        return points;
    }
    void updateShape(Windows::Foundation::Point position) {
        shapeEnd=position;
        auto points=preview.Points(); points.Clear();
        for (auto const& point:shapePoints()) points.Append(point);
    }
    void commitShape() {
        auto points=shapePoints();
        auto dx=std::abs(shapeEnd.X-shapeStart.X),dy=std::abs(shapeEnd.Y-shapeStart.Y);
        bool valid=(tool==Tool::Rectangle || tool==Tool::Ellipse) ? dx>=2 && dy>=2 : std::hypot(dx,dy)>=2;
        if (valid) {
            InkStrokeBuilder builder; builder.SetDefaultDrawingAttributes(shapeAttributes);
            presenter.StrokeContainer().AddStroke(builder.CreateStroke(points));
        }
        cancelShape();
        if (tool!=requestedTool) setTool(requestedTool);
    }
    void initializeShapes(Grid const& root) {
        shapeSurface=Canvas(); shapeSurface.Background(SolidColorBrush(Color{0,0,0,0}));
        shapeSurface.Visibility(tool==Tool::Pen ? Visibility::Collapsed : Visibility::Visible);
        preview=Windows::UI::Xaml::Shapes::Polyline();
        preview.IsHitTestVisible(false); preview.StrokeLineJoin(PenLineJoin::Round);
        preview.StrokeStartLineCap(PenLineCap::Round); preview.StrokeEndLineCap(PenLineCap::Round);
        shapeSurface.Children().Append(preview); root.Children().Append(shapeSurface);
        shapeSurface.PointerPressed([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            auto point=args.GetCurrentPoint(shapeSurface);
            if (!active || tool==Tool::Pen || drawingShape || !point.IsInContact()) return;
            if (point.PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Mouse && !point.Properties().IsLeftButtonPressed()) return;
            if (!shapeSurface.CapturePointer(args.Pointer())) return;
            drawingShape=true; shapePointer=args.Pointer().PointerId(); shapeStart=point.Position();
            shapeAttributes=presenter.CopyDefaultDrawingAttributes();
            shapeAttributes.IgnorePressure(true); shapeAttributes.FitToCurve(false);
            shapeAttributes.ModelerAttributes().UseVelocityBasedPressure(false);
            preview.Stroke(SolidColorBrush(shapeAttributes.Color())); preview.StrokeThickness(shapeAttributes.Size().Width);
            updateShape(shapeStart); args.Handled(true);
        });
        shapeSurface.PointerMoved([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            if (!drawingShape || args.Pointer().PointerId()!=shapePointer) return;
            updateShape(args.GetCurrentPoint(shapeSurface).Position()); args.Handled(true);
        });
        shapeSurface.PointerReleased([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            if (!drawingShape || args.Pointer().PointerId()!=shapePointer) return;
            updateShape(args.GetCurrentPoint(shapeSurface).Position()); commitShape(); args.Handled(true);
        });
        shapeSurface.PointerCanceled([this](auto const&, auto const& args) {
            if (drawingShape && args.Pointer().PointerId()==shapePointer) {
                cancelShape(); if (tool!=requestedTool) setTool(requestedTool);
            }
        });
        shapeSurface.PointerCaptureLost([this](auto const&, auto const& args) {
            if (drawingShape && args.Pointer().PointerId()==shapePointer) {
                cancelShape(); if (tool!=requestedTool) setTool(requestedTool);
            }
        });
    }




    TextBlock hintText{nullptr};
    Border hintPanel{nullptr};
    bool showHint{}, runAtStartup{};
    float penWidth{12};
    size_t colorIndex{};
    int wheelRemainder{};
    Canvas cursorSurface{nullptr};
    Windows::UI::Xaml::Shapes::Ellipse cursorDot{nullptr};
    ULONGLONG cursorStarted{};
    bool cursorAnimating{};
    unsigned cursorHideCalls{};
    void hideSystemCursor() {
        // Read the thread's display count without changing it, then retain only our hides.
        ShowCursor(TRUE);
        int count=ShowCursor(FALSE);
        while (count>=0) { count=ShowCursor(FALSE); ++cursorHideCalls; }
        SetCursor(nullptr);
    }
    void restoreSystemCursor() {
        while (cursorHideCalls) { ShowCursor(TRUE); --cursorHideCalls; }
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    }
    float cursorDiameter{};
    static constexpr UINT_PTR CursorTimer=2;
    void positionCursor() {
        if (!active || !cursorDot) return;
        POINT point{}; GetCursorPos(&point); ScreenToClient(island,&point);
        float scale=static_cast<float>(GetDpiForWindow(island))/96.0f;
        Canvas::SetLeft(cursorDot,point.x/scale-static_cast<float>(cursorDot.Width())/2);
        Canvas::SetTop(cursorDot,point.y/scale-static_cast<float>(cursorDot.Height())/2);
    }
    void refreshCursor(float diameter, float opacity=1.0f) {
        if (!cursorDot) return;
        auto color=Palette[colorIndex].value;
        BYTE contrast=(color.R*299+color.G*587+color.B*114)>150000 ? 25 : 245;
        cursorDot.Fill(SolidColorBrush(color));
        cursorDot.Stroke(SolidColorBrush(Color{255,contrast,contrast,contrast}));
        cursorDot.StrokeThickness(1);
        cursorDot.Width(diameter+2); cursorDot.Height(diameter+2);
        cursorDot.Opacity(opacity); cursorDiameter=diameter;
        cursorSurface.Visibility(active ? Visibility::Visible : Visibility::Collapsed);
        positionCursor();
    }
    void finishCursorAnimation() {
        cursorAnimating=false;
        if (active) refreshCursor(penWidth);
    }
    void animateCursor() {
        if (!active) return;
        hideSystemCursor();
        if (cursorAnimating) {
            float progress=std::min(1.0f,static_cast<float>(GetTickCount64()-cursorStarted)/180.0f);
            float remaining=1-progress;
            refreshCursor(penWidth+(200-penWidth)*remaining*remaining*remaining,0.25f+0.75f*progress);
            if (progress>=1) cursorAnimating=false;
        } else positionCursor();
    }
    unsigned testStep{};
    int testResult{1};
    std::ofstream log;
    HWND settingsWindow{};
    inline static HWND hotkeyEdit{};
    static LRESULT CALLBACK hotkeyMessageProcedure(int code, WPARAM w, LPARAM l) {
        if (code>=0 && w==PM_REMOVE && hotkeyEdit && GetFocus()==hotkeyEdit) {
            auto msg=reinterpret_cast<MSG*>(l);
            // DialogBox handles Alt as menu navigation before dispatch. Give the editor normal key messages.
            if (msg->hwnd==hotkeyEdit) {
                if (msg->message==WM_SYSKEYDOWN) msg->message=WM_KEYDOWN;
                else if (msg->message==WM_SYSKEYUP) msg->message=WM_KEYUP;
                else if (msg->message==WM_SYSCHAR) msg->message=WM_CHAR;
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    UINT hotkeyModifiers{MOD_CONTROL}, hotkeyKey{'2'};
    int hotkeyId{1};
    std::wstring settingsPath;
    const wchar_t* startupKey() const {
        return selfTest ? L"Software\\MyZoomIt\\SelfTestRun" : L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    }
    static std::wstring executablePath() {
        std::wstring path(32768,L'\0');
        auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if (!length || length>=path.size()) throw hresult_error(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
        path.resize(length); return path;
    }
    bool startupEnabled() const {
        wchar_t command[32768]{}; DWORD bytes=sizeof(command);
        if (RegGetValueW(HKEY_CURRENT_USER,startupKey(),L"MyZoomIt",RRF_RT_REG_SZ,nullptr,command,&bytes)!=ERROR_SUCCESS) return false;
        auto expected=L"\""+executablePath()+L"\"";
        return _wcsicmp(command,expected.c_str())==0;
    }
    bool setStartup(bool enabled, std::wstring& error) {
        if (!enabled && !startupEnabled()) return true;
        HKEY keyHandle{};
        auto status=enabled ? RegCreateKeyExW(HKEY_CURRENT_USER,startupKey(),0,nullptr,0,KEY_SET_VALUE,nullptr,&keyHandle,nullptr) :
            RegOpenKeyExW(HKEY_CURRENT_USER,startupKey(),0,KEY_SET_VALUE,&keyHandle);
        if (!enabled && status==ERROR_FILE_NOT_FOUND) return true;
        if (status!=ERROR_SUCCESS) { error=L"无法更新开机自动运行设置。"; return false; }
        if (enabled) {
            wchar_t executable[32768]{};
            auto length=GetModuleFileNameW(nullptr,executable,32768);
            if (!length || length>=32768) { RegCloseKey(keyHandle); error=L"无法获取程序路径。"; return false; }
            auto command=std::wstring(L"\"")+executable+L"\"";
            status=RegSetValueExW(keyHandle,L"MyZoomIt",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
        } else {
            status=RegDeleteValueW(keyHandle,L"MyZoomIt");
            if (status==ERROR_FILE_NOT_FOUND) status=ERROR_SUCCESS;
        }
        RegCloseKey(keyHandle);
        if (status!=ERROR_SUCCESS) { error=L"无法更新开机自动运行设置。"; return false; }
        return true;
    }
    bool applySettings(UINT mods, UINT code, bool hint, bool startup, std::wstring& error) {
        if (!validShortcut(mods,code)) { error=L"请选择包含 Ctrl 或 Alt 的组合键；F12 为系统保留键。"; return false; }
        auto parent=settingsPath.substr(0,settingsPath.find_last_of(L"\\"));
        CreateDirectoryW(parent.c_str(),nullptr);
        auto oldStartup=startupEnabled();
        if (!setStartup(startup,error)) return false;
        if (settingsPath.empty() || !WritePrivateProfileStringW(L"UI",L"ShowHint",hint ? L"1" : L"0",settingsPath.c_str())) {
            std::wstring rollback; setStartup(oldStartup,rollback);
            error=L"无法保存悬浮状态菜单设置。"; return false;
        }
        if (!applyShortcut(mods,code,error)) {
            WritePrivateProfileStringW(L"UI",L"ShowHint",showHint ? L"1" : L"0",settingsPath.c_str());
            std::wstring rollback; setStartup(oldStartup,rollback); return false;
        }
        showHint=hint; runAtStartup=startup; updateHint(); saveBrushSettings(); return true;
    }
    std::wstring shortcutText() const {
        if (!hotkeyKey) return L"未设置";
        std::wstring result;
        if (hotkeyModifiers & MOD_CONTROL) result+=L"Ctrl+";
        if (hotkeyModifiers & MOD_ALT) result+=L"Alt+";
        if (hotkeyModifiers & MOD_SHIFT) result+=L"Shift+";
        wchar_t name[64]{};
        GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(hotkeyKey,MAPVK_VK_TO_VSC)<<16),name,64);
        return result+name;
    }
    static bool validShortcut(UINT mods, UINT code) {
        if (!code) return mods==0;
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
        auto executable=executablePath();
        settingsPath=executable.substr(0,executable.find_last_of(L"\\"))+L"\\settings.ini";
        auto mods=GetPrivateProfileIntW(L"Hotkey",L"Modifiers",MOD_CONTROL,settingsPath.c_str());
        auto code=GetPrivateProfileIntW(L"Hotkey",L"Key",'2',settingsPath.c_str());
        if (validShortcut(mods,code)) { hotkeyModifiers=mods; hotkeyKey=code; }
        showHint=GetPrivateProfileIntW(L"UI",L"ShowHint",0,settingsPath.c_str())!=0;
        runAtStartup=startupEnabled();
        auto savedWidth=GetPrivateProfileIntW(L"Pen",L"Width",12,settingsPath.c_str());
        penWidth=static_cast<float>(std::clamp(savedWidth,6u,24u));
        auto savedColor=GetPrivateProfileIntW(L"Pen",L"Color",0,settingsPath.c_str());
        if (savedColor<Palette.size()) colorIndex=savedColor;
        auto savedTool=GetPrivateProfileIntW(L"Pen",L"Tool",0,settingsPath.c_str());
        if (savedTool<=4) tool=requestedTool=static_cast<Tool>(savedTool);
    }
    bool saveBrushSettings() {
        if (selfTest) return true;
        auto section=L"Width="+std::to_wstring(static_cast<int>(penWidth))+L'\0'+
            L"Color="+std::to_wstring(colorIndex)+L'\0'+L"Tool="+std::to_wstring(static_cast<int>(tool))+L'\0';
        return WritePrivateProfileSectionW(L"Pen",section.c_str(),settingsPath.c_str())!=FALSE;
    }
    bool applyShortcut(UINT mods, UINT code, std::wstring& error) {
        if (!validShortcut(mods,code)) { error=L"请选择包含 Ctrl 或 Alt 的组合键；F12 为系统保留键。"; return false; }
        if (mods==hotkeyModifiers && code==hotkeyKey && hotkey) return true;
        int next=hotkeyId==1 ? 2 : 1;
        if (code && !RegisterHotKey(window,next,mods|MOD_NOREPEAT,code)) {
            error=L"该快捷键已被占用或不可用，请选择其他组合键。"; return false;
        }
        auto parent=settingsPath.substr(0,settingsPath.find_last_of(L"\\"));
        CreateDirectoryW(parent.c_str(),nullptr);
        auto section=L"Modifiers="+std::to_wstring(mods)+L'\0'+L"Key="+std::to_wstring(code)+L'\0';
        if (settingsPath.empty() || !WritePrivateProfileSectionW(L"Hotkey",section.c_str(),settingsPath.c_str())) {
            if (code) UnregisterHotKey(window,next);
            error=L"无法保存设置，原快捷键仍然有效。"; return false;
        }
        if (hotkey) UnregisterHotKey(window,hotkeyId);
        hotkeyId=next; hotkey=code!=0; hotkeyModifiers=mods; hotkeyKey=code;
        NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; icon.uFlags=NIF_TIP;
        auto tip=L"MyZoomIt · "+shortcutText()+L" 标注";
        wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE); Shell_NotifyIconW(NIM_MODIFY,&icon);
        return true;
    }
    static LRESULT CALLBACK hotkeyProcedure(HWND control, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
        if (msg==WM_NCDESTROY) { RemoveWindowSubclass(control,hotkeyProcedure,1); return DefSubclassProc(control,msg,w,l); }
        if (msg==WM_GETDLGCODE) {
            auto input=reinterpret_cast<MSG*>(l);
            if (input && input->wParam==VK_TAB && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000))
                return DefSubclassProc(control,msg,w,l);
            return DefSubclassProc(control,msg,w,l)|DLGC_WANTALLKEYS;
        }
        if (msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN) {
            BYTE flags=0;
            if (GetKeyState(VK_CONTROL)&0x8000) flags|=HOTKEYF_CONTROL;
            if (GetKeyState(VK_MENU)&0x8000) flags|=HOTKEYF_ALT;
            if (GetKeyState(VK_SHIFT)&0x8000) flags|=HOTKEYF_SHIFT;
            if (w==VK_ESCAPE && !flags) { SendMessageW(GetParent(control),WM_COMMAND,IDCANCEL,0); return 0; }
            bool modifier=w==VK_CONTROL || w==VK_LCONTROL || w==VK_RCONTROL || w==VK_SHIFT ||
                w==VK_LSHIFT || w==VK_RSHIFT || w==VK_MENU || w==VK_LMENU || w==VK_RMENU;
            // Modifier-only events (including synthetic Ctrl around Alt release) must not erase a captured key.
            if (modifier) return 0;
            if (l & (1LL<<24)) flags|=HOTKEYF_EXT;
            SendMessageW(control,HKM_SETHOTKEY,MAKEWORD(static_cast<BYTE>(w),flags),0);
            return 0;
        }
        if (msg==WM_KEYUP || msg==WM_SYSKEYUP || msg==WM_CHAR || msg==WM_SYSCHAR) return 0;
        return DefSubclassProc(control,msg,w,l);
    }
    static void fitSettings(HWND dialog) {
        auto label=GetDlgItem(dialog,IDC_INSTRUCTIONS);
        RECT area{}; GetClientRect(label,&area);
        int length=GetWindowTextLengthW(label);
        std::wstring text(static_cast<size_t>(length)+1,L'\0'); GetWindowTextW(label,text.data(),length+1);
        auto dc=GetDC(label); auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(label,WM_GETFONT,0,0)));
        RECT measured{0,0,area.right,0};
        DrawTextW(dc,text.c_str(),length,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old); ReleaseDC(label,dc);
        int delta=measured.bottom-area.bottom;
        SetWindowPos(label,nullptr,0,0,area.right,measured.bottom,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        for (int id:{IDC_INSTRUCTION_GROUP,IDC_SHOW_HINT,IDC_STARTUP,IDOK,IDCANCEL}) {
            auto control=GetDlgItem(dialog,id); RECT r{}; GetWindowRect(control,&r);
            MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&r),2);
            if (id==IDC_INSTRUCTION_GROUP)
                SetWindowPos(control,nullptr,0,0,r.right-r.left,r.bottom-r.top+delta,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            else SetWindowPos(control,nullptr,r.left,r.top+delta,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        RECT bounds{}; GetWindowRect(dialog,&bounds);
        SetWindowPos(dialog,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top+delta,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    static INT_PTR CALLBACK settingsProcedure(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
        auto app=reinterpret_cast<App*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) {
            SendMessageW(dialog,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(appIcon(false)));
            SendMessageW(dialog,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(appIcon(true)));
            app=reinterpret_cast<App*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); app->settingsWindow=dialog;
            hotkeyEdit=GetDlgItem(dialog,IDC_HOTKEY);
            SetDlgItemTextW(dialog,IDC_INSTRUCTIONS,
                L"- 按下设定的快捷键进入标注状态，再次按下或按Esc退出；\r\n\r\n"
                L"- 用鼠标左键、触控笔或触摸屏幕绘制\r\n"
                L"- 触控笔模式支持压感\r\n\r\n"
                L"- 标注状态下按键1~5切换标注图形模式：\r\n"
                L"  1 手写· 2 矩形· 3 椭圆·4 直线· 5 箭头\r\n\r\n"
                L"- 亦可用快捷键快速绘制图形：\r\n"
                L"  Ctrl 矩形；Tab 椭圆；Shift 直线；Ctrl+Shift 箭头\r\n\r\n"
                L"- 按键字母键切换笔迹颜色：\r\n"
                L"  R 红色；G 绿色；O 橙色；P 粉紫色\r\n"
                L"  B 蓝色；W 白色；Y 黄色\r\n\r\n"
                L"- Ctrl +滚轮：调整笔迹粗细（6–24，默认12)\r\n\r\n"
                L"- Ctrl + Z：撤销上一笔或图形\r\n"
                L"- C：清空全部笔迹\r\n"
                L"- 退出标注状态时会清空笔迹，如需要请按PrtScr键截屏保存");
            fitSettings(dialog);
            if (app->selfTest) SetWindowPos(dialog,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            app->runAtStartup=app->startupEnabled();
            CheckDlgButton(dialog,IDC_SHOW_HINT,app->showHint ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog,IDC_STARTUP,app->runAtStartup ? BST_CHECKED : BST_UNCHECKED);
            BYTE flags=0;
            if (app->hotkeyModifiers & MOD_CONTROL) flags|=HOTKEYF_CONTROL;
            if (app->hotkeyModifiers & MOD_ALT) flags|=HOTKEYF_ALT;
            if (app->hotkeyModifiers & MOD_SHIFT) flags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD(app->hotkeyKey,flags),0);
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETRULES,0,0);
            SetWindowSubclass(GetDlgItem(dialog,IDC_HOTKEY),hotkeyProcedure,1,0);
            PostMessageW(dialog,SettingsReadyMessage,0,0);
            return TRUE;
        }
        if (!app) return FALSE;
        if (msg==SettingsReadyMessage) {
            ShowWindow(dialog,SW_SHOWNORMAL);
            SetForegroundWindow(dialog);
            SetFocus(GetDlgItem(dialog,IDC_HOTKEY));
            return TRUE;
        }
        if (msg==WM_COMMAND) {
            if (LOWORD(w)==IDC_DELETE_HOTKEY) {
                SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,0,0);
                SetFocus(GetDlgItem(dialog,IDC_HOTKEY)); return TRUE;
            }
            if (LOWORD(w)==IDC_RESET) {
                SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('2',HOTKEYF_CONTROL),0);
                return TRUE;
            }
            if (LOWORD(w)==IDOK) {
                auto value=SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_GETHOTKEY,0,0);
                UINT mods=0; auto flags=HIBYTE(value);
                if (flags & HOTKEYF_CONTROL) mods|=MOD_CONTROL;
                if (flags & HOTKEYF_ALT) mods|=MOD_ALT;
                if (flags & HOTKEYF_SHIFT) mods|=MOD_SHIFT;
                std::wstring error;
                if (!app->applySettings(mods,LOBYTE(value),IsDlgButtonChecked(dialog,IDC_SHOW_HINT)==BST_CHECKED,
                    IsDlgButtonChecked(dialog,IDC_STARTUP)==BST_CHECKED,error)) {
                    MessageBoxW(dialog,error.c_str(),L"MyZoomIt 设置",MB_OK|MB_ICONWARNING); return TRUE;
                }
                EndDialog(dialog,IDOK); return TRUE;
            }
            if (LOWORD(w)==IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
        }
        if (msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        if (msg==WM_DESTROY) { app->settingsWindow=nullptr; hotkeyEdit=nullptr; }
        return FALSE;
    }
    void settings() {
        if (settingsWindow && IsWindow(settingsWindow)) {
            ShowWindow(settingsWindow,SW_RESTORE); SetForegroundWindow(settingsWindow); return;
        }
        leave();
        // Suspend the binding so the hotkey editor receives the currently registered combination.
        if (hotkey) { UnregisterHotKey(window,hotkeyId); hotkey=false; }
        auto inputHook=SetWindowsHookExW(WH_GETMESSAGE,hotkeyMessageProcedure,nullptr,GetCurrentThreadId());
        if (selfTest) log << "editor-input-filter-installed=" << (inputHook!=nullptr) << "\n";
        auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),nullptr,settingsProcedure,reinterpret_cast<LPARAM>(this));
        if (inputHook) UnhookWindowsHookEx(inputHook);
        hotkeyEdit=nullptr;
        settingsWindow=nullptr;
        if (!IsWindow(window)) return;
        if (!hotkey && hotkeyKey) {
            hotkey=RegisterHotKey(window,hotkeyId,hotkeyModifiers|MOD_NOREPEAT,hotkeyKey)!=FALSE;
            if (!hotkey) MessageBoxW(nullptr,L"快捷键现已被其他程序占用，请在设置中重新选择。",L"MyZoomIt",MB_OK|MB_ICONWARNING);
        }
        if (result==-1) MessageBoxW(nullptr,L"无法打开设置窗口。",L"MyZoomIt",MB_OK|MB_ICONERROR);
    }

    void updateHint() {
        hintText.Text(std::wstring(L"标注中  ·  ")+toolName()+L"  ·  "+Palette[colorIndex].name+L"  ·  Ctrl+滚轮 粗细 "+
            std::to_wstring(static_cast<int>(penWidth))+L"  ·  Ctrl+Z 撤销  ·  C 清空  ·  Esc 返回\n按住 Ctrl 矩形 / Tab 椭圆 / Shift 直线 / Ctrl+Shift 箭头");
        if (hintPanel) hintPanel.Visibility(showHint ? Visibility::Visible : Visibility::Collapsed);
    }
    bool setColor(WORD keyCode) {
        for (size_t i=0;i<Palette.size();++i) {
            if (Palette[i].key!=keyCode) continue;
            colorIndex=i;
            auto attributes=presenter.CopyDefaultDrawingAttributes();
            attributes.Color(Palette[i].value);
            presenter.UpdateDefaultDrawingAttributes(attributes);
            updateHint(); if (active) finishCursorAnimation(); return true;
        }
        return false;
    }

    void updateWidth(float width) {
        penWidth=std::clamp(width,6.0f,24.0f);
        auto attributes=presenter.CopyDefaultDrawingAttributes();
        attributes.Size({penWidth,penWidth});
        attributes.ModelerAttributes().UseVelocityBasedPressure(false);
        presenter.UpdateDefaultDrawingAttributes(attributes);
        updateHint(); if (active) finishCursorAnimation();
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
        icon.hIcon = appIcon(true);
        auto tip=L"MyZoomIt · "+shortcutText()+L" 标注";
        wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &icon);
    }
    void clear() {
        cancelShape();
        if (shapeSurface && tool!=requestedTool) setTool(requestedTool);
        if (presenter) presenter.StrokeContainer().Clear();
    }
    void undo() {
        if (drawingShape) { cancelShape(); if (tool!=requestedTool) setTool(requestedTool); return; }
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
        cursorAnimating=false; KillTimer(window,CursorTimer);
        if (cursorSurface) cursorSurface.Visibility(Visibility::Collapsed);
        restoreSystemCursor();
        presenter.IsInputEnabled(false);
        ShowWindow(window, SW_HIDE);
        active = false;
        clear();
        if (heldTool) { heldTool=false; setTool(Tool::Pen); }
        saveBrushSettings();
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
            presenter.IsInputEnabled(tool==Tool::Pen);
            active = true;
            syncHeldTool();
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            SetFocus(island);
            hideSystemCursor();
            cursorStarted=GetTickCount64(); cursorAnimating=true;
            animateCursor(); SetTimer(window,CursorTimer,16,nullptr);
            changing = false;
        } catch (...) {
            changing = false;
            ShowWindow(window,SW_HIDE); active=false; restoreSystemCursor();
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
        PostMessageW(window, WM_NULL, 0, 0);
        if (command == 1) enter();
        else if (command == 2) clear();
        else if (command == 3) PostMessageW(window, WM_CLOSE, 0, 0);
        else if (command == 4) PostMessageW(window,SettingsMessage,0,0);
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
        case SettingsMessage: settings(); return 0;
        case WM_HOTKEY: if (hotkey && !settingsWindow && static_cast<int>(w)==hotkeyId) enter(); return 0;
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
        case WM_SETCURSOR: if (active) { SetCursor(nullptr); return TRUE; } break;
        case WM_TIMER: if (w==CursorTimer) animateCursor(); else if (selfTest && w==1) test(); return 0;
        case WM_CLOSE: saveBrushSettings(); if (settingsWindow) EndDialog(settingsWindow,IDCANCEL); leave(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, 1); KillTimer(window,CursorTimer);
            restoreSystemCursor();
            UnregisterHotKey(window, hotkeyId);
            WTSUnRegisterSessionNotification(window);
            { NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; Shell_NotifyIconW(NIM_DELETE, &icon); }
            PostQuitMessage(selfTest ? testResult : 0); return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }
    void testCursor() {
        if (testStep==0) {
            testResult=0;
            enter(); finishCursorAnimation(); setTool(Tool::Pen);
            RECT bounds{}; GetWindowRect(window,&bounds);
            testOrigin={bounds.left+240,bounds.top+240};
        }
        if (testStep==1 || testStep==3 || testStep==5 || testStep==7 || testStep==9 || testStep==11 || testStep==13) {
            mouseMove(testOrigin.x+static_cast<int>(testStep)*3,testOrigin.y);
        }
        if (testStep==2 || testStep==4 || testStep==6 || testStep==8 || testStep==10 || testStep==12 || testStep==14) {
            CURSORINFO state{sizeof(state)};
            bool hidden=GetCursorInfo(&state) && !(state.flags&CURSOR_SHOWING);
            bool inkHidden=inkCursorConfigured.load();
            bool dotVisible=cursorSurface.Visibility()==Visibility::Visible;
            log << "cursor-mode=" << static_cast<int>(tool) << " system-hidden=" << hidden << " ink-hidden=" << inkHidden << " dot-visible=" << dotVisible << "\n";
            if (!(hidden && inkHidden && dotVisible)) testResult=1;
            if (testStep==2) key(VK_CONTROL);
            if (testStep==4) key(VK_CONTROL,true);
            if (testStep==6) key(VK_SHIFT);
            if (testStep==8) key(VK_SHIFT,true);
            if (testStep==10) key(VK_TAB);
            if (testStep==12) key(VK_TAB,true);
        }
        if (testStep==15) leave();
        if (testStep==17) {
            CURSORINFO state{sizeof(state)};
            bool restored=GetCursorInfo(&state) && (state.flags&CURSOR_SHOWING) && cursorHideCalls==0 && cursorSurface.Visibility()==Visibility::Collapsed;
            log << "cursor-exit-restored=" << restored << "\n";
            if (!restored) testResult=1;
            log << "result=" << testResult << "\n"; log.flush();
            PostMessageW(window,WM_CLOSE,0,0);
        }
        ++testStep;
    }
    void test() {
        if (cursorOnlyTest) { testCursor(); return; }
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
            bool cursorVisible=cursorSurface.Visibility()==Visibility::Visible && cursorDot.Opacity()==1;
            log << "brush-cursor-visible-after-wheel=" << cursorVisible << "\n";
            if (!cursorVisible) testResult=1;
            bool resized=penWidth==14 && presenter.CopyDefaultDrawingAttributes().Size().Width==14 && cursorDiameter==14 && !cursorAnimating;
            log << "ctrl-wheel-increases-width=" << resized << "\n";
            if (!resized) testResult=1;
            mouseWheel(-30*WHEEL_DELTA);
        } else if (testStep == 26) {
            CURSORINFO cursorState{sizeof(cursorState)};
            bool systemHidden=GetCursorInfo(&cursorState) && !(cursorState.flags&CURSOR_SHOWING);
            log << "system-cursor-hidden-after-wheel=" << systemHidden << "\n";
            if (!systemHidden) testResult=1;
            log << "width-minimum=" << penWidth << "\n";
            if (penWidth!=6) testResult=1;
            mouseWheel(40*WHEEL_DELTA);
        } else if (testStep == 28) {
            log << "width-maximum=" << penWidth << "\n";
            if (penWidth!=24) testResult=1;
            key(VK_CONTROL,true); updateWidth(12); clear();
            auto initialized=InitializeTouchInjection(1,TOUCH_FEEDBACK_NONE);
            log << "touch-injection-initialized=" << initialized << " error=" << GetLastError() << "\n";
            if (!initialized) testResult=1;
        } else if (testStep == 29) {
            // Let the injected Ctrl key-up reach the tool switch before touch begins.
            touch(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,0);
        } else if (testStep == 30) {
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
            for (int attempt=0;attempt<3;++attempt) {
                testStep=111;
                SendMessageW(window,SettingsMessage,0,0);
                bool cancelled=hotkeyKey=='2' && hotkey && settingsWindow==nullptr && !showHint && !startupEnabled();
                log << "settings-dialog-cancel-restores-shortcut-" << attempt << "=" << cancelled << "\n";
                if (!cancelled) testResult=1;
            }
            testStep=113;
            clear(); SetForegroundWindow(testBackground); enter(); updateWidth(6);
        } else if (testStep == 112 && settingsWindow) {
            bool foreground=IsWindowVisible(settingsWindow) && GetForegroundWindow()==settingsWindow &&
                GetFocus()==GetDlgItem(settingsWindow,IDC_HOTKEY) && !hotkey;
            log << "settings-opens-visible-focused-binding-suspended=" << foreground << "\n";
            if (!foreground) { testResult=1; testStep=117; PostMessageW(settingsWindow,WM_COMMAND,IDCANCEL,0); return; }
            // Capture only this application's settings window for visual layout verification.
            RECT bounds{}; GetWindowRect(settingsWindow,&bounds);
            int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
            auto dc=GetDC(settingsWindow),memory=CreateCompatibleDC(dc);
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=width; bitmap.bmiHeader.biHeight=-height;
            bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32;
            void* pixels{}; auto image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
            auto old=SelectObject(memory,image);
            RedrawWindow(settingsWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            PrintWindow(settingsWindow,memory,2);
            BITMAPFILEHEADER file{}; file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
            file.bfSize=file.bfOffBits+width*height*4;
            std::ofstream capture("build/settings-test.bmp",std::ios::binary);
            capture.write(reinterpret_cast<char*>(&file),sizeof(file));
            capture.write(reinterpret_cast<char*>(&bitmap.bmiHeader),sizeof(BITMAPINFOHEADER));
            capture.write(static_cast<char*>(pixels),static_cast<std::streamsize>(width)*height*4);
            SelectObject(memory,old); DeleteObject(image); DeleteDC(memory); ReleaseDC(settingsWindow,dc);
            bool loaded=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('2',HOTKEYF_CONTROL);
            SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD('7',HOTKEYF_ALT),0);
            SendMessageW(settingsWindow,WM_COMMAND,IDC_RESET,0);
            bool reset=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('2',HOTKEYF_CONTROL);
            log << "settings-dialog-load-reset=" << (loaded && reset) << "\n";
            if (!(loaded && reset)) testResult=1;
            SetFocus(GetDlgItem(settingsWindow,IDC_HOTKEY)); key('F'); key('F',true);
        } else if (testStep==113 && settingsWindow) {
            bool raw=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('F',0);
            log << "hotkey-single-key-no-added-ctrl=" << raw << "\n"; if (!raw) testResult=1;
            key(VK_CONTROL); key('2'); key('2',true); key(VK_CONTROL,true);
        } else if (testStep==114 && settingsWindow) {
            bool ctrl=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD('2',HOTKEYF_CONTROL) && !active;
            log << "hotkey-captures-existing-ctrl-combination=" << ctrl << "\n"; if (!ctrl) testResult=1;
            key(VK_MENU); key('K'); key('K',true); key(VK_MENU,true);
        } else if (testStep==115 && settingsWindow) {
            auto captured=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0);
            bool alt=captured==MAKEWORD('K',HOTKEYF_ALT);
            log << "hotkey-captures-alt-combination=" << alt << "\n"; if (!alt) testResult=1;
            key(VK_CONTROL); key(VK_MENU); key(VK_SHIFT); key(VK_F9); key(VK_F9,true); key(VK_SHIFT,true); key(VK_MENU,true); key(VK_CONTROL,true);
        } else if (testStep==116 && settingsWindow) {
            bool combined=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==MAKEWORD(VK_F9,HOTKEYF_CONTROL|HOTKEYF_ALT|HOTKEYF_SHIFT);
            SendMessageW(settingsWindow,WM_COMMAND,IDC_DELETE_HOTKEY,0);
            bool cleared=SendDlgItemMessageW(settingsWindow,IDC_HOTKEY,HKM_GETHOTKEY,0,0)==0 && hotkeyKey=='2';
            log << "hotkey-captures-combined-modifiers=" << combined << "\nhotkey-delete-awaits-save=" << cleared << "\n";
            if (!(combined && cleared)) testResult=1;
            CheckDlgButton(settingsWindow,IDC_SHOW_HINT,BST_CHECKED); CheckDlgButton(settingsWindow,IDC_STARTUP,BST_CHECKED);
            PostMessageW(settingsWindow,WM_COMMAND,IDCANCEL,0);
        } else if (testStep>=114 && testStep<146) {
            auto index=(testStep-114)/8, phase=(testStep-114)%8;
            int x=testOrigin.x+150+static_cast<int>(index%2)*260;
            int y=testOrigin.y+180+static_cast<int>(index/2)*150;
            if (phase==0) {
                constexpr WORD colors[]{'R','G','B','P'};
                key(colors[index]); key(colors[index],true);
                key(static_cast<WORD>('2'+index)); key(static_cast<WORD>('2'+index),true); mouseMove(x,y);
            }
            if (phase==1) {
                INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; SendInput(1,&input,sizeof(input));
            }
            if (phase==2) mouseMove(x+60,y+35);
            if (phase==3) {
                bool shown=drawingShape && preview.Points().Size()>1;
                log << "shape-preview-" << index << "=" << shown << "\n";
                if (!shown) testResult=1;
                mouseMove(x+200,y+105);
            }
            if (phase==4) {
                INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input));
            }
            if (phase==6) {
                auto strokes=presenter.StrokeContainer().GetStrokes();
                bool complete=strokes.Size()==index+1 && !drawingShape && preview.Points().Size()==0;
                if (complete) {
                    auto attrs=strokes.GetAt(index).DrawingAttributes();
                    complete=attrs.IgnorePressure() && !attrs.FitToCurve() && attrs.Size().Width==6 && attrs.Color()==Palette[colorIndex].value;
                }
                log << "shape-committed-one-stroke-" << index << "=" << complete << "\n";
                if (!complete) testResult=1;
            }
        } else if (testStep==146) {
            auto dc=GetDC(nullptr),memory=CreateCompatibleDC(dc);
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth=600; bitmap.bmiHeader.biHeight=-400;
            bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32;
            void* pixels{}; auto image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
            auto old=SelectObject(memory,image);
            BitBlt(memory,0,0,600,400,dc,testOrigin.x+100,testOrigin.y+100,SRCCOPY);
            BITMAPFILEHEADER file{}; file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
            file.bfSize=file.bfOffBits+600*400*4;
            std::ofstream capture("build/shapes-test.bmp",std::ios::binary);
            capture.write(reinterpret_cast<char*>(&file),sizeof(file));
            capture.write(reinterpret_cast<char*>(&bitmap.bmiHeader),sizeof(BITMAPINFOHEADER));
            capture.write(static_cast<char*>(pixels),600*400*4);
            SelectObject(memory,old); DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr,dc);
            key(VK_CONTROL); key('Z'); key('Z',true); key(VK_CONTROL,true);
        } else if (testStep==148) {
            bool undone=presenter.StrokeContainer().GetStrokes().Size()==3;
            log << "arrow-undo-whole-shape=" << undone << "\n"; if (!undone) testResult=1;
            key('1'); key('1',true); mouseMove(testOrigin.x+200,testOrigin.y+450);
        } else if (testStep==149) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; SendInput(1,&input,sizeof(input));
        } else if (testStep>=150 && testStep<=152) {
            mouseMove(testOrigin.x+200+static_cast<int>(testStep-149)*30,testOrigin.y+450);
        } else if (testStep==153) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input));
        } else if (testStep==156) {
            bool mixed=presenter.StrokeContainer().GetStrokes().Size()==4 && presenter.IsInputEnabled();
            log << "return-to-freehand-and-mixed-ink=" << mixed << "\n"; if (!mixed) testResult=1;
            undo(); bool ordered=presenter.StrokeContainer().GetStrokes().Size()==3;
            log << "mixed-undo-order=" << ordered << "\n"; if (!ordered) testResult=1;
            clear(); key('2'); key('2',true);
        } else if (testStep==158) {
            touch(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,0);
        } else if (testStep==159) {
            POINTER_TOUCH_INFO contact{}; contact.pointerInfo.pointerType=PT_TOUCH; contact.pointerInfo.pointerId=0;
            contact.pointerInfo.pointerFlags=POINTER_FLAG_UPDATE|POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;
            contact.pointerInfo.ptPixelLocation={testOrigin.x+440,testOrigin.y+380};
            InjectTouchInput(1,&contact);
        } else if (testStep==160) {
            POINTER_TOUCH_INFO contact{}; contact.pointerInfo.pointerType=PT_TOUCH; contact.pointerInfo.pointerId=0;
            contact.pointerInfo.pointerFlags=POINTER_FLAG_UP;
            contact.pointerInfo.ptPixelLocation={testOrigin.x+440,testOrigin.y+380};
            InjectTouchInput(1,&contact);
        } else if (testStep==163) {
            bool finger=presenter.StrokeContainer().GetStrokes().Size()==1;
            log << "finger-shape=" << finger << "\n"; if (!finger) testResult=1;
            leave(); bool empty=presenter.StrokeContainer().GetStrokes().Size()==0 && !drawingShape;
            log << "shape-exit-clears=" << empty << "\n"; if (!empty) testResult=1;
            enter(); setTool(Tool::Rectangle); mouseMove(testOrigin.x+200,testOrigin.y+240);
        } else if (testStep==165 || testStep==171) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; SendInput(1,&input,sizeof(input));
        } else if (testStep==166) {
            mouseMove(testOrigin.x+400,testOrigin.y+370);
        } else if (testStep==167) {
            key(VK_CONTROL); key('Z'); key('Z',true); key(VK_CONTROL,true);
        } else if (testStep==168 || testStep==172) {
            INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input));
        } else if (testStep==170) {
            bool cancelled=!drawingShape && preview.Points().Size()==0 && presenter.StrokeContainer().GetStrokes().Size()==0;
            log << "undo-cancels-live-shape=" << cancelled << "\n"; if (!cancelled) testResult=1;
            setTool(Tool::Rectangle);
        } else if (testStep==174) {
            bool tiny=presenter.StrokeContainer().GetStrokes().Size()==0;
            log << "shape-click-without-drag-no-mark=" << tiny << "\n"; if (!tiny) testResult=1;
            shapeStart={400,350}; shapeEnd={200,180}; shapeAttributes=presenter.CopyDefaultDrawingAttributes();
            auto rect=shapePoints(); bool reversed=rect.size()==5 && rect.front()==rect.back() && rect[1].X==200 && rect[1].Y==350;
            setTool(Tool::Ellipse); auto ellipse=shapePoints();
            reversed=reversed && ellipse.size()>=49 && ellipse.front()==ellipse.back();
            log << "reverse-drag-shape-geometry=" << reversed << "\n"; if (!reversed) testResult=1;
            setTool(Tool::Arrow); auto arrow=shapePoints();
            bool tipAtStart=arrow.size()==5 && arrow[1]==shapeStart && arrow[3]==shapeStart && arrow[0]==shapeEnd;
            log << "arrow-tip-at-pointer-down=" << tipAtStart << "\n"; if (!tipAtStart) testResult=1;
            setTool(Tool::Pen); updateWidth(12); leave();
            enter(); clear();
        } else if (testStep>=176 && testStep<224) {
            auto index=(testStep-176)/12,phase=(testStep-176)%12;
            WORD modifier=index==0 || index==3 ? VK_CONTROL : index==1 ? VK_TAB : VK_SHIFT;
            if (phase==0) {
                key(modifier); if (index==3) key(VK_SHIFT);
                mouseMove(testOrigin.x+220,testOrigin.y+230);
            }
            if (phase==1) {
                bool selected=tool==static_cast<Tool>(index+1);
                log << "hold-select-tool-" << index << "=" << selected << "\n"; if (!selected) testResult=1;
            }
            if (phase==2) {
                INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; SendInput(1,&input,sizeof(input));
            }
            if (phase==3) mouseMove(testOrigin.x+420,testOrigin.y+350);
            if (phase==4) {
                key(modifier,true); if (index==3) key(VK_SHIFT,true);
            }
            if (phase==5) {
                bool preserved=drawingShape && tool==static_cast<Tool>(index+1) && requestedTool==Tool::Pen;
                log << "release-key-mid-drag-preserves-shape-" << index << "=" << preserved << "\n";
                if (!preserved) testResult=1;
            }
            if (phase==6) {
                INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input));
            }
            if (phase==8) {
                bool completed=tool==Tool::Pen && presenter.IsInputEnabled() && !drawingShape && presenter.StrokeContainer().GetStrokes().Size()==index+1;
                log << "held-shape-completes-and-restores-pen-" << index << "=" << completed << "\n";
                if (!completed) testResult=1;
            }
        } else if (testStep==225) {
            leave();
            bool defaults=!showHint && !startupEnabled() && hintPanel.Visibility()==Visibility::Collapsed;
            std::wstring error;
            bool enabled=applySettings(hotkeyModifiers,hotkeyKey,true,true,error) && startupEnabled() &&
                hintPanel.Visibility()==Visibility::Visible && GetPrivateProfileIntW(L"UI",L"ShowHint",0,settingsPath.c_str())==1;
            bool secondLine=std::wstring(hintText.Text()).find(L"\n按住 Ctrl 矩形 / Tab 椭圆 / Shift 直线 / Ctrl+Shift 箭头")!=std::wstring::npos;
            bool occupied=RegisterHotKey(window,21,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'8')!=FALSE;
            bool rolledBack=occupied && !applySettings(MOD_CONTROL|MOD_ALT,'8',false,false,error) && startupEnabled() && showHint &&
                GetPrivateProfileIntW(L"UI",L"ShowHint",0,settingsPath.c_str())==1;
            if (occupied) UnregisterHotKey(window,21);
            bool disabled=applySettings(hotkeyModifiers,hotkeyKey,false,false,error) && !startupEnabled() &&
                hintPanel.Visibility()==Visibility::Collapsed && GetPrivateProfileIntW(L"UI",L"ShowHint",1,settingsPath.c_str())==0;
            bool deleted=applySettings(0,0,false,false,error) && !hotkey && hotkeyKey==0 &&
                GetPrivateProfileIntW(L"Hotkey",L"Key",'2',settingsPath.c_str())==0 &&
                validShortcut(GetPrivateProfileIntW(L"Hotkey",L"Modifiers",99,settingsPath.c_str()),0);
            bool released=RegisterHotKey(window,20,MOD_CONTROL|MOD_NOREPEAT,'2')!=FALSE;
            if (released) UnregisterHotKey(window,20);
            bool restored=applyShortcut(MOD_CONTROL,'2',error);
            log << "hotkey-delete-persists-and-unregisters=" << (deleted && released && restored) << "\n";
            if (!(deleted && released && restored)) testResult=1;
            RegDeleteKeyW(HKEY_CURRENT_USER,startupKey());
            log << "new-options-default-off=" << defaults << "\noptions-enable-persist=" << enabled << "\nhint-shortcuts-second-line=" << secondLine <<
                "\noptions-shortcut-conflict-rolls-back=" << rolledBack << "\noptions-disable-persist=" << disabled << "\n";
            if (!(defaults && enabled && secondLine && rolledBack && disabled)) testResult=1;
            enter();
            bool animated=cursorAnimating && cursorDiameter>190 && cursorSurface.Visibility()==Visibility::Visible;
            SetCursor(LoadCursorW(nullptr,IDC_ARROW)); // Ink/XAML may replace the cursor shape while active.
            CURSORINFO nativeCursor{sizeof(nativeCursor)};
            bool hiddenDespiteShape=GetCursorInfo(&nativeCursor) && !(nativeCursor.flags&CURSOR_SHOWING);
            log << "system-cursor-hidden-despite-shape-change=" << hiddenDespiteShape << "\n";
            if (!hiddenDespiteShape) testResult=1;
            finishCursorAnimation();
            bool settled=!cursorAnimating && cursorDiameter==penWidth;
            setColor('W');
            bool colorSync=cursorDot.Fill().as<SolidColorBrush>().Color()==Palette[colorIndex].value &&
                cursorDot.Stroke().as<SolidColorBrush>().Color().R==25;
            POINT cursorPoint{}; GetCursorPos(&cursorPoint); ScreenToClient(island,&cursorPoint);
            float cursorScale=static_cast<float>(GetDpiForWindow(island))/96.0f;
            bool centered=std::abs(Canvas::GetLeft(cursorDot)+cursorDot.Width()/2-cursorPoint.x/cursorScale)<1 &&
                std::abs(Canvas::GetTop(cursorDot)+cursorDot.Height()/2-cursorPoint.y/cursorScale)<1;
            leave();
            bool restoredCursor=GetCursor()==LoadCursorW(nullptr,IDC_ARROW) && cursorHideCalls==0 && !cursorAnimating && cursorSurface.Visibility()==Visibility::Collapsed;
            log << "cursor-entry-200-animation=" << animated << "\ncursor-animation-settles-at-width=" << settled <<
                "\ncursor-color-and-contrast-sync=" << colorSync << "\ncursor-hotspot-centered=" << centered << "\ncursor-restored-on-exit=" << restoredCursor << "\n";
            if (!(animated && settled && colorSync && centered && restoredCursor)) testResult=1;
            log << "result=" << testResult << "\n"; log.flush();
            DestroyWindow(testBackground); PostMessageW(window,WM_CLOSE,0,0);
        }
        ++testStep;
    }
public:
    int verifyPortable() {
        loadSettings();
        auto path=executablePath(); auto folder=path.substr(0,path.find_last_of(L"\\"));
        bool valid=settingsPath==folder+L"\\settings.ini" && saveBrushSettings();
        valid=valid && GetPrivateProfileIntW(L"Pen",L"Width",0,settingsPath.c_str())==static_cast<UINT>(penWidth) &&
            GetPrivateProfileIntW(L"Pen",L"Color",99,settingsPath.c_str())==colorIndex &&
            GetPrivateProfileIntW(L"Pen",L"Tool",99,settingsPath.c_str())==static_cast<UINT>(tool);
        std::ofstream report(folder+L"\\portable-test.log");
        report << "adjacent-config-and-write=" << valid << "\nhotkey-modifiers=" << hotkeyModifiers << "\nhotkey-key=" << hotkeyKey <<
            "\npen-width=" << penWidth << "\npen-color=" << colorIndex << "\npen-tool=" << static_cast<int>(tool) <<
            "\nhint=" << showHint << "\nresult=" << (valid ? 0 : 1) << "\n";
        return valid ? 0 : 1;
    }
    int run(HINSTANCE instance, bool testing, bool cursorTesting=false) {
        selfTest=testing; cursorOnlyTest=cursorTesting;
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS}; InitCommonControlsEx(&controls);
        loadSettings();
        if (testing) log.open(cursorOnlyTest ? "build/cursor-test.log" : "build/self-test.log");
        WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=instance;
        wc.hIcon=appIcon(false);
        wc.lpszClassName=ClassName; wc.hCursor=nullptr;
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
        initializeShapes(root);
        root.AddHandler(UIElement::PointerPressedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, auto const&) { if (active && cursorAnimating) finishCursorAnimation(); })),true);
        root.AddHandler(UIElement::PointerWheelChangedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (active && (args.KeyModifiers() & Windows::System::VirtualKeyModifiers::Control) != Windows::System::VirtualKeyModifiers::None) {
                    wheel(args.GetCurrentPoint(canvas).Properties().MouseWheelDelta());
                    args.Handled(true);
                }
            })),true);
        Border hint;
        hintPanel=hint;
        hint.Background(SolidColorBrush(Color{235,30,33,39})); hint.CornerRadius({8,8,8,8});
        hint.Padding({14,8,14,8}); hint.Margin({16,16,16,16});
        hint.HorizontalAlignment(HorizontalAlignment::Left); hint.VerticalAlignment(VerticalAlignment::Top);
        hint.MaxWidth(760);
        hint.IsHitTestVisible(false);
        hintText=TextBlock();
        hintText.Foreground(SolidColorBrush(Color{255,255,255,255})); hintText.FontSize(14);
        hintText.TextWrapping(TextWrapping::Wrap);
        hint.Child(hintText); root.Children().Append(hint); updateWidth(penWidth);
        cursorSurface=Canvas(); cursorSurface.IsHitTestVisible(false);
        cursorSurface.Visibility(Visibility::Collapsed);
        cursorDot=Windows::UI::Xaml::Shapes::Ellipse();
        cursorSurface.Children().Append(cursorDot); root.Children().Append(cursorSurface);
        root.AddHandler(UIElement::PointerMovedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, auto const&) { positionCursor(); })),true);
        source.Content(root);
        // Ink uses an independent input thread and cursor, separate from CoreWindow's cursor.
        inkInput=Windows::UI::Input::Inking::Core::CoreInkIndependentInputSource::Create(presenter);
        inkInput.PointerEntering([this](auto const& input, auto const&) {
            // This setter must execute on Ink's input thread, not the XAML UI thread.
            input.PointerCursor(nullptr);
            inkCursorConfigured=true;
        });
        Window::Current().CoreWindow().PointerCursor(nullptr);
        SetWindowPos(island,nullptr,0,0,800,600,SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated"); tray();
        hotkey=hotkeyKey && RegisterHotKey(window,hotkeyId,hotkeyModifiers | MOD_NOREPEAT,hotkeyKey) != FALSE;
        if (testing) log << "hotkey-registered=" << hotkey << " error=" << GetLastError() << "\n";
        WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
        if (hotkeyKey && !hotkey && !testing) MessageBoxW(nullptr,(shortcutText()+L" 已被占用。请通过托盘开始标注，或在设置中修改快捷键。").c_str(),L"MyZoomIt",MB_OK | MB_ICONWARNING);
        if (testing) SetTimer(window,1,100,nullptr);
        MSG msg{};
        int status;
        while ((status=GetMessageW(&msg,nullptr,0,0)) > 0) {
            if (active && (msg.message==WM_KEYDOWN || msg.message==WM_KEYUP) &&
                (msg.wParam==VK_CONTROL || msg.wParam==VK_SHIFT || msg.wParam==VK_TAB ||
                 msg.wParam==VK_LCONTROL || msg.wParam==VK_RCONTROL || msg.wParam==VK_LSHIFT || msg.wParam==VK_RSHIFT)) {
                syncHeldTool();
                // Tab is a drawing modifier here, so it must not move keyboard focus.
                if (msg.wParam==VK_TAB) continue;
            }
            if (active && msg.message == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL)&0x8000)) {
                wheel(GET_WHEEL_DELTA_WPARAM(msg.wParam)); continue;
            }
            if (active && msg.message == WM_KEYDOWN && (msg.hwnd == window || IsChild(window,msg.hwnd))) {
                if (msg.wParam == VK_ESCAPE) { leave(); continue; }
                if (msg.wParam == 'Z' && (GetKeyState(VK_CONTROL)&0x8000)) { undo(); continue; }
                if (!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && msg.wParam>='1' && msg.wParam<='5') {
                    setTool(static_cast<Tool>(msg.wParam-'1')); continue;
                }
                if (!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && setColor(static_cast<WORD>(msg.wParam))) continue;
                if (msg.wParam == 'C') { clear(); continue; }
            }
            if (active && msg.message==WM_SETCURSOR && (msg.hwnd==window || IsChild(window,msg.hwnd))) {
                SetCursor(nullptr); continue;
            }
            if (active && cursorAnimating && (msg.message==WM_LBUTTONDOWN || msg.message==WM_POINTERDOWN)) finishCursorAnimation();
            BOOL handled=FALSE; check_hresult(native->PreTranslateMessage(&msg,&handled));
            if (!handled) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (active) hideSystemCursor();
        }
        cursorDot=nullptr; cursorSurface=nullptr; inkInput=nullptr; presenter=nullptr; canvas=nullptr; hintText=nullptr; hintPanel=nullptr; preview=nullptr; shapeSurface=nullptr; source.Content(nullptr); native=nullptr;
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
        App app; auto argument=std::wstring(command);
        result=argument==L"--portable-test" ? app.verifyPortable() : app.run(instance,argument==L"--self-test" || argument==L"--cursor-test",argument==L"--cursor-test");
    } catch (hresult_error const& e) {
        wchar_t code[32]{}; swprintf_s(code,L"\nHRESULT: 0x%08X",static_cast<unsigned>(e.code().value));
        auto text=std::wstring(L"初始化 Windows Ink 失败：\n") + e.message().c_str() + code;
        MessageBoxW(nullptr,text.c_str(),L"MyZoomIt",MB_OK | MB_ICONERROR);
    }
    CloseHandle(mutex); return result;
}



