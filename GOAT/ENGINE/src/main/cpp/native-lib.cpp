#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <elf.h>
#include <thread>
#include <math.h>
#include <algorithm>
#include <atomic>

#include "include/il2cpp_types.h"
#include "include/pool_types.h"
#include "include/ghost_sim.h"
#include "include/solitaire_auto.h"
#include "include/uno_auto.h"
#include "include/license_security.h"
#include "include/crypto.h"
#include "include/il2cpp_exports.h"
#include "dobby.h"


void overlay_init(JavaVM* jvm);
void overlay_setView(JNIEnv* env, jobject view);
void overlay_requestRedraw();
void overlay_updateCoords(JNIEnv* env, const std::vector<float>& coords, const std::vector<int>& pocketed);
extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnDraw(JNIEnv*, jclass, jobject, jobject, jfloatArray);
extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnClick(JNIEnv*, jclass, jint);
extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnForceChanged(JNIEnv*, jclass, jint);
extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeCreatePanel(JNIEnv*, jclass, jobject, jobject);
extern "C" JNIEXPORT jboolean JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnTouch(JNIEnv*, jclass, jint, jfloat, jfloat, jint);
extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeSetPerformanceProfile(
    JNIEnv*, jclass, jint, jint, jint, jint, jint, jint);

static JavaVM* g_jvm = nullptr;
static std::atomic<uintptr_t> g_installedIl2CppBase{0};

#define GOAT_NATIVE_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "GOAT_NATIVE", __VA_ARGS__)

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeSetPerformanceProfile(
        JNIEnv*, jclass, jint profile, jint sliceNodes, jint sliceMs,
        jint arenaNodes, jint cardSafetyMs, jint talonSafetyMs) {
    solitaire_set_performance_profile((int)profile, (int)sliceNodes, (int)sliceMs,
                                      (int)arenaNodes, (int)cardSafetyMs,
                                      (int)talonSafetyMs);
}

static void disableUnlicensedPoolRuntime() {
    g_ghost.botEnabled = false;
    g_ghost.botHumanMode = false;
    g_ghost.realForceEnabled = false;
    g_ghost.botState = GhostState::BOT_OFF;
    g_ghost.botShotFired = false;
    g_ghost.botFoundShot = false;
    g_ghost.angleOverride = false;
    g_ghost.localShotPending = false;
    g_ghost.powerBarActive = false;
    g_ghost.hideTrajectory = true;
    g_ghost.trajectoryHidden = true;
}

static void pushToOverlay() {
    if (!license_security_allows_game(GOAT_GAME_POOL)) return;
    if (!g_jvm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (env) {
        std::vector<float> copy;
        std::vector<int> pock;
        {
            std::lock_guard<std::mutex> lk(trajectoryMutex);
            if (hasNewTrajectory) {
                copy = cachedTrajectoryScreenCoords;
                pock = cachedWillPocket;
                hasNewTrajectory = false;
            }
        }
        if (!copy.empty()) {
            overlay_updateCoords(env, copy, pock);
            overlay_requestRedraw();
        }
    }
    if (attached) g_jvm->DetachCurrentThread();
}


// Resolve the linker-owned ELF load bias, never the first similarly named
// mapping from /proc/self/maps.  On recent Android builds libil2cpp may have a
// read-only shadow/file mapping before its real executable image.  Treating
// that mapping start as the RVA base made every Dobby target wrong: depending
// on startup timing the first launch either crashed or silently installed no
// hooks, while a later launch happened to work.
//
// dlsym only succeeds after the dynamic linker has made libil2cpp usable, and
// dladdr then returns the exact dli_fbase used for all IL2CPP RVAs.  Returning
// zero while either step is incomplete lets hookingThread retry safely instead
// of racing the loader.
static bool is_readable_elf_base(uintptr_t base) {
    if (!base) return false;
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    return header->e_ident[EI_MAG0] == ELFMAG0 &&
           header->e_ident[EI_MAG1] == ELFMAG1 &&
           header->e_ident[EI_MAG2] == ELFMAG2 &&
           header->e_ident[EI_MAG3] == ELFMAG3 &&
           header->e_ident[EI_CLASS] == ELFCLASS64 &&
           header->e_type == ET_DYN;
}

static uintptr_t get_module_base_from_maps(const char* name) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return 0;

    uintptr_t offsetZeroBase = 0;
    bool hasExecutableMapping = false;
    char line[4096];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long fileOffset = 0;
        unsigned long inode = 0;
        char permissions[5] = {};
        char device[32] = {};
        char path[3072] = {};
        const int fields = sscanf(line, "%lx-%lx %4s %lx %31s %lu %3071[^\n]",
                                  &start, &end, permissions, &fileOffset,
                                  device, &inode, path);
        if (fields < 7 || !strstr(path, name)) continue;

        if (permissions[2] == 'x') hasExecutableMapping = true;
        if (fileOffset == 0 && permissions[0] == 'r') {
            offsetZeroBase = static_cast<uintptr_t>(start);
        }
    }
    fclose(maps);

    if (!hasExecutableMapping || !is_readable_elf_base(offsetZeroBase)) return 0;
    return offsetZeroBase;
}

static uintptr_t get_module_base(const char* name) {
    if (!name || !*name) return 0;

    void* handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        void* exported = dlsym(handle, "il2cpp_init");
        Dl_info info{};
        const bool resolved = exported && dladdr(exported, &info) != 0 && info.dli_fbase;
        const uintptr_t base = resolved ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
        dlclose(handle);
        if (base) return base;
    }

    // The GOAT bridge is intentionally isolated from the game's dependency
    // graph.  On Android 13 that creates a second linker namespace, so the
    // library can be mapped and executable while RTLD_NOLOAD remains blind to
    // it.  /proc/self/maps is process-wide; select only the offset-zero ELF
    // mapping and require an executable sibling mapping before accepting it.
    const uintptr_t base = get_module_base_from_maps(name);
    static std::atomic<uintptr_t> loggedFallbackBase{0};
    if (base) {
        uintptr_t expected = 0;
        if (loggedFallbackBase.compare_exchange_strong(expected, base)) {
            GOAT_NATIVE_LOG("[IL2CPP_BASE_FALLBACK] namespace-isolated base=%p",
                            reinterpret_cast<void*>(base));
        }
    }
    return base;
}


typedef uint64_t (*GameStoreGetGID_t)(void* mi);
static GameStoreGetGID_t orig_GameStoreGetGID = nullptr;

static uint64_t my_GameStoreGetGID(void* mi) {
    uint64_t x0 = orig_GameStoreGetGID(mi);
    uint64_t x1_val = 0;
#if defined(__aarch64__)
    __asm__ volatile("mov %0, x1" : "=r"(x1_val));
#endif
    uint32_t newGid = 0;
    if (x0 == 1 && x1_val > 100)
        newGid = (uint32_t)x1_val;
    else if (x0 > 100)
        newGid = (uint32_t)x0;

    if (newGid > 0 && newGid != g_ghost.gameId) {
        g_ghost.gameId = newGid;
        g_ghost.ksCacheEncPropsHash.clear();
        g_ghost.ksCacheKeystream.clear();
        g_ghost.hasKeystream = false; 
        g_ghost.animating = false;   
        g_ghost.localShotPending = false;
        g_ghost.powerBarActive = false;
        
        g_ghost.lastAimDirX = -999.f;
        g_ghost.lastAimDirY = -999.f;
        g_ghost.lastAimForce = -999.f;
        g_ghost.lastSliderForce = -999.f;
        g_ghost.wasPowerBarActive = false;
        
        g_ghost.freshEncPropsString.clear();
        g_ghost.freshSignatureString.clear();
        
        g_ghost.hasSceneCueBallPos = false;
        g_ghost.hasSavedCueBallPos = false;
        g_ghost.bihActive = false;
        g_ghost.angleOverride = false;
        g_ghost.currentAimSpinX = 0.f; 
        g_ghost.currentAimSpinY = 0.f;
        
        g_ghost.botState = g_ghost.botEnabled ? GhostState::BOT_OFF : GhostState::BOT_OFF;
        g_ghost.botShotFired = false;
        g_ghost.botFoundShot = false;
        g_ghost.isMyTurn = false;
        g_ghost.turnJustChanged = false;
    }
    return x0;
}


typedef void (*GameStoreWrite_t)(void* key, void* value, uint8_t p3, uint8_t p4, void* parent, void* method);
static GameStoreWrite_t orig_GameStoreWrite = nullptr;

static GameStore_IsMyTurn_t orig_GameStoreIsMyTurn = nullptr;

static bool my_GameStoreIsMyTurn(void* method) {
    // This hook is intentionally transparent. IsMyTurn is shared by every
    // Jawaker game and is frequently called halfway through UI state mutation;
    // using it as a scheduler let one game's solver parse another game's state.
    // Solitaire and UNO now each own verified UnityMain heartbeat hooks.
    return orig_GameStoreIsMyTurn ? orig_GameStoreIsMyTurn(method) : false;
}

static void my_GameStoreWrite(void* key, void* value, uint8_t p3, uint8_t p4, void* parent, void* method) {
    if (orig_GameStoreWrite) orig_GameStoreWrite(key, value, p3, p4, parent, method);
    if (!key || !value) return;
    std::string keyStr = readIl2CppString(key);
    if (keyStr != "encrypted_props" && keyStr != "signature") return;
    void* strPtr = *(void**)((uint8_t*)value + 0x38);
    std::string val = readIl2CppString(strPtr);
    if (val.empty()) return;
    if (keyStr == "encrypted_props") {
        bool changed = (g_ghost.freshEncPropsString != val);
        g_ghost.freshEncPropsString = val;
        if (changed) {
            if (g_ghost.gameId > 1000000) {
                const auto& freshKs = getOrRefreshFreshKeystream();
                
                if (freshKs.size() >= 51) {
                    memcpy(g_ghost.ghostKeystream, freshKs.data(), 51);
                    g_ghost.hasKeystream = true;
                }
            }
        }
    } else {
        g_ghost.freshSignatureString = val;
    }
}


static void* my_RustBridgeSim(void* arr) {
    if (arr && !g_ghost.capturedKlass)
        g_ghost.capturedKlass = *(void**)arr;

    bool hasShotInfo = g_ghost.hasShotInfo;
    if (arr && !ghostCallActive) {
        int len = *(int*)((uint8_t*)arr + 0x18);
        if (len > 1000 && len < 50000) {
            memcpy(g_ghost.capturedData, (uint8_t*)arr + 0x20, len);
            g_ghost.capturedLen = len;
            g_ghost.shotInfoOffset = findShotInfoInBuffer(g_ghost.capturedData, len);
            if (g_ghost.shotInfoOffset >= 0) {
                int p = g_ghost.shotInfoOffset + 3;
                for (int j = 0; j < 51 && p < len; j++) {
                    if (g_ghost.capturedData[p] < 0x80) p += 1;
                    else if (g_ghost.capturedData[p] == 0xCC && p+1 < len) p += 2;
                    else { p = -1; break; }
                }
                g_ghost.shotInfoSectionEnd = p;
            }
            g_ghost.hasRustBridgeData = true;
            g_ghost.capturedGameId = g_ghost.gameId;
            if (hasShotInfo && g_ghost.shotInfoOffset >= 0)
                deriveGhostKeystream();
        }
    }

    void* result = orig_RustBridgeSim(arr);

    if (result && hasShotInfo && g_ghost.localShotPending) {
        g_ghost.localShotPending = false;
        g_ghost.animating = true; 
        g_ghost.lastShotTimeMs = nowMs();
    }

    return result;
}


typedef void (*SLSI_t)(void* shotInfo, void* method);
static SLSI_t orig_SLSI = nullptr;

