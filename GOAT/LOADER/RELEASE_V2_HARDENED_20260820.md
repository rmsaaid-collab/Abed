# GOAT Jawaker Loader VVIP v2 — Hardened Release

Release date: 2026-08-20

## Artifacts

- Current: `GOAT_JAWAKER_LOADER_VVIP_V2_HARDENED.apk`
  - SHA-256: `E6DFA3C99AC957035BF679027ECF6198AF611E4D44C993812D53B5732BBCD655`
  - Signing certificate SHA-256: `69f33c009629337b0ec4178016c86bfdc0084100386565efe308369e1ea1bfba`
- Temporary legacy fallback: `GOAT_JAWAKER_LOADER_VVIP_V1.apk`
  - SHA-256: `CE3E52FFEFF8ADECCB2B9F41B83DC4FA1D122A31C6A8AFB7AE0C3C67BBF7B064`
  - Uses the same signing certificate and protected engine binary.

## Compatibility pins

- Build ID: `GOAT-P3-20260818-FINAL`
- Protected `libengine.so` SHA-256: `bba69aa500fd045c3b3d4043bd0045f6bc05b854f1a9a15ea840f0e5979a4a8d`
- Validation Worker version: `897d06dc-cc90-4fda-a041-d98392d80f2b`

The Worker allow-list still contains the shared protected engine hash, so V1
remains compatible temporarily. No V1 artifact or signature was changed.

## Hardened v2 behavior

- Signed online license session is required before launch.
- License is bound to the device and Android Keystore install identity.
- Server responses are verified with the pinned ECDSA public key.
- Loader signer, package, release flags, and embedded engine hash are verified.
- Saved license data is encrypted with AES-GCM in Android Keystore.
- License expiry is displayed as a live countdown and checked online on reopen
  and immediately before every engine launch.
- Release code is shrunk/optimized/obfuscated while reflection-dependent
  BlackBox runtime packages retain their required names.

## Verification

- APK Signature Schemes v1, v2, and v3 verified.
- Cold launch and BlackBox services verified on device `R58RC08YNXR`.
- Signed activation verified as `VVIP ACTIVE`.
- Encrypted restore plus online revalidation verified after a cold reopen.
- `LAUNCH ENGINE` verified enabled only after activation.
- Virtual Jawaker activity launched successfully without a fatal exception.

