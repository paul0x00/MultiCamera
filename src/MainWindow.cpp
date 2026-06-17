#include "MainWindow.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "AppTheme.h"

// ============================ CameraView ============================

CameraView::CameraView(int index, QWidget *parent) : QWidget(parent), index_(index) {
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(2);

    infoLabel_ = new QLabel(this);
    infoLabel_->setStyleSheet("font-size:11px;color:#64748B;");
    infoLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    v->addWidget(infoLabel_, 0);

    imageLabel_ = new QLabel(this);
    imageLabel_->setFixedSize(480, 270);  // 固定大小（16:9），4 块按 2×2 排布
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setStyleSheet("background:#1e1e1e;color:#888;border:1px solid #444;");
    v->addWidget(imageLabel_, 0);

    clear();  // 初始为空槽：显示"相机 N  无图像"
}

void CameraView::showImage(const QImage &img) {
    if (img.isNull()) {
        return;
    }
    // 拉伸填满画面区，不留黑边（比例与相机输出不一致时画面会轻微变形）。
    imageLabel_->setPixmap(QPixmap::fromImage(img).scaled(imageLabel_->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void CameraView::setInfo(const QString &text) {
    infoLabel_->setText(text);
}

void CameraView::clear() {
    imageLabel_->setText(QString::fromUtf8("无画面"));  // setText 会自动清除已设置的 pixmap
    setInfo(QString::fromUtf8("相机 %1  无图像").arg(index_));
}

// ============================ MainWindow ============================

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8("Gemini 215 多相机控制台"));
    resize(1340, 800);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *outer = new QVBoxLayout(central);

    QHBoxLayout *row = new QHBoxLayout();
    outer->addLayout(row, 1);

    // 左：固定 2×2 预览（四槽，编号 1~4；槽 i 显示第 i 台已连接相机）
    QWidget *previewContainer = new QWidget(this);
    previewGrid_              = new QGridLayout(previewContainer);
    previewGrid_->setContentsMargins(6, 6, 6, 6);
    previewGrid_->setSpacing(6);
    const int cols = 2;
    for (int i = 0; i < 4; ++i) {
        slots_[i] = new CameraView(i + 1, previewContainer);
        previewGrid_->addWidget(slots_[i], i / cols, i % cols);
    }
    row->addWidget(previewContainer, 1, Qt::AlignTop | Qt::AlignLeft);

    // 右：采集 + 参数
    QWidget     *right = new QWidget(this);
    QVBoxLayout *rl    = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(buildCapturePanel(), 0);
    rl->addWidget(buildParamPanel(), 0);
    rl->addStretch(1);
    right->setFixedWidth(300);
    row->addWidget(right, 0);

    // 底：日志
    outer->addWidget(buildLogPanel(), 0);

    // 定时刷新（约 30fps）
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this]() { onTick(); });
    timer_->start(33);

    populateParamPanel();
    updateButtonStates();

    // 启动即枚举所有厂商设备并在日志列出（不自动连接，连接由"全部连接"触发）。
    {
        std::vector<DiscoveredDevice> list = manager_.refresh();
        if (list.empty()) {
            const std::string err = manager_.lastError();
            if (!err.empty()) {
                log(QString::fromUtf8("启动枚举：") + QString::fromStdString(err), true);
            } else {
                log(QString::fromUtf8("启动枚举：未发现相机设备。点击\"全部连接\"可重试。"));
            }
        } else {
            log(QString::fromUtf8("启动枚举：发现 %1 台相机：").arg(static_cast<int>(list.size())));
            for (size_t i = 0; i < list.size(); ++i) {
                log(QString::fromUtf8("  [%1] %2  序列号=%3  接口=%4")
                        .arg(QString::fromStdString(list[i].vendor))
                        .arg(QString::fromStdString(list[i].name))
                        .arg(QString::fromStdString(list[i].serial))
                        .arg(QString::fromStdString(list[i].connectionType)));
            }
            log(QString::fromUtf8("点击\"全部连接\"连接所有相机，再\"开始采集\"。"));
        }
    }
}

