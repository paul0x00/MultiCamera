#include "OrbbecCameraDevice.h"

#include <chrono>
#include <thread>
#include <utility>

#include "FrameConverter.h"
#include "TriggerBarrier.h"

namespace {

std::string safeStr(const char *s) { return s ? std::string(s) : std::string(); }

// Orbbec 属性 ID 映射（仅本实现使用）。
OBPropertyID exposureProp(ExposureTarget t) {
    switch (t) {
    case ExposureTarget::Depth: return OB_PROP_DEPTH_EXPOSURE_INT;
    case ExposureTarget::IR:    return OB_PROP_IR_EXPOSURE_INT;
    case ExposureTarget::Color: return OB_PROP_COLOR_EXPOSURE_INT;
    }
    return OB_PROP_DEPTH_EXPOSURE_INT;
}

OBPropertyID gainProp(ExposureTarget t) {
    switch (t) {
    case ExposureTarget::Depth: return OB_PROP_DEPTH_GAIN_INT;
    case ExposureTarget::IR:    return OB_PROP_IR_GAIN_INT;
    case ExposureTarget::Color: return OB_PROP_COLOR_GAIN_INT;
    }
    return OB_PROP_DEPTH_GAIN_INT;
}

OBPropertyID autoExposureProp(ExposureTarget t) {
    switch (t) {
    case ExposureTarget::Depth: return OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL;
    case ExposureTarget::IR:    return OB_PROP_IR_AUTO_EXPOSURE_BOOL;
    case ExposureTarget::Color: return OB_PROP_COLOR_AUTO_EXPOSURE_BOOL;
    }
    return OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL;
}

}  // namespace

OrbbecCameraDevice::OrbbecCameraDevice(std::shared_ptr<ob::Device> device)
    : device_(std::move(device)),
      vendor_("Orbbec"),
      state_(CamState::Connected),
      frameCount_(0),
      triggerMode_(false),
      globalTsEnabled_(false),
      tsAnchorSet_(false),
      hostAnchorUs_(0),
      devAnchorUs_(0),
      depthW_(0), depthH_(0), irW_(0), irH_(0), colorW_(0), colorH_(0),
      depthAlive_(false), irAlive_(false), colorAlive_(false) {
    // 缓存设备信息
    try {
        std::shared_ptr<ob::DeviceInfo> info = device_->getDeviceInfo();
        name_           = safeStr(info->getName());
        serial_         = safeStr(info->getSerialNumber());
        uid_            = safeStr(info->getUid());
        connectionType_ = safeStr(info->getConnectionType());
        pid_            = info->getPid();
    } catch (const ob::Error &e) {
        setError(std::string("读取设备信息失败: ") + e.what());
    }

    // 枚举该设备的传感器（不创建，仅读取类型）
    try {
        std::shared_ptr<ob::SensorList> sl = device_->getSensorList();
        const uint32_t                  n  = sl->getCount();
        for (uint32_t i = 0; i < n; ++i) {
            sensors_.insert(sl->getSensorType(i));
        }
    } catch (const ob::Error &e) {
        setError(std::string("枚举传感器失败: ") + e.what());
    }

    // 创建与该设备绑定的管线
    try {
        pipeline_ = std::make_shared<ob::Pipeline>(device_);
    } catch (const ob::Error &e) {
        setError(std::string("创建管线失败: ") + e.what());
    }
}

OrbbecCameraDevice::~OrbbecCameraDevice() {
    stopStream();
    // 先释放管线再释放设备，避免回调期间设备被销毁
    pipeline_.reset();
    device_.reset();
}

void OrbbecCameraDevice::setError(const std::string &msg) {
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = msg;
    }
    state_.store(CamState::Error);
}

std::string OrbbecCameraDevice::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

// ---------------- 采集控制 ----------------

