#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <pthread.h>
#include <unistd.h>

// Dobby is loaded first by EngineLibManager and is also a DT_NEEDED dependency
// of this small compatibility module. Keeping this fix separate means the
// proven game engine binary and every Solitaire/OONO decision path stay byte
// for byte unchanged.
extern "C" int DobbyHook(void* address, void* replace_call, void** origin_call);

namespace {

constexpr const char* kTag = "GOAT_LOADING_COMPAT";

// Jawaker 28.2.90 exact RVAs from the verified IL2CPP metadata. The loader
// refuses every other game version before this module can be staged.
constexpr uintptr_t kCrashlyticsGetEnabledRva = 0x058C0DEC;
constexpr uintptr_t kCrashlyticsSetEnabledRva = 0x058C0EA4;
constexpr uintptr_t kCrashlyticsInitializeRva = 0x058C0F0C;
constexpr uintptr_t kCrashlyticsLogRva = 0x058C0F58;
constexpr uintptr_t kCrashlyticsSetCustomKeyRva = 0x058C0FC0;
constexpr uintptr_t kCrashlyticsSetUserIdRva = 0x058C1038;
constexpr uintptr_t kCrashlyticsLogExceptionRva = 0x058C10A0;

std::atomic<int> g_installState{0};  // 0=pending, 1=installing, 2=ready, -1=failed
std::atomic<bool> g_suppressionLogged{false};
void* g_originals[7] = {};

#define COMPAT_LOG(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define COMPAT_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

bool isReadableElfBase(uintptr_t base) {
    if (!base) return false;
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    return header->e_ident[EI_MAG0] == ELFMAG0 &&
           header->e_ident[EI_MAG1] == ELFMAG1 &&
           header->e_ident[EI_MAG2] == ELFMAG2 &&
           header->e_ident[EI_MAG3] == ELFMAG3 &&
           header->e_ident[EI_CLASS] == ELFCLASS64 &&
           header->e_type == ET_DYN;
}

// Android's class-loader namespace can make RTLD_NOLOAD blind to an already
// mapped libil2cpp. In that case, select its offset-zero ELF mapping only after
// confirming that the same file also has an executable mapping.
uintptr_t moduleBaseFromMaps(const char* libraryName) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return 0;

    uintptr_t offsetZeroBase = 0;
    bool hasExecutableMapping = false;
    char line[4096] = {};
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long fileOffset = 0;
        unsigned long inode = 0;
        char permissions[5] = {};
        char device[32] = {};
        char path[3072] = {};
        const int fields = sscanf(
            line,
            "%lx-%lx %4s %lx %31s %lu %3071[^\n]",
            &start,
            &end,
            permissions,
            &fileOffset,
            device,
            &inode,
            path
        );
        if (fields < 7 || !strstr(path, libraryName)) continue;
        if (permissions[2] == 'x') hasExecutableMapping = true;
        if (fileOffset == 0 && permissions[0] == 'r') {
            offsetZeroBase = static_cast<uintptr_t>(start);
        }
    }
    fclose(maps);

    if (!hasExecutableMapping || !isReadableElfBase(offsetZeroBase)) return 0;
    return offsetZeroBase;
}

uintptr_t getModuleBase(const char* libraryName) {
    void* handle = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        void* anchor = dlsym(handle, "il2cpp_domain_get");
        Dl_info info{};
        const bool resolved = anchor && dladdr(anchor, &info) && info.dli_fbase;
        const uintptr_t base = resolved
            ? reinterpret_cast<uintptr_t>(info.dli_fbase)
            : 0;
        dlclose(handle);
        if (base) return base;
    }
    return moduleBaseFromMaps(libraryName);
}

bool isExecutableAddress(uintptr_t address) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return false;

    bool executable = false;
    char line[1024] = {};
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char permissions[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3) continue;
        if (address >= start && address < end) {
            executable = permissions[2] == 'x';
            break;
        }
    }
    fclose(maps);
    return executable;
}

void logSuppressionOnce() {
    bool expected = false;
    if (g_suppressionLogged.compare_exchange_strong(expected, true)) {
        COMPAT_LOG("[SUPPRESSED] optional Firebase Crashlytics call; account callback continues");
    }
}

// These methods only report telemetry. Returning a neutral result prevents the
// unavailable Firebase PlatformAccessor from throwing through Jawaker's
// CreateOrFindUser callback, which previously left the game on Loading.
extern "C" bool compatGetCollectionEnabled(void*) {
    logSuppressionOnce();
    return false;
}

