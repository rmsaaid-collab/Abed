#pragma once
#include <stdint.h>
#include <stddef.h>

struct Il2CppClass;
struct MonitorData;

struct Il2CppObject {
    Il2CppClass* klass;
    MonitorData* monitor;
};

struct Il2CppArray {
    Il2CppObject obj;
    void*        bounds;
    uintptr_t    max_length;
};

static_assert(sizeof(Il2CppArray) == 0x20, "Il2CppArray must be 32 bytes");

struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Vector4 { float x, y, z, w; };

struct Matrix4x4 {
    float m[16];
};
