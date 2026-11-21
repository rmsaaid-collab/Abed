#!/usr/bin/env python3
"""
patch_and_build_xapk.py — GoldenLib patcher (NoLiMiT setup)
Uso:
  python patch_and_build_xapk.py
"""
import sys, os, re, shutil, subprocess, zipfile, json, getpass
from pathlib import Path

# ─── Paths (NoLiMiT setup) ───────────────────────────────────────────────────
GOLDEN_DIR   = Path(__file__).resolve().parent
ORIGINAL_DIR = GOLDEN_DIR / "original"
TOOLS_DIR    = GOLDEN_DIR.parent / "GoldenTools"
JAVA_EXE     = Path(os.environ.get("JAVA_EXE", r"C:\Program Files\Android\Android Studio\jbr\bin\java.exe"))
APKTOOL_JAR  = TOOLS_DIR / "apktool.jar"
SIGNER_JAR   = TOOLS_DIR / "uber-apk-signer.jar"
KEYSTORE     = GOLDEN_DIR / "goat_solitaire_2026.jks"
KEY_ALIAS    = "goat-solitaire"
_KEYSTORE_PASS = None

# Libs compilados
LIBENGINE    = GOLDEN_DIR / "build" / "libengine.so"
LIBDOBBY     = GOLDEN_DIR / "libs" / "arm64-v8a" / "libdobby.so"

# Smali patches
SMALI_DIR    = GOLDEN_DIR / "smali_patches"
OVERLAY_SMALI = SMALI_DIR / "TrainerOverlayView.smali"
GOAT_PLATE    = SMALI_DIR / "goat_plate.png"

# APKs originais
BASE_APK     = ORIGINAL_DIR / "base.apk"
SPLIT_APK    = ORIGINAL_DIR / "split_config.arm64_v8a.apk"

# Output
OUT_XAPK     = GOLDEN_DIR / "jawaker_golden.xapk"
WORK_DIR     = GOLDEN_DIR / "_work"

ACTIVITY_CLASS = "com/google/firebase/MessagingUnityPlayerActivity"
PACKAGE_NAME   = "com.boundless.jawaker"

# ─── Helpers ──────────────────────────────────────────────────────────────────
def run(cmd, cwd=None, check=True):
    print(f"\n> {' '.join(str(c) for c in cmd)}")
    r = subprocess.run(cmd, text=True, capture_output=True, cwd=cwd)
    if r.stdout: print(r.stdout[-3000:])
    if r.stderr and r.returncode != 0: print(r.stderr[-1000:])
    if check and r.returncode != 0:
        print(f"FAILED (code {r.returncode})")
        sys.exit(1)
    return r

def apktool(*args):
    return run([str(JAVA_EXE), "-jar", str(APKTOOL_JAR)] + list(args))

def signing_password():
    global _KEYSTORE_PASS
    if _KEYSTORE_PASS is None:
        _KEYSTORE_PASS = os.environ.get("GOAT_KEYSTORE_PASS") or getpass.getpass(
            "GOAT release keystore password: "
        )
    return _KEYSTORE_PASS

def sign_apk(apk_path):
    password = signing_password()
    run([str(JAVA_EXE), "-jar", str(SIGNER_JAR),
         "--apks", str(apk_path), "--allowResign", "--overwrite",
         "--ks", str(KEYSTORE), "--ksAlias", KEY_ALIAS,
         "--ksPass", password, "--ksKeyPass", password], check=False)
    # uber-apk-signer renames output
    for pattern in ["*aligned-signed*", "*debugSigned*", "*signed*"]:
        for f in apk_path.parent.glob(pattern):
            if f.suffix == ".apk": return f
    return apk_path

def find_smali_file(decompiled, class_path):
    for d in sorted(decompiled.glob("smali*/")):
        c = d / (class_path + ".smali")
        if c.exists(): return c
    return None

def strip_unwanted_endpoints(decompiled):
    targets = [
        "api16-access-ttp.tiktokpangle-b.us",
        "api16-access-ttp-b.tiktokpangle-b.us",
        "api16-access-ttp.tiktokpangle.us",
        "api16-access-ttp-b.tiktokpangle.us",
    ]
    total = 0
    for smali in decompiled.glob("smali*/**/*.smali"):
        try:
            text = smali.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            text = smali.read_text(encoding="utf-8", errors="ignore")
        changed = text
        for target in targets:
            total += changed.count(target)
            changed = changed.replace(target, "")
        if changed != text:
            smali.write_text(changed, encoding="utf-8")
    print(f"  Removed Pangle access endpoints: {total}")

