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

#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <cstring>
#include <winrt/Windows.Storage.Streams.h>
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
#include <memory>
#include <windowsx.h>
#include <stdexcept>
#include "timer_window.h"
#include "settings_tabs.h"

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
constexpr UINT AboutMessage = WM_APP + 5;
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

MIDL_INTERFACE("905a0fef-bc53-11df-8c49-001e4fc686da")
PixelBufferAccess : ::IUnknown { virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value)=0; };

class App {
    TimerWindow timer;
    bool timerHotkey{};
    int settingsPage{};
    void timerSettings(HWND =nullptr) { settings(2); }

    bool snapshotMode{}, browsing{};
    UINT snapshotMods{MOD_CONTROL}, snapshotKey{'1'};
    bool snapshotHotkey{};
    int initialZoom{200}, defaultPenWidth{8}, zoomWheelRemainder{};
    double zoom{1}, viewX{}, viewY{}, sceneWidth{}, sceneHeight{};
    Grid scene{nullptr};
    Image screenshot{nullptr};
    CompositeTransform viewTransform{nullptr};
    Grid viewport{nullptr};
    Grid activationSurface{nullptr};
    std::vector<uint32_t> activationPointers;
    void resetActivationSurface(bool visible) {
        activationPointers.clear();
        activationSurface.ReleasePointerCaptures();
        activationSurface.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
    }
    void releaseActivationPointer(uint32_t id) {
        std::erase(activationPointers,id);
        if (activationPointers.empty() && !browsing)
            activationSurface.Visibility(Visibility::Collapsed);
    }
    static constexpr UINT_PTR ZoomTimer=3;
    bool zoomAnimating{};
    ULONGLONG zoomStarted{};
    double zoomTarget{1}, zoomAnchorX{}, zoomAnchorY{};
    void stopZoomAnimation() {
        zoomAnimating=false; KillTimer(window,ZoomTimer);
    }
    void animateZoom() {
        if (!active || !snapshotMode || !zoomAnimating) return;
        double t=std::min(1.0,(GetTickCount64()-zoomStarted)/180.0);
        double eased=t*t*(3-2*t);
        zoom=1+(zoomTarget-1)*eased;
        viewX=zoomAnchorX*(1-1/zoom); viewY=zoomAnchorY*(1-1/zoom);
        updateView();
        if (t>=1) { stopZoomAnimation(); updateHint(); }
    }
    static HBITMAP captureScreen(RECT r) {
        auto dc=GetDC(nullptr); if (!dc) return nullptr;
        auto memory=CreateCompatibleDC(dc);
        auto bitmap=CreateCompatibleBitmap(dc,r.right-r.left,r.bottom-r.top);
        if (!memory || !bitmap) { if (memory) DeleteDC(memory); if (bitmap) DeleteObject(bitmap); ReleaseDC(nullptr,dc); return nullptr; }
        auto old=SelectObject(memory,bitmap);
        BOOL ok=BitBlt(memory,0,0,r.right-r.left,r.bottom-r.top,dc,r.left,r.top,SRCCOPY|CAPTUREBLT);
        SelectObject(memory,old); DeleteDC(memory); ReleaseDC(nullptr,dc);
        if (!ok) { DeleteObject(bitmap); return nullptr; }
        return bitmap;
    }
    void copyFinalScreen() {
        // Capture the final visible viewport before hiding or clearing the overlay.
        DwmFlush();
        RECT r{}; GetWindowRect(window,&r); auto bitmap=captureScreen(r);
        if (!bitmap) return;
        bool copied=false;
        for (int attempt=0;attempt<5;++attempt) {
            if (OpenClipboard(window)) {
                if (EmptyClipboard()) copied=SetClipboardData(CF_BITMAP,bitmap)!=nullptr;
                CloseClipboard(); break;
            }
            Sleep(10);
        }
        if (!copied) { DeleteObject(bitmap); OutputDebugStringW(L"MyZoomIt: clipboard copy failed\n"); }
    }
    void updateView() {
        viewX=std::clamp(viewX,0.0,std::max(0.0,sceneWidth-sceneWidth/zoom));
        viewY=std::clamp(viewY,0.0,std::max(0.0,sceneHeight-sceneHeight/zoom));
        viewTransform.ScaleX(zoom); viewTransform.ScaleY(zoom);
        viewTransform.TranslateX(-viewX*zoom); viewTransform.TranslateY(-viewY*zoom);
    }
    POINT browsePointer{};
    void browseMove(bool reset=false) {
        POINT p{}; GetCursorPos(&p); ScreenToClient(island,&p);
        if (!reset && p.x==browsePointer.x && p.y==browsePointer.y) return;
        double dpi=GetDpiForWindow(island)/96.0;
        if (!reset && zoomAnimating) {
            zoomAnchorX=p.x/dpi; zoomAnchorY=p.y/dpi;
            browsePointer=p; animateZoom(); return;
        }
        stopZoomAnimation();
        if (reset) { viewX=p.x/dpi*(1-1/zoom); viewY=p.y/dpi*(1-1/zoom); }
        else { viewX+=(p.x-browsePointer.x)/dpi*(1-1/zoom); viewY+=(p.y-browsePointer.y)/dpi*(1-1/zoom); }
        browsePointer=p;
        updateView();
    }
    void browseWheel(int delta) {
        zoomWheelRemainder+=delta;
        int steps=zoomWheelRemainder/WHEEL_DELTA;
        zoomWheelRemainder%=WHEEL_DELTA;
        if (!steps) return;
        if (zoomAnimating) {
            zoom=zoomTarget; viewX=zoomAnchorX*(1-1/zoom); viewY=zoomAnchorY*(1-1/zoom);
        }
        stopZoomAnimation();
        POINT p{}; GetCursorPos(&p); ScreenToClient(island,&p);
        double dpi=GetDpiForWindow(island)/96.0, x=p.x/dpi, y=p.y/dpi;
        double next=std::clamp(std::round(zoom*4)/4+steps*0.25,1.0,8.0);
        viewX+=x/zoom-x/next; viewY+=y/zoom-y/next; zoom=next;
        if (zoom==1) viewX=viewY=0;
        browsePointer=p; updateView(); updateHint();
    }
    void returnToBrowse() {
        cancelFreehand(); cancelShape(); finishErase();
        GetCursorPos(&browsePointer); ScreenToClient(island,&browsePointer);
        browsing=true; cursorAnimating=false; KillTimer(window,CursorTimer);
        resetActivationSurface(true);
        Window::Current().CoreWindow().PointerCursor(Windows::UI::Core::CoreCursor(Windows::UI::Core::CoreCursorType::Arrow,0));
        cursorSurface.Visibility(Visibility::Collapsed); restoreSystemCursor(); updateHint();
    }
    void beginAnnotation() {
        if (!active || !snapshotMode || !browsing) return;
        stopZoomAnimation();
        browsing=false;
        Window::Current().CoreWindow().PointerCursor(nullptr);
        updateHint(); hideSystemCursor(); finishCursorAnimation();
        SetTimer(window,CursorTimer,16,nullptr);
    }
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
    Border blackboard{nullptr};
    InkPresenter presenter{nullptr};
    Windows::UI::Input::Inking::Core::CoreInkIndependentInputSource inkInput{nullptr};
    enum class Tool { Pen, Rectangle, Ellipse, Line, Arrow };
    static constexpr float ShapeWidthScale=0.6f;
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
    bool eraserMode{}, erasing{}, penTipEraser{};
    uint32_t eraserPointer{};
    Windows::Foundation::Point eraserPosition{}, eraserPrevious{};
    bool cursorTouch{};
    bool cursorSuppressedByContact{};
    POINT contactCursorPosition{};
    void observeCursorInput(Windows::Devices::Input::PointerDeviceType device, bool contact) {
        if (!active) return;
        if (device!=Windows::Devices::Input::PointerDeviceType::Mouse) {
            if (!contact) return;
            cursorSuppressedByContact=true; cursorAnimating=false;
            GetCursorPos(&contactCursorPosition);
            if (cursorSurface) cursorSurface.Visibility(Visibility::Collapsed);
        } else if (cursorSuppressedByContact) {
            POINT point{}; GetCursorPos(&point);
            if (point.x==contactCursorPosition.x && point.y==contactCursorPosition.y) return;
            cursorSuppressedByContact=false; cursorTouch=false;
            refreshCursor(penWidth);
        }
    }
    static constexpr float EraserDiameter=12;
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
        InkStroke inkSnapshot{nullptr};
        uint64_t sequence{};
        std::shared_ptr<std::vector<std::vector<Windows::Foundation::Point>>> contours;
    };
    std::vector<UndoItem> drawingOrder;
    struct Edit { bool added{}; std::vector<UndoItem> items; };
    std::vector<Edit> undoHistory, redoHistory;
    std::vector<UndoItem> erasedItems;
    uint64_t nextSequence{};
    std::shared_ptr<std::vector<std::vector<Windows::Foundation::Point>>> liveContours;
    size_t frozenContourCount{};

    void recordEdit(Edit edit) {
        if (edit.items.empty()) return;
        undoHistory.push_back(std::move(edit)); redoHistory.clear();
    }
    void rememberInk(InkStroke const& stroke) {
        UndoItem item; item.inkId=stroke.Id(); item.inkSnapshot=stroke.Clone(); item.sequence=++nextSequence;
        // Each completed Ink stroke keeps its native rendering and pressure, but
        // lives alongside mouse/touch strokes in the same chronological layer.
        item.freehandStroke=Canvas(); item.freehandStroke.IsHitTestVisible(false);
        InkCanvas display; display.IsHitTestVisible(false);
        display.Width(canvas.ActualWidth()); display.Height(canvas.ActualHeight());
        display.InkPresenter().InputDeviceTypes(Windows::UI::Core::CoreInputDeviceTypes::None);
        display.InkPresenter().StrokeContainer().AddStroke(stroke.Clone());
        item.freehandStroke.Children().Append(display);
        freehandSurface.Children().Append(item.freehandStroke);
        drawingOrder.push_back(item); recordEdit({true,{item}});
    }
    void removeItem(UndoItem const& item) {
        if (item.freehandStroke) {
            uint32_t index{};
            if (freehandSurface.Children().IndexOf(item.freehandStroke,index)) freehandSurface.Children().RemoveAt(index);
        } else {
            auto container=presenter.StrokeContainer();
            for (auto const& stroke:container.GetStrokes()) stroke.Selected(stroke.Id()==item.inkId);
            container.DeleteSelected();
        }
    }
    void applyEdit(Edit const& edit, bool restore) {
        for (auto const& saved:edit.items) {
            if (!restore) {
                auto found=std::find_if(drawingOrder.begin(),drawingOrder.end(),[&](auto const& item) { return item.sequence==saved.sequence; });
                if (found!=drawingOrder.end()) { removeItem(*found); drawingOrder.erase(found); }
            } else {
                auto item=saved;
                uint32_t index=0;
                for (auto const& existing:drawingOrder) if (existing.sequence<item.sequence) ++index;
                freehandSurface.Children().InsertAt(index,item.freehandStroke);
                auto at=std::lower_bound(drawingOrder.begin(),drawingOrder.end(),item.sequence,
                    [](auto const& existing,uint64_t sequence) { return existing.sequence<sequence; });
                drawingOrder.insert(at,item);
            }
        }
    }
    static float pointDistance(Windows::Foundation::Point p,Windows::Foundation::Point a,Windows::Foundation::Point b) {
        float dx=b.X-a.X,dy=b.Y-a.Y,denominator=dx*dx+dy*dy;
        float t=denominator>0 ? std::clamp(((p.X-a.X)*dx+(p.Y-a.Y)*dy)/denominator,0.0f,1.0f) : 0;
        return std::hypot(p.X-a.X-t*dx,p.Y-a.Y-t*dy);
    }
    bool eraserHits(UndoItem const& item,Windows::Foundation::Point p) {
        if (item.contours) {
            for (auto const& polygon:*item.contours) {
                int winding=0;
                for (size_t i=0;i<polygon.size();++i) {
                    auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
                    if (pointDistance(p,a,b)<=EraserDiameter/2) return true;
                    float side=(b.X-a.X)*(p.Y-a.Y)-(p.X-a.X)*(b.Y-a.Y);
                    if (a.Y<=p.Y && b.Y>p.Y && side>0) ++winding;
                    if (a.Y>p.Y && b.Y<=p.Y && side<0) --winding;
                }
                if (winding) return true;
            }
        } else if (item.inkSnapshot) {
            auto points=item.inkSnapshot.GetInkPoints();
            float radius=EraserDiameter/2+item.inkSnapshot.DrawingAttributes().Size().Width/2;
            for (uint32_t i=0;i<points.Size();++i) {
                auto a=points.GetAt(i).Position(),b=points.GetAt(i ? i-1 : i).Position();
                if (pointDistance(p,a,b)<=radius) return true;
            }
        }
        return false;
    }
    void eraseTo(Windows::Foundation::Point point) {
        float length=std::hypot(point.X-eraserPrevious.X,point.Y-eraserPrevious.Y);
        int steps=std::max(1,static_cast<int>(std::ceil(length/3)));
        for (size_t i=drawingOrder.size();i>0;--i) {
            auto const& item=drawingOrder[i-1]; bool hit=false;
            for (int j=0;j<=steps && !hit;++j) {
                float t=static_cast<float>(j)/steps;
                hit=eraserHits(item,{eraserPrevious.X+t*(point.X-eraserPrevious.X),eraserPrevious.Y+t*(point.Y-eraserPrevious.Y)});
            }
            if (hit) { removeItem(item); erasedItems.push_back(item); drawingOrder.erase(drawingOrder.begin()+i-1); }
        }
        eraserPrevious=eraserPosition=point; positionCursor();
    }
    void finishErase() {
        if (!erasing) return;
        erasing=false; penTipEraser=false; recordEdit({false,std::move(erasedItems)}); erasedItems.clear();
        if (freehandInputRoot) freehandInputRoot.ReleasePointerCaptures();
        refreshCursor(penWidth);
    }
    void toggleEraser() {
        cancelFreehand(); finishErase();
        if (tool!=Tool::Pen) setTool(Tool::Pen);
        eraserMode=!eraserMode; cursorTouch=false;
        cursorAnimationStart=(eraserMode ? 48.0f : 24.0f)/static_cast<float>(snapshotMode ? zoom : 1.0);
        cursorStarted=GetTickCount64(); cursorAnimating=true;
        SetTimer(window,CursorTimer,16,nullptr);
        animateCursor(); updateHint();
    }

    void cancelFreehand() {
        if (!drawingFreehand) return;
        drawingFreehand=false;
        if (liveFreehandStroke && freehandSurface) {
            uint32_t index{};
            if (freehandSurface.Children().IndexOf(liveFreehandStroke,index)) freehandSurface.Children().RemoveAt(index);
        }
        liveFreehandPath=nullptr; liveFreehandStroke=nullptr; freehandSamples.clear(); freehandTail.clear(); liveContours.reset();
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
        liveContours->resize(frozenContourCount);
        auto saveContour=[this](auto const& vertices) {
            std::vector<Windows::Foundation::Point> points; points.reserve(vertices.size());
            for (auto const& point:vertices) points.push_back({point.x,point.y});
            liveContours->push_back(std::move(points));
        };
        float widthScale=freehandDevice==Windows::Devices::Input::PointerDeviceType::Mouse ? 0.8f : 1.0f;
        // Freeze bounded portions. Keep a shared endpoint and its incoming pressure.
        // Frozen Paths do not change; only the live tail is tessellated again.
        // One parent Canvas keeps undo/clear at stroke granularity.
        while (freehandTail.size()>=258) {
            std::vector<freehand::Point> chunk(freehandTail.begin(),freehandTail.begin()+256);
            auto result=freehand::outline(chunk,freehandStrokeWidth,freehandPressureSeed,widthScale);
            saveContour(result.vertices); ++frozenContourCount;
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
        saveContour(result.vertices);
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
                if (drawingFreehand || erasing) { args.Handled(true); return; }
                if ((isMouse && !point.Properties().IsLeftButtonPressed()) ||
                    (isTouch && !point.IsInContact())) return;
                if (!freehandInputRoot.CapturePointer(args.Pointer())) return;
                freehandDevice=device;
                cursorTouch=isTouch;
                if (eraserMode) {
                    cursorAnimating=false; erasing=true; eraserPointer=args.Pointer().PointerId();
                    erasedItems.clear(); eraserPrevious=eraserPosition=point.Position();
                    refreshCursor(penWidth); eraseTo(point.Position()); args.Handled(true); return;
                }
                finishCursorAnimation(); drawingFreehand=true; freehandPointer=args.Pointer().PointerId();
                freehandStrokeWidth=penWidth; freehandSamples.clear();
                freehandTail.clear(); freehandPressureSeed=-1; freehandRunningLength=0;
                freehandMinimumLength=false; freehandLastRender=0;
                liveContours=std::make_shared<std::vector<std::vector<Windows::Foundation::Point>>>(); frozenContourCount=0;
                addFreehandSample(point);
                liveFreehandPath=Windows::UI::Xaml::Shapes::Path();
                liveFreehandPath.IsHitTestVisible(false); liveFreehandPath.Fill(SolidColorBrush(Palette[colorIndex].value));
                liveFreehandStroke=Canvas(); liveFreehandStroke.IsHitTestVisible(false);
                liveFreehandStroke.Children().Append(liveFreehandPath);
                freehandSurface.Children().Append(liveFreehandStroke); renderFreehand(); args.Handled(true);
            })),true);
        root.AddHandler(UIElement::PointerMovedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (erasing && args.Pointer().PointerId()==eraserPointer) {
                    auto points=args.GetIntermediatePoints(freehandInputRoot);
                    for (uint32_t i=points.Size();i>0;--i) eraseTo(points.GetAt(i-1).Position());
                    args.Handled(true); return;
                }
                if (!drawingFreehand || args.Pointer().PointerId()!=freehandPointer) return;
                auto points=args.GetIntermediatePoints(freehandInputRoot);
                for (uint32_t i=points.Size();i>0;--i) addFreehandSample(points.GetAt(i-1));
                renderFreehand(); args.Handled(true);
            })),true);
        root.AddHandler(UIElement::PointerReleasedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (erasing && args.Pointer().PointerId()==eraserPointer) {
                    eraseTo(args.GetCurrentPoint(freehandInputRoot).Position()); finishErase(); args.Handled(true); return;
                }
                if (!drawingFreehand || args.Pointer().PointerId()!=freehandPointer) return;
                addFreehandSample(args.GetCurrentPoint(freehandInputRoot)); renderFreehand(true);
                UndoItem item; item.freehandStroke=liveFreehandStroke; item.sequence=++nextSequence; item.contours=liveContours;
                drawingOrder.push_back(item); recordEdit({true,{item}}); liveContours.reset();
                drawingFreehand=false; liveFreehandPath=nullptr; liveFreehandStroke=nullptr; freehandSamples.clear(); freehandTail.clear();
                freehandInputRoot.ReleasePointerCapture(args.Pointer()); args.Handled(true);
            })),true);
        root.PointerCanceled([this](auto const&, auto const& args) {
            if (erasing && args.Pointer().PointerId()==eraserPointer) finishErase();
            if (drawingFreehand && args.Pointer().PointerId()==freehandPointer) cancelFreehand();
        });
        root.PointerCaptureLost([this](auto const&, auto const& args) {
            if (erasing && args.Pointer().PointerId()==eraserPointer) finishErase();
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
        finishErase(); eraserMode=false;
        cancelFreehand();
        cancelShape(); tool=requestedTool=next;
        shapeSurface.Visibility(tool==Tool::Pen ? Visibility::Collapsed : Visibility::Visible);
        presenter.IsInputEnabled(active && tool==Tool::Pen);
        updateHint();
        if (active) finishCursorAnimation();
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
                float head=std::min(length*0.45f,std::max(18.0f,shapeAttributes.Size().Width/ShapeWidthScale*3));
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
            rememberInk(stroke);
            presenter.StrokeContainer().Clear();
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
            auto size=shapeAttributes.Size();
            shapeAttributes.Size({size.Width*ShapeWidthScale,size.Height*ShapeWidthScale});
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
    Windows::UI::Xaml::Shapes::Ellipse cursorRing{nullptr};
    ULONGLONG cursorStarted{};
    bool cursorAnimating{};
    float cursorAnimationStart{200};
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
        if (!active || !cursorDot || browsing) return;
        POINT point{}; GetCursorPos(&point); ScreenToClient(island,&point);
        float scale=static_cast<float>(GetDpiForWindow(island))/96.0f;
        float x=(penTipEraser || (eraserMode && cursorTouch)) ? eraserPosition.X : point.x/scale;
        if (snapshotMode && !penTipEraser && !(eraserMode && cursorTouch)) x=static_cast<float>(x/zoom+viewX);
        float y=(penTipEraser || (eraserMode && cursorTouch)) ? eraserPosition.Y : point.y/scale;
        if (snapshotMode && !penTipEraser && !(eraserMode && cursorTouch)) y=static_cast<float>(y/zoom+viewY);
        Canvas::SetLeft(cursorDot,x-static_cast<float>(cursorDot.Width())/2);
        Canvas::SetTop(cursorDot,y-static_cast<float>(cursorDot.Height())/2);
        if (cursorRing) {
            Canvas::SetLeft(cursorRing,x-static_cast<float>(cursorRing.Width())/2);
            Canvas::SetTop(cursorRing,y-static_cast<float>(cursorRing.Height())/2);
        }
    }
    void refreshCursor(float diameter, float opacity=1.0f) {
        if (!cursorDot) return;
        if (cursorSuppressedByContact || browsing) {
            cursorSurface.Visibility(Visibility::Collapsed); return;
        }
        if (eraserMode || penTipEraser) {
            float size=cursorAnimating && eraserMode && !penTipEraser ? diameter : EraserDiameter;
            cursorRing.Width(size); cursorRing.Height(size);
            cursorDot.Fill(nullptr); cursorDot.Stroke(SolidColorBrush(Color{255,255,255,255})); cursorDot.StrokeThickness(1);
            cursorDot.Width(size-2); cursorDot.Height(size-2); cursorDot.Opacity(1);
            cursorRing.Visibility(Visibility::Visible);
            cursorSurface.Visibility(active && (!cursorTouch || erasing || cursorAnimating || penTipEraser) ? Visibility::Visible : Visibility::Collapsed);
            positionCursor(); return;
        }
        if (cursorRing) cursorRing.Visibility(Visibility::Collapsed);
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
            float target=eraserMode ? EraserDiameter : penWidth;
            refreshCursor(target+(cursorAnimationStart-target)*remaining*remaining*remaining,0.25f+0.75f*progress);
            if (progress>=1) cursorAnimating=false;
        } else positionCursor();
    }
    HWND settingsWindow{};
    HWND aboutWindow{};
    inline static HWND hotkeyEdit{};
    static LRESULT CALLBACK hotkeyMessageProcedure(int code, WPARAM w, LPARAM l) {
        if (code>=0 && w==PM_REMOVE && hotkeyEdit && (GetFocus()==hotkeyEdit || GetDlgCtrlID(GetFocus())==IDC_SNAPSHOT_HOTKEY || GetDlgCtrlID(GetFocus())==IDC_TIMER_HOTKEY)) {
            auto msg=reinterpret_cast<MSG*>(l);
            // DialogBox handles Alt as menu navigation before dispatch. Give the editor normal key messages.
            if (msg->hwnd==GetFocus()) {
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
    static bool setMouseEmphasis(bool enabled) {
        return SystemParametersInfoW(SPI_SETMOUSESONAR,0,
            reinterpret_cast<void*>(static_cast<INT_PTR>(enabled)),SPIF_UPDATEINIFILE|SPIF_SENDCHANGE)!=FALSE;
    }
    bool applySettings(UINT mods, UINT code, bool hint, bool startup, int emphasis, std::wstring& error) {
        if (!validShortcut(mods,code)) { error=L"请选择包含 Ctrl 或 Alt 的组合键；F12 为系统保留键。"; return false; }
        BOOL oldEmphasis{};
        bool emphasisChanged=false;
        if (emphasis>=0) {
            if (!SystemParametersInfoW(SPI_GETMOUSESONAR,0,&oldEmphasis,0)) {
                error=L"无法读取 Windows 鼠标强调设置。"; return false;
            }
            emphasisChanged=(oldEmphasis!=FALSE)!=(emphasis!=0);
            if (emphasisChanged && !setMouseEmphasis(emphasis!=0)) {
                error=L"无法更新 Windows 鼠标强调设置。"; return false;
            }
        }
        auto restoreEmphasis=[&] {
            if (emphasisChanged && !setMouseEmphasis(oldEmphasis!=FALSE))
                error+=L"\n鼠标强调设置恢复失败，请在 Windows 鼠标设置中检查。";
        };
        auto parent=settingsPath.substr(0,settingsPath.find_last_of(L"\\"));
        CreateDirectoryW(parent.c_str(),nullptr);
        auto oldStartup=startupEnabled();
        if (!setStartup(startup,error)) { restoreEmphasis(); return false; }
        if (settingsPath.empty() || !WritePrivateProfileStringW(L"UI",L"ShowHint",hint ? L"1" : L"0",settingsPath.c_str())) {
            std::wstring rollback; setStartup(oldStartup,rollback);
            error=L"无法保存悬浮状态菜单设置。"; restoreEmphasis(); return false;
        }
        if (!applyShortcut(mods,code,error)) {
            WritePrivateProfileStringW(L"UI",L"ShowHint",showHint ? L"1" : L"0",settingsPath.c_str());
            std::wstring rollback; setStartup(oldStartup,rollback); restoreEmphasis(); return false;
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
        auto sm=GetPrivateProfileIntW(L"Snapshot",L"Modifiers",MOD_CONTROL,settingsPath.c_str());
        auto sk=GetPrivateProfileIntW(L"Snapshot",L"Key",'1',settingsPath.c_str());
        if (validShortcut(sm,sk)) { snapshotMods=sm; snapshotKey=sk; }
        initialZoom=100+25*std::clamp((static_cast<int>(GetPrivateProfileIntW(L"Snapshot",L"Zoom",200,settingsPath.c_str()))-100+12)/25,0,12);
        showHint=GetPrivateProfileIntW(L"UI",L"ShowHint",0,settingsPath.c_str())!=0;
        runAtStartup=startupEnabled();
        auto savedDefault=GetPrivateProfileIntW(L"Pen",L"DefaultWidth",8,settingsPath.c_str());
        defaultPenWidth=4+2*std::clamp((static_cast<int>(savedDefault)-4+1)/2,0,10);
        penWidth=static_cast<float>(defaultPenWidth);
        auto savedColor=GetPrivateProfileIntW(L"Pen",L"Color",0,settingsPath.c_str());
        if (savedColor<Palette.size()) colorIndex=savedColor;
        auto savedTool=GetPrivateProfileIntW(L"Pen",L"Tool",0,settingsPath.c_str());
        if (savedTool<=4) tool=requestedTool=static_cast<Tool>(savedTool);
    }
    bool saveBrushSettings() {
        auto section=L"Width="+std::to_wstring(static_cast<int>(penWidth))+L'\0'+
            L"DefaultWidth="+std::to_wstring(defaultPenWidth)+L'\0'+L"Color="+std::to_wstring(colorIndex)+L'\0'+L"Tool="+std::to_wstring(static_cast<int>(tool))+L'\0';
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
        if (msg==WM_PAINT) {
            PAINTSTRUCT paint{}; auto dc=BeginPaint(control,&paint);
            RECT area{}; GetClientRect(control,&area);
            FillRect(dc,&area,GetSysColorBrush(COLOR_WINDOW));
            auto font=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
            SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
            auto value=SendMessageW(control,HKM_GETHOTKEY,0,0);
            auto flags=HIBYTE(value); auto key=LOBYTE(value);
            std::wstring text;
            auto append=[&text](const wchar_t* part) { if (!text.empty()) text+=L" + "; text+=part; };
            if (flags & HOTKEYF_CONTROL) append(L"Ctrl");
            if (flags & HOTKEYF_ALT) append(L"Alt");
            if (flags & HOTKEYF_SHIFT) append(L"Shift");
            if (key) {
                wchar_t name[64]{};
                LONG scan=static_cast<LONG>(MapVirtualKeyW(key,MAPVK_VK_TO_VSC)<<16);
                if (flags & HOTKEYF_EXT) scan|=1<<24;
                GetKeyNameTextW(scan,name,64); append(name);
            }
            if (GetFocus()==control) {
                if (!key) text=L"请按 Ctrl / Alt 组合键…";
                RECT focus=area;InflateRect(&focus,-3,-3);DrawFocusRect(dc,&focus);
                SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHT));
            }
            DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,font); EndPaint(control,&paint); return 0;
        }
        if (msg==WM_SETFOCUS) {
            auto result=DefSubclassProc(control,msg,w,l); HideCaret(control); InvalidateRect(control,nullptr,TRUE); return result;
        }
        if (msg==WM_KILLFOCUS) {
            auto result=DefSubclassProc(control,msg,w,l);InvalidateRect(control,nullptr,TRUE);return result;
        }
        if (msg==HKM_SETHOTKEY) {
            auto result=DefSubclassProc(control,msg,w,l); InvalidateRect(control,nullptr,TRUE); return result;
        }
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
            if (w==VK_BACK || w==VK_DELETE) { SendMessageW(control,HKM_SETHOTKEY,0,0);return 0; }
            if (modifier || !(flags&(HOTKEYF_CONTROL|HOTKEYF_ALT)) || w==VK_F12 || w==VK_LWIN || w==VK_RWIN) return 0;
            if (l & (1LL<<24)) flags|=HOTKEYF_EXT;
            SendMessageW(control,HKM_SETHOTKEY,MAKEWORD(static_cast<BYTE>(w),flags),0);
            return 0;
        }
        if (msg==WM_KEYUP || msg==WM_SYSKEYUP || msg==WM_CHAR || msg==WM_SYSCHAR) return 0;
        return DefSubclassProc(control,msg,w,l);
    }
    static void updateSliderLabels(HWND dialog) {
        auto zoomValue=100+25*static_cast<int>(SendDlgItemMessageW(dialog,IDC_SNAPSHOT_ZOOM,TBM_GETPOS,0,0));
        auto widthValue=4+2*static_cast<int>(SendDlgItemMessageW(dialog,IDC_DEFAULT_WIDTH,TBM_GETPOS,0,0));
        SetDlgItemTextW(dialog,IDC_SNAPSHOT_ZOOM_VALUE,(std::to_wstring(zoomValue)+L"%").c_str());
        SetDlgItemTextW(dialog,IDC_DEFAULT_WIDTH_VALUE,(std::to_wstring(widthValue)+L" px").c_str());
    }
    static INT_PTR CALLBACK settingsProcedure(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
        auto app=reinterpret_cast<App*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) {
            SendMessageW(dialog,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(appIcon(false)));
            SendMessageW(dialog,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(appIcon(true)));
            app=reinterpret_cast<App*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); app->settingsWindow=dialog;
            auto themeLibrary=LoadLibraryW(L"uxtheme.dll");
            if (themeLibrary) {
                using SetTheme=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);
                auto setTheme=reinterpret_cast<SetTheme>(GetProcAddress(themeLibrary,"SetWindowTheme"));
                if (setTheme) setTheme(GetDlgItem(dialog,IDC_SETTINGS_TABS),L"",L"");
                FreeLibrary(themeLibrary);
            }
            SetDlgItemTextW(dialog,IDC_ABOUT_TEXT,L"myZoomit - " MYZOOMIT_VERSION_W L"\r\n轻量屏幕标注工具");
            int coffeeSize=MulDiv(96,static_cast<int>(GetDpiForWindow(dialog)),96);
            SetWindowPos(GetDlgItem(dialog,IDC_COFFEE_IMAGE),nullptr,0,0,coffeeSize,coffeeSize,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            hotkeyEdit=GetDlgItem(dialog,IDC_HOTKEY);
            for (auto name:{L"缩放",L"标注",L"倒计时",L"关于"}) {
                TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(name);
                TabCtrl_InsertItem(GetDlgItem(dialog,IDC_SETTINGS_TABS),TabCtrl_GetItemCount(GetDlgItem(dialog,IDC_SETTINGS_TABS)),&item);
            }
            TabCtrl_SetCurSel(GetDlgItem(dialog,IDC_SETTINGS_TABS),app->settingsPage);
            selectSettingsTab(dialog,app->settingsPage);
            BYTE timerFlags=0;
            if (app->timer.modifiers&MOD_CONTROL) timerFlags|=HOTKEYF_CONTROL;
            if (app->timer.modifiers&MOD_ALT) timerFlags|=HOTKEYF_ALT;
            if (app->timer.modifiers&MOD_SHIFT) timerFlags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_SETHOTKEY,MAKEWORD(app->timer.key,timerFlags),0);
            SetWindowSubclass(GetDlgItem(dialog,IDC_TIMER_HOTKEY),hotkeyProcedure,1,0);
            SetDlgItemInt(dialog,IDC_TIMER_MINUTES,app->timer.minutes,FALSE);
            for (auto name:{L"环形倒计时",L"条形倒计时"}) SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_SETCURSEL,app->timer.style,0);
            for (auto name:{L"全屏",L"透明浮窗"}) SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_SETCURSEL,app->timer.defaultFull ? 0 : 1,0);
            app->runAtStartup=app->startupEnabled();
            CheckDlgButton(dialog,IDC_SHOW_HINT,app->showHint ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog,IDC_STARTUP,app->runAtStartup ? BST_CHECKED : BST_UNCHECKED);
            BOOL emphasis{};
            if (SystemParametersInfoW(SPI_GETMOUSESONAR,0,&emphasis,0))
                CheckDlgButton(dialog,IDC_MOUSE_EMPHASIS,emphasis ? BST_CHECKED : BST_UNCHECKED);
            else {
                EnableWindow(GetDlgItem(dialog,IDC_MOUSE_EMPHASIS),FALSE);
                SetDlgItemTextW(dialog,IDC_MOUSE_EMPHASIS,L"鼠标强调（无法读取 Windows 系统设置）");
            }
            BYTE flags=0;
            if (app->hotkeyModifiers & MOD_CONTROL) flags|=HOTKEYF_CONTROL;
            if (app->hotkeyModifiers & MOD_ALT) flags|=HOTKEYF_ALT;
            if (app->hotkeyModifiers & MOD_SHIFT) flags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,MAKEWORD(app->hotkeyKey,flags),0);
            SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETRULES,0,0);
            SetWindowSubclass(GetDlgItem(dialog,IDC_HOTKEY),hotkeyProcedure,1,0);
            BYTE snapshotFlags=0;
            if (app->snapshotMods & MOD_CONTROL) snapshotFlags|=HOTKEYF_CONTROL;
            if (app->snapshotMods & MOD_ALT) snapshotFlags|=HOTKEYF_ALT;
            if (app->snapshotMods & MOD_SHIFT) snapshotFlags|=HOTKEYF_SHIFT;
            SendDlgItemMessageW(dialog,IDC_SNAPSHOT_HOTKEY,HKM_SETHOTKEY,MAKEWORD(app->snapshotKey,snapshotFlags),0);
            SetWindowSubclass(GetDlgItem(dialog,IDC_SNAPSHOT_HOTKEY),hotkeyProcedure,1,0);
            SendDlgItemMessageW(dialog,IDC_SNAPSHOT_ZOOM,TBM_SETRANGE,TRUE,MAKELPARAM(0,12));
            SendDlgItemMessageW(dialog,IDC_DEFAULT_WIDTH,TBM_SETRANGE,TRUE,MAKELPARAM(0,10));
            for (int id:{IDC_SNAPSHOT_ZOOM,IDC_DEFAULT_WIDTH}) {
                SendDlgItemMessageW(dialog,id,TBM_SETPAGESIZE,0,1);
                SendDlgItemMessageW(dialog,id,TBM_SETTICFREQ,1,0);
            }
            SendDlgItemMessageW(dialog,IDC_SNAPSHOT_ZOOM,TBM_SETPOS,TRUE,(app->initialZoom-100)/25);
            SendDlgItemMessageW(dialog,IDC_DEFAULT_WIDTH,TBM_SETPOS,TRUE,(app->defaultPenWidth-4)/2);
            updateSliderLabels(dialog);
            PostMessageW(dialog,SettingsReadyMessage,0,0);
            return TRUE;
        }
        if (!app) return FALSE;
        if (aboutContentProcedure(dialog,msg,w,l)) return TRUE;
        if (msg==WM_NOTIFY && reinterpret_cast<NMHDR*>(l)->idFrom==IDC_SETTINGS_TABS && reinterpret_cast<NMHDR*>(l)->code==TCN_SELCHANGE) {
            app->settingsPage=TabCtrl_GetCurSel(GetDlgItem(dialog,IDC_SETTINGS_TABS));
            selectSettingsTab(dialog,app->settingsPage);return TRUE;
        }
        if (msg==WM_HSCROLL) { updateSliderLabels(dialog); return TRUE; }
        if (msg==SettingsReadyMessage) {
            ShowWindow(dialog,SW_SHOWNORMAL);
            alignSettingsControls(dialog);
            SetForegroundWindow(dialog);
            SetFocus(GetDlgItem(dialog,app->settingsPage==2 ? IDC_TIMER_HOTKEY : app->settingsPage==1 ? IDC_HOTKEY : IDC_SNAPSHOT_HOTKEY));
            return TRUE;
        }
        if (msg==WM_COMMAND) {
            if (LOWORD(w)==IDC_TIMER_DELETE) { SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_SETHOTKEY,0,0); return TRUE; }
            if (LOWORD(w)==IDC_SNAPSHOT_DELETE) { SendDlgItemMessageW(dialog,IDC_SNAPSHOT_HOTKEY,HKM_SETHOTKEY,0,0); return TRUE; }
            if (LOWORD(w)==IDC_DELETE_HOTKEY) {
                SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_SETHOTKEY,0,0);
                SetFocus(GetDlgItem(dialog,IDC_HOTKEY)); return TRUE;
            }
            if (LOWORD(w)==IDOK) {
                auto value=SendDlgItemMessageW(dialog,IDC_HOTKEY,HKM_GETHOTKEY,0,0);
                UINT mods=0; auto flags=HIBYTE(value);
                if (flags & HOTKEYF_CONTROL) mods|=MOD_CONTROL;
                if (flags & HOTKEYF_ALT) mods|=MOD_ALT;
                if (flags & HOTKEYF_SHIFT) mods|=MOD_SHIFT;
                auto sv=SendDlgItemMessageW(dialog,IDC_SNAPSHOT_HOTKEY,HKM_GETHOTKEY,0,0);
                UINT sm=0; auto sf=HIBYTE(sv); UINT sk=LOBYTE(sv);
                if (sf & HOTKEYF_CONTROL) sm|=MOD_CONTROL;
                if (sf & HOTKEYF_ALT) sm|=MOD_ALT;
                if (sf & HOTKEYF_SHIFT) sm|=MOD_SHIFT;
                int sz=100+25*static_cast<int>(SendDlgItemMessageW(dialog,IDC_SNAPSHOT_ZOOM,TBM_GETPOS,0,0));
                int width=4+2*static_cast<int>(SendDlgItemMessageW(dialog,IDC_DEFAULT_WIDTH,TBM_GETPOS,0,0));
                auto tv=SendDlgItemMessageW(dialog,IDC_TIMER_HOTKEY,HKM_GETHOTKEY,0,0);
                UINT tm=0,tk=LOBYTE(tv);auto tf=HIBYTE(tv);
                if (tf&HOTKEYF_CONTROL) tm|=MOD_CONTROL;
                if (tf&HOTKEYF_ALT) tm|=MOD_ALT;
                if (tf&HOTKEYF_SHIFT) tm|=MOD_SHIFT;
                BOOL validMinutes{};auto minutes=GetDlgItemInt(dialog,IDC_TIMER_MINUTES,&validMinutes,FALSE);
                if (!validShortcut(mods,LOBYTE(value)) || !validShortcut(sm,sk) || !validShortcut(tm,tk) ||
                    (sk && sk==LOBYTE(value) && sm==mods) || (tk && tk==sk && tm==sm) || (tk && tk==LOBYTE(value) && tm==mods)) {
                    MessageBoxW(dialog,L"请为三个模式设置互不重复的 Ctrl / Alt 组合快捷键。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING);return TRUE;
                }
                if (!validMinutes || minutes<1 || minutes>1440) {
                    TabCtrl_SetCurSel(GetDlgItem(dialog,IDC_SETTINGS_TABS),2);app->settingsPage=2;selectSettingsTab(dialog,2);
                    MessageBoxW(dialog,L"倒计时默认时长须为 1–1440 分钟。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING);SetFocus(GetDlgItem(dialog,IDC_TIMER_MINUTES));return TRUE;
                }
                // Release bindings left by an earlier failed save before validating all three.
                if (app->hotkey) { UnregisterHotKey(app->window,app->hotkeyId);app->hotkey=false; }
                if (app->snapshotHotkey) { UnregisterHotKey(app->window,3);app->snapshotHotkey=false; }
                if (app->timerHotkey) { UnregisterHotKey(app->window,4);app->timerHotkey=false; }
                if (tk && !RegisterHotKey(app->window,4,tm|MOD_NOREPEAT,tk)) {
                    MessageBoxW(dialog,L"倒计时快捷键已被其他程序占用。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING);return TRUE;
                }
                app->timerHotkey=tk!=0;
                if (sk && !RegisterHotKey(app->window,3,sm|MOD_NOREPEAT,sk)) {
                    UnregisterHotKey(app->window,4);app->timerHotkey=false;
                    MessageBoxW(dialog,L"截图快捷键已被其他程序占用。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING); return TRUE;
                }
                app->snapshotHotkey=sk!=0;
                // Keep a failed multi-page save from applying only some of the pages.
                auto oldHotMods=app->hotkeyModifiers,oldHotKey=app->hotkeyKey;
                auto oldSnapMods=app->snapshotMods,oldSnapKey=app->snapshotKey;
                int oldZoom=app->initialZoom,oldWidth=app->defaultPenWidth;
                float oldPenWidth=app->penWidth;bool oldHint=app->showHint,oldStartup=app->runAtStartup;
                auto oldTimerMods=app->timer.modifiers,oldTimerKey=app->timer.key;
                int oldTimerMinutes=app->timer.minutes,oldTimerStyle=app->timer.style;bool oldTimerFull=app->timer.defaultFull;
                BOOL oldEmphasis{};bool haveEmphasis=SystemParametersInfoW(SPI_GETMOUSESONAR,0,&oldEmphasis,0)!=FALSE;
                std::vector<std::pair<std::wstring,std::wstring>> sections;
                for (auto name:{L"Hotkey",L"UI",L"Pen",L"Snapshot",L"Timer"}) {
                    std::wstring contents(32768,L'\0');GetPrivateProfileSectionW(name,contents.data(),static_cast<DWORD>(contents.size()),app->settingsPath.c_str());
                    sections.emplace_back(name,std::move(contents));
                }
                bool committed=false;
                struct Rollback { std::function<void()> action;~Rollback() { action(); } } rollback{[&] {
                    if (committed) return;
                    UnregisterHotKey(app->window,app->hotkeyId);UnregisterHotKey(app->window,3);UnregisterHotKey(app->window,4);
                    app->hotkey=app->snapshotHotkey=app->timerHotkey=false;
                    app->hotkeyModifiers=oldHotMods;app->hotkeyKey=oldHotKey;app->snapshotMods=oldSnapMods;app->snapshotKey=oldSnapKey;
                    app->initialZoom=oldZoom;app->defaultPenWidth=oldWidth;app->showHint=oldHint;app->runAtStartup=oldStartup;
                    app->timer.modifiers=oldTimerMods;app->timer.key=oldTimerKey;app->timer.minutes=oldTimerMinutes;app->timer.style=oldTimerStyle;app->timer.defaultFull=oldTimerFull;
                    app->updateWidth(oldPenWidth);
                    for (auto const& section:sections) WritePrivateProfileSectionW(section.first.c_str(),section.second.c_str(),app->settingsPath.c_str());
                    std::wstring restoreError;if (app->startupEnabled()!=oldStartup) app->setStartup(oldStartup,restoreError);
                    if (haveEmphasis) { BOOL current{};if (SystemParametersInfoW(SPI_GETMOUSESONAR,0,&current,0) && current!=oldEmphasis) app->setMouseEmphasis(oldEmphasis!=FALSE); }
                    app->timer.refresh();
                }};
                std::wstring error;
                if (!app->applySettings(mods,LOBYTE(value),IsDlgButtonChecked(dialog,IDC_SHOW_HINT)==BST_CHECKED,
                    IsDlgButtonChecked(dialog,IDC_STARTUP)==BST_CHECKED,
                    IsWindowEnabled(GetDlgItem(dialog,IDC_MOUSE_EMPHASIS)) ?
                        (IsDlgButtonChecked(dialog,IDC_MOUSE_EMPHASIS)==BST_CHECKED ? 1 : 0) : -1,error)) {
                    UnregisterHotKey(app->window,4);app->timerHotkey=false;
                    UnregisterHotKey(app->window,3); app->snapshotHotkey=false;
                    MessageBoxW(dialog,error.c_str(),L"MyZoomIt 设置",MB_OK|MB_ICONWARNING); return TRUE;
                }
                std::wstring section=L"Modifiers="+std::to_wstring(sm)+L'\0'+L"Key="+std::to_wstring(sk)+L'\0'+L"Zoom="+std::to_wstring(sz)+L'\0';
                if (!WritePrivateProfileSectionW(L"Snapshot",section.c_str(),app->settingsPath.c_str())) {
                    UnregisterHotKey(app->window,4);app->timerHotkey=false;
                    UnregisterHotKey(app->window,3); app->snapshotHotkey=false;
                    MessageBoxW(dialog,L"无法保存截图设置。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING); return TRUE;
                }
                app->snapshotMods=sm; app->snapshotKey=sk; app->initialZoom=static_cast<int>(sz);
                app->defaultPenWidth=width; app->updateWidth(static_cast<float>(width));
                if (!app->saveBrushSettings()) {
                    MessageBoxW(dialog,L"无法保存默认笔宽设置。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING); return TRUE;
                }
                auto oldMinutes=app->timer.minutes,oldStyle=app->timer.style;auto oldMods=app->timer.modifiers,oldKey=app->timer.key;bool oldFull=app->timer.defaultFull;
                app->timer.modifiers=tm;app->timer.key=tk;app->timer.minutes=static_cast<int>(minutes);
                app->timer.style=static_cast<int>(SendDlgItemMessageW(dialog,IDC_TIMER_STYLE,CB_GETCURSEL,0,0));
                app->timer.defaultFull=SendDlgItemMessageW(dialog,IDC_TIMER_SHAPE,CB_GETCURSEL,0,0)==0;
                if (!app->timer.save()) {
                    app->timer.modifiers=oldMods;app->timer.key=oldKey;app->timer.minutes=oldMinutes;app->timer.style=oldStyle;app->timer.defaultFull=oldFull;
                    UnregisterHotKey(app->window,4);app->timerHotkey=false;
                    MessageBoxW(dialog,L"无法保存倒计时设置。",L"MyZoomIt 设置",MB_OK|MB_ICONWARNING);return TRUE;
                }
                if (!app->timer.state.started) app->timer.state.remaining=app->timer.state.duration=static_cast<int64_t>(minutes)*60000;
                app->timer.refresh();
                committed=true;
                EndDialog(dialog,IDOK); return TRUE;
            }
            if (LOWORD(w)==IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
        }
        if (msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        if (msg==WM_DESTROY) { app->settingsWindow=nullptr; hotkeyEdit=nullptr; }
        return FALSE;
    }
    void settings(int page=0) {
        settingsPage=page;
        if (settingsWindow && IsWindow(settingsWindow)) {
            TabCtrl_SetCurSel(GetDlgItem(settingsWindow,IDC_SETTINGS_TABS),page);selectSettingsTab(settingsWindow,page);
            ShowWindow(settingsWindow,SW_RESTORE); SetForegroundWindow(settingsWindow); return;
        }
        leave();
        // Suspend the binding so the hotkey editor receives the currently registered combination.
        if (hotkey) { UnregisterHotKey(window,hotkeyId); hotkey=false; }
        if (snapshotHotkey) { UnregisterHotKey(window,3); snapshotHotkey=false; }
        if (timerHotkey) { UnregisterHotKey(window,4);timerHotkey=false; }
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
        if (!snapshotHotkey && snapshotKey) snapshotHotkey=RegisterHotKey(window,3,snapshotMods|MOD_NOREPEAT,snapshotKey)!=FALSE;
        if (!timerHotkey && timer.key) timerHotkey=RegisterHotKey(window,4,timer.modifiers|MOD_NOREPEAT,timer.key)!=FALSE;
        if (result==-1) MessageBoxW(nullptr,L"无法打开设置窗口。",L"MyZoomIt",MB_OK|MB_ICONERROR);
    }

    void updateHint() {
        hintText.Text(std::wstring(L"标注中  ·  ")+(eraserMode ? L"橡皮擦" : toolName())+L"  ·  "+Palette[colorIndex].name+L"  ·  Ctrl+滚轮 粗细 "+
            std::to_wstring(static_cast<int>(penWidth))+L"  ·  Ctrl+Z 撤销  ·  Ctrl+Y 恢复  ·  E：橡皮擦  ·  C 清空  ·  K 黑板  ·  Esc 返回\n按住 Ctrl 矩形 / Tab 椭圆 / Shift 直线 / Ctrl+Shift 箭头");
        if (snapshotMode) hintText.Text(browsing ?
            L"截图浏览 · "+std::to_wstring(static_cast<int>(zoom*100))+L"% · 移动鼠标浏览 · 滚轮缩放 · 点击或触控标注 · Esc 退出并复制" :
            std::wstring(hintText.Text())+L"\n截图标注：右键返回浏览（保留笔迹）");
        if (hintPanel) hintPanel.Visibility(showHint ? Visibility::Visible : Visibility::Collapsed);
    }
    bool setColor(WORD keyCode) {
        for (size_t i=0;i<Palette.size();++i) {
            if (Palette[i].key!=keyCode) continue;
            if (eraserMode) { finishErase(); eraserMode=false; cursorTouch=false; }
            colorIndex=i;
            auto attributes=presenter.CopyDefaultDrawingAttributes();
            attributes.Color(Palette[i].value);
            presenter.UpdateDefaultDrawingAttributes(attributes);
            updateHint(); if (active) finishCursorAnimation(); return true;
        }
        return false;
    }

    void updateWidth(float width) {
        penWidth=std::clamp(width,4.0f,24.0f);
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
    void clear(bool resetHistory=true) {
        cancelFreehand(); finishErase();
        if (!resetHistory && !drawingOrder.empty()) recordEdit({false,drawingOrder});
        drawingOrder.clear();
        if (resetHistory) { undoHistory.clear(); redoHistory.clear(); }
        if (freehandSurface) freehandSurface.Children().Clear();
        cancelShape();
        if (shapeSurface && tool!=requestedTool) setTool(requestedTool);
        if (presenter) presenter.StrokeContainer().Clear();
    }
    void undo() {
        if (drawingFreehand) { cancelFreehand(); return; }
        if (drawingShape) { cancelShape(); if (tool!=requestedTool) setTool(requestedTool); return; }
        finishErase();
        if (undoHistory.empty()) return;
        auto edit=undoHistory.back(); applyEdit(edit,!edit.added);
        undoHistory.pop_back(); redoHistory.push_back(std::move(edit));
    }
    void redo() {
        if (drawingFreehand || drawingShape) return;
        finishErase();
        if (redoHistory.empty()) return;
        auto edit=redoHistory.back(); applyEdit(edit,edit.added);
        redoHistory.pop_back(); undoHistory.push_back(std::move(edit));
    }
    static bool fullscreenOnMonitor(HWND foreground, HMONITOR target, const RECT& screen) {
        if (!IsWindow(foreground) || !IsWindowVisible(foreground) || IsIconic(foreground)) return false;
        auto root=GetAncestor(foreground,GA_ROOT);
        if (!root || MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST)!=target) return false;
        wchar_t name[128]{}; GetClassNameW(root,name,128);
        // The desktop also fills a monitor, but must preserve normal taskbar behavior.
        if (wcscmp(name,L"Progman")==0 || wcscmp(name,L"WorkerW")==0 ||
            wcscmp(name,L"Shell_TrayWnd")==0 || wcscmp(name,L"Shell_SecondaryTrayWnd")==0) return false;
        auto covers=[&](const RECT& bounds) {
            constexpr int tolerance=2;
            return bounds.left<=screen.left+tolerance && bounds.top<=screen.top+tolerance &&
                bounds.right>=screen.right-tolerance && bounds.bottom>=screen.bottom-tolerance;
        };
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(root,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))) &&
            !GetWindowRect(root,&bounds)) return false;
        if (!covers(bounds)) return false;
        // Client coverage distinguishes F11 / slide shows from maximized windows,
        // including maximized windows when the taskbar is set to auto-hide.
        RECT client{};
        if (!GetClientRect(root,&client)) return false;
        POINT origin{client.left,client.top}, end{client.right,client.bottom};
        if (!ClientToScreen(root,&origin) || !ClientToScreen(root,&end)) return false;
        return covers(RECT{origin.x,origin.y,end.x,end.y});
    }
    void normalTaskbarPolicy() {
        if (!SetPropW(window,L"NonRudeHWND",reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1))))
            throw_last_error();
    }
    void leave(bool restore = true) {
        if (!active || changing) return;
        changing = true;
        stopZoomAnimation();
        copyFinalScreen();
        cursorAnimating=false; KillTimer(window,CursorTimer);
        if (cursorSurface) cursorSurface.Visibility(Visibility::Collapsed);
        restoreSystemCursor();
        presenter.IsInputEnabled(false);
        ShowWindow(window, SW_HIDE);
        normalTaskbarPolicy();
        active = false;
        resetActivationSurface(false);
        snapshotMode=false; browsing=false; screenshot.Source(nullptr); screenshot.Visibility(Visibility::Collapsed);
        viewport.Background(SolidColorBrush(Color{0,0,0,0}));
        zoom=1; viewX=viewY=0; updateView();
        blackboard.Visibility(Visibility::Collapsed);
        clear(); eraserMode=false; cursorTouch=false;
        cursorSuppressedByContact=false;
        if (heldTool) { heldTool=false; setTool(Tool::Pen); }
        saveBrushSettings();
        if (restore && IsWindow(previous)) SetForegroundWindow(previous);
        changing = false;
    }
    static double GetDpiForMonitorScale(RECT r) {
        auto lib=LoadLibraryW(L"shcore.dll"); UINT x=96,y=96;
        if (lib) {
            using Fn=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);
            auto fn=reinterpret_cast<Fn>(GetProcAddress(lib,"GetDpiForMonitor"));
            if (fn) fn(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),0,&x,&y);
            FreeLibrary(lib);
        }
        return x/96.0;
    }
    void enter(bool snapshot=false) {
        if (timer.isFullscreen()) timer.hide();
        if (settingsWindow) { SetForegroundWindow(settingsWindow); return; }
        if (aboutWindow) { SetForegroundWindow(aboutWindow); return; }
        if (changing) return;
        if (active) { leave(); return; }
        changing = true;
        updateWidth(static_cast<float>(defaultPenWidth)); zoomWheelRemainder=0;
        cursorSuppressedByContact=false;
        previous = GetForegroundWindow();
        POINT point{}; GetCursorPos(&point);
        auto next = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        if (next != monitor) clear();
        monitor = next;
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) { changing = false; return; }
        auto r = info.rcMonitor;
        try {
            // Decide before activating the overlay: activation changes the Shell's
            // fullscreen assessment of the previous application.
            if (fullscreenOnMonitor(previous,monitor,r)) RemovePropW(window,L"NonRudeHWND");
            else normalTaskbarPolicy();
            snapshotMode=snapshot; browsing=snapshot;
            viewport.Background(SolidColorBrush(Color{static_cast<BYTE>(snapshot ? 255 : 0),0,0,0}));
            zoom=snapshot ? initialZoom/100.0 : 1.0; viewX=viewY=0;
            double dpi=GetDpiForMonitorScale(r);
            sceneWidth=(r.right-r.left)/dpi; sceneHeight=(r.bottom-r.top)/dpi;
            scene.Width(snapshot ? sceneWidth : NAN); scene.Height(snapshot ? sceneHeight : NAN);
            if (snapshot) {
                auto bitmap=captureScreen(r);
                if (!bitmap) throw hresult_error(E_FAIL,L"无法捕获屏幕");
                Windows::UI::Xaml::Media::Imaging::WriteableBitmap image(r.right-r.left,r.bottom-r.top);
                try {
                    auto buffer=image.PixelBuffer(); auto access=buffer.as<PixelBufferAccess>();
                    BYTE* bytes{}; check_hresult(access->Buffer(&bytes));
                    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth=r.right-r.left; bi.bmiHeader.biHeight=-(r.bottom-r.top);
                    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
                    auto dc=GetDC(nullptr);
                    int rows=GetDIBits(dc,bitmap,0,r.bottom-r.top,bytes,&bi,DIB_RGB_COLORS); ReleaseDC(nullptr,dc);
                    if (rows!=r.bottom-r.top) throw hresult_error(E_FAIL);
                    for (uint32_t i=3;i<buffer.Length();i+=4) bytes[i]=255;
                    image.Invalidate(); screenshot.Source(image);
                } catch (...) { DeleteObject(bitmap); throw; }
                DeleteObject(bitmap); screenshot.Visibility(Visibility::Visible);
            }
            updateView();
            blackboard.Visibility(Visibility::Collapsed);
            SetWindowPos(window, HWND_TOPMOST, r.left, r.top, r.right-r.left, r.bottom-r.top, 0);
            presenter.IsInputEnabled(tool==Tool::Pen);
            active = true;
            updateHint(); if (!snapshot) syncHeldTool();
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            SetFocus(island);
            hideSystemCursor();
            cursorAnimationStart=200; cursorStarted=GetTickCount64(); cursorAnimating=true;
            animateCursor(); SetTimer(window,CursorTimer,16,nullptr);
            if (snapshot) {
                browseMove(true); returnToBrowse();
                POINT anchor{}; GetCursorPos(&anchor); ScreenToClient(island,&anchor);
                double actualDpi=GetDpiForWindow(island)/96.0;
                zoomAnchorX=anchor.x/actualDpi; zoomAnchorY=anchor.y/actualDpi;
                zoomTarget=zoom;
                if (zoomTarget>1) {
                    zoom=1; viewX=viewY=0; updateView();
                    zoomStarted=GetTickCount64(); zoomAnimating=true;
                    SetTimer(window,ZoomTimer,16,nullptr);
                }
            }
            timer.raiseFloating();
            changing = false;
        } catch (...) {
            changing = false;
            ShowWindow(window,SW_HIDE); active=false; restoreSystemCursor();
            normalTaskbarPolicy();
            throw;
        }
    }
    static INT_PTR aboutContentProcedure(HWND dialog,UINT msg,WPARAM w,LPARAM l) {
        if (msg==WM_DRAWITEM && w==IDC_COFFEE_IMAGE) {
            auto item=reinterpret_cast<DRAWITEMSTRUCT*>(l);
            auto bitmap=static_cast<HBITMAP>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDB_COFFEE),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION));
            if (bitmap) {
                BITMAP info{}; GetObjectW(bitmap,sizeof(info),&info);
                auto memory=CreateCompatibleDC(item->hDC); auto old=SelectObject(memory,bitmap);
                int mode=SetStretchBltMode(item->hDC,COLORONCOLOR);
                StretchBlt(item->hDC,0,0,item->rcItem.right,item->rcItem.bottom,memory,0,0,info.bmWidth,info.bmHeight,SRCCOPY);
                SetStretchBltMode(item->hDC,mode); SelectObject(memory,old); DeleteDC(memory); DeleteObject(bitmap);
            }
            return TRUE;
        }
        if (msg==WM_NOTIFY) {
            auto notification=reinterpret_cast<NMHDR*>(l);
            if (notification->code==NM_CLICK || notification->code==NM_RETURN) {
                const wchar_t* target=notification->idFrom==IDC_PROJECT_LINK ? L"https://github.com/Algustav/myZoomit" :
                    notification->idFrom==IDC_CREDITS_LINK ? L"https://github.com/steveruizok/perfect-freehand" :
                    notification->idFrom==IDC_CONTACT_LINK ? L"mailto:mail@ganlei.com" : nullptr;
                if (!target) return FALSE;
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(dialog,L"open",target,nullptr,nullptr,SW_SHOWNORMAL))<=32)
                    MessageBoxW(dialog,L"无法打开链接，请检查默认浏览器或邮件应用。",L"myZoomit",MB_OK|MB_ICONWARNING);
                return TRUE;
            }
        }
        return FALSE;
    }
    static INT_PTR CALLBACK aboutProcedure(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
        auto app=reinterpret_cast<App*>(GetWindowLongPtrW(dialog,DWLP_USER));
        if (msg==WM_INITDIALOG) {
            app=reinterpret_cast<App*>(l); SetWindowLongPtrW(dialog,DWLP_USER,l); app->aboutWindow=dialog;
            auto image=GetDlgItem(dialog,IDC_COFFEE_IMAGE);
            RECT imageRect{}; GetWindowRect(image,&imageRect);
            int size=MulDiv(96,static_cast<int>(GetDpiForWindow(dialog)),96);
            int delta=size-(imageRect.bottom-imageRect.top);
            SetWindowPos(image,nullptr,0,0,size,size,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            for (int id:{IDC_COFFEE_CAPTION,IDC_ABOUT_AUTHOR,IDC_ABOUT_GROUP,IDOK}) {
                auto control=GetDlgItem(dialog,id); RECT rect{}; GetWindowRect(control,&rect);
                MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&rect),2);
                if (id==IDC_ABOUT_GROUP)
                    SetWindowPos(control,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top+delta,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                else SetWindowPos(control,nullptr,rect.left,rect.top+delta,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            RECT bounds{}; GetWindowRect(dialog,&bounds);
            SetWindowPos(dialog,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top+delta,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(dialog,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(appIcon(false)));
            SendMessageW(dialog,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(appIcon(true)));
            SetDlgItemTextW(dialog,IDC_ABOUT_TEXT,
                L"myZoomit - " MYZOOMIT_VERSION_W L"\r\n轻量屏幕标注工具");
            PostMessageW(dialog,SettingsReadyMessage,0,0); return TRUE;
        }
        if (!app) return FALSE;
        if (aboutContentProcedure(dialog,msg,w,l)) return TRUE;
        if (msg==SettingsReadyMessage) {
            ShowWindow(dialog,SW_SHOWNORMAL); SetForegroundWindow(dialog); SetFocus(GetDlgItem(dialog,IDOK)); return TRUE;
        }
        if (msg==WM_COMMAND && (LOWORD(w)==IDOK || LOWORD(w)==IDCANCEL)) { EndDialog(dialog,LOWORD(w)); return TRUE; }
        if (msg==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
        if (msg==WM_DESTROY) app->aboutWindow=nullptr;
        return FALSE;
    }
    void about() {
        if (aboutWindow) { SetForegroundWindow(aboutWindow); return; }
        leave();
        if (DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_ABOUT),nullptr,aboutProcedure,reinterpret_cast<LPARAM>(this))==-1) {
            auto error=L"无法打开关于窗口。错误代码："+std::to_wstring(GetLastError());
            MessageBoxW(nullptr,error.c_str(),L"myZoomit",MB_OK|MB_ICONWARNING);
        }
        aboutWindow=nullptr;
    }
    void menu() {
        auto popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, 4, L"设置…");
        AppendMenuW(popup, MF_STRING, 5, L"关于…");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 3, L"退出");
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(window);
        auto command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY,
            point.x, point.y, 0, window, nullptr);
        DestroyMenu(popup);
        PostMessageW(window, WM_NULL, 0, 0);
        if (command == 3) PostMessageW(window, WM_CLOSE, 0, 0);
        else if (command == 4) PostMessageW(window,SettingsMessage,0,0);
        else if (command == 5) PostMessageW(window,AboutMessage,0,0);
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
        case AboutMessage: about(); return 0;
        case WM_APP+7: timerSettings(); return 0;
        case WM_HOTKEY: if (timer.settingsOpen()) return 0; if (w==4 && timerHotkey && !settingsWindow && !aboutWindow) { timer.toggle(); return 0; } if (snapshotHotkey && w==3 && !settingsWindow && !aboutWindow) { enter(true); return 0; } if (hotkey && !settingsWindow && !aboutWindow && static_cast<int>(w)==hotkeyId) enter(); return 0;
        case WM_SIZE:
            if (island) SetWindowPos(island, nullptr, 0, 0, LOWORD(l), HIWORD(l), SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !timer.owns(reinterpret_cast<HWND>(l))) leave(false); return 0;
        case WM_DISPLAYCHANGE: case WM_DPICHANGED: leave(false); monitor = nullptr; clear(); return 0;
        case WM_WTSSESSION_CHANGE: if (w == WTS_SESSION_LOCK) { leave(false); timer.hide(); } return 0;
        case TrayMessage:
            if (l == WM_LBUTTONUP || l == WM_RBUTTONUP) menu();
            return 0;
        case WM_SETCURSOR: if (active && !browsing) { SetCursor(nullptr); return TRUE; } break;
        case WM_TIMER: if (w==CursorTimer) animateCursor(); else if (w==ZoomTimer) animateZoom(); return 0;
        case WM_CLOSE: timer.hide(); saveBrushSettings(); if (settingsWindow) EndDialog(settingsWindow,IDCANCEL); if (aboutWindow) EndDialog(aboutWindow,IDCANCEL); leave(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window,CursorTimer);
            KillTimer(window,ZoomTimer);
            restoreSystemCursor();
            UnregisterHotKey(window, hotkeyId); UnregisterHotKey(window,3); UnregisterHotKey(window,4);
            WTSUnRegisterSessionNotification(window);
            { NOTIFYICONDATAW icon{sizeof(icon)}; icon.hWnd=window; icon.uID=1; Shell_NotifyIconW(NIM_DELETE, &icon); }
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }
public:
    int run(HINSTANCE instance) {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS|ICC_LINK_CLASS|ICC_BAR_CLASSES|ICC_TAB_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        loadSettings();
        WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=instance;
        wc.hIcon=appIcon(false);
        wc.lpszClassName=ClassName; wc.hCursor=nullptr;
        if (!RegisterClassW(&wc)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        window=CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            ClassName, L"MyZoomIt", WS_POPUP, 0,0,800,600,nullptr,nullptr,instance,this);
        if (!window) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        // This screen-sized annotation overlay must not trigger fullscreen taskbar behavior.
        if (!SetPropW(window,L"NonRudeHWND",reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1))))
            throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
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
        scene=root; viewTransform=CompositeTransform(); scene.RenderTransform(viewTransform);
        viewport=Grid(); viewport.Background(SolidColorBrush(Color{0,0,0,0}));
        viewport.Children().Append(scene);
        // A separate hit-test surface keeps the activation contact away from
        // Ink's independent input thread, including the entire initial drag.
        activationSurface=Grid();
        activationSurface.Background(SolidColorBrush(Color{0,0,0,0}));
        activationSurface.Visibility(Visibility::Collapsed);
        viewport.Children().Append(activationSurface);
        activationSurface.PointerPressed([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            if (!active || !snapshotMode) return;
            args.Handled(true);
            auto point=args.GetCurrentPoint(activationSurface);
            bool mouse=point.PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Mouse;
            if (point.Properties().IsRightButtonPressed() ||
                (mouse ? !point.Properties().IsLeftButtonPressed() : !point.IsInContact())) return;
            auto id=args.Pointer().PointerId();
            if (std::find(activationPointers.begin(),activationPointers.end(),id)==activationPointers.end())
                activationPointers.push_back(id);
            activationSurface.CapturePointer(args.Pointer());
            observeCursorInput(point.PointerDevice().PointerDeviceType(),point.IsInContact());
            beginAnnotation();
        });
        activationSurface.PointerMoved([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            auto point=args.GetCurrentPoint(activationSurface);
            observeCursorInput(point.PointerDevice().PointerDeviceType(),point.IsInContact());
            if (active && snapshotMode && browsing && args.GetCurrentPoint(activationSurface).PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Mouse)
                browseMove();
            args.Handled(true);
        });
        activationSurface.PointerWheelChanged([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            if (active && snapshotMode && browsing) browseWheel(args.GetCurrentPoint(activationSurface).Properties().MouseWheelDelta());
            args.Handled(true);
        });
        activationSurface.PointerReleased([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            args.Handled(true); releaseActivationPointer(args.Pointer().PointerId());
            activationSurface.ReleasePointerCapture(args.Pointer());
        });
        activationSurface.PointerCanceled([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            releaseActivationPointer(args.Pointer().PointerId());
        });
        activationSurface.PointerCaptureLost([this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
            releaseActivationPointer(args.Pointer().PointerId());
        });
        screenshot=Image(); screenshot.Stretch(Stretch::Fill); screenshot.IsHitTestVisible(false);
        screenshot.Visibility(Visibility::Collapsed); root.Children().Append(screenshot);
        blackboard=Border();
        blackboard.Background(SolidColorBrush(Color{255,0,0,0}));
        blackboard.IsHitTestVisible(false);
        blackboard.Visibility(Visibility::Collapsed);
        root.Children().Append(blackboard);
        canvas=InkCanvas(); presenter=canvas.InkPresenter();
        presenter.InputDeviceTypes(Windows::UI::Core::CoreInputDeviceTypes::Pen);
        InkDrawingAttributes attributes;
        attributes.Color(Palette[colorIndex].value); attributes.Size({penWidth,penWidth});
        attributes.IgnorePressure(false); attributes.FitToCurve(true);
        attributes.ModelerAttributes().UseVelocityBasedPressure(false);
        presenter.UpdateDefaultDrawingAttributes(attributes); presenter.IsInputEnabled(false);
        presenter.StrokesCollected([this](auto const&, InkStrokesCollectedEventArgs const& args) {
            if (!active) { clear(); return; }
            finishErase();
            for (auto const& stroke:args.Strokes()) rememberInk(stroke);
            presenter.StrokeContainer().Clear();
        });
        root.AddHandler(UIElement::PointerPressedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                auto cursorPoint=args.GetCurrentPoint(scene);
                observeCursorInput(cursorPoint.PointerDevice().PointerDeviceType(),cursorPoint.IsInContact());
                if (!active || !snapshotMode) return;
                auto point=args.GetCurrentPoint(scene);
                if (point.Properties().IsRightButtonPressed()) {
                    if (!browsing) returnToBrowse();
                    args.Handled(true); return;
                }
                bool mouse=point.PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Mouse;
                if ((mouse && point.Properties().IsLeftButtonPressed()) || (!mouse && point.IsInContact())) beginAnnotation();
            })),true);
        root.AddHandler(UIElement::RightTappedEvent(),box_value(XamlInput::RightTappedEventHandler(
            [this](auto const&, XamlInput::RightTappedRoutedEventArgs const& args) {
                if (!active || !snapshotMode) return;
                if (!browsing) returnToBrowse();
                args.Handled(true);
            })),true);
        initializeFreehand(root);
        // Live native pen ink sits above every completed stroke. Completed native
        // strokes move to the shared layer without replacing Ink's renderer.
        root.Children().Append(canvas);
        initializeShapes(root);
        root.AddHandler(UIElement::PointerPressedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, auto const&) { if (active && cursorAnimating) finishCursorAnimation(); })),true);
        root.AddHandler(UIElement::PointerWheelChangedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                if (active && snapshotMode && browsing) {
                    browseWheel(args.GetCurrentPoint(scene).Properties().MouseWheelDelta());
                    args.Handled(true);
                } else if (active && (args.KeyModifiers() & Windows::System::VirtualKeyModifiers::Control) != Windows::System::VirtualKeyModifiers::None) {
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
        cursorRing=Windows::UI::Xaml::Shapes::Ellipse(); cursorRing.Fill(nullptr);
        cursorRing.Stroke(SolidColorBrush(Color{255,0,0,0})); cursorRing.StrokeThickness(1);
        cursorRing.Width(EraserDiameter); cursorRing.Height(EraserDiameter); cursorRing.Visibility(Visibility::Collapsed);
        cursorSurface.Children().Append(cursorRing);
        cursorDot=Windows::UI::Xaml::Shapes::Ellipse();
        cursorSurface.Children().Append(cursorDot); root.Children().Append(cursorSurface);
        root.AddHandler(UIElement::PointerMovedEvent(),box_value(XamlInput::PointerEventHandler(
            [this](auto const&, XamlInput::PointerRoutedEventArgs const& args) {
                auto point=args.GetCurrentPoint(freehandInputRoot);
                observeCursorInput(point.PointerDevice().PointerDeviceType(),point.IsInContact());
                if (active && snapshotMode && browsing && point.PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Mouse) {
                    browseMove(); return;
                }
                if (!cursorAnimating && eraserMode && (!erasing || args.Pointer().PointerId()==eraserPointer)) {
                    cursorTouch=point.PointerDevice().PointerDeviceType()==Windows::Devices::Input::PointerDeviceType::Touch;
                    if (cursorTouch) eraserPosition=point.Position();
                    refreshCursor(penWidth);
                } else positionCursor();
            })),true);
        // Status remains in screen coordinates while the drawing scene moves.
        root.Children().RemoveAt(root.Children().Size()-2);
        viewport.Children().Append(hint);
        source.Content(viewport);
        // Ink uses an independent input thread and cursor, separate from CoreWindow's cursor.
        inkInput=Windows::UI::Input::Inking::Core::CoreInkIndependentInputSource::Create(presenter);
        inkInput.PointerEntering([this](auto const& input, auto const&) {
            // This setter must execute on Ink's input thread, not the XAML UI thread.
            input.PointerCursor(nullptr);
        });
        auto dispatcher=root.Dispatcher();
        inkInput.PointerPressing([this,dispatcher](auto const&, auto const& args) {
            auto point=args.CurrentPoint();
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::High,[this] {
                observeCursorInput(Windows::Devices::Input::PointerDeviceType::Pen,true);
                beginAnnotation();
            });
            if (!point.Properties().IsEraser()) return;
            auto position=point.Position(); auto id=point.PointerId();
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,[this,position,id] {
                if (!active || drawingFreehand || drawingShape || erasing) return;
                cursorAnimating=false; penTipEraser=true; erasing=true; eraserPointer=id;
                eraserPrevious=eraserPosition=position; refreshCursor(penWidth);
                eraseTo(position);
            });
        });
        inkInput.PointerMoving([this,dispatcher](auto const&, auto const& args) {
            auto point=args.CurrentPoint();
            if (point.IsInContact()) dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::High,[this] {
                observeCursorInput(Windows::Devices::Input::PointerDeviceType::Pen,true);
            });
            if (!point.Properties().IsEraser() || !point.IsInContact()) return;
            auto position=point.Position(); auto id=point.PointerId();
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,[this,position,id] {
                if (active && erasing && eraserPointer==id) eraseTo(position);
            });
        });
        inkInput.PointerReleasing([this,dispatcher](auto const&, auto const& args) {
            auto point=args.CurrentPoint(); auto position=point.Position(); auto id=point.PointerId();
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,[this,position,id] {
                if (active && erasing && eraserPointer==id) { eraseTo(position); finishErase(); }
            });
        });
        inkInput.PointerLost([this,dispatcher](auto const&, auto const& args) {
            auto id=args.CurrentPoint().PointerId();
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,[this,id] {
                if (erasing && eraserPointer==id) finishErase();
            });
        });
        Window::Current().CoreWindow().PointerCursor(nullptr);
        SetWindowPos(island,nullptr,0,0,800,600,SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        timer.initialize(window,settingsPath);
        timer.beforeFullscreen=[this] { leave(); };
        if (!validShortcut(timer.modifiers,timer.key)) { timer.modifiers=MOD_CONTROL; timer.key='3'; }
        timerHotkey=timer.key && RegisterHotKey(window,4,timer.modifiers|MOD_NOREPEAT,timer.key)!=FALSE;
        if (timer.key && !timerHotkey) MessageBoxW(nullptr,L"计时快捷键已被占用，请在计时设置中修改。",L"MyZoomIt",MB_OK|MB_ICONWARNING);
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated"); tray();
        hotkey=hotkeyKey && RegisterHotKey(window,hotkeyId,hotkeyModifiers | MOD_NOREPEAT,hotkeyKey) != FALSE;
        snapshotHotkey=snapshotKey && RegisterHotKey(window,3,snapshotMods|MOD_NOREPEAT,snapshotKey)!=FALSE;
        if (snapshotKey && !snapshotHotkey) MessageBoxW(nullptr,L"截图快捷键已被占用，请在设置中修改。",L"MyZoomIt",MB_OK|MB_ICONWARNING);
        WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
        if (hotkeyKey && !hotkey) MessageBoxW(nullptr,(shortcutText()+L" 已被占用。请通过托盘打开设置，修改快捷键。").c_str(),L"MyZoomIt",MB_OK | MB_ICONWARNING);
        MSG msg{};
        int status;
        while ((status=GetMessageW(&msg,nullptr,0,0)) > 0) {
            if (timer.owns(msg.hwnd)) {
                restoreSystemCursor();
                if (cursorSurface) cursorSurface.Visibility(Visibility::Collapsed);
                TranslateMessage(&msg); DispatchMessageW(&msg); continue;
            }
            bool ownInput=msg.hwnd==window || IsChild(window,msg.hwnd);
            if (active && snapshotMode && ownInput) {
                if (msg.message==WM_RBUTTONDOWN || msg.message==WM_RBUTTONUP) {
                    if (msg.message==WM_RBUTTONDOWN && !browsing) returnToBrowse();
                    continue;
                }
                if (msg.message==WM_MOUSEWHEEL && browsing) { browseWheel(GET_WHEEL_DELTA_WPARAM(msg.wParam)); continue; }
            }
            if (active && !browsing && (msg.message==WM_KEYDOWN || msg.message==WM_KEYUP) &&
                (msg.wParam==VK_CONTROL || msg.wParam==VK_SHIFT || msg.wParam==VK_TAB ||
                 msg.wParam==VK_LCONTROL || msg.wParam==VK_RCONTROL || msg.wParam==VK_LSHIFT || msg.wParam==VK_RSHIFT)) {
                syncHeldTool();
                // Tab is a drawing modifier here, so it must not move keyboard focus.
                if (msg.wParam==VK_TAB) continue;
            }
            if (active && msg.message == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL)&0x8000)) {
                wheel(GET_WHEEL_DELTA_WPARAM(msg.wParam)); continue;
            }
            if (active && browsing && msg.message==WM_KEYDOWN && msg.wParam!=VK_ESCAPE) continue;
            if (active && msg.message == WM_KEYDOWN && (msg.hwnd == window || IsChild(window,msg.hwnd))) {
                if (msg.wParam=='K' && !(GetKeyState(VK_CONTROL)&0x8000) &&
                    !(GetKeyState(VK_MENU)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000) &&
                    !(GetKeyState(VK_LWIN)&0x8000) && !(GetKeyState(VK_RWIN)&0x8000)) {
                    // A held key toggles once, rather than on every repeat message.
                    if (!(msg.lParam & (static_cast<LPARAM>(1)<<30)))
                        blackboard.Visibility(blackboard.Visibility()==Visibility::Visible ? Visibility::Collapsed : Visibility::Visible);
                    continue;
                }
                if (msg.wParam == VK_ESCAPE) {
                    // Ignore repeats so one held Escape cannot both restore the
                    // brush and immediately exit annotation mode.
                    if (!(msg.lParam & (static_cast<LPARAM>(1)<<30))) {
                        if (eraserMode && !browsing) toggleEraser(); else leave();
                    }
                    continue;
                }
                if (msg.wParam == 'Z' && (GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)) { undo(); continue; }
                if (msg.wParam == 'Y' && (GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)) { redo(); continue; }
                if (msg.wParam=='E' && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000)) {
                    if (!(msg.lParam & (static_cast<LPARAM>(1)<<30))) toggleEraser();
                    continue;
                }
                if (!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && msg.wParam>='1' && msg.wParam<='5') {
                    setTool(static_cast<Tool>(msg.wParam-'1')); continue;
                }
                if (!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && setColor(static_cast<WORD>(msg.wParam))) continue;
                if (msg.wParam == 'C') { clear(false); continue; }
            }
            if (active && !browsing && msg.message==WM_SETCURSOR && (msg.hwnd==window || IsChild(window,msg.hwnd))) {
                SetCursor(nullptr); continue;
            }
            if (active && cursorAnimating && (msg.message==WM_LBUTTONDOWN || msg.message==WM_POINTERDOWN)) finishCursorAnimation();
            BOOL handled=FALSE; check_hresult(native->PreTranslateMessage(&msg,&handled));
            if (!handled) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (active && !browsing) hideSystemCursor();
        }
        cancelFreehand(); freehandSurface=nullptr; freehandInputRoot=nullptr; drawingOrder.clear(); undoHistory.clear(); redoHistory.clear(); liveContours.reset();
        cursorDot=nullptr; cursorRing=nullptr; cursorSurface=nullptr; inkInput=nullptr; presenter=nullptr; canvas=nullptr; blackboard=nullptr; hintText=nullptr; hintPanel=nullptr; preview=nullptr; shapeSurface=nullptr; source.Content(nullptr); native=nullptr;
        screenshot=nullptr; scene=nullptr; viewTransform=nullptr; viewport=nullptr; activationSurface=nullptr;
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

