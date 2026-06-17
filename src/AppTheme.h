#ifndef APP_THEME_H
#define APP_THEME_H

#include <QColor>

#include "CameraTypes.h"

// 应用主题：工业灰 + 安全橙。
// 配色基准：背景 #F8FAFC，卡片白底，文字 #334155（对比度 > 4.5:1），
// 主操作/强调 #F97316，状态色见 stateColor()。
// 仅使用 Qt5/Qt6 共有的 QSS 属性。
namespace theme {

// 状态 → 颜色（用于设备表状态列等处的文字着色；
// 颜色只做辅助，状态始终有文字，不单靠颜色传达信息）。
inline QColor stateColor(CamState s) {
    switch (s) {
    case CamState::Connected: return QColor(0x64, 0x74, 0x8B);  // 灰蓝：待命
    case CamState::Starting:  return QColor(0xF9, 0x73, 0x16);  // 橙：进行中
    case CamState::Streaming: return QColor(0x16, 0xA3, 0x4A);  // 绿：采集中
    case CamState::Stopped:   return QColor(0x94, 0xA3, 0xB8);  // 浅灰：已停止
    case CamState::Error:     return QColor(0xDC, 0x26, 0x26);  // 红：错误
    }
    return QColor(0x33, 0x41, 0x55);
}

// 表中未连接设备的状态文字颜色。
inline QColor disconnectedColor() { return QColor(0x94, 0xA3, 0xB8); }

// 日志错误行颜色（十六进制串，供富文本使用）。
inline const char *errorHex() { return "#DC2626"; }

// 全局样式表。主操作按钮通过动态属性 primary=true 启用橙色强调。
inline const char *styleSheet() {
    return R"(
QMainWindow { background: #F8FAFC; }
QWidget { color: #334155; font-size: 13px; }

QGroupBox {
    background: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: #64748B;
    font-weight: bold;
}

QPushButton {
    background: #FFFFFF;
    border: 1px solid #CBD5E1;
    border-radius: 4px;
    padding: 5px 12px;
}
QPushButton:hover { background: #F1F5F9; }
QPushButton:pressed { background: #E2E8F0; }
QPushButton:disabled { color: #94A3B8; background: #F8FAFC; border-color: #E2E8F0; }

QPushButton[primary="true"] {
    background: #F97316;
    color: #FFFFFF;
    border: 1px solid #EA580C;
    font-weight: bold;
}
QPushButton[primary="true"]:hover { background: #EA580C; }
QPushButton[primary="true"]:pressed { background: #C2410C; }
QPushButton[primary="true"]:disabled { background: #FDBA74; color: #FFF7ED; border-color: #FDBA74; }

QTableWidget {
    background: #FFFFFF;
    border: 1px solid #E2E8F0;
    gridline-color: #F1F5F9;
    selection-background-color: #FFEDD5;
    selection-color: #334155;
}
QHeaderView::section {
    background: #F1F5F9;
    color: #64748B;
    border: none;
    border-bottom: 1px solid #E2E8F0;
    padding: 4px 6px;
    font-weight: bold;
}

QPlainTextEdit, QComboBox, QSpinBox {
    background: #FFFFFF;
    border: 1px solid #CBD5E1;
    border-radius: 4px;
    padding: 2px 4px;
}
QComboBox:disabled, QSpinBox:disabled { background: #F8FAFC; color: #94A3B8; border-color: #E2E8F0; }
QCheckBox:disabled { color: #94A3B8; }

QSlider::groove:horizontal { height: 4px; background: #E2E8F0; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #94A3B8; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 14px;
    margin: -5px 0;
    border-radius: 7px;
    background: #64748B;
    border: 1px solid #475569;
}
QSlider::handle:horizontal:hover { background: #F97316; border-color: #EA580C; }
QSlider::handle:horizontal:disabled { background: #CBD5E1; border-color: #CBD5E1; }

QScrollArea { border: 1px solid #E2E8F0; border-radius: 6px; }
)";
}

}  // namespace theme

#endif  // APP_THEME_H
