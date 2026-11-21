#pragma once

#include <cstdint>
#include <cstring>
#include <dlfcn.h>

// Jawaker 28.2.90 uses one exact IL2CPP build.  Android linker namespaces can
// make RTLD_NOLOAD/dlsym unable to see that already-loaded library even though
// its mappings are present in the same process.  Keep dlsym as the preferred
// path and use the verified export RVAs only for that namespace-isolation case.
static inline void* goat_resolve_il2cpp_export(uintptr_t base, const char* symbol) {
    if (!symbol || !*symbol) return nullptr;

    void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        void* resolved = dlsym(handle, symbol);
        dlclose(handle);
        if (resolved) return resolved;
    }

    if (!base) return nullptr;

    struct ExportRva {
        const char* name;
        uintptr_t rva;
    };
    static constexpr ExportRva kExports[] = {
        {"il2cpp_init",                        0x044C6B58UL},
        {"il2cpp_array_new",                   0x044C6C6CUL},
        {"il2cpp_assembly_get_image",          0x044C6C80UL},
        {"il2cpp_class_from_name",             0x044C6CB8UL},
        {"il2cpp_class_get_name",              0x044C6CE4UL},
        {"il2cpp_class_get_static_field_data", 0x044C6DA0UL},
        {"il2cpp_domain_get",                  0x044C717CUL},
        {"il2cpp_domain_get_assemblies",       0x044C7188UL},
        {"il2cpp_gchandle_new",                0x044C73F4UL},
        {"il2cpp_object_get_class",            0x044C754CUL},
        {"il2cpp_string_new",                  0x044C75BCUL},
        {"il2cpp_image_get_name",              0x044C7854UL},
    };

    for (const ExportRva& entry : kExports) {
        if (std::strcmp(entry.name, symbol) == 0) {
            return reinterpret_cast<void*>(base + entry.rva);
        }
    }
    return nullptr;
}
