#include "compress.h"

// ==========================================================================
// ファイル入出力ヘルパ
// ==========================================================================
bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) ifs.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(ifs);
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!data.empty())
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(ofs);
}

// std::filesystem::path 版 (Unicode ファイル名でも安全に開ける)
bool ReadFileFs(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) ifs.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(ifs);
}
bool WriteFileFs(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!data.empty())
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(ofs);
}

std::string PathToUtf8(const std::filesystem::path& p) {
    std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}
std::filesystem::path Utf8ToPath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

