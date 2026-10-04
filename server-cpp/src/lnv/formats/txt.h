#pragma once
// TXT 适配器（对应 Python formats/txt.py）：编码检测 + 章节切分。
// 行为照抄 txt.py：
// - 编码：decode_text（textutil.h），书级 override = cfg.encoding_override(vpath)；
// - 分章：match_chapter_title 匹配标题行；无任何标题且（UTF-8 字节 > 512KB）
//   且（总字符 > 64KB）时按"累计 ≥64*1024 字符"切"第 N 节"；
// - 每个非空行 = 一个 paragraph 块（空行自然结束段落，与母本一致）。
#include "lnv/config.h"
#include "lnv/formats/adapter.h"
#include "lnv/util/textutil.h"

namespace lnv {

class TxtAdapter : public BookAdapter {
public:
    TxtAdapter(const AppConfig& cfg, const BookRef& ref);

    AdapterMeta meta() override;
    DocumentModel document() override;
    // TXT 无内嵌封面，返回 nullopt（服务层退回文字封面）

private:
    // 懒加载：读文件（校验大小上限）→ decode_text
    const DecodedText& load_text();

    const AppConfig& cfg_;
    BookRef ref_;
    std::optional<DecodedText> text_;  // 缓存解码结果
};

}  // namespace lnv