bool OrbbecCameraDevice::startStream(bool triggerMode) {
    if (!pipeline_) {
        setError("管线未创建，无法启动");
        return false;
    }
    if (state_.load() == CamState::Streaming) {
        return true;
    }
    state_.store(CamState::Starting);
    triggerMode_.store(triggerMode);

    // 新一轮采集：清掉上次的"出帧"标记与残留帧。
    depthAlive_.store(false);
    irAlive_.store(false);
    colorAlive_.store(false);
    tsAnchorSet_.store(false);
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        latestFrameSet_.reset();
    }

    // 启用全局时间戳（多机时间轴对齐）。
    try {
        if (device_ && device_->isGlobalTimestampSupported()) {
            device_->enableGlobalTimestamp(true);
            globalTsEnabled_.store(true);
        } else {
            globalTsEnabled_.store(false);
        }
    } catch (const ob::Error &) {
        globalTsEnabled_.store(false);
    }

    // 配置多设备同步模式：预览态用 FREE_RUN（彩色连续出帧），触发态用软触发。
    try {
        OBMultiDeviceSyncConfig sc{};
        sc.syncMode = triggerMode ? OB_MULTI_DEVICE_SYNC_MODE_SOFTWARE_TRIGGERING
                                  : OB_MULTI_DEVICE_SYNC_MODE_FREE_RUN;
        sc.framesPerTrigger = 1;
        sc.triggerOutEnable = false;
        device_->setMultiDeviceSyncConfig(sc);
    } catch (const ob::Error &) {
        // 部分型号不支持设置同步模式；忽略并按默认模式继续。
    }

    // 预览只跑彩色：触发保存时由 captureTriggerSet 做停流→切 IR channel→恢复彩色 的分时序列。
    if (!startColorPreview(0)) {
        return false;
    }
    return true;
}

// 启动"仅彩色"配置的预览（先确保 pipeline 是停的）。waitFirstFrameMs>0 时等首帧到达。
bool OrbbecCameraDevice::startColorPreview(int waitFirstFrameMs) {
    if (!pipeline_) {
        return false;
    }
    ob::Pipeline::FrameSetCallback cb = [this](std::shared_ptr<ob::FrameSet> fs) { this->onFrameSet(fs); };
    try {
        std::shared_ptr<ob::Config> config = std::make_shared<ob::Config>();
        if (hasSensor(OB_SENSOR_COLOR)) {
            config->enableVideoStream(OB_SENSOR_COLOR);
            pipeline_->start(config, cb);
        } else {
            // 无彩色传感器：交给 SDK 默认配置（预览效果会退化，但保证不空跑）。
            pipeline_->start(std::shared_ptr<ob::Config>(), cb);
        }
        state_.store(CamState::Streaming);
    } catch (const ob::Error &e) {
        setError(std::string("启动彩色预览失败: ") + e.what());
        return false;
    }
    if (waitFirstFrameMs > 0) {
        const uint64_t baseline = frameCount_.load();
        waitNewFrame(baseline, waitFirstFrameMs);
    }
    return true;
}

// 启动单 IR 流并设定 IR_CHANNEL_DATA_SOURCE_INT（0=左 sensor, 1=右 sensor）。
bool OrbbecCameraDevice::startIrChannel(int channel) {
    if (!pipeline_) {
        return false;
    }
    // 切 channel 必须在管线停止状态下进行。
    try {
        if (device_ && device_->isPropertySupported(OB_PROP_IR_CHANNEL_DATA_SOURCE_INT, OB_PERMISSION_READ_WRITE)) {
            device_->setIntProperty(OB_PROP_IR_CHANNEL_DATA_SOURCE_INT, channel);
        }
    } catch (const ob::Error &) {
        // 不支持/失败则继续——部分型号默认通道也能取到一路 IR。
    }
    ob::Pipeline::FrameSetCallback cb = [this](std::shared_ptr<ob::FrameSet> fs) { this->onFrameSet(fs); };
    try {
        std::shared_ptr<ob::Config> config = std::make_shared<ob::Config>();
        if (hasSensor(OB_SENSOR_IR)) {
            config->enableVideoStream(OB_SENSOR_IR);
        } else if (channel == 0 && hasSensor(OB_SENSOR_IR_LEFT)) {
            config->enableVideoStream(OB_SENSOR_IR_LEFT);
        } else if (channel == 1 && hasSensor(OB_SENSOR_IR_RIGHT)) {
            config->enableVideoStream(OB_SENSOR_IR_RIGHT);
        } else if (hasSensor(OB_SENSOR_IR_LEFT)) {
            // 兜底：沿用左 IR sensor，channel 切换交给 IR_CHANNEL_DATA_SOURCE_INT。
            config->enableVideoStream(OB_SENSOR_IR_LEFT);
        }
        pipeline_->start(config, cb);
    } catch (const ob::Error &e) {
        setError(std::string("启动 IR 流失败: ") + e.what());
        return false;
    }
    return true;
}