static void my_SLSI(void* shotInfo, void* method) {
    
    
    if (shotInfo) {
        *(uint8_t*)((uint8_t*)shotInfo + 0x30) = 0;
        *(uint8_t*)((uint8_t*)shotInfo + 0x31) = 0;
    }
    if (orig_SLSI) orig_SLSI(shotInfo, method);
    if (!shotInfo) return;

    void* dirArr = *(void**)((uint8_t*)shotInfo + 0x10);
    if (dirArr) {
        g_ghost.dir[0] = *(float*)((uint8_t*)dirArr + 0x20);
        g_ghost.dir[1] = *(float*)((uint8_t*)dirArr + 0x24);
    }
    g_ghost.force = *(float*)((uint8_t*)shotInfo + 0x18);
    void* spinArr = *(void**)((uint8_t*)shotInfo + 0x20);
    if (spinArr) {
        g_ghost.spin[0] = *(float*)((uint8_t*)spinArr + 0x20);
        g_ghost.spin[1] = *(float*)((uint8_t*)spinArr + 0x24);
    }
    g_ghost.randOff = false;
    g_ghost.brkPow = false;
    g_ghost.incline = *(float*)((uint8_t*)shotInfo + 0x34);
    g_ghost.hasShotInfo = true;
    g_ghost.localShotPending = true;

    
    g_ghost.currentAimDirX  = g_ghost.dir[0];
    g_ghost.currentAimDirY  = g_ghost.dir[1];
    g_ghost.currentAimForce = g_ghost.force;
    g_ghost.currentAimSpinX = g_ghost.spin[0];
    g_ghost.currentAimSpinY = g_ghost.spin[1];

}


typedef void (*RotateCue_t)(void* instance, float angle, void* method);
static RotateCue_t orig_RotateCue = nullptr;

static void my_RotateCue(void* instance, float angle, void* method) {
    if (orig_RotateCue) orig_RotateCue(instance, angle, method);
    if (!instance) return;
    g_ghost.lastPoolGameController = instance;

    
    
    void* calcRC = *(void**)((uint8_t*)instance + 0x48);
    if (calcRC) {
        float dirX = *(float*)((uint8_t*)calcRC + 0x30);
        float dirZ = *(float*)((uint8_t*)calcRC + 0x38);
        float mag = sqrtf(dirX*dirX + dirZ*dirZ);
        if (mag >= 0.5f) {
            if (!g_ghost.angleOverride) {
                g_ghost.currentAimDirX = dirX;
                g_ghost.currentAimDirY = dirZ;
            }
        }
    }

    
    
    if (g_ghost.realForceEnabled) {
        void* pbc = *(void**)((uint8_t*)instance + 0x60);
        if (pbc) {
            float f = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
            *(float*)((uint8_t*)pbc + 0x58) = f;
        }
    } else {
        void* pbc = *(void**)((uint8_t*)instance + 0x60);
        if (pbc) {
            float f = *(float*)((uint8_t*)pbc + 0x58);
            if (f >= 0.03f && f <= 1.5f) g_ghost.currentAimForce = f;
        }
    }

    
    if (!(g_ghost.realForceEnabled && g_ghost.angleOverride)) {
        triggerGhostSimulation();
    }
    pushToOverlay();
}


static void convergeAimToTarget(float targetDirX, float targetDirZ) {
    void* pgc = g_ghost.lastPoolGameController;
    if (!pgc || !orig_RotateCue) return;
    void* calcRC = *(void**)((uint8_t*)pgc + 0x48);
    if (!calcRC) return;
    for (int attempt = 0; attempt < 6; attempt++) {
        float curX = *(float*)((uint8_t*)calcRC + 0x30);
        float curZ = *(float*)((uint8_t*)calcRC + 0x38);
        float curAngle = atan2f(curX, curZ);
        float tgtAngle = atan2f(targetDirX, targetDirZ);
        float delta = (tgtAngle - curAngle) * (180.f / 3.14159265f);
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        if (fabsf(delta) < 1e-5f) break;
        orig_RotateCue(pgc, delta, nullptr);
    }
    
    
}

// These helpers drive only the cue controller's visible direction.  They never
// write currentAimDir/Force, so candidate selection and ghost simulation stay
// exactly on the proven original gameplay path.
static void setVisibleCueDirection(void* cue, float dirX, float dirZ) {
    if (!cue) return;
    float length = sqrtf(dirX * dirX + dirZ * dirZ);
    if (length < 0.0001f) return;
    dirX /= length;
    dirZ /= length;
    convergeAimToTarget(dirX, dirZ);
    *(float*)((uint8_t*)cue + 0x64) = dirX;
    *(float*)((uint8_t*)cue + 0x6C) = dirZ;
}

static void nudgeVisibleCueDirection(void* cue, float dirX, float dirZ) {
    // Smooth visual steering for the search phase: move only a few degrees per
    // frame instead of snapping the cue to each visual checkpoint.
    if (!cue || !g_ghost.lastPoolGameController || !orig_RotateCue) return;
    float length = sqrtf(dirX * dirX + dirZ * dirZ);
    if (length < 0.0001f) return;
    dirX /= length;
    dirZ /= length;
    void* calcRC = *(void**)((uint8_t*)g_ghost.lastPoolGameController + 0x48);
    if (!calcRC) return;
    float curX = *(float*)((uint8_t*)calcRC + 0x30);
    float curZ = *(float*)((uint8_t*)calcRC + 0x38);
    float curAngle = atan2f(curX, curZ);
    float targetAngle = atan2f(dirX, dirZ);
    float delta = (targetAngle - curAngle) * (180.f / 3.14159265f);
    while (delta > 180.f) delta -= 360.f;
    while (delta < -180.f) delta += 360.f;
    const float maxStepDegrees = 2.35f;
    if (delta > maxStepDegrees) delta = maxStepDegrees;
    if (delta < -maxStepDegrees) delta = -maxStepDegrees;
    orig_RotateCue(g_ghost.lastPoolGameController, delta, nullptr);
    float visibleAngle = curAngle + delta * (3.14159265f / 180.f);
    *(float*)((uint8_t*)cue + 0x64) = sinf(visibleAngle);
    *(float*)((uint8_t*)cue + 0x6C) = cosf(visibleAngle);
}

static void applyHumanSearchMotion(void* cue, uint64_t now) {
    // Three deliberate looks across the table, with a brief pause at each
    // checkpoint.  The real scan continues independently in the background.
    static const float checkpoints[] = { -2.20f, -0.72f, 0.66f, 2.12f };
    float cycle = (float)((now - g_ghost.botStateStartMs) % 4800ULL) / 4800.0f;
    float scaled = cycle * 3.0f;
    int step = (int)scaled;
    if (step > 2) step = 2;
    float local = scaled - (float)step;
    float move = local <= 0.18f ? 0.0f : (local - 0.18f) / 0.82f;
    move = move * move * (3.0f - 2.0f * move);
    float theta = checkpoints[step] + (checkpoints[step + 1] - checkpoints[step]) * move;
    theta += 0.008f * sinf(cycle * 6.2831853f * 5.0f);
    nudgeVisibleCueDirection(cue, sinf(theta), cosf(theta));
}

static void applyHumanFinalAimMotion(void* cue, int frame) {
    float target = atan2f(g_ghost.currentAimDirX, g_ghost.currentAimDirY);
    float offset = 0.0f;
    if (frame <= 12) {
        // Two natural checks around the line, then converge precisely.
        offset = 0.052f * sinf((float)frame * 0.78f);
    } else if (frame <= 22) {
        offset = 0.015f * sinf((float)frame * 1.18f);
    }
    float visible = target + offset;
    setVisibleCueDirection(cue, sinf(visible), cosf(visible));
}

static int humanAimFrameLimit() {
    // A tiny visual-only timing variation prevents identical robotic cadence.
    int seed = (g_ghost.botBestPocket > 0 ? g_ghost.botBestPocket : 1) +
               (int)(fabsf(g_ghost.botBestAngle) * 10.0f);
    return 26 + (seed % 4) * 2;
}


typedef void (*ApplySpin_t)(void* instance, float spinX, float spinY, void* method);
static ApplySpin_t orig_ApplySpin = nullptr;

static void my_ApplySpin(void* instance, float spinX, float spinY, void* method) {
    if (isfinite(spinX) && isfinite(spinY)) {
        g_ghost.currentAimSpinX = spinX;
        g_ghost.currentAimSpinY = spinY;
    }
    if (orig_ApplySpin) orig_ApplySpin(instance, spinX, spinY, method);
    triggerGhostSimulation();
    pushToOverlay();
}


typedef void (*PBD_t)(void* instance, void* method);
static PBD_t orig_PBD = nullptr;

static void my_PBD(void* instance, void* method) {
    if (!instance) { if (orig_PBD) orig_PBD(instance, method); return; }
    g_ghost.lastPoolGameController = instance;

    if (g_ghost.realForceEnabled) {
        
        void* pbc = *(void**)((uint8_t*)instance + 0x60);
        if (pbc) {
            float f = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
            *(float*)((uint8_t*)pbc + 0x58) = f;
        }
        if (orig_PBD) orig_PBD(instance, method);
        
        if (pbc) {
            float f = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
            *(float*)((uint8_t*)pbc + 0x58) = f;
        }
        
        if (!g_ghost.angleOverride) {
            triggerGhostSimulation();
            pushToOverlay();
        }
    } else {
        if (orig_PBD) orig_PBD(instance, method);
        void* pbc = *(void**)((uint8_t*)instance + 0x60);
        if (pbc) {
            float f = *(float*)((uint8_t*)pbc + 0x58);
            if (f >= 0.03f && f <= 1.5f) {
                g_ghost.currentAimForce = f;
                g_ghost.powerBarActive = true;
            } else {
                g_ghost.powerBarActive = false;
            }
        }
        triggerGhostSimulation();
        pushToOverlay();
    }
}


typedef void (*BIHL_t)(void* instance, void* method);
static BIHL_t orig_BIHL = nullptr;

static void my_BIHL(void* instance, void* method) {
    if (orig_BIHL) orig_BIHL(instance, method);
    if (!instance) return;
    g_ghost.lastBIHController = instance;  
    uint8_t active = *(uint8_t*)((uint8_t*)instance + 0x74);
    if (!(active & 1)) {
        
        if (g_ghost.bihActive && g_ghost.botState != GhostState::BOT_PLACE_BALL) {
            g_ghost.bihDroppedThisTurn = true;
        }
        g_ghost.bihActive = false;
        return;
    }
    float valid = *(float*)((uint8_t*)instance + 0x7C);
    if (valid != 1.0f) return;
    float x = *(float*)((uint8_t*)instance + 0x78);
    float y = *(float*)((uint8_t*)instance + 0x80);
    if (!isfinite(x) || !isfinite(y)) return;
    if (fabsf(x) > 2.0f || fabsf(y) > 2.0f) return;
    if (!g_ghost.bihActive) {
        g_ghost.hasSceneCueBallPos = false;
        g_ghost.bihActivatedMs = nowMs();
    }
    g_ghost.bihActive = true;
    if (g_ghost.hasSavedCueBallPos &&
        fabsf(x - g_ghost.savedCueBallX) < 0.0001f &&
        fabsf(y - g_ghost.savedCueBallY) < 0.0001f) return;
    g_ghost.savedCueBallX = x; g_ghost.savedCueBallY = y;
    g_ghost.hasSavedCueBallPos = true;
    triggerGhostSimulation();
    pushToOverlay();
}


typedef void (*DBFH_t)(void* instance, void* method);
static DBFH_t orig_DBFH = nullptr;

static void my_DBFH(void* instance, void* method) {
    if (orig_DBFH) orig_DBFH(instance, method);
    if (!instance) return;
    void* bihCtrl = *(void**)((uint8_t*)instance + 0x30);
    if (!bihCtrl) return;
    float valid = *(float*)((uint8_t*)bihCtrl + 0x7C);
    if (valid != 1.0f) return;
    float x = *(float*)((uint8_t*)bihCtrl + 0x78);
    float y = *(float*)((uint8_t*)bihCtrl + 0x80);
    if (!isfinite(x) || !isfinite(y) || fabsf(x) > 2.f || fabsf(y) > 2.f) return;
    g_ghost.savedCueBallX = x; g_ghost.savedCueBallY = y;
    g_ghost.hasSavedCueBallPos = true;
    
    if (g_ghost.bihActive && g_ghost.botState != GhostState::BOT_PLACE_BALL) {
        g_ghost.bihDroppedThisTurn = true;
    }
    g_ghost.bihActive = false;
    triggerGhostSimulation();
    pushToOverlay();
}


