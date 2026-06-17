#include "FrameConverter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fc {

namespace {

// 经典 jet 颜色映射：t∈[0,1] -> RGB。这里调用方会把"近处"映射到较小的 t，
// 再翻转使近处为暖色（红），远处为冷色（蓝）。
inline void jet(float t, uint8_t &r, uint8_t &g, uint8_t &b) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float r0 = 1.5f - std::fabs(4.0f * t - 3.0f);
    const float g0 = 1.5f - std::fabs(4.0f * t - 2.0f);
    const float b0 = 1.5f - std::fabs(4.0f * t - 1.0f);
    r = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, r0)) * 255.0f);
    g = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, g0)) * 255.0f);
    b = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, b0)) * 255.0f);
}

}  // namespace

QImage depthToQImage(const std::shared_ptr<ob::DepthFrame> &frame, int minMm, int maxMm) {
    if (!frame) {
        return QImage();
    }
    const int width  = static_cast<int>(frame->getWidth());
    const int height = static_cast<int>(frame->getHeight());
    if (width <= 0 || height <= 0) {
        return QImage();
    }

    const uint16_t *src   = reinterpret_cast<const uint16_t *>(frame->getData());
    const float     scale = frame->getValueScale();  // 像素值 * scale = 毫米
    if (!src) {
        return QImage();
    }

    if (maxMm <= minMm) {
        maxMm = minMm + 1;
    }
    const float span = static_cast<float>(maxMm - minMm);

    QImage image(width, height, QImage::Format_RGB888);
    const int total = width * height;
    for (int y = 0; y < height; ++y) {
        uint8_t *dstLine = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int      idx = y * width + x;
            const uint16_t raw = (idx < total) ? src[idx] : 0;
            uint8_t        r = 0, g = 0, b = 0;
            if (raw != 0) {
                const float mm = static_cast<float>(raw) * scale;
                // 翻转：近处(mm 小) -> t 大 -> 暖色
                const float t = 1.0f - (mm - static_cast<float>(minMm)) / span;
                jet(t, r, g, b);
            }
            uint8_t *p = dstLine + x * 3;
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
    }
    return image;
}

QImage irToQImage(const std::shared_ptr<ob::VideoFrame> &frame) {
    if (!frame) {
        return QImage();
    }
    const int width  = static_cast<int>(frame->getWidth());
    const int height = static_cast<int>(frame->getHeight());
    if (width <= 0 || height <= 0) {
        return QImage();
    }

    const OBFormat format = frame->getFormat();
    const uint8_t *data   = frame->getData();
    if (!data) {
        return QImage();
    }

    QImage image(width, height, QImage::Format_Grayscale8);

    if (format == OB_FORMAT_Y8 || format == OB_FORMAT_BA81) {
        for (int y = 0; y < height; ++y) {
            std::memcpy(image.scanLine(y), data + static_cast<size_t>(y) * width, static_cast<size_t>(width));
        }
        return image;
    }

    // 其余按 16 位单通道处理（Y16/Z16/Y10/Y11/Y12/Y14，SDK 默认解包为 Y16）。
    const uint16_t *src = reinterpret_cast<const uint16_t *>(data);
    int             bits = static_cast<int>(frame->getPixelAvailableBitSize());
    if (bits <= 8 || bits > 16) {
        bits = 16;
    }
    const int shift = bits - 8;
    for (int y = 0; y < height; ++y) {
        uint8_t *dstLine = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            dstLine[x] = static_cast<uint8_t>(src[y * width + x] >> shift);
        }
    }
    return image;
}

QImage colorToQImage(const std::shared_ptr<ob::ColorFrame> &frame) {
    if (!frame) {
        return QImage();
    }
    const int      width  = static_cast<int>(frame->getWidth());
    const int      height = static_cast<int>(frame->getHeight());
    const OBFormat format = frame->getFormat();
    const uint8_t *data   = frame->getData();
    const uint32_t size   = frame->getDataSize();
    if (!data || size == 0) {
        return QImage();
    }

    switch (format) {
    case OB_FORMAT_MJPG:
    case OB_FORMAT_H264:   // 仅 MJPG 实际可由 QImage 直接解码，这里统一交给插件尝试
    case OB_FORMAT_H265: {
        QImage image;
        if (image.loadFromData(data, static_cast<int>(size), "JPG") && !image.isNull()) {
            return image.convertToFormat(QImage::Format_RGB888);
        }
        return QImage();
    }
    case OB_FORMAT_RGB: {
        if (width > 0 && height > 0) {
            QImage image(data, width, height, width * 3, QImage::Format_RGB888);
            return image.copy();
        }
        return QImage();
    }
    case OB_FORMAT_BGR: {
        if (width > 0 && height > 0) {
            QImage image(data, width, height, width * 3, QImage::Format_RGB888);
            return image.rgbSwapped();  // rgbSwapped 返回独立副本
        }
        return QImage();
    }
    case OB_FORMAT_RGBA: {
        if (width > 0 && height > 0) {
            QImage image(data, width, height, width * 4, QImage::Format_RGBA8888);
            return image.copy();
        }
        return QImage();
    }
    case OB_FORMAT_BGRA: {
        if (width > 0 && height > 0) {
            QImage image(data, width, height, width * 4, QImage::Format_RGBA8888);
            return image.rgbSwapped();
        }
        return QImage();
    }
    case OB_FORMAT_YUYV:
    case OB_FORMAT_YUY2: {
        if (width <= 0 || height <= 0) {
            return QImage();
        }
        QImage    image(width, height, QImage::Format_RGB888);
        const int macroPixels = width / 2;
        for (int y = 0; y < height; ++y) {
            const uint8_t *line = data + static_cast<size_t>(y) * width * 2;
            uint8_t       *dst  = image.scanLine(y);
            for (int i = 0; i < macroPixels; ++i) {
                const int y0 = line[i * 4 + 0];
                const int u  = line[i * 4 + 1] - 128;
                const int y1 = line[i * 4 + 2];
                const int v  = line[i * 4 + 3] - 128;
                // YUV -> RGB（整数近似）
                const int ruv = (91881 * v) >> 16;
                const int guv = ((22554 * u) + (46802 * v)) >> 16;
                const int buv = (116130 * u) >> 16;
                for (int k = 0; k < 2; ++k) {
                    const int yy = (k == 0) ? y0 : y1;
                    int       r  = yy + ruv;
                    int       g  = yy - guv;
                    int       b  = yy + buv;
                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);
                    uint8_t *p = dst + (i * 2 + k) * 3;
                    p[0] = static_cast<uint8_t>(r);
                    p[1] = static_cast<uint8_t>(g);
                    p[2] = static_cast<uint8_t>(b);
                }
            }
        }
        return image;
    }
    default:
        return QImage();
    }
}

}  // namespace fc
