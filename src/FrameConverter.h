#ifndef FRAME_CONVERTER_H
#define FRAME_CONVERTER_H

#include <QImage>
#include <memory>

#include <libobsensor/ObSensor.hpp>

// 将 Orbbec SDK 的帧数据转换为 QImage 以便在界面上预览。
// 所有函数都会对像素数据做深拷贝，返回的 QImage 独立拥有其缓冲，
// 因此可以安全地跨线程传递（回调线程生成、GUI 线程显示）。
namespace fc {

// 深度帧 -> 伪彩 QImage(RGB888)。近处偏暖色、远处偏冷色，无效像素(0)为黑色。
// minMm / maxMm 为参与上色的深度范围（毫米）。
QImage depthToQImage(const std::shared_ptr<ob::DepthFrame> &frame, int minMm = 200, int maxMm = 4000);

// 红外帧 -> 灰度 QImage(Grayscale8)。支持 Y8 / Y16（按有效位自动缩放到 8 位）。
QImage irToQImage(const std::shared_ptr<ob::VideoFrame> &frame);

// 彩色帧 -> QImage(RGB888)。支持 MJPG / RGB / BGR / YUYV(YUY2) 等常见格式。
QImage colorToQImage(const std::shared_ptr<ob::ColorFrame> &frame);

}  // namespace fc

#endif  // FRAME_CONVERTER_H
