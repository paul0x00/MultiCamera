#ifndef MINDVISION_BACKEND_H
#define MINDVISION_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include "ICameraBackend.h"

// ICameraBackend 的迈德威视（MindVision / CameraApi）实现：枚举设备并按序列号打开。
// 仅在 Windows 下编译（SDK 仅提供 Windows 库）。
class MindvisionBackend : public ICameraBackend {
public:
    MindvisionBackend();

    const char *vendor() const override { return "MindVision"; }

    std::vector<DiscoveredDevice>  discover(std::string &err) override;
    std::shared_ptr<ICameraDevice> open(const std::string &serial, std::string &err) override;
    // 时钟标定在 ICameraDevice::calibrateClock 内逐台进行（CameraRstTimeStamp）。
};

#endif  // MINDVISION_BACKEND_H
