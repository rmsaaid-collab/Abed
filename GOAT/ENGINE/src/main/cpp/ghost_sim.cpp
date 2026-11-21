#include "include/ghost_sim.h"
#include "include/crypto.h"
#include "include/license_security.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>

GhostState g_ghost;
bool ghostCallActive = false;

std::mutex         trajectoryMutex;
std::vector<float> cachedTrajectoryScreenCoords;
bool               hasNewTrajectory = false;
std::vector<int>   cachedWillPocket;
std::vector<int>   cachedPocketIndices;
int                cachedFirstHitBall = -1;
float              cachedCueFinalX = 0.f;
float              cachedCueFinalZ = 0.f;
float              cachedBall8FinalX = 0.f;
float              cachedBall8FinalZ = 0.f;
bool               cachedBall8Moved = false;
float              cachedAllBallFinals[16][2] = {};
bool               cachedAllBallMoved[16] = {};

float g_vpMatrix[16] = {};
int   g_screenWidth  = 0;
int   g_screenHeight = 0;
bool  g_w2sReady     = false;

GetMainCamera_t      g_Camera_get_main      = nullptr;
Screen_getDim_t      g_Screen_get_width     = nullptr;
Screen_getDim_t      g_Screen_get_height    = nullptr;
GetMatrixInjected_t  g_get_worldToCameraMatrix = nullptr;
GetMatrixInjected_t  g_get_projectionMatrix    = nullptr;

GetCueFloat_t g_GetCuePower      = nullptr;
GetCueFloat_t g_GetCueSpin       = nullptr;
GetCueFloat_t g_GetMaxMasseAngle = nullptr;

uintptr_t g_il2cppBase = 0;
RustBridgeSim_t orig_RustBridgeSim = nullptr;
il2cpp_array_new_t    g_il2cpp_array_new    = nullptr;
il2cpp_gchandle_new_t g_il2cpp_gchandle_new = nullptr;

uint64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string readIl2CppString(void* strObj) {
    if (!strObj) return "";
    int len = *(int*)((uint8_t*)strObj + 0x10);
    if (len <= 0 || len > 100000) return "";
    uint16_t* chars = (uint16_t*)((uint8_t*)strObj + 0x14);
    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; i++)
        result += (char)(chars[i] & 0xFF);
    return result;
}

void writeFloatBE(uint8_t* out, float f) {
    uint32_t u; __builtin_memcpy(&u, &f, 4);
    out[0] = (u >> 24) & 0xFF;
    out[1] = (u >> 16) & 0xFF;
    out[2] = (u >>  8) & 0xFF;
    out[3] = (u      ) & 0xFF;
}

static float readFloatBE(const uint8_t* d) {
    uint32_t u = ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|((uint32_t)d[2]<<8)|(uint32_t)d[3];
    float f; __builtin_memcpy(&f, &u, 4); return f;
}

int buildKnownPlaintext(uint8_t* out, float dir0, float dir1, float force,
                        float spin0, float spin1) {
    out[0]  = 0x97;
    out[1]  = 0x92;
    out[2]  = 0xCA; writeFloatBE(out+3,  dir0);
    out[7]  = 0xCA; writeFloatBE(out+8,  dir1);
    out[12] = 0xCA; writeFloatBE(out+13, force);
    out[17] = 0x92;
    out[18] = 0xCA; writeFloatBE(out+19, spin0);
    out[23] = 0xCA; writeFloatBE(out+24, spin1);
    return 28;
}

void buildTailPlaintext(uint8_t* out, bool randOff, bool brkPow, float incline) {
    out[0] = randOff ? 0xC3 : 0xC2;
    out[1] = brkPow  ? 0xC3 : 0xC2;
    out[2] = 0xCA; writeFloatBE(out+3, incline);
}

int findShotInfoInBuffer(uint8_t* data, int len) {
    for (int i = 0; i < len - 3; i++) {
        if (data[i] == 0xDC && data[i+1] == 0x00 && data[i+2] == 0x33) {
            int p = i + 3;
            bool valid = true;
            for (int j = 0; j < 0x33 && valid; j++) {
                if (p >= len) { valid = false; break; }
                if      (data[p] < 0x80) p += 1;
                else if (data[p] == 0xCC && p+1 < len) p += 2;
                else    { valid = false; }
            }
            if (valid) return i;
        }
    }
    return -1;
}

