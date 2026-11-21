//
// Created by Milk on 4/9/21.
//

#include <IO.h>
#include <cstring>
#include "UnixFileSystemHook.h"
#import "JniHook/JniHook.h"
#include "BaseHook.h"

/*
 * Android's UnixFileSystem ABI differs between platform releases/OEM builds.
 * Older releases expose native canonicalize0(String), while newer builds use
 * a Java wrapper with a native canonicalize0(String, boolean) implementation.
 * Keep separate trampolines so RegisterNatives always receives the exact ABI.
 */
/*
 * File.mkdirs() recursively canonicalizes each missing parent.  A virtual
 * destination may intentionally live below its source root (for example
 * /storage/emulated/0 -> /storage/emulated/0/AkatskiGamespace).  Redirecting
 * the source root during that parent walk turns a parent into its own child
 * and causes unbounded File.mkdirs() recursion.  Keep ordinary file paths
 * redirected, but never let canonicalization expand an input into one of its
 * descendants.  The create/list/stat hooks still apply the actual I/O rule.
 */
static jstring redirectCanonicalPath(JNIEnv *env, jstring path) {
    if (path == nullptr)
        return path;

    jstring redirect = IO::redirectPath(env, path);
    if (redirect == nullptr || env->ExceptionCheck())
        return path;

    const char *sourceChars = env->GetStringUTFChars(path, nullptr);
    const char *redirectChars = env->GetStringUTFChars(redirect, nullptr);
    bool expandsIntoChild = false;
    if (sourceChars != nullptr && redirectChars != nullptr) {
        const size_t sourceLength = std::strlen(sourceChars);
        expandsIntoChild = sourceLength > 1 &&
                           std::strncmp(sourceChars, redirectChars, sourceLength) == 0 &&
                           redirectChars[sourceLength] == '/';
    }
    if (redirectChars != nullptr)
        env->ReleaseStringUTFChars(redirect, redirectChars);
    if (sourceChars != nullptr)
        env->ReleaseStringUTFChars(path, sourceChars);

    return expandsIntoChild ? path : redirect;
}

HOOK_JNI(jstring, canonicalize0_legacy, JNIEnv *env, jobject obj, jstring path) {
    jstring redirect = redirectCanonicalPath(env, path);
    return orig_canonicalize0_legacy(env, obj, redirect);
}

