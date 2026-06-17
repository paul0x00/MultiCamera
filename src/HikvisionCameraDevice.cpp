#include "HikvisionCameraDevice.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "TriggerBarrier.h"

namespace {
// 海康帧 → QImage（深拷贝，跨线程安全）。Mono8 直接灰度；其余格式经 SDK 转 RGB8。
QImage convertFrame(void *handle, unsigned char *pData, MV_FRAME_OUT_INFO_EX *pInfo) {
    const int w = pInfo->nWidth;
    const int h = pInfo->nHeight;
    if (w <= 0 || h <= 0) {
        return QImage();
    }
    if (pInfo->enPixelType == PixelType_Gvsp_Mono8) {
        QImage img(pData, w, h, w, QImage::Format_Grayscale8);
        return img.copy();  // 脱离 SDK 缓冲
    }
    // 其余（Bayer/YUV/RGB/BGR/JPEG…）统一转 RGB8。
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    MV_CC_PIXEL_CONVERT_PARAM_EX cvt;
    std::memset(&cvt, 0, sizeof(cvt));
    cvt.nWidth         = static_cast<unsigned int>(w);
    cvt.nHeight        = static_cast<unsigned int>(h);
    cvt.enSrcPixelType = pInfo->enPixelType;
    cvt.pSrcData       = pData;
    cvt.nSrcDataLen    = pInfo->nFrameLen;
    cvt.enDstPixelType = PixelType_Gvsp_RGB8_Packed;
    cvt.pDstBuffer     = rgb.data();
    cvt.nDstBufferSize = static_cast<unsigned int>(rgb.size());
    if (MV_CC_ConvertPixelTypeEx(handle, &cvt) != MV_OK) {
        return QImage();
    }
    QImage img(rgb.data(), w, h, w * 3, QImage::Format_RGB888);
    return img.copy();
}
}  // namespace

HikvisionCameraDevice::HikvisionCameraDevice(void *handle, const DiscoveredDevice &info)
    : handle_(handle),
      vendor_("Hikvision"),
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
      pendingCalib_(false), pendingHostRefUs_(0) {
    // 判定黑白/彩色：读取像素格式枚举，失败按彩色处理。
    MVCC_ENUMVALUE pf;
    std::memset(&pf, 0, sizeof(pf));
    if (MV_CC_GetEnumValue(handle_, "PixelFormat", &pf) == MV_OK) {
        mono_ = (pf.nCurValue == PixelType_Gvsp_Mono8);
    }
}

HikvisionCameraDevice::~HikvisionCameraDevice() {
    stopStream();
    if (handle_) {
        MV_CC_CloseDevice(handle_);
        MV_CC_DestroyHandle(handle_);
        handle_ = nullptr;
    }
}

void HikvisionCameraDevice::setError(const std::string &msg) {
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = msg;
    }
    state_.store(CamState::Error);
}

std::string HikvisionCameraDevice::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

uint64_t HikvisionCameraDevice::nowEpochUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// ---------------- 采集控制 ----------------

bool HikvisionCameraDevice::startStream(bool triggerMode) {
    if (!handle_) {
        setError("设备句柄无效，无法启动");
        return false;
    }
    if (state_.load() == CamState::Streaming) {
        return true;
    }
    state_.store(CamState::Starting);
    triggerMode_.store(triggerMode);
    anchorSet_.store(false);  // 新一轮采集重建时间戳锚点

    // 软触发 / 连续模式：TriggerMode On/Off + TriggerSource=Software。
    MV_CC_SetEnumValue(handle_, "TriggerMode", triggerMode ? 1 : 0);
    if (triggerMode) {
        MV_CC_SetEnumValueByString(handle_, "TriggerSource", "Software");
    }

    // 注册帧回调（SDK 内部线程调用）。
    const int rc = MV_CC_RegisterImageCallBackEx(handle_, &HikvisionCameraDevice::frameCallback, this);
    if (rc != MV_OK) {
        setError("注册回调失败 (0x" + std::to_string(rc) + ")");
        return false;
    }
    const int rs = MV_CC_StartGrabbing(handle_);
    if (rs != MV_OK) {
        setError("启动取流失败 (0x" + std::to_string(rs) + ")");
        return false;
    }
    state_.store(CamState::Streaming);
    return true;
}

void HikvisionCameraDevice::stopStream() {
    const CamState s = state_.load();
    if (handle_ && (s == CamState::Streaming || s == CamState::Starting)) {
        MV_CC_StopGrabbing(handle_);
    }
    if (s != CamState::Error) {
        state_.store(CamState::Stopped);
    }
}

