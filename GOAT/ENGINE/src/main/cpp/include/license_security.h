#pragma once

#include <jni.h>
#include <stdint.h>
#include <string>

// Signed per-license game entitlements. Multiple bits may be combined.
constexpr uint32_t GOAT_GAME_POOL = 1U;
constexpr uint32_t GOAT_GAME_SOLITAIRE = 2U;
constexpr uint32_t GOAT_GAME_ONOO = 4U;
constexpr uint32_t GOAT_GAME_ALL = GOAT_GAME_POOL | GOAT_GAME_SOLITAIRE | GOAT_GAME_ONOO;

// Initializes immutable integrity material and the per-install Android
// Keystore identity. Returns false when the signed package or native image is
// not the protected production build.
bool license_security_init(JNIEnv* env, jobject activity);

// Performs the protocol-v2 challenge/response exchange. The returned status
// deliberately matches the legacy UI status codes used by license.cpp.
int license_security_validate(JNIEnv* env,
                              jobject activity,
                              const std::string& normalizedKey,
                              const std::string& deviceId,
                              std::string& outLicenseExpiry);

// Access is represented by a short-lived, integrity-checked lease rather than
// a writable global boolean. Every protected subsystem calls this gate.
bool license_security_has_access();
uint32_t license_security_game_mask();
bool license_security_allows_game(uint32_t game);
void license_security_revoke();

// Hash of the exact on-disk libengine.so currently loaded in this process.
// Used to bind server grants to one production binary.
std::string license_security_binary_hash();