MainWindow::~MainWindow() {
    if (timer_) {
        timer_->stop();
    }
    // manager_ 析构会停止采集并断开所有设备
}

// ---------------------------- 面板构建 ----------------------------

QWidget *MainWindow::buildCapturePanel() {
    QGroupBox   *g = new QGroupBox(QString::fromUtf8("采集控制"), this);
    QVBoxLayout *v = new QVBoxLayout(g);

    // 连接（全局，针对所有相机）
    QHBoxLayout *r0   = new QHBoxLayout();
    btnConnectAll_    = new QPushButton(QString::fromUtf8("全部连接"), g);
    btnDisconnectAll_ = new QPushButton(QString::fromUtf8("全部断开"), g);
    r0->addWidget(btnConnectAll_);
    r0->addWidget(btnDisconnectAll_);
    v->addLayout(r0);

    QHBoxLayout *r1 = new QHBoxLayout();
    btnStart_       = new QPushButton(QString::fromUtf8("开始采集"), g);
    btnStart_->setProperty("primary", true);  // 橙色主操作样式（见 AppTheme.h）
    btnStop_        = new QPushButton(QString::fromUtf8("停止采集"), g);
    r1->addWidget(btnStart_);
    r1->addWidget(btnStop_);
    v->addLayout(r1);

    btnCalibrate_ = new QPushButton(QString::fromUtf8("时间戳标定"), g);
    btnCalibrate_->setToolTip(QString::fromUtf8("对齐所有相机的内部时钟到同一主机时间轴；触发保存时各路帧时间戳可直接相减，反映曝光时刻差。可随时重复标定。"));
    v->addWidget(btnCalibrate_);

    btnTrigger_ = new QPushButton(QString::fromUtf8("触发保存"), g);
    btnTrigger_->setProperty("primary", true);
    btnTrigger_->setToolTip(QString::fromUtf8("对所有相机同时下发软触发，等齐各路新帧后保存彩色/左IR/右IR并记录曝光时间戳"));
    v->addWidget(btnTrigger_);

    connect(btnConnectAll_, &QPushButton::clicked, this, [this]() { onConnectAll(); });
    connect(btnDisconnectAll_, &QPushButton::clicked, this, [this]() { onDisconnectAll(); });
    connect(btnStart_, &QPushButton::clicked, this, [this]() { onStartCapture(); });
    connect(btnStop_, &QPushButton::clicked, this, [this]() { onStopCapture(); });
    connect(btnCalibrate_, &QPushButton::clicked, this, [this]() { onCalibrateClocks(); });
    connect(btnTrigger_, &QPushButton::clicked, this, [this]() { onTriggerSave(); });

    return g;
}

QWidget *MainWindow::buildParamPanel() {
    QGroupBox   *g = new QGroupBox(QString::fromUtf8("参数调节"), this);
    QFormLayout *f = new QFormLayout(g);

    targetCombo_ = new QComboBox(g);
    f->addRow(QString::fromUtf8("作用对象"), targetCombo_);

    autoExpCheck_ = new QCheckBox(QString::fromUtf8("自动曝光"), g);
    f->addRow(QString(), autoExpCheck_);

    QHBoxLayout *expRow = new QHBoxLayout();
    expSlider_          = new QSlider(Qt::Horizontal, g);
    expSpin_            = new QSpinBox(g);
    expSpin_->setRange(0, 1000000);
    expRow->addWidget(expSlider_, 1);
    expRow->addWidget(expSpin_, 0);
    f->addRow(QString::fromUtf8("曝光"), expRow);

    QHBoxLayout *gainRow = new QHBoxLayout();
    gainSlider_          = new QSlider(Qt::Horizontal, g);
    gainSpin_            = new QSpinBox(g);
    gainSpin_->setRange(0, 1000000);
    gainRow->addWidget(gainSlider_, 1);
    gainRow->addWidget(gainSpin_, 0);
    f->addRow(QString::fromUtf8("增益"), gainRow);

    // 目标切换：重新读取该目标的当前值
    connect(targetCombo_, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!updatingParam_) {
            loadParamValues();
        }
    });
    // 自动曝光开关
    connect(autoExpCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updatingParam_) {
            applyAutoExposure(on);
        }
    });
    // 曝光滑条 <-> 数字框 同步并应用
    connect(expSlider_, &QSlider::valueChanged, this, [this](int val) {
        if (updatingParam_) {
            return;
        }
        QSignalBlocker b(expSpin_);
        expSpin_->setValue(val);
        applyExposure(val);
    });
    connect(expSpin_, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this](int val) {
        if (updatingParam_) {
            return;
        }
        QSignalBlocker b(expSlider_);
        expSlider_->setValue(val);
        applyExposure(val);
    });
    // 增益滑条 <-> 数字框
    connect(gainSlider_, &QSlider::valueChanged, this, [this](int val) {
        if (updatingParam_) {
            return;
        }
        QSignalBlocker b(gainSpin_);
        gainSpin_->setValue(val);
        applyGain(val);
    });
    connect(gainSpin_, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this](int val) {
        if (updatingParam_) {
            return;
        }
        QSignalBlocker b(gainSlider_);
        gainSlider_->setValue(val);
        applyGain(val);
    });

    return g;
}

