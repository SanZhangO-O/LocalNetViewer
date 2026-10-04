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

}  // namespace lnv
