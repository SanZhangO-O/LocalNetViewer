#include "lnv/formats/adapter.h"
#include <cstdio>
#include "lnv/util/sha1.h"

namespace lnv {

// 注意：`format_from_path` 已删除 —— 它的三元表达式是恒等的空操作，且
// path.string() 在 Windows 上走 ANSI 代码页（不可表示的字符会抛异常），
// 全工程也没有调用者（审查 NIT）。需要扩展名时直接用 ext_of()。

std::string compute_book_id(const std::filesystem::path& path, long long size) {
    Sha1 sha;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", size);
    sha.update(buf, std::strlen(buf));
    std::ifstream in(path, std::ios::binary);
    if (in) {
        // 1MiB 缓冲必须放堆上（默认栈仅 1MB，栈上分配会直接爆栈）
        std::vector<char> head(1 << 20);
        in.read(head.data(), head.size());
        std::streamsize got = in.gcount();
        if (got > 0) sha.update(head.data(), size_t(got));
    }
    return sha.hexdigest().substr(0, 16);
}

// ---------------------------------------------------------------- 内容修订号
// 逐字节对齐 Python：sha1_hex_ascii(payload)[:12]；mtime_int 用 (long long) 截断
// （向零，与 Python int(float) 一致；"取整到秒"在正 epoch 下即 floor）。
namespace {
std::string sha1_hex_ascii(const std::string& payload) {
    Sha1 sha;
    sha.update(payload.data(), payload.size());
    return sha.hexdigest();
}
}  // namespace

std::string compute_file_rev(const std::string& book_id, long long size, double mtime) {
    std::string payload = book_id + ":" + std::to_string(size) + ":" +
                          std::to_string(static_cast<long long>(mtime));
    return sha1_hex_ascii(payload).substr(0, 12);
}

std::string compute_gallery_rev(const std::string& book_id, int image_count,
                                const std::optional<double>& dir_mtime) {
    if (!dir_mtime) return book_id;  // 目录 stat 失败 → 用 book_id 兜底
    std::string payload = book_id + ":" + std::to_string(image_count) + ":" +
                          std::to_string(static_cast<long long>(*dir_mtime));
    return sha1_hex_ascii(payload).substr(0, 12);
}

}  // namespace lnv
