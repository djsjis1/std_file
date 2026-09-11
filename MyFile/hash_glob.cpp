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
            {
                ++pi;
                ++ni;
            }
            else if (pi < pattern.size() && pattern[pi] == '*')
            {
                starP = pi++;
                starN = ni;
            }
            else if (starP != std::string_view::npos)
            {
                pi = starP + 1;
                ni = ++starN;
            }
            else
                return false;
        }
        while (pi < pattern.size() && pattern[pi] == '*')
            ++pi;
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
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
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
        if (ec)
            break;
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
    if (!fd)
        return std::nullopt;

    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    uint8_t pending[64]{};
    size_t pendingLen = 0;
    uint64_t totalBytes = 0;

    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt;
        if (r == 0)
            break;
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
            if (pendingLen == 64)
            {
                Sha256Transform(state, pending);
                pendingLen = 0;
            }
        }
        while (remaining >= 64)
        {
            Sha256Transform(state, data);
            data += 64;
            remaining -= 64;
        }
        if (remaining > 0)
        {
            std::memcpy(pending, data, remaining);
            pendingLen = remaining;
        }
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

// ==================== CRC32 ====================

namespace
{
    const uint32_t *Crc32Table()
    {
        static uint32_t table[256] = {};
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j)
                    c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        return table;
    }

    uint32_t Crc32Update(uint32_t crc, const char *data, size_t len)
    {
        const uint32_t *table = Crc32Table();
        crc = ~crc;
        for (size_t i = 0; i < len; ++i)
            crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    constexpr uint64_t kXXH_P1 = 0x9E3779B185EBCA87ULL;
    constexpr uint64_t kXXH_P2 = 0xC2B2AE3D27D4EB4FULL;
    constexpr uint64_t kXXH_P3 = 0x165667B19E3779F9ULL;
    constexpr uint64_t kXXH_P4 = 0x85EBCA77C2B2AE63ULL;
    constexpr uint64_t kXXH_P5 = 0x27D4EB2F165667C5ULL;

    uint64_t XXHR(uint64_t a, uint64_t i)
    {
        a += i * kXXH_P2;
        a = (a << 31) | (a >> 33);
        a *= kXXH_P1;
        return a;
    }
    uint64_t XXHA(uint64_t h)
    {
        h ^= h >> 33;
        h *= kXXH_P2;
        h ^= h >> 29;
        h *= kXXH_P3;
        h ^= h >> 32;
        return h;
    }
    uint64_t RLE64(const void *p)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        return uint64_t(b[0]) | (uint64_t(b[1]) << 8) | (uint64_t(b[2]) << 16) | (uint64_t(b[3]) << 24) | (uint64_t(b[4]) << 32) | (uint64_t(b[5]) << 40) | (uint64_t(b[6]) << 48) | (uint64_t(b[7]) << 56);
    }
    uint32_t RLE32(const void *p)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }

    uint64_t XXH64(const char *data, size_t len)
    {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
        const uint8_t *const end = p + len;
        uint64_t h;
        if (len >= 32)
        {
            const uint8_t *const limit = end - 32;
            uint64_t v1 = kXXH_P1 + kXXH_P2, v2 = kXXH_P2, v3 = 0, v4 = 0 - kXXH_P1;
            do
            {
                v1 = XXHR(v1, RLE64(p));
                p += 8;
                v2 = XXHR(v2, RLE64(p));
                p += 8;
                v3 = XXHR(v3, RLE64(p));
                p += 8;
                v4 = XXHR(v4, RLE64(p));
                p += 8;
            } while (p <= limit);
            h = ((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) + ((v3 << 12) | (v3 >> 52)) + ((v4 << 18) | (v4 >> 46));
            h = (h ^ (((v1 << 31) | (v1 >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
            h = (h ^ (((v2 << 31) | (v2 >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
            h = (h ^ (((v3 << 31) | (v3 >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
            h = (h ^ (((v4 << 31) | (v4 >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
        }
        else
            h = kXXH_P5;
        h += len;
        while (p + 8 <= end)
        {
            h ^= XXHR(0, RLE64(p));
            h = ((h << 27) | (h >> 37)) * kXXH_P1 + kXXH_P4;
            p += 8;
        }
        while (p + 4 <= end)
        {
            h ^= uint64_t(RLE32(p)) * kXXH_P1;
            h = ((h << 23) | (h >> 41)) * kXXH_P2 + kXXH_P3;
            p += 4;
        }
        while (p < end)
        {
            h ^= uint64_t(*p) * kXXH_P5;
            h = ((h << 11) | (h >> 53)) * kXXH_P1;
            ++p;
        }
        return XXHA(h);
    }

    std::string Hex64(uint64_t v)
    {
        static constexpr char h[] = "0123456789abcdef";
        std::string r(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            r[i] = h[v & 0xF];
            v >>= 4;
        }
        return r;
    }
    std::string Hex32(uint32_t v)
    {
        static constexpr char h[] = "0123456789abcdef";
        std::string r(8, '0');
        for (int i = 7; i >= 0; --i)
        {
            r[i] = h[v & 0xF];
            v >>= 4;
        }
        return r;
    }
} // namespace

// ==================== filesEqual ====================

bool My::File::filesEqual(std::string_view file1, std::string_view file2)
{
    const std::filesystem::path p1 = ToPath(file1), p2 = ToPath(file2);
    std::error_code ec;
    const auto s1 = std::filesystem::file_size(p1, ec);
    if (ec)
        return false;
    const auto s2 = std::filesystem::file_size(p2, ec);
    if (ec || s1 != s2)
        return false;
    if (s1 == 0)
        return true;
    Fd fd1(OpenRead(p1)), fd2(OpenRead(p2));
    if (!fd1 || !fd2)
        return false;
    const auto b1 = std::make_unique<char[]>(kIoBlockSize);
    const auto b2 = std::make_unique<char[]>(kIoBlockSize);
    std::uintmax_t rem = s1;
    while (rem > 0)
    {
        const size_t chunk = static_cast<size_t>((std::min)(rem, std::uintmax_t(kIoBlockSize)));
        if (ReadFull(fd1.get(), b1.get(), chunk) != chunk)
            return false;
        if (ReadFull(fd2.get(), b2.get(), chunk) != chunk)
            return false;
        if (std::memcmp(b1.get(), b2.get(), chunk) != 0)
            return false;
        rem -= chunk;
    }
    return true;
}

// ==================== fileCrc32 ====================

std::optional<std::string> My::File::fileCrc32(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
        return std::nullopt;
    uint32_t crc = 0;
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt;
        if (r == 0)
            break;
        crc = Crc32Update(crc, buffer.get(), static_cast<size_t>(r));
    }
    return Hex32(crc);
}

// ==================== fileXxHash64 ====================

std::optional<std::string> My::File::fileXxHash64(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
        return std::nullopt;
    My::Hasher hasher(My::Hasher::Algorithm::XxHash64);
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt;
        if (r == 0)
            break;
        hasher.update(buffer.get(), static_cast<size_t>(r));
    }
    return hasher.finalize();
}

// ==================== Hasher ====================

My::Hasher::Hasher(Algorithm algo) : algo_(algo)
{
    if (algo_ == Algorithm::Sha256)
    {
        shaState_[0] = 0x6a09e667;
        shaState_[1] = 0xbb67ae85;
        shaState_[2] = 0x3c6ef372;
        shaState_[3] = 0xa54ff53a;
        shaState_[4] = 0x510e527f;
        shaState_[5] = 0x9b05688c;
        shaState_[6] = 0x1f83d9ab;
        shaState_[7] = 0x5be0cd19;
        shaPendingLen_ = 0;
        shaTotalBytes_ = 0;
    }
    else if (algo_ == Algorithm::Crc32)
    {
        crcValue_ = 0xFFFFFFFF;
    }
    else
    {
        xxState_[0] = kXXH_P1 + kXXH_P2;
        xxState_[1] = kXXH_P2;
        xxState_[2] = 0;
        xxState_[3] = 0 - kXXH_P1;
        xxTotalLen_ = 0;
        xxBufferLen_ = 0;
    }
}

void My::Hasher::sha256Transform(uint32_t state[8], const uint8_t block[64])
{
    static constexpr uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto Rr = [](uint32_t x, int n) -> uint32_t
    { return (x >> n) | (x << (32 - n)); };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = Rr(w[i - 15], 7) ^ Rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rr(w[i - 2], 17) ^ Rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1 = Rr(e, 6) ^ Rr(e, 11) ^ Rr(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K[i] + w[i], S0 = Rr(a, 2) ^ Rr(a, 13) ^ Rr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void My::Hasher::update(const char *data, size_t len)
{
    if (algo_ == Algorithm::Sha256)
    {
        shaTotalBytes_ += len;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
        size_t rem = len;
        if (shaPendingLen_ > 0)
        {
            size_t need = 64 - shaPendingLen_, take = (std::min)(need, rem);
            std::memcpy(shaPending_ + shaPendingLen_, p, take);
            shaPendingLen_ += take;
            p += take;
            rem -= take;
            if (shaPendingLen_ == 64)
            {
                sha256Transform(shaState_, shaPending_);
                shaPendingLen_ = 0;
            }
        }
        while (rem >= 64)
        {
            sha256Transform(shaState_, p);
            p += 64;
            rem -= 64;
        }
        if (rem > 0)
        {
            std::memcpy(shaPending_, p, rem);
            shaPendingLen_ = rem;
        }
    }
    else if (algo_ == Algorithm::Crc32)
    {
        const uint32_t *table = Crc32Table();
        uint32_t c = crcValue_;
        for (size_t i = 0; i < len; ++i)
            c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
        crcValue_ = c;
    }
    else
    {
        xxTotalLen_ += len;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
        size_t rem = len;
        if (xxBufferLen_ > 0)
        {
            size_t need = 32 - xxBufferLen_, take = (std::min)(need, rem);
            std::memcpy(xxBuffer_ + xxBufferLen_, p, take);
            xxBufferLen_ += take;
            p += take;
            rem -= take;
            if (xxBufferLen_ == 32)
            {
                xxState_[0] = XXHR(xxState_[0], RLE64(xxBuffer_));
                xxState_[1] = XXHR(xxState_[1], RLE64(xxBuffer_ + 8));
                xxState_[2] = XXHR(xxState_[2], RLE64(xxBuffer_ + 16));
                xxState_[3] = XXHR(xxState_[3], RLE64(xxBuffer_ + 24));
                xxBufferLen_ = 0;
            }
        }
        while (rem >= 32)
        {
            xxState_[0] = XXHR(xxState_[0], RLE64(p));
            p += 8;
            xxState_[1] = XXHR(xxState_[1], RLE64(p));
            p += 8;
            xxState_[2] = XXHR(xxState_[2], RLE64(p));
            p += 8;
            xxState_[3] = XXHR(xxState_[3], RLE64(p));
            p += 8;
            rem -= 32;
        }
        if (rem > 0)
        {
            std::memcpy(xxBuffer_, p, rem);
            xxBufferLen_ = rem;
        }
    }
}

std::string My::Hasher::sha256Final()
{
    uint32_t state[8];
    std::memcpy(state, shaState_, sizeof(state));
    uint8_t pending[128];
    std::memcpy(pending, shaPending_, shaPendingLen_);
    size_t pLen = shaPendingLen_;
    pending[pLen++] = 0x80;
    if (pLen > 56)
    {
        std::memset(pending + pLen, 0, 64 - pLen);
        sha256Transform(state, pending);
        pLen = 0;
    }
    std::memset(pending + pLen, 0, 56 - pLen);
    uint64_t bits = shaTotalBytes_ * 8;
    for (int i = 0; i < 8; ++i)
        pending[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    sha256Transform(state, pending);
    shaState_[0] = 0x6a09e667;
    shaState_[1] = 0xbb67ae85;
    shaState_[2] = 0x3c6ef372;
    shaState_[3] = 0xa54ff53a;
    shaState_[4] = 0x510e527f;
    shaState_[5] = 0x9b05688c;
    shaState_[6] = 0x1f83d9ab;
    shaState_[7] = 0x5be0cd19;
    shaPendingLen_ = 0;
    shaTotalBytes_ = 0;
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

std::string My::Hasher::crc32Final()
{
    std::string r = Hex32(crcValue_ ^ 0xFFFFFFFF);
    crcValue_ = 0xFFFFFFFF;
    return r;
}

std::string My::Hasher::xxHash64Final()
{
    uint64_t h;
    if (xxTotalLen_ >= 32)
    {
        h = ((xxState_[0] << 1) | (xxState_[0] >> 63)) + ((xxState_[1] << 7) | (xxState_[1] >> 57)) + ((xxState_[2] << 12) | (xxState_[2] >> 52)) + ((xxState_[3] << 18) | (xxState_[3] >> 46));
        h = (h ^ (((xxState_[0] << 31) | (xxState_[0] >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
        h = (h ^ (((xxState_[1] << 31) | (xxState_[1] >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
        h = (h ^ (((xxState_[2] << 31) | (xxState_[2] >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
        h = (h ^ (((xxState_[3] << 31) | (xxState_[3] >> 33)) * kXXH_P1)) * kXXH_P1 + kXXH_P4;
    }
    else
        h = kXXH_P5;
    h += xxTotalLen_;
    const uint8_t *p = xxBuffer_;
    size_t rem = xxBufferLen_;
    while (rem >= 8)
    {
        h ^= XXHR(0, RLE64(p));
        h = ((h << 27) | (h >> 37)) * kXXH_P1 + kXXH_P4;
        p += 8;
        rem -= 8;
    }
    while (rem >= 4)
    {
        h ^= uint64_t(RLE32(p)) * kXXH_P1;
        h = ((h << 23) | (h >> 41)) * kXXH_P2 + kXXH_P3;
        p += 4;
        rem -= 4;
    }
    while (rem > 0)
    {
        h ^= uint64_t(*p) * kXXH_P5;
        h = ((h << 11) | (h >> 53)) * kXXH_P1;
        ++p;
        --rem;
    }
    h = XXHA(h);
    xxState_[0] = kXXH_P1 + kXXH_P2;
    xxState_[1] = kXXH_P2;
    xxState_[2] = 0;
    xxState_[3] = 0 - kXXH_P1;
    xxTotalLen_ = 0;
    xxBufferLen_ = 0;
    return Hex64(h);
}

std::string My::Hasher::finalize()
{
    if (algo_ == Algorithm::Sha256)
        return sha256Final();
    if (algo_ == Algorithm::Crc32)
        return crc32Final();
    return xxHash64Final();
}