void deriveGhostKeystream() {
    if (!g_ghost.hasShotInfo || g_ghost.shotInfoOffset < 0 || g_ghost.capturedLen == 0) return;

    uint8_t encBytes[51] = {};
    uint8_t* src = g_ghost.capturedData + g_ghost.shotInfoOffset + 3;
    int pos = 0;
    for (int i = 0; i < 51; i++) {
        if (src[pos] < 0x80) encBytes[i] = src[pos++];
        else if (src[pos] == 0xCC) { encBytes[i] = src[pos+1]; pos += 2; }
        else return;
    }

    memcpy(g_ghost.ghostOrigEncBytes, encBytes, 51);

    uint8_t plain[28] = {};
    buildKnownPlaintext(plain, g_ghost.dir[0], g_ghost.dir[1], g_ghost.force,
                        g_ghost.spin[0], g_ghost.spin[1]);
    for (int i = 0; i < 28; i++)
        g_ghost.ghostKeystream[i] = encBytes[i] ^ plain[i];

    uint8_t tail[7] = {};
    buildTailPlaintext(tail, g_ghost.randOff, g_ghost.brkPow, g_ghost.incline);
    for (int i = 0; i < 7; i++)
        g_ghost.ghostKeystream[44 + i] = encBytes[44 + i] ^ tail[i];

    if (g_ghost.hasModifiers) {
        for (int i = 0; i < 16; i++)
            g_ghost.ghostKeystream[28 + i] = encBytes[28 + i] ^ g_ghost.capturedModifiersBytes[i];
    } else {
        uint8_t defaultMod[16] = {0x93, 0xCA, 0,0,0,0, 0xCA, 0,0,0,0, 0xCA, 0x41,0xF0,0,0};
        for (int i = 0; i < 16; i++)
            g_ghost.ghostKeystream[28 + i] = encBytes[28 + i] ^ defaultMod[i];
    }

    g_ghost.hasKeystream = true;

    if (!g_ghost.freshEncPropsString.empty() && g_ghost.gameId > 1000000) {
        float sx, sy, tx, ty; bool hs, ht;
        auto freshKs = generateFreshShotInfoKeystream(
            g_ghost.gameId, g_ghost.freshEncPropsString, sx, sy, hs, tx, ty, ht);
        if (freshKs.size() >= 51) {
            bool match = true;
            for (int i = 0; i < 28; i++) {
                if (g_ghost.ghostKeystream[i] != freshKs[i]) { match = false; break; }
            }
            char xorHex[80] = {}, chaHex[80] = {};
            for (int i = 0; i < 10; i++) {
                snprintf(xorHex + i*3, 4, "%02X ", g_ghost.ghostKeystream[i]);
                snprintf(chaHex + i*3, 4, "%02X ", freshKs[i]);
            }
        }
    }
}

struct Vec3 { float x, y, z; };
typedef Vec3 (*WorldToScreenPoint_t)(void* cam, Vec3 pos, void* method);
static WorldToScreenPoint_t g_WorldToScreenPoint = nullptr;
typedef void (*TransformGetPosition_Injected_t)(void* transform, float* outPos);
static TransformGetPosition_Injected_t g_TransformGetPosition_Injected = nullptr;
static void* g_cachedMainCamera = nullptr;

#define RVA_WorldToScreenPoint 0x9B3411C

void initW2S() {
    if (!g_Screen_get_width || !g_Screen_get_height) return;
    g_screenWidth  = g_Screen_get_width(nullptr);
    g_screenHeight = g_Screen_get_height(nullptr);

    if (!g_WorldToScreenPoint && g_il2cppBase) {
        g_WorldToScreenPoint = (WorldToScreenPoint_t)(g_il2cppBase + RVA_WorldToScreenPoint);
    }

    if (!g_TransformGetPosition_Injected) {
        typedef void* (*resolve_icall_t)(const char*);
        void* il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
        if (il2cpp_handle) {
            auto resolve = (resolve_icall_t)dlsym(il2cpp_handle, "il2cpp_resolve_icall");
            if (resolve) {
                g_TransformGetPosition_Injected = (TransformGetPosition_Injected_t)
                    resolve("UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)");
            }
        }
    }

    if (g_screenWidth > 0 && g_screenHeight > 0 && g_WorldToScreenPoint)
        g_w2sReady = true;
}

static inline bool isValidPtr(void* p) {
    uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x0000800000000000ULL && (v & 0x3) == 0;
}

void captureTableWorldY() {
    if (!g_TransformGetPosition_Injected || !g_ghost.lastPoolGameController) return;
    if (!g_ghost.bihActive) return;
    
    
    if (g_ghost.bihActivatedMs == 0) return;
    if (nowMs() - g_ghost.bihActivatedMs < 800) return;
    void* bihCtrl = *(void**)((uint8_t*)g_ghost.lastPoolGameController + OFF_PGC_BallInHandCtrl);
    if (!bihCtrl || !isValidPtr(bihCtrl)) return;
    void* cueBallTransform = *(void**)((uint8_t*)bihCtrl + 0x40);
    if (!cueBallTransform || !isValidPtr(cueBallTransform)) return;
    void* nativeTransform = *(void**)((uint8_t*)cueBallTransform + 0x10);
    if (!nativeTransform || !isValidPtr(nativeTransform)) return;
    
    uintptr_t ntv = (uintptr_t)nativeTransform;
    if (ntv < 0x1000000 || ntv > 0x0000800000000000ULL) return;
    float pos[3] = {0};
    g_TransformGetPosition_Injected(nativeTransform, pos);
    if (pos[1] > 0.1f && pos[1] < 3.0f) {
        if (fabsf(pos[1] - g_ghost.tableWorldY) > 0.01f)
        g_ghost.tableWorldY = pos[1];
    }
}

