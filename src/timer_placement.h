#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

inline RECT timerRelocate(RECT saved,RECT source,RECT target,UINT sourceDpi,UINT targetDpi) {
    double scale=static_cast<double>(targetDpi)/std::max(1u,sourceDpi);
    LONG width=std::min(target.right-target.left,std::max(1L,static_cast<LONG>(std::lround((saved.right-saved.left)*scale))));
    LONG height=std::min(target.bottom-target.top,std::max(1L,static_cast<LONG>(std::lround((saved.bottom-saved.top)*scale))));
    double x=static_cast<double>(saved.left-source.left)/std::max(1L,source.right-source.left-(saved.right-saved.left));
    double y=static_cast<double>(saved.top-source.top)/std::max(1L,source.bottom-source.top-(saved.bottom-saved.top));
    LONG left=target.left+static_cast<LONG>(std::lround(std::clamp(x,0.0,1.0)*(target.right-target.left-width)));
    LONG top=target.top+static_cast<LONG>(std::lround(std::clamp(y,0.0,1.0)*(target.bottom-target.top-height)));
    return {left,top,left+width,top+height};
}
inline RECT timerDragDpiRect(RECT oldRect,POINT grab,POINT cursor,LONG newWidth,LONG newHeight) {
    double x=static_cast<double>(grab.x-oldRect.left)/std::max(1L,oldRect.right-oldRect.left);
    double y=static_cast<double>(grab.y-oldRect.top)/std::max(1L,oldRect.bottom-oldRect.top);
    LONG left=cursor.x-static_cast<LONG>(std::lround(x*newWidth));
    LONG top=cursor.y-static_cast<LONG>(std::lround(y*newHeight));
    return {left,top,left+newWidth,top+newHeight};
}
