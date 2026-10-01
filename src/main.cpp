#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include "settings.h"
#include "version.h"
#include "freehand.h"
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
#include <string>
#include <algorithm>
#include <array>
#include <vector>

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
    HMONITOR monitor{};
    bool active{}, changing{}, hotkey{};
    UINT taskbarCreated{};
    WindowsXamlManager manager{nullptr};
    DesktopWindowXamlSource source{nullptr};
    com_ptr<IDesktopWindowXamlSourceNative2> native;
    InkCanvas canvas{nullptr};
    InkPresenter presenter{nullptr};
    Windows::UI::Input::Inking::Core::CoreInkIndependentInputSource inkInput{nullptr};
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
    bool drawingFreehand{};
    uint32_t freehandPointer{};
    Windows::Devices::Input::PointerDeviceType freehandDevice{Windows::Devices::Input::PointerDeviceType::Mouse};
    Canvas freehandSurface{nullptr};
    Grid freehandInputRoot{nullptr};
    Windows::UI::Xaml::Shapes::Path liveFreehandPath{nullptr};
    Canvas liveFreehandStroke{nullptr};
    struct FreehandSample { Windows::Foundation::Point point; uint64_t time; };
    std::vector<FreehandSample> freehandSamples;
    float freehandStrokeWidth{};
    std::vector<freehand::Point> freehandTail;
    float freehandPressureSeed{-1}, freehandRunningLength{};
    bool freehandMinimumLength{};
    uint64_t freehandLastRender{};
    struct UndoItem {
        uint32_t inkId{};
        Canvas freehandStroke{nullptr};
    };
    std::vector<UndoItem> drawingOrder;

    void cancelFreehand() {
        if (!drawingFreehand) return;
        drawingFreehand=false;
        if (liveFreehandStroke && freehandSurface) {
            uint32_t index{};
            if (freehandSurface.Children().IndexOf(liveFreehandStroke,index)) freehandSurface.Children().RemoveAt(index);
        }
        liveFreehandPath=nullptr; liveFreehandStroke=nullptr; freehandSamples.clear(); freehandTail.clear();
        if (freehandInputRoot) freehandInputRoot.ReleasePointerCaptures();
    }
    void addFreehandSample(Windows::UI::Input::PointerPoint const& point) {
        auto position=point.Position();
        if (!freehandSamples.empty()) {
            auto const& last=freehandSamples.back();
            if (std::hypot(position.X-last.point.X,position.Y-last.point.Y)<0.3f) return;
            if (point.Timestamp()<=last.time) return;
        }
        freehandSamples.push_back({position,point.Timestamp()});
        if (freehandSamples.size()>2) freehandSamples.erase(freehandSamples.begin());
        using namespace freehand;
        Vec raw{position.X,position.Y};
        if (freehandTail.empty()) { freehandTail.push_back({raw,{1,1},0,0}); return; }
        auto prior=freehandTail.back();
        Vec adjusted=lerp(prior.position,raw,0.575f); // upstream streamline=.5
        float distance=length(sub(adjusted,prior.position));
        if (distance<=0) return;
        freehandRunningLength+=distance;
        if (!freehandMinimumLength && freehandRunningLength<freehandStrokeWidth) return;
        freehandMinimumLength=true;
        freehandTail.push_back({adjusted,unit(sub(prior.position,adjusted)),distance,freehandRunningLength});
        if (freehandPressureSeed<0 && freehandTail.size()==2) freehandTail[0].vector=freehandTail[1].vector;
    }
    PathFigure freehandFigure(std::vector<freehand::Vec> const& vertices) {
        PathFigure figure; figure.IsClosed(true); figure.IsFilled(true);
        if (vertices.empty()) return figure;
        auto midpoint=[](auto a, auto b) {
            return Windows::Foundation::Point{(a.x+b.x)*0.5f,(a.y+b.y)*0.5f};
        };
        figure.StartPoint(midpoint(vertices.back(),vertices.front()));
        // Upstream's quadratic midpoint rendering, rather than a faceted polygon.
        for (size_t i=0;i<vertices.size();++i) {
            auto a=vertices[i], b=vertices[(i+1)%vertices.size()];
            QuadraticBezierSegment segment;
            segment.Point1({a.x,a.y}); segment.Point2(midpoint(a,b));
            figure.Segments().Append(segment);
        }
        return figure;
    }
    void renderFreehand(bool finishing=false) {
        if (!liveFreehandPath || freehandTail.empty()) return;
        auto now=GetTickCount64();
        if (!finishing && freehandLastRender && now-freehandLastRender<8 && freehandTail.size()<258) return;
        freehandLastRender=now;
        float widthScale=freehandDevice==Windows::Devices::Input::PointerDeviceType::Mouse ? 0.8f : 1.0f;
        // Freeze bounded portions. Keep a shared endpoint and its incoming pressure.
        // Frozen Paths do not change; only the live tail is tessellated again.
        // One parent Canvas keeps undo/clear at stroke granularity.
        while (freehandTail.size()>=258) {
            std::vector<freehand::Point> chunk(freehandTail.begin(),freehandTail.begin()+256);
            auto result=freehand::outline(chunk,freehandStrokeWidth,freehandPressureSeed,widthScale);
            PathGeometry frozenGeometry; frozenGeometry.FillRule(FillRule::Nonzero);
            frozenGeometry.Figures().Append(freehandFigure(result.vertices));
            Windows::UI::Xaml::Shapes::Path frozen;
            frozen.IsHitTestVisible(false); frozen.Fill(liveFreehandPath.Fill()); frozen.Data(frozenGeometry);
            liveFreehandStroke.Children().InsertAt(liveFreehandStroke.Children().Size()-1,frozen);
            freehandPressureSeed=result.pressureBeforeLast;
            freehandTail.erase(freehandTail.begin(),freehandTail.begin()+255);
        }
        auto tail=freehandTail;
        if (!freehandMinimumLength && !freehandSamples.empty()) {
            auto raw=freehandSamples.back().point;
            freehand::Vec target{raw.X,raw.Y};
            auto start=tail.front().position;
            auto end=finishing ? target : freehand::lerp(start,target,0.575f);
            float distance=freehand::length(freehand::sub(end,start));
            if (distance>0) {
                auto direction=freehand::unit(freehand::sub(start,end));
                tail.front().vector=direction;
                tail.push_back({end,direction,distance,distance});
            }
        } else if (finishing && tail.size()>1 && !freehandSamples.empty()) {
            auto raw=freehandSamples.back().point;
            auto& last=tail.back(); auto prior=tail[tail.size()-2];
            last.position={raw.X,raw.Y};
            last.distance=freehand::length(freehand::sub(last.position,prior.position));
            last.vector=freehand::unit(freehand::sub(prior.position,last.position));
            last.runningLength=prior.runningLength+last.distance;
        }
        auto result=freehand::outline(tail,freehandStrokeWidth,freehandPressureSeed,widthScale);
        PathGeometry geometry; geometry.FillRule(FillRule::Nonzero);
        geometry.Figures().Append(freehandFigure(result.vertices));
        liveFreehandPath.Data(geometry);
    }
    void initializeFreehand(Grid const& root) {
        freehandInputRoot=root;
        freehandSurface=Canvas(); freehandSurface.IsHitTestVisible(false);
        root.Children().Append(freehandSurface);
        root.AddHandler(UIElement::PointerPressedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                auto point=args.GetCurrentPoint(freehandInputRoot);
                auto device=point.PointerDevice().PointerDeviceType();
                bool isMouse=device==Windows::Devices::Input::PointerDeviceType::Mouse;
                bool isTouch=device==Windows::Devices::Input::PointerDeviceType::Touch;
                if (!active || tool!=Tool::Pen || (!isMouse && !isTouch)) return;
                // One active freehand pointer; extra fingers must not start another stroke.
                if (drawingFreehand) { args.Handled(true); return; }
                if ((isMouse && !point.Properties().IsLeftButtonPressed()) ||
                    (isTouch && !point.IsInContact())) return;
                if (!freehandInputRoot.CapturePointer(args.Pointer())) return;
                freehandDevice=device;
                finishCursorAnimation(); drawingFreehand=true; freehandPointer=args.Pointer().PointerId();
                freehandStrokeWidth=penWidth; freehandSamples.clear();
                freehandTail.clear(); freehandPressureSeed=-1; freehandRunningLength=0;
                freehandMinimumLength=false; freehandLastRender=0;
                addFreehandSample(point);
                liveFreehandPath=Windows::UI::Xaml::Shapes::Path();
                liveFreehandPath.IsHitTestVisible(false); liveFreehandPath.Fill(SolidColorBrush(Palette[colorIndex].value));
                liveFreehandStroke=Canvas(); liveFreehandStroke.IsHitTestVisible(false);
                liveFreehandStroke.Children().Append(liveFreehandPath);
                freehandSurface.Children().Append(liveFreehandStroke); renderFreehand(); args.Handled(true);
            })),true);
        root.AddHandler(UIElement::PointerMovedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (!drawingFreehand || args.Pointer().PointerId()!=freehandPointer) return;
                auto points=args.GetIntermediatePoints(freehandInputRoot);
                for (uint32_t i=points.Size();i>0;--i) addFreehandSample(points.GetAt(i-1));
                renderFreehand(); args.Handled(true);
            })),true);
        root.AddHandler(UIElement::PointerReleasedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (!drawingFreehand || args.Pointer().PointerId()!=freehandPointer) return;
                addFreehandSample(args.GetCurrentPoint(freehandInputRoot)); renderFreehand(true);
                drawingOrder.push_back({0,liveFreehandStroke});
                drawingFreehand=false; liveFreehandPath=nullptr; liveFreehandStroke=nullptr; freehandSamples.clear(); freehandTail.clear();
                freehandInputRoot.ReleasePointerCapture(args.Pointer()); args.Handled(true);
            })),true);
        root.PointerCanceled([this](auto const&, auto const& args) {
            if (drawingFreehand && args.Pointer().PointerId()==freehandPointer) cancelFreehand();
        });
        root.PointerCaptureLost([this](auto const&, auto const& args) {
            if (drawingFreehand && args.Pointer().PointerId()==freehandPointer) cancelFreehand();
        });
    }

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
        cancelFreehand();
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
            auto stroke=builder.CreateStroke(points);
            presenter.StrokeContainer().AddStroke(stroke);
            drawingOrder.push_back({stroke.Id(),nullptr});
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
        cursorDot.Opacity(opacity);
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
        return L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
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
        cancelFreehand(); drawingOrder.clear();
        if (freehandSurface) freehandSurface.Children().Clear();
        cancelShape();
        if (shapeSurface && tool!=requestedTool) setTool(requestedTool);
        if (presenter) presenter.StrokeContainer().Clear();
    }
    void undo() {
        if (drawingFreehand) { cancelFreehand(); return; }
        if (drawingShape) { cancelShape(); if (tool!=requestedTool) setTool(requestedTool); return; }
        auto container=presenter.StrokeContainer();
        auto strokes=container.GetStrokes();
        if (!drawingOrder.empty()) {
            auto item=drawingOrder.back();
            if (item.freehandStroke) {
                uint32_t index{};
                if (freehandSurface.Children().IndexOf(item.freehandStroke,index)) freehandSurface.Children().RemoveAt(index);
            } else {
                for (auto const& stroke:strokes) stroke.Selected(stroke.Id()==item.inkId);
                container.DeleteSelected();
            }
            drawingOrder.pop_back(); return;
        }
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
        case WM_TIMER: if (w==CursorTimer) animateCursor(); return 0;
        case WM_CLOSE: saveBrushSettings(); if (settingsWindow) EndDialog(settingsWindow,IDCANCEL); leave(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window,CursorTimer);
            restoreSystemCursor();
            UnregisterHotKey(window, hotkeyId);
            WTSUnRegisterSessionNotification(window);
            { NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; Shell_NotifyIconW(NIM_DELETE, &icon); }
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }
public:
    int run(HINSTANCE instance) {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS}; InitCommonControlsEx(&controls);
        loadSettings();
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
        presenter.InputDeviceTypes(Windows::UI::Core::CoreInputDeviceTypes::Pen);
        InkDrawingAttributes attributes;
        attributes.Color(Palette[colorIndex].value); attributes.Size({penWidth,penWidth});
        attributes.IgnorePressure(false); attributes.FitToCurve(true);
        attributes.ModelerAttributes().UseVelocityBasedPressure(false);
        presenter.UpdateDefaultDrawingAttributes(attributes); presenter.IsInputEnabled(false);
        presenter.StrokesCollected([this](auto const&, InkStrokesCollectedEventArgs const& args) {
            if (!active) { clear(); return; }
            for (auto const& stroke:args.Strokes()) drawingOrder.push_back({stroke.Id(),nullptr});
        });
        root.Children().Append(canvas);
        initializeFreehand(root);
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
        });
        Window::Current().CoreWindow().PointerCursor(nullptr);
        SetWindowPos(island,nullptr,0,0,800,600,SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated"); tray();
        hotkey=hotkeyKey && RegisterHotKey(window,hotkeyId,hotkeyModifiers | MOD_NOREPEAT,hotkeyKey) != FALSE;
        WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
        if (hotkeyKey && !hotkey) MessageBoxW(nullptr,(shortcutText()+L" 已被占用。请通过托盘开始标注，或在设置中修改快捷键。").c_str(),L"MyZoomIt",MB_OK | MB_ICONWARNING);
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
        cancelFreehand(); freehandSurface=nullptr; freehandInputRoot=nullptr; drawingOrder.clear();
        cursorDot=nullptr; cursorSurface=nullptr; inkInput=nullptr; presenter=nullptr; canvas=nullptr; hintText=nullptr; hintPanel=nullptr; preview=nullptr; shapeSurface=nullptr; source.Content(nullptr); native=nullptr;
        source.Close(); source=nullptr; manager.Close(); manager=nullptr;
        return status==-1 ? 1 : static_cast<int>(msg.wParam);
    }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
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
        App app; result=app.run(instance);
    } catch (hresult_error const& e) {
        wchar_t code[32]{}; swprintf_s(code,L"\nHRESULT: 0x%08X",static_cast<unsigned>(e.code().value));
        auto text=std::wstring(L"初始化 Windows Ink 失败：\n") + e.message().c_str() + code;
        MessageBoxW(nullptr,text.c_str(),L"MyZoomIt",MB_OK | MB_ICONERROR);
    }
    CloseHandle(mutex); return result;
}

