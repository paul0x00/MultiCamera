#ifndef ORBBEC_CAMERA_DEVICE_H
#define ORBBEC_CAMERA_DEVICE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <QImage>

#include <libobsensor/ObSensor.hpp>

#include "ICameraDevice.h"

class TriggerBarrier;

// ICameraDevice 的 Orbbec 实现：持有 ob::Device 与 ob::Pipeline，负责采集、
// 帧转换、参数（曝光/增益/自动曝光）以及软触发。
//
// 线程模型：帧通过 SDK 内部线程回调到达，回调中只保存"最新一帧"的原始引用
// （不做像素转换，避免拖慢回调导致帧在 SDK 队列里积压）；GUI 线程在 latestImage()
// 中按需把最新一帧转换成 QImage。所有对外状态用原子量或互斥量保护，本类不直接
// 触碰任何 Qt GUI 对象。
class OrbbecCameraDevice : public ICameraDevice {
public:
    explicit OrbbecCameraDevice(std::shared_ptr<ob::Device> device);
    ~OrbbecCameraDevice();

    OrbbecCameraDevice(const OrbbecCameraDevice &)            = delete;
    OrbbecCameraDevice &operator=(const OrbbecCameraDevice &) = delete;

    // --- 设备信息（构造时缓存，线程安全只读） ---
    const std::string &vendor() const override { return vendor_; }
    const std::string &name() const override { return name_; }
    const std::string &serial() const override { return serial_; }
    const std::string &uid() const override { return uid_; }
    const std::string &connectionType() const override { return connectionType_; }
    int                pid() const override { return pid_; }

    // --- 采集控制 ---
    bool startStream(bool triggerMode) override;
    void stopStream() override;
    bool isStreaming() const override { return state_.load() == CamState::Streaming; }

    CamState    state() const override { return state_.load(); }
    uint64_t    frameCount() const override { return frameCount_.load(); }
    std::string lastError() const override;

    bool trigger() override;
    bool captureTriggerSet(TriggerShot &out, TriggerBarrier &barrier, int perStepTimeoutMs) override;

    // --- 参数控制 ---
    std::vector<ExposureTarget> supportedTargets() const override;
    bool                        isTargetSupported(ExposureTarget t) const override;

    bool getExposureRange(ExposureTarget t, PropertyRange &range) const override;
    bool getGainRange(ExposureTarget t, PropertyRange &range) const override;
    bool supportsAutoExposure(ExposureTarget t) const override;

    bool setAutoExposure(ExposureTarget t, bool enable) override;
    bool getAutoExposure(ExposureTarget t, bool &enabled) const override;
    bool setExposure(ExposureTarget t, int value) override;
    bool getExposure(ExposureTarget t, int &value) const override;
    bool setGain(ExposureTarget t, int value) override;
    bool getGain(ExposureTarget t, int &value) const override;

    // --- 预览 ---
    QImage                     latestImage(PreviewStream stream) const override;
    std::string                resolution(PreviewStream stream) const override;
    std::vector<PreviewStream> availablePreviewStreams() const override;
    std::string                streamDiagnostics() const override;
    bool                       latestFrameTiming(FrameTiming &out) const override;

private:
    bool hasSensor(OBSensorType type) const { return sensors_.count(type) > 0; }

    // 启动彩色预览（停掉一切现有流后），可选超时等首帧到达。失败设 lastError_。
    bool startColorPreview(int waitFirstFrameMs);
    // 启动单 IR 流且把 IR_CHANNEL_DATA_SOURCE 设为指定 channel（0=左、1=右）。
    bool startIrChannel(int channel);
    // 等下一帧到达（按 frameCount_ 增量判定），超时返回 false。
    bool waitNewFrame(uint64_t baseline, int timeoutMs) const;

    void onFrameSet(const std::shared_ptr<ob::FrameSet> &frameSet);
    void setError(const std::string &msg);

    // 当前主机 system_clock 的 epoch 微秒（跨厂商公共时间轴）。
    static uint64_t nowEpochUs();

    std::shared_ptr<ob::Device>   device_;
    std::shared_ptr<ob::Pipeline> pipeline_;

    std::string vendor_;
    std::string name_;
    std::string serial_;
    std::string uid_;
    std::string connectionType_;
    int         pid_ = 0;

    std::set<OBSensorType> sensors_;

    std::atomic<CamState> state_;
    std::atomic<uint64_t> frameCount_;
    std::atomic<bool>     triggerMode_;

    // 时间戳对齐：优先用 SDK 全局时间戳（主机域、已消漂移的捕获时刻）；
    // 设备不支持时回退到"首帧锚点 + 硬件时间戳增量"换算到主机 epoch。
    std::atomic<bool>     globalTsEnabled_;  // 本次采集是否成功启用全局时间戳
    std::atomic<bool>     tsAnchorSet_;      // 回退用锚点是否已在首帧建立
    std::atomic<uint64_t> hostAnchorUs_;     // 首帧到达时的主机 epoch（µs）
    std::atomic<uint64_t> devAnchorUs_;      // 首帧的设备硬件时间戳（µs）

    mutable std::mutex errorMutex_;
    std::string        lastError_;

    // 最新一帧的原始引用：回调只保存它，像素转换推迟到 GUI 线程的 latestImage()。
    mutable std::mutex            frameMutex_;
    std::shared_ptr<ob::FrameSet> latestFrameSet_;
    std::atomic<int>   depthW_, depthH_;
    std::atomic<int>   irW_, irH_;
    std::atomic<int>   colorW_, colorH_;

    // 各流是否已产出过非空画面（决定 availablePreviewStreams 报告哪些流）。
    std::atomic<bool>  depthAlive_, irAlive_, colorAlive_;
};

#endif  // ORBBEC_CAMERA_DEVICE_H