typedef void (*CCShootStart_t)(void* cue, void* method);
static CCShootStart_t orig_CCShootStart = nullptr;

static void my_CCShootStart(void* cue, void* method) {
    if (!ghostCallActive && g_ghost.realForceEnabled && cue) {
        if (g_ghost.angleOverride) {
            convergeAimToTarget(g_ghost.currentAimDirX, g_ghost.currentAimDirY);
            *(float*)((uint8_t*)cue + 0x64) = g_ghost.currentAimDirX;
            *(float*)((uint8_t*)cue + 0x6C) = g_ghost.currentAimDirY;
        }
        
        if (g_ghost.lastPoolGameController) {
            void* pbc = *(void**)((uint8_t*)g_ghost.lastPoolGameController + 0x60);
            if (pbc) {
                float f = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
                *(float*)((uint8_t*)pbc + 0x58) = f;
            }
        }
    }
    if (orig_CCShootStart) orig_CCShootStart(cue, method);
}


GameStore_IsMyTurn_t g_IsMyTurn = nullptr;
OnTurnChanged_t      orig_OnTurnChanged = nullptr;
GameOver_t           orig_GameOver = nullptr;
MoveBIHInstantly_t   g_MoveBIHInstantly = nullptr;
PGC_Shoot_t          g_PGC_Shoot = nullptr;
GM_getInstance_t     g_GM_getInstance = nullptr;
PocketSelected_t     g_PocketSelected = nullptr;
TakeBIH_t            g_TakeBIH = nullptr;


static void my_OnTurnChanged(void* instance, void* method) {
    if (orig_OnTurnChanged) orig_OnTurnChanged(instance, method);
    if (instance) g_ghost.lastPoolGameController = instance;

    
    bool myTurn = g_IsMyTurn ? g_IsMyTurn(nullptr) : false;
    g_ghost.isMyTurn = myTurn;
    g_ghost.turnJustChanged = true;

    
    bool gameActive = (g_ghost.gameId > 1000000 &&
                       !g_ghost.freshEncPropsString.empty());

    
    if (!myTurn) {
        g_ghost.bihDroppedThisTurn = false;
    }
    if (g_ghost.botEnabled && g_ghost.botLastShotMs > 0) {
        uint64_t elapsed = nowMs() - g_ghost.botLastShotMs;
        if (elapsed < 3000) {
            return;
        }
    }

    
    if (myTurn && g_ghost.botEnabled && gameActive
        && g_ghost.botState != GhostState::BOT_SCANNING
        && g_ghost.botState != GhostState::BOT_AIMING
        && g_ghost.botState != GhostState::BOT_NOMINATING
        && g_ghost.botState != GhostState::BOT_SHOOTING) {
        g_ghost.botState = GhostState::BOT_WAIT_TURN;
        g_ghost.botStateStartMs = nowMs();
        
        g_ghost.botActionDelayMs = 1500 + (rand() % 2000);
        g_ghost.botShotFired = false;
        g_ghost.botFoundShot = false;
        g_ghost.botAimFrames = 0;
        g_ghost.botBIHAttempt = -1;
        g_ghost.botScanAttempt = 0;
        g_ghost.hasBallPositions = false;
        g_ghost.botIsBreak = false;
        g_ghost.turnStartMs = nowMs();
        
        g_ghost.botTurnHadBIH = false;
        void* gmBih = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
        if (gmBih) {
            g_ghost.botTurnHadBIH = *(bool*)((uint8_t*)gmBih + OFF_GM_CanHaveBallInHand);
        }
    }
    
    else if (!myTurn && g_ghost.botEnabled) {
        g_ghost.botLastShotMs = 0;  
        if (g_ghost.botState != GhostState::BOT_OFF &&
            g_ghost.botState != GhostState::BOT_ANIMATING &&
            g_ghost.botState != GhostState::BOT_GAME_OVER) {
            g_ghost.botState = GhostState::BOT_OFF;
            g_ghost.angleOverride = false;
        }
    }
}


static void my_GameOver(void* instance, void* callback, void* confetti, void* method) {
    if (orig_GameOver) orig_GameOver(instance, callback, confetti, method);

    g_ghost.isMyTurn = false;
    g_ghost.animating = false;

    if (g_ghost.botEnabled) {
        g_ghost.botState = GhostState::BOT_GAME_OVER;
        g_ghost.botStateStartMs = nowMs();
        g_ghost.botAimFrames = 0;  
    }
}


static PocketSelected_t orig_PocketSelected = nullptr;

static void my_PocketSelected(void* pgc, int32_t pocket, void* method) {
    if (orig_PocketSelected) orig_PocketSelected(pgc, pocket, method);
}
typedef void (*FinishAnim_t)(void* instance, void* method);
static FinishAnim_t orig_FinishAnim = nullptr;

static void my_FinishAnim(void* instance, void* method) {
    if (orig_FinishAnim) orig_FinishAnim(instance, method);
    g_ghost.angleOverride = false;
    g_ghost.animating = false;
    g_ghost.powerBarActive = false; 
    g_ghost.currentAimSpinX = 0.f; 
    g_ghost.currentAimSpinY = 0.f;
    if (g_ghost.sliderForceValue >= 0.05f)
        g_ghost.currentAimForce = g_ghost.sliderForceValue;
    if (instance) g_ghost.lastPoolGameController = instance;


    
    if (g_ghost.botEnabled && g_ghost.botState == GhostState::BOT_ANIMATING) {
        g_ghost.botState = GhostState::BOT_OFF;
        g_ghost.botLastShotMs = 0;  
    }
}


static int getListCount(void* listPtr);

struct ValidShot {
    float angle;
    float force;
    int   ballPocketed;
    int   pocketIdx;
    float cueFinalX, cueFinalZ;
    float ball8FinalX, ball8FinalZ;
    bool  ball8Moved;
    float allBallFinals[16][2];    
    bool  allBallMoved[16];
    int   firstHitBall;
    float strategicScore;
};
static std::vector<ValidShot> s_validShots;


static bool hasLineOfSightCustom(float ax, float az, float bx, float bz,
                                  int skipBall1, int skipBall2,
                                  const float ballPositions[][2], const bool ballOnTable[], int ballCount) {
    float dx = bx - ax;
    float dz = bz - az;
    float dist = sqrtf(dx*dx + dz*dz);
    if (dist < 0.01f) return true;
    float ndx = dx / dist;
    float ndz = dz / dist;

    for (int i = 0; i < ballCount && i <= 15; i++) {
        if (i == skipBall1 || i == skipBall2 || i == 0) continue;
        if (!ballOnTable[i]) continue;

        float bpx = ballPositions[i][0] - ax;
        float bpz = ballPositions[i][1] - az;
        float proj = bpx * ndx + bpz * ndz;
        if (proj < BALL_RADIUS_WORLD || proj > dist - BALL_RADIUS_WORLD) continue;
        float perpX = bpx - ndx * proj;
        float perpZ = bpz - ndz * proj;
        float perpDist = sqrtf(perpX*perpX + perpZ*perpZ);
        if (perpDist < BALL_RADIUS_WORLD * 2.5f) return false;
    }
    return true;
}


static bool hasLineOfSight(float ax, float az, float bx, float bz, int skipBall1, int skipBall2) {
    return hasLineOfSightCustom(ax, az, bx, bz, skipBall1, skipBall2,
                                 g_ghost.ballPos, g_ghost.ballOnTable, g_ghost.ballCount);
}


static int countBlockedBallsCustom(int skipBall, const float ballPositions[][2],
                                     const bool ballOnTable[], int ballCount,
                                     int myMin, int myMax,
                                     float cueFinalX = -999.f, float cueFinalZ = -999.f) {
    int blocked = 0;
    
    bool hasCueFinal = (cueFinalX > -900.f);
    for (int bi = myMin; bi <= myMax; bi++) {
        if (bi == skipBall) continue;
        if (!ballOnTable[bi]) continue;

        float bx = ballPositions[bi][0];
        float bz = ballPositions[bi][1];

        
        bool anyPocketReachable = false;
        for (int pi = 0; pi < 6; pi++) {
            if (hasLineOfSightCustom(bx, bz, POCKET_POS[pi][0], POCKET_POS[pi][1],
                                      bi, 0, ballPositions, ballOnTable, ballCount)) {
                anyPocketReachable = true;
                break;
            }
        }

        
        bool cueCanReach = true;
        if (hasCueFinal) {
            cueCanReach = hasLineOfSightCustom(cueFinalX, cueFinalZ, bx, bz,
                                                0, bi, ballPositions, ballOnTable, ballCount);
        }

        if (!anyPocketReachable || !cueCanReach) blocked++;
    }
    return blocked;
}


static int countBlockedBalls(int skipBall) {
    void* gm = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
    if (!gm || !g_ghost.hasBallPositions) return 0;

    bool ownsSolids = *(bool*)((uint8_t*)gm + OFF_GM_MyPlayerOwnsSolids);
    void* solidsList = *(void**)((uint8_t*)gm + OFF_GM_RemainingSolids);
    void* stripesList = *(void**)((uint8_t*)gm + OFF_GM_RemainingStripes);
    int totalRem = getListCount(solidsList) + getListCount(stripesList);
    if (totalRem >= 14) return 0;  

    int myMin = ownsSolids ? 1 : 9;
    int myMax = ownsSolids ? 7 : 15;
    int blocked = 0;

    
    float cueX = g_ghost.ballPos[0][0];
    float cueZ = g_ghost.ballPos[0][1];

    for (int bi = myMin; bi <= myMax; bi++) {
        if (bi == skipBall) continue;
        if (!g_ghost.ballOnTable[bi]) continue;

        float bx = g_ghost.ballPos[bi][0];
        float bz = g_ghost.ballPos[bi][1];

        
        bool anyPocketReachable = false;
        for (int pi = 0; pi < 6; pi++) {
            if (hasLineOfSight(bx, bz, POCKET_POS[pi][0], POCKET_POS[pi][1], bi, 0)) {
                anyPocketReachable = true;
                break;
            }
        }

        
        bool cueCanReach = hasLineOfSight(cueX, cueZ, bx, bz, 0, bi);

        
        if (!anyPocketReachable || !cueCanReach) blocked++;
    }
    return blocked;
}


static const float CLUSTER_RADIUS    = 0.154f;
static const float CLUSTER_RADIUS_SQ = 0.154f * 0.154f;


static int countNeighbors(int ballIdx,
                            const float positions[][2],
                            const bool  onTable[],
                            int         count) {
    float bx = positions[ballIdx][0];
    float bz = positions[ballIdx][1];
    int n = 0;
    for (int i = 0; i < count && i <= 15; i++) {
        if (i == ballIdx || !onTable[i]) continue;
        float dx = positions[i][0] - bx;
        float dz = positions[i][1] - bz;
        if (dx*dx + dz*dz < CLUSTER_RADIUS_SQ) n++;
    }
    return n;
}


static int totalClusterPressure(int skipBall,
                                 const float positions[][2],
                                 const bool  onTable[],
                                 int         count,
                                 int myMin, int myMax) {
    int total = 0;
    for (int bi = myMin; bi <= myMax; bi++) {
        if (bi == skipBall || !onTable[bi]) continue;
        total += countNeighbors(bi, positions, onTable, count);
    }
    return total;
}


