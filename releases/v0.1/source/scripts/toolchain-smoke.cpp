#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <inkpresenterdesktop.h>
#include <winrt/Windows.UI.Input.Inking.h>
#include <cstdio>

int main() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::Windows::UI::Input::Inking::InkDrawingAttributes attributes;
    attributes.IgnorePressure(false);
    IInkDesktopHost* host = nullptr;
    HRESULT result = CoCreateInstance(__uuidof(InkDesktopHost), nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&host));
    if (FAILED(result)) {
        std::printf("InkDesktopHost activation failed: 0x%08lX\n", result);
        return 1;
    }
    host->Release();
    std::puts("Windows Ink headers, linking and host activation: OK");
    return 0;
}
