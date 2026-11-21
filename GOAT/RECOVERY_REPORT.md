# V6.1 Phone Fast — exact native-source recovery

Recovered on 2026-09-06. No new gameplay changes were made.

## Proven result

The restored ENGINE was compiled from source with Android NDK 25.1.8937393, Clang 14.0.6, arm64-v8a, Android API 21, Release. After the original `llvm-strip --strip-unneeded` step, the resulting complete `libengine.so` has exactly the same SHA-256 and length as the original V6.1 library embedded in the user's installed APK.

- Native SHA-256: `732934d77189886c63fd93affd69ee1645159bb7eae481835e39cb816b0d48a7`
- Native bytes: `618984`
- Original APK SHA-256: `1422270335f8a824adc10c1f62c3fe2a824b70eab432859ebc966be32f81b038`
- Restored `ENGINE/src/main/cpp/solitaire_auto.cpp` SHA-256: `b796038b003b79d8bb4842d6dd187a1d7ea5ee3b3dae52e059641bf9bdc340c0`
- Original preserved binary: `C:/Users/laith/Downloads/src Project V28.2.90/SOLITAIRE_GOAT_ENGINE_V6_FIX_20260830/BUILD_ARTIFACTS/libengine-v61-stripped.so`
- Original binary timestamp: 2026-08-30 15:36:11 Asia/Amman.

This is a reproducible native-binary match, not a decompilation or a guessed rollback. It proves native executable identity; it does not establish that the historical version has no latent gameplay bugs.

## Recovery provenance

The historical session is `C:/Users/laith/.codex/sessions/2026/08/11/rollout-2026-08-11T18-43-34-019ff17e-4f29-7701-8c19-6a4a51b9064d.jsonl`. Relevant events occurred on August 30, despite the session filename.

1. The 07:11:48 UTC event copied the original `SOLITAIRE_PRO_EXPERT_WORK_20260829/BASE_FAST/GOAT_SOLITAIRE_V4_FAST_3_WINS_READY_SOURCE_2026-08-26` project, then replaced ONLY `solitaire_auto.cpp` with `GOAT_MULTI_GAME_WORK/_backups/2026-08-27_before_stall_recovery_v7/solitaire_auto.cpp`.
2. That initial Solitaire file still exists and matches its logged SHA-256: `12a31ac7829102b7656239a089dd8f8583fae14fed75f5892415b3271cc25f3a`.
3. Four successful historical patches were replayed, in order: 07:19:53, 11:43:39, 12:29:53 and 12:32:36 UTC. Their original text and call IDs are saved in RECOVERY_EVIDENCE. Failed historical patch attempts were excluded.
4. The cutoff is the V6.1 native build at 12:36:11 UTC / 15:36:11 local. V6.2 and every later patch were excluded.
5. LOADER was copied from the historical base and its historical expected native fingerprint restored to the V6.1 value. The rebuilt matching native library was placed in its arm64 jniLibs directory.

## Contents and boundaries

- `ENGINE/`: restored native source and dependencies.
- `LOADER/`: historical loader source with the V6.1 native pin and library.
- `VERIFY_NATIVE.ps1`: local rebuild + exact SHA-256 verification; no signing, install, phone access or server access.
- `RECOVERY_EVIDENCE/`: historical patches, for evidence only. Their original paths must NOT be applied to the live old project.
- No SERVER folder is included: the live server has later authorized fingerprints that must remain intact. Do not roll back server settings to August 30.
- No APK was installed, no app data was cleared, and no live fingerprint was changed during recovery.
- Older copied README/release notes inside components describe their own historical baselines; this report defines the recovered version.

Keep this recovery directory as an untouched baseline. Future enhancements should use a separate copy.

## Loader rebuild validation

`:app:assembleDeviceRelease --offline --no-daemon` succeeded using JDK 11 and the original Gradle wrapper. The resulting APK is unsigned and was NOT installed.

Compared uncompressed entries with the original signed phone APK, excluding signing metadata:

- 779 original entries match byte-for-byte, including `classes.dex`, `AndroidManifest.xml`, `resources.arsc`, and `lib/arm64-v8a/libengine.so`.
- `assets/goat_bridge.jar`: inner `classes.dex` matches; its generated manifest/container differs.
- `libABridge.so`: `.text`, `.rodata` and `.dynsym` match; build ID differs.
- `libpine.so`: rebuilt dependency differs, including `.text`. Exact reproduction of that auxiliary dependency has NOT been established.

Therefore the complete unsigned APK is NOT claimed byte-identical. The full native gameplay engine and loader DEX ARE proven exact. Use the original signed V6.1 APK for unchanged phone operation; do not install this validation build. No speculative dependency adjustments were made to force a match.