static float scoreShot(const ValidShot& shot) {
    float score = 0.f;

    
    float distFromCenter = sqrtf(shot.cueFinalX * shot.cueFinalX + shot.cueFinalZ * shot.cueFinalZ);
    score += fmaxf(0.f, 10.f - distFromCenter * 8.f);

    
    float minPocketDist = 1e10f;
    for (int pi = 0; pi < 6; pi++) {
        float dx = shot.cueFinalX - POCKET_POS[pi][0];
        float dz = shot.cueFinalZ - POCKET_POS[pi][1];
        float d = sqrtf(dx*dx + dz*dz);
        if (d < minPocketDist) minPocketDist = d;
    }
    score += fminf(5.f, minPocketDist * 5.f);  

    
    if (g_ghost.hasBallPositions) {
        void* gm = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
        if (gm) {
            bool ownsSolids = *(bool*)((uint8_t*)gm + OFF_GM_MyPlayerOwnsSolids);
            int myMin = ownsSolids ? 1 : 9;
            int myMax = ownsSolids ? 7 : 15;
            int myRem = ownsSolids ? getListCount(*(void**)((uint8_t*)gm + OFF_GM_RemainingSolids))
                                   : getListCount(*(void**)((uint8_t*)gm + OFF_GM_RemainingStripes));

            
            float postPos[16][2];
            bool  postOnTable[16];
            memcpy(postPos, shot.allBallFinals, sizeof(postPos));
            memcpy(postOnTable, g_ghost.ballOnTable, sizeof(postOnTable));
            
            if (shot.ballPocketed >= 0 && shot.ballPocketed < 16)
                postOnTable[shot.ballPocketed] = false;

            
            int reachableBalls = 0;
            for (int bi = myMin; bi <= myMax; bi++) {
                if (bi == shot.ballPocketed) continue;
                if (!postOnTable[bi]) continue;
                if (hasLineOfSightCustom(shot.cueFinalX, shot.cueFinalZ,
                                          postPos[bi][0], postPos[bi][1],
                                          0, shot.ballPocketed,
                                          postPos, postOnTable, g_ghost.ballCount)) {
                    reachableBalls++;
                }
            }
            score += reachableBalls * 5.f;

            
            int extraBalls = 0;
            
            for (int bi = 1; bi <= 15; bi++) {
                if (bi == 8) continue;  
                if (g_ghost.ballOnTable[bi] && !postOnTable[bi])
                    extraBalls++;
            }
            if (extraBalls > 1) {
                
                score += (extraBalls - 1) * 12.f;
            }

            
            if (myRem > 1) {
                int blockedBefore = countBlockedBalls(-1);
                int blockedAfter = countBlockedBallsCustom(shot.ballPocketed,
                                                            postPos, postOnTable,
                                                            g_ghost.ballCount, myMin, myMax,
                                                            shot.cueFinalX, shot.cueFinalZ);

                int unblocked = blockedBefore - blockedAfter;

                if (unblocked > 0) {
                    
                    float bonus = (blockedBefore > 0) ? 75.f : 30.f;
                    score += unblocked * bonus;
                }
                
                if (blockedAfter > blockedBefore) {
                    int newBlocks = blockedAfter - blockedBefore;
                    score -= newBlocks * 90.f;
                }
                
                if (blockedAfter > 0 && unblocked == 0) {
                    float penalty = (blockedBefore > 0) ? 40.f : 10.f;
                    score -= penalty;
                }
                
                
                if (myRem == 2 && blockedAfter > 0 && unblocked == 0) {
                    score -= 60.f;
                }

                
                
                
                
                if (myRem == 2 &&
                    shot.ballPocketed >= myMin && shot.ballPocketed <= myMax) {
                    int otherBall = -1;
                    for (int bi = myMin; bi <= myMax; bi++) {
                        if (bi == shot.ballPocketed || !postOnTable[bi]) continue;
                        otherBall = bi; break;
                    }
                    if (otherBall >= 0) {
                        float obx = postPos[otherBall][0];
                        float obz = postPos[otherBall][1];
                        bool canSetupShot = false;
                        for (int pi = 0; pi < 6 && !canSetupShot; pi++) {
                            float pdx = POCKET_POS[pi][0] - obx;
                            float pdz = POCKET_POS[pi][1] - obz;
                            float pdist = sqrtf(pdx*pdx + pdz*pdz);
                            if (pdist < 0.05f) continue;
                            
                            if (!hasLineOfSightCustom(
                                    obx, obz,
                                    POCKET_POS[pi][0], POCKET_POS[pi][1],
                                    otherBall, 0,
                                    postPos, postOnTable, g_ghost.ballCount))
                                continue;
                            
                            float ghostX = obx - (pdx / pdist) * 2.0f * BALL_RADIUS_WORLD;
                            float ghostZ = obz - (pdz / pdist) * 2.0f * BALL_RADIUS_WORLD;
                            
                            if (hasLineOfSightCustom(
                                    shot.cueFinalX, shot.cueFinalZ,
                                    ghostX, ghostZ,
                                    0, otherBall,
                                    postPos, postOnTable, g_ghost.ballCount))
                                canSetupShot = true;
                        }
                        if (!canSetupShot) {
                            
                            score -= 150.f;
                        }
                    }
                }
            }

            
            
            if (myRem == 1) {
                float b8x = postPos[8][0];
                float b8z = postPos[8][1];
                bool ball8HasPath = false;
                for (int pi = 0; pi < 6 && !ball8HasPath; pi++) {
                    if (!hasLineOfSightCustom(b8x, b8z,
                             POCKET_POS[pi][0], POCKET_POS[pi][1],
                             8, -1, postPos, postOnTable, g_ghost.ballCount)) continue;
                    if (hasLineOfSightCustom(shot.cueFinalX, shot.cueFinalZ,
                             b8x, b8z, 0, 8, postPos, postOnTable, g_ghost.ballCount))
                        ball8HasPath = true;
                }
                if (!ball8HasPath) score -= 120.f;
            }

            
            if (myRem > 0) {
                
                int pressureBefore = totalClusterPressure(
                    -1, g_ghost.ballPos, g_ghost.ballOnTable,
                    g_ghost.ballCount, myMin, myMax);

                
                int pressureAfter = totalClusterPressure(
                    shot.ballPocketed, postPos, postOnTable,
                    g_ghost.ballCount, myMin, myMax);

                int reduction = pressureBefore - pressureAfter;
                if (reduction > 0) {
                    
                    score += reduction * 14.f;
                } else if (reduction < 0) {
                    
                    score += reduction * 15.f;  
                }

                
                
                if (shot.ballPocketed >= myMin && shot.ballPocketed <= myMax) {
                    int pocketedNeighbors = countNeighbors(
                        shot.ballPocketed, g_ghost.ballPos,
                        g_ghost.ballOnTable, g_ghost.ballCount);
                    if (pocketedNeighbors >= 2) {
                        
                        score += pocketedNeighbors * 10.f;
                    }
                }
            }
        }
    }

    
    score += (1.f - shot.force) * 3.f;

    
    if (shot.ball8Moved && shot.ballPocketed != 8) {
        
        float postPos[16][2];
        bool  postOnTable[16];
        memcpy(postPos, shot.allBallFinals, sizeof(postPos));
        memcpy(postOnTable, g_ghost.ballOnTable, sizeof(postOnTable));
        if (shot.ballPocketed >= 0 && shot.ballPocketed < 16)
            postOnTable[shot.ballPocketed] = false;

        bool ball8HasPath = false;
        for (int pi = 0; pi < 6; pi++) {
            if (hasLineOfSightCustom(shot.ball8FinalX, shot.ball8FinalZ,
                                      POCKET_POS[pi][0], POCKET_POS[pi][1], 8, shot.ballPocketed,
                                      postPos, postOnTable, g_ghost.ballCount)) {
                ball8HasPath = true;
                break;
            }
        }
        if (!ball8HasPath) {
            score -= 50.f;
        }
    }

    return score;
}


static int getListCount(void* listPtr) {
    if (!listPtr) return 0;
    return *(int*)((uint8_t*)listPtr + 0x18);
}


static bool segmentHasCushionHit(const std::vector<std::pair<float,float>>& wp,
                                   int start, int end) {
    for (int i = start + 1; i < end - 1 && i < (int)wp.size() - 1; i++) {
        float d1x = wp[i].first    - wp[i-1].first;
        float d1z = wp[i].second   - wp[i-1].second;
        float d2x = wp[i+1].first  - wp[i].first;
        float d2z = wp[i+1].second - wp[i].second;
        float len1 = sqrtf(d1x*d1x + d1z*d1z);
        float len2 = sqrtf(d2x*d2x + d2z*d2z);
        if (len1 < 0.001f || len2 < 0.001f) continue;
        if ((d1x*d2x + d1z*d2z) / (len1 * len2) < 0.85f) return true;
    }
    return false;
}


static bool ball8HasAnyCushionHit(const std::vector<std::pair<float,float>>& wp) {
    return (wp.size() >= 3) && segmentHasCushionHit(wp, 0, (int)wp.size());
}


static bool cueBallHitCushionBeforeBall8(
        const std::vector<std::pair<float,float>>& cueWp,
        float ball8InitX, float ball8InitZ) {
    if (cueWp.size() < 3) return false;
    
    
    float minDist = 1e10f;
    int impactIdx = 0;
    for (int i = 0; i < (int)cueWp.size(); i++) {
        float dx = cueWp[i].first  - ball8InitX;
        float dz = cueWp[i].second - ball8InitZ;
        float d = dx*dx + dz*dz;
        if (d < minDist) { minDist = d; impactIdx = i; }
    }
    
    return (impactIdx >= 2) && segmentHasCushionHit(cueWp, 0, impactIdx);
}


static bool ball8HitAdjacentCushion(int pocketIdx,
                                      const std::vector<std::pair<float,float>>& wp) {
    if (wp.size() < 3) return false;
    int pi = pocketIdx - 201;
    if (pi < 0 || pi > 5) return false;
    float px = POCKET_POS[pi][0];
    float pz = POCKET_POS[pi][1];
    const float ADJ_RADIUS_SQ = 0.18f * 0.18f;
    int n = (int)wp.size();
    int start = (n - 8 > 0) ? (n - 8) : 0;
    for (int i = start + 1; i < n - 1; i++) {
        float wx = wp[i].first,  wz = wp[i].second;
        float ddx = wx - px,     ddz = wz - pz;
        if (ddx*ddx + ddz*ddz > ADJ_RADIUS_SQ) continue;
        float d1x = wx - wp[i-1].first,  d1z = wz - wp[i-1].second;
        float d2x = wp[i+1].first - wx,   d2z = wp[i+1].second - wz;
        float len1 = sqrtf(d1x*d1x + d1z*d1z);
        float len2 = sqrtf(d2x*d2x + d2z*d2z);
        if (len1 < 0.001f || len2 < 0.001f) continue;
        if ((d1x*d2x + d1z*d2z) / (len1 * len2) < 0.85f) return true;
    }
    return false;
}

