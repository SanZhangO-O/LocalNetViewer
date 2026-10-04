#pragma once
// 图片解码/编码/缩放（对应 Python util/imaging.py；stb 实现）
#include <string>
#include "lnv/formats/adapter.h"

namespace lnv {

struct ImageTooLargeError : ParseError {
    ImageTooLargeError() : ParseError("图片超出像素上限") {}
};
struct ImageDecodeError : ParseError {
    ImageDecodeError() : ParseError("图片解码失败") {}
};

// RGBA 像素缓冲
struct ImageBuf {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;  // 4 通道
    bool has_alpha = false;           // 是否存在非 255 的 alpha（扫描得出/解码时已知）
};

// 魔数 sniff（失败退扩展名表）；未知返回 "application/octet-stream"
std::string sniff_mime(const std::string& bytes, const std::string& fallback_ext = "");
// 由文件名扩展名给 mime
std::string mime_from_ext(const std::string& ext);

// 读头探测尺寸；EXIF Orientation 5/6/7/8 宽高互换。失败返回 {0,0}
std::pair<int, int> probe_size(const std::string& bytes);

// 解码（RGBA）。像素数 > 2 × max_source_pixels → ImageTooLargeError
// （Pillow：MAX_IMAGE_PIXELS 只警告，超过 2 倍才抛 DecompressionBombError）；
// 解码失败 → ImageDecodeError。EXIF 方向已校正。
ImageBuf decode_image(const std::string& bytes, long long max_source_pixels);

// 缩放（只缩不放大；LANCZOS 近似）。max_w/max_h ≤0 表示不限制
ImageBuf resize_image(const ImageBuf& src, int max_w, int max_h);
ImageBuf resize_to(const ImageBuf& src, int w, int h);  // 强制尺寸

// 编码。PNG 无损；JPEG quality 1..100。
std::string encode_png(const ImageBuf& img);
std::string encode_jpeg(const ImageBuf& img, int quality);

// encode_image 等价（imaging.py:218-240）：
//   use_png = force_png or (has_alpha and not prefer_jpeg)
//   否则 prefer_jpeg=False → PNG；prefer_jpeg=True → JPEG（即使有 alpha 也走 JPEG）
struct EncodeOpts {
    bool prefer_jpeg = true;
    bool force_png = false;
    int jpeg_quality = 85;
};
ImageBytes encode_image(const ImageBuf& img, const EncodeOpts& opts);

}  // namespace lnv
