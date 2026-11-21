#include <jni.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <math.h>
#include <stdio.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include "include/ghost_sim.h"
#include "include/solitaire_auto.h"
#include "include/uno_auto.h"
#include "include/license.h"
#include "include/license_security.h"

extern std::mutex          trajectoryMutex;
extern std::vector<float>  cachedTrajectoryScreenCoords;
extern bool                hasNewTrajectory;
extern std::vector<int>    cachedWillPocket;
static JavaVM*   g_jvm        = nullptr;
static jobject   g_overlayRef = nullptr;
static jobject   g_drawPaint  = nullptr;
static jmethodID g_setColor   = nullptr;
static jmethodID g_setStyle   = nullptr;
static jmethodID g_setStrokeW = nullptr;
static jmethodID g_setStrokeCap = nullptr;
static jmethodID g_setAA      = nullptr;
static jmethodID g_drawLine   = nullptr;
static jmethodID g_drawCircle = nullptr;
static jmethodID g_drawText   = nullptr;
static jmethodID g_drawRRect  = nullptr;
static jmethodID g_setTextSize = nullptr;
static jmethodID g_measureText = nullptr;
static jobject   g_strokeStyle = nullptr;
static jobject   g_fillStyle   = nullptr;
static bool      g_paintInited = false;
static void updateExpiryLabelText(JNIEnv* env);
static void setBtnActive(JNIEnv* env, jobject btn, bool active, jint inactiveColor);
static void applyEntitlementUi(JNIEnv* env);

static float mixf(float a, float b, float t) {
    return a + (b - a) * t;
}

static void drawNeonSegment(JNIEnv* env, jobject canvas,
                            float x1, float y1, float x2, float y2,
                            uint32_t baseColor) {
    const jint shadow = (jint)0x24000000;
    const jint glow   = (jint)((baseColor & 0x00FFFFFF) | 0x46000000);
    const jint main   = (jint)((baseColor & 0x00FFFFFF) | 0xFF000000);

    env->CallVoidMethod(g_drawPaint, g_setStyle, g_strokeStyle);
    env->CallVoidMethod(g_drawPaint, g_setColor, shadow);
    env->CallVoidMethod(g_drawPaint, g_setStrokeW, 5.20f);
    env->CallVoidMethod(canvas, g_drawLine, x1, y1, x2, y2, g_drawPaint);

    env->CallVoidMethod(g_drawPaint, g_setColor, glow);
    env->CallVoidMethod(g_drawPaint, g_setStrokeW, 4.05f);
    env->CallVoidMethod(canvas, g_drawLine, x1, y1, x2, y2, g_drawPaint);

    env->CallVoidMethod(g_drawPaint, g_setColor, main);
    env->CallVoidMethod(g_drawPaint, g_setStrokeW, 3.05f);
    env->CallVoidMethod(canvas, g_drawLine, x1, y1, x2, y2, g_drawPaint);
}

static void drawSmoothNeonPath(JNIEnv* env, jobject canvas,
                               const std::vector<std::pair<float,float>>& pts,
                               uint32_t baseColor) {
    if (pts.size() < 2) return;
    for (size_t idx = 1; idx < pts.size(); idx++) {
        float x0 = pts[idx - 1].first;
        float y0 = pts[idx - 1].second;
        float x1 = pts[idx].first;
        float y1 = pts[idx].second;
        drawNeonSegment(env, canvas, x0, y0, x1, y1, baseColor);
    }
}

void overlay_init(JavaVM* jvm) { g_jvm = jvm; }

void overlay_setView(JNIEnv* env, jobject view) {
    if (g_overlayRef) env->DeleteGlobalRef(g_overlayRef);
    g_overlayRef = env->NewGlobalRef(view);
}

void overlay_requestRedraw() {
    if (!g_jvm || !g_overlayRef) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (env) {
        jclass cls = env->GetObjectClass(g_overlayRef);
        jmethodID m = env->GetMethodID(cls, "postInvalidate", "()V");
        if (m) env->CallVoidMethod(g_overlayRef, m);
        env->DeleteLocalRef(cls);
    }
    if (attached) g_jvm->DetachCurrentThread();
}

void overlay_updateCoords(JNIEnv* env, const std::vector<float>& coords,
                          const std::vector<int>& pocketed) {
    if (!env || !g_overlayRef || coords.empty()) return;
    jfloatArray arr = env->NewFloatArray((jsize)coords.size());
    if (!arr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    env->SetFloatArrayRegion(arr, 0, (jsize)coords.size(), coords.data());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(arr);
        return;
    }

    // The loader owns TrainerOverlayView through a child BridgeDexClassLoader.
    // FindClass() from Unity's native thread therefore cannot resolve it even
    // though the actual View is alive. Resolve the exact loaded class from the
    // global View instance instead, so cachedCoords is updated on first launch.
    jclass cls = env->GetObjectClass(g_overlayRef);
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(arr);
        return;
    }
    jfieldID fid = env->GetStaticFieldID(cls, "cachedCoords", "[F");
    if (fid) {
        env->SetStaticObjectField(cls, fid, arr);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(arr);
}

static uint32_t getBallColor(int b) {
    static const uint32_t c[] = {
        0xFFF5F5F0,
        0xFFE8C800,
        0xFF1B47A5,
        0xFFCC2222,
        0xFF5B2D8E,
        0xFFE86B10,
        0xFF1B7A3D,
        0xFF7B1818,
        0xFF1A1A1A,
        0xFFE8C800,
        0xFF1B47A5,
        0xFFCC2222,
        0xFF5B2D8E,
        0xFFE86B10,
        0xFF1B7A3D,
        0xFF7B1818,
    };
    if (b < 0 || b > 15) return 0xFFFF00FF;
    return c[b];
}