static bool botValidatePocketedBalls(const std::vector<int>& pocketed) {
    if (pocketed.empty()) return false;

    
    {
        char logbuf[80] = {};
        snprintf(logbuf, sizeof(logbuf), "validate [");
        for (int b : pocketed) {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%d,", b);
            strncat(logbuf, tmp, sizeof(logbuf)-strlen(logbuf)-1);
        }
        strncat(logbuf, "] fhit=", sizeof(logbuf)-strlen(logbuf)-1);
        char tmp2[8]; snprintf(tmp2, sizeof(tmp2), "%d", cachedFirstHitBall);
        strncat(logbuf, tmp2, sizeof(logbuf)-strlen(logbuf)-1);
    }

    
    if (cachedFirstHitBall <= 0) {  return false; }

    
    for (int b : pocketed) {
        if (b == 0) {  return false; }
    }

    
    bool pockets8 = false;
    int  myBallsPocketed = 0;
    int  totalNon8Pocketed = 0;

    void* gm = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
    if (!gm) {
        
        for (int b : pocketed) {
            if (b == 8) {  return false; }
            if (b >= 1 && b <= 15) return true;
        }
        return false;
    }

    bool ownsSolids = *(bool*)((uint8_t*)gm + OFF_GM_MyPlayerOwnsSolids);
    void* solidsList = *(void**)((uint8_t*)gm + OFF_GM_RemainingSolids);
    void* stripesList = *(void**)((uint8_t*)gm + OFF_GM_RemainingStripes);
    int remSolids = getListCount(solidsList);
    int remStripes = getListCount(stripesList);
    int totalRem = remSolids + remStripes;

    int myMin = ownsSolids ? 1 : 9;
    int myMax = ownsSolids ? 7 : 15;
    int myRem = ownsSolids ? remSolids : remStripes;
    bool openTable = (totalRem >= 14);

    for (int b : pocketed) {
        if (b == 8) {
            pockets8 = true;
        } else if (b >= 1 && b <= 15) {
            totalNon8Pocketed++;
            if (!openTable) {
                if (b >= myMin && b <= myMax) myBallsPocketed++;
            }
        }
    }

    
    if (openTable) {
        
        if (pockets8) {  return false; }
        
        if (cachedFirstHitBall == 8) {  return false; }
        
        return (totalNon8Pocketed > 0);
    }

    

    
    

    
    if (cachedFirstHitBall > 0) {
        bool firstHitMine = (cachedFirstHitBall >= myMin && cachedFirstHitBall <= myMax);
        bool firstHit8 = (cachedFirstHitBall == 8);

        if (myRem > 0) {
            
            if (!firstHitMine) {  return false; }
        } else {
            
            if (!firstHit8) {  return false; }
        }
    }

    
    if (pockets8) {
        
        if (myRem > 0) {  return false; }

        
        if (totalNon8Pocketed > 0) {  return false; }

        
        if (!g_ghost.hasBall8Waypoints || g_ghost.ball8Waypoints.empty()) {
            
            return false;
        }
        if (g_ghost.hasBall8Waypoints && !g_ghost.ball8Waypoints.empty()) {
            const auto& wp8 = g_ghost.ball8Waypoints;

            
            int ball8PocketIdx = -1;
            for (int i = 0; i < (int)pocketed.size(); i++) {
                if (pocketed[i] == 8 && i < (int)cachedPocketIndices.size()) {
                    ball8PocketIdx = cachedPocketIndices[i];
                    break;
                }
            }

            
            bool cueHitCushionFirst = false;
            if (g_ghost.hasCueWaypoints && !g_ghost.cueWaypoints.empty())
                cueHitCushionFirst = cueBallHitCushionBeforeBall8(
                    g_ghost.cueWaypoints, g_ghost.ball8InitX, g_ghost.ball8InitZ);

            if (cueHitCushionFirst) {
                
            } else {
                
                if (!ball8HasAnyCushionHit(wp8)) {
                    return false;
                }
                if (ball8PocketIdx > 0 && ball8HitAdjacentCushion(ball8PocketIdx, wp8)) {
                    return false;
                }
            }
        }

        
        return true;
    }

    
    return (myBallsPocketed > 0);
}


static bool findFreeBIHPosition(float& outX, float& outZ) {
    if (!g_ghost.hasBallPositions || g_ghost.ballCount < 1) {
        outX = 0.f; outZ = 0.f; return false;
    }
    auto isFree = [&](float cx, float cz) -> bool {
        for (int i = 1; i < g_ghost.ballCount && i <= 15; i++) {
            if (!g_ghost.ballOnTable[i]) continue;
            float dx = g_ghost.ballPos[i][0] - cx;
            float dz = g_ghost.ballPos[i][1] - cz;
            if (dx*dx + dz*dz < 0.08f * 0.08f) return false;
        }
        return true;
    };
    static const float C[][2] = {
        { 0.00f,  0.00f},{-0.15f,  0.00f},{ 0.15f,  0.00f},
        { 0.00f,-0.15f},{ 0.00f, 0.15f},{-0.30f,  0.00f},
        { 0.30f,  0.00f},{-0.15f,-0.15f},{ 0.15f,-0.15f},
        {-0.15f, 0.15f},{ 0.15f, 0.15f},{ 0.00f,-0.30f},
        { 0.00f, 0.30f},{-0.45f,  0.00f},{ 0.45f,  0.00f},
    };
    for (auto& c : C)
        if (isFree(c[0], c[1])) { outX = c[0]; outZ = c[1]; return true; }
    for (float z = -0.45f; z <= 0.45f; z += 0.12f)
        for (float x = -0.90f; x <= 0.90f; x += 0.12f)
            if (isFree(x, z)) { outX = x; outZ = z; return true; }
    outX = 0.f; outZ = 0.f; return false;
}


static const int    BOT_SCAN_STEPS       = 720;   
static const int    BOT_POWER_SCAN_STEPS = 2094;  
static const int    BOT_SIMS_PER_FRAME   = 10;
static const float  BOT_SCAN_FORCES[]    = { 1.0f, 0.60f, 0.30f };
static const int    BOT_SCAN_FORCES_N    = 3;
static const float  BOT_POWER_FORCE      = 1.0f;
static const int    BOT_AIM_FRAMES       = 30;
static const int    BOT_SCAN_TIMEOUT     = 25000;


struct ScanCandidate {
    float angle;
    float force;
    float score;
};
static std::vector<ScanCandidate> s_smartCandidates;
static int s_smartIdx = 0;


static int    s_scanStep     = 0;
static int    s_scanForceIdx = 0;
static bool   s_scanDone     = false;


static void buildSmartCandidates() {
    s_smartCandidates.clear();
    if (!g_ghost.hasBallPositions) return;

    float cueX = g_ghost.ballPos[0][0];
    float cueZ = g_ghost.ballPos[0][1];

    void* gm = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
    bool ownsSolids = gm ? *(bool*)((uint8_t*)gm + OFF_GM_MyPlayerOwnsSolids) : false;
    void* solidsList = gm ? *(void**)((uint8_t*)gm + OFF_GM_RemainingSolids) : nullptr;
    void* stripesList = gm ? *(void**)((uint8_t*)gm + OFF_GM_RemainingStripes) : nullptr;
    int remSolids = getListCount(solidsList);
    int remStripes = getListCount(stripesList);
    int totalRem = remSolids + remStripes;
    bool openTable = (totalRem >= 14);

    int myMin = ownsSolids ? 1 : 9;
    int myMax = ownsSolids ? 7 : 15;
    int myRem = ownsSolids ? remSolids : remStripes;

    for (int bi = 1; bi < g_ghost.ballCount && bi <= 15; bi++) {
        if (!g_ghost.ballOnTable[bi]) continue;

        if (!openTable) {
            if (myRem > 0) {
                if (bi < myMin || bi > myMax) continue;
            } else {
                if (bi != 8) continue;
            }
        } else {
            if (bi == 8) continue;
        }

        float bx = g_ghost.ballPos[bi][0];
        float bz = g_ghost.ballPos[bi][1];

        for (int pi = 0; pi < 6; pi++) {
            float px = POCKET_POS[pi][0];
            float pz = POCKET_POS[pi][1];

            float dx = px - bx;
            float dz = pz - bz;
            float distToPocket = sqrtf(dx*dx + dz*dz);
            if (distToPocket < 0.05f) continue;

            float dirX = dx / distToPocket;
            float dirZ = dz / distToPocket;

            
            float ghostX = bx - dirX * 2.0f * BALL_RADIUS_WORLD;
            float ghostZ = bz - dirZ * 2.0f * BALL_RADIUS_WORLD;

            float shotDx = ghostX - cueX;
            float shotDz = ghostZ - cueZ;
            float shotDist = sqrtf(shotDx*shotDx + shotDz*shotDz);
            if (shotDist < 0.01f) continue;

            float angle = atan2f(shotDx, shotDz);
            if (angle < 0) angle += 2.0f * 3.14159265f;

            float totalDist = shotDist + distToPocket;
            float score = totalDist;

            
            
            
            float force = sqrtf(totalDist * 0.4f);
            force = fmaxf(0.15f, fminf(1.0f, force));

            s_smartCandidates.push_back({angle, force, score});
        }
    }

    std::sort(s_smartCandidates.begin(), s_smartCandidates.end(),
        [](const ScanCandidate& a, const ScanCandidate& b) { return a.score < b.score; });

}


static bool isRackFormation() {
    if (!g_ghost.hasBallPositions) return false;

    
    int count = 0;
    float sumX = 0.f, sumZ = 0.f;
    for (int i = 1; i <= 15; i++) {
        if (!g_ghost.ballOnTable[i]) return false;  
        sumX += g_ghost.ballPos[i][0];
        sumZ += g_ghost.ballPos[i][1];
        count++;
    }
    if (count != 15) return false;

    float cx = sumX / 15.f;
    float cz = sumZ / 15.f;

    
    float maxDist = 0.f;
    for (int i = 1; i <= 15; i++) {
        float dx = g_ghost.ballPos[i][0] - cx;
        float dz = g_ghost.ballPos[i][1] - cz;
        float d = sqrtf(dx*dx + dz*dz);
        if (d > maxDist) maxDist = d;
    }

    
    bool isRack = (maxDist < 0.30f);
    return isRack;
}


static const float BREAK_CUE_X     = -0.502960f;
static const float BREAK_CUE_Z     = -0.466480f;
static const float BREAK_FORCE     =  0.925298f;
static const float BREAK_DIR_X     =  0.955033f;  
static const float BREAK_DIR_Z     =  0.296499f;
static const float BREAK_SPIN_X    =  0.996832f;  
static const float BREAK_SPIN_Z    = -0.079534f;

