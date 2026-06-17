#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QWidget>

#include "CameraManager.h"

class QTableWidget;
class QPushButton;
class QSpinBox;
class QSlider;
class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QGridLayout;
class QLabel;
class QTimer;
class QScrollArea;

// 单台相机的固定预览块：编号 + 画面 + 信息标签。画面区固定 480×360。无需 moc。
class CameraView : public QWidget {
public:
    explicit CameraView(int index, QWidget *parent = nullptr);

    void showImage(const QImage &img);
    void setInfo(const QString &text);
    void clear();  // 无对应相机时置空（显示"无画面"）

private:
    int     index_;
    QLabel *imageLabel_;
    QLabel *infoLabel_;
};

// 主窗口。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    // 构建界面
    QWidget *buildCapturePanel();
    QWidget *buildParamPanel();
    QWidget *buildLogPanel();

    // 业务动作（连接/断开均针对所有相机）
    void onConnectAll();
    void onDisconnectAll();
    void onStartCapture();
    void onStopCapture();
    void onTriggerSave();  // 对所有相机同时下发软触发，等齐各路新帧后保存并记录时间戳
    void onTick();

    // 触发保存流程（非阻塞）：onTriggerSave 下发触发并起轮询定时器；
    // pollTriggeredFrames 等每台出"这次触发的新帧"后抓取图像+时间戳；
    // 全部就绪或超时后 finishShot 统一写盘并输出时间戳 sidecar。
    void pollTriggeredFrames();
    void finishShot();

    // 参数面板
    void populateParamPanel();   // 依据选中相机重建"目标"下拉
    void loadParamValues();      // 读取当前目标的自动曝光/曝光/增益并填充控件
    void applyAutoExposure(bool enable);
    void applyExposure(int value);
    void applyGain(int value);
    std::vector<std::shared_ptr<ICameraDevice>> paramTargets();  // 应用对象（选中或全部）

    // 辅助
    void                          log(const QString &msg, bool isError = false);
    std::shared_ptr<ICameraDevice> firstConnectedCamera() const;  // 参数面板取值来源（第一台已连接相机）
    ExposureTarget                currentTarget() const;
    void                          updateButtonStates();
    static QString                stateText(CamState s);

    CameraManager manager_;
    QTimer       *timer_ = nullptr;

    // 连接（全局，针对所有相机）
    QPushButton  *btnConnectAll_    = nullptr;
    QPushButton  *btnDisconnectAll_ = nullptr;

    // 采集面板
    QPushButton *btnStart_     = nullptr;
    QPushButton *btnStop_      = nullptr;
    QPushButton *btnTrigger_   = nullptr;  // 触发保存
    QSpinBox    *intervalSpin_ = nullptr;

    // 参数面板
    QComboBox *targetCombo_   = nullptr;
    QCheckBox *autoExpCheck_  = nullptr;
    QSlider   *expSlider_     = nullptr;
    QSpinBox  *expSpin_       = nullptr;
    QSlider   *gainSlider_    = nullptr;
    QSpinBox  *gainSpin_      = nullptr;

    // 日志
    QPlainTextEdit *logEdit_ = nullptr;

    // 预览（固定 2×2 四槽，槽 i 显示第 i 台已连接相机，无则置空）
    QGridLayout *previewGrid_ = nullptr;
    CameraView  *slots_[4]    = {nullptr, nullptr, nullptr, nullptr};

    // FPS 计算（每1秒更新一次）
    std::map<std::string, uint64_t> lastFrameCount_;
    std::map<std::string, qint64>   lastFpsUpdateTime_;
    std::map<std::string, double>   fps_;

    bool updatingParam_ = false;  // 防止填充控件时触发回调
    int  triggerSeq_    = 0;      // 触发保存的递增序号（每次点击+1）
    std::set<std::string> diagLogged_;  // 已打印过流诊断的相机（每次采集打印一次）

    // 一次"触发保存"中单台相机的待收集状态。
    struct PendingShot {
        std::shared_ptr<ICameraDevice> dev;
        uint64_t    baselineFrameCount = 0;  // 触发前的帧计数；超过它即为本次触发的新帧
        bool        ready              = false;
        bool        timedOut           = false;
        QImage      color;
        QImage      left;
        QImage      right;
        FrameTiming timing;
    };

    // 进行中的触发保存会话（GUI 线程独占，无需加锁）。
    QTimer                  *triggerPollTimer_ = nullptr;  // 轮询新帧的定时器
    std::vector<PendingShot> pendingShots_;
    int                      pendingSeq_        = 0;
    uint64_t                 pendingTriggerUs_  = 0;  // triggerAll 返回的派发时刻
    qint64                   pendingDeadlineMs_ = 0;  // 等帧截止时刻（主机 ms）
};

#endif  // MAIN_WINDOW_H
