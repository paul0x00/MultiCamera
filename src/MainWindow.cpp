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
    imageLabel_->setFixedSize(320, 180);  // 固定大小（16:9），6 块按 2×3 排布
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), triggerBusy_(false) {
    setWindowTitle(QString::fromUtf8("Gemini 215 多相机控制台"));
    resize(1340, 800);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *outer = new QVBoxLayout(central);

    QHBoxLayout *row = new QHBoxLayout();
    outer->addLayout(row, 1);

    // 左：固定 2×3 预览（六槽）。按厂商映射：槽0/1/2=Orbbec，槽3=海康，槽4/5=迈德威视左/右。
    QWidget *previewContainer = new QWidget(this);
    previewGrid_              = new QGridLayout(previewContainer);
    previewGrid_->setContentsMargins(6, 6, 6, 6);
    previewGrid_->setSpacing(6);
    const int cols = 3;
    for (int i = 0; i < 6; ++i) {
        slots_[i]      = new CameraView(i + 1, previewContainer);
        slotStream_[i] = PreviewStream::Color;
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

    // 后台触发线程完成后经此信号回到 GUI 线程写盘（跨线程须 QueuedConnection + 注册 metatype）。
    qRegisterMetaType<TriggerBatch>("TriggerBatch");
    connect(this, &MainWindow::triggerBatchReady, this,
            [this](TriggerBatch batch) { saveBatch(batch); }, Qt::QueuedConnection);

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
    // 等后台触发线程结束，避免析构后线程仍访问 manager_。
    if (triggerThread_.joinable()) {
        triggerThread_.join();
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
    if (triggerBusy_.load()) {
        log(QString::fromUtf8("上一次触发保存仍在进行，请稍候。"), true);
        return;
    }
    if (manager_.connectedCount() == 0) {
        log(QString::fromUtf8("没有已连接的相机，无法保存。"));
        return;
    }

    const int seq = ++triggerSeq_;
    log(QString::fromUtf8("触发 #%1：后台发起触发保存序列（Orbbec 将分时取彩色+左右IR）…").arg(seq));

    // 上一个线程若已结束需先 join，避免 std::thread 赋值时仍 joinable 触发 terminate。
    if (triggerThread_.joinable()) {
        triggerThread_.join();
    }
    triggerBusy_.store(true);
    updateButtonStates();

    // 后台线程跑阻塞的 captureTriggerSets（含 Orbbec 停流/切 IR channel/恢复，耗时几百 ms），
    // 完成后经 triggerBatchReady 信号把成果送回 GUI 线程写盘。
    triggerThread_ = std::thread([this, seq]() {
        TriggerBatch batch;
        batch.seq     = seq;
        batch.results = manager_.captureTriggerSets(800, batch.dispatchUs);
        emit triggerBatchReady(batch);
    });
}

void MainWindow::saveBatch(const TriggerBatch &batch) {
    // 信号在 GUI 线程触发：先收尾后台线程句柄，解除忙标志。
    if (triggerThread_.joinable()) {
        triggerThread_.join();
    }
    triggerBusy_.store(false);

    // 输出目录（可执行文件所在目录下），按厂商/路分目录。
    const QString base = QCoreApplication::applicationDirPath();
    const QString obColorDir = base + "/orbbec_color";
    const QString obLeftDir  = base + "/orbbec_leftir";
    const QString obRightDir = base + "/orbbec_rightir";
    const QString hikDir     = base + "/hik_color";
    const QString mvLeftDir  = base + "/mv_left";
    const QString mvRightDir = base + "/mv_right";
    const QString tsDir      = base + "/timestamps";
    QDir().mkpath(obColorDir);
    QDir().mkpath(obLeftDir);
    QDir().mkpath(obRightDir);
    QDir().mkpath(hikDir);
    QDir().mkpath(mvLeftDir);
    QDir().mkpath(mvRightDir);
    QDir().mkpath(tsDir);

    const QString seqStr = QString("%1").arg(batch.seq, 4, 10, QChar('0'));

    // 以最早曝光时刻为基准，计算各机相对差值。
    uint64_t earliest     = 0;
    bool     haveEarliest = false;
    for (size_t i = 0; i < batch.results.size(); ++i) {
        const TriggerShot &s = batch.results[i].shot;
        if (s.ok && s.timing.valid) {
            if (!haveEarliest || s.timing.hostEpochUs < earliest) {
                earliest     = s.timing.hostEpochUs;
                haveEarliest = true;
            }
        }
    }

    // sidecar：记录本次触发各机的曝光时刻（主机 epoch µs）与相对最早机的差，供后处理核对同步性。
    QString csv;
    csv += QString::fromUtf8("# trigger_seq=%1, dispatch_host_us=%2\n")
               .arg(seqStr)
               .arg(static_cast<qulonglong>(batch.dispatchUs));
    csv += QString::fromUtf8("vendor,serial,name,host_epoch_us,capture_raw_us,exposure_us,delta_to_earliest_us,status\n");

    int      savedCount = 0;
    uint64_t maxDelta   = 0;

    for (size_t i = 0; i < batch.results.size(); ++i) {
        const std::shared_ptr<ICameraDevice> &dev    = batch.results[i].dev;
        const TriggerShot                    &s      = batch.results[i].shot;
        const QString                         vendor = QString::fromStdString(dev->vendor());
        const QString                         model  = sanitizeModel(dev->name());
        const QString                         serial = QString::fromStdString(dev->serial());

        if (!s.ok) {
            log(QString::fromUtf8("相机 %1（%2）触发未取到帧，未保存。").arg(serial).arg(vendor), true);
            csv += QString("%1,%2,%3,,,,,timeout\n").arg(vendor).arg(serial).arg(model);
            continue;
        }

        const uint64_t hostUs = s.timing.valid ? s.timing.hostEpochUs : 0;
        const QString  tsStr  = QString::number(static_cast<qulonglong>(hostUs));
        const QString  file   = model + "_" + serial + "_" + seqStr + "_" + tsStr + ".png";

        bool any = false;
        if (!s.color.isNull()) {
            // Orbbec 彩色 → orbbec_color；海康彩色 → hik_color。
            const QString dir = (vendor == "Hikvision") ? hikDir : obColorDir;
            if (s.color.save(dir + "/" + file, "PNG")) any = true;
        }
        if (!s.left.isNull()) {
            // Orbbec 左 IR → orbbec_leftir；迈德威视左目 → mv_left。
            const QString dir = (vendor == "MindVision") ? mvLeftDir : obLeftDir;
            if (s.left.save(dir + "/" + file, "PNG")) any = true;
        }
        if (!s.right.isNull()) {
            const QString dir = (vendor == "MindVision") ? mvRightDir : obRightDir;
            if (s.right.save(dir + "/" + file, "PNG")) any = true;
        }
        if (any) {
            ++savedCount;
        }

        const uint64_t delta = (haveEarliest && s.timing.valid) ? (hostUs - earliest) : 0;
        if (s.timing.valid && delta > maxDelta) {
            maxDelta = delta;
        }

        csv += QString("%1,%2,%3,%4,%5,%6,%7,%8\n")
                   .arg(vendor)
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
            .arg(static_cast<int>(batch.results.size()))
            .arg(maxDelta / 1000.0, 0, 'f', 1));

    updateButtonStates();
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

    // 按厂商重建槽位映射（设备数量/连接状态可能变化）。
    assignSlots();

    // 六槽按厂商固定：槽0/1/2=Orbbec 彩色，槽3=海康彩色，槽4=迈德威视左目，槽5=迈德威视右目。
    for (int i = 0; i < 6; ++i) {
        CameraView                    *view = slots_[i];
        std::shared_ptr<ICameraDevice> dev  = slotDev_[i];
        if (!dev) {
            view->clear();
            continue;
        }
        const std::string  &sn     = dev->serial();
        const PreviewStream  stream = slotStream_[i];

        // 采集后首次拿到帧时，打印一次流诊断（每台一次）。
        if (dev->frameCount() > 0 && diagLogged_.find(sn) == diagLogged_.end()) {
            diagLogged_.insert(sn);
            log(QString::fromUtf8("相机 %1（%2）流诊断：%3")
                    .arg(i + 1)
                    .arg(QString::fromStdString(sn))
                    .arg(QString::fromStdString(dev->streamDiagnostics())));
        }

        QImage img = dev->latestImage(stream);
        if (!img.isNull()) {
            view->showImage(img);
        }
        QString info = QString::fromUtf8("相机 %1  ").arg(i + 1) + QString::fromStdString(sn) +
                       "  " + QString::fromUtf8(previewStreamName(stream)) + "  " + stateText(dev->state());
        if (dev->isStreaming()) {
            info += QString::fromUtf8("  %1fps").arg(fps_.count(sn) ? fps_[sn] : 0.0, 0, 'f', 1);
            const std::string res = dev->resolution(stream);
            if (!res.empty()) {
                info += "  " + QString::fromStdString(res);
            }
        }
        view->setInfo(info);
    }

    updateButtonStates();
}

// 按厂商把已连接设备路由到固定槽：Orbbec→0/1/2（彩色），海康→3（彩色），
// 迈德威视→4（左目）/5（右目，同一台设备占两槽）。同厂商多台按连接顺序占位，超出则丢弃显示。
void MainWindow::assignSlots() {
    for (int i = 0; i < 6; ++i) {
        slotDev_[i].reset();
        slotStream_[i] = PreviewStream::Color;
    }
    std::vector<std::shared_ptr<ICameraDevice>> devs = manager_.connectedDevices();
    int obNext = 0;  // 下一个空闲的 Orbbec 槽（0..2）
    for (size_t i = 0; i < devs.size(); ++i) {
        const std::string &vendor = devs[i]->vendor();
        if (vendor == "Orbbec") {
            if (obNext < 3) {
                slotDev_[obNext]    = devs[i];
                slotStream_[obNext] = PreviewStream::Color;
                ++obNext;
            }
        } else if (vendor == "Hikvision") {
            slotDev_[3]    = devs[i];
            slotStream_[3] = PreviewStream::Color;
        } else if (vendor == "MindVision") {
            slotDev_[4]    = devs[i];
            slotStream_[4] = PreviewStream::IRLeft;
            slotDev_[5]    = devs[i];
            slotStream_[5] = PreviewStream::IRRight;
        }
    }
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
    const bool   triggering = triggerBusy_.load();

    // 采集中禁止连接/断开，避免与启动线程争用设备
    btnConnectAll_->setEnabled(!capturing);
    btnDisconnectAll_->setEnabled(!capturing && connected > 0);

    btnStart_->setEnabled(!capturing && connected > 0);
    // 触发保存进行中禁止停止采集：分时序列正在停/起流，避免争用设备生命周期。
    btnStop_->setEnabled(capturing && !triggering);
    btnCalibrate_->setEnabled(connected > 0 && !triggering);
    // 采集中且无进行中的触发保存时可触发。
    btnTrigger_->setEnabled(capturing && !triggering);
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
