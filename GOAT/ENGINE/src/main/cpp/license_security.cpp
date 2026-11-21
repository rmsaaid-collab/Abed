#include "include/license_security.h"

#include "obfuscate.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kProtocolVersion = 2;
constexpr uint64_t kMaximumLeaseMs = 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kProbeIntervalMs = 2200ULL;

#define KS_ALIAS 0xA17D0E6E5D2F1041ULL
#define KS_EC 0xC67232BE438173A1ULL
#define KS_PROVIDER 0xF282A19CBE4E51D3ULL
#define KS_CURVE 0xDE03829185E6A4F1ULL
#define KS_DIGEST 0xBDE23D2A4FC82E33ULL
#define KS_SIGNATURE 0x8AC07DD53B18F7A5ULL
#define KS_URL_A 0xE751332AB69370E1ULL
#define KS_URL_B 0xB54D909274AE1139ULL
#define KS_URL_C 0xAB92FB0715E8A9C7ULL
#define KS_PUBLIC_A 0xC4AE90DF1182BD29ULL
#define KS_PUBLIC_B 0x99741DD892EE557FULL
#define KS_CERT 0x92B63EC1F4D08A73ULL

struct Sha256Context {
    uint32_t state[8];
    uint64_t totalBytes;
    uint8_t block[64];
    size_t blockBytes;
};

constexpr uint32_t kSha256Constants[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
};

static std::mutex s_initMutex;
static bool s_initialized = false;
static std::atomic<bool> s_integrityReady{false};
static std::string s_binaryHash;

static std::atomic<uint64_t> s_secretA{0};
static std::atomic<uint64_t> s_secretB{0};
static std::atomic<uint64_t> s_generation{0};
static std::atomic<uint64_t> s_grantDigest{0};
static std::atomic<uint64_t> s_deadlineToken{0};
static std::atomic<uint64_t> s_accessMarker{0};
static std::atomic<uint32_t> s_gameMask{0};
static std::atomic<uint64_t> s_lastProbeMs{0};

static inline uint32_t rotateRight(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << (32U - amount));
}

static void sha256Transform(Sha256Context& context, const uint8_t block[64]) {
    uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
        words[i] = (static_cast<uint32_t>(block[i * 4]) << 24U)
                 | (static_cast<uint32_t>(block[i * 4 + 1]) << 16U)
                 | (static_cast<uint32_t>(block[i * 4 + 2]) << 8U)
                 | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotateRight(words[i - 15], 7U)
                          ^ rotateRight(words[i - 15], 18U)
                          ^ (words[i - 15] >> 3U);
        const uint32_t s1 = rotateRight(words[i - 2], 17U)
                          ^ rotateRight(words[i - 2], 19U)
                          ^ (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = context.state[0];
    uint32_t b = context.state[1];
    uint32_t c = context.state[2];
    uint32_t d = context.state[3];
    uint32_t e = context.state[4];
    uint32_t f = context.state[5];
    uint32_t g = context.state[6];
    uint32_t h = context.state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t sigma1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sigma1 + choose + kSha256Constants[i] + words[i];
        const uint32_t sigma0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sigma0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    context.state[0] += a; context.state[1] += b;
    context.state[2] += c; context.state[3] += d;
    context.state[4] += e; context.state[5] += f;
    context.state[6] += g; context.state[7] += h;
}

static void sha256Init(Sha256Context& context) {
    context.state[0] = 0x6a09e667U; context.state[1] = 0xbb67ae85U;
    context.state[2] = 0x3c6ef372U; context.state[3] = 0xa54ff53aU;
    context.state[4] = 0x510e527fU; context.state[5] = 0x9b05688cU;
    context.state[6] = 0x1f83d9abU; context.state[7] = 0x5be0cd19U;
    context.totalBytes = 0;
    context.blockBytes = 0;
    std::memset(context.block, 0, sizeof(context.block));
}

static void sha256Update(Sha256Context& context, const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    context.totalBytes += size;
    while (size > 0) {
        const size_t available = sizeof(context.block) - context.blockBytes;
        const size_t take = size < available ? size : available;
        std::memcpy(context.block + context.blockBytes, data, take);
        context.blockBytes += take;
        data += take;
        size -= take;
        if (context.blockBytes == sizeof(context.block)) {
            sha256Transform(context, context.block);
            context.blockBytes = 0;
        }
    }
}

static void sha256Final(Sha256Context& context, uint8_t output[32]) {
    const uint64_t bitLength = context.totalBytes * 8ULL;
    context.block[context.blockBytes++] = 0x80U;
    if (context.blockBytes > 56) {
        while (context.blockBytes < 64) context.block[context.blockBytes++] = 0;
        sha256Transform(context, context.block);
        context.blockBytes = 0;
    }
    while (context.blockBytes < 56) context.block[context.blockBytes++] = 0;
    for (int i = 7; i >= 0; --i) {
        context.block[context.blockBytes++] = static_cast<uint8_t>(bitLength >> (i * 8));
    }
    sha256Transform(context, context.block);
    for (int i = 0; i < 8; ++i) {
        output[i * 4] = static_cast<uint8_t>(context.state[i] >> 24U);
        output[i * 4 + 1] = static_cast<uint8_t>(context.state[i] >> 16U);
        output[i * 4 + 2] = static_cast<uint8_t>(context.state[i] >> 8U);
        output[i * 4 + 3] = static_cast<uint8_t>(context.state[i]);
    }
}

static void sha256Bytes(const uint8_t* data, size_t size, uint8_t output[32]) {
    Sha256Context context{};
    sha256Init(context);
    sha256Update(context, data, size);
    sha256Final(context, output);
}

static std::string hexLower(const uint8_t* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(size * 2, '\0');
    for (size_t i = 0; i < size; ++i) {
        output[i * 2] = digits[data[i] >> 4U];
        output[i * 2 + 1] = digits[data[i] & 0x0fU];
    }
    return output;
}

static std::string sha256Hex(const std::string& value) {
    uint8_t digest[32];
    sha256Bytes(reinterpret_cast<const uint8_t*>(value.data()), value.size(), digest);
    return hexLower(digest, sizeof(digest));
}

static uint64_t monotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL
         + static_cast<uint64_t>(now.tv_nsec / 1000000ULL);
}

static uint64_t wallClockMs() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL
         + static_cast<uint64_t>(now.tv_nsec / 1000000ULL);
}

static uint64_t mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

static bool secureRandom(uint8_t* output, size_t size) {
    const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    size_t complete = 0;
    while (complete < size) {
        const ssize_t count = read(descriptor, output + complete, size - complete);
        if (count > 0) complete += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    close(descriptor);
    return complete == size;
}

static bool clearException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

static std::vector<uint8_t> byteArrayToVector(JNIEnv* env, jbyteArray array) {
    std::vector<uint8_t> output;
    if (!env || !array) return output;
    const jsize size = env->GetArrayLength(array);
    if (size <= 0 || size > 32768) return output;
    output.resize(static_cast<size_t>(size));
    env->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte*>(output.data()));
    if (clearException(env)) output.clear();
    return output;
}

