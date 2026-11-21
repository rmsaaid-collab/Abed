# Jawaker Engine ProGuard Rules

# ─── General ──────────────────────────────────────────────────────────
-keepattributes Signature
-keepattributes *Annotation*
-keepattributes InnerClasses
-keepattributes EnclosingMethod
-renamesourcefileattribute GOAT

# ─── BlackBox Core ────────────────────────────────────────────────────
# Keep all BlackBox classes — the virtual engine uses reflection heavily
-keep class top.niunaijun.blackbox.** { *; }
-keep class top.niunaijun.blackbox.app.** { *; }
-keep class top.niunaijun.blackbox.core.** { *; }

# BlackReflection generates BR* accessors whose class/member names are the
# runtime lookup contract. Optimizing or renaming them makes successful
# lookups return null and crashes BlackBoxCore during static initialization.
-keep class black.** { *; }
-keep interface black.** { *; }

# These framework-compatibility layers are reached through reflection/JNI.
# Keep only those runtime packages stable; GOAT's own license/security classes
# remain eligible for shrinking, optimization, and obfuscation.
-keep class top.niunaijun.jnihook.** { *; }
-keep class top.canyie.pine.** { *; }
-keep class top.canyie.dreamland.** { *; }
-keep class de.robv.android.xposed.** { *; }
-keep class external.org.apache.commons.** { *; }
-keep class xposed.dummy.** { *; }
-keep class android.** { *; }
-keep class com.android.** { *; }

# Keep native methods (JNI bridge)
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep @Keep annotated classes and methods
-keep @androidx.annotation.Keep class * { *; }
-keepclassmembers class * {
    @androidx.annotation.Keep *;
}

# ─── Application Entry Points ─────────────────────────────────────────
# The Android Gradle plugin generates precise manifest keep rules for the
# three entry-point constructors. Do not pin every application member here:
# allowing R8 to rename and optimize the internal loader/license graph is a
# material part of the release hardening.

# ─── AppLifecycleCallback ────────────────────────────────────────────
# Keep all lifecycle callback classes and their methods
-keep class * extends top.niunaijun.blackbox.app.AppLifecycleCallback { *; }
-keep class * extends top.niunaijun.blackbox.app.config.ClientConfiguration { *; }

# ─── Serialization ────────────────────────────────────────────────────
-keepclassmembers class * implements android.os.Parcelable {
    public static final android.os.Parcelable$Creator CREATOR;
}

# ─── Kotlin Metadata ──────────────────────────────────────────────────
-keep class kotlin.Metadata { *; }
-dontwarn kotlin.**

# ─── Suppress warnings ────────────────────────────────────────────────
-dontwarn top.niunaijun.blackbox.**
-dontwarn javax.annotation.**
-dontwarn org.jetbrains.annotations.**

# Remove nonessential release telemetry. Warning/error logs remain available
# for real field diagnostics without exposing the normal loading sequence.
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
}
