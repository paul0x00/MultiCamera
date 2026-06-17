#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CameraTypes.h"
#include "ICameraBackend.h"
#include "ICameraDevice.h"

// 多相机管理器：聚合各厂商后端（ICameraBackend），负责设备枚举、连接/断开，
// 以及"错峰启动"（按采集间隔依次启动各相机的采集）。
// 连接请求按枚举结果中的 vendor 字段路由到对应后端。
class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    CameraManager(const CameraManager &)            = delete;
    CameraManager &operator=(const CameraManager &) = delete;

    // 重新枚举设备（所有后端的结果合并），返回当前设备列表。
    std::vector<DiscoveredDevice> refresh();
    std::vector<DiscoveredDevice> discovered() const;
    std::string                   lastError() const;

    // 连接/断开（按序列号）。采集进行中不应调用。
    bool connect(const std::string &serial, std::string &err);
    void disconnect(const std::string &serial);
    int  connectAll(std::vector<std::string> &errors);
    void disconnectAll();

    // 同步所有后端已打开设备的时钟（多机时间戳对齐）。连接成功后自动调用。
    void syncDeviceClocks();

    // 错峰启动采集：依次启动每台已连接相机，相邻两台间隔 intervalMs 毫秒。
    // 启动在后台线程进行，不阻塞调用者（GUI）。
    void startCapture(int intervalMs, bool triggerMode);
    void stopCapture();
    bool isCapturing() const { return capturing_.load(); }

    // 向所有处于软触发模式且正在采集的相机"尽可能同时"发送一次触发：
    // 每台用独立线程在同一屏障上等待，统一释放后各自触发，压低派发偏差。
    // 返回触发派发时刻（主机 system_clock epoch 微秒），供存图配帧/记录用。
    uint64_t triggerAll();

    // 已连接相机（按连接顺序）。
    std::vector<std::shared_ptr<ICameraDevice>> connectedDevices() const;
    std::shared_ptr<ICameraDevice>              device(const std::string &serial) const;
    size_t                                      connectedCount() const;

private:
    void            joinStartThread();
    ICameraBackend *backendFor(const std::string &vendor) const;

    std::vector<std::shared_ptr<ICameraBackend>> backends_;
    std::vector<DiscoveredDevice>                discovered_;
    std::vector<std::shared_ptr<ICameraDevice>>  devices_;  // 已连接

    mutable std::mutex mutex_;  // 保护 discovered_ / devices_ / lastError_
    std::string        lastError_;

    std::thread       startThread_;
    std::atomic<bool> abortStart_;
    std::atomic<bool> capturing_;
};

#endif  // CAMERA_MANAGER_H
