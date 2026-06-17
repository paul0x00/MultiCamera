#include "MindvisionCameraDevice.h"

#include <chrono>
#include <cstring>

MindvisionCameraDevice::MindvisionCameraDevice(int handle, const DiscoveredDevice &info, bool mono)
    : handle_(handle),
      mono_(mono),
      vendor_("MindVision"),
      name_(info.name),
      serial_(info.serial),
      uid_(info.uid),
      connectionType_(info.connectionType),
      pid_(info.pid),
      state_(CamState::Connected),
      frameCount_(0),
      triggerMode_(false),
      width_(0), height_(0),
      anchorSet_(false), hostAnchorUs_(0), devAnchorUs_(0),
      pendingCalib_(false), pendingHostRefUs_(0) {}

MindvisionCameraDevice::~MindvisionCameraDevice() {
    stopStream();
    if (handle_) {
        CameraUnInit(static_cast<CameraHandle>(handle_));
        handle_ = 0;
    }
}

void MindvisionCameraDevice::setError(const std::string &msg) {
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = msg;
    }
    state_.store(CamState::Error);
}

std::string MindvisionCameraDevice::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

uint64_t MindvisionCameraDevice::nowEpochUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// ---------------- 采集控制 ----------------

bool MindvisionCameraDevice::startStream(bool triggerMode) {
    if (!handle_) {
        setError("设备句柄无效，无法启动");
        return false;
    }
    if (state_.load() == CamState::Streaming) {
        return true;
    }
    const CameraHandle h = static_cast<CameraHandle>(handle_);
    state_.store(CamState::Starting);
    triggerMode_.store(triggerMode);
    anchorSet_.store(false);

    // 0=连续采集，1=软触发。软触发下每次触发出 1 帧。
    CameraSetTriggerMode(h, triggerMode ? 1 : 0);
    if (triggerMode) {
        CameraSetTriggerCount(h, 1);
    }

    CameraSetCallbackFunction(h, &MindvisionCameraDevice::frameCallback, this, nullptr);

    const int rc = CameraPlay(h);
    if (rc != CAMERA_STATUS_SUCCESS) {
        setError("启动采集失败 (" + std::to_string(rc) + ")");
        return false;
    }
    state_.store(CamState::Streaming);
    return true;
}

void MindvisionCameraDevice::stopStream() {
    const CamState s = state_.load();
    if (handle_ && (s == CamState::Streaming || s == CamState::Starting)) {
        CameraPause(static_cast<CameraHandle>(handle_));
    }
    if (s != CamState::Error) {
        state_.store(CamState::Stopped);
    }
}

bool MindvisionCameraDevice::trigger() {
    if (!handle_ || !triggerMode_.load()) {
        return false;
    }
    return CameraSoftTrigger(static_cast<CameraHandle>(handle_)) == CAMERA_STATUS_SUCCESS;
}

void MindvisionCameraDevice::calibrateClock(uint64_t hostRefUs) {
    // 清零设备时间戳，并请求下一帧以统一主机基准重锚（各机共用 hostRefUs）。
    if (handle_) {
        CameraRstTimeStamp(static_cast<CameraHandle>(handle_));
    }
    pendingHostRefUs_.store(hostRefUs);
    pendingCalib_.store(true);
}

// ---------------- 帧回调 ----------------

void __stdcall MindvisionCameraDevice::frameCallback(CameraHandle h, BYTE *pFrameBuffer, tSdkFrameHead *pHead, PVOID ctx) {
    (void)h;
    if (ctx) {
        static_cast<MindvisionCameraDevice *>(ctx)->onFrame(pFrameBuffer, pHead);
    }
}

void MindvisionCameraDevice::onFrame(BYTE *pFrameBuffer, tSdkFrameHead *pHead) {
    if (!pFrameBuffer || !pHead) {
        return;
    }
    try {
        const CameraHandle h = static_cast<CameraHandle>(handle_);
        const int          w = pHead->iWidth;
        const int          h2 = pHead->iHeight;
        width_.store(w);
        height_.store(h2);

        // 设备时间戳：0.1ms → µs。
        const uint64_t devTsUs = static_cast<uint64_t>(pHead->uiTimeStamp) * 100ULL;

        if (pendingCalib_.exchange(false)) {
            hostAnchorUs_.store(pendingHostRefUs_.load());
            devAnchorUs_.store(devTsUs);
            anchorSet_.store(true);
        } else if (!anchorSet_.load()) {
            hostAnchorUs_.store(nowEpochUs());
            devAnchorUs_.store(devTsUs);
            anchorSet_.store(true);
        }

        // ISP 处理为 RGB8 / Mono8，再深拷贝成 QImage。
        const int        bpp = mono_ ? 1 : 3;
        const size_t     need = static_cast<size_t>(w) * h2 * bpp;
        if (ispBuffer_.size() < need) {
            ispBuffer_.resize(need);
        }
        tSdkFrameHead outHead = *pHead;
        QImage        img;
        if (CameraImageProcess(h, pFrameBuffer, ispBuffer_.data(), &outHead) == CAMERA_STATUS_SUCCESS) {
            if (mono_) {
                img = QImage(ispBuffer_.data(), w, h2, w, QImage::Format_Grayscale8).copy();
            } else {
                img = QImage(ispBuffer_.data(), w, h2, w * 3, QImage::Format_RGB888).copy();
            }
        }

        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            latestImage_    = img;
            lastExposureUs_ = pHead->uiExpTime;
            lastDevTsUs_    = devTsUs;
        }
        frameCount_.fetch_add(1);
    } catch (...) {
    }
}

// ---------------- 预览 ----------------

