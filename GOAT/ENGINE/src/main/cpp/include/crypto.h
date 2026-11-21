#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct AnzuPRNG {
    uint64_t state;
    void init(uint64_t seed);
    int nextInt();
    std::string nextHexChars(int count);
};

void chacha20Block(const uint8_t key[32], const uint8_t nonce[8],
                   uint64_t counter, uint8_t out[64]);
std::vector<uint8_t> generateAnzuKeystream(uint64_t seed, int length);

struct JokerArgumentBuilder {
    uint32_t x[8];
    uint32_t c[8];
    uint32_t carry;

    void init(const uint8_t* secret16);
    void _N();
    void _KS(uint8_t out[16]);
    std::vector<uint8_t> generateKeystream(int length);
};

std::vector<uint8_t> generateFreshShotInfoKeystream(
    uint32_t gameId,
    const std::string& encPropsHex,
    float& outSceneCueX, float& outSceneCueY, bool& outHasSceneCue,
    float& outTrickTargetX, float& outTrickTargetY, bool& outHasTrickTarget);
