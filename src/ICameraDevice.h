#ifndef I_CAMERA_DEVICE_H
#define I_CAMERA_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include <QImage>

#include "CameraTypes.h"

class TriggerBarrier;

// 相机设备的抽象接口。每个厂商 SDK 提供一个实现（如 OrbbecCameraDevice），
// 由对应的 ICameraBackend 创建；界面与 CameraManager 只依赖本接口，
// 不接触任何厂商 SDK 类型。
//
// 实现必须遵守的线程约定（与整个工程的线程模型一致）：
//  - 帧经 SDK 内部线程回调到达，实现内部把帧转成 QImage 并加锁缓存；
//    GUI 线程通过 latestImage() 取走副本显示。
//  - 对外状态用原子量或互斥量保护；实现不得触碰任何 Qt GUI 对象。
//  - 任何 C++ 异常不得逃逸进 SDK 的 C 回调边界。
class ICameraDevice {
public:
    virtual ~ICameraDevice() {}

    // --- 设备信息（构造时缓存，线程安全只读） ---
    virtual const std::string &vendor() const         = 0;  // 所属后端标识
    virtual const std::string &name() const           = 0;
    virtual const std::string &serial() const         = 0;
    virtual const std::string &uid() const            = 0;
    virtual const std::string &connectionType() const = 0;
    virtual int                pid() const            = 0;

    // --- 采集控制 ---
    // 启动采集。triggerMode=true 时配置为软触发模式（需调用 trigger() 才出帧）。
    // 失败返回 false 并可通过 lastError() 获取信息。
    virtual bool startStream(bool triggerMode) = 0;
    virtual void stopStream()                  = 0;
    virtual bool isStreaming() const           = 0;

    virtual CamState    state() const      = 0;
    virtual uint64_t    frameCount() const = 0;
    virtual std::string lastError() const  = 0;

    // 发送一次软触发抓拍（仅在软触发模式下有效）。
    virtual bool trigger() = 0;

    // 一次"触发保存"的完整采集序列（阻塞，必须在后台线程调用，不可在 GUI 线程调）。
    // 实现需遵守：先做完各自准备（切软触发模式、必要时停/重启取流），随后调 barrier.arrive()
    // 并 barrier.wait() 在同一屏障上等齐；wait() 返回后立即执行"主帧采集动作"——下发软触发
    // （海康/迈德威视）或抓取预览缓冲中的最近帧（Orbbec 彩色），使各机主曝光时刻尽量靠拢。
    // 之后再各自完成剩余分时采集（如 Orbbec 切 IR channel 取左右）。结果写入 out；
    // 序列结束须恢复到原预览态（如 Orbbec 恢复彩色 free-run 预览）。
    // 关键：无论准备成功与否，arrive() 必须恰好调用一次，否则控制线程会死等。
    // perStepTimeoutMs 为每一步等待新帧的超时（毫秒）。返回 out.ok。
    virtual bool captureTriggerSet(TriggerShot &out, TriggerBarrier &barrier, int perStepTimeoutMs) = 0;

    // --- 参数控制 ---
    // 曝光/增益的取值区间由 get*Range 给出，具体单位由实现定义。
    virtual std::vector<ExposureTarget> supportedTargets() const             = 0;
    virtual bool isTargetSupported(ExposureTarget t) const                   = 0;

    virtual bool getExposureRange(ExposureTarget t, PropertyRange &range) const = 0;
    virtual bool getGainRange(ExposureTarget t, PropertyRange &range) const     = 0;
    virtual bool supportsAutoExposure(ExposureTarget t) const                   = 0;

    virtual bool setAutoExposure(ExposureTarget t, bool enable)         = 0;
    virtual bool getAutoExposure(ExposureTarget t, bool &enabled) const = 0;
    virtual bool setExposure(ExposureTarget t, int value)               = 0;
    virtual bool getExposure(ExposureTarget t, int &value) const        = 0;
    virtual bool setGain(ExposureTarget t, int value)                   = 0;
    virtual bool getGain(ExposureTarget t, int &value) const            = 0;

    // --- 预览 ---
    // 返回指定流最近一帧的 QImage 副本（无帧时返回空 QImage）。
    virtual QImage latestImage(PreviewStream stream) const = 0;
    // 返回指定流的分辨率字符串，如 "640x400"，无则返回空串。
    virtual std::string resolution(PreviewStream stream) const = 0;
    // 当前真正在出帧（已产出过非空画面）的预览流；据此选择显示哪一路。
    // 注意：与"设备拥有哪些 sensor"不同——某路 sensor 存在但未启用/解码失败时不会列出。
    virtual std::vector<PreviewStream> availablePreviewStreams() const = 0;
    // 调试用：返回最新一帧里各流的有无与彩色格式，便于排查"看不到画面/存不了图"。
    virtual std::string streamDiagnostics() const = 0;
    // 最新一帧代表帧的时间信息（见 FrameTiming）。hostEpochUs 落在主机 epoch 轴上，
    // 跨厂商可直接相减得曝光时刻差；无帧时返回 false。
    virtual bool latestFrameTiming(FrameTiming &out) const = 0;

    // 时间戳标定：把本设备内部时钟对齐到主机时间轴。hostRefUs 为标定基准时刻
    // （主机 system_clock epoch 微秒，由调用方在同一屏障上统一取值，保证多机共用一基准）。
    // 实现可重置/锚定设备时钟，使此后各帧的 hostEpochUs 落在 hostRefUs 同一条轴上，
    // 从而触发时各机帧时间戳可直接相减，间接反映曝光时刻差。
    // 不支持的实现保持默认空实现（依赖既有的全局时间戳/锚点换算）。
    virtual void calibrateClock(uint64_t hostRefUs) { (void)hostRefUs; }

    // 周期时钟再锚（由 CameraManager 后台线程定期调用）：令本设备用"下一帧到达时刻的真实主机
    // 时间"重新锚定其内部时钟，约束长时间漂移。各机独立用真实主机时刻锚定，相对比较仍有效，
    // 无需屏障同步。不支持的实现保持默认空实现。
    virtual void syncClock() {}
};

#endif  // I_CAMERA_DEVICE_H