bool HikvisionCameraDevice::trigger() {
    if (!handle_ || !triggerMode_.load()) {
        return false;
    }
    return MV_CC_SetCommandValue(handle_, "TriggerSoftware") == MV_OK;
}

bool HikvisionCameraDevice::waitNewFrame(uint64_t baseline, int timeoutMs) const {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (frameCount_.load() > baseline) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// 一次触发保存：屏障对齐后下发软触发（触发模式）或直接等下一新帧（连续模式），取彩色帧。
bool HikvisionCameraDevice::captureTriggerSet(TriggerShot &out, TriggerBarrier &barrier, int perStepTimeoutMs) {
    const uint64_t baseline = frameCount_.load();

    // 屏障对齐：与其他厂商主曝光尽量同瞬间。
    barrier.arrive();
    barrier.wait();

    trigger();  // 触发模式下出一帧；连续模式此调用返回 false 但无害，靠等新帧。

    if (waitNewFrame(baseline, perStepTimeoutMs)) {
        out.color = latestImage(PreviewStream::Color);
        latestFrameTiming(out.timing);
    }
    out.ok = !out.color.isNull();
    return out.ok;
}

void HikvisionCameraDevice::calibrateClock(uint64_t hostRefUs) {
    // 请求在下一帧重锚：把该帧设备时间戳与 hostRefUs 对齐，使各机落到同一主机轴。
    pendingHostRefUs_.store(hostRefUs);
    pendingCalib_.store(true);
}

void HikvisionCameraDevice::syncClock() {
    // 周期再锚：请求下一帧用其到达时刻的真实主机时间重锚，约束长时间漂移。
    pendingHostRefUs_.store(nowEpochUs());
    pendingCalib_.store(true);
}

// ---------------- 帧回调 ----------------

void __stdcall HikvisionCameraDevice::frameCallback(unsigned char *pData, MV_FRAME_OUT_INFO_EX *pInfo, void *pUser) {
    if (pUser) {
        static_cast<HikvisionCameraDevice *>(pUser)->onFrame(pData, pInfo);
    }
}

void HikvisionCameraDevice::onFrame(unsigned char *pData, MV_FRAME_OUT_INFO_EX *pInfo) {
    // 绝不让异常逃逸进 SDK 的 C 回调边界。
    if (!pData || !pInfo) {
        return;
    }
    try {
        const int w = pInfo->nWidth;
        const int h = pInfo->nHeight;
        width_.store(w);
        height_.store(h);

        // 设备硬件时间戳（高低 32 位拼接），单位由设备定义；仅用于锚点增量换算。
        const uint64_t devTs =
            (static_cast<uint64_t>(pInfo->nDevTimeStampHigh) << 32) | pInfo->nDevTimeStampLow;
        // 主机接收时刻（SDK 生成，单位 ms）→ µs；作为跨机可比的主机轴时间。
        const uint64_t hostTsUs = static_cast<uint64_t>(pInfo->nHostTimeStamp) * 1000ULL;

        // 标定/首帧：建立锚点。标定时用统一的 hostRefUs，使各机共用一基准。
        if (pendingCalib_.exchange(false)) {
            hostAnchorUs_.store(pendingHostRefUs_.load());
            devAnchorUs_.store(devTs);
            anchorSet_.store(true);
        } else if (!anchorSet_.load()) {
            hostAnchorUs_.store(hostTsUs ? hostTsUs : nowEpochUs());
            devAnchorUs_.store(devTs);
            anchorSet_.store(true);
        }

        // 转成 QImage（深拷贝）：Mono8 → 灰度；其余统一经 SDK 转 RGB8。
        QImage img = convertFrame(handle_, pData, pInfo);

        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            latestImage_    = img;
            lastExposureUs_ = static_cast<uint32_t>(pInfo->fExposureTime);
            lastDevTsUs_    = devTs;
            lastHostTsUs_   = hostTsUs;
        }
        frameCount_.fetch_add(1);
    } catch (...) {
    }
}

// ---------------- 预览 ----------------

QImage HikvisionCameraDevice::latestImage(PreviewStream stream) const {
    // 单路工业相机：仅 Color 槽有画面，其余流返回空。
    if (stream != PreviewStream::Color) {
        return QImage();
    }
    std::lock_guard<std::mutex> lk(frameMutex_);
    return latestImage_;
}

std::string HikvisionCameraDevice::resolution(PreviewStream stream) const {
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

std::vector<PreviewStream> HikvisionCameraDevice::availablePreviewStreams() const {
    std::vector<PreviewStream> v;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        if (!latestImage_.isNull()) {
            v.push_back(PreviewStream::Color);
        }
    }
    return v;
}