static jbyteArray vectorToByteArray(JNIEnv* env, const uint8_t* data, size_t size) {
    if (!env || !data || size == 0 || size > 32768) return nullptr;
    jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
    if (!array) return nullptr;
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
    if (clearException(env)) {
        env->DeleteLocalRef(array);
        return nullptr;
    }
    return array;
}

static std::string base64UrlEncode(const uint8_t* data, size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((size * 4U + 2U) / 3U);
    size_t offset = 0;
    while (offset + 3 <= size) {
        const uint32_t value = (static_cast<uint32_t>(data[offset]) << 16U)
                             | (static_cast<uint32_t>(data[offset + 1]) << 8U)
                             | static_cast<uint32_t>(data[offset + 2]);
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(alphabet[(value >> 6U) & 63U]);
        output.push_back(alphabet[value & 63U]);
        offset += 3;
    }
    const size_t remaining = size - offset;
    if (remaining == 1) {
        const uint32_t value = static_cast<uint32_t>(data[offset]) << 16U;
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
    } else if (remaining == 2) {
        const uint32_t value = (static_cast<uint32_t>(data[offset]) << 16U)
                             | (static_cast<uint32_t>(data[offset + 1]) << 8U);
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(alphabet[(value >> 6U) & 63U]);
    }
    return output;
}

static int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '-') return 62;
    if (character == '_') return 63;
    return -1;
}

static std::vector<uint8_t> base64UrlDecode(const std::string& input, size_t maximum) {
    std::vector<uint8_t> output;
    if (input.empty() || input.size() > maximum * 2U) return output;
    uint32_t accumulator = 0;
    int bits = 0;
    for (char character : input) {
        const int value = base64Value(character);
        if (value < 0) {
            output.clear();
            return output;
        }
        accumulator = (accumulator << 6U) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xffU));
            if (output.size() > maximum) {
                output.clear();
                return output;
            }
        }
    }
    if (bits >= 6 || (bits > 0 && (accumulator & ((1U << bits) - 1U)) != 0)) output.clear();
    return output;
}

static bool constantTimeHexEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return difference == 0;
}

static std::string buildId() {
    return "GOAT-P3-20260818-FINAL";
}

static std::string sessionEndpoint() {
    std::string value((char*)AY_OBFUSCATE_KEY("https://golden-validate-", KS_URL_A));
    value += AY_OBFUSCATE_KEY("worker.goat7.workers.dev", KS_URL_B);
    value += AY_OBFUSCATE_KEY("/api/v2/7f4a9c/session", KS_URL_C);
    return value;
}

static std::string serverPublicSpki() {
    std::string value((char*)AY_OBFUSCATE_KEY(
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQXP9FPKAoLuwBJQK3SKBvZrD8CH3Hd",
        KS_PUBLIC_A));
    value += AY_OBFUSCATE_KEY(
        "HuMW5tXlYNBRmeS-xpLIlUjfACerN1i6W4V6Fk9ICEivuKRWLz9aCIPA",
        KS_PUBLIC_B);
    return value;
}

static bool readSelfHash(std::string& output) {
    Dl_info information{};
    if (dladdr(reinterpret_cast<void*>(&license_security_init), &information) == 0
        || !information.dli_fname || information.dli_fname[0] != '/') {
        return false;
    }
    const int descriptor = open(information.dli_fname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat status{};
    if (fstat(descriptor, &status) != 0 || status.st_size < 64 * 1024 || !S_ISREG(status.st_mode)) {
        close(descriptor);
        return false;
    }
    Sha256Context context{};
    sha256Init(context);
    uint8_t buffer[16384];
    bool valid = true;
    while (true) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0) sha256Update(context, buffer, static_cast<size_t>(count));
        else if (count == 0) break;
        else if (errno == EINTR) continue;
        else { valid = false; break; }
    }
    close(descriptor);
    if (!valid) return false;
    uint8_t digest[32];
    sha256Final(context, digest);
    output = hexLower(digest, sizeof(digest));
    return output.size() == 64;
}

static bool processIsTraced() {
    FILE* file = std::fopen("/proc/self/status", "r");
    if (!file) return true;
    char line[256];
    int tracerPid = 0;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            tracerPid = std::atoi(line + 10);
            break;
        }
    }
    std::fclose(file);
    return tracerPid != 0;
}

static bool suspiciousMappingsPresent() {
    FILE* file = std::fopen("/proc/self/maps", "r");
    if (!file) return true;
    static const char* needles[] = {
        "frida", "gadget", "linjector", "xposed", "substrate", nullptr,
    };
    char line[768];
    bool suspicious = false;
    while (std::fgets(line, sizeof(line), file)) {
        for (size_t i = 0; needles[i]; ++i) {
            if (std::strstr(line, needles[i])) {
                suspicious = true;
                break;
            }
        }
        if (suspicious) break;
    }
    std::fclose(file);
    return suspicious;
}

static bool javaDebuggerConnected(JNIEnv* env) {
    if (!env) return true;
    jclass debugClass = env->FindClass("android/os/Debug");
    const bool classFailed = clearException(env);
    if (!debugClass || classFailed) return true;
    jmethodID method = env->GetStaticMethodID(debugClass, "isDebuggerConnected", "()Z");
    const bool methodFailed = clearException(env);
    if (!method || methodFailed) {
        env->DeleteLocalRef(debugClass);
        return true;
    }
    const bool connected = env->CallStaticBooleanMethod(debugClass, method) == JNI_TRUE;
    const bool failed = clearException(env);
    env->DeleteLocalRef(debugClass);
    return failed || connected;
}

static jobject getSignedLoaderContext(JNIEnv* env) {
    if (!env) return nullptr;

    // TrainerOverlayView is resolved by the guest class loader, but its
    // bridge holds the real GOAT loader Context.  Verify that signed host APK
    // instead of Jawaker's untouched official certificate.
    jclass bridgeClass = env->FindClass("com/google/firebase/TrainerOverlayView");
    const bool classFailed = clearException(env);
    if (!bridgeClass || classFailed) return nullptr;

    jmethodID getHostContext = env->GetStaticMethodID(
        bridgeClass,
        "getHostContext",
        "()Landroid/content/Context;");
    const bool methodFailed = clearException(env);
    if (!getHostContext || methodFailed) {
        env->DeleteLocalRef(bridgeClass);
        return nullptr;
    }

    jobject context = env->CallStaticObjectMethod(bridgeClass, getHostContext);
    const bool callFailed = clearException(env);
    env->DeleteLocalRef(bridgeClass);
    return callFailed ? nullptr : context;
}

