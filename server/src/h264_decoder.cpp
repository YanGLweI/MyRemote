#include "h264_decoder.hpp"

#include "wels/codec_api.h"

#include "log.hpp"

H264Decoder::~H264Decoder() {
    shutdown();
}

bool H264Decoder::initialize(int width, int height) {
    shutdown();
    width_ = width;
    height_ = height;

    ISVCDecoder* decoder = nullptr;
    if (WelsCreateDecoder(&decoder) != 0 || !decoder) {
        mlog::error("WelsCreateDecoder failed");
        return false;
    }

    SDecodingParam params{};
    memset(&params, 0, sizeof(params));
    if (decoder->Initialize(&params) != 0) {
        mlog::error("OpenH264 decoder Initialize failed");
        WelsDestroyDecoder(decoder);
        return false;
    }

    decoder_ = decoder;
    initialized_ = true;
    mlog::info("OpenH264 decoder initialized");
    return true;
}

void H264Decoder::shutdown() {
    if (decoder_) {
        WelsDestroyDecoder(static_cast<ISVCDecoder*>(decoder_));
        decoder_ = nullptr;
    }
    initialized_ = false;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, QImage& out) {
    if (!initialized_ || !decoder_ || !data || size == 0) {
        return false;
    }
    unsigned char* dst = nullptr;
    SBufferInfo info{};
    info.iBufferStatus = 0;
    static_cast<ISVCDecoder*>(decoder_)
        ->DecodeFrameNoDelay(data, static_cast<int>(size), &dst, &info);
    if (info.iBufferStatus != 1 || !info.pDst[0] || !info.pDst[1] || !info.pDst[2]) {
        return false;
    }

    const SSysMEMBuffer& b = info.UsrData.sSystemBuffer;
    int w = b.iWidth;
    int h = b.iHeight;
    int sy = b.iStride[0];
    int sc = b.iStride[1] > 0 ? b.iStride[1] : (w / 2);
    if (w <= 0 || h <= 0 || sy < w || sc < w / 2) {
        return false;
    }

    const uint8_t* plane_y = info.pDst[0];
    const uint8_t* plane_u = info.pDst[1];
    const uint8_t* plane_v = info.pDst[2];

    // Clamp readable rows to the actual space between plane pointers so we
    // never read past a plane even if the decoder under-allocates chroma.
    long long y_to_u = static_cast<const uint8_t*>(plane_u) - plane_y;
    long long u_to_v = static_cast<const uint8_t*>(plane_v) - plane_u;
    if (y_to_u > 0) {
        int max_luma_rows = static_cast<int>(y_to_u / sy);
        if (h > max_luma_rows) h = max_luma_rows;
    }
    if (u_to_v > 0) {
        int max_chroma_rows = static_cast<int>(u_to_v / sc);
        if (h / 2 > max_chroma_rows) h = max_chroma_rows * 2;
    }
    if (w <= 0 || h <= 0) {
        return false;
    }

    if (sc < w / 2) sc = w / 2;
    out = QImage(w, h, QImage::Format_RGB32);
    for (int row = 0; row < h; ++row) {
        const uint8_t* y_row = plane_y + static_cast<size_t>(row) * sy;
        const uint8_t* u_row = plane_u + static_cast<size_t>(row / 2) * sc;
        const uint8_t* v_row = plane_v + static_cast<size_t>(row / 2) * sc;
        uint32_t* dpx = reinterpret_cast<uint32_t*>(out.scanLine(row));
        for (int col = 0; col < w; ++col) {
            int yy = y_row[col] - 16;
            int uu = u_row[col >> 1] - 128;
            int vv = v_row[col >> 1] - 128;
            int r = (298 * yy + 409 * vv + 128) >> 8;
            int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
            int bl = (298 * yy + 516 * uu + 128) >> 8;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            bl = bl < 0 ? 0 : (bl > 255 ? 255 : bl);
            dpx[col] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                       (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(bl);
        }
    }
    return true;
}