bool OrbbecCameraDevice::waitNewFrame(uint64_t baseline, int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (frameCount_.load() > baseline) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void OrbbecCameraDevice::stopStream() {
    const CamState s = state_.load();
    if (pipeline_ && (s == CamState::Streaming || s == CamState::Starting)) {
        try {
            pipeline_->stop();
        } catch (const ob::Error &) {
            // 忽略停止异常
        }
    }
    if (s != CamState::Error) {
        state_.store(CamState::Stopped);
    }
    // 释放最新帧引用，避免停止后仍持有 SDK 帧缓冲。
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        latestFrameSet_.reset();
    }
}

bool OrbbecCameraDevice::trigger() {
    if (!device_ || !triggerMode_.load()) {
        return false;
    }
    try {
        device_->triggerCapture();
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

// 一次触发保存的完整分时序列（后台线程，阻塞）：
//   1) 屏障对齐 → 保存预览缓冲里最近的彩色帧 + 其时间信息（主帧，与其他厂商主曝光同瞬间）；
//   2) 停彩色流 → 开 IR channel 0 取左 IR → 停 → 开 IR channel 1 取右 IR → 停；
//   3) 无论成败恢复彩色 free-run 预览。
bool OrbbecCameraDevice::captureTriggerSet(TriggerShot &out, TriggerBarrier &barrier, int perStepTimeoutMs) {
    // 序列结束总是恢复彩色预览（RAII 兜底，覆盖任何提前返回路径）。
    struct PreviewRestorer {
        OrbbecCameraDevice *self;
        ~PreviewRestorer() {
            self->stopStream();
            self->startColorPreview(0);
        }
    } restorer{this};

    // 屏障对齐：与海康/迈德威视的主帧触发尽量同瞬间释放。
    barrier.arrive();
    barrier.wait();

    // 主帧：直接取预览缓冲里最近的彩色帧（spec 要求"保存最近的彩色帧"），并记录其时间信息。
    out.color = latestImage(PreviewStream::Color);
    latestFrameTiming(out.timing);

    // 切到分时取 IR：先停彩色流。
    stopStream();

    // 左 IR：channel 0。
    {
        const uint64_t baseline = frameCount_.load();
        if (startIrChannel(0) && waitNewFrame(baseline, perStepTimeoutMs)) {
            out.left = latestImage(PreviewStream::IR);
        }
        stopStream();
    }

    // 右 IR：channel 1。
    {
        const uint64_t baseline = frameCount_.load();
        if (startIrChannel(1) && waitNewFrame(baseline, perStepTimeoutMs)) {
            out.right = latestImage(PreviewStream::IR);
        }
        stopStream();
    }

    // 至少主帧（彩色）有效即视为成功；IR 缺失不致命（型号/通道差异）。
    out.ok = !out.color.isNull() || !out.left.isNull() || !out.right.isNull();
    return out.ok;
}

// ---------------- 帧回调 ----------------

void OrbbecCameraDevice::onFrameSet(const std::shared_ptr<ob::FrameSet> &frameSet) {
    // 关键：绝不让任何异常逃逸进 SDK 的 C 回调；且回调里只做轻量记录并保存最新帧引用，
    // 不做像素转换（转换推迟到 GUI 线程的 latestImage），避免拖慢回调导致帧在队列里积压。
    if (!frameSet) {
        return;
    }
    try {
        std::shared_ptr<ob::DepthFrame> depth = frameSet->getDepthFrame();
        if (depth) {
            depthW_.store(static_cast<int>(depth->getWidth()));
            depthH_.store(static_cast<int>(depth->getHeight()));
            depthAlive_.store(true);
        }

        std::shared_ptr<ob::VideoFrame> ir = frameSet->getIrFrame();
        if (!ir) {
            std::shared_ptr<ob::Frame> f = frameSet->getFrame(OB_FRAME_IR_LEFT);
            if (f) {
                ir = f->as<ob::VideoFrame>();
            }
        }
        if (ir) {
            irW_.store(static_cast<int>(ir->getWidth()));
            irH_.store(static_cast<int>(ir->getHeight()));
            irAlive_.store(true);
        }

        std::shared_ptr<ob::ColorFrame> color = frameSet->getColorFrame();
        if (color) {
            colorW_.store(static_cast<int>(color->getWidth()));
            colorH_.store(static_cast<int>(color->getHeight()));
            colorAlive_.store(true);
        }

        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            latestFrameSet_ = frameSet;
        }

        // 回退路径用的锚点：首帧到达时记一对 (主机 epoch, 设备硬件时间戳)。
        // 仅当全局时间戳不可用时才会用到；建立一次即可。
        if (!tsAnchorSet_.load()) {
            std::shared_ptr<ob::Frame> rep = frameSet->getColorFrame();
            if (!rep) rep = frameSet->getFrame(OB_FRAME_IR_LEFT);
            if (!rep) rep = frameSet->getFrame(OB_FRAME_IR_RIGHT);
            if (!rep) rep = frameSet->getFrame(OB_FRAME_IR);
            if (!rep) rep = frameSet->getDepthFrame();
            if (rep) {
                devAnchorUs_.store(rep->getTimeStampUs());
                hostAnchorUs_.store(nowEpochUs());
                tsAnchorSet_.store(true);
            }
        }

        frameCount_.fetch_add(1);
    } catch (...) {
        // 吞掉异常，保证不跨越 C 边界。
    }
}

// ---------------- 预览 ----------------

QImage OrbbecCameraDevice::latestImage(PreviewStream stream) const {
    // 取出最新一帧（仅拷贝 shared_ptr），随后在锁外做转换，避免长时间持锁。
    std::shared_ptr<ob::FrameSet> fs;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        fs = latestFrameSet_;
    }
    if (!fs) {
        return QImage();
    }
    try {
        switch (stream) {
        case PreviewStream::Depth:
            return fc::depthToQImage(fs->getDepthFrame());
        case PreviewStream::IR: {
            std::shared_ptr<ob::VideoFrame> ir = fs->getIrFrame();
            if (!ir) {
                std::shared_ptr<ob::Frame> f = fs->getFrame(OB_FRAME_IR_LEFT);
                if (f) {
                    ir = f->as<ob::VideoFrame>();
                }
            }
            return fc::irToQImage(ir);
        }
        case PreviewStream::Color:
            return fc::colorToQImage(fs->getColorFrame());
        case PreviewStream::IRLeft: {
            std::shared_ptr<ob::Frame> f = fs->getFrame(OB_FRAME_IR_LEFT);
            if (f) {
                return fc::irToQImage(f->as<ob::VideoFrame>());
            }
            return QImage();
        }
        case PreviewStream::IRRight: {
            std::shared_ptr<ob::Frame> f = fs->getFrame(OB_FRAME_IR_RIGHT);
            if (f) {
                return fc::irToQImage(f->as<ob::VideoFrame>());
            }
            return QImage();
        }
        }
    } catch (...) {
        return QImage();
    }
    return QImage();
}