QWidget *MainWindow::buildLogPanel() {
    QGroupBox   *g = new QGroupBox(QString::fromUtf8("日志"), this);
    QVBoxLayout *v = new QVBoxLayout(g);
    logEdit_       = new QPlainTextEdit(g);
    logEdit_->setReadOnly(true);
    logEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logEdit_->setMaximumBlockCount(500);
    logEdit_->setFixedHeight(140);
    v->addWidget(logEdit_);
    return g;
}

// ---------------------------- 业务动作 ----------------------------

void MainWindow::onConnectAll() {
    // 没有设备列表了：先枚举一次，再连接全部。
    std::vector<DiscoveredDevice> list = manager_.refresh();
    if (list.empty()) {
        const std::string err = manager_.lastError();
        if (!err.empty()) {
            log(QString::fromUtf8("枚举失败：") + QString::fromStdString(err), true);
        } else {
            log(QString::fromUtf8("未发现相机设备。"));
        }
        populateParamPanel();
        updateButtonStates();
        return;
    }
    std::vector<std::string> errors;
    const int                ok = manager_.connectAll(errors);
    log(QString::fromUtf8("枚举到 %1 台，已连接 %2 台。").arg(static_cast<int>(list.size())).arg(ok));
    for (size_t i = 0; i < errors.size(); ++i) {
        log(QString::fromStdString(errors[i]), true);
    }
    populateParamPanel();
    updateButtonStates();
}

void MainWindow::onDisconnectAll() {
    manager_.disconnectAll();
    log(QString::fromUtf8("已断开全部设备。"));
    populateParamPanel();
    updateButtonStates();
}

void MainWindow::onStartCapture() {
    if (manager_.connectedCount() == 0) {
        log(QString::fromUtf8("没有已连接的相机，无法开始采集。"));
        return;
    }
    manager_.startCapture(false);  // 连续模式，实时预览
    log(QString::fromUtf8("开始采集：所有相机连续模式。"));
    updateButtonStates();
}

void MainWindow::onCalibrateClocks() {
    if (manager_.connectedCount() == 0) {
        log(QString::fromUtf8("没有已连接的相机，无法标定。"));
        return;
    }
    const uint64_t ref = manager_.calibrateClocks();
    log(QString::fromUtf8("时间戳标定完成：%1 台相机已对齐到主机基准 %2 µs。")
            .arg(static_cast<int>(manager_.connectedCount()))
            .arg(static_cast<qulonglong>(ref)));
    updateButtonStates();
}

void MainWindow::onStopCapture() {
    manager_.stopCapture();
    diagLogged_.clear();  // 下次采集重新打印一次流诊断
    log(QString::fromUtf8("已停止采集。"));
    updateButtonStates();
}

namespace {
// 文件名安全化：替换非法字符、空格与逗号（逗号会破坏 sidecar CSV）。
QString sanitizeModel(const std::string &raw) {
    QString       model = QString::fromStdString(raw);
    const QString bad   = QString::fromUtf8("\\/:*?\"<>| ,");
    for (int k = 0; k < model.size(); ++k) {
        if (bad.contains(model.at(k))) {
            model[k] = QChar('_');
        }
    }
    if (model.isEmpty()) {
        model = QString::fromUtf8("camera");
    }
    return model;
}
}  // namespace

