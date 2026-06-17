#ifndef HIKVISION_BACKEND_H
#define HIKVISION_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include "ICameraBackend.h"

// ICameraBackend 的海康威视（MVS / MvCameraControl）实现：枚举 USB3/GigE 设备并按序列号打开。
// 仅在 Windows 下编译（SDK 仅提供 Windows 库）。
class HikvisionBackend : public ICameraBackend {
public:
    HikvisionBackend();

    const char *vendor() const override { return "Hikvision"; }

    std::vector<DiscoveredDevice>  discover(std::string &err) override;
    std::shared_ptr<ICameraDevice> open(const std::string &serial, std::string &err) override;
    // 海康设备的时钟标定在 ICameraDevice::calibrateClock 内逐台进行，本后端无全局上下文同步。
};

#endif  // HIKVISION_BACKEND_H
