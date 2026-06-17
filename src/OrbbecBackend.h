#ifndef ORBBEC_BACKEND_H
#define ORBBEC_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include <libobsensor/ObSensor.hpp>

#include "ICameraBackend.h"

// ICameraBackend 的 Orbbec 实现：持有 ob::Context，负责设备枚举与打开。
class OrbbecBackend : public ICameraBackend {
public:
    OrbbecBackend();

    const char *vendor() const override { return "Orbbec"; }

    std::vector<DiscoveredDevice>  discover(std::string &err) override;
    std::shared_ptr<ICameraDevice> open(const std::string &serial, std::string &err) override;
    void                           syncDeviceClock() override;

private:
    std::shared_ptr<ob::Context> context_;
    std::string                  initError_;  // Context 创建失败的原因
};

#endif  // ORBBEC_BACKEND_H
