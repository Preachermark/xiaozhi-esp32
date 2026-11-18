#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// TF 卡电子书：读 /sdcard/books 和 /sdcard/notes 里的 txt/md。
// 关键设计：不整本读进内存，按字节偏移分页，翻页只读一小块。
class SdBooks {
public:
    enum Kind { kBooks = 0, kNotes = 1 };

    struct Entry {
        std::string name;    // 卡上的原始文件名字节（可能 GBK），只用于 fopen
        std::string title;   // 显示名（已转 UTF-8，去扩展名与 ~1 短名尾巴）
        size_t size = 0;
        bool utf8_ok = true; // 内容编码检测结果
    };

    static SdBooks& GetInstance();

    void Scan(Kind kind);
    Kind CurrentKind() const { return kind_; }
    void SetKind(Kind k) { kind_ = k; }
    int Count() const { return (int)entries_.size(); }
    const Entry* At(int idx) const;
    const Entry* CurrentEntry() const;   // 当前打开的那本

    bool Open(int idx);                  // 打开第 idx 个文件，自动跳到上次读到的位置
    void Close();
    bool IsOpen() const { return file_ != nullptr; }

    // 读一页：从 offset 开始，返回本页文本和下一页起始偏移。is_end 表示读完了
    bool LoadPage(size_t offset, std::string& text, size_t& next_offset, bool& is_end);
    size_t CurrentOffset() const { return offset_; }
    size_t FileSize() const { return file_size_; }
    void SetOffset(size_t off) { offset_ = off; }
    const char* LastError() const { return error_; }
    int Percent() const;

    // 阅读进度（存 NVS，按文件名记）
    size_t RestoreProgress(const std::string& name);
    void SaveProgress(const std::string& name, size_t offset);

    static constexpr int kCharsPerLine = 16;   // 20px 字体，320 宽
    static constexpr int kLinesPerPage = 7;    // 正文区约 170px
    static constexpr int kCharsPerPage = kCharsPerLine * kLinesPerPage;

private:
    SdBooks() = default;
    void ScanDir(const char* dir, const char* manifest);
    bool DetectEncoding();

    std::vector<Entry> entries_;
    Kind kind_ = kBooks;
    void* file_ = nullptr;      // FILE*
    size_t file_size_ = 0;
    size_t offset_ = 0;
    int cur_index_ = -1;
    bool utf8_mode_ = true;   // 当前文件内容是否 UTF-8（否则按 GBK 转）
    char error_[64] = {0};
};