bool refreshCameraMatrices() {
    if (!g_Camera_get_main) return false;
    g_cachedMainCamera = g_Camera_get_main(nullptr);
    if (!g_cachedMainCamera) return false;
    
    
    return true;
}

bool worldToScreen(float wx, float wz, float& sx, float& sy) {
    if (!g_WorldToScreenPoint || !g_cachedMainCamera) return false;
    Vec3 worldPos = { wx, g_ghost.tableWorldY, wz };
    Vec3 screenPos = g_WorldToScreenPoint(g_cachedMainCamera, worldPos, nullptr);
    float androidY = (float)g_screenHeight - screenPos.y;
    sx = screenPos.x;
    sy = androidY;
    return (screenPos.z > 0 && sx > -500 && sx < g_screenWidth + 500 && sy > -500 && sy < g_screenHeight + 500);
}

static std::vector<uint8_t> buildStr16Bytes(const std::string& str) {
    int slen = (int)str.size();
    std::vector<uint8_t> out(3 + slen);
    out[0] = 0xDA;
    out[1] = (slen >> 8) & 0xFF;
    out[2] = slen & 0xFF;
    for (int i = 0; i < slen; i++) out[3 + i] = (uint8_t)str[i];
    return out;
}

static int buildShotInfoSection(uint8_t* out, const uint8_t enc51[51]) {
    out[0] = 0xDC; out[1] = 0x00; out[2] = 0x33;
    int pos = 3;
    for (int i = 0; i < 51; i++) {
        uint8_t b = enc51[i];
        if (b > 0x7F) { out[pos++] = 0xCC; out[pos++] = b; }
        else          { out[pos++] = b; }
    }
    return pos;
}

static void buildPrefix(uint8_t* out, uint32_t gid, int turn) {
    out[0] = 0x96;
    out[1] = turn & 0x7F;
    out[2] = 0xCE;
    out[3] = (gid >> 24) & 0xFF;
    out[4] = (gid >> 16) & 0xFF;
    out[5] = (gid >>  8) & 0xFF;
    out[6] =  gid        & 0xFF;
}

static bool parseBall1Proper(const uint8_t* d, int len, float& outX, float& outY);

static int findClosestPocket(float x, float y) {
    int best = 0;
    float bestDist = 1e10f;
    for (int p = 0; p < 6; p++) {
        float dx = x - POCKET_POS[p][0];
        float dy = y - POCKET_POS[p][1];
        float d = dx*dx + dy*dy;
        if (d < bestDist) { bestDist = d; best = p; }
    }
    return best + 201;
}