std::string OrbbecCameraDevice::resolution(PreviewStream stream) const {
    int w = 0, h = 0;
    switch (stream) {
    case PreviewStream::Depth:   w = depthW_.load(); h = depthH_.load(); break;
    case PreviewStream::IR:      w = irW_.load();    h = irH_.load();    break;
    case PreviewStream::Color:   w = colorW_.load(); h = colorH_.load(); break;
    case PreviewStream::IRLeft:
    case PreviewStream::IRRight: w = irW_.load();    h = irH_.load();    break;
    }
    if (w <= 0 || h <= 0) {
        return std::string();
    }
    return std::to_string(w) + "x" + std::to_string(h);
}

// 只返回"真正产出过非空画面"的流：sensor 存在不等于该流已启用/在出帧
// （启动可能回退到仅深度，或某些格式解码失败），据此选流可避免锁定一路死流。
std::vector<PreviewStream> OrbbecCameraDevice::availablePreviewStreams() const {
    std::vector<PreviewStream> v;
    if (depthAlive_.load()) {
        v.push_back(PreviewStream::Depth);
    }
    if (irAlive_.load()) {
        v.push_back(PreviewStream::IR);
    }
    if (colorAlive_.load()) {
        v.push_back(PreviewStream::Color);
    }
    return v;
}

