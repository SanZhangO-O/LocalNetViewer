#pragma once
// FB2 适配器（对应 Python formats/fb2.py）。
// - pugixml 全量读入（禁 DTD 防 XXE），受 max_book_mb 限制；
// - description/title-info 提供元数据；每个 body 下直属 section 分章；
// - binary 内嵌 base64 图片；coverpage/image href（去 #）→ binary。
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

class Fb2Adapter : public BookAdapter {
public:
    Fb2Adapter(const AppConfig& cfg, const BookRef& ref);
    ~Fb2Adapter() override;

    AdapterMeta meta() override;
    DocumentModel document() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    ImageBytes asset(const std::string& path) override;

    // binary id → (原始字节, content-type)；找不到返回 nullopt（供正文 image 块使用）
    std::optional<std::pair<std::string, std::string>> find_binary(const std::string& href);

private:
    void load();  // 懒加载：读文件 + XML 解析 + binary base64 解码

    const AppConfig& cfg_;
    BookRef ref_;
    bool loaded_ = false;
    // binary id → (原始字节, content-type)
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> binaries_;
    std::map<std::string, size_t> binary_index_;  // id → binaries_ 下标
    // XML 解析结果（pugixml 文档指针，析构时 delete；避免在头里引 pugixml）
    void* doc_ = nullptr;
    std::string xml_bytes_;  // load_buffer 不拷贝，文档字节须与 document 同生命周期
};

}  // namespace lnv
