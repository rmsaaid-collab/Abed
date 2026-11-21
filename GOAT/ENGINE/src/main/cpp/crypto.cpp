#include "include/crypto.h"
#include <string.h>
#include <math.h>

void AnzuPRNG::init(uint64_t seed) {
    state = (seed & 0xFFFFFFFFFFFFULL) ^ 0x5DEECE66DULL;
}

int AnzuPRNG::nextInt() {
    state = (state * 0x5DEECE66DULL + 11ULL) & 0xFFFFFFFFFFFFULL;
    return (int)((state >> 17) & 0x7FFFFFFFULL);
}

std::string AnzuPRNG::nextHexChars(int count) {
    static const char hexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(count);
    for (int i = 0; i < count; i++) {
        int val = nextInt();
        int idx = (val >> 27) & 0xF;
        result += hexChars[idx];
    }
    return result;
}

static inline uint32_t rotl32(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

static void quarterRound(uint32_t s[16], int a, int b, int c, int d) {
    s[a] = s[a] + s[b]; s[d] = rotl32(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d]; s[b] = rotl32(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b]; s[d] = rotl32(s[d] ^ s[a],  8);
    s[c] = s[c] + s[d]; s[b] = rotl32(s[b] ^ s[c],  7);
}

void chacha20Block(const uint8_t key[32], const uint8_t nonce[8],
                   uint64_t counter, uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e;
    s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        s[4 + i] = (uint32_t)key[i*4]
                 | ((uint32_t)key[i*4+1] << 8)
                 | ((uint32_t)key[i*4+2] << 16)
                 | ((uint32_t)key[i*4+3] << 24);
    }
    s[12] = (uint32_t)(counter & 0xFFFFFFFF);
    s[13] = (uint32_t)(counter >> 32);
    s[14] = (uint32_t)nonce[0]
          | ((uint32_t)nonce[1] << 8)
          | ((uint32_t)nonce[2] << 16)
          | ((uint32_t)nonce[3] << 24);
    s[15] = (uint32_t)nonce[4]
          | ((uint32_t)nonce[5] << 8)
          | ((uint32_t)nonce[6] << 16)
          | ((uint32_t)nonce[7] << 24);

    uint32_t w[16];
    memcpy(w, s, sizeof(w));

    for (int i = 0; i < 10; i++) {
        quarterRound(w, 0,4, 8,12); quarterRound(w, 1,5, 9,13);
        quarterRound(w, 2,6,10,14); quarterRound(w, 3,7,11,15);
        quarterRound(w, 0,5,10,15); quarterRound(w, 1,6,11,12);
        quarterRound(w, 2,7, 8,13); quarterRound(w, 3,4, 9,14);
    }

    for (int i = 0; i < 16; i++) w[i] += s[i];

    for (int i = 0; i < 16; i++) {
        out[i*4+0] =  w[i]        & 0xFF;
        out[i*4+1] = (w[i] >>  8) & 0xFF;
        out[i*4+2] = (w[i] >> 16) & 0xFF;
        out[i*4+3] = (w[i] >> 24) & 0xFF;
    }
}

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t hi = 0, lo = 0;
        char c1 = hex[i], c2 = hex[i+1];
        if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
        bytes.push_back((hi << 4) | lo);
    }
    return bytes;
}

std::vector<uint8_t> generateAnzuKeystream(uint64_t seed, int length) {
    AnzuPRNG keyPrng;
    keyPrng.init(seed);
    std::string keyHex = keyPrng.nextHexChars(64);
    std::vector<uint8_t> key = hexToBytes(keyHex);

    AnzuPRNG noncePrng;
    noncePrng.init(seed * 2);
    std::string nonceHex = noncePrng.nextHexChars(16);
    std::vector<uint8_t> nonce = hexToBytes(nonceHex);

    std::vector<uint8_t> ks;
    ks.reserve(length + 64);
    uint64_t counter = 0;
    while ((int)ks.size() < length) {
        uint8_t block[64];
        chacha20Block(key.data(), nonce.data(), counter, block);
        for (int i = 0; i < 64 && (int)ks.size() < length; i++)
            ks.push_back(block[i]);
        counter++;
    }
    return ks;
}

void JokerArgumentBuilder::init(const uint8_t* key) {
    memset(x, 0, sizeof(x));
    memset(c, 0, sizeof(c));
    carry = 0;

    uint16_t shorts[8];
    for (int i = 0; i < 8; i++)
        shorts[i] = ((uint16_t)key[i*2] << 8) | key[i*2+1];

    uint32_t v17 = ((uint32_t)shorts[0] << 16) | shorts[1];
    uint32_t v18 = ((uint32_t)shorts[2] << 16) | shorts[3];
    uint32_t v19 = ((uint32_t)shorts[4] << 16) | shorts[5];
    uint32_t v20 = ((uint32_t)shorts[6] << 16) | shorts[7];

    x[0] = v17; x[1] = v18; x[2] = v19; x[3] = v20;
    x[4] = (v17 << 16) | (v17 >> 16);
    x[5] = (v18 << 16) | (v18 >> 16);
    x[6] = (v19 << 16) | (v19 >> 16);
    x[7] = (v20 << 16) | (v20 >> 16);

    c[0] = v19; c[1] = v20; c[2] = v17; c[3] = v18;
    c[4] = x[6]; c[5] = x[7]; c[6] = x[4]; c[7] = x[5];

    for (int i = 0; i < 4; i++) _N();
    for (int i = 0; i < 8; i++) c[i] ^= x[(i + 4) & 7];
}

