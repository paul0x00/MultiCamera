#ifndef HIKVISION_CAMERA_DEVICE_H
#define HIKVISION_CAMERA_DEVICE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QImage>

#include "MvCameraControl.h"

#include "ICameraDevice.h"

class TriggerBarrier;

// ICameraDevice 的海康威视实现：单路工业相机（彩色或黑白），映射到 PreviewStream::Color。
//
// 线程模型：帧经 SDK 内部线程经 MV_CC_RegisterImageCallBackEx 回调到达，回调里转成 QImage、
// 加锁存入缓存并记录时间戳；GUI 线程通过 latestImage() 取副本。回调全程 try/catch(...) 兜底，
// 异常绝不逃逸进 SDK 的 C 回调边界。曝光/增益作用对象统一为 ExposureTarget::Industrial。
class HikvisionCameraDevice : public ICameraDevice {
public:
    // handle 为 MV_CC_CreateHandle 创建并已 MV_CC_OpenDevice 打开的设备句柄。
    HikvisionCameraDevice(void *handle, const DiscoveredDevice &info);
    ~HikvisionCameraDevice();

    HikvisionCameraDevice(const HikvisionCameraDevice &)            = delete;
    HikvisionCameraDevice &operator=(const HikvisionCameraDevice &) = delete;

    const std::string &vendor() const override { return vendor_; }
    const std::string &name() const override { return name_; }
    const std::string &serial() const override { return serial_; }
    const std::string &uid() const override { return uid_; }
    const std::string &connectionType() const override { return connectionType_; }
    int                pid() const override { return pid_; }

    bool startStream(bool triggerMode) override;
    void stopStream() override;
    bool isStreaming() const override { return state_.load() == CamState::Streaming; }

    CamState    state() const override { return state_.load(); }
    uint64_t    frameCount() const override { return frameCount_.load(); }
    std::string lastError() const override;

    bool trigger() override;
    bool captureTriggerSet(TriggerShot &out, TriggerBarrier &barrier, int perStepTimeoutMs) override;

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

    QImage                     latestImage(PreviewStream stream) const override;
    std::string                resolution(PreviewStream stream) const override;
    std::vector<PreviewStream> availablePreviewStreams() const override;
    std::string                streamDiagnostics() const override;
    bool                       latestFrameTiming(FrameTiming &out) const override;
    void                       calibrateClock(uint64_t hostRefUs) override;
    void                       syncClock() override;

private:
    static void __stdcall frameCallback(unsigned char *pData, MV_FRAME_OUT_INFO_EX *pInfo, void *pUser);
    void                  onFrame(unsigned char *pData, MV_FRAME_OUT_INFO_EX *pInfo);
    void                  setError(const std::string &msg);
    static uint64_t       nowEpochUs();
    bool                  waitNewFrame(uint64_t baseline, int timeoutMs) const;

    void *handle_;

    std::string vendor_;
    std::string name_;
    std::string serial_;
    std::string uid_;
    std::string connectionType_;
    int         pid_ = 0;
    bool        mono_ = false;  // 黑白 sensor：无彩色相关属性

    std::atomic<CamState> state_;
    std::atomic<uint64_t> frameCount_;
    std::atomic<bool>     triggerMode_;
    std::atomic<int>      width_, height_;

    // 时间戳锚点：把设备硬件时间戳换算到主机 epoch。标定（或首帧）时记一对
    // (hostAnchorUs, devAnchorUs)，此后 hostUs = hostAnchorUs + (devTs - devAnchorUs)。
    std::atomic<bool>     anchorSet_;
    std::atomic<uint64_t> hostAnchorUs_;
    std::atomic<uint64_t> devAnchorUs_;
    std::atomic<bool>     pendingCalib_;     // calibrateClock 已请求，等下一帧重锚
    std::atomic<uint64_t> pendingHostRefUs_; // 标定基准时刻

    mutable std::mutex errorMutex_;
    std::string        lastError_;

    mutable std::mutex frameMutex_;
    QImage             latestImage_;
    uint32_t           lastExposureUs_ = 0;
    uint64_t           lastDevTsUs_    = 0;
    uint64_t           lastHostTsUs_   = 0;
};

#endif  // HIKVISION_CAMERA_DEVICE_H