std::string HikvisionCameraDevice::streamDiagnostics() const {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (latestImage_.isNull()) {
        return std::string("尚无帧到达");
    }
    return std::string(mono_ ? "黑白" : "彩色") + "=" +
           std::to_string(width_.load()) + "x" + std::to_string(height_.load());
}

bool HikvisionCameraDevice::latestFrameTiming(FrameTiming &out) const {
    uint64_t devTs, hostTs;
    uint32_t expUs;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        if (latestImage_.isNull()) {
            return false;
        }
        devTs  = lastDevTsUs_;
        hostTs = lastHostTsUs_;
        expUs  = lastExposureUs_;
    }
    // 主机轴曝光时刻：已标定则用锚点换算设备时间戳（多机同一基准，可直接相减）；
    // 否则退回 SDK 的主机接收时间戳。
    uint64_t hostUs = 0;
    if (anchorSet_.load()) {
        hostUs = hostAnchorUs_.load() + (devTs - devAnchorUs_.load());
    } else if (hostTs != 0) {
        hostUs = hostTs;
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
// 曝光走 GenICam "ExposureTime"（µs，float）；增益走 "Gain"（dB，float）；
// 自动曝光走 "ExposureAuto" 枚举（0=Off, 2=Continuous）。界面用 int，故四舍五入换算。

bool HikvisionCameraDevice::isTargetSupported(ExposureTarget t) const {
    return t == ExposureTarget::Industrial;
}

std::vector<ExposureTarget> HikvisionCameraDevice::supportedTargets() const {
    std::vector<ExposureTarget> v;
    v.push_back(ExposureTarget::Industrial);
    return v;
}

bool HikvisionCameraDevice::supportsAutoExposure(ExposureTarget t) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_ENUMVALUE e;
    std::memset(&e, 0, sizeof(e));
    return MV_CC_GetEnumValue(handle_, "ExposureAuto", &e) == MV_OK;
}

bool HikvisionCameraDevice::getExposureRange(ExposureTarget t, PropertyRange &range) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_FLOATVALUE f;
    std::memset(&f, 0, sizeof(f));
    if (MV_CC_GetFloatValue(handle_, "ExposureTime", &f) != MV_OK) {
        return false;
    }
    range.min  = static_cast<int>(f.fMin);
    range.max  = static_cast<int>(f.fMax);
    range.cur  = static_cast<int>(f.fCurValue);
    range.step = 1;
    range.def  = range.cur;
    return true;
}

bool HikvisionCameraDevice::getGainRange(ExposureTarget t, PropertyRange &range) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_FLOATVALUE f;
    std::memset(&f, 0, sizeof(f));
    if (MV_CC_GetFloatValue(handle_, "Gain", &f) != MV_OK) {
        return false;
    }
    range.min  = static_cast<int>(f.fMin);
    range.max  = static_cast<int>(f.fMax);
    range.cur  = static_cast<int>(f.fCurValue);
    range.step = 1;
    range.def  = range.cur;
    return true;
}

bool HikvisionCameraDevice::setAutoExposure(ExposureTarget t, bool enable) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    // 2=Continuous, 0=Off
    return MV_CC_SetEnumValue(handle_, "ExposureAuto", enable ? 2 : 0) == MV_OK;
}

bool HikvisionCameraDevice::getAutoExposure(ExposureTarget t, bool &enabled) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_ENUMVALUE e;
    std::memset(&e, 0, sizeof(e));
    if (MV_CC_GetEnumValue(handle_, "ExposureAuto", &e) != MV_OK) {
        return false;
    }
    enabled = (e.nCurValue != 0);
    return true;
}

bool HikvisionCameraDevice::setExposure(ExposureTarget t, int value) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    return MV_CC_SetFloatValue(handle_, "ExposureTime", static_cast<float>(value)) == MV_OK;
}

bool HikvisionCameraDevice::getExposure(ExposureTarget t, int &value) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_FLOATVALUE f;
    std::memset(&f, 0, sizeof(f));
    if (MV_CC_GetFloatValue(handle_, "ExposureTime", &f) != MV_OK) {
        return false;
    }
    value = static_cast<int>(f.fCurValue);
    return true;
}

bool HikvisionCameraDevice::setGain(ExposureTarget t, int value) {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    return MV_CC_SetFloatValue(handle_, "Gain", static_cast<float>(value)) == MV_OK;
}

bool HikvisionCameraDevice::getGain(ExposureTarget t, int &value) const {
    if (t != ExposureTarget::Industrial || !handle_) {
        return false;
    }
    MVCC_FLOATVALUE f;
    std::memset(&f, 0, sizeof(f));
    if (MV_CC_GetFloatValue(handle_, "Gain", &f) != MV_OK) {
        return false;
    }
    value = static_cast<int>(f.fCurValue);
    return true;
}