void JokerArgumentBuilder::_N() {
    const uint32_t A = 0x4D34D34D;
    uint32_t carryLocal = carry;
    for (int i = 0; i < 8; i++) {
        uint64_t temp = (uint64_t)c[i] + (uint64_t)A + carryLocal;
        carryLocal = (temp > 0xFFFFFFFFULL) ? 1 : 0;
        c[i] = (uint32_t)(temp & 0xFFFFFFFFU);
    }
    carry = carryLocal;

    uint32_t g[8];
    for (int i = 0; i < 8; i++) {
        uint64_t u = (uint64_t)((x[i] + c[i]) & 0xFFFFFFFFU);
        uint64_t uSq = u * u;
        g[i] = (uint32_t)((uSq >> 32) ^ uSq);
    }

    uint32_t newX[8];
    auto rotl16 = [](uint32_t v) -> uint32_t { return (v << 16) | (v >> 16); };
    for (int i = 0; i < 8; i++) {
        int idx1 = (i + 7) & 7;
        int idx2 = (i + 6) & 7;
        newX[i] = g[i] + rotl16(g[idx1]) + g[idx2];
    }
    memcpy(x, newX, sizeof(x));
}

void JokerArgumentBuilder::_KS(uint8_t out[16]) {
    uint32_t s[8];
    s[0] = ((x[5] >> 16) ^ x[0]);
    s[1] = ((x[7] >> 16) ^ x[2]);
    s[2] = ((x[1] >> 16) ^ x[4]);
    s[3] = ((x[3] >> 16) ^ x[6]);
    s[4] = ((x[6] >> 16) ^ x[1]);
    s[5] = ((x[0] >> 16) ^ x[3]);
    s[6] = ((x[2] >> 16) ^ x[5]);
    s[7] = ((x[4] >> 16) ^ x[7]);
    for (int i = 0; i < 8; i++) {
        out[i*2]   =  s[i]       & 0xFF;
        out[i*2+1] = (s[i] >> 8) & 0xFF;
    }
}

std::vector<uint8_t> JokerArgumentBuilder::generateKeystream(int length) {
    std::vector<uint8_t> result;
    result.reserve(length);
    while ((int)result.size() < length) {
        _N();
        uint8_t ks[16];
        _KS(ks);
        for (int i = 0; i < 16 && (int)result.size() < length; i++)
            result.push_back(ks[i]);
    }
    return result;
}

std::vector<uint8_t> generateFreshShotInfoKeystream(
    uint32_t gameId,
    const std::string& encPropsHex,
    float& outSceneCueX, float& outSceneCueY, bool& outHasSceneCue,
    float& outTrickTargetX, float& outTrickTargetY, bool& outHasTrickTarget)
{
    if (gameId <= 1000000 || encPropsHex.empty()) return {};

    std::vector<uint8_t> cipherBytes = hexToBytes(encPropsHex);
    std::vector<uint8_t> ks = generateAnzuKeystream(gameId, (int)cipherBytes.size());
    std::string plain;
    plain.reserve(cipherBytes.size());
    for (size_t i = 0; i < cipherBytes.size(); i++)
        plain += (char)(cipherBytes[i] ^ ks[i]);

    const char* needle = "\"shot_info_secret\":\"";
    size_t secretIdx = plain.find(needle);
    if (secretIdx == std::string::npos) return {};
    size_t secretStart = secretIdx + strlen(needle);
    if (secretStart + 32 > plain.size()) return {};
    std::string secretHex = plain.substr(secretStart, 32);

    std::vector<uint8_t> secretBytes = hexToBytes(secretHex);
    if (secretBytes.size() != 16) return {};

    JokerArgumentBuilder jab;
    jab.init(secretBytes.data());
    std::vector<uint8_t> shotInfoKs = jab.generateKeystream(51);

    {
        size_t numPos = 0;
        while ((numPos = plain.find("\"number\":0", numPos)) != std::string::npos) {
            size_t afterZero = numPos + 10;
            if (afterZero < plain.size() && plain[afterZero] >= '0' && plain[afterZero] <= '9') {
                numPos++; continue;
            }
            size_t blockStart = plain.rfind('{', numPos);
            size_t blockEnd = plain.find('}', numPos);
            if (blockStart == std::string::npos || blockEnd == std::string::npos) { numPos++; continue; }
            std::string block = plain.substr(blockStart, blockEnd - blockStart + 1);
            size_t centerPos = block.find("\"center\":[");
            if (centerPos == std::string::npos) { numPos++; continue; }
            size_t arrStart = centerPos + 10;
            size_t comma = block.find(',', arrStart);
            size_t arrEnd = block.find(']', arrStart);
            if (comma == std::string::npos || arrEnd == std::string::npos) { numPos++; continue; }
            float cx = strtof(block.c_str() + arrStart, nullptr);
            float cy = strtof(block.c_str() + comma + 1, nullptr);
            if (isfinite(cx) && isfinite(cy)) {
                outSceneCueX = cx;
                outSceneCueY = cy;
                outHasSceneCue = true;
            }
            break;
        }
    }

    {
        size_t targetPos = plain.find("\"target\"");
        if (targetPos != std::string::npos) {
            size_t centerPos = plain.find("\"center\":[", targetPos);
            if (centerPos != std::string::npos && centerPos < targetPos + 200) {
                size_t arrStart = centerPos + 10;
                size_t comma = plain.find(',', arrStart);
                size_t arrEnd = plain.find(']', arrStart);
                if (comma != std::string::npos && arrEnd != std::string::npos) {
                    float tx = strtof(plain.c_str() + arrStart, nullptr);
                    float ty = strtof(plain.c_str() + comma + 1, nullptr);
                    if (isfinite(tx) && isfinite(ty)) {
                        outTrickTargetX = tx;
                        outTrickTargetY = ty;
                        outHasTrickTarget = true;
                    }
                }
            }
        } else {
            outHasTrickTarget = false;
        }
    }

    return shotInfoKs;
}