void MainWindow::onTriggerSave() {
    if (triggerPollTimer_ && triggerPollTimer_->isActive()) {
        log(QString::fromUtf8("上一次触发仍在等待出帧，请稍候。"), true);
        return;
    }

    std::vector<std::shared_ptr<ICameraDevice>> devs = manager_.connectedDevices();
    if (devs.empty()) {
        log(QString::fromUtf8("没有已连接的相机，无法保存。"));
        return;
    }

    const int seq = ++triggerSeq_;

    // 输出目录（可执行文件所在目录下）
    const QString base = QCoreApplication::applicationDirPath();
    QDir().mkpath(base + "/middlecolor");
    QDir().mkpath(base + "/leftir");
    QDir().mkpath(base + "/rightir");
    QDir().mkpath(base + "/timestamps");

    // 建立本次触发的待收集列表 + 触发前帧计数基线（超过它即为本次触发产生的新帧）
    pendingShots_.clear();
    pendingShots_.reserve(devs.size());
    for (size_t i = 0; i < devs.size(); ++i) {
        PendingShot s;
        s.dev                = devs[i];
        s.baselineFrameCount = devs[i]->frameCount();
        pendingShots_.push_back(s);
    }
    pendingSeq_ = seq;

    // 并行屏障：尽可能同时下发软触发，记录派发时刻
    pendingTriggerUs_  = manager_.triggerAll();
    pendingDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + 500;  // 最多等 500ms

    log(QString::fromUtf8("触发 #%1：已对 %2 台同时下发软触发，等待各相机出帧…")
            .arg(seq)
            .arg(static_cast<int>(devs.size())));

    if (!triggerPollTimer_) {
        triggerPollTimer_ = new QTimer(this);
        triggerPollTimer_->setInterval(10);
        connect(triggerPollTimer_, &QTimer::timeout, this, [this]() { pollTriggeredFrames(); });
    }
    triggerPollTimer_->start();
    pollTriggeredFrames();  // 立即查一次（帧可能已到）
}

void MainWindow::pollTriggeredFrames() {
    const bool deadlineHit = QDateTime::currentMSecsSinceEpoch() >= pendingDeadlineMs_;
    bool       allReady    = true;

    for (size_t i = 0; i < pendingShots_.size(); ++i) {
        PendingShot &s = pendingShots_[i];
        if (s.ready || s.timedOut) {
            continue;
        }
        if (s.dev->frameCount() > s.baselineFrameCount) {
            // 本次触发产生的新帧已到：抓取图像与时间信息
            s.color = s.dev->latestImage(PreviewStream::Color);
            s.left  = s.dev->latestImage(PreviewStream::IRLeft);
            s.right = s.dev->latestImage(PreviewStream::IRRight);
            s.dev->latestFrameTiming(s.timing);
            s.ready = true;
        } else {
            allReady = false;
        }
    }

    if (!allReady && !deadlineHit) {
        return;  // 继续等
    }
    if (deadlineHit) {
        for (size_t i = 0; i < pendingShots_.size(); ++i) {
            if (!pendingShots_[i].ready) {
                pendingShots_[i].timedOut = true;
            }
        }
    }
    triggerPollTimer_->stop();
    finishShot();
}

