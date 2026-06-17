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

// 一次"触发保存"中单台设备的采集结果（设备指针 + 抓到的图像/时间信息）。
struct CaptureResult {
    std::shared_ptr<ICameraDevice> dev;
    TriggerShot                    shot;
};

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

    // 时间戳标定：在同一屏障上对所有已连接设备下发 calibrateClock(同一主机基准)，
    // 使各机内部时钟对齐到同一主机轴，触发存图时帧时间戳可直接相减。
    // 返回标定基准时刻（主机 system_clock epoch 微秒）。可随时手动调用。
    uint64_t calibrateClocks();

    // 启动采集：一次性启动所有已连接相机（triggerMode=true 为软触发模式）。
    void startCapture(bool triggerMode);
    void stopCapture();
    bool isCapturing() const { return capturing_.load(); }

    // 向所有处于采集中的相机"尽可能同时"发起一次触发保存：每台用独立线程跑各自的
    // captureTriggerSet 序列（Orbbec 分时取彩色+左右IR；海康/迈德威视取一帧），
    // 在同一屏障上等齐后统一释放，使各机主曝光尽量同瞬间。阻塞至全部完成或超时。
    // perStepTimeoutMs 为每步等帧超时。返回各设备结果；dispatchUs 输出主曝光派发时刻。
    std::vector<CaptureResult> captureTriggerSets(int perStepTimeoutMs, uint64_t &dispatchUs);

    // 周期时钟同步：启动/停止一条后台线程，每 intervalMs 对所有已连接设备调 syncClock()，
    // 约束各机内部时钟的长时间漂移。连接成功后自动启动。
    void startClockSync(int intervalMs);
    void stopClockSync();

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

    // 周期时钟同步后台线程。
    std::thread       clockSyncThread_;
    std::atomic<bool> clockSyncRunning_;
    int               clockSyncIntervalMs_;
};

#endif  // CAMERA_MANAGER_H