static void initPaint(JNIEnv* env) {
    jclass paintCls = env->FindClass("android/graphics/Paint");
    jclass styleCls = env->FindClass("android/graphics/Paint$Style");
    jclass capCls   = env->FindClass("android/graphics/Paint$Cap");

    jmethodID ctor = env->GetMethodID(paintCls, "<init>", "()V");
    jobject paint = env->NewObject(paintCls, ctor);

    g_setColor    = env->GetMethodID(paintCls, "setColor", "(I)V");
    g_setStyle    = env->GetMethodID(paintCls, "setStyle", "(Landroid/graphics/Paint$Style;)V");
    g_setStrokeW  = env->GetMethodID(paintCls, "setStrokeWidth", "(F)V");
    g_setStrokeCap= env->GetMethodID(paintCls, "setStrokeCap", "(Landroid/graphics/Paint$Cap;)V");
    g_setAA       = env->GetMethodID(paintCls, "setAntiAlias", "(Z)V");

    env->CallVoidMethod(paint, g_setStrokeW, 4.0f);
    env->CallVoidMethod(paint, g_setAA, JNI_TRUE);

    jfieldID sf = env->GetStaticFieldID(styleCls, "STROKE", "Landroid/graphics/Paint$Style;");
    g_strokeStyle = env->NewGlobalRef(env->GetStaticObjectField(styleCls, sf));
    jfieldID ff = env->GetStaticFieldID(styleCls, "FILL", "Landroid/graphics/Paint$Style;");
    g_fillStyle = env->NewGlobalRef(env->GetStaticObjectField(styleCls, ff));

    env->CallVoidMethod(paint, g_setStyle, g_strokeStyle);
    jfieldID rf = env->GetStaticFieldID(capCls, "ROUND", "Landroid/graphics/Paint$Cap;");
    env->CallVoidMethod(paint, g_setStrokeCap, env->GetStaticObjectField(capCls, rf));

    g_drawPaint = env->NewGlobalRef(paint);

    jclass canvasCls = env->FindClass("android/graphics/Canvas");
    g_drawLine   = env->GetMethodID(canvasCls, "drawLine", "(FFFFLandroid/graphics/Paint;)V");
    g_drawCircle = env->GetMethodID(canvasCls, "drawCircle", "(FFFLandroid/graphics/Paint;)V");
    g_drawText   = env->GetMethodID(canvasCls, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    g_drawRRect  = env->GetMethodID(canvasCls, "drawRoundRect", "(FFFFFFLandroid/graphics/Paint;)V");
    g_setTextSize = env->GetMethodID(paintCls, "setTextSize", "(F)V");
    g_measureText = env->GetMethodID(paintCls, "measureText", "(Ljava/lang/String;)F");

    env->DeleteLocalRef(paint);
    env->DeleteLocalRef(paintCls);
    env->DeleteLocalRef(styleCls);
    env->DeleteLocalRef(capCls);
    env->DeleteLocalRef(canvasCls);
    g_paintInited = true;
}

void disableTrajectories() {
    solitaire_set_enabled(false);
    uno_set_enabled(false);
    g_ghost.botEnabled = false;
    g_ghost.botHumanMode = false;
    g_ghost.realForceEnabled = false;
    g_ghost.botState = GhostState::BOT_OFF;
    g_ghost.botShotFired = false;
    g_ghost.botFoundShot = false;
    g_ghost.angleOverride = false;
    g_ghost.localShotPending = false;
    g_ghost.localShotJustFired = false;
    g_ghost.powerBarActive = false;
    g_ghost.wasPowerBarActive = false;
    g_ghost.botVisiblePower = 0.0f;
    g_ghost.hideTrajectory = true;
    g_ghost.trajectoryHidden = true;
    g_licenseValidated.store(false);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnDraw(
        JNIEnv* env, jclass clz, jobject view, jobject canvas, jfloatArray points)
{
    license_checkPendingResult(env);
    license_enforce_runtime(env);
    updateExpiryLabelText(env);

    if (!g_overlayRef && view) {
        g_overlayRef = env->NewGlobalRef(view);
    }

    if (!license_security_allows_game(GOAT_GAME_POOL)) return;

    if (!canvas || !points) return;
    jsize len = env->GetArrayLength(points);
    if (len < 5) return;
    if (!g_paintInited) initPaint(env);

    jfloat* pts = env->GetFloatArrayElements(points, nullptr);
    if (!pts) return;
    std::vector<int> pocketedNow;
    {
        std::lock_guard<std::mutex> lk(trajectoryMutex);
        pocketedNow = cachedWillPocket;
    }

    if (!g_ghost.trajectoryHidden && !g_ghost.hideTrajectory) {
    jsize i = 0;
    while (i < len) {
        if (pts[i] == -9999.f && i + 2 < len) {
            int ballId = (int)pts[i + 1];
            float radius = pts[i + 2];
            if (ballId < 0 || ballId > 15) ballId = 0;
            i += 3;

            float finalX = -1, finalY = -1;
            uint32_t baseColor = getBallColor(ballId);
            std::vector<std::pair<float,float>> pathPts;
            pathPts.reserve(72);
            while (i + 1 < len && pts[i] != -9999.f && pts[i] != -8888.f) {
                float cx = pts[i], cy = pts[i+1];
                pathPts.emplace_back(cx, cy);
                finalX = cx; finalY = cy;
                i += 2;
            }

            drawSmoothNeonPath(env, canvas, pathPts, baseColor);

            bool showPocketEndpoint = false;
            for (int b : pocketedNow) {
                if (b == ballId && b > 0 && b <= 15) { showPocketEndpoint = true; break; }
            }

            if (finalX >= 0) {
                const bool striped = (ballId >= 9 && ballId <= 15);
                const float haloR = showPocketEndpoint ? fmaxf(radius * 1.58f, 14.5f)
                                                       : fmaxf(radius * 1.16f, 9.5f);
                const float ringR = showPocketEndpoint ? fmaxf(radius * 1.24f, 11.8f)
                                                       : fmaxf(radius * 0.92f, 8.0f);
                const float innerR = showPocketEndpoint ? fmaxf(radius * 0.24f, 3.2f)
                                                        : fmaxf(radius * 0.36f, 3.8f);

                env->CallVoidMethod(g_drawPaint, g_setStyle, g_strokeStyle);
                env->CallVoidMethod(g_drawPaint, g_setStrokeW, showPocketEndpoint ? 1.25f : 1.05f);
                env->CallVoidMethod(g_drawPaint, g_setColor, (jint)((baseColor & 0x00FFFFFF) | (showPocketEndpoint ? 0x62000000 : 0x52000000)));
                env->CallVoidMethod(canvas, g_drawCircle, finalX, finalY, haloR, g_drawPaint);

                env->CallVoidMethod(g_drawPaint, g_setStyle, g_strokeStyle);
                env->CallVoidMethod(g_drawPaint, g_setStrokeW, showPocketEndpoint ? 2.70f : 2.35f);
                env->CallVoidMethod(g_drawPaint, g_setColor, (jint)((baseColor & 0x00FFFFFF) | 0xFF000000));
                env->CallVoidMethod(canvas, g_drawCircle, finalX, finalY, ringR, g_drawPaint);

                if (!striped) {
                    env->CallVoidMethod(g_drawPaint, g_setStyle, g_fillStyle);
                    env->CallVoidMethod(g_drawPaint, g_setColor, (jint)((baseColor & 0x00FFFFFF) | (showPocketEndpoint ? 0xB8000000 : 0xF0000000)));
                    env->CallVoidMethod(canvas, g_drawCircle, finalX, finalY, innerR, g_drawPaint);
                } else {
                    env->CallVoidMethod(g_drawPaint, g_setStyle, g_strokeStyle);
                    env->CallVoidMethod(g_drawPaint, g_setStrokeW, showPocketEndpoint ? 1.85f : 1.65f);
                    env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0xFFF7F7F2);
                    env->CallVoidMethod(canvas, g_drawCircle, finalX, finalY, innerR * 1.22f, g_drawPaint);
                }

                if (showPocketEndpoint) {
                    env->CallVoidMethod(g_drawPaint, g_setStyle, g_fillStyle);
                    env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0x50FFFFFF);
                    env->CallVoidMethod(canvas, g_drawCircle,
                                        finalX - innerR * 0.35f,
                                        finalY - innerR * 0.35f,
                                        fmaxf(innerR * 0.36f, 2.5f),
                                        g_drawPaint);
                }
            }
        } else if (pts[i] == -8888.f && i + 3 < len) {
            float tgtSX = pts[i + 1];
            float tgtSY = pts[i + 2];
            float dist  = pts[i + 3];
            i += 4;

            char distBuf[32];
            snprintf(distBuf, sizeof(distBuf), "%.7f", dist * 100.0f);
            jstring distStr = env->NewStringUTF(distBuf);

            env->CallVoidMethod(g_drawPaint, g_setStyle, g_fillStyle);
            env->CallVoidMethod(g_drawPaint, g_setTextSize, 26.0f);
            env->CallVoidMethod(g_drawPaint, g_setStrokeW, 0.0f);

            env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0xFF000000);
            env->CallVoidMethod(canvas, g_drawText, distStr,
                                (jfloat)(tgtSX + 22.0f + 1.5f),
                                (jfloat)(tgtSY + 8.0f + 1.5f), g_drawPaint);

            env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0xFF00FF00);
            env->CallVoidMethod(canvas, g_drawText, distStr,
                                (jfloat)(tgtSX + 22.0f),
                                (jfloat)(tgtSY + 8.0f), g_drawPaint);

            env->CallVoidMethod(g_drawPaint, g_setStrokeW, 4.0f);

            env->DeleteLocalRef(distStr);
        } else {
            i++;
        }
    }

    }

    env->ReleaseFloatArrayElements(points, pts, JNI_ABORT);

    if (g_ghost.hideTrajectory) return;
    std::vector<int> pocketed;
    {
        std::lock_guard<std::mutex> lk(trajectoryMutex);
        pocketed = cachedWillPocket;
    }
    int displayBall = -1;
    for (int b : pocketed) {
        if (b > 0 && b <= 15) { displayBall = b; break; }
    }
    if (displayBall < 0 && g_ghost.botBestPocket > 0 && g_ghost.botBestPocket <= 15) {
        displayBall = g_ghost.botBestPocket;
    }
    const char* humanState = nullptr;
    if (g_ghost.botHumanMode && g_ghost.botEnabled) {
        switch (g_ghost.botState) {
            case GhostState::BOT_SCANNING:   humanState = "GOAT  \xC2\xB7  THINKING"; break;
            case GhostState::BOT_AIMING:     humanState = "GOAT  \xC2\xB7  LOCKED"; break;
            case GhostState::BOT_NOMINATING: humanState = "GOAT  \xC2\xB7  CALLING"; break;
            case GhostState::BOT_SHOOTING:   humanState = "GOAT  \xC2\xB7  PULLING"; break;
            default: break;
        }
    }
    if ((displayBall > 0 || humanState) && g_screenWidth > 0 && g_screenHeight > 0) {
        char labelBuf[40];
        if (humanState) {
            snprintf(labelBuf, sizeof(labelBuf), "%s", humanState);
        } else {
            snprintf(labelBuf, sizeof(labelBuf), "GOAT AIM  \xC2\xB7  Pot #%d", displayBall);
        }
        jstring label = env->NewStringUTF(labelBuf);

        env->CallVoidMethod(g_drawPaint, g_setTextSize, 14.0f);
        float textW = env->CallFloatMethod(g_drawPaint, g_measureText, label);
        float padX = 15.0f;
        float pillW = fmaxf(126.0f, textW + padX * 2.0f);
        float pillH = 34.0f;
        float x1 = (float)g_screenWidth * 0.5f - pillW * 0.5f;
        float y1 = (float)g_screenHeight - 82.0f;
        if (y1 < (float)g_screenHeight * 0.74f) y1 = (float)g_screenHeight * 0.74f;
        float x2 = x1 + pillW;
        float y2 = y1 + pillH;

        env->CallVoidMethod(g_drawPaint, g_setStyle, g_fillStyle);
        env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0xB6121720);
        env->CallVoidMethod(canvas, g_drawRRect, x1, y1, x2, y2, 13.0f, 13.0f, g_drawPaint);

        env->CallVoidMethod(g_drawPaint, g_setStyle, g_strokeStyle);
        env->CallVoidMethod(g_drawPaint, g_setStrokeW, 1.2f);
        env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0x772FEAF2);
        env->CallVoidMethod(canvas, g_drawRRect, x1, y1, x2, y2, 13.0f, 13.0f, g_drawPaint);

        env->CallVoidMethod(g_drawPaint, g_setStyle, g_fillStyle);
        env->CallVoidMethod(g_drawPaint, g_setColor, (jint)0xFFE9F8FF);
        env->CallVoidMethod(canvas, g_drawText, label, x1 + padX, y1 + 21.5f, g_drawPaint);

        env->DeleteLocalRef(label);
        env->CallVoidMethod(g_drawPaint, g_setStrokeW, 4.0f);
    }
}

static jobject g_panelLayout = nullptr;
static jobject g_forceLabel  = nullptr;
static jobject g_circleBtn   = nullptr;

#define CIRCLE_ID  100
#define PANEL_ID   200
#define REAL_ID    101
#define AUTO_ID    108
#define HUMAN_ID   109
#define VISIB_ID   110
#define MINUS_ID   104
#define PLUS_ID    105
#define HIDE_ID    107
#define MAXBALL_ID 111
#define POCKET_ID  112
#define POOL_TAB_ID 113
#define SOL_TAB_ID  114
#define SOL_AUTO_ID 115
#define UNO_TAB_ID  116
#define UNO_AUTO_ID 117

static jobject g_realBtn     = nullptr;
static jobject g_autoBtn     = nullptr;
static jobject g_humanBtn    = nullptr;
static jobject g_visibBtn    = nullptr;
static jobject g_hideBtn     = nullptr;
static jobject g_maxBallBtn  = nullptr;
static jobject g_pocketBtn   = nullptr;
static jobject g_poolTab     = nullptr;
static jobject g_solTab      = nullptr;
static jobject g_unoTab      = nullptr;
static jobject g_poolContent = nullptr;
static jobject g_solContent  = nullptr;
static jobject g_unoContent  = nullptr;
static jobject g_solAutoBtn  = nullptr;
static jobject g_unoAutoBtn  = nullptr;
static jobject g_seekBarRef  = nullptr;
static JavaVM* g_jvmOverlay = nullptr;
static bool g_maxBallsUiEnabled = false;
static bool g_autoPocketUiEnabled = false;
enum GamePanelTab : int {
    GAME_TAB_POOL = 0,
    GAME_TAB_SOLITAIRE = 1,
    GAME_TAB_UNOO = 2,
};
static GamePanelTab g_selectedGameTab = GAME_TAB_POOL;

static jobject g_expiryLabel = nullptr;
static std::atomic<time_t> g_expiryTimeVal{0};
static std::atomic<unsigned long long> g_expiryGeneration{0};
static time_t g_lastExpiryUiSecond = 0;

static void updateExpiryLabelText(JNIEnv* env) {
    if (!env || !g_expiryLabel) return;
    const time_t expiry = g_expiryTimeVal.load();
    if (expiry <= 0) return;
    const time_t now = time(nullptr);
    if (now == g_lastExpiryUiSecond) return;
    g_lastExpiryUiSecond = now;

    long long s = (long long)difftime(expiry, now);
    char buf[96];
    jint color;
    if (s <= 0) {
        snprintf(buf, sizeof(buf), "\xE2\x97\x86  LICENSE EXPIRED");
        color = (jint)0xFFFF526B;
    } else {
        const int d = (int)(s / 86400);
        const int h = (int)((s % 86400) / 3600);
        const int m = (int)((s % 3600) / 60);
        const int sec = (int)(s % 60);
        if (d > 0)
            snprintf(buf, sizeof(buf), "\xE2\x97\x86  LICENSE ACTIVE   \xE2\x80\xA2   %02dD  %02d:%02d:%02d", d, h, m, sec);
        else
            snprintf(buf, sizeof(buf), "\xE2\x97\x86  LICENSE ACTIVE   \xE2\x80\xA2   %02d:%02d:%02d", h, m, sec);
        color = s <= 3600 ? (jint)0xFFFF6B7D
                          : (s <= 86400 ? (jint)0xFFFFC857 : (jint)0xFF55FF9A);
    }

    jclass tc = env->FindClass("android/widget/TextView");
    if (!tc) { env->ExceptionClear(); return; }
    jstring jt = env->NewStringUTF(buf);
    env->CallVoidMethod(g_expiryLabel,
        env->GetMethodID(tc, "setText", "(Ljava/lang/CharSequence;)V"), jt);
    env->CallVoidMethod(g_expiryLabel,
        env->GetMethodID(tc, "setTextColor", "(I)V"), color);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(tc);
}

static void startExpiryCountdown(JNIEnv* env, time_t expiry) {
    g_expiryTimeVal.store(expiry);
    g_lastExpiryUiSecond = 0;
    updateExpiryLabelText(env);

    const unsigned long long generation = g_expiryGeneration.fetch_add(1) + 1;
    std::thread([generation]() {
        while (g_expiryGeneration.load() == generation) {
            sleep(1);
            if (g_expiryGeneration.load() != generation) break;
            // postInvalidate schedules nativeOnDraw on Android's UI thread;
            // the worker never mutates a TextView directly.
            overlay_requestRedraw();
        }
    }).detach();
}

