#pragma once
#include <jni.h>
#include <atomic>
#include <string>
#include <time.h>

#define LIC_BTN_ACTIVATE    300
#define LIC_BTN_COPY_DEVID  301
#define LIC_BTN_SUPPORT     302

extern std::atomic<bool> g_licenseValidated;

void   license_init(JNIEnv* env, jobject activity, jobject listenerView);
bool   checkSavedLicense(JNIEnv* env);
void   showLicenseDialog(JNIEnv* env);
void   onLicenseClick(JNIEnv* env, int btnId);
void   license_checkPendingResult(JNIEnv* env);
bool   license_enforce_runtime(JNIEnv* env);
int    validateKey(const std::string& raw);
time_t getExpiryTime(JNIEnv* env);
