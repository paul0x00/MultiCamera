#ifndef MINDVISION_CAMERA_DEVICE_H
#define MINDVISION_CAMERA_DEVICE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QImage>

#ifdef _WIN32
#include <windows.h>  // CameraApi.h 依赖 BYTE/PVOID/UINT/WINAPI 等 Windows 类型
#endif
#include "CameraApi.h"

#include "ICameraDevice.h"

class TriggerBarrier;

// ICameraDevice 的迈德威视实现：一台双目相机输出一张左右拼接大图，软件按列中点切两半，
// 左半 → PreviewStream::IRLeft，右半 → PreviewStream::IRRight（均为灰度，各 1280×1024）。
//
// 线程模型：帧经 SDK 内部线程经 CameraSetCallbackFunction 回调到达，回调里 ISP 处理成 RGB8/Mono8、
// 切左右两半转 QImage 加锁缓存并记录时间戳；GUI 线程通过 latestImage() 取副本。回调全程 try/catch(...) 兜底。
// 曝光/增益作用对象统一为 ExposureTarget::Industrial。设备时间戳 uiTimeStamp 单位 0.1ms，
// CameraRstTimeStamp 可将其清零；标定时各机清零并锚定到统一主机基准，使帧时间戳跨机可比。
class MindvisionCameraDevice : public ICameraDevice {
public:
    MindvisionCameraDevice(int handle, const DiscoveredDevice &info, bool mono);
    ~MindvisionCameraDevice();

    MindvisionCameraDevice(const MindvisionCameraDevice &)            = delete;
    MindvisionCameraDevice &operator=(const MindvisionCameraDevice &) = delete;

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
    static void __stdcall frameCallback(CameraHandle h, BYTE *pFrameBuffer, tSdkFrameHead *pHead, PVOID ctx);
    void                  onFrame(BYTE *pFrameBuffer, tSdkFrameHead *pHead);
    void                  setError(const std::string &msg);
    static uint64_t       nowEpochUs();
    bool                  waitNewFrame(uint64_t baseline, int timeoutMs) const;

    int  handle_;  // CameraHandle
    bool mono_;

    std::string vendor_;
    std::string name_;
    std::string serial_;
    std::string uid_;
    std::string connectionType_;
    int         pid_ = 0;

    std::atomic<CamState> state_;
    std::atomic<uint64_t> frameCount_;
    std::atomic<bool>     triggerMode_;
    std::atomic<int>      width_, height_;

    // 时间戳锚点：hostUs = hostAnchorUs + (devTs - devAnchorUs)。标定时清零设备时间戳并以统一基准锚定。
    std::atomic<bool>     anchorSet_;
    std::atomic<uint64_t> hostAnchorUs_;
    std::atomic<uint64_t> devAnchorUs_;
    std::atomic<bool>     pendingCalib_;
    std::atomic<uint64_t> pendingHostRefUs_;

    std::vector<unsigned char> ispBuffer_;  // ISP 输出缓冲（回调线程独占，无需加锁）

    mutable std::mutex errorMutex_;
    std::string        lastError_;

    mutable std::mutex frameMutex_;
    QImage             latestLeft_;   // 左半（左目灰度）
    QImage             latestRight_;  // 右半（右目灰度）
    uint32_t           lastExposureUs_ = 0;
    uint64_t           lastDevTsUs_    = 0;
};

#endif  // MINDVISION_CAMERA_DEVICE_H