static void botUpdate(void* cue) {
    if (!license_security_allows_game(GOAT_GAME_POOL)) {
        disableUnlicensedPoolRuntime();
        return;
    }
    uint64_t now = nowMs();
    void* pgc = g_ghost.lastPoolGameController;

    
    
    if (g_ghost.botState == GhostState::BOT_GAME_OVER) {
        
    } else {
        
        bool gameActive = (g_ghost.gameId > 1000000 &&
                           !g_ghost.freshEncPropsString.empty() &&
                           pgc != nullptr);
        if (!gameActive) {
            if (g_ghost.botState != GhostState::BOT_OFF) {
                g_ghost.botState = GhostState::BOT_OFF;
                g_ghost.angleOverride = false;
            }
            return;
        }

        
        if (g_ghost.botState != GhostState::BOT_ANIMATING) {
            if (!g_ghost.isMyTurn) {
                if (g_ghost.botState != GhostState::BOT_OFF) {
                    g_ghost.botState = GhostState::BOT_OFF;
                    g_ghost.angleOverride = false;
                }
                return;
            }
        }

        
        if (g_ghost.animating && g_ghost.botState != GhostState::BOT_ANIMATING) {
            return;
        }
    }

    switch (g_ghost.botState) {

    
    case GhostState::BOT_WAIT_TURN: {
        if (now - g_ghost.botStateStartMs < g_ghost.botActionDelayMs) return;

        
        if (!g_ghost.hasBallPositions) {
            g_ghost.currentAimDirX = 0.0f;
            g_ghost.currentAimDirY = 1.0f;
            g_ghost.sliderForceValue = 0.5f;
            g_ghost.currentAimForce = 0.5f;
            performGhostSimulation();
            if (!g_ghost.hasBallPositions) return;  
        }

        
        g_ghost.botIsBreak = isRackFormation();

        if (g_ghost.botIsBreak) {
            
            if (!g_ghost.bihActive && pgc && g_TakeBIH) {
                
                g_TakeBIH(pgc, nullptr);
                g_ghost.bihActive = true;
            }
            g_ghost.botState = GhostState::BOT_PLACE_BALL;
            g_ghost.botStateStartMs = now;
            g_ghost.botAimFrames = 0;
            g_ghost.botBIHAttempt = 0;
            return;
        }

        
        bool isBIHNow = false;
        void* gmBih2 = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
        if (gmBih2) {
            isBIHNow = *(bool*)((uint8_t*)gmBih2 + OFF_GM_CanHaveBallInHand);
        }
        bool wasBIH = g_ghost.botTurnHadBIH || isBIHNow;
        if (wasBIH) {
            if (!g_ghost.bihActive && pgc && g_TakeBIH) {
                g_TakeBIH(pgc, nullptr);
                g_ghost.bihActive = true;
            }
            g_ghost.botState = GhostState::BOT_PLACE_BALL;
            g_ghost.botStateStartMs = now;
            g_ghost.botAimFrames = 0;
            g_ghost.botBIHAttempt = 0;
            return;
        }

        
        g_ghost.botState = GhostState::BOT_SCANNING;
        g_ghost.botStateStartMs = now;
        g_ghost.botFoundShot = false;
        g_ghost.botBestDist = 1e10f;
        g_ghost.botBestPocketIdx = -1;
        g_ghost.botScanAttempt = 0;  
        g_ghost.hasBallPositions = false;
        s_smartIdx = 0;
        s_validShots.clear();
        s_scanStep = 0;
        s_scanForceIdx = 0;
        s_scanDone = false;
        return;
    }

    
    case GhostState::BOT_PLACE_BALL: {
        if (!pgc) {
            g_ghost.botState = GhostState::BOT_SCANNING;
            g_ghost.botStateStartMs = now;
            break;
        }

        
        static const float BIH_POSITIONS[][2] = {
            { 0.00f,  0.00f},  
            {-0.20f,  0.00f},  
            { 0.20f,  0.00f},  
            { 0.00f, -0.10f},  
        };
        static const int BIH_MAX_POSITIONS = 4;

        g_ghost.botAimFrames++;
        static float s_bihPlacedX = 0.f, s_bihPlacedZ = 0.f;
        static float s_bihStartX = 0.f, s_bihStartZ = 0.f;

        if (g_ghost.botAimFrames == 1) {
            float px, pz;
            if (g_ghost.botIsBreak) {
                px = BREAK_CUE_X;
                pz = BREAK_CUE_Z;
            } else {
                if (!findFreeBIHPosition(px, pz)) {
                    int idx = g_ghost.botBIHAttempt;
                    if (idx >= BIH_MAX_POSITIONS) idx = 0;
                    px = BIH_POSITIONS[idx][0];
                    pz = BIH_POSITIONS[idx][1];
                }
            }
            s_bihPlacedX = px;
            s_bihPlacedZ = pz;
            s_bihStartX = g_ghost.savedCueBallX;
            s_bihStartZ = g_ghost.savedCueBallY;
        }
        
        if (g_ghost.botAimFrames >= 1 && g_ghost.botAimFrames <= 25) {
            float t = (float)g_ghost.botAimFrames / 25.0f;
            float ease = t * (2.0f - t);  
            float cx = s_bihStartX + (s_bihPlacedX - s_bihStartX) * ease;
            float cz = s_bihStartZ + (s_bihPlacedZ - s_bihStartZ) * ease;
            if (g_MoveBIHInstantly) {
                g_MoveBIHInstantly(pgc, cx, g_ghost.tableWorldY, cz, nullptr);
            }
            
            g_ghost.savedCueBallX = cx;
            g_ghost.savedCueBallY = cz;
            g_ghost.hasSavedCueBallPos = true;
            g_ghost.hasSceneCueBallPos = false;  
            
            float dx = s_bihPlacedX - cx;
            float dz = s_bihPlacedZ - cz;
            float dist = sqrtf(dx*dx + dz*dz);
            if (dist < 0.01f) { dx = 0.f; dz = 1.f; }
            else { dx /= dist; dz /= dist; }
            g_ghost.currentAimDirX = dx;
            g_ghost.currentAimDirY = dz;
            g_ghost.angleOverride = true;
            g_ghost.realForceEnabled = true;
            
            if (g_ghost.botAimFrames % 5 == 0) {
                g_ghost.sliderForceValue = 0.5f;
                g_ghost.currentAimForce = 0.5f;
                triggerGhostSimulation();
                pushToOverlay();
            }
        }
        else if (g_ghost.botAimFrames == 30) {
            if (orig_DBFH) {
                orig_DBFH(pgc, nullptr);
                g_ghost.savedCueBallX = s_bihPlacedX;
                g_ghost.savedCueBallY = s_bihPlacedZ;
                g_ghost.hasSavedCueBallPos = true;
            }
        }
        else if (g_ghost.botAimFrames == 45 && !g_ghost.botIsBreak) {
            
            int idx = g_ghost.botBIHAttempt;
            if (idx >= BIH_MAX_POSITIONS) idx = 0;
            float cx = BIH_POSITIONS[idx][0];
            float cz = BIH_POSITIONS[idx][1];
            
            float dx = -cx;
            float dz = -cz;
            float dist = sqrtf(dx*dx + dz*dz);
            if (dist < 0.01f) { dx = 1.0f; dz = 0.0f; }
            else { dx /= dist; dz /= dist; }
            g_ghost.currentAimDirX = dx;
            g_ghost.currentAimDirY = dz;
            g_ghost.sliderForceValue = 0.5f;
            g_ghost.currentAimForce = 0.5f;
            g_ghost.angleOverride = true;
            g_ghost.realForceEnabled = true;
            
            triggerGhostSimulation();
            pushToOverlay();
        }
        else if (g_ghost.botAimFrames >= 60) {
            g_ghost.bihActive = false;

            if (g_ghost.botIsBreak) {
                
                g_ghost.currentAimDirX = BREAK_DIR_X;
                g_ghost.currentAimDirY = BREAK_DIR_Z;
                g_ghost.sliderForceValue = BREAK_FORCE;
                g_ghost.currentAimForce = BREAK_FORCE;
                g_ghost.currentAimSpinX = BREAK_SPIN_X;
                g_ghost.currentAimSpinY = BREAK_SPIN_Z;
                g_ghost.angleOverride = true;
                g_ghost.realForceEnabled = true;
                g_ghost.lastAimDirX = -999.f;
                g_ghost.lastAimForce = -999.f;
                g_ghost.lastSliderForce = -999.f;
                g_ghost.botAimFrames = 0;
                g_ghost.botFoundShot = true;
                g_ghost.botState = GhostState::BOT_AIMING;
                g_ghost.botStateStartMs = now;
            } else {
                
                g_ghost.botState = GhostState::BOT_SCANNING;
                g_ghost.botStateStartMs = now;
                g_ghost.botFoundShot = false;
                s_scanStep = 0;
                s_scanForceIdx = 0;
                s_scanDone = false;
                g_ghost.botScanAttempt = 0;
                g_ghost.hasBallPositions = false;
                s_smartIdx = 0;
                s_validShots.clear();
            }
        }
        return;
    }

    
    case GhostState::BOT_SCANNING: {
        if (g_ghost.botScanAttempt == 0) {
            
            if (!g_ghost.hasBallPositions) {
                g_ghost.currentAimDirX = 0.0f;
                g_ghost.currentAimDirY = 1.0f;
                g_ghost.sliderForceValue = 0.5f;
                g_ghost.currentAimForce = 0.5f;
                performGhostSimulation();
                if (!g_ghost.hasBallPositions) {
                    return;
                }
                buildSmartCandidates();
                s_smartIdx = 0;
            }

            
            if (s_smartCandidates.empty()) {
                g_ghost.botScanAttempt = 1;
                s_scanStep = 0;
                s_scanForceIdx = 0;
                s_scanDone = false;
                return;
            }

            for (int sim = 0; sim < BOT_SIMS_PER_FRAME && s_smartIdx < (int)s_smartCandidates.size(); sim++, s_smartIdx++) {
                auto& cand = s_smartCandidates[s_smartIdx];
                float theta = cand.angle;
                float force = cand.force;

                g_ghost.currentAimDirX = sinf(theta);
                g_ghost.currentAimDirY = cosf(theta);
                g_ghost.sliderForceValue = force;
                g_ghost.currentAimForce = force;
                performGhostSimulation();

                std::lock_guard<std::mutex> lk(trajectoryMutex);
                if (botValidatePocketedBalls(cachedWillPocket)) {
                    ValidShot vs;
                    vs.angle = theta;
                    vs.force = force;
                    vs.ballPocketed = cachedWillPocket.empty() ? -1 : cachedWillPocket[0];
                    vs.pocketIdx = -1;
                    for (size_t pi = 0; pi < cachedWillPocket.size() && pi < cachedPocketIndices.size(); pi++) {
                        if (cachedWillPocket[pi] != 0) {
                            vs.pocketIdx = cachedPocketIndices[pi];
                            break;
                        }
                    }
                    vs.cueFinalX = cachedCueFinalX;
                    vs.cueFinalZ = cachedCueFinalZ;
                    vs.ball8FinalX = cachedBall8FinalX;
                    vs.ball8FinalZ = cachedBall8FinalZ;
                    vs.ball8Moved = cachedBall8Moved;
                    memcpy(vs.allBallFinals, cachedAllBallFinals, sizeof(vs.allBallFinals));
                    memcpy(vs.allBallMoved, cachedAllBallMoved, sizeof(vs.allBallMoved));
                    vs.firstHitBall = cachedFirstHitBall;
                    vs.strategicScore = 0.f;
                    s_validShots.push_back(vs);
                }
            }

            
            if (s_smartIdx >= (int)s_smartCandidates.size()) {
                if (!s_validShots.empty()) {
                    
                    for (auto& vs : s_validShots) {
                        vs.strategicScore = scoreShot(vs);
                    }
                    
                    auto& best = *std::max_element(s_validShots.begin(), s_validShots.end(),
                        [](const ValidShot& a, const ValidShot& b) { return a.strategicScore < b.strategicScore; });

                    g_ghost.botBestAngle = best.angle;
                    g_ghost.botBestForce = best.force;
                    g_ghost.currentAimSpinX = 0.f;
                    g_ghost.currentAimSpinY = 0.f;
                    g_ghost.botBestPocket = best.ballPocketed;
                    g_ghost.botBestPocketIdx = best.pocketIdx;
                    g_ghost.botFoundShot = true;
                    s_scanDone = true;

                } else {
                    g_ghost.botScanAttempt = 1;
                    s_scanStep = 0;
                    s_scanForceIdx = 0;
                    s_scanDone = false;
                    return;
                }
            }
        }
        

        if (s_scanDone) {
            if (g_ghost.botFoundShot) {
                float theta = g_ghost.botBestAngle;
                g_ghost.currentAimDirX = sinf(theta);
                g_ghost.currentAimDirY = cosf(theta);
                g_ghost.sliderForceValue = g_ghost.botBestForce;
                g_ghost.currentAimForce = g_ghost.botBestForce;
                g_ghost.angleOverride = true;
                g_ghost.realForceEnabled = true;
                
                g_ghost.lastAimDirX = -999.f;
                g_ghost.lastAimForce = -999.f;
                g_ghost.lastSliderForce = -999.f;
                g_ghost.botAimFrames = 0;
                g_ghost.botState = GhostState::BOT_AIMING;
                g_ghost.botStateStartMs = now;
            } else {
                
                
                if (g_ghost.botBIHAttempt >= 0 && g_ghost.botBIHAttempt < 3 && pgc && g_TakeBIH) {
                    g_TakeBIH(pgc, nullptr);
                    g_ghost.bihActive = true;
                    g_ghost.botState = GhostState::BOT_PLACE_BALL;
                    g_ghost.botStateStartMs = now;
                    g_ghost.botAimFrames = 0;
                    g_ghost.botBIHAttempt++;
                    return;
                }
                
                if (g_ghost.botBIHAttempt >= 0) {
                    float theta = (float)(rand() % 628) / 100.0f;
                    g_ghost.currentAimDirX = sinf(theta);
                    g_ghost.currentAimDirY = cosf(theta);
                    g_ghost.sliderForceValue = 0.90f;
                    g_ghost.currentAimForce = 0.90f;
                    g_ghost.angleOverride = true;
                    g_ghost.realForceEnabled = true;
                    g_ghost.lastAimDirX = -999.f;
                    g_ghost.lastAimForce = -999.f;
                    g_ghost.lastSliderForce = -999.f;
                    g_ghost.botAimFrames = 0;
                    g_ghost.botState = GhostState::BOT_AIMING;
                    g_ghost.botStateStartMs = now;
                    return;
                }
                
                if (g_ghost.botScanAttempt == 1) {
                    g_ghost.botScanAttempt = 2;
                    g_ghost.botState = GhostState::BOT_SCANNING;
                    g_ghost.botStateStartMs = now;
                    g_ghost.botFoundShot = false;
                    s_scanStep = 0;
                    s_scanForceIdx = 0;
                    s_scanDone = false;
                    return;
                }
                
                g_ghost.angleOverride = false;
                g_ghost.botState = GhostState::BOT_OFF;
                return;
            }
            return;
        }

        
        
        
        
        bool isBIH = (g_ghost.botBIHAttempt >= 0);
        bool isPowerScan = (!isBIH && g_ghost.botScanAttempt >= 2);
        int scanSteps = isPowerScan ? BOT_POWER_SCAN_STEPS : BOT_SCAN_STEPS;
        int maxForces = (isBIH || isPowerScan) ? 1 : BOT_SCAN_FORCES_N;
        float bihForce = BOT_POWER_FORCE;  

        int simsThisFrame = 0;
        for (int sim = 0; sim < BOT_SIMS_PER_FRAME && !s_scanDone; sim++) {
            if (s_scanStep >= scanSteps) {
                s_scanForceIdx++;
                s_scanStep = 0;
                if (s_scanForceIdx >= maxForces) {
                    s_scanDone = true;
                    break;
                }
            }

            float theta = (float)s_scanStep * (2.0f * 3.14159265f / (float)scanSteps);
            float force = isBIH ? bihForce : (isPowerScan ? BOT_POWER_FORCE : BOT_SCAN_FORCES[s_scanForceIdx]);

            
            g_ghost.currentAimDirX = sinf(theta);
            g_ghost.currentAimDirY = cosf(theta);
            g_ghost.sliderForceValue = force;
            g_ghost.currentAimForce = force;
            g_ghost.currentAimSpinX = 0.f;
            g_ghost.currentAimSpinY = 0.f;

            
            performGhostSimulation();
            simsThisFrame++;

            
            {
                std::lock_guard<std::mutex> lk(trajectoryMutex);

                if (botValidatePocketedBalls(cachedWillPocket)) {
                    g_ghost.botBestAngle = theta;
                    g_ghost.botBestForce = force;
                    g_ghost.botBestPocket = cachedWillPocket.empty() ? -1 : cachedWillPocket[0];
                    g_ghost.botBestPocketIdx = -1;
                    for (size_t pi = 0; pi < cachedWillPocket.size() && pi < cachedPocketIndices.size(); pi++) {
                        if (cachedWillPocket[pi] != 0) {
                            g_ghost.botBestPocketIdx = cachedPocketIndices[pi];
                            break;
                        }
                    }
                    g_ghost.botFoundShot = true;
                    s_scanDone = true;
                    break;
                }
            }

            s_scanStep++;
        }

        
        if (now - g_ghost.botStateStartMs > BOT_SCAN_TIMEOUT && !s_scanDone) {
            s_scanDone = true;
        }
        return;
    }

    
    case GhostState::BOT_AIMING: {
        g_ghost.botAimFrames++;

        if (cue && g_ghost.angleOverride) {
            if (g_ghost.botHumanMode) {
                applyHumanFinalAimMotion(cue, g_ghost.botAimFrames);
            } else {
                setVisibleCueDirection(cue, g_ghost.currentAimDirX, g_ghost.currentAimDirY);
            }
        }

        const int aimFrames = g_ghost.botHumanMode ? humanAimFrameLimit() : BOT_AIM_FRAMES;
        if (g_ghost.botAimFrames >= aimFrames) {
            
            void* gmNom = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
            bool shouldCall = false;
            bool needsNomination = false;
            int calledPocket = -1;

            if (gmNom) {
                shouldCall = *(bool*)((uint8_t*)gmNom + OFF_GM_ShouldCallPocket);
                calledPocket = *(int32_t*)((uint8_t*)gmNom + OFF_GM_CalledPocket);

                
                
                void* sl = *(void**)((uint8_t*)gmNom + OFF_GM_RemainingSolids);
                void* st = *(void**)((uint8_t*)gmNom + OFF_GM_RemainingStripes);
                bool ownsSolids = *(bool*)((uint8_t*)gmNom + OFF_GM_MyPlayerOwnsSolids);
                int myRem = ownsSolids ? getListCount(sl) : getListCount(st);

                
                
                needsNomination = shouldCall || (myRem == 0);

            }

            if (needsNomination && g_ghost.botBestPocketIdx >= 0) {
                g_ghost.botState = GhostState::BOT_NOMINATING;
                g_ghost.botStateStartMs = now;
                g_ghost.botAimFrames = 0;
            } else {
                g_ghost.botState = GhostState::BOT_SHOOTING;
                g_ghost.botStateStartMs = now;
                g_ghost.botAimFrames = 0; // starts the visual cue-pull sequence
            }
        }
        return;
    }

    
    case GhostState::BOT_NOMINATING: {
        g_ghost.botAimFrames++;

        if (cue && g_ghost.angleOverride) {
            // After the pocket call the cue settles precisely on the same shot.
            if (g_ghost.botHumanMode && g_ghost.botAimFrames < 18) {
                applyHumanFinalAimMotion(cue, g_ghost.botAimFrames + 8);
            } else {
                setVisibleCueDirection(cue, g_ghost.currentAimDirX, g_ghost.currentAimDirY);
            }
        }

        
        if (g_ghost.botAimFrames == 5) {
            if (pgc && orig_PocketSelected) {
                int pIdx = g_ghost.botBestPocketIdx;
                
                if (pIdx < 0) {
                    
                    performGhostSimulation();
                    {
                        std::lock_guard<std::mutex> lk(trajectoryMutex);
                        for (size_t i = 0; i < cachedWillPocket.size() && i < cachedPocketIndices.size(); i++) {
                            if (cachedWillPocket[i] != 0 && cachedPocketIndices[i] >= 0) {
                                pIdx = cachedPocketIndices[i];
                                break;
                            }
                        }
                    }
                }
                
                if (pIdx < 201) {
                    pIdx = 201;
                }
                orig_PocketSelected(pgc, pIdx, nullptr);
                
                void* gmCheck = g_GM_getInstance ? g_GM_getInstance(nullptr) : nullptr;
                if (gmCheck) {
                    bool scAfter = *(bool*)((uint8_t*)gmCheck + OFF_GM_ShouldCallPocket);
                    int cpAfter = *(int32_t*)((uint8_t*)gmCheck + OFF_GM_CalledPocket);
                }
            }
        }

        
        if (g_ghost.botAimFrames >= 35) {
            g_ghost.botState = GhostState::BOT_SHOOTING;
            g_ghost.botStateStartMs = now;
            g_ghost.botAimFrames = 0; // starts the visual cue-pull sequence
        }
        return;
    }

    
    case GhostState::BOT_SHOOTING: {
        if (g_ghost.botShotFired) return;  

        if (!pgc) {
            g_ghost.botState = GhostState::BOT_OFF;
            return;
        }

        
        if (cue) {
            convergeAimToTarget(g_ghost.currentAimDirX, g_ghost.currentAimDirY);
            *(float*)((uint8_t*)cue + 0x64) = g_ghost.currentAimDirX;
            *(float*)((uint8_t*)cue + 0x6C) = g_ghost.currentAimDirY;
        }

        
        void* pbc = *(void**)((uint8_t*)pgc + OFF_PGC_PowerBarCtrl);
        // Visual-only cue pull: the original chosen direction and force remain untouched.
        g_ghost.botAimFrames++;
        const float shotPower = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
        // Keep the proven GOATREADY pull timing: it gives the game enough
        // visible drag frames before the original shot is released.
        const int settleFrames = 5;
        const int pullFrames = 18;
        const int holdFrames = 12;
        float visualPower = 0.05f;
        if (g_ghost.botAimFrames > settleFrames) {
            float t = (float)(g_ghost.botAimFrames - settleFrames) / (float)pullFrames;
            if (t > 1.f) t = 1.f;
            float ease = t * t * (3.0f - 2.0f * t);
            visualPower = fmaxf(0.05f, shotPower * ease);
        }
        if (pbc) {
            *(float*)((uint8_t*)pbc + 0x58) = visualPower;
            if (orig_PBD && visualPower > 0.051f) {
                orig_PBD(pgc, nullptr);
            }
        }
        if (g_ghost.botAimFrames < settleFrames + pullFrames + holdFrames) {
            return;
        }

        if (pbc) {
            *(float*)((uint8_t*)pbc + 0x58) = shotPower;
            if (orig_PBD) {
                orig_PBD(pgc, nullptr);
            }
        }
        float power = shotPower;

        
        if (g_PGC_Shoot) {
            g_ghost.botShotFired = true;
            g_ghost.localShotPending = true;
            g_ghost.botLastShotMs = now;  
            g_PGC_Shoot(pgc, power, nullptr);
            g_ghost.animating = true;
            g_ghost.bihDroppedThisTurn = false;  
            g_ghost.botState = GhostState::BOT_ANIMATING;
            g_ghost.botStateStartMs = now;
        } else {
            g_ghost.botState = GhostState::BOT_OFF;
        }
        return;
    }

    
    case GhostState::BOT_ANIMATING:
        
        if (now - g_ghost.botStateStartMs > 30000) {
            g_ghost.botState = GhostState::BOT_OFF;
            g_ghost.animating = false;
        }
        return;

    
    case GhostState::BOT_GAME_OVER:
        return;

    default:
        return;
    }
}


