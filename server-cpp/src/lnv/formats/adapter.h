#pragma once
// 书籍适配器接口（对应 Python formats/base.py）
#include <functional>
#include <map>
#include "lnv/model.h"
#include "lnv/util/paths.h"

namespace lnv {

struct UnsupportedFormatError : std::runtime_error {
    std::string reason;
    UnsupportedFormatError(const std::string& msg, const std::string& r = "")
        : std::runtime_error(msg), reason(r.empty() ? msg : r) {}
};
struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };
struct AssetNotFoundError : std::runtime_error { using std::runtime_error::runtime_error; };
struct ParseTimeoutError : ParseError { using ParseError::ParseError; };
struct ServerBusyError : ParseError {
    using ParseError::ParseError;
};

struct BookRef {
    std::string book_id;
    std::string vpath;
    std::filesystem::path path;
    std::string format;
    long long size = 0;
    double mtime = 0.0;
    bool virtual_book = false;
    std::map<std::string, nlohmann::json> extra;

    std::string name() const {
        auto p = vpath.rfind('/');
        return p == std::string::npos ? vpath : vpath.substr(p + 1);
    }
    std::string title_guess() const {
        return virtual_book ? name() : stem_of(name());
    }
};

struct AdapterEnv;  // 前置（services 层定义）

struct AdapterMeta {
    std::string title;
    std::string format;
    std::string mode = "flow";
    std::optional<std::string> author;
    std::optional<std::string> language;
    long long size = 0;
    std::optional<int> page_count;
    std::vector<nlohmann::json> chapters;  // {"index","title","total_chars"?,"start_page"?}
    bool supports_json = true;
};

struct StripPage {
    int image_index = 0;
    long long y0 = 0;
    long long height = 0;
    bool is_first_of_image = false;
    bool is_last_of_image = false;
};

struct ImageBytes {
    std::string bytes;  // 以二进制容器
    std::string mime;
};

class BookAdapter {
public:
    std::string format;
    std::string mode = "flow";
    virtual ~BookAdapter() = default;

    virtual AdapterMeta meta() = 0;
    // 封面图字节；无封面返回 nullopt
    virtual std::optional<ImageBytes> cover(int width = 0) { return std::nullopt; }
    bool supports_json() const { return mode == "flow"; }

    // flow
    virtual DocumentModel document() {
        throw UnsupportedFormatError(format + " 不支持文本分页");
    }
    // fixed
    virtual int page_count() {
        throw UnsupportedFormatError(format + " 不是固定版式格式");
    }
    virtual ImageBytes page_bytes(int page, const std::string& fit = "viewport",
                                  int quality = 85, int viewport_w = 1080,
                                  double render_scale = 1.0) {
        throw UnsupportedFormatError(format + " 不支持取页");
    }
    virtual ImageBytes render_native(int page, const std::string& fit = "viewport",
                                     int quality = 85, int viewport_w = 1080,
                                     double render_scale = 1.0) {
        return page_bytes(page, fit, quality, viewport_w, render_scale);
    }
    virtual std::pair<int, int> page_native_size(int page) {
        throw UnsupportedFormatError(format + " 无法取得页面尺寸");
    }
    virtual double page_aspect(int page) {
        try {
            auto [w, h] = page_native_size(page);
            return w ? double(h) / w : 0.0;
        } catch (const UnsupportedFormatError&) {
            return 0.0;
        }
    }
    virtual ImageBytes pair_bytes(int page, int quality = 85, int viewport_w = 1080,
                                  double render_scale = 1.0, bool rtl = false) {
        throw UnsupportedFormatError(format + " 不支持跨页拼接");
    }
    // strip
    virtual std::vector<StripPage> strip_pages(int viewport_h, int overlap = 0) {
        throw UnsupportedFormatError(format + " 不支持条漫切片");
    }
    virtual ImageBytes strip_slice_bytes(const StripPage& sp, int quality = 85,
                                         int viewport_w = 1080, double render_scale = 1.0,
                                         const std::string& fit = "viewport") {
        throw UnsupportedFormatError(format + " 不支持条漫切片");
    }
    // 书内资源
    virtual ImageBytes asset(const std::string& path) {
        throw UnsupportedFormatError(format + " 不含内嵌资源");
    }
};

// book_id = sha1(ascii(size) + 头 1MiB)[:16]
std::string compute_book_id(const std::filesystem::path& path, long long size);

}  // namespace lnv
