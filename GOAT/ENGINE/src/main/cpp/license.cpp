
#include "include/license.h"
#include "include/license_security.h"
#include <jni.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include "obfuscate.h"

#define K_URL1     0x837E89F7AAFF7EF5ULL
#define K_URL2     0xCF71CBD6B1886576ULL
#define K_H1       0xAFC2BC79CAFDE3E9ULL
#define K_H2       0xB1C793BA55575151ULL
#define K_H3       0x97966174338D7A62ULL
#define K_H4       0xCAA58A32FEBE9612ULL
#define K_SALT     0x9475BAC19D3EAB5DULL
#define K_PF       0x956CC68F668474DAULL
#define K_PK       0xB9A153E39B49A426ULL
#define K_PE       0xBBFD6690FA42131DULL
#define K_AID      0x9458DFA899C2CF8BULL
#define K_POST     0xB0C2881C33BB824EULL
#define K_CT       0x925FFD318172C33AULL
#define K_JSON     0x8F71BDA51A653E7BULL
#define K_BODY     0xA4B7993F2C1ED3D3ULL
#define K_EXP      0xEFC6E898706B374BULL
#define K_HMM      0xBF2C7E7CE98185F5ULL
#define K_REV      0xCF8AFDD0E1CF4B87ULL
#define K_INVK     0xC712733838E86E97ULL
#define K_DVID     0x9D67F9E740CCEFE7ULL
#define K_PLIC     0xD372BF4F2762E36DULL
#define K_IFMT     0xFACCD9C7CCB1A51FULL
#define K_CONT     0xDAA89AA3EB9C982CULL
#define K_MONO     0xCA703097CC5C8E31ULL
#define K_CPYD     0xEC8B1AA27864FA29ULL
#define K_DLBL     0xDF47B647EA61F1A2ULL
#define K_COPY     0xB2767DCB79A4F0D8ULL
#define K_ACKY     0xC1DC51778ADCB8B1ULL
#define K_HINT     0xAF56F49AF9F23D6BULL
#define K_ACTV     0x88FACF13247CEB8FULL
#define K_MADE     0xBFB29D50EFED11DBULL

// Protocol v2 never embeds a server secret in the client. These legacy
// helpers deliberately resolve to empty strings and are removed by section
// garbage collection because the production validation path no longer calls
// them.
static const char* getValidateUrl()  { return ""; }
static const char* getHmacSecret()   { return ""; }
static const char* getPrefsFile()    { return AY_OBFUSCATE_KEY("gp_s", K_PF); }
static const char* getPrefsKey()     { return AY_OBFUSCATE_KEY("k0", K_PK); }
static const char* getPrefsExpiry()  { return AY_OBFUSCATE_KEY("k1", K_PE); }

// The signed server lease lasts 30 minutes. Renew well before it expires so a
// valid player never loses access during a long game.
static constexpr int REVALIDATE_SECONDS = 10 * 60;

std::atomic<bool> g_licenseValidated{false};
static JavaVM*  s_jvm         = nullptr;
static jobject  s_activity    = nullptr;
static jobject  s_listener    = nullptr;
static jobject  s_container   = nullptr;
static jobject  s_keyField    = nullptr;
static jobject  s_statusTv    = nullptr;
static std::string s_deviceId;
static time_t   s_lastValidated = 0;

static std::atomic<int>  s_pendingResult{-1};
static std::string       s_pendingExpiry;
static std::mutex        s_pendingMux;
static std::atomic<bool> s_validating{false};
static std::atomic<bool> s_revalidateThreadStarted{false};
static std::atomic<bool> s_forceReloginPending{false};
static std::atomic<time_t> s_expiryEpoch{0};


static constexpr jint COL_GOLD        = (jint)0xFF9B4DFF;
static constexpr jint COL_GOLD_DIM    = (jint)0xFF211036;
static constexpr jint COL_GOLD_BORDER = (jint)0xFF8A3DFF;
static constexpr jint COL_BG_CARD     = (jint)0xF20A0D18;
static constexpr jint COL_BG_SECTION  = (jint)0xE6111522;
static constexpr jint COL_BG_OVERLAY  = (jint)0xE9000008;
static constexpr jint COL_TEXT_PRI    = (jint)0xFFEEEEEE;
static constexpr jint COL_TEXT_SEC    = (jint)0xFF9BA0B8;
static constexpr jint COL_TEXT_FAINT  = (jint)0xFF2A2A2A;
static constexpr jint COL_SUCCESS     = (jint)0xFF4CAF50;
static constexpr jint COL_ERROR       = (jint)0xFFEF5350;
static constexpr jint COL_INFO        = (jint)0xFF9BA0B8;

extern void startMainPanel(JNIEnv* env, jobject activity, jobject listenerView);
extern void disableTrajectories();
extern void showExpiredLicenseUi(JNIEnv* env);

static time_t parseExpiryUtc(const std::string& value) {
    if (value.empty()) return 0;
    int Y=0, M=0, D=0, h=0, m=0, sec=0;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;
    struct tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = sec;
    return timegm(&t);
}

static void requestLicenseUiTick(JNIEnv* env) {
    if (!env || !s_listener) return;
    jclass vCls = env->FindClass("android/view/View");
    if (!vCls) { env->ExceptionClear(); return; }
    jmethodID postInvalidate = env->GetMethodID(vCls, "postInvalidate", "()V");
    if (postInvalidate) env->CallVoidMethod(s_listener, postInvalidate);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(vCls);
}

static uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define S1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define G0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define G1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_compress(uint32_t s[8], const uint8_t* blk) {
    uint32_t w[64]; uint32_t a,b,c,d,e,f,g,h,t1,t2;
    for(int i=0;i<16;i++) w[i]=((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for(int i=16;i<64;i++) w[i]=G1(w[i-2])+w[i-7]+G0(w[i-15])+w[i-16];
    a=s[0];b=s[1];c=s[2];d=s[3];e=s[4];f=s[5];g=s[6];h=s[7];
    for(int i=0;i<64;i++){t1=h+S1(e)+CH(e,f,g)+sha256_k[i]+w[i];t2=S0(a)+MAJ(a,b,c);h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}

static void hmac_sha256(const char* key, int klen,
                         const char* msg, int mlen,
                         uint8_t out[32]) {
    uint8_t k[64]={}, ipad[64], opad[64];
    if(klen>64){} else memcpy(k,key,klen);
    for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}

    uint32_t si[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t blk[64];
    memcpy(blk,ipad,64); sha256_compress(si,blk);
    int off=0;
    while(off+64<=mlen){memcpy(blk,(uint8_t*)msg+off,64);sha256_compress(si,blk);off+=64;}
    int rem=mlen-off;
    memset(blk,0,64); memcpy(blk,(uint8_t*)msg+off,rem);
    blk[rem]=0x80;
    uint64_t bitlen_inner=(uint64_t)(64+mlen)*8;
    if(rem>=56){sha256_compress(si,blk);memset(blk,0,64);}
    for(int i=0;i<8;i++) blk[56+i]=(uint8_t)(bitlen_inner>>((7-i)*8));
    sha256_compress(si,blk);
    uint8_t inner[32];
    for(int i=0;i<8;i++){inner[i*4]=(uint8_t)(si[i]>>24);inner[i*4+1]=(uint8_t)(si[i]>>16);inner[i*4+2]=(uint8_t)(si[i]>>8);inner[i*4+3]=(uint8_t)si[i];}

    uint32_t so[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(blk,opad,64); sha256_compress(so,blk);
    memset(blk,0,64); memcpy(blk,inner,32); blk[32]=0x80;
    uint64_t bitlen_outer=(uint64_t)(64+32)*8;
    for(int i=0;i<8;i++) blk[56+i]=(uint8_t)(bitlen_outer>>((7-i)*8));
    sha256_compress(so,blk);
    for(int i=0;i<8;i++){out[i*4]=(uint8_t)(so[i]>>24);out[i*4+1]=(uint8_t)(so[i]>>16);out[i*4+2]=(uint8_t)(so[i]>>8);out[i*4+3]=(uint8_t)so[i];}
}

static std::string hexlower(const uint8_t* d,int n){
    static const char h[]="0123456789abcdef";
    std::string s; s.resize(n*2);
    for(int i=0;i<n;i++){s[i*2]=h[d[i]>>4];s[i*2+1]=h[d[i]&0xf];}
    return s;
}

static bool verifyHmac(const std::string& resp, const std::string& sig) {
    auto extract = [&](const char* key) -> std::string {
        std::string search = std::string("\"") + key + "\":";
        size_t p = resp.find(search);
        if (p == std::string::npos) return "";
        p += search.size();
        if (p >= resp.size()) return "";
        if (resp[p] == '"') {
            size_t s = p + 1;
            size_t e = resp.find('"', s);
            if (e == std::string::npos) return "";
            return resp.substr(s, e - s);
        } else {
            size_t e = resp.find_first_of(",}", p);
            if (e == std::string::npos) e = resp.size();
            std::string v = resp.substr(p, e - p);
            while (!v.empty() && v.front()==' ') v.erase(0,1);
            while (!v.empty() && v.back()==' ')  v.pop_back();
            return v;
        }
    };

    std::string validStr  = extract("valid");
    std::string reason    = extract("reason");
    std::string expires   = extract("expires");

    std::string validBit  = (validStr == "true") ? "1" : "0";
    std::string canonical = validBit + "|" + reason + "|" + expires;

    uint8_t computed[32];
    hmac_sha256(getHmacSecret(), (int)strlen(getHmacSecret()),
                canonical.c_str(), (int)canonical.size(), computed);
    return (sig == hexlower(computed, 32));
}

static std::string xorWithDeviceId(const std::string& data, const std::string& devId) {
    if (devId.empty() || data.empty()) return data;
    std::string out = data;
    for (size_t i = 0; i < out.size(); i++)
        out[i] ^= devId[i % devId.size()];
    return out;
}

static std::string encodeHex(const std::string& s) {
    static const char h[] = "0123456789ABCDEF";
    std::string out; out.resize(s.size() * 2);
    for (size_t i = 0; i < s.size(); i++) {
        out[i*2]   = h[(uint8_t)s[i] >> 4];
        out[i*2+1] = h[(uint8_t)s[i] & 0xF];
    }
    return out;
}
static std::string decodeHex(const std::string& s) {
    std::string out; out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        auto hv = [](char c) -> uint8_t {
            if (c>='0'&&c<='9') return c-'0';
            if (c>='A'&&c<='F') return c-'A'+10;
            if (c>='a'&&c<='f') return c-'a'+10;
            return 0;
        };
        out[i] = (char)((hv(s[i*2]) << 4) | hv(s[i*2+1]));
    }
    return out;
}

static std::string encryptPref(const std::string& plain, const std::string& devId) {
    return encodeHex(xorWithDeviceId(plain, devId));
}
static std::string decryptPref(const std::string& encoded, const std::string& devId) {
    if (encoded.size() % 2 != 0) return "";
    return xorWithDeviceId(decodeHex(encoded), devId);
}

static bool checkSuspiciousMaps() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512]; bool found = false;
    const char* suspects[] = {"frida", "gadget", "linjector", "xposed", "substrate", nullptr};
    while (fgets(line, sizeof(line), f)) {
        for (int i = 0; suspects[i]; i++) {
            if (strstr(line, suspects[i])) { found = true; break; }
        }
        if (found) break;
    }
    fclose(f);
    return found;
}

static bool isEnvironmentSuspicious() {
    if (checkSuspiciousMaps()) return true;

    return false;
}

static std::string getJavaStaticString(JNIEnv* env, const char* cls, const char* field) {
    jclass c = env->FindClass(cls);
    if (!c) return "";
    jfieldID f = env->GetStaticFieldID(c, field, "Ljava/lang/String;");
    if (!f) { env->DeleteLocalRef(c); return ""; }
    jstring js = (jstring)env->GetStaticObjectField(c, f);
    env->DeleteLocalRef(c);
    if (!js) return "";
    const char* ch = env->GetStringUTFChars(js, nullptr);
    std::string out(ch ? ch : "");
    env->ReleaseStringUTFChars(js, ch);
    env->DeleteLocalRef(js);
    return out;
}

static std::string buildCompositeHwid(JNIEnv* env, const std::string& androidId) {
    std::string fp    = getJavaStaticString(env, "android/os/Build", "FINGERPRINT");
    std::string board = getJavaStaticString(env, "android/os/Build", "BOARD");
    std::string raw   = androidId + "|" + fp + "|" + board;

    uint8_t hash[32];
    hmac_sha256("hwid", 4, raw.c_str(), (int)raw.size(), hash);
    return hexlower(hash, 8);
}

static jobject openPrefs(JNIEnv* env) {
    jclass ac = env->GetObjectClass(s_activity);
    jmethodID gp = env->GetMethodID(ac, "getSharedPreferences",
        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
    jstring nm = env->NewStringUTF(getPrefsFile());
    jobject pr = env->CallObjectMethod(s_activity, gp, nm, 0);
    env->DeleteLocalRef(nm); env->DeleteLocalRef(ac);
    return pr;
}

static std::string prefsGetString(JNIEnv* env, const char* key) {
    jobject pr = openPrefs(env);
    if (!pr) return "";
    jclass pc = env->GetObjectClass(pr);
    jmethodID gs = env->GetMethodID(pc, "getString",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jstring k = env->NewStringUTF(key);
    jstring def = env->NewStringUTF("");
    jstring r = (jstring)env->CallObjectMethod(pr, gs, k, def);
    env->DeleteLocalRef(k); env->DeleteLocalRef(def);
    env->DeleteLocalRef(pc); env->DeleteLocalRef(pr);
    if (!r) return "";
    const char* c = env->GetStringUTFChars(r, nullptr);
    std::string out(c ? c : "");
    env->ReleaseStringUTFChars(r, c);
    env->DeleteLocalRef(r);
    return out;
}

static void prefsPutString(JNIEnv* env, const char* key, const char* val) {
    jobject pr = openPrefs(env);
    if (!pr) return;
    jclass pc = env->GetObjectClass(pr);
    jmethodID edit = env->GetMethodID(pc, "edit",
        "()Landroid/content/SharedPreferences$Editor;");
    jobject ed = env->CallObjectMethod(pr, edit);
    env->DeleteLocalRef(pc); env->DeleteLocalRef(pr);
    if (!ed) return;
    jclass ec = env->GetObjectClass(ed);
    jmethodID ps = env->GetMethodID(ec, "putString",
        "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;");
    jmethodID apply = env->GetMethodID(ec, "apply", "()V");
    jstring jk = env->NewStringUTF(key);
    jstring jv = env->NewStringUTF(val);
    env->CallObjectMethod(ed, ps, jk, jv);
    env->CallVoidMethod(ed, apply);
    env->DeleteLocalRef(jk); env->DeleteLocalRef(jv);
    env->DeleteLocalRef(ec); env->DeleteLocalRef(ed);
}

static std::string loadSavedKey(JNIEnv* env) {
    std::string enc = prefsGetString(env, getPrefsKey());
    if (enc.empty()) return "";
    return decryptPref(enc, s_deviceId);
}
static std::string loadSavedExpiry(JNIEnv* env) {
    std::string enc = prefsGetString(env, getPrefsExpiry());
    if (enc.empty()) return "";
    return decryptPref(enc, s_deviceId);
}
static void saveKey(JNIEnv* env, const std::string& v) {
    prefsPutString(env, getPrefsKey(), encryptPref(v, s_deviceId).c_str());
}
static void saveExpiry(JNIEnv* env, const std::string& v) {
    prefsPutString(env, getPrefsExpiry(), encryptPref(v, s_deviceId).c_str());
    s_expiryEpoch.store(parseExpiryUtc(v));
}
static void clearSavedCreds(JNIEnv* env) {
    prefsPutString(env, getPrefsKey(),    "");
    prefsPutString(env, getPrefsExpiry(), "");
    s_expiryEpoch.store(0);
}

static std::string getDeviceId(JNIEnv* env) {
    if (!s_deviceId.empty()) return s_deviceId;

    jclass sec = env->FindClass("android/provider/Settings$Secure");
    if (!sec) return "UNKNOWN";
    jmethodID gs = env->GetStaticMethodID(sec, "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    jclass ac = env->GetObjectClass(s_activity);
    jobject res = env->CallObjectMethod(s_activity,
        env->GetMethodID(ac, "getContentResolver", "()Landroid/content/ContentResolver;"));
    env->DeleteLocalRef(ac);
    jstring k = env->NewStringUTF(AY_OBFUSCATE_KEY("android_id", K_AID));
    jstring r = (jstring)env->CallStaticObjectMethod(sec, gs, res, k);
    env->DeleteLocalRef(k); env->DeleteLocalRef(res); env->DeleteLocalRef(sec);
    std::string androidId;
    if (r) {
        const char* c = env->GetStringUTFChars(r, nullptr);
        androidId = c ? c : "";
        env->ReleaseStringUTFChars(r, c);
        env->DeleteLocalRef(r);
    }

    s_deviceId = buildCompositeHwid(env, androidId);
    for (char& ch : s_deviceId) ch = (char)toupper((unsigned char)ch);
    return s_deviceId;
}

int validateKey(const std::string& raw) {
    std::string k;
    for (char c : raw) if (c != '-') k += (char)toupper((unsigned char)c);
    if (k.size() != 16) return 0;
    for (char c : k) if (!isalnum((unsigned char)c)) return 0;
    return 1;
}
static std::string normalizeKey(const std::string& raw) {
    std::string k;
    for (char c : raw) if (c != '-') k += (char)toupper((unsigned char)c);
    if (k.size() != 16) return "";
    return k.substr(0,4)+"-"+k.substr(4,4)+"-"+k.substr(8,4)+"-"+k.substr(12,4);
}

static std::string httpPost(JNIEnv* env, const char* urlStr, const char* jsonBody) {
    std::string result;
    jclass urlCls = env->FindClass("java/net/URL");
    jstring jurl = env->NewStringUTF(urlStr);
    jobject urlObj = env->NewObject(urlCls,
        env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V"), jurl);
    env->DeleteLocalRef(jurl);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(urlCls); return ""; }
    jobject conn = env->CallObjectMethod(urlObj,
        env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;"));
    env->DeleteLocalRef(urlObj); env->DeleteLocalRef(urlCls);
    if (!conn || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jclass httpCls = env->FindClass("java/net/HttpURLConnection");
    env->CallVoidMethod(conn, env->GetMethodID(httpCls, "setRequestMethod","(Ljava/lang/String;)V"),
        env->NewStringUTF(AY_OBFUSCATE_KEY("POST", K_POST)));
    env->CallVoidMethod(conn, env->GetMethodID(httpCls, "setDoOutput","(Z)V"), JNI_TRUE);
    env->CallVoidMethod(conn, env->GetMethodID(httpCls, "setConnectTimeout","(I)V"), (jint)6000);
    env->CallVoidMethod(conn, env->GetMethodID(httpCls, "setReadTimeout","(I)V"),    (jint)8000);
    jmethodID setRP = env->GetMethodID(httpCls, "setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
    { jstring a=env->NewStringUTF(AY_OBFUSCATE_KEY("Content-Type", K_CT)), b=env->NewStringUTF(AY_OBFUSCATE_KEY("application/json", K_JSON));
      env->CallVoidMethod(conn, setRP, a, b); env->DeleteLocalRef(a); env->DeleteLocalRef(b); }

    jobject os = env->CallObjectMethod(conn, env->GetMethodID(httpCls,"getOutputStream","()Ljava/io/OutputStream;"));
    if (os && !env->ExceptionCheck()) {
        jclass oc = env->GetObjectClass(os);
        int len = (int)strlen(jsonBody);
        jbyteArray ba = env->NewByteArray(len);
        env->SetByteArrayRegion(ba, 0, len, (const jbyte*)jsonBody);
        env->CallVoidMethod(os, env->GetMethodID(oc,"write","([B)V"), ba);
        env->CallVoidMethod(os, env->GetMethodID(oc,"flush","()V"));
        env->CallVoidMethod(os, env->GetMethodID(oc,"close","()V"));
        env->DeleteLocalRef(ba); env->DeleteLocalRef(oc); env->DeleteLocalRef(os);
    } else { if (env->ExceptionCheck()) env->ExceptionClear(); }

    jint code = env->CallIntMethod(conn, env->GetMethodID(httpCls,"getResponseCode","()I"));
    if (!env->ExceptionCheck()) {
        jmethodID getIS = (code==200)
            ? env->GetMethodID(httpCls,"getInputStream","()Ljava/io/InputStream;")
            : env->GetMethodID(httpCls,"getErrorStream","()Ljava/io/InputStream;");
        jobject is = env->CallObjectMethod(conn, getIS);
        if (is && !env->ExceptionCheck()) {
            jclass isrC = env->FindClass("java/io/InputStreamReader");
            jclass  brC = env->FindClass("java/io/BufferedReader");
            jobject isr = env->NewObject(isrC, env->GetMethodID(isrC,"<init>","(Ljava/io/InputStream;)V"), is);
            jobject  br = env->NewObject(brC,  env->GetMethodID(brC, "<init>","(Ljava/io/Reader;)V"), isr);
            jmethodID rl = env->GetMethodID(brC,"readLine","()Ljava/lang/String;");
            while(true){
                jstring line=(jstring)env->CallObjectMethod(br,rl);
                if(!line||env->ExceptionCheck()){if(env->ExceptionCheck())env->ExceptionClear();break;}
                const char* c=env->GetStringUTFChars(line,nullptr);
                if(c) result+=c;
                env->ReleaseStringUTFChars(line,c); env->DeleteLocalRef(line);
            }
            env->CallVoidMethod(br, env->GetMethodID(brC,"close","()V"));
            env->DeleteLocalRef(br); env->DeleteLocalRef(isr);
            env->DeleteLocalRef(brC); env->DeleteLocalRef(isrC); env->DeleteLocalRef(is);
        } else { if(env->ExceptionCheck()) env->ExceptionClear(); }
    } else { env->ExceptionClear(); }

    env->CallVoidMethod(conn, env->GetMethodID(httpCls,"disconnect","()V"));
    env->DeleteLocalRef(conn); env->DeleteLocalRef(httpCls);
    return result;
}

static int validateKeyOnline(JNIEnv* env, const std::string& key, std::string& outExpiry) {
    std::string norm  = normalizeKey(key);
    if (norm.empty()) return 3;
    return license_security_validate(env, s_activity, norm, getDeviceId(env), outExpiry);
}

static bool beginOnlineValidation(const std::string& key, bool persistKeyOnSuccess) {
    bool expected = false;
    if (!s_validating.compare_exchange_strong(expected, true)) return false;

    std::thread([key, persistKeyOnSuccess]() {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (s_jvm && s_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            s_jvm->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
        if (env) {
            std::string expiry;
            const int result = validateKeyOnline(env, key, expiry);
            if (result == 1 && persistKeyOnSuccess) saveKey(env, key);
            {
                std::lock_guard<std::mutex> lock(s_pendingMux);
                s_pendingExpiry = expiry;
            }
            s_pendingResult.store(result);
            requestLicenseUiTick(env);
        } else {
            s_pendingResult.store(0);
        }
        if (attached) s_jvm->DetachCurrentThread();
    }).detach();
    return true;
}

static void startPeriodicRevalidation() {
    bool expected = false;
    if (!s_revalidateThreadStarted.compare_exchange_strong(expected, true)) return;
    std::thread([]() {
        while (true) {
            sleep(REVALIDATE_SECONDS);
            if (!g_licenseValidated.load()) continue;

            JNIEnv* env = nullptr; bool att = false;
            if (s_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
                s_jvm->AttachCurrentThread(&env, nullptr); att = true;
            }
            if (env) {
                std::string key    = loadSavedKey(env);
                std::string expiry;
                int res = key.empty() ? 3 : validateKeyOnline(env, key, expiry);
                if (res == 1) {
                    s_lastValidated = time(nullptr);
                    if (!expiry.empty()) saveExpiry(env, expiry);
                } else if (res == 0 && license_security_has_access()) {
                    // A short network outage does not cancel an otherwise
                    // authentic lease. The next renewal retries in 10 min.
                } else {
                    // JNI UI and feature shutdown are completed on the main
                    // drawing thread.  This avoids touching Android views or
                    // game state from the network worker.
                    license_security_revoke();
                    s_forceReloginPending.store(true);
                    requestLicenseUiTick(env);
                }
            }
            if (att) s_jvm->DetachCurrentThread();
        }
    }).detach();
}

static void setStatus(JNIEnv* env, const char* text, jint color) {
    if (!s_statusTv) return;
    jclass tc = env->FindClass("android/widget/TextView");
    jstring jt = env->NewStringUTF(text);
    env->CallVoidMethod(s_statusTv, env->GetMethodID(tc,"setText","(Ljava/lang/CharSequence;)V"), jt);
    env->CallVoidMethod(s_statusTv, env->GetMethodID(tc,"setTextColor","(I)V"), color);
    env->DeleteLocalRef(jt); env->DeleteLocalRef(tc);
}

static void dismissDialog(JNIEnv* env) {
    if (!s_container) return;
    jclass vc  = env->FindClass("android/view/View");
    jclass vgc = env->FindClass("android/view/ViewGroup");
    jobject par = env->CallObjectMethod(s_container,
        env->GetMethodID(vc, "getParent", "()Landroid/view/ViewParent;"));
    if (par) {
        env->CallVoidMethod(par, env->GetMethodID(vgc,"removeView","(Landroid/view/View;)V"), s_container);
        env->DeleteLocalRef(par);
    }
    env->DeleteLocalRef(vc); env->DeleteLocalRef(vgc);
    env->DeleteGlobalRef(s_container); s_container = nullptr;
    if (s_keyField) { env->DeleteGlobalRef(s_keyField); s_keyField = nullptr; }
    if (s_statusTv) { env->DeleteGlobalRef(s_statusTv); s_statusTv = nullptr; }
}

void license_checkPendingResult(JNIEnv* env) {
    int res = s_pendingResult.exchange(-1);
    if (res == -1) return;
    s_validating.store(false);

    if (res == 1) {
        std::string expiry;
        { std::lock_guard<std::mutex> lk(s_pendingMux); expiry = s_pendingExpiry; }
        const time_t expiryEpoch = parseExpiryUtc(expiry);
        if (expiryEpoch <= time(nullptr)) {
            g_licenseValidated.store(false);
            clearSavedCreds(env);
            setStatus(env, "\u2717  This license key has expired", COL_ERROR);
            return;
        }
        saveExpiry(env, expiry);
        if (!license_security_has_access()) {
            g_licenseValidated.store(false);
            setStatus(env, "\u2717  Security session rejected \u2014 contact support", COL_ERROR);
            return;
        }
        g_licenseValidated.store(true);
        s_lastValidated = time(nullptr);
        startPeriodicRevalidation();
        setStatus(env, "\u2713  License activated successfully", COL_SUCCESS);
        usleep(600000);
        dismissDialog(env);
        startMainPanel(env, s_activity, s_listener);
    } else if (res == 0) {
        setStatus(env, "Network error \u2014 check your connection", COL_ERROR);
    } else if (res == 2) {
        setStatus(env, "\u2717  This license key has expired", COL_ERROR);
    } else if (res == 4) {
        setStatus(env, "\u2717  Device ID mismatch \u2014 contact support", COL_ERROR);
    } else if (res == 5) {
        setStatus(env, "\u2717  License has been revoked", COL_ERROR);
    } else if (res == 6) {
        setStatus(env, "\u2717  Security error \u2014 contact support", COL_ERROR);
    } else {
        setStatus(env, AY_OBFUSCATE_KEY("\u2717  Invalid license key", K_INVK), COL_ERROR);
    }
}

void showLicenseDialog(JNIEnv* env) {
    // Expiry/revocation checks can arrive from more than one redraw.  Never
    // stack duplicate login overlays on top of each other.
    if (s_container) return;

    jclass viewCls = env->FindClass("android/view/View");
    jclass tvCls   = env->FindClass("android/widget/TextView");
    jclass etCls   = env->FindClass("android/widget/EditText");
    jclass llCls   = env->FindClass("android/widget/LinearLayout");
    jclass flCls   = env->FindClass("android/widget/FrameLayout");
    jclass llpCls  = env->FindClass("android/widget/LinearLayout$LayoutParams");
    jclass flpCls  = env->FindClass("android/widget/FrameLayout$LayoutParams");
    jclass gdCls   = env->FindClass("android/graphics/drawable/GradientDrawable");
    jclass tfCls   = env->FindClass("android/graphics/Typeface");
    jclass actCls  = env->FindClass("android/app/Activity");

    jmethodID viewCtor = env->GetMethodID(viewCls,"<init>","(Landroid/content/Context;)V");
    jmethodID tvCtor   = env->GetMethodID(tvCls,  "<init>","(Landroid/content/Context;)V");
    jmethodID etCtor   = env->GetMethodID(etCls,  "<init>","(Landroid/content/Context;)V");
    jmethodID llCtor   = env->GetMethodID(llCls,  "<init>","(Landroid/content/Context;)V");
    jmethodID flCtor   = env->GetMethodID(flCls,  "<init>","(Landroid/content/Context;)V");
    jmethodID llpCtor  = env->GetMethodID(llpCls, "<init>","(II)V");
    jmethodID llpCtorW = env->GetMethodID(llpCls, "<init>","(IIF)V");
    jmethodID flpCtor3 = env->GetMethodID(flpCls, "<init>","(III)V");
    jmethodID gdCtor   = env->GetMethodID(gdCls,  "<init>","()V");
    jmethodID setText  = env->GetMethodID(tvCls,"setText","(Ljava/lang/CharSequence;)V");
    jmethodID setTxtSz = env->GetMethodID(tvCls,"setTextSize","(F)V");
    jmethodID setTxtCl = env->GetMethodID(tvCls,"setTextColor","(I)V");
    jmethodID setGravT = env->GetMethodID(tvCls,"setGravity","(I)V");
    jmethodID setTf    = env->GetMethodID(tvCls,"setTypeface","(Landroid/graphics/Typeface;)V");
    jmethodID setLetterSp = env->GetMethodID(tvCls,"setLetterSpacing","(F)V");
    jmethodID setOrient= env->GetMethodID(llCls,"setOrientation","(I)V");
    jmethodID setGravL = env->GetMethodID(llCls,"setGravity","(I)V");
    jmethodID llAdd    = env->GetMethodID(llCls,"addView","(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    jmethodID flAdd    = env->GetMethodID(flCls,"addView","(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    jmethodID addCont  = env->GetMethodID(actCls,"addContentView","(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    jmethodID setPad   = env->GetMethodID(viewCls,"setPadding","(IIII)V");
    jmethodID setBg    = env->GetMethodID(viewCls,"setBackground","(Landroid/graphics/drawable/Drawable;)V");
    jmethodID setBgClr = env->GetMethodID(viewCls,"setBackgroundColor","(I)V");
    jmethodID setId    = env->GetMethodID(viewCls,"setId","(I)V");
    jmethodID setClick = env->GetMethodID(viewCls,"setOnClickListener","(Landroid/view/View$OnClickListener;)V");
    jmethodID setClkbl = env->GetMethodID(viewCls,"setClickable","(Z)V");
    jmethodID setFocus = env->GetMethodID(viewCls,"setFocusable","(Z)V");
    jmethodID setFitm  = env->GetMethodID(viewCls,"setFocusableInTouchMode","(Z)V");
    jmethodID gdSetClr = env->GetMethodID(gdCls,"setColor","(I)V");
    jmethodID gdSetCR  = env->GetMethodID(gdCls,"setCornerRadius","(F)V");
    jmethodID gdSetStr = env->GetMethodID(gdCls,"setStroke","(II)V");
    jfieldID  llpTopM  = env->GetFieldID(llpCls,"topMargin","I");
    jfieldID  llpBotM  = env->GetFieldID(llpCls,"bottomMargin","I");
    jfieldID  llpLeftM = env->GetFieldID(llpCls,"leftMargin","I");

    jobject boldTf = env->GetStaticObjectField(tfCls,
        env->GetStaticFieldID(tfCls,"DEFAULT_BOLD","Landroid/graphics/Typeface;"));
    jstring monoName = env->NewStringUTF(AY_OBFUSCATE_KEY("monospace", K_MONO));
    jobject monoTf = env->CallStaticObjectMethod(tfCls,
        env->GetStaticMethodID(tfCls,"create","(Ljava/lang/String;I)Landroid/graphics/Typeface;"),monoName,0);
    env->DeleteLocalRef(monoName);

    auto makeBg = [&](jint bg, float r, int sw, jint sc) -> jobject {
        jobject d = env->NewObject(gdCls, gdCtor);
        env->CallVoidMethod(d, gdSetClr, bg);
        if (r>0) env->CallVoidMethod(d, gdSetCR, r);
        if (sw>0) env->CallVoidMethod(d, gdSetStr, sw, sc);
        return d;
    };

    jobject overlay = env->NewObject(flCls, flCtor, s_activity);
    env->CallVoidMethod(overlay, setBgClr, COL_BG_OVERLAY);
    env->CallVoidMethod(overlay, setPad, 28, 0, 28, 0);
    env->CallVoidMethod(overlay, setClkbl, JNI_TRUE);
    env->CallVoidMethod(overlay, setFocus, JNI_TRUE);
    env->CallVoidMethod(overlay, setFitm,  JNI_TRUE);

    jobject card = env->NewObject(llCls, llCtor, s_activity);
    env->CallVoidMethod(card, setOrient, 1);
    env->CallVoidMethod(card, setGravL, 1);
    env->CallVoidMethod(card, setPad, 36, 36, 36, 34);
    { jobject bg = makeBg(COL_BG_CARD, 30.f, 2, COL_GOLD_BORDER);
      env->CallVoidMethod(card, setBg, bg); env->DeleteLocalRef(bg); }

    { jobject icon = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("\xE2\x97\x88");
      env->CallVoidMethod(icon, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(icon, setTxtSz, 24.f);
      env->CallVoidMethod(icon, setTxtCl, COL_GOLD);
      env->CallVoidMethod(icon, setTf, boldTf);
      env->CallVoidMethod(icon, setGravT, 17);
      env->CallVoidMethod(icon, setPad, 0, 12, 0, 12);
      { jobject bg = makeBg((jint)0xAA171226, 32.f, 1, (jint)0x668A3DFF);
        env->CallVoidMethod(icon, setBg, bg); env->DeleteLocalRef(bg); }
      jobject p = env->NewObject(llpCls, llpCtor, 74, 74);
      env->SetIntField(p, llpBotM, 18);
      env->CallVoidMethod(card, llAdd, icon, p); env->DeleteLocalRef(icon); env->DeleteLocalRef(p); }

    { jobject hdr = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("GOAT VVIP");
      env->CallVoidMethod(hdr, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(hdr, setTxtSz, 24.f);
      env->CallVoidMethod(hdr, setTxtCl, COL_TEXT_PRI);
      env->CallVoidMethod(hdr, setTf, boldTf);
      env->CallVoidMethod(hdr, setGravT, 17);
      env->CallVoidMethod(hdr, setLetterSp, 0.08f);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 8);
      env->CallVoidMethod(card, llAdd, hdr, p); env->DeleteLocalRef(hdr); env->DeleteLocalRef(p); }

    { jobject sub = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("Enter your license key to continue");
      env->CallVoidMethod(sub, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(sub, setTxtSz, 12.f);
      env->CallVoidMethod(sub, setTxtCl, COL_TEXT_SEC);
      env->CallVoidMethod(sub, setGravT, 17);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 26);
      env->CallVoidMethod(card, llAdd, sub, p); env->DeleteLocalRef(sub); env->DeleteLocalRef(p); }

    { jobject dv = env->NewObject(viewCls, viewCtor, s_activity);
      env->CallVoidMethod(dv, setBgClr, (jint)0x448A3DFF);
      jobject p = env->NewObject(llpCls, llpCtor, -1, 1);
      env->SetIntField(p, llpBotM, 18);
      env->CallVoidMethod(card, llAdd, dv, p); env->DeleteLocalRef(dv); env->DeleteLocalRef(p); }

    jobject devBox = env->NewObject(llCls, llCtor, s_activity);
    env->CallVoidMethod(devBox, setOrient, 1);
    env->CallVoidMethod(devBox, setPad, 20, 16, 20, 16);
    { jobject bg = makeBg(COL_BG_SECTION, 18.f, 1, (jint)0x443D455F);
      env->CallVoidMethod(devBox, setBg, bg); env->DeleteLocalRef(bg); }

    { jobject lbl = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF(AY_OBFUSCATE_KEY("DEVICE ID", K_DLBL));
      env->CallVoidMethod(lbl, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(lbl, setTxtSz, 9.f);
      env->CallVoidMethod(lbl, setTxtCl, (jint)0xFFB7BBCD);
      env->CallVoidMethod(lbl, setLetterSp, 0.08f);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 10);
      env->CallVoidMethod(devBox, llAdd, lbl, p); env->DeleteLocalRef(lbl); env->DeleteLocalRef(p); }

    jobject devRow = env->NewObject(llCls, llCtor, s_activity);
    env->CallVoidMethod(devRow, setOrient, 0);
    env->CallVoidMethod(devRow, setGravL, 16);

    std::string devId = getDeviceId(env);
    std::string devIdFmt = (devId.size() >= 16)
        ? devId.substr(0,8) + "  " + devId.substr(8) : devId;

    { jobject dv = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF(devIdFmt.c_str());
      env->CallVoidMethod(dv, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(dv, setTxtSz, 12.5f);
      env->CallVoidMethod(dv, setTxtCl, COL_TEXT_PRI);
      env->CallVoidMethod(dv, setTf, monoTf);
      jobject p = env->NewObject(llpCls, llpCtorW, 0, -2, 1.f);
      env->CallVoidMethod(devRow, llAdd, dv, p); env->DeleteLocalRef(dv); env->DeleteLocalRef(p); }

    { jobject btn = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF(AY_OBFUSCATE_KEY("COPY", K_COPY));
      env->CallVoidMethod(btn, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(btn, setTxtSz, 8.5f);
      env->CallVoidMethod(btn, setTxtCl, (jint)0xFFFFFFFF);
      env->CallVoidMethod(btn, setTf, boldTf);
      env->CallVoidMethod(btn, setGravT, 17);
      env->CallVoidMethod(btn, setPad, 18, 9, 18, 9);
      { jobject bg = makeBg((jint)0xFF24123E, 14.f, 1, COL_GOLD_BORDER);
        env->CallVoidMethod(btn, setBg, bg); env->DeleteLocalRef(bg); }
      env->CallVoidMethod(btn, setId, (jint)LIC_BTN_COPY_DEVID);
      env->CallVoidMethod(btn, setClick, s_listener);
      jobject p = env->NewObject(llpCls, llpCtor, -2, -2);
      env->SetIntField(p, llpLeftM, 16);
      env->CallVoidMethod(devRow, llAdd, btn, p); env->DeleteLocalRef(btn); env->DeleteLocalRef(p); }

    { jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->CallVoidMethod(devBox, llAdd, devRow, p); env->DeleteLocalRef(devRow); env->DeleteLocalRef(p); }
    { jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 20);
      env->CallVoidMethod(card, llAdd, devBox, p); env->DeleteLocalRef(devBox); env->DeleteLocalRef(p); }

    { jobject lbl = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("LICENSE KEY");
      env->CallVoidMethod(lbl, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(lbl, setTxtSz, 9.f);
      env->CallVoidMethod(lbl, setTxtCl, (jint)0xFFB7BBCD);
      env->CallVoidMethod(lbl, setLetterSp, 0.08f);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 10);
      env->CallVoidMethod(card, llAdd, lbl, p); env->DeleteLocalRef(lbl); env->DeleteLocalRef(p); }

    { jobject et = env->NewObject(etCls, etCtor, s_activity);
      env->CallVoidMethod(et, setTxtSz, 15.5f);
      env->CallVoidMethod(et, setTxtCl, COL_TEXT_PRI);
      env->CallVoidMethod(et, setTf, monoTf);
      env->CallVoidMethod(et, setGravT, 16);
      env->CallVoidMethod(et, setPad, 24, 20, 24, 20);
      env->CallVoidMethod(et, env->GetMethodID(etCls,"setSingleLine","(Z)V"), JNI_TRUE);
      env->CallVoidMethod(et, env->GetMethodID(etCls,"setInputType","(I)V"), (jint)0x00001001);
      { jstring h = env->NewStringUTF("Enter license key");
        env->CallVoidMethod(et, env->GetMethodID(etCls,"setHint","(Ljava/lang/CharSequence;)V"), h);
        env->DeleteLocalRef(h); }
      env->CallVoidMethod(et, env->GetMethodID(etCls,"setHintTextColor","(I)V"), (jint)0xFF6F748B);
      { jobject bg = makeBg((jint)0xD60B0F18, 18.f, 1, (jint)0x333D455F);
        env->CallVoidMethod(et, setBg, bg); env->DeleteLocalRef(bg); }
      s_keyField = env->NewGlobalRef(et);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 22);
      env->CallVoidMethod(card, llAdd, et, p); env->DeleteLocalRef(et); env->DeleteLocalRef(p); }

    { jobject btn = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("UNLOCK ACCESS");
      env->CallVoidMethod(btn, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(btn, setTxtSz, 14.f);
      env->CallVoidMethod(btn, setTxtCl, (jint)0xFFFFFFFF);
      env->CallVoidMethod(btn, setTf, boldTf);
      env->CallVoidMethod(btn, setGravT, 17);
      env->CallVoidMethod(btn, setPad, 0, 26, 0, 26);
      { jobject bg = makeBg((jint)0xFF6F20F2, 18.f, 1, (jint)0xFF9B4DFF);
        env->CallVoidMethod(btn, setBg, bg); env->DeleteLocalRef(bg); }
      env->CallVoidMethod(btn, setId, (jint)LIC_BTN_ACTIVATE);
      env->CallVoidMethod(btn, setClick, s_listener);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpBotM, 14);
      env->CallVoidMethod(card, llAdd, btn, p); env->DeleteLocalRef(btn); env->DeleteLocalRef(p); }

    { jobject stv = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("");
      env->CallVoidMethod(stv, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(stv, setTxtSz, 11.f);
      env->CallVoidMethod(stv, setTxtCl, COL_INFO);
      env->CallVoidMethod(stv, setGravT, 17);
      s_statusTv = env->NewGlobalRef(stv);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->SetIntField(p, llpTopM, 4); env->SetIntField(p, llpBotM, 18);
      env->CallVoidMethod(card, llAdd, stv, p); env->DeleteLocalRef(stv); env->DeleteLocalRef(p); }

    { jobject ftv = env->NewObject(tvCls, tvCtor, s_activity);
      jstring s = env->NewStringUTF("Need a key?  Contact support");
      env->CallVoidMethod(ftv, setText, s); env->DeleteLocalRef(s);
      env->CallVoidMethod(ftv, setTxtSz, 12.5f);
      env->CallVoidMethod(ftv, setTxtCl, COL_GOLD);
      env->CallVoidMethod(ftv, setTf, boldTf);
      env->CallVoidMethod(ftv, setGravT, 17);
      env->CallVoidMethod(ftv, setPad, 0, 8, 0, 0);
      env->CallVoidMethod(ftv, setId, (jint)LIC_BTN_SUPPORT);
      env->CallVoidMethod(ftv, setClick, s_listener);
      jobject p = env->NewObject(llpCls, llpCtor, -1, -2);
      env->CallVoidMethod(card, llAdd, ftv, p); env->DeleteLocalRef(ftv); env->DeleteLocalRef(p); }

    { jobject p = env->NewObject(flpCls, flpCtor3, -1, -2, 17);
      env->CallVoidMethod(overlay, flAdd, card, p);
      env->DeleteLocalRef(p); env->DeleteLocalRef(card); }

    { jobject p = env->NewObject(flpCls, flpCtor3, -1, -1, 17);
      env->CallVoidMethod(s_activity, addCont, overlay, p); env->DeleteLocalRef(p); }
    s_container = env->NewGlobalRef(overlay);
    env->DeleteLocalRef(overlay);

    env->DeleteLocalRef(viewCls); env->DeleteLocalRef(tvCls); env->DeleteLocalRef(etCls);
    env->DeleteLocalRef(llCls);   env->DeleteLocalRef(flCls); env->DeleteLocalRef(llpCls);
    env->DeleteLocalRef(flpCls);  env->DeleteLocalRef(gdCls); env->DeleteLocalRef(tfCls);
    env->DeleteLocalRef(actCls);  env->DeleteLocalRef(boldTf); env->DeleteLocalRef(monoTf);
}

void license_init(JNIEnv* env, jobject activity, jobject listenerView) {
    env->GetJavaVM(&s_jvm);
    // Re-activation calls nativeCreatePanel with these same global handles.
    // Deleting and replacing such a handle here invalidates the caller's
    // still-live parameter, so later Android View constructors receive a null
    // Context. Preserve identical globals; replace them only for a genuinely
    // new Activity/View instance.
    if (activity && (!s_activity || !env->IsSameObject(s_activity, activity))) {
        jobject nextActivity = env->NewGlobalRef(activity);
        if (s_activity) env->DeleteGlobalRef(s_activity);
        s_activity = nextActivity;
    }
    if (listenerView && (!s_listener || !env->IsSameObject(s_listener, listenerView))) {
        jobject nextListener = env->NewGlobalRef(listenerView);
        if (s_listener) env->DeleteGlobalRef(s_listener);
        s_listener = nextListener;
    }

    getDeviceId(env);
    if (!license_security_init(env, s_activity) || isEnvironmentSuspicious()) {
        license_security_revoke();
        clearSavedCreds(env);
    }

}

bool checkSavedLicense(JNIEnv* env) {
    g_licenseValidated.store(false);
    std::string saved = loadSavedKey(env);
    if (saved.empty()) return false;
    if (validateKey(saved) == 0) { clearSavedCreds(env); return false; }

    std::string expiry = loadSavedExpiry(env);
    if (!expiry.empty()) {
        time_t now = time(nullptr);
        const time_t expiryEpoch = parseExpiryUtc(expiry);
        s_expiryEpoch.store(expiryEpoch);
        if (expiryEpoch > now) {
            if (license_security_has_access()) {
                g_licenseValidated.store(true);
                s_lastValidated = now;
                startPeriodicRevalidation();
                return true;
            }
            // Local preferences are only a convenience. They never grant
            // access by themselves; silently obtain a fresh signed lease.
            beginOnlineValidation(saved, false);
            return false;
        }
    }
    clearSavedCreds(env);
    return false;
}

void onLicenseClick(JNIEnv* env, int btnId) {

    if (btnId == LIC_BTN_COPY_DEVID) {
        std::string devId = getDeviceId(env);
        jclass ctxCls  = env->FindClass("android/content/Context");
        jfieldID clipF = env->GetStaticFieldID(ctxCls,"CLIPBOARD_SERVICE","Ljava/lang/String;");
        jstring clipSvc = (jstring)env->GetStaticObjectField(ctxCls, clipF);
        jobject mgr = env->CallObjectMethod(s_activity,
            env->GetMethodID(ctxCls,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;"), clipSvc);
        env->DeleteLocalRef(ctxCls); env->DeleteLocalRef(clipSvc);
        if (mgr) {
            jclass cdCls = env->FindClass("android/content/ClipData");
            jstring lbl  = env->NewStringUTF(AY_OBFUSCATE_KEY("devid", K_DVID));
            jstring cont = env->NewStringUTF(devId.c_str());
            jobject clip = env->CallStaticObjectMethod(cdCls,
                env->GetStaticMethodID(cdCls,"newPlainText",
                    "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;"),
                lbl, cont);
            env->CallVoidMethod(mgr,
                env->GetMethodID(env->GetObjectClass(mgr),"setPrimaryClip","(Landroid/content/ClipData;)V"),clip);
            env->DeleteLocalRef(lbl); env->DeleteLocalRef(cont);
            env->DeleteLocalRef(clip); env->DeleteLocalRef(cdCls); env->DeleteLocalRef(mgr);
            setStatus(env, "\u2713  Device ID copied to clipboard", COL_SUCCESS);
        }
        return;
    }

    if (btnId == LIC_BTN_SUPPORT) {
        jclass uriCls = env->FindClass("android/net/Uri");
        jclass intentCls = env->FindClass("android/content/Intent");
        jclass actCls = env->FindClass("android/app/Activity");
        jstring url = env->NewStringUTF("https://t.me/onlygoat07");
        jobject uri = env->CallStaticObjectMethod(uriCls,
            env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), url);
        jstring action = env->NewStringUTF("android.intent.action.VIEW");
        jobject intent = env->NewObject(intentCls,
            env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"), action, uri);
        env->CallVoidMethod(s_activity,
            env->GetMethodID(actCls, "startActivity", "(Landroid/content/Intent;)V"), intent);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            setStatus(env, "Open Telegram: t.me/onlygoat07", COL_INFO);
        }
        env->DeleteLocalRef(url); env->DeleteLocalRef(uri); env->DeleteLocalRef(action);
        env->DeleteLocalRef(intent); env->DeleteLocalRef(uriCls); env->DeleteLocalRef(intentCls);
        env->DeleteLocalRef(actCls);
        return;
    }

    if (btnId == LIC_BTN_ACTIVATE) {
        if (!s_keyField || s_validating.load()) return;

        jmethodID gT = env->GetMethodID(env->GetObjectClass(s_keyField),"getText","()Landroid/text/Editable;");
        jobject editable = env->CallObjectMethod(s_keyField, gT);
        jstring jkey = (jstring)env->CallObjectMethod(editable,
            env->GetMethodID(env->GetObjectClass(editable),"toString","()Ljava/lang/String;"));
        env->DeleteLocalRef(editable);
        const char* c = env->GetStringUTFChars(jkey, nullptr);
        std::string key(c ? c : "");
        env->ReleaseStringUTFChars(jkey, c);
        env->DeleteLocalRef(jkey);

        if (key.empty()) { setStatus(env,AY_OBFUSCATE_KEY("Please enter your license key", K_PLIC),COL_INFO); return; }
        if (validateKey(key)==0) { setStatus(env,AY_OBFUSCATE_KEY("\u2717  Invalid key format (XXXX-XXXX-XXXX-XXXX)", K_IFMT),COL_ERROR); return; }

        setStatus(env, AY_OBFUSCATE_KEY("Contacting server...", K_CONT), COL_INFO);
        beginOnlineValidation(key, true);
    }
}

time_t getExpiryTime(JNIEnv* env) {
    const time_t cached = s_expiryEpoch.load();
    if (cached > 0) return cached;
    std::string s = loadSavedExpiry(env);
    const time_t parsed = parseExpiryUtc(s);
    s_expiryEpoch.store(parsed);
    return parsed;
}

bool license_enforce_runtime(JNIEnv* env) {
    // Online revocation/error is delivered by the worker thread and consumed
    // here so all UI and gameplay state changes happen on the game/UI thread.
    if (s_forceReloginPending.exchange(false)) {
        license_security_revoke();
        g_licenseValidated.store(false);
        clearSavedCreds(env);
        disableTrajectories();
        showExpiredLicenseUi(env);
        return false;
    }

    if (!g_licenseValidated.load()) return false;

    if (!license_security_has_access()) {
        bool expected = true;
        if (g_licenseValidated.compare_exchange_strong(expected, false)) {
            disableTrajectories();
            showExpiredLicenseUi(env);
        }
        return false;
    }

    time_t expiry = s_expiryEpoch.load();
    if (expiry <= 0) expiry = getExpiryTime(env);
    if (expiry > time(nullptr)) return true;

    // Local expiry is authoritative and immediate; it does not wait for the
    // six-hour online revocation cycle or for an app restart/data clear.
    bool expected = true;
    if (g_licenseValidated.compare_exchange_strong(expected, false)) {
        license_security_revoke();
        clearSavedCreds(env);
        disableTrajectories();
        showExpiredLicenseUi(env);
    }
    return false;
}