void extractTrajectoryFromMsgPackResponse(void* responseByteArray) {
    if (!responseByteArray) return;
    int respLen = *(int*)((uint8_t*)responseByteArray + 0x18);
    if (respLen < 10) return;
    uint8_t* respData = (uint8_t*)responseByteArray + 0x20;

    MsgPackReader reader(respData, respLen);
    int outerLen = reader.readArrayLen();
    if (outerLen < 1) return;

    int numBalls = reader.readArrayLen();
    if (numBalls <= 0 || numBalls > 20) return;

    std::vector<float> screenCoords;

    int firstHitBall = -1;

    struct BodyFinalPos { int idx; float lastX, lastY; float initX, initY; bool moved; };
    std::vector<BodyFinalPos> bodyFinals;

    for (int ballIdx = 0; ballIdx < numBalls; ballIdx++) {
        
        if (ballIdx <= 15) g_ghost.allBallWaypoints[ballIdx].clear();

        int numFrames = reader.readArrayLen();
        if (numFrames < 2) {
            for (int f = 0; f < numFrames; f++) reader.skipElement();
            continue;
        }

        std::vector<float> ballX, ballY;
        ballX.reserve(numFrames);
        ballY.reserve(numFrames);

        for (int f = 0; f < numFrames; f++) {
            int coordLen = reader.readArrayLen();
            if (coordLen >= 2) {
                float x = reader.readNumericAsFloat();
                float y = reader.readNumericAsFloat();
                ballX.push_back(x);
                ballY.push_back(y);
                for (int c = 2; c < coordLen; c++) reader.skipElement();
            } else {
                for (int c = 0; c < coordLen; c++) reader.skipElement();
            }
        }

        int n = (int)ballX.size();
        if (n < 2) continue;

        float dx = ballX.back() - ballX[0];
        float dy = ballY.back() - ballY[0];
        bool moved = (dx*dx + dy*dy > 0.0001f);

        
        
        
        if (ballIdx <= 15 && moved) {
            auto& wvec = g_ghost.allBallWaypoints[ballIdx];
            wvec.clear();
            int step = (n > 60) ? (n / 60) : 1;
            for (int i = 0; i < n; i += step)
                wvec.emplace_back(ballX[i], ballY[i]);
            if (wvec.empty() || wvec.back().first != ballX.back())
                wvec.emplace_back(ballX.back(), ballY.back());
            g_ghost.allBallWaypointsInit[ballIdx][0] = ballX[0];
            g_ghost.allBallWaypointsInit[ballIdx][1] = ballY[0];
        } else if (ballIdx <= 15) {
            g_ghost.allBallWaypoints[ballIdx].clear();
        }

        bodyFinals.push_back({ballIdx, ballX.back(), ballY.back(), ballX[0], ballY[0], moved});

        if (!moved) continue;
        if (!g_w2sReady) continue;

        float radiusPx = 10.f;
        {
            float sx1, sy1, sx2, sy2;
            if (worldToScreen(ballX[0], ballY[0], sx1, sy1) &&
                worldToScreen(ballX[0] + BALL_RADIUS_WORLD, ballY[0], sx2, sy2)) {
                radiusPx = fabsf(sx2 - sx1);
                if (radiusPx < 3.f) radiusPx = 3.f;
                if (radiusPx > 30.f) radiusPx = 30.f;
            }
        }

        screenCoords.push_back(-9999.f);
        screenCoords.push_back((float)ballIdx);
        screenCoords.push_back(radiusPx);

        int stepSize = n > 100 ? n / 50 : (n > 50 ? 2 : 1);
        if (stepSize < 1) stepSize = 1;

        for (int i = 0; i < n; i += stepSize) {
            float sx, sy;
            if (worldToScreen(ballX[i], ballY[i], sx, sy)) {
                screenCoords.push_back(sx);
                screenCoords.push_back(sy);
            }
        }
        if ((n - 1) % stepSize != 0) {
            float sx, sy;
            if (worldToScreen(ballX[n-1], ballY[n-1], sx, sy)) {
                screenCoords.push_back(sx);
                screenCoords.push_back(sy);
            }
        }
    }

    if (g_ghost.hasTrickTarget && g_w2sReady) {
        float ball1X = 0, ball1Y = 0;
        bool hasBall1 = parseBall1Proper(respData, respLen, ball1X, ball1Y);

        if (hasBall1) {
            double ddx = (double)ball1X - (double)g_ghost.trickTargetX;
            double ddy = (double)ball1Y - (double)g_ghost.trickTargetY;
            float dist = (float)sqrt(ddx * ddx + ddy * ddy);

            float tgtSX, tgtSY;
            if (worldToScreen(g_ghost.trickTargetX, g_ghost.trickTargetY, tgtSX, tgtSY)) {
                screenCoords.push_back(-8888.f);
                screenCoords.push_back(tgtSX);
                screenCoords.push_back(tgtSY);
                screenCoords.push_back(dist);
            }
        }
    }

    if (outerLen >= 2) {
        int collCount = reader.readArrayLen();
        if (collCount > 0 && collCount <= 10000) {
            uint32_t bestIter = 0xFFFFFFFFu;
            for (int ci = 0; ci < collCount; ci++) {
                int entryLen = reader.readArrayLen();
                int bodyA = -1, bodyB = -1;
                uint32_t iter = 0;
                if (entryLen >= 1) bodyA = (int)reader.readNumericAsFloat();
                if (entryLen >= 2) bodyB = (int)reader.readNumericAsFloat();
                if (entryLen >= 3) iter  = (uint32_t)reader.readNumericAsFloat();
                for (int ei = 3; ei < entryLen; ei++) reader.skipElement();

                int other = -1;
                if      (bodyA == 0 && bodyB >= 1 && bodyB <= 15) other = bodyB;
                else if (bodyB == 0 && bodyA >= 1 && bodyA <= 15) other = bodyA;
                if (other > 0 && iter < bestIter) {
                    bestIter = iter;
                    firstHitBall = other;
                }
            }
        } else if (collCount > 0) {
            for (int ci = 0; ci < collCount; ci++) reader.skipElement();
        }
    }

    std::vector<int> pocketedBalls;
    std::vector<int> pocketIndices;
    if (outerLen >= 3) {
        int pottCount = reader.readArrayLen();
        if (pottCount > 0 && pottCount <= 30) {
            for (int pi = 0; pi < pottCount; pi++) {
                int entryLen = reader.readArrayLen();
                if (entryLen >= 1) {
                    int bodyNum = (int)reader.readNumericAsFloat();
                    pocketedBalls.push_back(bodyNum);
                    for (int ei = 1; ei < entryLen; ei++) reader.skipElement();
                    int pocketIdx = -1;
                    for (auto& bf : bodyFinals) {
                        if (bf.idx == bodyNum && bf.moved) {
                            pocketIdx = findClosestPocket(bf.lastX, bf.lastY);
                            break;
                        }
                    }
                    pocketIndices.push_back(pocketIdx);
                } else {
                    reader.skipElement();
                }
            }
        }
    }

    
    
    g_ghost.hasBall8Waypoints = false;
    g_ghost.ball8Waypoints.clear();
    g_ghost.hasCueWaypoints = false;
    g_ghost.cueWaypoints.clear();
    for (auto& bf : bodyFinals) {
        if (bf.idx == 8) {
            g_ghost.ball8InitX = bf.initX;
            g_ghost.ball8InitZ = bf.initY;
            if (bf.idx <= 15 && !g_ghost.allBallWaypoints[bf.idx].empty()) {
                g_ghost.ball8Waypoints = g_ghost.allBallWaypoints[bf.idx];
                g_ghost.hasBall8Waypoints = true;
            }
        }
        if (bf.idx == 0) {
            if (bf.idx <= 15 && !g_ghost.allBallWaypoints[bf.idx].empty()) {
                g_ghost.cueWaypoints = g_ghost.allBallWaypoints[bf.idx];
                g_ghost.hasCueWaypoints = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(trajectoryMutex);
        cachedTrajectoryScreenCoords = std::move(screenCoords);
        cachedWillPocket = std::move(pocketedBalls);
        cachedPocketIndices = std::move(pocketIndices);
        cachedFirstHitBall = firstHitBall;
        cachedBall8Moved = false;
        memset(cachedAllBallMoved, 0, sizeof(cachedAllBallMoved));
        for (auto& bf : bodyFinals) {
            if (bf.idx == 0) {
                cachedCueFinalX = bf.lastX;
                cachedCueFinalZ = bf.lastY;
            }
            if (bf.idx == 8) {
                cachedBall8FinalX = bf.lastX;
                cachedBall8FinalZ = bf.lastY;
                float d8x = bf.lastX - bf.initX;
                float d8z = bf.lastY - bf.initY;
                cachedBall8Moved = (d8x*d8x + d8z*d8z > 0.0001f);
            }
            if (bf.idx >= 0 && bf.idx < 16) {
                cachedAllBallFinals[bf.idx][0] = bf.lastX;
                cachedAllBallFinals[bf.idx][1] = bf.lastY;
                cachedAllBallMoved[bf.idx] = bf.moved;
            }
        }
        hasNewTrajectory = true;

        if (!g_ghost.hasBallPositions && numBalls > 0 && numBalls <= 16) {
            g_ghost.ballCount = numBalls;
            memset(g_ghost.ballOnTable, 0, sizeof(g_ghost.ballOnTable));
            memset(g_ghost.ballPos, 0, sizeof(g_ghost.ballPos));
            for (auto& bf : bodyFinals) {
                if (bf.idx >= 0 && bf.idx < 16) {
                    g_ghost.ballPos[bf.idx][0] = bf.initX;
                    g_ghost.ballPos[bf.idx][1] = bf.initY;
                    g_ghost.ballOnTable[bf.idx] = true;
                }
            }
            g_ghost.hasBallPositions = true;
        }
    }
}

const std::vector<uint8_t>& getOrRefreshFreshKeystream() {
    static std::vector<uint8_t> empty;
    if (g_ghost.gameId <= 1000000 || g_ghost.freshEncPropsString.empty()) return empty;

    if (g_ghost.ksCacheEncPropsHash == g_ghost.freshEncPropsString &&
        !g_ghost.ksCacheKeystream.empty()) {
        return g_ghost.ksCacheKeystream;
    }

    float scX, scY, ttX, ttY;
    bool hasSc = false, hasTt = false;
    auto ks = generateFreshShotInfoKeystream(
        g_ghost.gameId, g_ghost.freshEncPropsString,
        scX, scY, hasSc, ttX, ttY, hasTt);
    if (ks.empty()) return empty;

    if (hasSc) {
        g_ghost.sceneCueBallX = scX; g_ghost.sceneCueBallY = scY;
        g_ghost.hasSceneCueBallPos = true;
    }
    if (hasTt) {
        g_ghost.trickTargetX = ttX; g_ghost.trickTargetY = ttY;
        g_ghost.hasTrickTarget = true;
    } else {
        g_ghost.hasTrickTarget = false;
    }

    g_ghost.ksCacheEncPropsHash = g_ghost.freshEncPropsString;
    g_ghost.ksCacheKeystream = std::move(ks);
    return g_ghost.ksCacheKeystream;
}

void performGhostSimulation() {
    if (!orig_RustBridgeSim) return;
    if (!g_ghost.capturedKlass) return;

    bool preShotReady = (g_ghost.gameId > 1000000 &&
                         !g_ghost.freshEncPropsString.empty() &&
                         !g_ghost.freshSignatureString.empty());
    bool useCaptured = (!preShotReady &&
                        g_ghost.capturedLen > 100 &&
                        g_ghost.shotInfoOffset >= 0 &&
                        g_ghost.shotInfoSectionEnd > g_ghost.shotInfoOffset &&
                        g_ghost.capturedGameId == g_ghost.gameId);
    uint8_t activeKeystream[51];

    if (g_ghost.hasKeystream) {
        memcpy(activeKeystream, g_ghost.ghostKeystream, 51);
    } else {
        const auto& freshKs = getOrRefreshFreshKeystream();
        if (freshKs.empty() || freshKs.size() < 51) {
        }
        memcpy(activeKeystream, freshKs.data(), 51);
    }

    float cueX, cueY;
    if (g_ghost.bihActive && g_ghost.hasSavedCueBallPos) {
        cueX = g_ghost.savedCueBallX; cueY = g_ghost.savedCueBallY;
    } else if (g_ghost.hasSceneCueBallPos) {
        cueX = g_ghost.sceneCueBallX; cueY = g_ghost.sceneCueBallY;
    } else if (g_ghost.hasSavedCueBallPos) {
        cueX = g_ghost.savedCueBallX; cueY = g_ghost.savedCueBallY;
    }
    uint8_t newEnc[51];
    uint8_t newPlain[28];
    float ghostForce = g_ghost.powerBarActive
        ? fmaxf(g_ghost.currentAimForce, 0.05f)
        : fmaxf(g_ghost.sliderForceValue, 0.05f);
    buildKnownPlaintext(newPlain, g_ghost.currentAimDirX, g_ghost.currentAimDirY,
                        ghostForce,
                        g_ghost.currentAimSpinX, g_ghost.currentAimSpinY);
    for (int i = 0; i < 28; i++)
        newEnc[i] = activeKeystream[i] ^ newPlain[i];

    {
        uint8_t modPlain[16];
        float cuePower = 0.f, cueSpin = 0.f, cueMasse = 30.f;
        if (g_ghost.lastPoolGameController && g_GetCuePower && g_GetCueSpin && g_GetMaxMasseAngle) {
            cuePower = g_GetCuePower(g_ghost.lastPoolGameController, nullptr);
            cueSpin  = g_GetCueSpin(g_ghost.lastPoolGameController, nullptr);
            cueMasse = g_GetMaxMasseAngle(g_ghost.lastPoolGameController, nullptr);
            if (!isfinite(cuePower)) cuePower = 0.f;
            if (!isfinite(cueSpin))  cueSpin = 0.f;
            if (!isfinite(cueMasse)) cueMasse = 30.f;
        }
        modPlain[0] = 0x93;
        modPlain[1] = 0xCA; writeFloatBE(modPlain + 2, cuePower);
        modPlain[6] = 0xCA; writeFloatBE(modPlain + 7, cueSpin);
        modPlain[11] = 0xCA; writeFloatBE(modPlain + 12, cueMasse);
        for (int i = 0; i < 16; i++)
            newEnc[28 + i] = activeKeystream[28 + i] ^ modPlain[i];
    }
    uint8_t newTail[7];
    buildTailPlaintext(newTail, false, false, 0.0f);
    for (int i = 0; i < 7; i++)
        newEnc[44 + i] = activeKeystream[44 + i] ^ newTail[i];

    uint8_t newSection[128];
    int nsLen = buildShotInfoSection(newSection, newEnc);

    uint8_t* finalBlob = nullptr;
    int finalLen = 0;
    static uint8_t workBuf[131072];
    static uint8_t workBuf2[131072];

    if (useCaptured) {
        int siOff = g_ghost.shotInfoOffset;
        int siEnd = g_ghost.shotInfoSectionEnd;
        int capLen = g_ghost.capturedLen;
        int wp = 0;
        memcpy(workBuf + wp, g_ghost.capturedData, siOff); wp += siOff;
        memcpy(workBuf + wp, newSection, nsLen); wp += nsLen;
        memcpy(workBuf + wp, g_ghost.capturedData + siEnd, capLen - siEnd);
        wp += (capLen - siEnd);
        finalBlob = workBuf;
        finalLen = wp;

        if (!g_ghost.freshEncPropsString.empty() && !g_ghost.freshSignatureString.empty()) {
            auto encStr16 = buildStr16Bytes(g_ghost.freshEncPropsString);
            auto sigStr16 = buildStr16Bytes(g_ghost.freshSignatureString);

            struct StrInfo { int offset; int headerLen; int strLen; };
            auto findStrs = [](uint8_t* blob, int len) -> std::pair<StrInfo,StrInfo> {
                StrInfo found[8]; int count = 0;
                for (int i = 0; i < len - 2 && count < 8; i++) {
                    uint8_t b = blob[i]; int hdr = 0, slen = 0;
                    if (b == 0xDA && i+2 < len) { slen = ((int)blob[i+1]<<8)|blob[i+2]; hdr = 3; }
                    else if (b == 0xD9 && i+1 < len) { slen = blob[i+1]; hdr = 2; }
                    else continue;
                    if (slen < 50 || i + hdr + slen > len) continue;
                    bool ok = true;
                    for (int j = 0; j < 10 && j < slen; j++) {
                        uint8_t c = blob[i + hdr + j];
                        if (c < 0x20 || c > 0x7E) { ok = false; break; }
                    }
                    if (!ok) continue;
                    found[count++] = {i, hdr, slen};
                    i += hdr + slen - 1;
                }
                for (int a = 0; a < count-1; a++)
                    for (int b2 = a+1; b2 < count; b2++)
                        if (found[a].strLen < found[b2].strLen) { auto t=found[a]; found[a]=found[b2]; found[b2]=t; }
                StrInfo none = {-1,0,0};
                return { count>0?found[0]:none, count>1?found[1]:none };
            };

            auto [enc, sig] = findStrs(finalBlob, finalLen);
            if (enc.offset >= 0 && sig.offset >= 0) {
                StrInfo* first = enc.offset < sig.offset ? &enc : &sig;
                StrInfo* second = enc.offset < sig.offset ? &sig : &enc;
                const auto* fStr = (first==&enc) ? &encStr16 : &sigStr16;
                const auto* sStr = (second==&enc) ? &encStr16 : &sigStr16;
                int p = 0;
                memcpy(workBuf2+p, finalBlob, first->offset); p += first->offset;
                memcpy(workBuf2+p, fStr->data(), fStr->size()); p += (int)fStr->size();
                int gap = second->offset - (first->offset + first->headerLen + first->strLen);
                if (gap > 0) { memcpy(workBuf2+p, finalBlob + first->offset + first->headerLen + first->strLen, gap); p += gap; }
                memcpy(workBuf2+p, sStr->data(), sStr->size()); p += (int)sStr->size();
                int after = second->offset + second->headerLen + second->strLen;
                if (after < finalLen) { memcpy(workBuf2+p, finalBlob + after, finalLen - after); p += (finalLen - after); }
                finalBlob = workBuf2;
                finalLen = p;
            }
        }
    } else {
        uint8_t prefix[7];
        buildPrefix(prefix, g_ghost.gameId, 0);
        auto encStr16 = buildStr16Bytes(g_ghost.freshEncPropsString);
        auto sigStr16 = buildStr16Bytes(g_ghost.freshSignatureString);
        uint8_t cueBall[34];
        cueBall[0] = 0x93;
        cueBall[1] = 0x92;
        cueBall[2] = 0xCA; writeFloatBE(cueBall+3, cueX);
        cueBall[7] = 0xCA; writeFloatBE(cueBall+8, cueY);
        cueBall[12] = 0x94;
        cueBall[13] = 0xCA; cueBall[14]=0;cueBall[15]=0;cueBall[16]=0;cueBall[17]=0;
        cueBall[18] = 0xCA; cueBall[19]=0;cueBall[20]=0;cueBall[21]=0;cueBall[22]=0;
        cueBall[23] = 0xCA; cueBall[24]=0;cueBall[25]=0;cueBall[26]=0;cueBall[27]=0;
        cueBall[28] = 0xCA; cueBall[29]=0x3F;cueBall[30]=0x80;cueBall[31]=0;cueBall[32]=0;
        cueBall[33] = 0x00;
        int wp = 0;
        memcpy(workBuf + wp, prefix, 7); wp += 7;
        memcpy(workBuf + wp, newSection, nsLen); wp += nsLen;
        memcpy(workBuf + wp, encStr16.data(), encStr16.size()); wp += (int)encStr16.size();
        memcpy(workBuf + wp, cueBall, 34); wp += 34;
        memcpy(workBuf + wp, sigStr16.data(), sigStr16.size()); wp += (int)sigStr16.size();
        finalBlob = workBuf;
        finalLen = wp;
    }

    if (useCaptured) {
        for (int i = 0; i < finalLen - 12; i++) {
            if (finalBlob[i] == 0x93 && finalBlob[i+1] == 0x92 &&
                finalBlob[i+2] == 0xCA && finalBlob[i+12] == 0x94) {
                writeFloatBE(finalBlob + i + 3, cueX);
                writeFloatBE(finalBlob + i + 8, cueY);
                break;
            }
        }
    }

    static void* ghostPinnedArr = nullptr;
    static int   ghostPinnedCap = 0;
    int needed = finalLen + 16;
    if ((!ghostPinnedArr || ghostPinnedCap < needed) && g_il2cpp_array_new && g_ghost.capturedKlass) {
        int newCap = needed > 0x20000 ? needed : 0x20000;
        ghostPinnedArr = g_il2cpp_array_new(g_ghost.capturedKlass, newCap);
        if (ghostPinnedArr && g_il2cpp_gchandle_new) g_il2cpp_gchandle_new(ghostPinnedArr, 1);
        ghostPinnedCap = ghostPinnedArr ? newCap : 0;
    }
    if (!ghostPinnedArr) return;
    *(int*)((uint8_t*)ghostPinnedArr + 0x18) = finalLen;
    memcpy((uint8_t*)ghostPinnedArr + 0x20, finalBlob, finalLen);
    ghostCallActive = true;
    void* response = orig_RustBridgeSim(ghostPinnedArr);
    ghostCallActive = false;

    if (!response) { return; }
    int respLen = *(int*)((uint8_t*)response + 0x18);
    if (respLen < 100 || respLen > 1000000) { return; }

    if (g_ghost.trajectoryHidden) {
        uint8_t* rd = (uint8_t*)response + 0x20;
        if (respLen > 40 && rd[0] == 0x98 && rd[1] == 0xDC) {
            int nb = (rd[2] << 8) | rd[3];
            if (nb >= 1 && nb <= 30) {
                int off = 4;
                uint8_t m = rd[off];
                int nf = 0;
                if ((m & 0xF0) == 0x90) { nf = m & 0x0F; off++; }
                else if (m == 0xDC && off+2 < respLen) { nf = (rd[off+1]<<8)|rd[off+2]; off += 3; }
                if (nf > 0 && off + nf * 36 <= respLen) {
                    int lastOff = off + (nf - 1) * 36;
                    if (rd[lastOff] == 0x97 && rd[lastOff+1] == 0xCA && rd[lastOff+6] == 0xCA) {
                        float cx = readFloatBE(rd + lastOff + 2);
                        float cy = readFloatBE(rd + lastOff + 7);
                        if (isfinite(cx) && isfinite(cy) && fabsf(cx) < 2.0f && fabsf(cy) < 1.5f) {
                            g_ghost.savedCueBallX = cx;
                            g_ghost.savedCueBallY = cy;
                            g_ghost.hasSavedCueBallPos = true;
                        }
                    }
                }
            }
        }
        return;
    }

    if (!g_w2sReady) initW2S();
    if (g_w2sReady) refreshCameraMatrices();

    if (!g_w2sReady) { return; }

    extractTrajectoryFromMsgPackResponse(response);
}

void triggerGhostSimulation() {
    // Game access is enforced in the simulation core as well as the UI, so a
    // stale flag or patched button cannot generate Pool trajectories.
    if (!license_security_allows_game(GOAT_GAME_POOL)) return;
    if (ghostCallActive) return;
    if (g_ghost.localShotPending || g_ghost.animating) return;
    uint64_t now = nowMs();
    if (now - g_ghost.lastGhostTimeMs < 16) return;
    g_ghost.lastGhostTimeMs = now;

    bool changed = g_ghost.currentAimDirX  != g_ghost.lastAimDirX  ||
                   g_ghost.currentAimDirY  != g_ghost.lastAimDirY  ||
                   g_ghost.currentAimForce != g_ghost.lastAimForce ||
                   g_ghost.currentAimSpinX != g_ghost.lastAimSpinX ||
                   g_ghost.currentAimSpinY != g_ghost.lastAimSpinY ||
                   g_ghost.sliderForceValue != g_ghost.lastSliderForce ||
                   g_ghost.powerBarActive != g_ghost.wasPowerBarActive;
    if (!changed) return;

    g_ghost.lastAimDirX      = g_ghost.currentAimDirX;
    g_ghost.lastAimDirY      = g_ghost.currentAimDirY;
    g_ghost.lastAimForce     = g_ghost.currentAimForce;
    g_ghost.lastAimSpinX     = g_ghost.currentAimSpinX;
    g_ghost.lastAimSpinY     = g_ghost.currentAimSpinY;
    g_ghost.lastSliderForce  = g_ghost.sliderForceValue;
    g_ghost.wasPowerBarActive = g_ghost.powerBarActive;

    performGhostSimulation();
}

static bool parseBall1Proper(const uint8_t* d, int len, float& outX, float& outY) {
    if (len < 10) return false;
    int off = 0;
    if (d[off++] != 0x98) return false;
    if (d[off++] != 0xDC) return false;
    if (off + 2 > len) return false;
    int numBalls = (d[off] << 8) | d[off+1]; off += 2;
    if (numBalls < 2 || numBalls > 30) return false;

    for (int b = 0; b < numBalls && b <= 1; b++) {
        if (off >= len) return false;
        uint8_t marker = d[off];
        int numFrames = 0;
        if (marker >= 0x90 && marker <= 0x9F) { numFrames = marker & 0x0F; off++; }
        else if (marker == 0xDC) { off++; if(off+2>len) return false; numFrames=(d[off]<<8)|d[off+1]; off+=2; }
        else if (marker == 0xDD) { off++; if(off+4>len) return false; numFrames=(d[off]<<24)|(d[off+1]<<16)|(d[off+2]<<8)|d[off+3]; off+=4; }
        else return false;
        if (numFrames < 0 || numFrames > 10000) return false;
        if (numFrames == 0) continue;

        float lastX = 0, lastY = 0;
        bool hasLast = false;
        for (int f = 0; f < numFrames; f++) {
            if (off + 36 > len) return false;
            if (d[off] != 0x97) return false; off++;
            if (d[off] != 0xCA) return false;
            lastX = readFloatBE(d + off + 1); off += 5;
            if (d[off] != 0xCA) return false;
            lastY = readFloatBE(d + off + 1); off += 5;
            off += 25;
            hasLast = true;
        }

        if (b == 1 && hasLast) {
            outX = lastX; outY = lastY;
            return isfinite(lastX) && isfinite(lastY);
        }
    }
    return false;
}