std::string OrbbecCameraDevice::streamDiagnostics() const {
    std::shared_ptr<ob::FrameSet> fs;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        fs = latestFrameSet_;
    }
    if (!fs) {
        return std::string("尚无帧到达");
    }
    std::string s;
    try {
        std::shared_ptr<ob::ColorFrame> color = fs->getColorFrame();
        if (color) {
            s += "彩色=" + std::to_string(static_cast<int>(color->getWidth())) + "x" +
                 std::to_string(static_cast<int>(color->getHeight())) + "/" +
                 ob::TypeHelper::convertOBFormatTypeToString(color->getFormat());
        } else {
            s += "彩色=无";
        }

        std::shared_ptr<ob::Frame> irl = fs->getFrame(OB_FRAME_IR_LEFT);
        std::shared_ptr<ob::Frame> irr = fs->getFrame(OB_FRAME_IR_RIGHT);
        std::shared_ptr<ob::Frame> ir  = fs->getFrame(OB_FRAME_IR);
        s += std::string(", 左IR=") + (irl ? "有" : "无");
        s += std::string(", 右IR=") + (irr ? "有" : "无");
        s += std::string(", 单IR=") + (ir ? "有" : "无");

        std::shared_ptr<ob::DepthFrame> depth = fs->getDepthFrame();
        s += std::string(", 深度=") + (depth ? "有" : "无");
    } catch (const ob::Error &e) {
        s += std::string(" [诊断异常: ") + e.what() + "]";
    } catch (...) {
        s += " [诊断异常]";
    }
    return s;
}