static bool verifyAppCertificate(JNIEnv* env, jobject activity) {
    if (!env || !activity) return false;
    jobject signedContext = getSignedLoaderContext(env);
    if (!signedContext) return false;

    jclass activityClass = env->GetObjectClass(signedContext);
    const bool activityClassFailed = clearException(env);
    if (!activityClass || activityClassFailed) {
        env->DeleteLocalRef(signedContext);
        return false;
    }
    jmethodID getPackageManager = env->GetMethodID(
        activityClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jmethodID getPackageName = env->GetMethodID(
        activityClass, "getPackageName", "()Ljava/lang/String;");
    const bool lookupFailed = clearException(env);
    if (!getPackageManager || !getPackageName || lookupFailed) {
        env->DeleteLocalRef(activityClass);
        env->DeleteLocalRef(signedContext);
        return false;
    }
    jobject packageManager = env->CallObjectMethod(signedContext, getPackageManager);
    jstring packageName = static_cast<jstring>(env->CallObjectMethod(signedContext, getPackageName));
    env->DeleteLocalRef(activityClass);
    const bool packageLookupFailed = clearException(env);
    if (!packageManager || !packageName || packageLookupFailed) {
        if (packageManager) env->DeleteLocalRef(packageManager);
        if (packageName) env->DeleteLocalRef(packageName);
        env->DeleteLocalRef(signedContext);
        return false;
    }

    jclass packageManagerClass = env->GetObjectClass(packageManager);
    jmethodID getPackageInfo = env->GetMethodID(
        packageManagerClass,
        "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jobject packageInfo = getPackageInfo
        ? env->CallObjectMethod(packageManager, getPackageInfo, packageName, static_cast<jint>(0x08000000))
        : nullptr;
    const bool modernLookupFailed = clearException(env);
    if (!packageInfo || modernLookupFailed) {
        if (packageInfo) env->DeleteLocalRef(packageInfo);
        packageInfo = getPackageInfo
            ? env->CallObjectMethod(packageManager, getPackageInfo, packageName, static_cast<jint>(0x40))
            : nullptr;
        clearException(env);
    }

    jobjectArray signatures = nullptr;
    if (packageInfo) {
        jclass packageInfoClass = env->GetObjectClass(packageInfo);
        jfieldID signingInfoField = env->GetFieldID(
            packageInfoClass, "signingInfo", "Landroid/content/pm/SigningInfo;");
        if (signingInfoField && !clearException(env)) {
            jobject signingInfo = env->GetObjectField(packageInfo, signingInfoField);
            const bool signingInfoFailed = clearException(env);
            if (signingInfo && !signingInfoFailed) {
                jclass signingInfoClass = env->GetObjectClass(signingInfo);
                jmethodID getSigners = env->GetMethodID(
                    signingInfoClass,
                    "getApkContentsSigners",
                    "()[Landroid/content/pm/Signature;");
                const bool signerLookupFailed = clearException(env);
                if (getSigners && !signerLookupFailed) {
                    signatures = static_cast<jobjectArray>(env->CallObjectMethod(signingInfo, getSigners));
                    clearException(env);
                }
                env->DeleteLocalRef(signingInfoClass);
                env->DeleteLocalRef(signingInfo);
            }
        } else {
            clearException(env);
        }
        if (!signatures) {
            jfieldID signaturesField = env->GetFieldID(
                packageInfoClass, "signatures", "[Landroid/content/pm/Signature;");
            if (signaturesField && !clearException(env)) {
                signatures = static_cast<jobjectArray>(env->GetObjectField(packageInfo, signaturesField));
                clearException(env);
            } else {
                clearException(env);
            }
        }
        env->DeleteLocalRef(packageInfoClass);
    }

    bool matched = false;
    const std::string expected((char*)AY_OBFUSCATE_KEY(
        "69f33c009629337b0ec4178016c86bfdc0084100386565efe308369e1ea1bfba",
        KS_CERT));
    if (signatures) {
        const jsize count = env->GetArrayLength(signatures);
        jclass signatureClass = env->FindClass("android/content/pm/Signature");
        jmethodID toByteArray = signatureClass
            ? env->GetMethodID(signatureClass, "toByteArray", "()[B")
            : nullptr;
        if (clearException(env)) toByteArray = nullptr;
        for (jsize index = 0; index < count && toByteArray; ++index) {
            jobject signature = env->GetObjectArrayElement(signatures, index);
            jbyteArray encoded = signature
                ? static_cast<jbyteArray>(env->CallObjectMethod(signature, toByteArray))
                : nullptr;
            const std::vector<uint8_t> bytes = byteArrayToVector(env, encoded);
            if (!bytes.empty()) {
                uint8_t digest[32];
                sha256Bytes(bytes.data(), bytes.size(), digest);
                matched = constantTimeHexEquals(hexLower(digest, sizeof(digest)), expected);
            }
            if (encoded) env->DeleteLocalRef(encoded);
            if (signature) env->DeleteLocalRef(signature);
            if (matched) break;
        }
        if (signatureClass) env->DeleteLocalRef(signatureClass);
        env->DeleteLocalRef(signatures);
    }

    if (packageInfo) env->DeleteLocalRef(packageInfo);
    env->DeleteLocalRef(packageManagerClass);
    env->DeleteLocalRef(packageManager);
    env->DeleteLocalRef(packageName);
    env->DeleteLocalRef(signedContext);
    return matched;
}

struct InstallIdentity {
    jobject privateKey = nullptr;
    std::vector<uint8_t> publicKey;
};

static bool ensureKeystoreAlias(JNIEnv* env, jobject keyStore, jstring alias) {
    jclass keyStoreClass = env->GetObjectClass(keyStore);
    jmethodID containsAlias = env->GetMethodID(keyStoreClass, "containsAlias", "(Ljava/lang/String;)Z");
    bool exists = false;
    if (containsAlias && !clearException(env)) {
        exists = env->CallBooleanMethod(keyStore, containsAlias, alias) == JNI_TRUE;
        if (clearException(env)) exists = false;
    } else {
        clearException(env);
    }
    env->DeleteLocalRef(keyStoreClass);
    if (exists) return true;

    jclass generatorClass = env->FindClass("java/security/KeyPairGenerator");
    jmethodID getInstance = generatorClass
        ? env->GetStaticMethodID(
            generatorClass,
            "getInstance",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/KeyPairGenerator;")
        : nullptr;
    jstring ec = env->NewStringUTF(AY_OBFUSCATE_KEY("EC", KS_EC));
    jstring provider = env->NewStringUTF(AY_OBFUSCATE_KEY("AndroidKeyStore", KS_PROVIDER));
    jobject generator = getInstance
        ? env->CallStaticObjectMethod(generatorClass, getInstance, ec, provider)
        : nullptr;
    if (ec) env->DeleteLocalRef(ec);
    if (provider) env->DeleteLocalRef(provider);
    const bool generatorFailed = clearException(env);
    if (!generator || generatorFailed) {
        if (generator) env->DeleteLocalRef(generator);
        if (generatorClass) env->DeleteLocalRef(generatorClass);
        return false;
    }

    jclass curveClass = env->FindClass("java/security/spec/ECGenParameterSpec");
    jmethodID curveConstructor = curveClass
        ? env->GetMethodID(curveClass, "<init>", "(Ljava/lang/String;)V")
        : nullptr;
    jstring curveName = env->NewStringUTF(AY_OBFUSCATE_KEY("secp256r1", KS_CURVE));
    jobject curve = curveConstructor
        ? env->NewObject(curveClass, curveConstructor, curveName)
        : nullptr;
    if (curveName) env->DeleteLocalRef(curveName);

    jclass builderClass = env->FindClass("android/security/keystore/KeyGenParameterSpec$Builder");
    jmethodID builderConstructor = builderClass
        ? env->GetMethodID(builderClass, "<init>", "(Ljava/lang/String;I)V")
        : nullptr;
    jobject builder = builderConstructor
        ? env->NewObject(builderClass, builderConstructor, alias, static_cast<jint>(4 | 8))
        : nullptr;
    const bool constructionFailed = clearException(env);
    bool configured = builder && curve && !constructionFailed;
    if (configured) {
        jmethodID setCurve = env->GetMethodID(
            builderClass,
            "setAlgorithmParameterSpec",
            "(Ljava/security/spec/AlgorithmParameterSpec;)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
        jobject ignored = setCurve ? env->CallObjectMethod(builder, setCurve, curve) : nullptr;
        if (ignored) env->DeleteLocalRef(ignored);
        const bool setCurveFailed = clearException(env);
        configured = setCurve && !setCurveFailed;
    }
    if (configured) {
        jclass stringClass = env->FindClass("java/lang/String");
        jobjectArray digests = stringClass ? env->NewObjectArray(1, stringClass, nullptr) : nullptr;
        jstring digest = env->NewStringUTF(AY_OBFUSCATE_KEY("SHA-256", KS_DIGEST));
        if (digests && digest) env->SetObjectArrayElement(digests, 0, digest);
        jmethodID setDigests = env->GetMethodID(
            builderClass,
            "setDigests",
            "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
        jobject ignored = setDigests ? env->CallObjectMethod(builder, setDigests, digests) : nullptr;
        if (ignored) env->DeleteLocalRef(ignored);
        if (digest) env->DeleteLocalRef(digest);
        if (digests) env->DeleteLocalRef(digests);
        if (stringClass) env->DeleteLocalRef(stringClass);
        const bool setDigestsFailed = clearException(env);
        configured = setDigests && !setDigestsFailed;
    }
    if (configured) {
        jmethodID setAuth = env->GetMethodID(
            builderClass,
            "setUserAuthenticationRequired",
            "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
        jobject ignored = setAuth ? env->CallObjectMethod(builder, setAuth, JNI_FALSE) : nullptr;
        if (ignored) env->DeleteLocalRef(ignored);
        const bool setAuthFailed = clearException(env);
        configured = setAuth && !setAuthFailed;
    }

    jobject specification = nullptr;
    if (configured) {
        jmethodID build = env->GetMethodID(
            builderClass,
            "build",
            "()Landroid/security/keystore/KeyGenParameterSpec;");
        specification = build ? env->CallObjectMethod(builder, build) : nullptr;
        const bool buildFailed = clearException(env);
        configured = specification && !buildFailed;
    }
    if (configured) {
        jclass generatorObjectClass = env->GetObjectClass(generator);
        jmethodID initialize = env->GetMethodID(
            generatorObjectClass,
            "initialize",
            "(Ljava/security/spec/AlgorithmParameterSpec;)V");
        jmethodID generate = env->GetMethodID(
            generatorObjectClass,
            "generateKeyPair",
            "()Ljava/security/KeyPair;");
        if (initialize) env->CallVoidMethod(generator, initialize, specification);
        const bool initializeFailed = clearException(env);
        jobject pair = generate && initialize && !initializeFailed
            ? env->CallObjectMethod(generator, generate)
            : nullptr;
        const bool generationFailed = clearException(env);
        configured = pair && !generationFailed;
        if (pair) env->DeleteLocalRef(pair);
        env->DeleteLocalRef(generatorObjectClass);
    }

    if (specification) env->DeleteLocalRef(specification);
    if (builder) env->DeleteLocalRef(builder);
    if (builderClass) env->DeleteLocalRef(builderClass);
    if (curve) env->DeleteLocalRef(curve);
    if (curveClass) env->DeleteLocalRef(curveClass);
    env->DeleteLocalRef(generator);
    env->DeleteLocalRef(generatorClass);
    return configured;
}

static InstallIdentity loadInstallIdentity(JNIEnv* env) {
    InstallIdentity identity;
    if (!env) return identity;
    jclass keyStoreClass = env->FindClass("java/security/KeyStore");
    jmethodID getInstance = keyStoreClass
        ? env->GetStaticMethodID(
            keyStoreClass,
            "getInstance",
            "(Ljava/lang/String;)Ljava/security/KeyStore;")
        : nullptr;
    jstring provider = env->NewStringUTF(AY_OBFUSCATE_KEY("AndroidKeyStore", KS_PROVIDER));
    jobject keyStore = getInstance
        ? env->CallStaticObjectMethod(keyStoreClass, getInstance, provider)
        : nullptr;
    if (provider) env->DeleteLocalRef(provider);
    const bool keyStoreFailed = clearException(env);
    if (!keyStore || keyStoreFailed) {
        if (keyStore) env->DeleteLocalRef(keyStore);
        if (keyStoreClass) env->DeleteLocalRef(keyStoreClass);
        return identity;
    }
    jmethodID load = env->GetMethodID(
        keyStoreClass,
        "load",
        "(Ljava/security/KeyStore$LoadStoreParameter;)V");
    if (load) env->CallVoidMethod(keyStore, load, nullptr);
    const bool loadFailed = clearException(env);
    if (!load || loadFailed) {
        env->DeleteLocalRef(keyStore);
        env->DeleteLocalRef(keyStoreClass);
        return identity;
    }

    jstring alias = env->NewStringUTF(AY_OBFUSCATE_KEY("goat.p2.install.identity", KS_ALIAS));
    if (!alias || !ensureKeystoreAlias(env, keyStore, alias)) {
        if (alias) env->DeleteLocalRef(alias);
        env->DeleteLocalRef(keyStore);
        env->DeleteLocalRef(keyStoreClass);
        return identity;
    }

    jmethodID getCertificate = env->GetMethodID(
        keyStoreClass,
        "getCertificate",
        "(Ljava/lang/String;)Ljava/security/cert/Certificate;");
    jmethodID getKey = env->GetMethodID(
        keyStoreClass,
        "getKey",
        "(Ljava/lang/String;[C)Ljava/security/Key;");
    jobject certificate = getCertificate
        ? env->CallObjectMethod(keyStore, getCertificate, alias)
        : nullptr;
    jobject privateKey = getKey
        ? env->CallObjectMethod(keyStore, getKey, alias, nullptr)
        : nullptr;
    if (clearException(env)) {
        if (certificate) env->DeleteLocalRef(certificate);
        if (privateKey) env->DeleteLocalRef(privateKey);
        certificate = nullptr;
        privateKey = nullptr;
    }

    if (certificate && privateKey) {
        jclass certificateClass = env->GetObjectClass(certificate);
        jmethodID getPublicKey = env->GetMethodID(
            certificateClass,
            "getPublicKey",
            "()Ljava/security/PublicKey;");
        jobject publicKey = getPublicKey
            ? env->CallObjectMethod(certificate, getPublicKey)
            : nullptr;
        const bool publicKeyFailed = clearException(env);
        if (publicKey && !publicKeyFailed) {
            jclass publicKeyClass = env->GetObjectClass(publicKey);
            jmethodID getEncoded = env->GetMethodID(publicKeyClass, "getEncoded", "()[B");
            jbyteArray encoded = getEncoded
                ? static_cast<jbyteArray>(env->CallObjectMethod(publicKey, getEncoded))
                : nullptr;
            identity.publicKey = byteArrayToVector(env, encoded);
            if (encoded) env->DeleteLocalRef(encoded);
            env->DeleteLocalRef(publicKeyClass);
            env->DeleteLocalRef(publicKey);
        } else if (publicKey) {
            env->DeleteLocalRef(publicKey);
        }
        env->DeleteLocalRef(certificateClass);
    }
    if (!identity.publicKey.empty() && privateKey) {
        identity.privateKey = privateKey;
    } else if (privateKey) {
        env->DeleteLocalRef(privateKey);
    }
    if (certificate) env->DeleteLocalRef(certificate);
    env->DeleteLocalRef(alias);
    env->DeleteLocalRef(keyStore);
    env->DeleteLocalRef(keyStoreClass);
    return identity;
}

static std::vector<uint8_t> signWithInstallKey(
    JNIEnv* env,
    jobject privateKey,
    const std::string& canonical) {
    std::vector<uint8_t> output;
    if (!env || !privateKey || canonical.empty()) return output;
    jclass signatureClass = env->FindClass("java/security/Signature");
    jmethodID getInstance = signatureClass
        ? env->GetStaticMethodID(
            signatureClass,
            "getInstance",
            "(Ljava/lang/String;)Ljava/security/Signature;")
        : nullptr;
    jstring algorithm = env->NewStringUTF(AY_OBFUSCATE_KEY("SHA256withECDSA", KS_SIGNATURE));
    jobject signature = getInstance
        ? env->CallStaticObjectMethod(signatureClass, getInstance, algorithm)
        : nullptr;
    if (algorithm) env->DeleteLocalRef(algorithm);
    const bool signatureCreateFailed = clearException(env);
    if (!signature || signatureCreateFailed) {
        if (signature) env->DeleteLocalRef(signature);
        if (signatureClass) env->DeleteLocalRef(signatureClass);
        return output;
    }
    jmethodID initSign = env->GetMethodID(signatureClass, "initSign", "(Ljava/security/PrivateKey;)V");
    jmethodID update = env->GetMethodID(signatureClass, "update", "([B)V");
    jmethodID sign = env->GetMethodID(signatureClass, "sign", "()[B");
    jbyteArray message = vectorToByteArray(
        env,
        reinterpret_cast<const uint8_t*>(canonical.data()),
        canonical.size());
    if (initSign) env->CallVoidMethod(signature, initSign, privateKey);
    const bool initSignFailed = clearException(env);
    if (update && message && !initSignFailed) env->CallVoidMethod(signature, update, message);
    const bool updateFailed = clearException(env);
    jbyteArray encoded = sign && !updateFailed
        ? static_cast<jbyteArray>(env->CallObjectMethod(signature, sign))
        : nullptr;
    const bool signFailed = clearException(env);
    if (!signFailed) output = byteArrayToVector(env, encoded);
    if (encoded) env->DeleteLocalRef(encoded);
    if (message) env->DeleteLocalRef(message);
    env->DeleteLocalRef(signature);
    env->DeleteLocalRef(signatureClass);
    return output;
}

static bool verifyServerSignature(
    JNIEnv* env,
    const std::vector<uint8_t>& payload,
    const std::vector<uint8_t>& derSignature) {
    if (!env || payload.empty() || derSignature.empty()) return false;
    const std::vector<uint8_t> spki = base64UrlDecode(serverPublicSpki(), 256);
    if (spki.empty()) return false;

    jclass specClass = env->FindClass("java/security/spec/X509EncodedKeySpec");
    jmethodID specConstructor = specClass
        ? env->GetMethodID(specClass, "<init>", "([B)V")
        : nullptr;
    jbyteArray spkiArray = vectorToByteArray(env, spki.data(), spki.size());
    jobject specification = specConstructor
        ? env->NewObject(specClass, specConstructor, spkiArray)
        : nullptr;
    const bool specificationFailed = clearException(env);
    if (!specification || specificationFailed) {
        if (specification) env->DeleteLocalRef(specification);
        if (spkiArray) env->DeleteLocalRef(spkiArray);
        if (specClass) env->DeleteLocalRef(specClass);
        return false;
    }

    jclass factoryClass = env->FindClass("java/security/KeyFactory");
    jmethodID factoryGetInstance = factoryClass
        ? env->GetStaticMethodID(
            factoryClass,
            "getInstance",
            "(Ljava/lang/String;)Ljava/security/KeyFactory;")
        : nullptr;
    jstring ec = env->NewStringUTF(AY_OBFUSCATE_KEY("EC", KS_EC));
    jobject factory = factoryGetInstance
        ? env->CallStaticObjectMethod(factoryClass, factoryGetInstance, ec)
        : nullptr;
    if (ec) env->DeleteLocalRef(ec);
    jobject publicKey = nullptr;
    const bool factoryFailed = clearException(env);
    if (factory && !factoryFailed) {
        jmethodID generatePublic = env->GetMethodID(
            factoryClass,
            "generatePublic",
            "(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;");
        publicKey = generatePublic
            ? env->CallObjectMethod(factory, generatePublic, specification)
            : nullptr;
        clearException(env);
    }

    bool verified = false;
    if (publicKey) {
        jclass signatureClass = env->FindClass("java/security/Signature");
        jmethodID getInstance = signatureClass
            ? env->GetStaticMethodID(
                signatureClass,
                "getInstance",
                "(Ljava/lang/String;)Ljava/security/Signature;")
            : nullptr;
        jstring algorithm = env->NewStringUTF(AY_OBFUSCATE_KEY("SHA256withECDSA", KS_SIGNATURE));
        jobject signature = getInstance
            ? env->CallStaticObjectMethod(signatureClass, getInstance, algorithm)
            : nullptr;
        if (algorithm) env->DeleteLocalRef(algorithm);
        const bool signatureFailed = clearException(env);
        if (signature && !signatureFailed) {
            jmethodID initVerify = env->GetMethodID(
                signatureClass,
                "initVerify",
                "(Ljava/security/PublicKey;)V");
            jmethodID update = env->GetMethodID(signatureClass, "update", "([B)V");
            jmethodID verify = env->GetMethodID(signatureClass, "verify", "([B)Z");
            jbyteArray payloadArray = vectorToByteArray(env, payload.data(), payload.size());
            jbyteArray signatureArray = vectorToByteArray(env, derSignature.data(), derSignature.size());
            if (initVerify) env->CallVoidMethod(signature, initVerify, publicKey);
            const bool initVerifyFailed = clearException(env);
            if (update && payloadArray && !initVerifyFailed)
                env->CallVoidMethod(signature, update, payloadArray);
            const bool updateFailed = clearException(env);
            if (verify && signatureArray && !updateFailed) {
                verified = env->CallBooleanMethod(signature, verify, signatureArray) == JNI_TRUE;
                if (clearException(env)) verified = false;
            }
            if (payloadArray) env->DeleteLocalRef(payloadArray);
            if (signatureArray) env->DeleteLocalRef(signatureArray);
            env->DeleteLocalRef(signature);
        } else if (signature) {
            env->DeleteLocalRef(signature);
        }
        if (signatureClass) env->DeleteLocalRef(signatureClass);
        env->DeleteLocalRef(publicKey);
    }

    if (factory) env->DeleteLocalRef(factory);
    if (factoryClass) env->DeleteLocalRef(factoryClass);
    env->DeleteLocalRef(specification);
    env->DeleteLocalRef(spkiArray);
    env->DeleteLocalRef(specClass);
    return verified;
}

struct HttpResult {
    int status = 0;
    std::string body;
};

static HttpResult httpPostJson(JNIEnv* env, const std::string& url, const std::string& body) {
    HttpResult result;
    if (!env || url.empty() || body.empty() || body.size() > 12288) return result;
    jclass urlClass = env->FindClass("java/net/URL");
    jmethodID constructor = urlClass
        ? env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V")
        : nullptr;
    jstring urlString = env->NewStringUTF(url.c_str());
    jobject urlObject = constructor ? env->NewObject(urlClass, constructor, urlString) : nullptr;
    if (urlString) env->DeleteLocalRef(urlString);
    const bool urlFailed = clearException(env);
    if (!urlObject || urlFailed) {
        if (urlObject) env->DeleteLocalRef(urlObject);
        if (urlClass) env->DeleteLocalRef(urlClass);
        return result;
    }
    jmethodID openConnection = env->GetMethodID(
        urlClass,
        "openConnection",
        "()Ljava/net/URLConnection;");
    jobject connection = openConnection
        ? env->CallObjectMethod(urlObject, openConnection)
        : nullptr;
    env->DeleteLocalRef(urlObject);
    env->DeleteLocalRef(urlClass);
    const bool connectionFailed = clearException(env);
    if (!connection || connectionFailed) {
        if (connection) env->DeleteLocalRef(connection);
        return result;
    }

    jclass connectionClass = env->FindClass("java/net/HttpURLConnection");
    jmethodID setRequestMethod = env->GetMethodID(
        connectionClass, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setDoOutput = env->GetMethodID(connectionClass, "setDoOutput", "(Z)V");
    jmethodID setConnectTimeout = env->GetMethodID(connectionClass, "setConnectTimeout", "(I)V");
    jmethodID setReadTimeout = env->GetMethodID(connectionClass, "setReadTimeout", "(I)V");
    jmethodID setProperty = env->GetMethodID(
        connectionClass,
        "setRequestProperty",
        "(Ljava/lang/String;Ljava/lang/String;)V");
    jstring post = env->NewStringUTF("POST");
    env->CallVoidMethod(connection, setRequestMethod, post);
    env->DeleteLocalRef(post);
    env->CallVoidMethod(connection, setDoOutput, JNI_TRUE);
    env->CallVoidMethod(connection, setConnectTimeout, static_cast<jint>(6500));
    env->CallVoidMethod(connection, setReadTimeout, static_cast<jint>(9000));
    jstring contentName = env->NewStringUTF("Content-Type");
    jstring contentValue = env->NewStringUTF("application/json; charset=utf-8");
    env->CallVoidMethod(connection, setProperty, contentName, contentValue);
    env->DeleteLocalRef(contentName);
    env->DeleteLocalRef(contentValue);
    jstring acceptName = env->NewStringUTF("Accept");
    jstring acceptValue = env->NewStringUTF("application/json");
    env->CallVoidMethod(connection, setProperty, acceptName, acceptValue);
    env->DeleteLocalRef(acceptName);
    env->DeleteLocalRef(acceptValue);
    if (clearException(env)) {
        env->DeleteLocalRef(connection);
        env->DeleteLocalRef(connectionClass);
        return result;
    }

    jmethodID getOutputStream = env->GetMethodID(
        connectionClass,
        "getOutputStream",
        "()Ljava/io/OutputStream;");
    jobject outputStream = getOutputStream
        ? env->CallObjectMethod(connection, getOutputStream)
        : nullptr;
    const bool outputFailed = clearException(env);
    if (!outputStream || outputFailed) {
        if (outputStream) env->DeleteLocalRef(outputStream);
        env->DeleteLocalRef(connection);
        env->DeleteLocalRef(connectionClass);
        return result;
    }
    jclass outputClass = env->GetObjectClass(outputStream);
    jmethodID write = env->GetMethodID(outputClass, "write", "([B)V");
    jmethodID flush = env->GetMethodID(outputClass, "flush", "()V");
    jmethodID closeOutput = env->GetMethodID(outputClass, "close", "()V");
    jbyteArray bodyArray = vectorToByteArray(
        env,
        reinterpret_cast<const uint8_t*>(body.data()),
        body.size());
    if (write && bodyArray) env->CallVoidMethod(outputStream, write, bodyArray);
    if (flush && !clearException(env)) env->CallVoidMethod(outputStream, flush);
    if (closeOutput && !clearException(env)) env->CallVoidMethod(outputStream, closeOutput);
    if (bodyArray) env->DeleteLocalRef(bodyArray);
    env->DeleteLocalRef(outputClass);
    env->DeleteLocalRef(outputStream);
    if (clearException(env)) {
        env->DeleteLocalRef(connection);
        env->DeleteLocalRef(connectionClass);
        return result;
    }

    jmethodID getResponseCode = env->GetMethodID(connectionClass, "getResponseCode", "()I");
    result.status = getResponseCode ? env->CallIntMethod(connection, getResponseCode) : 0;
    if (clearException(env)) result.status = 0;
    const char* streamName = result.status >= 200 && result.status < 300
        ? "getInputStream"
        : "getErrorStream";
    jmethodID getStream = env->GetMethodID(
        connectionClass,
        streamName,
        "()Ljava/io/InputStream;");
    jobject inputStream = getStream
        ? env->CallObjectMethod(connection, getStream)
        : nullptr;
    if (!clearException(env) && inputStream) {
        jclass inputClass = env->GetObjectClass(inputStream);
        jmethodID readMethod = env->GetMethodID(inputClass, "read", "([B)I");
        jmethodID closeInput = env->GetMethodID(inputClass, "close", "()V");
        jbyteArray buffer = env->NewByteArray(2048);
        while (readMethod && buffer && result.body.size() <= 16384) {
            const jint count = env->CallIntMethod(inputStream, readMethod, buffer);
            if (clearException(env) || count <= 0) break;
            if (result.body.size() + static_cast<size_t>(count) > 16384) {
                result.body.clear();
                break;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(count));
            env->GetByteArrayRegion(buffer, 0, count, reinterpret_cast<jbyte*>(bytes.data()));
            if (clearException(env)) {
                result.body.clear();
                break;
            }
            result.body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        if (closeInput) env->CallVoidMethod(inputStream, closeInput);
        clearException(env);
        if (buffer) env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(inputClass);
        env->DeleteLocalRef(inputStream);
    } else {
        clearException(env);
    }
    jmethodID disconnect = env->GetMethodID(connectionClass, "disconnect", "()V");
    if (disconnect) env->CallVoidMethod(connection, disconnect);
    clearException(env);
    env->DeleteLocalRef(connection);
    env->DeleteLocalRef(connectionClass);
    return result;
}

static std::string extractJsonString(const std::string& json, const char* field) {
    const std::string needle = std::string("\"") + field + "\":\"";
    const size_t beginning = json.find(needle);
    if (beginning == std::string::npos) return {};
    const size_t content = beginning + needle.size();
    const size_t end = json.find('"', content);
    if (end == std::string::npos || end - content > 8192) return {};
    const std::string output = json.substr(content, end - content);
    if (output.find('\\') != std::string::npos) return {};
    return output;
}

static int extractJsonInteger(const std::string& json, const char* field) {
    const std::string needle = std::string("\"") + field + "\":";
    const size_t beginning = json.find(needle);
    if (beginning == std::string::npos) return -1;
    char* end = nullptr;
    const long parsed = std::strtol(json.c_str() + beginning + needle.size(), &end, 10);
    if (!end || end == json.c_str() + beginning + needle.size()) return -1;
    return static_cast<int>(parsed);
}

static std::vector<std::string> splitCanonical(const std::string& payload) {
    std::vector<std::string> fields;
    size_t offset = 0;
    while (offset <= payload.size()) {
        const size_t end = payload.find('\n', offset);
        if (end == std::string::npos) {
            fields.push_back(payload.substr(offset));
            break;
        }
        fields.push_back(payload.substr(offset, end - offset));
        offset = end + 1;
    }
    return fields;
}

static bool parseUnsigned64(const std::string& input, uint64_t& output) {
    if (input.empty() || input.size() > 20) return false;
    uint64_t value = 0;
    for (char character : input) {
        if (character < '0' || character > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10ULL) return false;
        value = value * 10ULL + digit;
    }
    output = value;
    return true;
}

static bool parseGrantGameMask(const std::string& grant, uint32_t& output) {
    // Pre-entitlement grants were signed random tokens. They remain full
    // access for compatibility; restricted keys are never issued to those
    // older binaries by the Worker.
    output = GOAT_GAME_ALL;
    if (grant.compare(0, 3, "G1.") != 0) return grant.size() >= 22;

    const size_t separator = grant.find('.', 3);
    if (separator == std::string::npos || separator == 3 || separator > 5) return false;
    uint64_t parsed = 0;
    if (!parseUnsigned64(grant.substr(3, separator - 3), parsed)) return false;
    if (parsed < 1 || parsed > GOAT_GAME_ALL || (parsed & ~GOAT_GAME_ALL) != 0) return false;
    if (grant.size() - separator - 1 < 22) return false;
    output = static_cast<uint32_t>(parsed);
    return true;
}

static bool grantLease(uint64_t sessionExpiryWallMs,
                       const std::string& sessionId,
                       const std::string& grant) {
    uint32_t gameMask = 0;
    if (!parseGrantGameMask(grant, gameMask)) return false;
    const uint64_t wallNow = wallClockMs();
    const uint64_t monoNow = monotonicMs();
    if (sessionExpiryWallMs <= wallNow) return false;
    uint64_t duration = sessionExpiryWallMs - wallNow;
    if (duration > kMaximumLeaseMs) duration = kMaximumLeaseMs;
    const uint64_t deadline = monoNow + duration;
    const uint64_t secretA = s_secretA.load(std::memory_order_acquire);
    const uint64_t secretB = s_secretB.load(std::memory_order_acquire);
    if (!secretA || !secretB) return false;

    uint8_t digest[32];
    const std::string material = sessionId + "|" + grant + "|" + std::to_string(sessionExpiryWallMs);
    sha256Bytes(reinterpret_cast<const uint8_t*>(material.data()), material.size(), digest);
    uint64_t grantDigest = 0;
    std::memcpy(&grantDigest, digest, sizeof(grantDigest));
    const uint64_t generation = s_generation.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    const uint64_t token = deadline ^ mix64(secretA ^ generation ^ 0x95a6dca8734b2e11ULL);
    const uint64_t gameBinding = mix64(static_cast<uint64_t>(gameMask) ^ 0x68bc21f937e4ad05ULL);
    const uint64_t marker = mix64(secretB ^ token ^ generation ^ grantDigest ^ gameBinding ^ 0x47f20a9dce5138b7ULL);
    s_grantDigest.store(grantDigest, std::memory_order_release);
    s_deadlineToken.store(token, std::memory_order_release);
    s_gameMask.store(gameMask, std::memory_order_release);
    s_accessMarker.store(marker, std::memory_order_release);
    return true;
}

} // namespace

bool license_security_init(JNIEnv* env, jobject activity) {
    std::lock_guard<std::mutex> lock(s_initMutex);
    if (s_initialized) return s_integrityReady.load(std::memory_order_acquire);
    s_initialized = true;

    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    uint8_t random[16];
    if (!secureRandom(random, sizeof(random))) return false;
    uint64_t a = 0;
    uint64_t b = 0;
    std::memcpy(&a, random, sizeof(a));
    std::memcpy(&b, random + sizeof(a), sizeof(b));
    a = mix64(a ^ monotonicMs() ^ reinterpret_cast<uintptr_t>(&s_initMutex));
    b = mix64(b ^ wallClockMs() ^ reinterpret_cast<uintptr_t>(&license_security_init));
    if (!a || !b || a == b) return false;
    s_secretA.store(a, std::memory_order_release);
    s_secretB.store(b, std::memory_order_release);

    if (processIsTraced() || suspiciousMappingsPresent() || javaDebuggerConnected(env)) return false;
    if (!verifyAppCertificate(env, activity)) return false;
    if (!readSelfHash(s_binaryHash)) return false;

    InstallIdentity identity = loadInstallIdentity(env);
    const bool identityReady = identity.privateKey && identity.publicKey.size() >= 80;
    if (identity.privateKey) env->DeleteLocalRef(identity.privateKey);
    s_integrityReady.store(identityReady, std::memory_order_release);
    return identityReady;
}

int license_security_validate(JNIEnv* env,
                              jobject activity,
                              const std::string& normalizedKey,
                              const std::string& deviceId,
                              std::string& outLicenseExpiry) {
    outLicenseExpiry.clear();
    if (!env || !activity || normalizedKey.empty() || deviceId.empty()) return 3;
    if (!license_security_init(env, activity)
        || !s_integrityReady.load(std::memory_order_acquire)) return 6;
    if (processIsTraced() || suspiciousMappingsPresent() || javaDebuggerConnected(env)) {
        license_security_revoke();
        return 6;
    }

    std::string currentHash;
    if (!readSelfHash(currentHash) || !constantTimeHexEquals(currentHash, s_binaryHash)) {
        license_security_revoke();
        return 6;
    }

    InstallIdentity identity = loadInstallIdentity(env);
    if (!identity.privateKey || identity.publicKey.size() < 80) {
        if (identity.privateKey) env->DeleteLocalRef(identity.privateKey);
        return 6;
    }
    uint8_t publicDigest[32];
    sha256Bytes(identity.publicKey.data(), identity.publicKey.size(), publicDigest);
    const std::string publicHash = hexLower(publicDigest, sizeof(publicDigest));

    uint8_t nonceBytes[24];
    if (!secureRandom(nonceBytes, sizeof(nonceBytes))) {
        env->DeleteLocalRef(identity.privateKey);
        return 6;
    }
    const std::string nonce = base64UrlEncode(nonceBytes, sizeof(nonceBytes));
    const uint64_t timestamp = wallClockMs();
    const std::string canonical = std::string("GOAT-AUTH-2\n")
        + normalizedKey + "\n"
        + deviceId + "\n"
        + buildId() + "\n"
        + s_binaryHash + "\n"
        + nonce + "\n"
        + std::to_string(timestamp) + "\n"
        + publicHash;
    const std::vector<uint8_t> proof = signWithInstallKey(env, identity.privateKey, canonical);
    env->DeleteLocalRef(identity.privateKey);
    if (proof.empty()) return 6;

    const std::string publicEncoded = base64UrlEncode(identity.publicKey.data(), identity.publicKey.size());
    const std::string proofEncoded = base64UrlEncode(proof.data(), proof.size());
    const std::string request = std::string("{")
        + "\"protocol\":" + std::to_string(kProtocolVersion)
        + ",\"key\":\"" + normalizedKey + "\""
        + ",\"device_id\":\"" + deviceId + "\""
        + ",\"build_id\":\"" + buildId() + "\""
        + ",\"binary_hash\":\"" + s_binaryHash + "\""
        + ",\"nonce\":\"" + nonce + "\""
        + ",\"timestamp_ms\":" + std::to_string(timestamp)
        + ",\"install_pub\":\"" + publicEncoded + "\""
        + ",\"proof\":\"" + proofEncoded + "\"}";

    const HttpResult response = httpPostJson(env, sessionEndpoint(), request);
    if (response.status == 0 || response.body.empty()) return 0;
    if (extractJsonInteger(response.body, "protocol") != kProtocolVersion) return 6;
    const std::string payloadEncoded = extractJsonString(response.body, "payload");
    const std::string signatureEncoded = extractJsonString(response.body, "signature");
    const std::vector<uint8_t> payload = base64UrlDecode(payloadEncoded, 8192);
    const std::vector<uint8_t> signature = base64UrlDecode(signatureEncoded, 256);
    if (payload.empty() || signature.empty() || !verifyServerSignature(env, payload, signature)) return 6;

    const std::string payloadText(reinterpret_cast<const char*>(payload.data()), payload.size());
    const std::vector<std::string> fields = splitCanonical(payloadText);
    if (fields.size() != 13 || fields[0] != "GOAT-LICENSE-2") return 6;
    if (fields[3] != sha256Hex(normalizedKey)
        || fields[4] != deviceId
        || fields[5] != buildId()
        || fields[6] != s_binaryHash
        || fields[7] != nonce) {
        return 6;
    }
    uint64_t issuedAt = 0;
    uint64_t sessionExpiry = 0;
    if (!parseUnsigned64(fields[8], issuedAt) || !parseUnsigned64(fields[9], sessionExpiry)) return 6;
    const uint64_t now = wallClockMs();
    const uint64_t skew = now > issuedAt ? now - issuedAt : issuedAt - now;
    if (skew > 5ULL * 60ULL * 1000ULL || sessionExpiry < issuedAt
        || sessionExpiry - issuedAt > kMaximumLeaseMs) {
        return 6;
    }

    if (fields[1] != "1" || fields[2] != "ok") {
        if (fields[2] == "license_expired") return 2;
        if (fields[2] == "device_mismatch") return 4;
        if (fields[2] == "license_denied") return 5;
        if (fields[2] == "rate_limited") return 0;
        return 6;
    }
    if (response.status != 200 || fields[11].empty() || fields[12].size() < 22) return 6;
    if (!grantLease(sessionExpiry, fields[11], fields[12])) return 6;
    if (!license_security_has_access()) return 6;
    outLicenseExpiry = fields[10];
    return 1;
}

bool license_security_has_access() {
    const uint64_t marker = s_accessMarker.load(std::memory_order_acquire);
    const uint64_t generation = s_generation.load(std::memory_order_acquire);
    const uint64_t token = s_deadlineToken.load(std::memory_order_acquire);
    const uint64_t grantDigest = s_grantDigest.load(std::memory_order_acquire);
    const uint32_t gameMask = s_gameMask.load(std::memory_order_acquire);
    const uint64_t secretA = s_secretA.load(std::memory_order_acquire);
    const uint64_t secretB = s_secretB.load(std::memory_order_acquire);
    if (!s_integrityReady.load(std::memory_order_acquire)
        || !marker || !generation || !token || !secretA || !secretB
        || gameMask < 1 || gameMask > GOAT_GAME_ALL || (gameMask & ~GOAT_GAME_ALL) != 0) return false;
    const uint64_t gameBinding = mix64(static_cast<uint64_t>(gameMask) ^ 0x68bc21f937e4ad05ULL);
    const uint64_t expected = mix64(secretB ^ token ^ generation ^ grantDigest ^ gameBinding ^ 0x47f20a9dce5138b7ULL);
    if (marker != expected) {
        license_security_revoke();
        return false;
    }
    const uint64_t deadline = token ^ mix64(secretA ^ generation ^ 0x95a6dca8734b2e11ULL);
    const uint64_t now = monotonicMs();
    if (deadline <= now || deadline - now > kMaximumLeaseMs) {
        license_security_revoke();
        return false;
    }

    uint64_t previousProbe = s_lastProbeMs.load(std::memory_order_relaxed);
    if (now - previousProbe >= kProbeIntervalMs
        && s_lastProbeMs.compare_exchange_strong(previousProbe, now, std::memory_order_acq_rel)) {
        if (processIsTraced() || suspiciousMappingsPresent()) {
            license_security_revoke();
            return false;
        }
    }
    return true;
}

uint32_t license_security_game_mask() {
    if (!license_security_has_access()) return 0;
    return s_gameMask.load(std::memory_order_acquire) & GOAT_GAME_ALL;
}

bool license_security_allows_game(uint32_t game) {
    if (game != GOAT_GAME_POOL && game != GOAT_GAME_SOLITAIRE && game != GOAT_GAME_ONOO) return false;
    return (license_security_game_mask() & game) != 0;
}

void license_security_revoke() {
    s_accessMarker.store(0, std::memory_order_release);
    s_deadlineToken.store(0, std::memory_order_release);
    s_grantDigest.store(0, std::memory_order_release);
    s_gameMask.store(0, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::string license_security_binary_hash() {
    std::lock_guard<std::mutex> lock(s_initMutex);
    return s_binaryHash;
}
