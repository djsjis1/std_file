#include "detail/internal.h"

using namespace My::detail;

namespace
{
    bool GlobMatch(std::string_view pattern, std::string_view name)
    {
        size_t pi = 0, ni = 0, starP = std::string_view::npos, starN = 0;
        while (ni < name.size())
        {
            if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni]))
            { ++pi; ++ni; }
            else if (pi < pattern.size() && pattern[pi] == '*')
            { starP = pi++; starN = ni; }
            else if (starP != std::string_view::npos)
            { pi = starP + 1; ni = ++starN; }
            else
                return false;
        }
        while (pi < pattern.size() && pattern[pi] == '*') ++pi;
        return pi == pattern.size();
    }

    constexpr uint32_t kSha256K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    constexpr uint32_t Rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Sha256Transform(uint32_t state[8], const uint8_t block[64])
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = Rr(w[i - 15], 7) ^ Rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rr(w[i - 2], 17) ^ Rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
                 e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t S1 = Rr(e, 6) ^ Rr(e, 11) ^ Rr(e, 25), ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
            uint32_t S0 = Rr(a, 2) ^ Rr(a, 13) ^ Rr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
} // namespace

// ==================== 目录遍历 ====================

std::vector<std::string> My::File::listFiles(std::string_view path)
{
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(ToPath(path), ec))
    {
        if (entry.is_regular_file())
        {
            const std::u8string u8name = entry.path().filename().u8string();
            result.push_back(std::string(reinterpret_cast<const char *>(u8name.data()), u8name.size()));
        }
    }
    return result;
}

bool My::File::walk(std::string_view path,
                    const std::function<bool(std::string_view, bool)> &callback)
{
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(ToPath(path), ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        const std::u8string u8path = it->path().u8string();
        if (!callback(std::string_view(reinterpret_cast<const char *>(u8path.data()), u8path.size()), it->is_directory()))
            return true;
    }
    return !ec;
}

std::vector<std::string> My::File::globFiles(std::string_view path, std::string_view pattern)
{
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(ToPath(path), ec))
    {
        const std::u8string u8name = entry.path().filename().u8string();
        const std::string name(reinterpret_cast<const char *>(u8name.data()), u8name.size());
        if (GlobMatch(pattern, name))
            result.push_back(name);
    }
    return result;
}

// ==================== SHA-256 哈希 ====================

std::optional<std::string> My::File::fileHash(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd) return std::nullopt;

    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    uint8_t pending[64]{};
    size_t pendingLen = 0;
    uint64_t totalBytes = 0;

    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0) return std::nullopt;
        if (r == 0) break;
        totalBytes += static_cast<size_t>(r);

        const uint8_t *data = reinterpret_cast<const uint8_t *>(buffer.get());
        size_t remaining = static_cast<size_t>(r);

        if (pendingLen > 0)
        {
            size_t need = 64 - pendingLen;
            size_t take = (std::min)(need, remaining);
            std::memcpy(pending + pendingLen, data, take);
            pendingLen += take;
            data += take;
            remaining -= take;
            if (pendingLen == 64) { Sha256Transform(state, pending); pendingLen = 0; }
        }
        while (remaining >= 64) { Sha256Transform(state, data); data += 64; remaining -= 64; }
        if (remaining > 0) { std::memcpy(pending, data, remaining); pendingLen = remaining; }
    }

    pending[pendingLen++] = 0x80;
    if (pendingLen > 56)
    {
        std::memset(pending + pendingLen, 0, 64 - pendingLen);
        Sha256Transform(state, pending);
        pendingLen = 0;
    }
    std::memset(pending + pendingLen, 0, 56 - pendingLen);
    uint64_t bits = totalBytes * 8;
    for (int i = 0; i < 8; ++i)
        pending[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    Sha256Transform(state, pending);

    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (int i = 0; i < 8; ++i)
    {
        result[i * 8 + 0] = hex[(state[i] >> 28) & 0xf];
        result[i * 8 + 1] = hex[(state[i] >> 24) & 0xf];
        result[i * 8 + 2] = hex[(state[i] >> 20) & 0xf];
        result[i * 8 + 3] = hex[(state[i] >> 16) & 0xf];
        result[i * 8 + 4] = hex[(state[i] >> 12) & 0xf];
        result[i * 8 + 5] = hex[(state[i] >> 8) & 0xf];
        result[i * 8 + 6] = hex[(state[i] >> 4) & 0xf];
        result[i * 8 + 7] = hex[state[i] & 0xf];
    }
    return result;
}