HOOK_JNI(jstring, canonicalize0_modern, JNIEnv *env, jobject obj, jstring path,
         jboolean resolve_parent_of_root_dir) {
    jstring redirect = redirectCanonicalPath(env, path);
    return orig_canonicalize0_modern(env, obj, redirect, resolve_parent_of_root_dir);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getBooleanAttributes0
 * Signature: (Ljava/lang/String;)I
 */
HOOK_JNI(jint, getBooleanAttributes0, JNIEnv *env, jobject obj, jstring abspath) {
    jstring redirect = IO::redirectPath(env, abspath);
    return orig_getBooleanAttributes0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getLastModifiedTime0
 * Signature: (Ljava/io/File;)J
 */
HOOK_JNI(jlong, getLastModifiedTime0, JNIEnv *env, jobject obj, jobject path) {
    jobject redirect = IO::redirectPath(env, path);
    return orig_getLastModifiedTime0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setPermission0
 * Signature: (Ljava/io/File;IZZ)Z
 */
HOOK_JNI(jboolean, setPermission0, JNIEnv *env, jobject obj, jobject file, jint access,
         jboolean enable, jboolean owneronly) {
    jobject redirect = IO::redirectPath(env, file);
    return orig_setPermission0(env, obj, redirect, access, enable, owneronly);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    createFileExclusively0
 * Signature: (Ljava/lang/String;)Z
 */
HOOK_JNI(jboolean, createFileExclusively0, JNIEnv *env, jobject obj, jstring path) {
    jstring redirect = IO::redirectPath(env, path);
    return orig_createFileExclusively0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    list0
 * Signature: (Ljava/io/File;)[Ljava/lang/String;
 */
HOOK_JNI(jobjectArray, list0, JNIEnv *env, jobject obj, jobject file) {
    jobject redirect = IO::redirectPath(env, file);
    return orig_list0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    createDirectory0
 * Signature: (Ljava/io/File;)Z
 */
HOOK_JNI(jboolean, createDirectory0, JNIEnv *env, jobject obj, jobject path) {
    jobject redirect = IO::redirectPath(env, path);
    return orig_createDirectory0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setLastModifiedTime0
 * Signature: (Ljava/io/File;J)Z
 */
HOOK_JNI(jboolean, setLastModifiedTime0, JNIEnv *env, jobject obj, jobject file, jlong time) {
    jobject redirect = IO::redirectPath(env, file);
    return orig_setLastModifiedTime0(env, obj, redirect, time);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setReadOnly0
 * Signature: (Ljava/io/File;)Z
 */
HOOK_JNI(jboolean, setReadOnly0, JNIEnv *env, jobject obj, jobject file) {
    jobject redirect = IO::redirectPath(env, file);
    return orig_setReadOnly0(env, obj, redirect);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getSpace0
 * Signature: (Ljava/io/File;I)J
 */
HOOK_JNI(jlong, getSpace0, JNIEnv *env, jobject obj, jobject file, jint t) {
    jobject redirect = IO::redirectPath(env, file);
    return orig_getSpace0(env, obj, redirect, t);
}

void UnixFileSystemHook::init(JNIEnv *env) {
    const char *className = "java/io/UnixFileSystem";

    // Prefer the modern native overload when present. Samsung's Android 13
    // backports this ABI even though canonicalize0(String) still exists as a
    // non-native wrapper, so selecting by method existence is required.
    jclass unixFileSystem = env->FindClass(className);
    jmethodID modernCanonicalize = nullptr;
    if (unixFileSystem != nullptr) {
        modernCanonicalize = env->GetMethodID(
                unixFileSystem, "canonicalize0",
                "(Ljava/lang/String;Z)Ljava/lang/String;");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            modernCanonicalize = nullptr;
        }
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (modernCanonicalize != nullptr) {
        JniHook::HookJniFun(env, className, "canonicalize0",
                            "(Ljava/lang/String;Z)Ljava/lang/String;",
                            (void *) new_canonicalize0_modern,
                            (void **) (&orig_canonicalize0_modern), false);
    } else {
        JniHook::HookJniFun(env, className, "canonicalize0",
                            "(Ljava/lang/String;)Ljava/lang/String;",
                            (void *) new_canonicalize0_legacy,
                            (void **) (&orig_canonicalize0_legacy), false);
    }
//    JniHook::HookJniFun(env, className, "getBooleanAttributes0", "(Ljava/lang/String;)I",
//                        (void *) new_getBooleanAttributes0,
//                        (void **) (&orig_getBooleanAttributes0), false);
    JniHook::HookJniFun(env, className, "getLastModifiedTime0", "(Ljava/io/File;)J",
                        (void *) new_getLastModifiedTime0, (void **) (&orig_getLastModifiedTime0),
                        false);
    JniHook::HookJniFun(env, className, "setPermission0", "(Ljava/io/File;IZZ)Z",
                        (void *) new_setPermission0, (void **) (&orig_setPermission0), false);
    JniHook::HookJniFun(env, className, "createFileExclusively0", "(Ljava/lang/String;)Z",
                        (void *) new_createFileExclusively0,
                        (void **) (&orig_createFileExclusively0), false);
    JniHook::HookJniFun(env, className, "list0", "(Ljava/io/File;)[Ljava/lang/String;",
                        (void *) new_list0, (void **) (&orig_list0), false);
    JniHook::HookJniFun(env, className, "createDirectory0", "(Ljava/io/File;)Z",
                        (void *) new_createDirectory0, (void **) (&orig_createDirectory0), false);
    JniHook::HookJniFun(env, className, "setLastModifiedTime0", "(Ljava/io/File;J)Z",
                        (void *) new_setLastModifiedTime0, (void **) (&orig_setLastModifiedTime0),
                        false);
    JniHook::HookJniFun(env, className, "setReadOnly0", "(Ljava/io/File;)Z",
                        (void *) new_setReadOnly0, (void **) (&orig_setReadOnly0), false);
    JniHook::HookJniFun(env, className, "getSpace0", "(Ljava/io/File;I)J",
                        (void *) new_getSpace0, (void **) (&orig_getSpace0), false);
}