# ─── Verificações iniciais ────────────────────────────────────────────────────
def check_prereqs():
    errors = []
    if not JAVA_EXE.exists():   errors.append(f"Java not found: {JAVA_EXE}")
    if not APKTOOL_JAR.exists():errors.append(f"apktool.jar not found: {APKTOOL_JAR}")
    if not SIGNER_JAR.exists(): errors.append(f"uber-apk-signer.jar not found: {SIGNER_JAR}")
    if not KEYSTORE.exists():   errors.append(f"release keystore not found: {KEYSTORE}")
    if not LIBENGINE.exists():  errors.append(f"libengine.so not found: {LIBENGINE}")
    if not LIBDOBBY.exists():   errors.append(f"libdobby.so not found: {LIBDOBBY}")
    if not BASE_APK.exists():   errors.append(f"base.apk not found: {BASE_APK}")
    if not SPLIT_APK.exists():  errors.append(f"split APK not found: {SPLIT_APK}")
    if not OVERLAY_SMALI.exists(): errors.append(f"TrainerOverlayView.smali not found: {OVERLAY_SMALI}")
    if errors:
        for e in errors: print(f"  ERROR: {e}")
        sys.exit(1)
    print("  All prerequisites OK")

# ─── Patch base APK (smali only) ─────────────────────────────────────────────
def patch_base():
    print("\n[2] Decompiling base APK...")
    decompiled = WORK_DIR / "_base_dec"
    if not (os.environ.get("REUSE_DECOMPILED") == "1" and decompiled.exists()):
        if decompiled.exists(): shutil.rmtree(decompiled)
        apktool("d", str(BASE_APK), "-o", str(decompiled), "--no-res")
    else:
        print("  Reusing completed decompilation")

    print("\n[3] Patching smali...")
    strip_unwanted_endpoints(decompiled)
    activity = find_smali_file(decompiled, ACTIVITY_CLASS)
    if not activity:
        candidates = list(decompiled.glob("**/MessagingUnityPlayerActivity.smali"))
        activity = candidates[0] if candidates else None
    if not activity:
        print("  ERROR: MessagingUnityPlayerActivity.smali not found")
        for a in list(decompiled.glob("**/*Activity*.smali"))[:10]:
            print(f"    found: {a.relative_to(decompiled)}")
        sys.exit(1)

    # Patch onCreate
    text = activity.read_text(encoding="utf-8")
    if 'loadLibrary' in text and '"engine"' in text:
        print("  Already patched")
    else:
        new_oncreate = '''.method protected onCreate(Landroid/os/Bundle;)V
    .registers 6

    const-string v0, "dobby"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V

    const-string v0, "engine"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V

    invoke-super {p0, p1}, Lcom/unity3d/player/UnityPlayerActivity;->onCreate(Landroid/os/Bundle;)V

    new-instance v0, Lcom/google/firebase/TrainerOverlayView;
    invoke-direct {v0, p0}, Lcom/google/firebase/TrainerOverlayView;-><init>(Landroid/content/Context;)V

    new-instance v1, Landroid/widget/FrameLayout$LayoutParams;
    const/4 v2, -0x1
    const/4 v3, -0x1
    invoke-direct {v1, v2, v3}, Landroid/widget/FrameLayout$LayoutParams;-><init>(II)V

    invoke-virtual {p0, v0, v1}, Landroid/app/Activity;->addContentView(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V

    invoke-static {p0, v0}, Lcom/google/firebase/TrainerOverlayView;->nativeCreatePanel(Landroid/app/Activity;Lcom/google/firebase/TrainerOverlayView;)V

    return-void
.end method'''

        pattern = re.compile(
            r'\.method protected onCreate\(Landroid/os/Bundle;\)V.*?\.end method',
            re.DOTALL)
        if pattern.search(text):
            text = pattern.sub(new_oncreate, text, count=1)
            print("  onCreate replaced")
        else:
            text = text.replace(".end class", new_oncreate + "\n\n.end class", 1)
            print("  onCreate inserted")

        activity.write_text(text, encoding="utf-8")

    # Copy TrainerOverlayView.smali to same directory
    dst = activity.parent / "TrainerOverlayView.smali"
    shutil.copy2(OVERLAY_SMALI, dst)
    print(f"  TrainerOverlayView.smali copied")

    if GOAT_PLATE.exists():
        assets_dir = decompiled / "assets"
        assets_dir.mkdir(exist_ok=True)
        shutil.copy2(GOAT_PLATE, assets_dir / "goat_plate.png")
        print("  goat_plate.png asset copied")

    print("\n[4] Rebuilding base APK...")
    rebuilt = WORK_DIR / "base_rebuilt.apk"
    rebuilt.unlink(missing_ok=True)
    apktool("b", str(decompiled), "-o", str(rebuilt))

    print("\n[5] Signing base APK...")
    signed = sign_apk(rebuilt)
    print(f"  OK: {signed.name}")
    return signed