void showExpiredLicenseUi(JNIEnv* env) {
    g_expiryGeneration.fetch_add(1);
    g_ghost.panelVisible = false;
    g_maxBallsUiEnabled = false;
    g_autoPocketUiEnabled = false;
    setBtnActive(env, g_realBtn, false, (jint)0x00000000);
    setBtnActive(env, g_autoBtn, false, (jint)0x00000000);
    setBtnActive(env, g_humanBtn, false, (jint)0x00000000);
    setBtnActive(env, g_visibBtn, false, (jint)0x00000000);
    setBtnActive(env, g_maxBallBtn, false, (jint)0x00000000);
    setBtnActive(env, g_pocketBtn, false, (jint)0x00000000);
    solitaire_set_enabled(false);
    uno_set_enabled(false);
    setBtnActive(env, g_solAutoBtn, false, (jint)0x00000000);
    setBtnActive(env, g_unoAutoBtn, false, (jint)0x00000000);
    jclass viewCls = env->FindClass("android/view/View");
    if (viewCls) {
        jmethodID setVis = env->GetMethodID(viewCls, "setVisibility", "(I)V");
        if (setVis) {
            if (g_panelLayout) env->CallVoidMethod(g_panelLayout, setVis, 8);
            if (g_circleBtn) env->CallVoidMethod(g_circleBtn, setVis, 8);
        }
        env->DeleteLocalRef(viewCls);
    } else {
        env->ExceptionClear();
    }
    showLicenseDialog(env);
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeCreatePanel(
    JNIEnv* env, jclass clz, jobject activity, jobject listenerView);

void startMainPanel(JNIEnv* env, jobject activity, jobject listenerView) {
    Java_com_google_firebase_TrainerOverlayView_nativeCreatePanel(
        env, nullptr, activity, listenerView);
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeCreatePanel(
    JNIEnv* env, jclass clz, jobject activity, jobject listenerView)
{
    // Keep the drawing view ready before the first simulation result arrives.
    // Waiting for its first onDraw can otherwise drop the initial trajectory.
    if (listenerView) overlay_setView(env, listenerView);
    license_init(env, activity, listenerView);
    if (!checkSavedLicense(env)) {
        showLicenseDialog(env);
        return;
    }

    env->GetJavaVM(&g_jvmOverlay);
    const bool poolLicensed = license_security_allows_game(GOAT_GAME_POOL);
    const bool solitaireLicensed = license_security_allows_game(GOAT_GAME_SOLITAIRE);
    const bool onooLicensed = license_security_allows_game(GOAT_GAME_ONOO);
    g_selectedGameTab = poolLicensed ? GAME_TAB_POOL
                         : (solitaireLicensed ? GAME_TAB_SOLITAIRE : GAME_TAB_UNOO);

    // A new key after expiry reuses the existing panel instead of stacking a
    // second overlay and leaking global JNI references.
    if (g_panelLayout && g_circleBtn) {
        jclass viewCls = env->FindClass("android/view/View");
        jmethodID setVis = env->GetMethodID(viewCls, "setVisibility", "(I)V");
        env->CallVoidMethod(g_circleBtn, setVis, 0);
        env->CallVoidMethod(g_panelLayout, setVis, 8);
        env->DeleteLocalRef(viewCls);
        g_ghost.panelVisible = false;
        applyEntitlementUi(env);
        const time_t expTime = getExpiryTime(env);
        if (expTime > 0) startExpiryCountdown(env, expTime);
        return;
    }

    jclass actCls      = env->FindClass("android/app/Activity");
    jclass tvCls       = env->FindClass("android/widget/TextView");
    jclass llCls       = env->FindClass("android/widget/LinearLayout");
    jclass sbCls       = env->FindClass("android/widget/SeekBar");
    jclass flpCls      = env->FindClass("android/widget/FrameLayout$LayoutParams");
    jclass llpCls      = env->FindClass("android/widget/LinearLayout$LayoutParams");
    jclass gdCls       = env->FindClass("android/graphics/drawable/GradientDrawable");
    jclass viewCls     = env->FindClass("android/view/View");
    jclass cslCls      = env->FindClass("android/content/res/ColorStateList");

    jmethodID tvCtor     = env->GetMethodID(tvCls, "<init>", "(Landroid/content/Context;)V");
    jmethodID llCtor     = env->GetMethodID(llCls, "<init>", "(Landroid/content/Context;)V");
    jmethodID sbCtor     = env->GetMethodID(sbCls, "<init>", "(Landroid/content/Context;)V");
    jmethodID flpCtor    = env->GetMethodID(flpCls, "<init>", "(II)V");
    jmethodID llpCtor    = env->GetMethodID(llpCls, "<init>", "(II)V");
    jmethodID gdCtor     = env->GetMethodID(gdCls, "<init>", "()V");
    jmethodID setText    = env->GetMethodID(tvCls, "setText", "(Ljava/lang/CharSequence;)V");
    jmethodID setTxtSz   = env->GetMethodID(tvCls, "setTextSize", "(F)V");
    jmethodID setTxtClr  = env->GetMethodID(tvCls, "setTextColor", "(I)V");
    jmethodID setGrav    = env->GetMethodID(tvCls, "setGravity", "(I)V");
    jmethodID setOrient  = env->GetMethodID(llCls, "setOrientation", "(I)V");
    jmethodID llAddView  = env->GetMethodID(llCls, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    jmethodID setPad     = env->GetMethodID(viewCls, "setPadding", "(IIII)V");
    jmethodID setBg      = env->GetMethodID(viewCls, "setBackground", "(Landroid/graphics/drawable/Drawable;)V");
    jmethodID setId      = env->GetMethodID(viewCls, "setId", "(I)V");
    jmethodID setVis     = env->GetMethodID(viewCls, "setVisibility", "(I)V");
    jmethodID setClick   = env->GetMethodID(viewCls, "setOnClickListener", "(Landroid/view/View$OnClickListener;)V");
    jmethodID setTouch   = env->GetMethodID(viewCls, "setOnTouchListener", "(Landroid/view/View$OnTouchListener;)V");
    jmethodID addContent = env->GetMethodID(actCls, "addContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    jmethodID gdSetShape = env->GetMethodID(gdCls, "setShape", "(I)V");
    jmethodID gdSetColor = env->GetMethodID(gdCls, "setColor", "(I)V");
    jmethodID gdSetCR    = env->GetMethodID(gdCls, "setCornerRadius", "(F)V");
    jmethodID gdSetStroke = env->GetMethodID(gdCls, "setStroke", "(II)V");
    jmethodID sbSetMax   = env->GetMethodID(sbCls, "setMax", "(I)V");
    jmethodID sbSetProg  = env->GetMethodID(sbCls, "setProgress", "(I)V");
    jmethodID sbSetLstn  = env->GetMethodID(sbCls, "setOnSeekBarChangeListener",
        "(Landroid/widget/SeekBar$OnSeekBarChangeListener;)V");
    jmethodID sbSetProgTint = env->GetMethodID(sbCls, "setProgressTintList",
        "(Landroid/content/res/ColorStateList;)V");
    jmethodID sbSetThumbTint = env->GetMethodID(sbCls, "setThumbTintList",
        "(Landroid/content/res/ColorStateList;)V");
    jmethodID cslValueOf = env->GetStaticMethodID(cslCls, "valueOf",
        "(I)Landroid/content/res/ColorStateList;");
    jfieldID flpGrav     = env->GetFieldID(flpCls, "gravity", "I");
    jfieldID flpTopM     = env->GetFieldID(flpCls, "topMargin", "I");
    jfieldID flpLeftM    = env->GetFieldID(flpCls, "leftMargin", "I");

    jclass ivCls = env->FindClass("android/widget/ImageView");
    jmethodID ivCtor = env->GetMethodID(ivCls, "<init>", "(Landroid/content/Context;)V");
    jobject circle = env->NewObject(ivCls, ivCtor, activity);
    {
        jclass ctxCls = env->FindClass("android/content/Context");
        jmethodID getAssets = env->GetMethodID(ctxCls, "getAssets", "()Landroid/content/res/AssetManager;");
        jobject assets = env->CallObjectMethod(activity, getAssets);
        jclass amCls = env->FindClass("android/content/res/AssetManager");
        jmethodID openAsset = env->GetMethodID(amCls, "open", "(Ljava/lang/String;)Ljava/io/InputStream;");
        jstring assetName = env->NewStringUTF("goat_plate.png");
        jobject stream = assets ? env->CallObjectMethod(assets, openAsset, assetName) : nullptr;
        env->DeleteLocalRef(assetName);
        if (stream) {
            jclass bfCls = env->FindClass("android/graphics/BitmapFactory");
            jmethodID decodeStream = env->GetStaticMethodID(bfCls, "decodeStream",
                "(Ljava/io/InputStream;)Landroid/graphics/Bitmap;");
            jobject bmp = env->CallStaticObjectMethod(bfCls, decodeStream, stream);
            if (bmp) {
                jmethodID setBmp = env->GetMethodID(ivCls, "setImageBitmap", "(Landroid/graphics/Bitmap;)V");
                env->CallVoidMethod(circle, setBmp, bmp);
                env->DeleteLocalRef(bmp);
            }
            env->DeleteLocalRef(bfCls);
            env->DeleteLocalRef(stream);
        }
        jclass scaleCls = env->FindClass("android/widget/ImageView$ScaleType");
        jfieldID fitCenterF = env->GetStaticFieldID(scaleCls, "FIT_CENTER", "Landroid/widget/ImageView$ScaleType;");
        jobject fitCenter = env->GetStaticObjectField(scaleCls, fitCenterF);
        jmethodID setScaleType = env->GetMethodID(ivCls, "setScaleType", "(Landroid/widget/ImageView$ScaleType;)V");
        env->CallVoidMethod(circle, setScaleType, fitCenter);
        env->DeleteLocalRef(scaleCls);
        env->DeleteLocalRef(amCls);
        env->DeleteLocalRef(ctxCls);
        if (assets) env->DeleteLocalRef(assets);
    }

    env->CallVoidMethod(circle, setId, CIRCLE_ID);
    env->CallVoidMethod(circle, setTouch, listenerView);

    jobject circleBg = env->NewObject(gdCls, gdCtor);
    env->CallVoidMethod(circleBg, gdSetCR, 8.0f);
    env->CallVoidMethod(circleBg, gdSetColor, (jint)0x00000000);
    env->CallVoidMethod(circleBg, gdSetStroke, 0, (jint)0x00000000);
    env->CallVoidMethod(circle, setBg, circleBg);
    env->DeleteLocalRef(circleBg);

    jobject circleFg = env->NewObject(gdCls, gdCtor);
    env->CallVoidMethod(circleFg, gdSetCR, 8.0f);
    env->CallVoidMethod(circleFg, gdSetColor, (jint)0x00000000);
    env->CallVoidMethod(circleFg, gdSetStroke, 0, (jint)0x00000000);
    jmethodID setFg = env->GetMethodID(viewCls, "setForeground", "(Landroid/graphics/drawable/Drawable;)V");
    env->CallVoidMethod(circle, setFg, circleFg);
    env->DeleteLocalRef(circleFg);

    env->CallVoidMethod(circle, setPad, 0, 0, 0, 0);
    jmethodID setClip = env->GetMethodID(viewCls, "setClipToOutline", "(Z)V");
    env->CallVoidMethod(circle, setClip, JNI_TRUE);

    jobject circleParams = env->NewObject(flpCls, flpCtor, 178, 90);
    env->SetIntField(circleParams, flpGrav, 51);
    env->SetIntField(circleParams, flpTopM, 38);
    env->SetIntField(circleParams, flpLeftM, 28);
    env->CallVoidMethod(activity, addContent, circle, circleParams);
    g_circleBtn = env->NewGlobalRef(circle);
    env->DeleteLocalRef(circle);
    env->DeleteLocalRef(circleParams);

    jobject panel = env->NewObject(llCls, llCtor, activity);
    env->CallVoidMethod(panel, setOrient, 1);
    env->CallVoidMethod(panel, setPad, 30, 22, 30, 22);
    env->CallVoidMethod(panel, setId, PANEL_ID);

    jobject panelBg = env->NewObject(gdCls, gdCtor);
    env->CallVoidMethod(panelBg, gdSetCR, 30.0f);
    env->CallVoidMethod(panelBg, gdSetColor, (jint)0xE9090B16);
    env->CallVoidMethod(panelBg, gdSetStroke, 2, (jint)0xAA00E5FF);
    env->CallVoidMethod(panel, setBg, panelBg);
    env->DeleteLocalRef(panelBg);

    jmethodID setElev = env->GetMethodID(viewCls, "setElevation", "(F)V");
    env->CallVoidMethod(panel, setElev, 12.0f);

    jmethodID setClipPad = env->GetMethodID(env->FindClass("android/view/ViewGroup"),
        "setClipToPadding", "(Z)V");
    env->CallVoidMethod(panel, setClipPad, JNI_FALSE);

    jmethodID setClickable = env->GetMethodID(viewCls, "setClickable", "(Z)V");
    env->CallVoidMethod(panel, setClickable, JNI_TRUE);

    {
        jclass typefaceCls2 = env->FindClass("android/graphics/Typeface");
        jfieldID tfBold2 = env->GetStaticFieldID(typefaceCls2, "DEFAULT_BOLD", "Landroid/graphics/Typeface;");
        jobject boldTf2 = env->GetStaticObjectField(typefaceCls2, tfBold2);
        jmethodID setTypeface2 = env->GetMethodID(tvCls, "setTypeface", "(Landroid/graphics/Typeface;)V");
        jmethodID setLetterSp2 = env->GetMethodID(tvCls, "setLetterSpacing", "(F)V");
        jmethodID llSetGrav2 = env->GetMethodID(llCls, "setGravity", "(I)V");
        jmethodID llpCtorW2 = env->GetMethodID(llpCls, "<init>", "(IIF)V");
        jfieldID llpTopM2 = env->GetFieldID(llpCls, "topMargin", "I");
        jfieldID llpBotM2 = env->GetFieldID(llpCls, "bottomMargin", "I");
        jfieldID llpRightM2 = env->GetFieldID(llpCls, "rightMargin", "I");

        env->CallVoidMethod(panel, setPad, 18, 16, 18, 14);
        jobject newPanelBg = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(newPanelBg, gdSetCR, 14.0f);
        env->CallVoidMethod(newPanelBg, gdSetColor, (jint)0x9A060A14);
        env->CallVoidMethod(newPanelBg, gdSetStroke, 3, (jint)0xE68F35FF);
        env->CallVoidMethod(panel, setBg, newPanelBg);
        env->DeleteLocalRef(newPanelBg);

        auto miniBg = [&](jobject v) {
            jobject bg = env->NewObject(gdCls, gdCtor);
            env->CallVoidMethod(bg, gdSetCR, 8.0f);
            env->CallVoidMethod(bg, gdSetColor, (jint)0x880B1022);
            env->CallVoidMethod(bg, gdSetStroke, 2, (jint)0xCC7D31FF);
            env->CallVoidMethod(v, setBg, bg);
            env->DeleteLocalRef(bg);
        };

        jobject header = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(header, setOrient, 0);
        env->CallVoidMethod(header, llSetGrav2, 16);

        jobject title2 = env->NewObject(tvCls, tvCtor, activity);
        jstring titleTxt2 = env->NewStringUTF("GOAT VVIP");
        env->CallVoidMethod(title2, setText, titleTxt2); env->DeleteLocalRef(titleTxt2);
        env->CallVoidMethod(title2, setTxtSz, 17.0f);
        env->CallVoidMethod(title2, setTxtClr, (jint)0xFFEDEDF7);
        env->CallVoidMethod(title2, setTypeface2, boldTf2);
        env->CallVoidMethod(title2, setLetterSp2, 0.10f);
        jobject titleP2 = env->NewObject(llpCls, llpCtorW2, 0, -2, 1.0f);
        env->CallVoidMethod(header, llAddView, title2, titleP2);
        env->DeleteLocalRef(title2); env->DeleteLocalRef(titleP2);

        auto makeMini2 = [&](const char* text) -> jobject {
            jobject b = env->NewObject(tvCls, tvCtor, activity);
            jstring s = env->NewStringUTF(text);
            env->CallVoidMethod(b, setText, s); env->DeleteLocalRef(s);
            env->CallVoidMethod(b, setTxtSz, 17.0f);
            env->CallVoidMethod(b, setTxtClr, (jint)0xFFEDEDF7);
            env->CallVoidMethod(b, setGrav, 17);
            env->CallVoidMethod(b, setTypeface2, boldTf2);
            env->CallVoidMethod(b, setId, CIRCLE_ID);
            env->CallVoidMethod(b, setClick, listenerView);
            miniBg(b);
            return b;
        };
        jobject m2 = makeMini2("-");
        jobject m2p = env->NewObject(llpCls, llpCtor, 40, 34);
        env->SetIntField(m2p, llpRightM2, 7);
        env->CallVoidMethod(header, llAddView, m2, m2p);
        env->DeleteLocalRef(m2); env->DeleteLocalRef(m2p);
        jobject x2 = makeMini2("×");
        jobject x2p = env->NewObject(llpCls, llpCtor, 40, 34);
        env->CallVoidMethod(header, llAddView, x2, x2p);
        env->DeleteLocalRef(x2); env->DeleteLocalRef(x2p);

        jobject hp2 = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(hp2, llpBotM2, 10);
        env->CallVoidMethod(panel, llAddView, header, hp2);
        env->DeleteLocalRef(header); env->DeleteLocalRef(hp2);

        jobject badges = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(badges, setOrient, 0);
        auto addBadge2 = [&](const char* text, jint fg, jint bgc) {
            jobject b = env->NewObject(tvCls, tvCtor, activity);
            jstring s = env->NewStringUTF(text);
            env->CallVoidMethod(b, setText, s); env->DeleteLocalRef(s);
            env->CallVoidMethod(b, setTxtSz, 10.0f);
            env->CallVoidMethod(b, setTxtClr, fg);
            env->CallVoidMethod(b, setTypeface2, boldTf2);
            env->CallVoidMethod(b, setGrav, 17);
            env->CallVoidMethod(b, setPad, 11, 4, 11, 5);
            jobject bg = env->NewObject(gdCls, gdCtor);
            env->CallVoidMethod(bg, gdSetCR, 8.0f);
            env->CallVoidMethod(bg, gdSetColor, bgc);
            env->CallVoidMethod(bg, gdSetStroke, 1, (jint)0x665B35FF);
            env->CallVoidMethod(b, setBg, bg);
            env->DeleteLocalRef(bg);
            jobject p = env->NewObject(llpCls, llpCtor, -2, -2);
            env->SetIntField(p, llpRightM2, 6);
            env->CallVoidMethod(badges, llAddView, b, p);
            env->DeleteLocalRef(b); env->DeleteLocalRef(p);
        };
        addBadge2("V2.5", (jint)0xFFC06BFF, (jint)0x661B1030);
        addBadge2("ONLINE  \xE2\x80\xA2", (jint)0xFF31FF86, (jint)0x66102317);
        jobject bp2 = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(bp2, llpBotM2, 13);
        env->CallVoidMethod(panel, llAddView, badges, bp2);
        env->DeleteLocalRef(badges); env->DeleteLocalRef(bp2);

        // Luxury live license card.  The countdown is server-expiry based and
        // updates every second while keeping all TextView work on the UI thread.
        if (g_expiryLabel) { env->DeleteGlobalRef(g_expiryLabel); g_expiryLabel = nullptr; }
        jobject expLbl2 = env->NewObject(tvCls, tvCtor, activity);
        jstring expInit2 = env->NewStringUTF("\xE2\x97\x86  LICENSE ACTIVE");
        env->CallVoidMethod(expLbl2, setText, expInit2); env->DeleteLocalRef(expInit2);
        env->CallVoidMethod(expLbl2, setTxtSz, 10.5f);
        env->CallVoidMethod(expLbl2, setTxtClr, (jint)0xFF55FF9A);
        env->CallVoidMethod(expLbl2, setTypeface2, boldTf2);
        env->CallVoidMethod(expLbl2, setLetterSp2, 0.055f);
        env->CallVoidMethod(expLbl2, setGrav, 17);
        env->CallVoidMethod(expLbl2, setPad, 12, 8, 12, 8);
        jobject expBg2 = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(expBg2, gdSetCR, 10.0f);
        env->CallVoidMethod(expBg2, gdSetColor, (jint)0xB20A1020);
        env->CallVoidMethod(expBg2, gdSetStroke, 2, (jint)0xAA8F35FF);
        env->CallVoidMethod(expLbl2, setBg, expBg2);
        env->DeleteLocalRef(expBg2);
        jobject expP2 = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(expP2, llpBotM2, 12);
        env->CallVoidMethod(panel, llAddView, expLbl2, expP2);
        g_expiryLabel = env->NewGlobalRef(expLbl2);
        env->DeleteLocalRef(expLbl2); env->DeleteLocalRef(expP2);

        // Separate game workspaces. Pool, Solitaire and UNOO keep independent state;
        // the selected tab only changes what is displayed in this panel.
        jobject tabs = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(tabs, setOrient, 0);
        auto makeTab2 = [&](const char* text, jint id, bool active) -> jobject {
            jobject tab = env->NewObject(tvCls, tvCtor, activity);
            jstring s = env->NewStringUTF(text);
            env->CallVoidMethod(tab, setText, s); env->DeleteLocalRef(s);
            env->CallVoidMethod(tab, setTxtSz, 10.5f);
            env->CallVoidMethod(tab, setTxtClr, active ? (jint)0xFFF2ECFF : (jint)0xFF8F95B8);
            env->CallVoidMethod(tab, setTypeface2, boldTf2);
            env->CallVoidMethod(tab, setLetterSp2, 0.08f);
            env->CallVoidMethod(tab, setGrav, 17);
            env->CallVoidMethod(tab, setPad, 8, 7, 8, 7);
            env->CallVoidMethod(tab, setId, id);
            env->CallVoidMethod(tab, setClick, listenerView);
            jobject bg = env->NewObject(gdCls, gdCtor);
            env->CallVoidMethod(bg, gdSetCR, 8.0f);
            env->CallVoidMethod(bg, gdSetColor, active ? (jint)0xAA3B147A : (jint)0x4A090D19);
            env->CallVoidMethod(bg, gdSetStroke, 1, active ? (jint)0xFFAF52FF : (jint)0x66706A91);
            env->CallVoidMethod(tab, setBg, bg);
            env->DeleteLocalRef(bg);
            return tab;
        };
        jobject poolTab = makeTab2("POOL", POOL_TAB_ID, g_selectedGameTab == GAME_TAB_POOL);
        jobject poolTabP = env->NewObject(llpCls, llpCtorW2, 0, -2, 1.0f);
        env->SetIntField(poolTabP, llpRightM2, 7);
        env->CallVoidMethod(tabs, llAddView, poolTab, poolTabP);
        g_poolTab = env->NewGlobalRef(poolTab);
        env->DeleteLocalRef(poolTab); env->DeleteLocalRef(poolTabP);
        jobject solTab = makeTab2("SOLITAIRE", SOL_TAB_ID, g_selectedGameTab == GAME_TAB_SOLITAIRE);
        jobject solTabP = env->NewObject(llpCls, llpCtorW2, 0, -2, 1.0f);
        env->SetIntField(solTabP, llpRightM2, 7);
        env->CallVoidMethod(tabs, llAddView, solTab, solTabP);
        g_solTab = env->NewGlobalRef(solTab);
        env->DeleteLocalRef(solTab); env->DeleteLocalRef(solTabP);
        jobject unoTab = makeTab2("UNOO", UNO_TAB_ID, g_selectedGameTab == GAME_TAB_UNOO);
        jobject unoTabP = env->NewObject(llpCls, llpCtorW2, 0, -2, 1.0f);
        env->CallVoidMethod(tabs, llAddView, unoTab, unoTabP);
        g_unoTab = env->NewGlobalRef(unoTab);
        env->DeleteLocalRef(unoTab); env->DeleteLocalRef(unoTabP);
        jobject tabsP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(tabsP, llpBotM2, 10);
        env->CallVoidMethod(panel, llAddView, tabs, tabsP);
        env->DeleteLocalRef(tabs); env->DeleteLocalRef(tabsP);

        jobject poolContent = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(poolContent, setOrient, 1);
        env->CallVoidMethod(poolContent, setVis, g_selectedGameTab == GAME_TAB_POOL ? 0 : 8);
        jobject solContent = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(solContent, setOrient, 1);
        env->CallVoidMethod(solContent, setVis, g_selectedGameTab == GAME_TAB_SOLITAIRE ? 0 : 8);
        jobject unoContent = env->NewObject(llCls, llCtor, activity);
        env->CallVoidMethod(unoContent, setOrient, 1);
        env->CallVoidMethod(unoContent, setVis, g_selectedGameTab == GAME_TAB_UNOO ? 0 : 8);
        jobject featureHost = poolContent;

        auto addFeature2 = [&](const char* icon, const char* label, jint id, bool checked, jobject* outRef) {
            jobject row = env->NewObject(llCls, llCtor, activity);
            env->CallVoidMethod(row, setOrient, 0);
            env->CallVoidMethod(row, llSetGrav2, 16);
            env->CallVoidMethod(row, setPad, 12, 9, 12, 9);
            env->CallVoidMethod(row, setId, id);
            env->CallVoidMethod(row, setClick, listenerView);
            jobject rbg = env->NewObject(gdCls, gdCtor);
            env->CallVoidMethod(rbg, gdSetCR, 9.0f);
            env->CallVoidMethod(rbg, gdSetColor, (jint)0x620B101C);
            env->CallVoidMethod(rbg, gdSetStroke, 1, (jint)0x526E78A0);
            env->CallVoidMethod(row, setBg, rbg);
            env->DeleteLocalRef(rbg);

            jobject ic = env->NewObject(tvCls, tvCtor, activity);
            jstring it = env->NewStringUTF(icon);
            env->CallVoidMethod(ic, setText, it); env->DeleteLocalRef(it);
            env->CallVoidMethod(ic, setTxtSz, 22.0f);
            env->CallVoidMethod(ic, setTxtClr, (jint)0xFFB349FF);
            env->CallVoidMethod(ic, setGrav, 17);
            env->CallVoidMethod(ic, setTypeface2, boldTf2);
            jobject ip = env->NewObject(llpCls, llpCtor, 42, 34);
            env->SetIntField(ip, llpRightM2, 10);
            env->CallVoidMethod(row, llAddView, ic, ip);
            env->DeleteLocalRef(ic); env->DeleteLocalRef(ip);

            jobject tv = env->NewObject(tvCls, tvCtor, activity);
            jstring lt = env->NewStringUTF(label);
            env->CallVoidMethod(tv, setText, lt); env->DeleteLocalRef(lt);
            env->CallVoidMethod(tv, setTxtSz, 14.0f);
            env->CallVoidMethod(tv, setTxtClr, (jint)0xFFF0F0F6);
            env->CallVoidMethod(tv, setGrav, 16);
            jobject tp = env->NewObject(llpCls, llpCtorW2, 0, -2, 1.0f);
            env->CallVoidMethod(row, llAddView, tv, tp);
            env->DeleteLocalRef(tv); env->DeleteLocalRef(tp);

            jobject chk = env->NewObject(tvCls, tvCtor, activity);
            jstring ct = env->NewStringUTF(checked ? "\xE2\x9C\x93" : "");
            env->CallVoidMethod(chk, setText, ct); env->DeleteLocalRef(ct);
            env->CallVoidMethod(chk, setTxtSz, 20.0f);
            env->CallVoidMethod(chk, setTxtClr, (jint)0xFFFFFFFF);
            env->CallVoidMethod(chk, setGrav, 17);
            env->CallVoidMethod(chk, setTypeface2, boldTf2);
            env->CallVoidMethod(chk, setId, id);
            env->CallVoidMethod(chk, setClick, listenerView);
            jobject cbg = env->NewObject(gdCls, gdCtor);
            env->CallVoidMethod(cbg, gdSetCR, 10.0f);
            env->CallVoidMethod(cbg, gdSetColor, checked ? (jint)0xFF7B25FF : (jint)0x22101830);
            env->CallVoidMethod(cbg, gdSetStroke, 2, checked ? (jint)0xFFE0A1FF : (jint)0xAA553083);
            env->CallVoidMethod(chk, setBg, cbg);
            env->DeleteLocalRef(cbg);
            jobject cp = env->NewObject(llpCls, llpCtor, 48, 38);
            env->CallVoidMethod(row, llAddView, chk, cp);
            if (outRef) *outRef = env->NewGlobalRef(chk);
            env->DeleteLocalRef(chk); env->DeleteLocalRef(cp);

            jobject rp = env->NewObject(llpCls, llpCtor, -1, -2);
            env->SetIntField(rp, llpBotM2, 8);
            env->CallVoidMethod(featureHost, llAddView, row, rp);
            env->DeleteLocalRef(row); env->DeleteLocalRef(rp);
        };

        addFeature2("\xE2\x8A\x95", "Auto Aim",    REAL_ID,    poolLicensed,  &g_realBtn);
        addFeature2("\xE2\x96\xB7", "Auto Play",   AUTO_ID,    poolLicensed,  &g_autoBtn);
        addFeature2("\xE2\x86\x97", "Prediction",  VISIB_ID,   poolLicensed,  &g_visibBtn);
        addFeature2("8", "Max Balls",   MAXBALL_ID, false, &g_maxBallBtn);
        addFeature2("\xE2\x96\xA1", "Auto Pocket", POCKET_ID,  false, &g_pocketBtn);
        addFeature2("\xE2\x9A\xA1", "Smart Aim",   HUMAN_ID,   poolLicensed,  &g_humanBtn);

        jobject poolContentP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->CallVoidMethod(panel, llAddView, poolContent, poolContentP);
        g_poolContent = env->NewGlobalRef(poolContent);
        env->DeleteLocalRef(poolContent); env->DeleteLocalRef(poolContentP);

        featureHost = solContent;
        addFeature2("\xE2\x99\xA0", "Auto Play Solitaire", SOL_AUTO_ID, false, &g_solAutoBtn);
        jobject solHint = env->NewObject(tvCls, tvCtor, activity);
        jstring solHintText = env->NewStringUTF("Native moves  \xC2\xB7  Smooth cards");
        env->CallVoidMethod(solHint, setText, solHintText); env->DeleteLocalRef(solHintText);
        env->CallVoidMethod(solHint, setTxtSz, 10.0f);
        env->CallVoidMethod(solHint, setTxtClr, (jint)0xFF9EA8C4);
        env->CallVoidMethod(solHint, setGrav, 17);
        jobject solHintP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(solHintP, llpBotM2, 5);
        env->CallVoidMethod(solContent, llAddView, solHint, solHintP);
        env->DeleteLocalRef(solHint); env->DeleteLocalRef(solHintP);
        jobject solContentP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->CallVoidMethod(panel, llAddView, solContent, solContentP);
        g_solContent = env->NewGlobalRef(solContent);
        env->DeleteLocalRef(solContent); env->DeleteLocalRef(solContentP);

        featureHost = unoContent;
        addFeature2("U", "Auto Play UNOO", UNO_AUTO_ID, false, &g_unoAutoBtn);
        jobject unoHint = env->NewObject(tvCls, tvCtor, activity);
        jstring unoHintText = env->NewStringUTF("Native rules  \xC2\xB7  Expert look-ahead");
        env->CallVoidMethod(unoHint, setText, unoHintText); env->DeleteLocalRef(unoHintText);
        env->CallVoidMethod(unoHint, setTxtSz, 10.0f);
        env->CallVoidMethod(unoHint, setTxtClr, (jint)0xFF9EA8C4);
        env->CallVoidMethod(unoHint, setGrav, 17);
        jobject unoHintP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(unoHintP, llpBotM2, 5);
        env->CallVoidMethod(unoContent, llAddView, unoHint, unoHintP);
        env->DeleteLocalRef(unoHint); env->DeleteLocalRef(unoHintP);
        jobject unoContentP = env->NewObject(llpCls, llpCtor, -1, -2);
        env->CallVoidMethod(panel, llAddView, unoContent, unoContentP);
        g_unoContent = env->NewGlobalRef(unoContent);
        env->DeleteLocalRef(unoContent); env->DeleteLocalRef(unoContentP);

        g_ghost.realForceEnabled = poolLicensed;
        g_ghost.botEnabled = poolLicensed;
        g_ghost.botHumanMode = poolLicensed;
        g_ghost.hideTrajectory = !poolLicensed;
        g_ghost.trajectoryHidden = !poolLicensed;

        jobject foot = env->NewObject(tvCls, tvCtor, activity);
        jstring ft = env->NewStringUTF("HIDE ADVANCED  \xE2\x8C\x83");
        env->CallVoidMethod(foot, setText, ft); env->DeleteLocalRef(ft);
        env->CallVoidMethod(foot, setTxtSz, 11.0f);
        env->CallVoidMethod(foot, setTxtClr, (jint)0xFFC45BFF);
        env->CallVoidMethod(foot, setGrav, 17);
        jobject fp = env->NewObject(llpCls, llpCtor, -1, -2);
        env->SetIntField(fp, llpTopM2, 3);
        env->CallVoidMethod(panel, llAddView, foot, fp);
        env->DeleteLocalRef(foot); env->DeleteLocalRef(fp);

        env->CallVoidMethod(panel, setVis, 8);
        jobject panelParams2 = env->NewObject(flpCls, flpCtor, 430, -2);
        env->SetIntField(panelParams2, flpGrav, 51);
        env->SetIntField(panelParams2, flpTopM, 128);
        env->SetIntField(panelParams2, flpLeftM, 8);
        env->CallVoidMethod(activity, addContent, panel, panelParams2);
        g_panelLayout = env->NewGlobalRef(panel);
        applyEntitlementUi(env);
        env->DeleteLocalRef(panel);
        env->DeleteLocalRef(panelParams2);
        env->DeleteLocalRef(typefaceCls2);
        env->DeleteLocalRef(boldTf2);
        env->DeleteLocalRef(cslCls);

        time_t expTime = getExpiryTime(env);
        if (expTime > 0) startExpiryCountdown(env, expTime);
        return;
    }

    jclass typefaceCls = env->FindClass("android/graphics/Typeface");
    jfieldID tfBold = env->GetStaticFieldID(typefaceCls, "DEFAULT_BOLD", "Landroid/graphics/Typeface;");
    jobject boldTf = env->GetStaticObjectField(typefaceCls, tfBold);
    jmethodID setTypeface = env->GetMethodID(tvCls, "setTypeface", "(Landroid/graphics/Typeface;)V");
    jmethodID setLetterSp = env->GetMethodID(tvCls, "setLetterSpacing", "(F)V");

    jfieldID llpTopM = env->GetFieldID(llpCls, "topMargin", "I");
    jfieldID llpBotM = env->GetFieldID(llpCls, "bottomMargin", "I");

    jobject title = env->NewObject(tvCls, tvCtor, activity);
    jstring titleText = env->NewStringUTF("GOAT CONTROL");
    env->CallVoidMethod(title, setText, titleText);
    env->DeleteLocalRef(titleText);
    env->CallVoidMethod(title, setTxtSz, 16.0f);
    env->CallVoidMethod(title, setTxtClr, (jint)0xFF00E5FF);
    env->CallVoidMethod(title, setGrav, 17);
    env->CallVoidMethod(title, setTypeface, boldTf);
    env->CallVoidMethod(title, setLetterSp, 0.08f);
    jobject titleParams = env->NewObject(llpCls, llpCtor, -1, -2);
    env->CallVoidMethod(panel, llAddView, title, titleParams);
    env->DeleteLocalRef(title); env->DeleteLocalRef(titleParams);

    jobject subTitle = env->NewObject(tvCls, tvCtor, activity);
    jstring subText = env->NewStringUTF("VVIP trajectory studio");
    env->CallVoidMethod(subTitle, setText, subText);
    env->DeleteLocalRef(subText);
    env->CallVoidMethod(subTitle, setTxtSz, 9.5f);
    env->CallVoidMethod(subTitle, setTxtClr, (jint)0xFF8F95B8);
    env->CallVoidMethod(subTitle, setGrav, 17);
    env->CallVoidMethod(subTitle, setLetterSp, 0.04f);
    jobject subParams = env->NewObject(llpCls, llpCtor, -1, -2);
    env->SetIntField(subParams, llpBotM, 12);
    env->CallVoidMethod(panel, llAddView, subTitle, subParams);
    env->DeleteLocalRef(subTitle); env->DeleteLocalRef(subParams);

    jobject divider = env->NewObject(viewCls, env->GetMethodID(viewCls, "<init>", "(Landroid/content/Context;)V"), activity);
    jobject dividerBg = env->NewObject(gdCls, gdCtor);
    env->CallVoidMethod(dividerBg, gdSetColor, (jint)0x6600E5FF);
    env->CallVoidMethod(divider, setBg, dividerBg);
    env->DeleteLocalRef(dividerBg);
    jobject dividerParams = env->NewObject(llpCls, llpCtor, -1, 2);
    env->SetIntField(dividerParams, llpBotM, 14);
    env->CallVoidMethod(panel, llAddView, divider, dividerParams);
    env->DeleteLocalRef(divider); env->DeleteLocalRef(dividerParams);

    jobject label = env->NewObject(tvCls, tvCtor, activity);
    jstring lblText = env->NewStringUTF("FORCE  50%");
    env->CallVoidMethod(label, setText, lblText);
    env->DeleteLocalRef(lblText);
    env->CallVoidMethod(label, setTxtSz, 11.5f);
    env->CallVoidMethod(label, setTxtClr, (jint)0xFFE6F9FF);
    env->CallVoidMethod(label, setGrav, 17);
    env->CallVoidMethod(label, setTypeface, boldTf);
    env->CallVoidMethod(label, setLetterSp, 0.06f);
    jobject lblParams = env->NewObject(llpCls, llpCtor, -1, -2);
    env->CallVoidMethod(panel, llAddView, label, lblParams);
    g_forceLabel = env->NewGlobalRef(label);
    env->DeleteLocalRef(label); env->DeleteLocalRef(lblParams);

    jobject sliderRow = env->NewObject(llCls, llCtor, activity);
    env->CallVoidMethod(sliderRow, setOrient, 0);
    jmethodID llSetGrav2 = env->GetMethodID(llCls, "setGravity", "(I)V");
    env->CallVoidMethod(sliderRow, llSetGrav2, 16);

    jobject minusBtn = env->NewObject(tvCls, tvCtor, activity);
    jstring mTxt = env->NewStringUTF("−");
    env->CallVoidMethod(minusBtn, setText, mTxt); env->DeleteLocalRef(mTxt);
    env->CallVoidMethod(minusBtn, setTxtSz, 18.0f);
    env->CallVoidMethod(minusBtn, setTxtClr, (jint)0xFF00E5FF);
    env->CallVoidMethod(minusBtn, setGrav, 17);
    env->CallVoidMethod(minusBtn, setPad, 18, 4, 18, 4);
    env->CallVoidMethod(minusBtn, setId, MINUS_ID);
    env->CallVoidMethod(minusBtn, setClick, listenerView);
    {
        jobject mbg = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(mbg, gdSetCR, 16.0f);
        env->CallVoidMethod(mbg, gdSetColor, (jint)0xFF0D1428);
        env->CallVoidMethod(mbg, gdSetStroke, 1, (jint)0x9900E5FF);
        env->CallVoidMethod(minusBtn, setBg, mbg);
        env->DeleteLocalRef(mbg);
    }
    jobject mparam = env->NewObject(llpCls, llpCtor, -2, -2);
    env->CallVoidMethod(sliderRow, llAddView, minusBtn, mparam);
    env->DeleteLocalRef(minusBtn); env->DeleteLocalRef(mparam);

    jobject seekBar = env->NewObject(sbCls, sbCtor, activity);
    env->CallVoidMethod(seekBar, sbSetMax, 5000);
    env->CallVoidMethod(seekBar, sbSetProg, 2500);
    env->CallVoidMethod(seekBar, sbSetLstn, listenerView);
    if (sbSetProgTint && sbSetThumbTint && cslValueOf) {
        jobject progTint = env->CallStaticObjectMethod(cslCls, cslValueOf, (jint)0xFF00E5FF);
        jobject thumbTint = env->CallStaticObjectMethod(cslCls, cslValueOf, (jint)0xFFFF4FD8);
        env->CallVoidMethod(seekBar, sbSetProgTint, progTint);
        env->CallVoidMethod(seekBar, sbSetThumbTint, thumbTint);
        env->DeleteLocalRef(progTint);
        env->DeleteLocalRef(thumbTint);
    }
    jmethodID llpCtorW = env->GetMethodID(llpCls, "<init>", "(IIF)V");
    jobject sbParams = env->NewObject(llpCls, llpCtorW, 0, -2, 1.0f);
    jfieldID llpLeftM3 = env->GetFieldID(llpCls, "leftMargin", "I");
    jfieldID llpRightM = env->GetFieldID(llpCls, "rightMargin", "I");
    env->SetIntField(sbParams, llpLeftM3, 14);
    env->SetIntField(sbParams, llpRightM, 14);
    env->CallVoidMethod(sliderRow, llAddView, seekBar, sbParams);
    g_seekBarRef = env->NewGlobalRef(seekBar);
    env->DeleteLocalRef(seekBar); env->DeleteLocalRef(sbParams);

    jobject plusBtn = env->NewObject(tvCls, tvCtor, activity);
    jstring pTxt = env->NewStringUTF("+");
    env->CallVoidMethod(plusBtn, setText, pTxt); env->DeleteLocalRef(pTxt);
    env->CallVoidMethod(plusBtn, setTxtSz, 18.0f);
    env->CallVoidMethod(plusBtn, setTxtClr, (jint)0xFF00E5FF);
    env->CallVoidMethod(plusBtn, setGrav, 17);
    env->CallVoidMethod(plusBtn, setPad, 18, 4, 18, 4);
    env->CallVoidMethod(plusBtn, setId, PLUS_ID);
    env->CallVoidMethod(plusBtn, setClick, listenerView);
    {
        jobject pbg = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(pbg, gdSetCR, 16.0f);
        env->CallVoidMethod(pbg, gdSetColor, (jint)0xFF0D1428);
        env->CallVoidMethod(pbg, gdSetStroke, 1, (jint)0x9900E5FF);
        env->CallVoidMethod(plusBtn, setBg, pbg);
        env->DeleteLocalRef(pbg);
    }
    jobject pparam = env->NewObject(llpCls, llpCtor, -2, -2);
    env->CallVoidMethod(sliderRow, llAddView, plusBtn, pparam);
    env->DeleteLocalRef(plusBtn); env->DeleteLocalRef(pparam);

    jobject sliderRowParams = env->NewObject(llpCls, llpCtor, -1, -2);
    env->SetIntField(sliderRowParams, llpTopM, 8);
    env->SetIntField(sliderRowParams, llpBotM, 4);
    env->CallVoidMethod(panel, llAddView, sliderRow, sliderRowParams);
    env->DeleteLocalRef(sliderRow); env->DeleteLocalRef(sliderRowParams);

    jobject btnRow = env->NewObject(llCls, llCtor, activity);
    env->CallVoidMethod(btnRow, setOrient, 0);
    jmethodID llSetGrav = env->GetMethodID(llCls, "setGravity", "(I)V");
    env->CallVoidMethod(btnRow, llSetGrav, 17);

    auto makeBtn = [&](const char* text, jint id, jint bgColor, jint borderColor) -> jobject {
        jobject btn = env->NewObject(tvCls, tvCtor, activity);
        jstring t = env->NewStringUTF(text);
        env->CallVoidMethod(btn, setText, t);
        env->DeleteLocalRef(t);
        env->CallVoidMethod(btn, setTxtSz, 9.5f);
        env->CallVoidMethod(btn, setTxtClr, (jint)0xFFEEEEEE);
        env->CallVoidMethod(btn, setGrav, 17);
        env->CallVoidMethod(btn, setPad, 18, 14, 18, 14);
        env->CallVoidMethod(btn, setId, id);
        env->CallVoidMethod(btn, setClick, listenerView);
        env->CallVoidMethod(btn, setTypeface, boldTf);
        env->CallVoidMethod(btn, setLetterSp, 0.05f);
        jobject bg = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(bg, gdSetCR, 22.0f);
        env->CallVoidMethod(bg, gdSetColor, bgColor);
        env->CallVoidMethod(bg, gdSetStroke, 2, borderColor);
        env->CallVoidMethod(btn, setBg, bg);
        env->DeleteLocalRef(bg);
        return btn;
    };

    jobject realBtn   = makeBtn("REAL",   REAL_ID,   (jint)0xFF111B3E, (jint)0xFF00E5FF);
    jobject autoBtn   = makeBtn("AUTO",   AUTO_ID,   (jint)0xFF102516, (jint)0xFF39FF88);
    jobject humanBtn  = makeBtn("HUMAN",  HUMAN_ID,  (jint)0xFF23182F, (jint)0xFFFF4FD8);
    jobject visibBtn  = makeBtn("LINES",  VISIB_ID,  (jint)0xFF151822, (jint)0xFF9AA4FF);

    jobject hideBtn = env->NewObject(tvCls, tvCtor, activity);
    {
        jstring ht = env->NewStringUTF("\u23FB");
        env->CallVoidMethod(hideBtn, setText, ht); env->DeleteLocalRef(ht);
        env->CallVoidMethod(hideBtn, setTxtSz, 14.0f);
        env->CallVoidMethod(hideBtn, setTxtClr, (jint)0xFFFF4F6A);
        env->CallVoidMethod(hideBtn, setGrav, 17);
        env->CallVoidMethod(hideBtn, setPad, 16, 8, 16, 8);
        env->CallVoidMethod(hideBtn, setId, HIDE_ID);
        env->CallVoidMethod(hideBtn, setClick, listenerView);
        env->CallVoidMethod(hideBtn, setTypeface, boldTf);
        jobject hbg = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(hbg, gdSetCR, 12.0f);
        env->CallVoidMethod(hbg, gdSetColor, (jint)0xFF1C1016);
        env->CallVoidMethod(hbg, gdSetStroke, 1, (jint)0x88FF4F6A);
        env->CallVoidMethod(hideBtn, setBg, hbg);
        env->DeleteLocalRef(hbg);
    }

    jfieldID llpLeftM2 = env->GetFieldID(llpCls, "leftMargin", "I");
    jobject rp = env->NewObject(llpCls, llpCtor, -2, -2);
    env->SetIntField(rp, llpTopM, 14);
    env->CallVoidMethod(btnRow, llAddView, realBtn, rp);
    g_realBtn = env->NewGlobalRef(realBtn);
    env->DeleteLocalRef(realBtn); env->DeleteLocalRef(rp);
    jobject ap = env->NewObject(llpCls, llpCtor, -2, -2);
    env->SetIntField(ap, llpTopM, 14);
    env->SetIntField(ap, llpLeftM2, 8);
    env->CallVoidMethod(btnRow, llAddView, autoBtn, ap);
    g_autoBtn = env->NewGlobalRef(autoBtn);
    env->DeleteLocalRef(autoBtn); env->DeleteLocalRef(ap);
    jobject hup = env->NewObject(llpCls, llpCtor, -2, -2);
    env->SetIntField(hup, llpTopM, 14);
    env->SetIntField(hup, llpLeftM2, 8);
    env->CallVoidMethod(btnRow, llAddView, humanBtn, hup);
    g_humanBtn = env->NewGlobalRef(humanBtn);
    env->DeleteLocalRef(humanBtn); env->DeleteLocalRef(hup);
    jobject vp = env->NewObject(llpCls, llpCtor, -2, -2);
    env->SetIntField(vp, llpTopM, 14);
    env->SetIntField(vp, llpLeftM2, 8);
    env->CallVoidMethod(btnRow, llAddView, visibBtn, vp);
    g_visibBtn = env->NewGlobalRef(visibBtn);
    env->DeleteLocalRef(visibBtn); env->DeleteLocalRef(vp);
    jobject hp = env->NewObject(llpCls, llpCtor, -2, -2);
    env->SetIntField(hp, llpTopM, 14);
    env->SetIntField(hp, llpLeftM2, 8);
    env->CallVoidMethod(btnRow, llAddView, hideBtn, hp);
    g_hideBtn = env->NewGlobalRef(hideBtn);
    env->DeleteLocalRef(hideBtn); env->DeleteLocalRef(hp);

    jobject rowParams = env->NewObject(llpCls, llpCtor, -1, -2);
    env->CallVoidMethod(panel, llAddView, btnRow, rowParams);
    env->DeleteLocalRef(btnRow); env->DeleteLocalRef(rowParams);

    env->DeleteLocalRef(typefaceCls);
    env->DeleteLocalRef(boldTf);
    env->DeleteLocalRef(cslCls);

    env->CallVoidMethod(panel, setVis, 8);

    if (g_expiryLabel) { env->DeleteGlobalRef(g_expiryLabel); g_expiryLabel = nullptr; }
    jobject expLbl = env->NewObject(tvCls, tvCtor, activity);
    jstring expInit = env->NewStringUTF("");
    env->CallVoidMethod(expLbl, setText, expInit); env->DeleteLocalRef(expInit);
    env->CallVoidMethod(expLbl, setTxtSz, 9.0f);
    env->CallVoidMethod(expLbl, setTxtClr, (jint)0xFF777777);
    env->CallVoidMethod(expLbl, setGrav, 17);
    jobject expP = env->NewObject(llpCls, llpCtor, -1, -2);
    env->SetIntField(expP, llpTopM, 8);
    env->CallVoidMethod(panel, llAddView, expLbl, expP);
    g_expiryLabel = env->NewGlobalRef(expLbl);
    env->DeleteLocalRef(expLbl); env->DeleteLocalRef(expP);

    jobject panelParams = env->NewObject(flpCls, flpCtor, 540, -2);
    env->SetIntField(panelParams, flpGrav, 51);
    env->SetIntField(panelParams, flpTopM, 150);
    env->SetIntField(panelParams, flpLeftM, 20);
    env->CallVoidMethod(activity, addContent, panel, panelParams);
    g_panelLayout = env->NewGlobalRef(panel);
    env->DeleteLocalRef(panel);
    env->DeleteLocalRef(panelParams);

    time_t expTime = getExpiryTime(env);
    if (expTime > 0) startExpiryCountdown(env, expTime);
}

static void updateBtnText(jobject btn, const char* text) {
    if (!btn || !g_jvmOverlay) return;
    JNIEnv* env = nullptr;
    bool att = false;
    if (g_jvmOverlay->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvmOverlay->AttachCurrentThread(&env, nullptr); att = true;
    }
    if (env) {
        jclass tvCls = env->FindClass("android/widget/TextView");
        jmethodID setText = env->GetMethodID(tvCls, "setText", "(Ljava/lang/CharSequence;)V");
        jstring t = env->NewStringUTF(text);
        env->CallVoidMethod(btn, setText, t);
        env->DeleteLocalRef(t);
        env->DeleteLocalRef(tvCls);
    }
    if (att) g_jvmOverlay->DetachCurrentThread();
}

static void updateSeekBarProgress(int progress) {
    if (!g_seekBarRef || !g_jvmOverlay) return;
    JNIEnv* env = nullptr;
    bool att = false;
    if (g_jvmOverlay->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvmOverlay->AttachCurrentThread(&env, nullptr); att = true;
    }
    if (env) {
        jclass sbCls = env->FindClass("android/widget/SeekBar");
        jmethodID setProg = env->GetMethodID(sbCls, "setProgress", "(I)V");
        env->CallVoidMethod(g_seekBarRef, setProg, (jint)progress);
        env->DeleteLocalRef(sbCls);
    }
    if (att) g_jvmOverlay->DetachCurrentThread();
}

static void setBtnActive(JNIEnv* env, jobject btn, bool active, jint inactiveColor) {
    if (!btn) return;
    jclass gdCls = env->FindClass("android/graphics/drawable/GradientDrawable");
    jmethodID gdSetColor = env->GetMethodID(gdCls, "setColor", "(I)V");
    jmethodID gdSetStroke = env->GetMethodID(gdCls, "setStroke", "(II)V");
    jclass viewCls = env->FindClass("android/view/View");
    jmethodID getBg = env->GetMethodID(viewCls, "getBackground",
        "()Landroid/graphics/drawable/Drawable;");
    jobject bg = env->CallObjectMethod(btn, getBg);
    if (bg) {
        env->CallVoidMethod(bg, gdSetColor, active ? (jint)0xFF7B25FF : (jint)0x22101830);
        env->CallVoidMethod(bg, gdSetStroke, 2, active ? (jint)0xFFE0A1FF : (jint)0xAA553083);
        env->DeleteLocalRef(bg);
    }
    jclass tvCls = env->FindClass("android/widget/TextView");
    jmethodID setTxtClr = env->GetMethodID(tvCls, "setTextColor", "(I)V");
    jmethodID setText = env->GetMethodID(tvCls, "setText", "(Ljava/lang/CharSequence;)V");
    jstring mark = env->NewStringUTF(active ? "\xE2\x9C\x93" : "");
    env->CallVoidMethod(btn, setText, mark);
    env->DeleteLocalRef(mark);
    env->CallVoidMethod(btn, setTxtClr, (jint)0xFFFFFFFF);
    env->DeleteLocalRef(gdCls); env->DeleteLocalRef(viewCls); env->DeleteLocalRef(tvCls);
}

static void setGameTabActive(JNIEnv* env, jobject tab, bool active) {
    if (!tab) return;
    jclass viewCls = env->FindClass("android/view/View");
    jmethodID getBg = env->GetMethodID(viewCls, "getBackground",
        "()Landroid/graphics/drawable/Drawable;");
    jobject bg = env->CallObjectMethod(tab, getBg);
    if (bg) {
        jclass gdCls = env->FindClass("android/graphics/drawable/GradientDrawable");
        env->CallVoidMethod(bg, env->GetMethodID(gdCls, "setColor", "(I)V"),
            active ? (jint)0xAA3B147A : (jint)0x4A090D19);
        env->CallVoidMethod(bg, env->GetMethodID(gdCls, "setStroke", "(II)V"),
            1, active ? (jint)0xFFAF52FF : (jint)0x66706A91);
        env->DeleteLocalRef(gdCls);
        env->DeleteLocalRef(bg);
    }
    jclass tvCls = env->FindClass("android/widget/TextView");
    env->CallVoidMethod(tab, env->GetMethodID(tvCls, "setTextColor", "(I)V"),
        active ? (jint)0xFFF2ECFF : (jint)0xFF8F95B8);
    env->DeleteLocalRef(tvCls);
    env->DeleteLocalRef(viewCls);
}

static bool gameTabAllowed(GamePanelTab tab) {
    if (tab == GAME_TAB_POOL) return license_security_allows_game(GOAT_GAME_POOL);
    if (tab == GAME_TAB_SOLITAIRE) return license_security_allows_game(GOAT_GAME_SOLITAIRE);
    return license_security_allows_game(GOAT_GAME_ONOO);
}

static GamePanelTab firstAllowedGameTab() {
    if (license_security_allows_game(GOAT_GAME_POOL)) return GAME_TAB_POOL;
    if (license_security_allows_game(GOAT_GAME_SOLITAIRE)) return GAME_TAB_SOLITAIRE;
    return GAME_TAB_UNOO;
}

static void selectGameTab(JNIEnv* env, GamePanelTab selected) {
    if (!gameTabAllowed(selected)) selected = firstAllowedGameTab();
    g_selectedGameTab = selected;
    jclass viewCls = env->FindClass("android/view/View");
    jmethodID setVis = env->GetMethodID(viewCls, "setVisibility", "(I)V");
    if (g_poolContent) env->CallVoidMethod(g_poolContent, setVis, selected == GAME_TAB_POOL ? 0 : 8);
    if (g_solContent) env->CallVoidMethod(g_solContent, setVis, selected == GAME_TAB_SOLITAIRE ? 0 : 8);
    if (g_unoContent) env->CallVoidMethod(g_unoContent, setVis, selected == GAME_TAB_UNOO ? 0 : 8);
    setGameTabActive(env, g_poolTab, gameTabAllowed(GAME_TAB_POOL) && selected == GAME_TAB_POOL);
    setGameTabActive(env, g_solTab, gameTabAllowed(GAME_TAB_SOLITAIRE) && selected == GAME_TAB_SOLITAIRE);
    setGameTabActive(env, g_unoTab, gameTabAllowed(GAME_TAB_UNOO) && selected == GAME_TAB_UNOO);
    env->DeleteLocalRef(viewCls);
}

static void applyEntitlementUi(JNIEnv* env) {
    if (!env) return;
    const bool poolAllowed = license_security_allows_game(GOAT_GAME_POOL);
    const bool solAllowed = license_security_allows_game(GOAT_GAME_SOLITAIRE);
    const bool unoAllowed = license_security_allows_game(GOAT_GAME_ONOO);

    if (!poolAllowed) {
        g_ghost.botEnabled = false;
        g_ghost.botHumanMode = false;
        g_ghost.realForceEnabled = false;
        g_ghost.botState = GhostState::BOT_OFF;
        g_ghost.angleOverride = false;
        g_ghost.hideTrajectory = true;
        g_ghost.trajectoryHidden = true;
        setBtnActive(env, g_realBtn, false, (jint)0x00000000);
        setBtnActive(env, g_autoBtn, false, (jint)0x00000000);
        setBtnActive(env, g_humanBtn, false, (jint)0x00000000);
        setBtnActive(env, g_visibBtn, false, (jint)0x00000000);
    }
    if (!solAllowed) {
        solitaire_set_enabled(false);
        setBtnActive(env, g_solAutoBtn, false, (jint)0x00000000);
    }
    if (!unoAllowed) {
        uno_set_enabled(false);
        setBtnActive(env, g_unoAutoBtn, false, (jint)0x00000000);
    }

    selectGameTab(env, gameTabAllowed(g_selectedGameTab) ? g_selectedGameTab : firstAllowedGameTab());

    jclass viewCls = env->FindClass("android/view/View");
    if (!viewCls) {
        env->ExceptionClear();
        return;
    }
    jmethodID setVis = env->GetMethodID(viewCls, "setVisibility", "(I)V");
    if (setVis) {
        if (g_poolTab) env->CallVoidMethod(g_poolTab, setVis, poolAllowed ? 0 : 8);
        if (g_solTab) env->CallVoidMethod(g_solTab, setVis, solAllowed ? 0 : 8);
        if (g_unoTab) env->CallVoidMethod(g_unoTab, setVis, unoAllowed ? 0 : 8);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(viewCls);
}

static void adjustForce(JNIEnv* env, int delta) {
    int cur = (int)(g_ghost.sliderForceValue * 5000);
    cur += delta;
    if (cur < 0) cur = 0; if (cur > 5000) cur = 5000;
    g_ghost.sliderForceValue = (float)cur / 5000.0f;
    g_ghost.lastAimForce = -999.f;
    if (g_seekBarRef) {
        jclass sbCls = env->FindClass("android/widget/SeekBar");
        jmethodID setProg = env->GetMethodID(sbCls, "setProgress", "(I)V");
        env->CallVoidMethod(g_seekBarRef, setProg, (jint)cur);
        env->DeleteLocalRef(sbCls);
    }
    if (g_forceLabel) {
        char buf[32];
        snprintf(buf, sizeof(buf), "FORCE  %d%%", (int)(g_ghost.sliderForceValue * 100));
        jstring txt = env->NewStringUTF(buf);
        jclass tvCls = env->FindClass("android/widget/TextView");
        jmethodID setText = env->GetMethodID(tvCls, "setText", "(Ljava/lang/CharSequence;)V");
        env->CallVoidMethod(g_forceLabel, setText, txt);
        env->DeleteLocalRef(txt); env->DeleteLocalRef(tvCls);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnClick(JNIEnv*, jclass, jint);

static float  s_dragStartRawX = 0, s_dragStartRawY = 0;
static int    s_dragStartMarginL = 0, s_dragStartMarginT = 0;
static bool   s_isDragging = false;
#define DRAG_THRESHOLD 15

extern "C" JNIEXPORT jboolean JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnTouch(
    JNIEnv* env, jclass clz, jint viewId, jfloat rawX, jfloat rawY, jint action)
{
    if (viewId != CIRCLE_ID || !g_circleBtn) return JNI_FALSE;

    jclass viewCls = env->FindClass("android/view/View");
    jmethodID getLP = env->GetMethodID(viewCls, "getLayoutParams",
        "()Landroid/view/ViewGroup$LayoutParams;");
    jobject lp = env->CallObjectMethod(g_circleBtn, getLP);
    jclass flpCls = env->FindClass("android/widget/FrameLayout$LayoutParams");
    jfieldID topMF  = env->GetFieldID(flpCls, "topMargin", "I");
    jfieldID leftMF = env->GetFieldID(flpCls, "leftMargin", "I");

    int ACTION_DOWN = 0, ACTION_MOVE = 2, ACTION_UP = 1;

    if (action == ACTION_DOWN) {
        s_dragStartRawX = rawX;
        s_dragStartRawY = rawY;
        s_dragStartMarginL = env->GetIntField(lp, leftMF);
        s_dragStartMarginT = env->GetIntField(lp, topMF);
        s_isDragging = false;
    }
    else if (action == ACTION_MOVE) {
        float dx = rawX - s_dragStartRawX;
        float dy = rawY - s_dragStartRawY;
        if (!s_isDragging && (dx*dx + dy*dy) > DRAG_THRESHOLD * DRAG_THRESHOLD)
            s_isDragging = true;
        if (s_isDragging) {
            int newL = s_dragStartMarginL + (int)dx;
            int newT = s_dragStartMarginT + (int)dy;
            if (newL < 0) newL = 0;
            if (newT < 0) newT = 0;
            env->SetIntField(lp, leftMF, newL);
            env->SetIntField(lp, topMF, newT);
            jmethodID setLP = env->GetMethodID(viewCls, "setLayoutParams",
                "(Landroid/view/ViewGroup$LayoutParams;)V");
            env->CallVoidMethod(g_circleBtn, setLP, lp);

            if (g_ghost.panelVisible && g_panelLayout) {
                jobject panLP = env->CallObjectMethod(g_panelLayout, getLP);
                env->SetIntField(panLP, topMF, newT + 86);
                env->SetIntField(panLP, leftMF, newL);
                env->CallVoidMethod(g_panelLayout, setLP, panLP);
                env->DeleteLocalRef(panLP);
            }
        }
    }
    else if (action == ACTION_UP) {
        if (!s_isDragging) {
            env->DeleteLocalRef(lp);
            env->DeleteLocalRef(flpCls);
            env->DeleteLocalRef(viewCls);
            Java_com_google_firebase_TrainerOverlayView_nativeOnClick(env, clz, viewId);
            return JNI_TRUE;
        }
    }

    env->DeleteLocalRef(lp);
    env->DeleteLocalRef(flpCls);
    env->DeleteLocalRef(viewCls);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnClick(
    JNIEnv* env, jclass clz, jint viewId)
{
    if (viewId == LIC_BTN_ACTIVATE || viewId == LIC_BTN_COPY_DEVID || viewId == LIC_BTN_SUPPORT) {
        onLicenseClick(env, (int)viewId);
        return;
    }

    // No stale UI event can re-enable a feature after the key expires.
    if (!license_enforce_runtime(env)) return;

    const bool poolControl = viewId == REAL_ID || viewId == AUTO_ID || viewId == HUMAN_ID
        || viewId == VISIB_ID || viewId == MINUS_ID || viewId == PLUS_ID
        || viewId == HIDE_ID || viewId == MAXBALL_ID || viewId == POCKET_ID;
    if (poolControl && !license_security_allows_game(GOAT_GAME_POOL)) return;
    if (viewId == SOL_AUTO_ID && !license_security_allows_game(GOAT_GAME_SOLITAIRE)) return;
    if (viewId == UNO_AUTO_ID && !license_security_allows_game(GOAT_GAME_ONOO)) return;

    if (viewId == CIRCLE_ID && g_panelLayout) {
        g_ghost.panelVisible = !g_ghost.panelVisible;
        jclass viewCls = env->FindClass("android/view/View");
        jmethodID setVis = env->GetMethodID(viewCls, "setVisibility", "(I)V");

        if (g_ghost.panelVisible && g_circleBtn) {
            jmethodID getLP = env->GetMethodID(viewCls, "getLayoutParams",
                "()Landroid/view/ViewGroup$LayoutParams;");
            jclass flpCls = env->FindClass("android/widget/FrameLayout$LayoutParams");
            jfieldID topMF  = env->GetFieldID(flpCls, "topMargin", "I");
            jfieldID leftMF = env->GetFieldID(flpCls, "leftMargin", "I");

            jobject cirLP = env->CallObjectMethod(g_circleBtn, getLP);
            int cirT = env->GetIntField(cirLP, topMF);
            int cirL = env->GetIntField(cirLP, leftMF);
            env->DeleteLocalRef(cirLP);

            jobject panLP = env->CallObjectMethod(g_panelLayout, getLP);
            env->SetIntField(panLP, topMF, cirT + 86);
            env->SetIntField(panLP, leftMF, cirL);
            jmethodID setLP = env->GetMethodID(viewCls, "setLayoutParams",
                "(Landroid/view/ViewGroup$LayoutParams;)V");
            env->CallVoidMethod(g_panelLayout, setLP, panLP);
            env->DeleteLocalRef(panLP);
            env->DeleteLocalRef(flpCls);
        }

        env->CallVoidMethod(g_panelLayout, setVis, g_ghost.panelVisible ? 0 : 8);
        env->DeleteLocalRef(viewCls);
    }
    else if (viewId == POOL_TAB_ID) {
        selectGameTab(env, GAME_TAB_POOL);
    }
    else if (viewId == SOL_TAB_ID) {
        selectGameTab(env, GAME_TAB_SOLITAIRE);
    }
    else if (viewId == UNO_TAB_ID) {
        selectGameTab(env, GAME_TAB_UNOO);
    }
    else if (viewId == SOL_AUTO_ID) {
        const bool enabled = !solitaire_is_enabled();
        if (enabled && uno_is_enabled()) {
            uno_set_enabled(false);
            setBtnActive(env, g_unoAutoBtn, false, (jint)0x00000000);
        }
        solitaire_set_enabled(enabled);
        setBtnActive(env, g_solAutoBtn, enabled, (jint)0x00000000);
    }
    else if (viewId == UNO_AUTO_ID) {
        const bool enabled = !uno_is_enabled();
        if (enabled && solitaire_is_enabled()) {
            solitaire_set_enabled(false);
            setBtnActive(env, g_solAutoBtn, false, (jint)0x00000000);
        }
        uno_set_enabled(enabled);
        setBtnActive(env, g_unoAutoBtn, uno_is_enabled(), (jint)0x00000000);
    }
    else if (viewId == REAL_ID) {
        g_ghost.realForceEnabled = !g_ghost.realForceEnabled;
        if (!g_ghost.realForceEnabled) {
            g_ghost.angleOverride = false;
            if (g_ghost.botEnabled) {
                g_ghost.botEnabled = false;
                g_ghost.botState = GhostState::BOT_OFF;
                setBtnActive(env, g_autoBtn, false, (jint)0xFF102516);
            }
            if (g_ghost.botHumanMode) {
                g_ghost.botHumanMode = false;
                setBtnActive(env, g_humanBtn, false, (jint)0xFF23182F);
            }
        }
        setBtnActive(env, g_realBtn, g_ghost.realForceEnabled, (jint)0xFF111B3E);
    }
    else if (viewId == AUTO_ID) {
        g_ghost.botEnabled = !g_ghost.botEnabled;
        if (g_ghost.botEnabled) {
            g_ghost.realForceEnabled = true;
            g_ghost.botState = GhostState::BOT_OFF;
            g_ghost.botShotFired = false;
            g_ghost.botFoundShot = false;
            setBtnActive(env, g_realBtn, true, (jint)0xFF111B3E);
            bool gameActive = (g_ghost.gameId > 1000000 &&
                               g_ghost.lastPoolGameController != nullptr &&
                               (!g_ghost.freshEncPropsString.empty() || g_ghost.capturedLen > 100));
            if (gameActive && !g_ghost.animating) {
                g_ghost.botState = GhostState::BOT_WAIT_TURN;
                g_ghost.botStateStartMs = nowMs();
                g_ghost.botActionDelayMs = g_ghost.botHumanMode ? 180 + (rand() % 260) : 80 + (rand() % 140);
            }
        } else {
            g_ghost.botState = GhostState::BOT_OFF;
            g_ghost.angleOverride = false;
            if (g_ghost.botHumanMode) {
                g_ghost.botHumanMode = false;
                setBtnActive(env, g_humanBtn, false, (jint)0xFF23182F);
            }
        }
        setBtnActive(env, g_autoBtn, g_ghost.botEnabled, (jint)0xFF102516);
    }
    else if (viewId == HUMAN_ID) {
        g_ghost.botHumanMode = !g_ghost.botHumanMode;
        if (g_ghost.botHumanMode) {
            g_ghost.realForceEnabled = true;
            g_ghost.botEnabled = true;
            g_ghost.botState = GhostState::BOT_OFF;
            g_ghost.botShotFired = false;
            g_ghost.botFoundShot = false;
            setBtnActive(env, g_realBtn, true, (jint)0xFF111B3E);
            setBtnActive(env, g_autoBtn, true, (jint)0xFF102516);
            bool gameActive = (g_ghost.gameId > 1000000 &&
                               g_ghost.lastPoolGameController != nullptr &&
                               (!g_ghost.freshEncPropsString.empty() || g_ghost.capturedLen > 100));
            if (gameActive && !g_ghost.animating) {
                g_ghost.botState = GhostState::BOT_WAIT_TURN;
                g_ghost.botStateStartMs = nowMs();
                g_ghost.botActionDelayMs = 180 + (rand() % 260);
            }
        }
        setBtnActive(env, g_humanBtn, g_ghost.botHumanMode, (jint)0xFF23182F);
    }
    else if (viewId == MINUS_ID) {
        adjustForce(env, -10);
    }
    else if (viewId == PLUS_ID) {
        adjustForce(env, 10);
    }
    else if (viewId == HIDE_ID) {
        g_ghost.trajectoryHidden = !g_ghost.trajectoryHidden;
        setBtnActive(env, g_hideBtn, g_ghost.trajectoryHidden, (jint)0xFF1C1016);
    }
    else if (viewId == MAXBALL_ID) {
        g_maxBallsUiEnabled = !g_maxBallsUiEnabled;
        setBtnActive(env, g_maxBallBtn, g_maxBallsUiEnabled, (jint)0x00000000);
    }
    else if (viewId == POCKET_ID) {
        g_autoPocketUiEnabled = !g_autoPocketUiEnabled;
        setBtnActive(env, g_pocketBtn, g_autoPocketUiEnabled, (jint)0x00000000);
    }
    else if (viewId == VISIB_ID) {
        g_ghost.hideTrajectory = !g_ghost.hideTrajectory;
        setBtnActive(env, g_visibBtn, !g_ghost.hideTrajectory, (jint)0x00000000);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_firebase_TrainerOverlayView_nativeOnForceChanged(
    JNIEnv* env, jclass clz, jint progress)
{
    if (!license_enforce_runtime(env)) return;
    if (!license_security_allows_game(GOAT_GAME_POOL)) return;
    float force = (float)progress / 5000.0f;
    g_ghost.sliderForceValue = force;
    g_ghost.lastAimForce = -999.f;

    if (g_forceLabel) {
        char buf[32];
        snprintf(buf, sizeof(buf), "FORCE  %d%%", (int)(force * 100));
        jstring txt = env->NewStringUTF(buf);
        jclass tvCls = env->FindClass("android/widget/TextView");
        jmethodID setText = env->GetMethodID(tvCls, "setText", "(Ljava/lang/CharSequence;)V");
        env->CallVoidMethod(g_forceLabel, setText, txt);
        env->DeleteLocalRef(txt);
        env->DeleteLocalRef(tvCls);
    }
}