typedef void (*CCUpdate_t)(void* cue, void* method);
static CCUpdate_t orig_CCUpdate = nullptr;

// Solitaire uses a separate UI/controller path from Pool.  Capturing its own
// Update instance keeps the new solver entirely outside the Pool game loop.
typedef void (*SolitaireUIUpdate_t)(void* ui, void* method);
static SolitaireUIUpdate_t orig_SolitaireUIUpdate = nullptr;
typedef void (*SolitaireUIAwake_t)(void* ui, void* method);
static SolitaireUIAwake_t orig_SolitaireUIAwake = nullptr;
typedef void (*SolitaireRaycastUpdate_t)(void* controller, void* method);
static SolitaireRaycastUpdate_t orig_SolitaireRaycastUpdate = nullptr;

static void my_SolitaireUIUpdate(void* ui, void* method) {
    if (orig_SolitaireUIUpdate) orig_SolitaireUIUpdate(ui, method);
    solitaire_capture_ui(ui);
    solitaire_tick();
}

static void my_SolitaireUIAwake(void* ui, void* method) {
    if (orig_SolitaireUIAwake) orig_SolitaireUIAwake(ui, method);
    solitaire_capture_ui(ui);
}

static void my_SolitaireRaycastUpdate(void* controller, void* method) {
    if (orig_SolitaireRaycastUpdate) orig_SolitaireRaycastUpdate(controller, method);
    solitaire_tick();
}

// UNO owns a completely separate UI and popup lifecycle. These hooks only
// retain scene instances; all decision logic remains in uno_auto.cpp.
typedef void (*UnoAwake_t)(void* object, void* method);
static UnoAwake_t orig_UnoGameUIAwake = nullptr;
static UnoAwake_t orig_UnoColorPopupAwake = nullptr;
static UnoAwake_t orig_UnoDrawPopupAwake = nullptr;
static UnoAwake_t orig_UnoGameUIChange = nullptr;
static UnoAwake_t orig_UnoPlusUIChange = nullptr;
static UnoAwake_t orig_UnoHeartbeatUpdate = nullptr;

static void my_UnoGameUIAwake(void* ui, void* method) {
    if (orig_UnoGameUIAwake) orig_UnoGameUIAwake(ui, method);
    uno_capture_ui(ui);
    uno_tick();
}

static void my_UnoColorPopupAwake(void* popup, void* method) {
    if (orig_UnoColorPopupAwake) orig_UnoColorPopupAwake(popup, method);
    uno_capture_color_popup(popup);
}

