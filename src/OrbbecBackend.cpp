#include "OrbbecBackend.h"

#include "OrbbecCameraDevice.h"

namespace {
std::string safeStr(const char *s) { return s ? std::string(s) : std::string(); }
}  // namespace

OrbbecBackend::OrbbecBackend() {
    // 降低 SDK 控制台日志噪声
    try {
        ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_ERROR);
    } catch (const ob::Error &) {
    }
    try {
        context_ = std::make_shared<ob::Context>();
    } catch (const ob::Error &e) {
        initError_ = std::string("初始化 Orbbec SDK Context 失败: ") + e.what();
    }
}

std::vector<DiscoveredDevice> OrbbecBackend::discover(std::string &err) {
    std::vector<DiscoveredDevice> result;
    if (!context_) {
        err = initError_.empty() ? std::string("Orbbec SDK Context 未初始化") : initError_;
        return result;
    }
    try {
        std::shared_ptr<ob::DeviceList> list = context_->queryDeviceList();
        const uint32_t                  n    = list->getCount();
        for (uint32_t i = 0; i < n; ++i) {
            DiscoveredDevice d;
            d.vendor         = vendor();
            d.name           = safeStr(list->getName(i));
            d.serial         = safeStr(list->getSerialNumber(i));
            d.uid            = safeStr(list->getUid(i));
            d.connectionType = safeStr(list->getConnectionType(i));
            d.pid            = list->getPid(i);
            result.push_back(d);
        }
    } catch (const ob::Error &e) {
        err = std::string("枚举 Orbbec 设备失败: ") + e.what();
    }
    return result;
}

std::shared_ptr<ICameraDevice> OrbbecBackend::open(const std::string &serial, std::string &err) {
    if (!context_) {
        err = initError_.empty() ? std::string("Orbbec SDK Context 未初始化") : initError_;
        return std::shared_ptr<ICameraDevice>();
    }
    try {
        std::shared_ptr<ob::DeviceList> list = context_->queryDeviceList();
        std::shared_ptr<ob::Device>     dev  = list->getDeviceBySN(serial.c_str());
        return std::make_shared<OrbbecCameraDevice>(dev);
    } catch (const ob::Error &e) {
        err = std::string("连接 ") + serial + " 失败: " + e.what();
        return std::shared_ptr<ICameraDevice>();
    }
}

void OrbbecBackend::syncDeviceClock() {
    if (!context_) {
        return;
    }
    try {
        // 0 = 同步一次，把主机与所有已打开设备的时钟对齐（多机时间戳可比较）。
        context_->enableDeviceClockSync(0);
    } catch (const ob::Error &) {
        // 部分设备/连接方式不支持时钟同步，忽略。
    }
}