void MainWindow::finishShot() {
    const QString base     = QCoreApplication::applicationDirPath();
    const QString colorDir = base + "/middlecolor";
    const QString leftDir  = base + "/leftir";
    const QString rightDir = base + "/rightir";
    const QString tsDir    = base + "/timestamps";
    const QString seqStr   = QString("%1").arg(pendingSeq_, 4, 10, QChar('0'));

    // 以最早曝光时刻为基准，计算各机相对差值
    uint64_t earliest     = 0;
    bool     haveEarliest = false;
    for (size_t i = 0; i < pendingShots_.size(); ++i) {
        const PendingShot &s = pendingShots_[i];
        if (s.ready && s.timing.valid) {
            if (!haveEarliest || s.timing.hostEpochUs < earliest) {
                earliest     = s.timing.hostEpochUs;
                haveEarliest = true;
            }
        }
    }

    // sidecar：记录本次触发各机的曝光时刻（主机 epoch µs）与两两相对差，供后处理。
    QString csv;
    csv += QString::fromUtf8("# trigger_seq=%1, dispatch_host_us=%2\n")
               .arg(seqStr)
               .arg(static_cast<qulonglong>(pendingTriggerUs_));
    csv += QString::fromUtf8("serial,name,host_epoch_us,capture_raw_us,exposure_us,delta_to_earliest_us,status\n");

    int      savedCount = 0;
    uint64_t maxDelta   = 0;

    for (size_t i = 0; i < pendingShots_.size(); ++i) {
        PendingShot  &s      = pendingShots_[i];
        const QString model  = sanitizeModel(s.dev->name());
        const QString serial = QString::fromStdString(s.dev->serial());

        if (!s.ready) {
            log(QString::fromUtf8("相机 %1 超时未出帧，未保存。").arg(serial), true);
            csv += QString("%1,%2,,,,,timeout\n").arg(serial).arg(model);
            continue;
        }

        const uint64_t hostUs = s.timing.valid ? s.timing.hostEpochUs : 0;
        const QString  tsStr  = QString::number(static_cast<qulonglong>(hostUs));
        const QString  file   = model + "_" + serial + "_" + seqStr + "_" + tsStr + ".png";

        bool any = false;
        if (!s.color.isNull() && s.color.save(colorDir + "/" + file, "PNG")) any = true;
        if (!s.left.isNull() && s.left.save(leftDir + "/" + file, "PNG")) any = true;
        if (!s.right.isNull() && s.right.save(rightDir + "/" + file, "PNG")) any = true;
        if (any) {
            ++savedCount;
        }

        const uint64_t delta = (haveEarliest && s.timing.valid) ? (hostUs - earliest) : 0;
        if (s.timing.valid && delta > maxDelta) {
            maxDelta = delta;
        }

        csv += QString("%1,%2,%3,%4,%5,%6,%7\n")
                   .arg(serial)
                   .arg(model)
                   .arg(static_cast<qulonglong>(hostUs))
                   .arg(static_cast<qulonglong>(s.timing.captureRawUs))
                   .arg(s.timing.exposureUs)
                   .arg(static_cast<qulonglong>(delta))
                   .arg(any ? QString("ok") : QString("noframe"));
    }

    QFile f(tsDir + "/trigger_" + seqStr + ".csv");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(csv.toUtf8());
        f.close();
    }

    log(QString::fromUtf8("触发 #%1 完成：保存 %2/%3 台，最大曝光时刻差 %4 ms。")
            .arg(seqStr)
            .arg(savedCount)
            .arg(static_cast<int>(pendingShots_.size()))
            .arg(maxDelta / 1000.0, 0, 'f', 1));

    pendingShots_.clear();
}