QImage MindvisionCameraDevice::latestImage(PreviewStream stream) const {
    if (stream != PreviewStream::Color) {
        return QImage();
    }
    std::lock_guard<std::mutex> lk(frameMutex_);
    return latestImage_;
}

std::string MindvisionCameraDevice::resolution(PreviewStream stream) const {
    if (stream != PreviewStream::Color) {
        return std::string();
    }
    const int w = width_.load();
    const int h = height_.load();
    if (w <= 0 || h <= 0) {
        return std::string();
    }
    return std::to_string(w) + "x" + std::to_string(h);
}

std::vector<PreviewStream> MindvisionCameraDevice::availablePreviewStreams() const {
    std::vector<PreviewStream> v;
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (!latestImage_.isNull()) {
        v.push_back(PreviewStream::Color);
    }
    return v;
}

std::string MindvisionCameraDevice::streamDiagnostics() const {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (latestImage_.isNull()) {
        return std::string("尚无帧到达");
    }
    return std::string(mono_ ? "黑白" : "彩色") + "=" +
           std::to_string(width_.load()) + "x" + std::to_string(height_.load());
}

bool MindvisionCameraDevice::latestFrameTiming(FrameTiming &out) const {
    uint64_t devTs;
    uint32_t expUs;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        if (latestImage_.isNull()) {
            return false;
        }
        devTs = lastDevTsUs_;
        expUs = lastExposureUs_;
    }
    uint64_t hostUs;
    if (anchorSet_.load()) {
        hostUs = hostAnchorUs_.load() + (devTs - devAnchorUs_.load());
    } else {
        hostUs = nowEpochUs();
    }
    out.hostEpochUs  = hostUs;
    out.captureRawUs = devTs;
    out.exposureUs   = expUs;
    out.valid        = true;
    return true;
}

// ---------------- 参数控制 ----------------
// 工业相机单路曝光：作用对象统一为 ExposureTarget::Industrial。
// 曝光走 CameraSet/GetExposureTime（µs，double）；增益走 CameraSet/GetAnalogGain（整数倍数档）；
// 自动曝光走 CameraSetAeState。

bool MindvisionCameraDevice::isTargetSupported(ExposureTarget t) const {
    return t == ExposureTarget::Industrial;
}

std::vector<ExposureTarget> MindvisionCameraDevice::supportedTargets() const {
    std::vector<ExposureTarget> v;
    v.push_back(ExposureTarget::Industrial);
    return v;
}

bool MindvisionCameraDevice::supportsAutoExposure(ExposureTarget t) const {
    return t == ExposureTarget::Industrial && handle_ != 0;
}

bool MindvisionCameraDevice::getExposureRange(ExposureTarget t, PropertyRange &range) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    const CameraHandle h = static_cast<CameraHandle>(handle_);
    double             mn = 0, mx = 0, st = 0, cur = 0;
    if (CameraGetExposureTimeRange(h, &mn, &mx, &st) != CAMERA_STATUS_SUCCESS) {
        return false;
    }
    CameraGetExposureTime(h, &cur);
    range.min  = static_cast<int>(mn);
    range.max  = static_cast<int>(mx);
    range.cur  = static_cast<int>(cur);
    range.step = st > 0 ? static_cast<int>(st) : 1;
    range.def  = range.cur;
    return true;
}

bool MindvisionCameraDevice::getGainRange(ExposureTarget t, PropertyRange &range) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    tSdkCameraCapbility cap;
    std::memset(&cap, 0, sizeof(cap));
    if (CameraGetCapability(static_cast<CameraHandle>(handle_), &cap) != CAMERA_STATUS_SUCCESS) {
        return false;
    }
    int cur = 0;
    CameraGetAnalogGain(static_cast<CameraHandle>(handle_), &cur);
    range.min  = static_cast<int>(cap.sExposeDesc.uiAnalogGainMin);
    range.max  = static_cast<int>(cap.sExposeDesc.uiAnalogGainMax);
    range.cur  = cur;
    range.step = 1;
    range.def  = cur;
    return true;
}

bool MindvisionCameraDevice::setAutoExposure(ExposureTarget t, bool enable) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    return CameraSetAeState(static_cast<CameraHandle>(handle_), enable ? 1 : 0) == CAMERA_STATUS_SUCCESS;
}

bool MindvisionCameraDevice::getAutoExposure(ExposureTarget t, bool &enabled) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    int ae = 0;
    if (CameraGetAeState(static_cast<CameraHandle>(handle_), &ae) != CAMERA_STATUS_SUCCESS) {
        return false;
    }
    enabled = (ae != 0);
    return true;
}

bool MindvisionCameraDevice::setExposure(ExposureTarget t, int value) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    return CameraSetExposureTime(static_cast<CameraHandle>(handle_), static_cast<double>(value)) == CAMERA_STATUS_SUCCESS;
}

bool MindvisionCameraDevice::getExposure(ExposureTarget t, int &value) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    double v = 0;
    if (CameraGetExposureTime(static_cast<CameraHandle>(handle_), &v) != CAMERA_STATUS_SUCCESS) {
        return false;
    }
    value = static_cast<int>(v);
    return true;
}

bool MindvisionCameraDevice::setGain(ExposureTarget t, int value) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    return CameraSetAnalogGain(static_cast<CameraHandle>(handle_), value) == CAMERA_STATUS_SUCCESS;
}

bool MindvisionCameraDevice::getGain(ExposureTarget t, int &value) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    int v = 0;
    if (CameraGetAnalogGain(static_cast<CameraHandle>(handle_), &v) != CAMERA_STATUS_SUCCESS) {
        return false;
    }
    value = v;
    return true;
}
