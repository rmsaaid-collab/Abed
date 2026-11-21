# GOAT SOLITAIRE — FINAL STRONG RELEASE

تاريخ الاعتماد: **2026-08-18**  
الحالة: **هذه هي آخر نسخة Solitaire قوية ومعتمدة.**

## Baseline

- الـExpert Solver هو نفس الإصدار الذي اختبره صاحب المشروع وحقق 9 انتصارات من أصل 10 ألعاب.
- اللعبة العاشرة كانت deal مغلقة؛ لم يتم تغيير خوارزمية اللعب في مرحلة الحماية النهائية.
- SHA-256 لـ`solitaire_auto.cpp`: `fcd20ef5a77348d4be6877f10e07a3f97efc5c5d78ad6ca9f1ecbb6c4a08f059`

## Final security profile

- Build ID: `GOAT-P3-20260818-FINAL`
- Server-signed ECDSA P-256 authorization; no server HMAC secret is embedded in the client.
- Per-install P-256 identity stored in Android Keystore.
- License bound to device, install identity, app signing certificate, build ID, and exact native binary hash.
- Short-lived signed runtime lease with automatic renewal.
- Exact license expiry is enforced while the app is open and displayed in the luxury UI timer.
- Native integrity verification, anti-debug/runtime mapping checks, RELRO/NOW, stack protector, hidden symbols, and one exported native entry point.
- The legacy `/validate` route is disabled in production; only protocol v2 is active.
- Root/Magisk devices remain supported intentionally.

## Production fingerprints

- App signing certificate SHA-256: `31c88d8f61b413991b5e9d6f6259d6bb24bb6e7e4ded08cbad1f4ce913b5f7de`
- `libengine.so` SHA-256: `0365c883a9244ac3093e8fdb66baba6dfca5762144d9ba1d4c7bc3aba9e58b6a`
- Base APK SHA-256: `bb51b80b0c086d9e692e8c8a2ece2ce98969aad55754c3f823724b69241178fa`
- ARM64 split APK SHA-256: `57858ce48370de137c4cf591c359104496acd3237a49c4aa20426e823c35d4d2`
- Final XAPK SHA-256: `1e54d4c5f69e7f3a3474472f5ffba82040c0821f9e38d3a8fc9d48d022fce429`
- Cloudflare Worker production version: `83a99f19-674c-45fc-a309-68c85d5f76d3`

## Live verification

- Installed successfully on device `R58RC08YNXR`.
- The installed `libengine.so` hash matched the production hash exactly.
- A cold process restart obtained a fresh protected session successfully.
- License UI restored automatically and displayed the live expiry countdown.
- No fatal, JNI, or security errors were observed after the final restart.

Do not modify the Solitaire solver in this snapshot. Make future experiments in a separate copy.