uint64_t OrbbecCameraDevice::nowEpochUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool OrbbecCameraDevice::latestFrameTiming(FrameTiming &out) const {
    std::shared_ptr<ob::FrameSet> fs;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        fs = latestFrameSet_;
    }
    if (!fs) {
        return false;
    }
    try {
        // 取代表帧并记下其对应的曝光作用对象：彩色 → 左IR → 右IR → 单IR → 深度。
        std::shared_ptr<ob::Frame> f      = fs->getColorFrame();
        ExposureTarget             target = ExposureTarget::Color;
        if (!f) { f = fs->getFrame(OB_FRAME_IR_LEFT);  target = ExposureTarget::IR; }
        if (!f) { f = fs->getFrame(OB_FRAME_IR_RIGHT); target = ExposureTarget::IR; }
        if (!f) { f = fs->getFrame(OB_FRAME_IR);       target = ExposureTarget::IR; }
        if (!f) { f = fs->getDepthFrame();             target = ExposureTarget::Depth; }
        if (!f) {
            return false;
        }

        const uint64_t hwUs = f->getTimeStampUs();

        // 主机域曝光时刻：优先全局时间戳（已消漂移）；否则用首帧锚点换算硬件时间戳。
        uint64_t hostUs = 0;
        if (globalTsEnabled_.load()) {
            const uint64_t g = f->getGlobalTimeStampUs();
            if (g != 0) {
                hostUs = g;
            }
        }
        if (hostUs == 0) {
            if (tsAnchorSet_.load()) {
                hostUs = hostAnchorUs_.load() + (hwUs - devAnchorUs_.load());
            } else {
                hostUs = nowEpochUs();  // 极端兜底：尚未建立锚点时退化为当前主机时间
            }
        }

        out.hostEpochUs  = hostUs;
        out.captureRawUs = hwUs;
        out.exposureUs   = 0;
        int expv = 0;
        if (getExposure(target, expv) && expv > 0) {
            out.exposureUs = static_cast<uint32_t>(expv);
        }
        out.valid = true;
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------- 参数控制 ----------------

bool OrbbecCameraDevice::isTargetSupported(ExposureTarget t) const {
    if (!device_) {
        return false;
    }
    try {
        return device_->isPropertySupported(exposureProp(t), OB_PERMISSION_READ_WRITE);
    } catch (const ob::Error &) {
        return false;
    }
}

std::vector<ExposureTarget> OrbbecCameraDevice::supportedTargets() const {
    std::vector<ExposureTarget> v;
    const ExposureTarget        all[] = {ExposureTarget::Depth, ExposureTarget::IR, ExposureTarget::Color};
    for (int i = 0; i < 3; ++i) {
        if (isTargetSupported(all[i])) {
            v.push_back(all[i]);
        }
    }
    return v;
}

bool OrbbecCameraDevice::supportsAutoExposure(ExposureTarget t) const {
    if (!device_) {
        return false;
    }
    try {
        return device_->isPropertySupported(autoExposureProp(t), OB_PERMISSION_READ_WRITE);
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::getExposureRange(ExposureTarget t, PropertyRange &range) const {
    if (!device_) {
        return false;
    }
    try {
        const OBIntPropertyRange r = device_->getIntPropertyRange(exposureProp(t));
        range.cur  = r.cur;
        range.max  = r.max;
        range.min  = r.min;
        range.step = r.step;
        range.def  = r.def;
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::getGainRange(ExposureTarget t, PropertyRange &range) const {
    if (!device_) {
        return false;
    }
    try {
        const OBIntPropertyRange r = device_->getIntPropertyRange(gainProp(t));
        range.cur  = r.cur;
        range.max  = r.max;
        range.min  = r.min;
        range.step = r.step;
        range.def  = r.def;
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::setAutoExposure(ExposureTarget t, bool enable) {
    if (!device_) {
        return false;
    }
    try {
        device_->setBoolProperty(autoExposureProp(t), enable);
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::getAutoExposure(ExposureTarget t, bool &enabled) const {
    if (!device_) {
        return false;
    }
    try {
        enabled = device_->getBoolProperty(autoExposureProp(t));
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::setExposure(ExposureTarget t, int value) {
    if (!device_) {
        return false;
    }
    try {
        device_->setIntProperty(exposureProp(t), value);
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::getExposure(ExposureTarget t, int &value) const {
    if (!device_) {
        return false;
    }
    try {
        value = device_->getIntProperty(exposureProp(t));
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::setGain(ExposureTarget t, int value) {
    if (!device_) {
        return false;
    }
    try {
        device_->setIntProperty(gainProp(t), value);
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}

bool OrbbecCameraDevice::getGain(ExposureTarget t, int &value) const {
    if (!device_) {
        return false;
    }
    try {
        value = device_->getIntProperty(gainProp(t));
        return true;
    } catch (const ob::Error &) {
        return false;
    }
}
