#include "sd_books.h"
#include "sd_card_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <esp_log.h>

#include "gbk_table.h"   // GBK -> Unicode 映射表

static const char* TAG = "SdBooks";

namespace {

constexpr const char* kBooksDir = "/sdcard/books";
constexpr const char* kNotesDir = "/sdcard/notes";
constexpr const char* kProgressFile = "/sdcard/books/.progress";
constexpr size_t kReadMax = 4096;

bool s_inited = false;

size_t Utf8CharLen(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;   // 非法首字节，当 1 字节跳过
}

bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) break;         // 末尾截断不算错
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

// GBK 双字节转 Unicode
uint16_t GbkCodeToUnicode(uint16_t gbk) {
    uint8_t hi = (uint8_t)(gbk >> 8);
    uint8_t lo = (uint8_t)(gbk & 0xFF);
    if (hi < 0x81 || hi > 0xFE) return 0xFFFD;
    int row = hi - 0x81;
    int col;
    if (lo >= 0x40 && lo < 0x7F) {
        col = lo - 0x40;
    } else if (lo >= 0x80 && lo <= 0xFE) {
        col = lo - 0x41;
    } else {
        return 0xFFFD;
    }
    if (col < 0 || col >= 190) return 0xFFFD;
    uint16_t u = kGbkToUnicode[row][col];
    return u == 0 ? 0xFFFD : u;
}

void AppendUtf8(std::string& out, uint16_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// GBK 字节串 -> UTF-8（文件名或整段文本都能用）
std::string GbkToUtf8(const char* s, size_t n) {
    std::string out;
    out.reserve(n + n / 2);
    size_t i = 0;
    while (i < n) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            out.push_back((char)b);
            i++;
        } else if (i + 1 < n) {
            uint16_t gbk = (uint16_t)((b << 8) | (uint8_t)s[i + 1]);
            AppendUtf8(out, GbkCodeToUnicode(gbk));
            i += 2;
        } else {
            AppendUtf8(out, 0xFFFD);
            i++;
        }
    }
    return out;
}

// 去掉 Windows 8.3 短名的 "~1" 尾巴
std::string StripShortNameTail(const std::string& s) {
    size_t tilde = s.find('~');
    if (tilde == std::string::npos || tilde == 0 || tilde + 1 >= s.size()) return s;
    if (s[tilde + 1] < '0' || s[tilde + 1] > '9') return s;
    return s.substr(0, tilde);
}

bool HasExt(const char* name, const char* ext) {
    size_t len = strlen(name);
    size_t elen = strlen(ext);
    return len > elen && strcasecmp(name + len - elen, ext) == 0;
}

std::string Stem(const char* name) {
    std::string s(name);
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

}  // namespace

SdBooks& SdBooks::GetInstance() {
    static SdBooks instance;
    return instance;
}

const SdBooks::Entry* SdBooks::At(int idx) const {
    if (idx < 0 || idx >= (int)entries_.size()) return nullptr;
    return &entries_[idx];
}

const SdBooks::Entry* SdBooks::CurrentEntry() const {
    return At(cur_index_);
}

void SdBooks::ScanDir(const char* dir, const char* manifest) {
    // 先读清单（一行「文件名|显示名」），中文书名放这里，卡上文件名保持 ASCII
    std::vector<std::pair<std::string, std::string>> titles;
    if (SdCardManager::GetInstance().LockFs()) {
        char mpath[96];
        snprintf(mpath, sizeof(mpath), "%s/%s", dir, manifest);
        FILE* mf = fopen(mpath, "r");
        if (mf != nullptr) {
            static char line[256];
            while (fgets(line, sizeof(line), mf) != nullptr) {
                char* nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                char* bar = strchr(line, '|');
                if (bar == nullptr) continue;
                *bar = '\0';
                titles.emplace_back(std::string(line), std::string(bar + 1));
            }
            fclose(mf);
        }
        SdCardManager::GetInstance().UnlockFs();
    }

    std::vector<std::string> names;
    if (!SdCardManager::GetInstance().LockFs()) return;
    DIR* d = opendir(dir);
    if (d != nullptr) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            if (HasExt(e->d_name, ".txt") || HasExt(e->d_name, ".md")) {
                names.emplace_back(e->d_name);
            }
        }
        closedir(d);
    }
    SdCardManager::GetInstance().UnlockFs();

    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        Entry ent;
        ent.name = n;                                        // 原始字节，fopen 用
        ent.title = StripShortNameTail(GbkToUtf8(n.c_str(), n.size()));
        ent.title = Stem(ent.title.c_str());                 // 去扩展名
        for (const auto& t : titles) {
            if (t.first == n) {
                ent.title = t.second;                        // 清单里写了就以清单为准
                break;
            }
        }
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, n.c_str());
        struct stat st = {};
        if (SdCardManager::GetInstance().LockFs()) {
            if (stat(path, &st) == 0) ent.size = (size_t)st.st_size;
            SdCardManager::GetInstance().UnlockFs();
        }
        entries_.push_back(std::move(ent));
    }
    ESP_LOGI(TAG, "%s: %u 个文件", dir, (unsigned)names.size());
}

void SdBooks::Scan(Kind kind) {
    kind_ = kind;
    entries_.clear();
    ScanDir(kind == kBooks ? kBooksDir : kNotesDir, kind == kBooks ? "books.txt" : "notes.txt");
}

bool SdBooks::DetectEncoding() {
    if (file_ == nullptr) return false;
    static uint8_t head[512];
    size_t n = 0;
    if (SdCardManager::GetInstance().LockFs()) {
        FILE* f = (FILE*)file_;
        fseek(f, 0, SEEK_SET);
        n = fread(head, 1, sizeof(head), f);
        SdCardManager::GetInstance().UnlockFs();
    }
    if (n == 0) return true;
    return IsValidUtf8(head, n);
}