void MainWindow::onTick() {
    std::vector<std::shared_ptr<ICameraDevice>> devs = manager_.connectedDevices();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 计算每台相机 FPS（每1秒更新一次）
    for (size_t i = 0; i < devs.size(); ++i) {
        const std::string &sn = devs[i]->serial();
        const uint64_t     c  = devs[i]->frameCount();

        // 首次遇到该相机，只记录初始状态
        if (lastFrameCount_.count(sn) == 0) {
            lastFrameCount_[sn]      = c;
            lastFpsUpdateTime_[sn]   = now;
            fps_[sn]                 = 0.0;
            continue;
        }

        // 计算距离上次FPS更新的时间间隔
        const qint64 lastUpdate = lastFpsUpdateTime_[sn];
        const qint64 elapsed    = now - lastUpdate;

        // 每隔1秒（1000ms）更新一次FPS
        if (elapsed >= 1000) {
            const uint64_t prev = lastFrameCount_[sn];
            const uint64_t diff = (c >= prev) ? (c - prev) : 0;
            const double   dt   = elapsed / 1000.0;

            // 真实帧率 = 时间段内的帧数 / 时间长度
            fps_[sn] = static_cast<double>(diff) / dt;

            // 更新记录
            lastFrameCount_[sn]    = c;
            lastFpsUpdateTime_[sn] = now;
        }
    }

    // 固定 4 槽：第 i 槽显示第 i 台已连接相机的彩色流，多出的槽置空（"无图像"）。
    for (int i = 0; i < 4; ++i) {
        CameraView *view = slots_[i];
        if (i >= static_cast<int>(devs.size())) {
            view->clear();
            continue;
        }
        const std::string &sn = devs[i]->serial();

        // 采集后首次拿到帧时，打印一次流诊断，便于排查"看不到画面/存不了图"。
        if (devs[i]->frameCount() > 0 && diagLogged_.find(sn) == diagLogged_.end()) {
            diagLogged_.insert(sn);
            log(QString::fromUtf8("相机 %1（%2）流诊断：%3")
                    .arg(i + 1)
                    .arg(QString::fromStdString(sn))
                    .arg(QString::fromStdString(devs[i]->streamDiagnostics())));
        }

        // 预览只显示彩色流。
        QImage img = devs[i]->latestImage(PreviewStream::Color);
        if (!img.isNull()) {
            view->showImage(img);
        }
        QString info = QString::fromUtf8("相机 %1  ").arg(i + 1) + QString::fromStdString(sn) + "  " + stateText(devs[i]->state());
        if (devs[i]->isStreaming()) {
            info += QString::fromUtf8("  %1fps").arg(fps_.count(sn) ? fps_[sn] : 0.0, 0, 'f', 1);
            const std::string res = devs[i]->resolution(PreviewStream::Color);
            if (!res.empty()) {
                info += "  " + QString::fromStdString(res);
            }
        }
        view->setInfo(info);
    }

    updateButtonStates();
}

// ---------------------------- 参数面板 ----------------------------

void MainWindow::populateParamPanel() {
    updatingParam_ = true;
    targetCombo_->clear();
    std::shared_ptr<ICameraDevice> cam = firstConnectedCamera();
    if (cam) {
        std::vector<ExposureTarget> targets = cam->supportedTargets();
        for (size_t i = 0; i < targets.size(); ++i) {
            targetCombo_->addItem(QString::fromUtf8(exposureTargetName(targets[i])), static_cast<int>(targets[i]));
        }
    }
    updatingParam_ = false;
    loadParamValues();
}

void MainWindow::loadParamValues() {
    updatingParam_ = true;

    std::shared_ptr<ICameraDevice> cam       = firstConnectedCamera();
    const bool                    hasTarget = (cam && targetCombo_->count() > 0);

    if (!hasTarget) {
        autoExpCheck_->setEnabled(false);
        expSlider_->setEnabled(false);
        expSpin_->setEnabled(false);
        gainSlider_->setEnabled(false);
        gainSpin_->setEnabled(false);
        targetCombo_->setEnabled(false);
        updatingParam_ = false;
        return;
    }

    targetCombo_->setEnabled(true);
    const ExposureTarget t = currentTarget();

    // 自动曝光
    bool       autoOn  = false;
    const bool hasAuto = cam->supportsAutoExposure(t);
    autoExpCheck_->setEnabled(hasAuto);
    if (hasAuto && cam->getAutoExposure(t, autoOn)) {
        autoExpCheck_->setChecked(autoOn);
    }

    // 曝光范围与当前值
    PropertyRange er;
    const bool    hasExp = cam->getExposureRange(t, er);
    if (hasExp) {
        int cur = er.cur;
        cam->getExposure(t, cur);
        expSlider_->setRange(er.min, er.max);
        expSpin_->setRange(er.min, er.max);
        expSlider_->setValue(cur);
        expSpin_->setValue(cur);
    }

    // 增益范围与当前值
    PropertyRange gr;
    const bool    hasGain = cam->getGainRange(t, gr);
    if (hasGain) {
        int cur = gr.cur;
        cam->getGain(t, cur);
        gainSlider_->setRange(gr.min, gr.max);
        gainSpin_->setRange(gr.min, gr.max);
        gainSlider_->setValue(cur);
        gainSpin_->setValue(cur);
    }

    // 自动曝光开启时禁用手动滑条
    const bool manual = !(hasAuto && autoOn);
    expSlider_->setEnabled(hasExp && manual);
    expSpin_->setEnabled(hasExp && manual);
    gainSlider_->setEnabled(hasGain && manual);
    gainSpin_->setEnabled(hasGain && manual);

    updatingParam_ = false;
}

