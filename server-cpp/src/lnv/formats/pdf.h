#pragma once
// PDF 适配器（对应 Python formats/pdf.py；后端 pdfium 动态加载）
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

// pdfium.dll 是否可用（/files 的 openable 探测用；进程内缓存）
bool pdfium_available();

class PdfAdapter : public BookAdapter {
public:
    // pdfium.dll 加载失败 → UnsupportedFormatError("缺少 PDF 后端","需放置 pdfium.dll")
    PdfAdapter(const AppConfig& cfg, const BookRef& ref);
    // 释放 FPDF_DOCUMENT：适配器按请求创建，缺了析构每次请求都会泄漏整份文档
    ~PdfAdapter() override;

    AdapterMeta meta() override;
    int page_count() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    std::pair<int, int> page_native_size(int page) override;
    double page_aspect(int page) override;
    ImageBytes page_bytes(int page, const std::string& fit = "viewport", int quality = 85,
                          int viewport_w = 1080, double render_scale = 1.0) override;
    ImageBytes pair_bytes(int page, int quality = 85, int viewport_w = 1080,
                          double render_scale = 1.0, bool rtl = false) override;

private:
    struct PtSize { double w = 1.0, h = 1.0; };
    PtSize page_pt(int page);                 // 页面 pt 尺寸（越界抛错）
    double clamp_scale(double w_pt, double h_pt, double scale);
    // 渲染已加载页面（调用方负责 LoadPage/关闭），RAII 管理位图
    ImageBytes render_loaded_page(void* pg, double w_pt, double h_pt, double scale, int quality);
    std::string meta_text(const char* tag);   // FPDF_GetMetaText（UTF-16 → UTF-8）

    AppConfig cfg_;
    BookRef ref_;
    void* doc_ = nullptr;   // FPDF_DOCUMENT
    int page_count_ = 0;
    long long max_pixels_ = 0;
    std::map<int, std::pair<int, int>> size_cache_;
};

}  // namespace lnv
