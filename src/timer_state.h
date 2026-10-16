#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

struct CountdownState {
    int64_t remaining{600000}, duration{600000};
    uint64_t anchor{};
    bool running{}, started{};
    int64_t milliseconds(uint64_t now) const {
        return remaining-(running ? static_cast<int64_t>(now-anchor) : 0);
    }
    int64_t seconds(uint64_t now) const {
        auto ms=milliseconds(now);
        return ms>0 ? (ms+999)/1000 : ms/1000;
    }
    void reset(int minutes, uint64_t now) {
        remaining=duration=static_cast<int64_t>(minutes)*60000;
        anchor=now; running=true; started=true;
    }
    void toggle(uint64_t now) {
        remaining=milliseconds(now); anchor=now; running=!running; started=true;
    }
    void adjust(int secondsDelta,uint64_t now) {
        remaining=milliseconds(now)+static_cast<int64_t>(secondsDelta)*1000;
        duration=remaining>0 ? remaining : 1000;
        anchor=now;
    }
    void adjustAligned(int secondsDelta,uint64_t now) {
        if (!secondsDelta) return;
        int64_t unit=static_cast<int64_t>(secondsDelta>0 ? secondsDelta : -secondsDelta)*1000;
        // Align the displayed whole seconds; subsecond overtime must not cancel an increase.
        auto candidate=(seconds(now)+static_cast<int64_t>(secondsDelta))*1000;
        // Apply the requested step, then floor to its minute/ten-second boundary.
        remaining=(candidate>=0 ? candidate/unit : -((-candidate+unit-1)/unit))*unit;
        duration=remaining>0 ? remaining : 1000;
        anchor=now;
    }
    double progress(uint64_t now) const {
        return std::clamp(static_cast<double>(milliseconds(now))/duration,0.0,1.0);
    }
    std::wstring text(uint64_t now) const {
        auto value=seconds(now); auto magnitude=value<0 ? -value : value;
        auto minutes=std::to_wstring(magnitude/60), secondsText=std::to_wstring(magnitude%60);
        if (minutes.size()<2) minutes=L"0"+minutes;
        if (secondsText.size()<2) secondsText=L"0"+secondsText;
        return (value<0 ? L"- " : L"")+minutes+L":"+secondsText;
    }
};

struct TimerProgressTransition {
    bool active{};
    double from{};
    uint64_t started{};
    void restart(double value,uint64_t now) { from=value; started=now; active=true; }
    double value(double target,uint64_t now) {
        if (!active) return target;
        double t=std::min(1.0,static_cast<double>(now-started)/150.0);
        if (t>=1) active=false;
        double eased=t*t*(3-2*t);
        return from+(target-from)*eased;
    }
};