std::vector<std::shared_ptr<ICameraDevice>> MainWindow::paramTargets() {
    // 参数统一应用到所有已连接相机。
    return manager_.connectedDevices();
}

void MainWindow::applyAutoExposure(bool enable) {
    const ExposureTarget                       t    = currentTarget();
    std::vector<std::shared_ptr<ICameraDevice>> devs = paramTargets();
    int                                        ok   = 0;
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i]->isTargetSupported(t) && devs[i]->setAutoExposure(t, enable)) {
            ++ok;
        }
    }
    log(QString::fromUtf8("设置自动曝光 %1（%2，成功 %3 台）。")
            .arg(enable ? QString::fromUtf8("开") : QString::fromUtf8("关"))
            .arg(QString::fromUtf8(exposureTargetName(t)))
            .arg(ok));
    loadParamValues();  // 刷新手动控件可用性与当前值
}

void MainWindow::applyExposure(int value) {
    const ExposureTarget                       t    = currentTarget();
    std::vector<std::shared_ptr<ICameraDevice>> devs = paramTargets();
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i]->isTargetSupported(t)) {
            devs[i]->setExposure(t, value);
        }
    }
}

void MainWindow::applyGain(int value) {
    const ExposureTarget                       t    = currentTarget();
    std::vector<std::shared_ptr<ICameraDevice>> devs = paramTargets();
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i]->isTargetSupported(t)) {
            devs[i]->setGain(t, value);
        }
    }
}

// ---------------------------- 辅助 ----------------------------

void MainWindow::log(const QString &msg, bool isError) {
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    if (isError) {
        // 错误行红色加粗，便于在日志流中一眼定位
        logEdit_->appendHtml(QString("<span style=\"color:%1;font-weight:bold;\">%2  %3</span>")
                                 .arg(QString::fromUtf8(theme::errorHex()), time, msg.toHtmlEscaped()));
    } else {
        logEdit_->appendPlainText(time + "  " + msg);
    }
}

std::shared_ptr<ICameraDevice> MainWindow::firstConnectedCamera() const {
    std::vector<std::shared_ptr<ICameraDevice>> devs = manager_.connectedDevices();
    if (devs.empty()) {
        return std::shared_ptr<ICameraDevice>();
    }
    return devs.front();
}

ExposureTarget MainWindow::currentTarget() const {
    const int idx = targetCombo_->currentIndex();
    if (idx < 0) {
        return ExposureTarget::Depth;
    }
    return static_cast<ExposureTarget>(targetCombo_->itemData(idx).toInt());
}

void MainWindow::updateButtonStates() {
    const bool   capturing = manager_.isCapturing();
    const size_t connected = manager_.connectedCount();

    // 采集中禁止连接/断开，避免与错峰启动线程争用设备
    btnConnectAll_->setEnabled(!capturing);
    btnDisconnectAll_->setEnabled(!capturing && connected > 0);

    btnStart_->setEnabled(!capturing && connected > 0);
    btnStop_->setEnabled(capturing);
    btnCalibrate_->setEnabled(connected > 0);   // 连接后随时可标定（采集中亦可）
    btnTrigger_->setEnabled(capturing);  // 采集中随时可触发保存
}

QString MainWindow::stateText(CamState s) {
    switch (s) {
    case CamState::Connected: return QString::fromUtf8("已连接");
    case CamState::Starting:  return QString::fromUtf8("启动中…");
    case CamState::Streaming: return QString::fromUtf8("采集中");
    case CamState::Stopped:   return QString::fromUtf8("已停止");
    case CamState::Error:     return QString::fromUtf8("错误");
    }
    return QString();
}
