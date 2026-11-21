# GOAT Engine — no-root Jawaker container

This project runs the official `com.boundless.jawaker` package inside the
BlackBox/BCore virtual environment and loads the GOAT native engine without
patching or resigning the game APK.

## Supported game build

- Package: `com.boundless.jawaker`
- Version name: `28.2.90`
- Version code: `28290`
- ABI: `arm64-v8a`

The loader intentionally blocks every other Jawaker version because the
native engine uses RVAs verified against this exact IL2CPP binary.

## Runtime pipeline

1. `MainActivity` verifies that the exact official build is installed.
2. BlackBox installs the system package for virtual user `0`.
3. `EngineLibManager` reads the base APK and every split APK, then extracts
   all original `lib/arm64-v8a/*.so` files unchanged into the virtual native
   directory.
4. Only `libdobby.so` and `libengine.so` are added beside those originals.
5. Before the guest class loader/native namespace is created, BCore selects
   that prepared native directory and exposes the loader dex as a shared path.
6. `libdobby.so` and `libengine.so` are loaded with an owner class from the
   guest class loader, keeping them in Jawaker's native namespace.
7. The host-provided `TrainerOverlayView` is attached after the original Unity
   Activity finishes `onCreate`.

There is no `libil2cpp.so` replacement and no IL2CPP file redirect.

## Important source files

- `app/src/main/java/com/jawaker/engine/EngineLibManager.kt`
- `app/src/main/java/com/jawaker/engine/MainApplication.kt`
- `app/src/main/java/com/jawaker/engine/OverlayController.kt`
- `app/src/main/java/com/google/firebase/TrainerOverlayView.java`
- `Bcore/src/main/java/top/niunaijun/blackbox/app/BActivityThread.java`
- `Bcore/src/main/java/top/niunaijun/blackbox/core/IOCore.java`

## Packaged native files

`app/src/main/jniLibs/arm64-v8a/` must contain only:

- `libdobby.so`
- `libengine.so`

Old container binaries are retained outside the APK in
`legacy_native_unused/` for reference only.

## Signing and license compatibility

The loader must be signed by the GOAT release certificate whose SHA-256 is:

`31c88d8f61b413991b5e9d6f6259d6bb24bb6e7e4ded08cbad1f4ce913b5f7de`

The official Jawaker certificate remains untouched. Native license security
verifies the signed loader context supplied through `TrainerOverlayView`.
After rebuilding `libengine.so`, its new SHA-256 must be approved by the
license server before testing.

## Build note

Do not build until the native engine has been rebuilt from the matching
`GOAT_MULTI_GAME_WORK` source and copied into the app's `jniLibs` directory.
