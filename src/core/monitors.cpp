#include "core/monitors.h"
#include "util/str.h"

#include <windows.h>

#include <algorithm>

namespace motion {

namespace {

BOOL CALLBACK monitorProc(HMONITOR mon, HDC, LPRECT, LPARAM userData) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(userData);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    MonitorInfo info;
    info.deviceW = mi.szDevice;
    info.device  = narrow(mi.szDevice);
    info.x       = mi.rcMonitor.left;
    info.y       = mi.rcMonitor.top;
    info.width   = mi.rcMonitor.right - mi.rcMonitor.left;
    info.height  = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    list->push_back(std::move(info));
    return TRUE;
}

}

std::vector<MonitorInfo> enumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitorProc,
                        reinterpret_cast<LPARAM>(&monitors));

    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.x < b.x;
    });
    for (size_t i = 0; i < monitors.size(); ++i)
        monitors[i].index = (int)i + 1;

    return monitors;
}

}