bool SdBooks::Open(int idx) {
    Close();
    const Entry* ent = At(idx);
    if (ent == nullptr) {
        snprintf(error_, sizeof(error_), "没有这本书");
        return false;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", kind_ == kBooks ? kBooksDir : kNotesDir,
             ent->name.c_str());
    if (SdCardManager::GetInstance().LockFs()) {
        file_ = fopen(path, "rb");
        if (file_ != nullptr) {
            fseek((FILE*)file_, 0, SEEK_END);
            file_size_ = (size_t)ftell((FILE*)file_);
            fseek((FILE*)file_, 0, SEEK_SET);
        }
        SdCardManager::GetInstance().UnlockFs();
    }
    if (file_ == nullptr) {
        snprintf(error_, sizeof(error_), "打不开文件");
        return false;
    }
    cur_index_ = idx;
    utf8_mode_ = DetectEncoding();
    if (!utf8_mode_) {
        ESP_LOGI(TAG, "%s 是 GBK 编码，按 GBK 转换显示", ent->name.c_str());
    }
    offset_ = RestoreProgress(ent->name);
    if (offset_ >= file_size_) offset_ = 0;
    ESP_LOGI(TAG, "打开 %s (%u 字节), 从偏移 %u 继续", ent->name.c_str(),
             (unsigned)file_size_, (unsigned)offset_);
    return true;
}

void SdBooks::Close() {
    cur_index_ = -1;
    if (file_ != nullptr) {
        if (SdCardManager::GetInstance().LockFs()) {
            fclose((FILE*)file_);
            SdCardManager::GetInstance().UnlockFs();
        }
        file_ = nullptr;
    }
    file_size_ = 0;
    offset_ = 0;
}

int SdBooks::Percent() const {
    if (file_size_ == 0) return 0;
    return (int)(offset_ * 100 / file_size_);
}

bool SdBooks::LoadPage(size_t offset, std::string& text, size_t& next_offset, bool& is_end) {
    text.clear();
    is_end = false;
    next_offset = offset;
    if (file_ == nullptr) return false;
    if (offset >= file_size_) {
        is_end = true;
        return true;
    }

    static char buf[kReadMax + 1];
    size_t want = kReadMax;
    if (offset + want > file_size_) want = file_size_ - offset;

    size_t n = 0;
    if (SdCardManager::GetInstance().LockFs()) {
        FILE* f = (FILE*)file_;
        fseek(f, (long)offset, SEEK_SET);
        n = fread(buf, 1, want, f);
        SdCardManager::GetInstance().UnlockFs();
    }
    if (n == 0) {
        is_end = true;
        return true;
    }

    // 按字符数切片，优先在换行处断开（但不早于半页）。UTF-8 和 GBK 的字符长度不同
    size_t pos = 0;
    int chars = 0;
    size_t last_nl = 0;
    int chars_at_nl = 0;
    while (pos < n && chars < kCharsPerPage) {
        uint8_t c = (uint8_t)buf[pos];
        size_t clen = utf8_mode_ ? Utf8CharLen(c) : ((c < 0x80) ? 1 : 2);
        if (pos + clen > n) break;      // 末尾半个字符，留给下一页
        if (c == '\n') {
            last_nl = pos + clen;
            chars_at_nl = chars + 1;
        }
        pos += clen;
        chars++;
    }
    size_t cut = pos;
    if (pos < n && chars_at_nl > kCharsPerPage / 2) {
        cut = last_nl;                  // 断在换行处更整齐
    }
    if (cut == 0) cut = pos;            // 兜底

    if (utf8_mode_) {
        // 过滤 \r（Windows 换行）和孤立控制字符
        text.reserve(cut);
        for (size_t i = 0; i < cut; i++) {
            if (buf[i] == '\r') continue;
            if ((uint8_t)buf[i] == 0) continue;
            text.push_back(buf[i]);
        }
    } else {
        // GBK：先整体转 UTF-8，再清掉残余控制字符
        text = GbkToUtf8(buf, cut);
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    }

    next_offset = offset + cut;
    is_end = (next_offset >= file_size_);
    offset_ = offset;
    return true;
}

size_t SdBooks::RestoreProgress(const std::string& name) {
    size_t result = 0;
    if (!SdCardManager::GetInstance().LockFs()) return 0;
    FILE* f = fopen(kProgressFile, "r");
    if (f != nullptr) {
        static char line[192];
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            char* bar = strchr(line, '|');
            if (bar == nullptr) continue;
            *bar = '\0';
            if (name == line) {
                result = (size_t)strtoul(bar + 1, nullptr, 10);
                break;
            }
        }
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
    return result;
}

void SdBooks::SaveProgress(const std::string& name, size_t offset) {
    if (!SdCardManager::GetInstance().LockFs()) return;
    // 全量读改写：条目很少，简单可靠
    std::string out;
    FILE* f = fopen(kProgressFile, "r");
    if (f != nullptr) {
        static char line[192];
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            char* bar = strchr(line, '|');
            if (bar == nullptr) continue;
            std::string key(line, bar);
            if (key == name) continue;   // 旧的这条丢掉，后面重写
            out += line;
            out += '\n';
        }
        fclose(f);
    }
    char myself[192];
    snprintf(myself, sizeof(myself), "%s|%u\n", name.c_str(), (unsigned)offset);
    out += myself;

    f = fopen(kProgressFile, "w");
    if (f != nullptr) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    SdCardManager::GetInstance().UnlockFs();
}