#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QMetaType>
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

// 一次触发保存的成果（后台线程产出，经信号回到 GUI 线程写盘）。
struct TriggerBatch {
    std::vector<CaptureResult> results;
    uint64_t                   dispatchUs = 0;
    int                        seq        = 0;
};
Q_DECLARE_METATYPE(TriggerBatch)

// 主窗口。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    // 后台触发线程完成时发射，携带成果回到 GUI 线程（Qt::QueuedConnection）。
    void triggerBatchReady(TriggerBatch batch);

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
    void onTriggerSave();  // 后台线程对所有采集中相机发起触发保存序列
    void onCalibrateClocks();  // 时间戳标定：对齐各机内部时钟到同一主机轴
    void onTick();

    // 触发保存：onTriggerSave 起后台线程跑 manager_.captureTriggerSets（阻塞，含 Orbbec 分时序列），
    // 完成后经 triggerBatchReady 信号回到 GUI 线程，由 saveBatch 统一写盘并输出时间戳 sidecar。
    void saveBatch(const TriggerBatch &batch);

    // 槽位分配：按厂商把设备路由到固定槽（Orbbec→0/1/2，海康→3，迈德威视左→4/右→5）。
    // 返回 (槽号, 该槽显示的预览流)。同厂商多台按连接顺序占位。
    void assignSlots();

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
    QPushButton *btnCalibrate_ = nullptr;  // 时间戳标定

    // 参数面板
    QComboBox *targetCombo_   = nullptr;
    QCheckBox *autoExpCheck_  = nullptr;
    QSlider   *expSlider_     = nullptr;
    QSpinBox  *expSpin_       = nullptr;
    QSlider   *gainSlider_    = nullptr;
    QSpinBox  *gainSpin_      = nullptr;

    // 日志
    QPlainTextEdit *logEdit_ = nullptr;

    // 预览（固定 2×3 六槽）：按厂商映射——槽0/1/2=Orbbec 彩色，槽3=海康彩色，
    // 槽4=迈德威视左目，槽5=迈德威视右目。每槽记录显示哪台设备的哪一路流。
    QGridLayout *previewGrid_ = nullptr;
    CameraView  *slots_[6]    = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    // 槽 → (设备, 预览流)。assignSlots 重建；onTick 据此刷新各槽。
    std::shared_ptr<ICameraDevice> slotDev_[6];
    PreviewStream                  slotStream_[6];

    // FPS 计算（每1秒更新一次）
    std::map<std::string, uint64_t> lastFrameCount_;
    std::map<std::string, qint64>   lastFpsUpdateTime_;
    std::map<std::string, double>   fps_;

    bool updatingParam_ = false;  // 防止填充控件时触发回调
    int  triggerSeq_    = 0;      // 触发保存的递增序号（每次点击+1）
    std::set<std::string> diagLogged_;  // 已打印过流诊断的相机（每次采集打印一次）

    // 后台触发线程：onTriggerSave 起线程跑阻塞的 captureTriggerSets，完成后发 triggerBatchReady。
    std::thread       triggerThread_;
    std::atomic<bool> triggerBusy_;  // 防重入：上一次触发未完成时不再起新线程
};

#endif  // MAIN_WINDOW_H