static void my_UnoDrawPopupAwake(void* popup, void* method) {
    if (orig_UnoDrawPopupAwake) orig_UnoDrawPopupAwake(popup, method);
    uno_capture_draw_popup(popup);
}

static void my_UnoGameUIChange(void* ui, void* method) {
    if (orig_UnoGameUIChange) orig_UnoGameUIChange(ui, method);
    uno_capture_ui(ui);
    uno_tick();
}

static void my_UnoPlusUIChange(void* ui, void* method) {
    if (orig_UnoPlusUIChange) orig_UnoPlusUIChange(ui, method);
    uno_capture_ui(ui);
    uno_tick();
}

// RaycastInputController.Update is the card-scene heartbeat used by both UNO
// and UNO Plus (V28.2.90 RVA verified in IL2CPP metadata).  Calling the solver
// after the original Update gives delayed actions, popup handling and idle
// recovery a guaranteed UnityMain frame without any background/native thread.
static void my_UnoHeartbeatUpdate(void* controller, void* method) {
    if (orig_UnoHeartbeatUpdate) orig_UnoHeartbeatUpdate(controller, method);
    if (uno_is_enabled()) uno_tick();
}

static void my_CCUpdate(void* cue, void* method) {
    const bool poolAllowed = license_security_allows_game(GOAT_GAME_POOL);
    if (!poolAllowed) disableUnlicensedPoolRuntime();

    if (poolAllowed && !ghostCallActive && !g_ghost.animating
        && g_ghost.realForceEnabled && cue) {
        if (g_ghost.botHumanMode && g_ghost.botState == GhostState::BOT_SCANNING) {
            applyHumanSearchMotion(cue, nowMs());
        } else if (g_ghost.angleOverride) {
            setVisibleCueDirection(cue, g_ghost.currentAimDirX, g_ghost.currentAimDirY);

            
            *(float*)((uint8_t*)cue + 0x70) = g_ghost.currentAimSpinX;
            *(float*)((uint8_t*)cue + 0x74) = g_ghost.currentAimSpinY;
        }
        
        if (g_ghost.lastPoolGameController) {
            void* pbc = *(void**)((uint8_t*)g_ghost.lastPoolGameController + 0x60);
            if (pbc) {
                float f = fmaxf(0.05f, fminf(1.0f, g_ghost.sliderForceValue));
                *(float*)((uint8_t*)pbc + 0x58) = f;
            }
        }
    }
    if (orig_CCUpdate) orig_CCUpdate(cue, method);

    if (!poolAllowed) return;

    
    if (g_ghost.botEnabled && g_ghost.botState != GhostState::BOT_OFF) {
        botUpdate(cue);
    }

    triggerGhostSimulation();
    pushToOverlay();
}


static void install_hooks(uintptr_t base) {
    if (!base) return;

    // JNI_OnLoad and the loader-wait thread are mutually exclusive in the
    // normal path, but keep installation idempotent in case Android publishes
    // libil2cpp between the two checks. Dobby must never patch the same entry
    // point twice.
    uintptr_t expectedBase = 0;
    if (!g_installedIl2CppBase.compare_exchange_strong(
            expectedBase, base, std::memory_order_acq_rel)) {
        GOAT_NATIVE_LOG("[HOOK_INIT] ignored duplicate requested=%p installed=%p",
                        reinterpret_cast<void*>(base),
                        reinterpret_cast<void*>(expectedBase));
        return;
    }

    GOAT_NATIVE_LOG("[HOOK_INIT] resolved linker base=%p",
                    reinterpret_cast<void*>(base));
    g_il2cppBase = base;
    solitaire_init(base);
    uno_init(base);

    
    g_il2cpp_array_new = reinterpret_cast<il2cpp_array_new_t>(
            goat_resolve_il2cpp_export(base, "il2cpp_array_new"));
    g_il2cpp_gchandle_new = reinterpret_cast<il2cpp_gchandle_new_t>(
            goat_resolve_il2cpp_export(base, "il2cpp_gchandle_new"));
    g_Camera_get_main         = (GetMainCamera_t)     (base + RVA_Camera_get_main);
    g_Screen_get_width        = (Screen_getDim_t)     (base + RVA_Screen_get_width);
    g_Screen_get_height       = (Screen_getDim_t)     (base + RVA_Screen_get_height);
    g_get_worldToCameraMatrix = (GetMatrixInjected_t) (base + RVA_get_worldToCameraMatrix_Injected);
    g_get_projectionMatrix    = (GetMatrixInjected_t) (base + RVA_get_projectionMatrix_Injected);

    
    g_GetCuePower       = (GetCueFloat_t)(base + RVA_GetCuePower);
    g_GetCueSpin        = (GetCueFloat_t)(base + RVA_GetCueSpin);
    g_GetMaxMasseAngle  = (GetCueFloat_t)(base + RVA_GetMaxMasseAngle);

    
    g_IsMyTurn = (GameStore_IsMyTurn_t)(base + RVA_GameStore_IsMyTurn);
    g_MoveBIHInstantly = (MoveBIHInstantly_t)(base + RVA_MoveBallInHandInstantly);
    g_PGC_Shoot = (PGC_Shoot_t)(base + RVA_PGC_Shoot);
    g_GM_getInstance = (GM_getInstance_t)(base + RVA_GM_get_Instance);
    g_TakeBIH = (TakeBIH_t)(base + RVA_TakeBallInHand);
    g_TakeBIH = (TakeBIH_t)(base + RVA_TakeBallInHand);

    
    orig_RustBridgeSim = (RustBridgeSim_t)(base + RVA_RustBridgeSimulatePool);

    
    
    int hookFailures = 0;
    #define HOOK(rva, my_fn, orig_ptr) do { \
        void* t = (void*)(base + (rva)); \
        int r = DobbyHook(t, (void*)(my_fn), (void**)(orig_ptr)); \
        if (r != 0) { \
            uint32_t firstInstr = *(uint32_t*)t; \
            if (firstInstr == 0xD503245F) { \
                void* t4 = (void*)(base + (rva) + 4); \
                r = DobbyHook(t4, (void*)(my_fn), (void**)(orig_ptr)); \
 \
            } else { \
 \
            } \
        } \
        if (r != 0) { \
            ++hookFailures; \
            GOAT_NATIVE_LOG("[HOOK_FAIL] rva=0x%lx result=%d", \
                            static_cast<unsigned long>(rva), r); \
        } \
    } while(0)

    HOOK(RVA_GameStore_get_GameID,     my_GameStoreGetGID,  &orig_GameStoreGetGID);
    HOOK(RVA_GameStore_IsMyTurn,       my_GameStoreIsMyTurn,&orig_GameStoreIsMyTurn);
    // All UNO IsMyTurn checks must call Dobby's original trampoline rather
    // than the hooked scheduler entry point.
    uno_set_is_my_turn_original(orig_GameStoreIsMyTurn);
    HOOK(RVA_GameStore_Write,          my_GameStoreWrite,   &orig_GameStoreWrite);
    HOOK(RVA_RustBridgeSimulatePool,   my_RustBridgeSim,    &orig_RustBridgeSim);
    HOOK(RVA_SetLastLocalShotInfo,     my_SLSI,             &orig_SLSI);
    HOOK(RVA_PoolGC_RotateCue,         my_RotateCue,        &orig_RotateCue);
    HOOK(RVA_PoolGC_ApplySpin,         my_ApplySpin,        &orig_ApplySpin);
    HOOK(RVA_PowerBarDragged,          my_PBD,              &orig_PBD);
    HOOK(RVA_BIH_LateUpdate,           my_BIHL,             &orig_BIHL);
    HOOK(RVA_DropBallFromHand,         my_DBFH,             &orig_DBFH);
    
    HOOK(RVA_CueController_Shoot, my_CCShootStart,     &orig_CCShootStart);
    HOOK(RVA_FinishAnimating,          my_FinishAnim,       &orig_FinishAnim);
    HOOK(RVA_CueController_Update,     my_CCUpdate,         &orig_CCUpdate);

    // Jawaker.SolitaireUIController.Update, verified from the installed
    // V28.2.90 metadata. It is dormant outside Solitaire.
    HOOK(0x049BC538UL,                 my_SolitaireUIUpdate,&orig_SolitaireUIUpdate);
    HOOK(0x049BC1F8UL,                 my_SolitaireUIAwake, &orig_SolitaireUIAwake);
    HOOK(0x049B538CUL,                 my_SolitaireRaycastUpdate, &orig_SolitaireRaycastUpdate);

    // Jawaker UNO V28.2.90. They are dormant in every non-UNO scene.
    HOOK(0x04CE8CD8UL,                 my_UnoGameUIAwake, &orig_UnoGameUIAwake);
    HOOK(0x04CE80A8UL,                 my_UnoColorPopupAwake, &orig_UnoColorPopupAwake);
    HOOK(0x04CE87A4UL,                 my_UnoDrawPopupAwake, &orig_UnoDrawPopupAwake);
    HOOK(0x04CE96ACUL,                 my_UnoGameUIChange, &orig_UnoGameUIChange);
    HOOK(0x04CF0044UL,                 my_UnoPlusUIChange, &orig_UnoPlusUIChange);
    HOOK(0x04ECAAC4UL,                 my_UnoHeartbeatUpdate, &orig_UnoHeartbeatUpdate);

    
    HOOK(RVA_OnTurnChanged,            my_OnTurnChanged,    &orig_OnTurnChanged);
    HOOK(RVA_GameOver,                 my_GameOver,         &orig_GameOver);
    HOOK(RVA_PocketSelected,           my_PocketSelected,   &orig_PocketSelected);

    GOAT_NATIVE_LOG("[HOOK_READY] base=%p failures=%d",
                    reinterpret_cast<void*>(base), hookFailures);

    #undef HOOK

}


static void hookingThread() {
    uintptr_t base = 0;
    uintptr_t observedBase = 0;
    int stablePolls = 0;
    for (int i = 0; i < 6000 && !base; i++) {
        const uintptr_t current = get_module_base("libil2cpp.so");
        if (current && current == observedBase) {
            if (++stablePolls >= 20) base = current;
        } else {
            observedBase = current;
            stablePolls = current ? 1 : 0;
        }
        if (!base) usleep(10000);
    }
    if (!base) {  return; }
    install_hooks(base);
}


static jclass g_overlayClassGlobal = nullptr;

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    g_jvm = vm;
    overlay_init(vm);

    
    jclass overlayLocal = env->FindClass("com/google/firebase/TrainerOverlayView");
    if (overlayLocal) {
        g_overlayClassGlobal = (jclass)env->NewGlobalRef(overlayLocal);
        env->DeleteLocalRef(overlayLocal);
        JNINativeMethod methods[] = {
            {"nativeOnDraw", "(Landroid/view/View;Landroid/graphics/Canvas;[F)V",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeOnDraw},
            {"nativeOnClick", "(I)V",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeOnClick},
            {"nativeOnForceChanged", "(I)V",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeOnForceChanged},
            {"nativeCreatePanel", "(Landroid/app/Activity;Lcom/google/firebase/TrainerOverlayView;)V",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeCreatePanel},
            {"nativeOnTouch", "(IFFI)Z",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeOnTouch},
            {"nativeSetPerformanceProfile", "(IIIIII)V",
             (void*)Java_com_google_firebase_TrainerOverlayView_nativeSetPerformanceProfile},
        };
        const jint registerResult = env->RegisterNatives(g_overlayClassGlobal, methods, 6);
        GOAT_NATIVE_LOG("[JNI_BRIDGE] overlay class resolved; RegisterNatives=%d",
                        static_cast<int>(registerResult));
    } else {
        GOAT_NATIVE_LOG("[JNI_BRIDGE] TrainerOverlayView was not visible to the loading class loader");
        env->ExceptionClear();
    }

    
    uintptr_t base = get_module_base("libil2cpp.so");
    if (base) install_hooks(base);
    else std::thread(hookingThread).detach();

    return JNI_VERSION_1_6;
}