extern "C" void compatSetCollectionEnabled(bool, void*) { logSuppressionOnce(); }
extern "C" void compatInitialize(void*) { logSuppressionOnce(); }
extern "C" void compatLog(void*, void*) { logSuppressionOnce(); }
extern "C" void compatSetCustomKey(void*, void*, void*) { logSuppressionOnce(); }
extern "C" void compatSetUserId(void*, void*) { logSuppressionOnce(); }
extern "C" void compatLogException(void*, void*) { logSuppressionOnce(); }

struct HookSpec {
    uintptr_t rva;
    void* replacement;
    void** original;
    const char* name;
};

bool installOneHook(uintptr_t base, const HookSpec& hook) {
    uintptr_t target = base + hook.rva;
    if (!isExecutableAddress(target)) {
        COMPAT_ERROR("[HOOK_INVALID] %s target=%p", hook.name, reinterpret_cast<void*>(target));
        return false;
    }

    int result = DobbyHook(
        reinterpret_cast<void*>(target),
        hook.replacement,
        hook.original
    );

    // Some Android ARM64 builds begin methods with BTI C. If the bundled Dobby
    // rejects that landing instruction, hook immediately after it, matching the
    // engine's already proven ARM64 fallback.
    if (result != 0) {
        const uint32_t firstInstruction = *reinterpret_cast<const uint32_t*>(target);
        if (firstInstruction == 0xD503245F) {
            target += sizeof(uint32_t);
            result = DobbyHook(
                reinterpret_cast<void*>(target),
                hook.replacement,
                hook.original
            );
        }
    }

    if (result != 0) {
        COMPAT_ERROR("[HOOK_FAILED] %s result=%d", hook.name, result);
        return false;
    }
    COMPAT_LOG("[HOOK_OK] %s target=%p", hook.name, reinterpret_cast<void*>(target));
    return true;
}

bool installCompatibilityHooks(uintptr_t base) {
    int expected = 0;
    if (!g_installState.compare_exchange_strong(expected, 1)) {
        return g_installState.load() == 2;
    }

    const HookSpec hooks[] = {
        {kCrashlyticsGetEnabledRva, reinterpret_cast<void*>(compatGetCollectionEnabled), &g_originals[0], "get_enabled"},
        {kCrashlyticsSetEnabledRva, reinterpret_cast<void*>(compatSetCollectionEnabled), &g_originals[1], "set_enabled"},
        {kCrashlyticsInitializeRva, reinterpret_cast<void*>(compatInitialize), &g_originals[2], "initialize"},
        {kCrashlyticsLogRva, reinterpret_cast<void*>(compatLog), &g_originals[3], "log"},
        {kCrashlyticsSetCustomKeyRva, reinterpret_cast<void*>(compatSetCustomKey), &g_originals[4], "set_custom_key"},
        {kCrashlyticsSetUserIdRva, reinterpret_cast<void*>(compatSetUserId), &g_originals[5], "set_user_id"},
        {kCrashlyticsLogExceptionRva, reinterpret_cast<void*>(compatLogException), &g_originals[6], "log_exception"},
    };

    bool success = true;
    for (const HookSpec& hook : hooks) {
        success = installOneHook(base, hook) && success;
    }
    g_installState.store(success ? 2 : -1);
    COMPAT_LOG("[READY] base=%p success=%d", reinterpret_cast<void*>(base), success ? 1 : 0);
    return success;
}

void* compatibilityThread(void*) {
    uintptr_t observedBase = 0;
    int stablePolls = 0;

    // Wait up to one minute for Unity to publish a stable IL2CPP mapping. This
    // thread sleeps between polls and never blocks Android's or Unity's main
    // thread.
    for (int poll = 0; poll < 6000; ++poll) {
        const uintptr_t current = getModuleBase("libil2cpp.so");
        if (current && current == observedBase) {
            ++stablePolls;
        } else {
            observedBase = current;
            stablePolls = current ? 1 : 0;
        }

        if (stablePolls >= 20) {
            installCompatibilityHooks(current);
            return nullptr;
        }
        usleep(10000);
    }

    g_installState.store(-1);
    COMPAT_ERROR("[TIMEOUT] libil2cpp.so did not become stable");
    return nullptr;
}

__attribute__((constructor)) void startLoadingCompatibility() {
    pthread_t thread{};
    const int result = pthread_create(&thread, nullptr, compatibilityThread, nullptr);
    if (result != 0) {
        g_installState.store(-1);
        COMPAT_ERROR("[THREAD_FAILED] result=%d", result);
        return;
    }
    pthread_detach(thread);
    COMPAT_LOG("[START] waiting for Jawaker 28.2.90 IL2CPP");
}

}  // namespace
