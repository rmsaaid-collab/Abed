#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <vector>
#include <mutex>
#include "pool_types.h"

struct MsgPackReader {
    uint8_t* data; int len; int pos;
    MsgPackReader(uint8_t* d, int l) : data(d), len(l), pos(0) {}
    uint8_t  readU8()  { return pos < len ? data[pos++] : 0; }
    uint16_t readU16BE() {
        if (pos+1>=len) return 0;
        uint16_t v=((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2; return v; }
    uint32_t readU32BE() {
        if (pos+3>=len) return 0;
        uint32_t v=((uint32_t)data[pos]<<24)|((uint32_t)data[pos+1]<<16)
                  |((uint32_t)data[pos+2]<<8)|(uint32_t)data[pos+3]; pos+=4; return v; }
    float readFloat32() { uint32_t u=readU32BE(); float f; __builtin_memcpy(&f,&u,4); return f; }
    int readArrayLen() {
        uint8_t b=readU8();
        if ((b&0xF0)==0x90) return b&0x0F;
        if (b==0xDC) return readU16BE();
        if (b==0xDD) return (int)readU32BE();
        return -1; }
    float readNumericAsFloat() {
        if (pos>=len) return 0.f;
        uint8_t b=data[pos];
        if (b==0xCA){pos++;return readFloat32();}
        if (b==0xCB){pos++;uint32_t hi=readU32BE();uint32_t lo=readU32BE();
            uint64_t u=((uint64_t)hi<<32)|lo;double d;__builtin_memcpy(&d,&u,8);return(float)d;}
        if (b<0x80){pos++;return (float)b;}
        if (b==0xCC){pos++;return (float)readU8();}
        if (b==0xCD){pos++;return (float)readU16BE();}
        if (b==0xCE){pos++;return (float)readU32BE();}
        if (b==0xD0){pos++;return (float)(int8_t)readU8();}
        if (b==0xD1){pos++;return (float)(int16_t)readU16BE();}
        if (b==0xD2){pos++;return (float)(int32_t)readU32BE();}
        if (b>=0xE0){pos++;return (float)(int8_t)b;}
        pos++;return 0.f; }
    void skipElement() {
        if (pos>=len) return;
        uint8_t b=readU8();
        if (b<=0x7F) return;
        if (b>=0xE0) return;
        if ((b&0xE0)==0xA0){pos+=b&0x1F;return;}
        if ((b&0xF0)==0x80){int n=(b&0xF)*2;for(int i=0;i<n;i++)skipElement();return;}
        if ((b&0xF0)==0x90){int n=b&0xF;for(int i=0;i<n;i++)skipElement();return;}
        switch(b){
            case 0xC0:case 0xC2:case 0xC3:return;
            case 0xCA:pos+=4;return; case 0xCB:pos+=8;return;
            case 0xCC:case 0xD0:pos+=1;return;
            case 0xCD:case 0xD1:pos+=2;return;
            case 0xCE:case 0xD2:pos+=4;return;
            case 0xCF:case 0xD3:pos+=8;return;
            case 0xC4:case 0xD9:pos+=readU8();return;
            case 0xC5:case 0xDA:pos+=readU16BE();return;
            case 0xC6:case 0xDB:pos+=readU32BE();return;
            case 0xDC:{int n=readU16BE();for(int i=0;i<n;i++)skipElement();return;}
            case 0xDD:{int n=(int)readU32BE();for(int i=0;i<n;i++)skipElement();return;}
            default:return;
        }
    }
};

struct GhostState {
    uint8_t  capturedData[65536]     = {};
    int      capturedLen             = 0;
    void*    capturedKlass           = nullptr;
    int      shotInfoOffset          = -1;
    int      shotInfoSectionEnd      = -1;
    bool     hasRustBridgeData       = false;

    uint8_t  ghostKeystream[51]      = {};
    uint8_t  ghostOrigEncBytes[51]   = {};
    bool     hasKeystream            = false;

    float    dir[2]                  = {};
    float    force                   = 0.f;
    float    spin[2]                 = {};
    bool     randOff                 = false;
    bool     brkPow                  = false;
    float    incline                 = 0.f;
    bool     hasShotInfo             = false;

    float    sceneCueBallX           = 0.f;
    float    sceneCueBallY           = 0.f;
    bool     hasSceneCueBallPos      = false;
    float    savedCueBallX           = 0.f;
    float    savedCueBallY           = 0.f;
    bool     hasSavedCueBallPos      = false;
    bool     bihActive               = false;

    float    trickTargetX            = 0.f;
    float    trickTargetY            = 0.f;
    bool     hasTrickTarget          = false;

    uint32_t gameId                  = 0;

    std::string freshEncPropsString;
    std::string freshSignatureString;

    std::string ksCacheEncPropsHash;
    std::vector<uint8_t> ksCacheKeystream;

    uint8_t  capturedModifiersBytes[16] = {};
    bool     hasModifiers            = false;

    uint8_t  ghostArrBuf[131072]     = {};

    float    currentAimDirX          = 0.f;
    float    currentAimDirY          = 1.f;
    float    currentAimForce         = 0.5f;
    float    currentAimSpinX         = 0.f;
    float    currentAimSpinY         = 0.f;

    float    lastAimDirX             = -999.f;
    float    lastAimDirY             = -999.f;
    float    lastAimForce            = -999.f;
    float    lastAimSpinX            = -999.f;
    float    lastAimSpinY            = -999.f;
    float    lastSliderForce         = -999.f;

    void*    lastPoolGameController  = nullptr;

    uint64_t lastGhostTimeMs         = 0;
    uint64_t lastShotTimeMs          = 0;

    bool     localShotPending        = false;
    bool     localShotJustFired      = false;
    bool     animating               = false;

    bool     realForceEnabled        = false;
    float    sliderForceValue        = 0.5f;
    bool     angleOverride           = false;
    bool     panelVisible            = false;
    bool     trajectoryHidden        = false;
    bool     powerBarActive          = false;
    bool     wasPowerBarActive       = false;
    float    tableWorldY             = 0.87f;

    uint32_t capturedGameId          = 0;

    uint8_t  lastModPlain[16]       = {};
    bool     hasLastModPlain        = false;

    enum BotState {
        BOT_OFF,
        BOT_WAIT_TURN,
        BOT_PLACE_BALL,
        BOT_SCANNING,
        BOT_AIMING,
        BOT_NOMINATING,
        BOT_SHOOTING,
        BOT_ANIMATING,
        BOT_GAME_OVER,
    };

    bool     botEnabled              = false;
    BotState botState                = BOT_OFF;
    uint64_t botStateStartMs         = 0;
    uint64_t botActionDelayMs        = 0;

    bool     isMyTurn                = false;
    bool     turnJustChanged         = false;

    int      botAimFrames            = 0;
    bool     botShotFired            = false;
    int      botBIHAttempt           = 0;
    int      botScanAttempt          = 0;
    bool     botIsBreak              = false;
    bool     botTurnHadBIH           = false;
    bool     bihDroppedThisTurn      = false;
    void*    lastBIHController       = nullptr;
    uint64_t bihActivatedMs          = 0;
    uint64_t turnStartMs             = 0;
    bool     hideTrajectory          = false;
    bool     botHumanMode            = false;
    float    botVisiblePower         = 0.f;
    uint64_t botVisiblePowerMs       = 0;

    float    ballPos[16][2];
    bool     ballOnTable[16];
    int      ballCount             = 0;
    bool     hasBallPositions      = false;

    
    std::vector<std::pair<float,float>> allBallWaypoints[16];
    float    allBallWaypointsInit[16][2] = {};

    
    std::vector<std::pair<float,float>> ball8Waypoints;
    bool     hasBall8Waypoints     = false;
    float    ball8InitX            = 0.f;
    float    ball8InitZ            = 0.f;

    
    std::vector<std::pair<float,float>> cueWaypoints;
    bool     hasCueWaypoints       = false;

    float    botBestAngle            = 0.f;
    float    botBestForce            = 0.5f;
    float    botBestDist             = 1e10f;
    int      botBestPocket           = -1;
    int      botBestPocketIdx        = -1;
    bool     botFoundShot            = false;
    uint64_t botLastShotMs           = 0;

    void*    pinnedArray             = nullptr;
    int      pinnedArrayCapacity     = 0;
};

extern GhostState g_ghost;
extern bool ghostCallActive;

extern GameStore_IsMyTurn_t g_IsMyTurn;
extern OnTurnChanged_t      orig_OnTurnChanged;
extern GameOver_t           orig_GameOver;

typedef void* (*il2cpp_array_new_t)(void* klass, uint32_t length);
typedef uint32_t (*il2cpp_gchandle_new_t)(void* obj, int pinned);
extern il2cpp_array_new_t    g_il2cpp_array_new;
extern il2cpp_gchandle_new_t g_il2cpp_gchandle_new;

extern std::mutex          trajectoryMutex;
extern std::vector<float>  cachedTrajectoryScreenCoords;
extern bool                hasNewTrajectory;
extern std::vector<int>    cachedWillPocket;
extern std::vector<int>    cachedPocketIndices;
extern int                 cachedFirstHitBall;
extern float               cachedCueFinalX;
extern float               cachedCueFinalZ;
extern float               cachedBall8FinalX;
extern float               cachedBall8FinalZ;
extern bool                cachedBall8Moved;
extern float               cachedAllBallFinals[16][2];
extern bool                cachedAllBallMoved[16];

extern RustBridgeSim_t orig_RustBridgeSim;

extern uintptr_t g_il2cppBase;

extern float g_vpMatrix[16];
extern int   g_screenWidth;
extern int   g_screenHeight;
extern bool  g_w2sReady;

extern GetMainCamera_t          g_Camera_get_main;
extern Screen_getDim_t          g_Screen_get_width;
extern Screen_getDim_t          g_Screen_get_height;
extern GetMatrixInjected_t      g_get_worldToCameraMatrix;
extern GetMatrixInjected_t      g_get_projectionMatrix;

typedef float (*GetCueFloat_t)(void* instance, void* method);
extern GetCueFloat_t g_GetCuePower;
extern GetCueFloat_t g_GetCueSpin;
extern GetCueFloat_t g_GetMaxMasseAngle;

void writeFloatBE(uint8_t* out, float f);
int  buildKnownPlaintext(uint8_t* out, float dir0, float dir1, float force,
                         float spin0, float spin1);
void buildTailPlaintext(uint8_t* out, bool randOff, bool brkPow, float incline);
void deriveGhostKeystream();
int  findShotInfoInBuffer(uint8_t* data, int len);
void extractTrajectoryFromMsgPackResponse(void* responseByteArray);
void performGhostSimulation();
void triggerGhostSimulation();
bool refreshCameraMatrices();
void initW2S();

bool worldToScreen(float wx, float wz, float& sx, float& sy);

uint64_t nowMs();
std::string readIl2CppString(void* strObj);
const std::vector<uint8_t>& getOrRefreshFreshKeystream();
