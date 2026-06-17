#ifndef I_CAMERA_BACKEND_H
#define I_CAMERA_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include "CameraTypes.h"
#include "ICameraDevice.h"

// 相机后端接口：封装一个厂商 SDK 的设备枚举与打开。
// 每个后端持有自己的 SDK 上下文；CameraManager 聚合多个后端，
// 按 DiscoveredDevice::vendor 把连接请求路由到对应后端。
// 本接口仅在 GUI 线程调用，实现无需考虑并发。
class ICameraBackend {
public:
    virtual ~ICameraBackend() {}

    // 后端标识，写入 DiscoveredDevice::vendor 与 ICameraDevice::vendor()。
    virtual const char *vendor() const = 0;

    // 枚举当前可用设备（实现需填充每项的 vendor 字段）。
    // 失败时返回空列表并设置 err。
    virtual std::vector<DiscoveredDevice> discover(std::string &err) = 0;

    // 按序列号打开设备。失败返回空指针并设置 err。
    virtual std::shared_ptr<ICameraDevice> open(const std::string &serial, std::string &err) = 0;

    // 同步本后端所有已打开设备的时钟（多机时间戳对齐）。
    // 默认空实现，不支持的后端可不 override。
    virtual void syncDeviceClock() {}
};

#endif  // I_CAMERA_BACKEND_H