# ─── Patch split APK (add libs) ──────────────────────────────────────────────
def patch_split():
    print("\n[6] Patching split APK (adding libs)...")
    patched = WORK_DIR / "split_patched.apk"

    with zipfile.ZipFile(SPLIT_APK, "r") as zin, \
         zipfile.ZipFile(patched, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            zout.writestr(item, zin.read(item.filename))
        # Add our libs
        zout.write(LIBDOBBY,  "lib/arm64-v8a/libdobby.so")
        zout.write(LIBENGINE, "lib/arm64-v8a/libengine.so")
        print(f"  + libdobby.so  ({LIBDOBBY.stat().st_size//1024}KB)")
        print(f"  + libengine.so ({LIBENGINE.stat().st_size//1024}KB)")

    print("\n[7] Signing split APK...")
    signed = sign_apk(patched)
    print(f"  OK: {signed.name}")
    return signed

# ─── Ler version do AndroidManifest ──────────────────────────────────────────
def get_version_info():
    dec = WORK_DIR / "_base_dec"
    manifest = dec / "AndroidManifest.xml"
    if manifest.exists():
        content = manifest.read_text(encoding="utf-8", errors="ignore")
        vcode = re.search(r'android:versionCode="([^"]+)"', content)
        vname = re.search(r'android:versionName="([^"]+)"', content)
        if vcode and vname:
            return int(vcode.group(1)), vname.group(1)
    return 1, "1.0"

# ─── Montar XAPK ─────────────────────────────────────────────────────────────
def build_xapk(signed_base, signed_split):
    print("\n[8] Building XAPK...")
    vcode, vname = get_version_info()

    manifest = {
        "xapk_version": 2,
        "package_name": PACKAGE_NAME,
        "name": "Jawaker",
        "version_code": str(vcode),
        "version_name": vname,
        "min_sdk_version": "23",
        "target_sdk_version": "35",
        "split_configs": ["config.arm64_v8a"],
        "split_apks": [
            {"file": f"{PACKAGE_NAME}.apk", "id": "base"},
            {"file": "config.arm64_v8a.apk", "id": "config.arm64_v8a"},
        ],
    }

    OUT_XAPK.unlink(missing_ok=True)
    with zipfile.ZipFile(OUT_XAPK, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
        z.write(signed_base,  f"{PACKAGE_NAME}.apk")
        z.write(signed_split, "config.arm64_v8a.apk")

    mb = OUT_XAPK.stat().st_size // 1024 // 1024
    print(f"\n  DONE: {OUT_XAPK.name} ({mb}MB)")
    print(f"\n  Para instalar:")
    print(f"    adb push {OUT_XAPK.name} /sdcard/Download/")
    print(f"    (usar SAI ou APKMirror Installer no device)")

# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("  GoldenLib XAPK Patcher")
    print("=" * 60)

    print("\n[1] Checking prerequisites...")
    check_prereqs()

    WORK_DIR.mkdir(exist_ok=True)

    signed_base = patch_base()
    signed_split = patch_split()
    build_xapk(signed_base, signed_split)

    shutil.rmtree(WORK_DIR, ignore_errors=True)
    print("\nClean up done.")

if __name__ == "__main__":
    main()
