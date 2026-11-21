package top.niunaijun.blackbox.core;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Environment;
import android.os.Process;
import android.text.TextUtils;

import java.io.File;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.app.BActivityThread;
import top.niunaijun.blackbox.core.env.BEnvironment;
import top.niunaijun.blackbox.utils.FileUtils;
import top.niunaijun.blackbox.utils.TrieTree;

/**
 * Created by Milk on 4/9/21.
 * * ∧＿∧
 * (`･ω･∥
 * 丶　つ０
 * しーＪ
 * 此处无Bug
 */
@SuppressLint("SdCardPath")
public class IOCore {
    public static final String TAG = "IOCore";

    private static final IOCore sIOCore = new IOCore();
    private static final TrieTree sBlackTree = new TrieTree();
    private final Map<String, String> mRedirectMap = new LinkedHashMap<>();

    private static final Map<String, Map<String, String>> sCachePackageRedirect = new HashMap<>();

    public static IOCore get() {
        return sIOCore;
    }

    // /data/data/com.google/  ----->  /data/data/com.virtual/data/com.google/
    public void addRedirect(String origPath, String redirectPath) {
        if (TextUtils.isEmpty(origPath) || TextUtils.isEmpty(redirectPath) || mRedirectMap.get(origPath) != null)
            return;
        // Publish the destination before creating it.  File.mkdirs() invokes
        // canonicalization internally, so every redirect must already be
        // idempotent if Java I/O observes this rule.
        mRedirectMap.put(origPath, redirectPath);
        File redirectFile = new File(redirectPath);
        if (!redirectFile.exists()) {
            FileUtils.mkdirs(redirectPath);
        }
        NativeCore.addIORule(origPath, redirectPath);
    }

    public void addBlackRedirect(String path) {
        if (TextUtils.isEmpty(path))
            return;
        sBlackTree.add(path);
    }

    public String redirectPath(String path) {
        if (TextUtils.isEmpty(path))
            return path;

        // Redirect destinations are terminal.  Also protect the portion of a
        // destination between a source root and the destination itself.  That
        // second case is essential for File.mkdirs(): while walking parents,
        // /storage/emulated/0/AkatskiGamespace must not be redirected back
        // below /storage/emulated/0/AkatskiGamespace again.
        if (isRedirectDestinationOrProtectedAncestor(path, mRedirectMap))
            return path;

        String search = sBlackTree.search(path);
        if (!TextUtils.isEmpty(search))
            return search;

        // Select the most-specific rule.  TrieTree.search() returns the first
        // terminal prefix, which made /data/.../app swallow the more precise
        // /data/.../app/lib rule.  Prefix splicing also avoids String.replace
        // rewriting matching text that appears later inside a destination.
        return applyLongestPrefixRule(path, mRedirectMap);
    }

    public File redirectPath(File path) {
        if (path == null)
            return null;
        String pathStr = path.getAbsolutePath();
        return new File(redirectPath(pathStr));
    }

    public String redirectPath(String path, Map<String, String> rule) {
        if (TextUtils.isEmpty(path))
            return path;
        if (rule == null || rule.isEmpty())
            return path;
        if (isRedirectDestinationOrProtectedAncestor(path, rule))
            return path;
        return applyLongestPrefixRule(path, rule);
    }

    /** True when {@code path} is {@code root} or one of its descendants. */
    private static boolean isAtOrBelow(String path, String root) {
        if (TextUtils.isEmpty(path) || TextUtils.isEmpty(root))
            return false;
        if (path.equals(root))
            return true;
        if (!path.startsWith(root))
            return false;
        if (root.endsWith("/"))
            return true;
        return path.length() > root.length() && path.charAt(root.length()) == '/';
    }

    /**
     * Prevents destination re-entry and the parent-walk cycle used by
     * java.io.File.mkdirs().  The source root itself remains redirectable;
     * only a strict descendant that is also an ancestor of its destination is
     * treated as part of the protected destination envelope.
     */
    private static boolean isRedirectDestinationOrProtectedAncestor(
            String path, Map<String, String> rules) {
        for (Map.Entry<String, String> entry : rules.entrySet()) {
            String source = entry.getKey();
            String destination = entry.getValue();
            if (TextUtils.isEmpty(source) || TextUtils.isEmpty(destination))
                continue;

            if (isAtOrBelow(path, destination))
                return true;

            boolean strictSourceDescendant = !path.equals(source) && isAtOrBelow(path, source);
            boolean destinationIsBelowPath = isAtOrBelow(destination, path);
            if (strictSourceDescendant && destinationIsBelowPath)
                return true;
        }
        return false;
    }

    /** Applies exactly one longest, path-segment-aligned source rule. */
    private static String applyLongestPrefixRule(String path, Map<String, String> rules) {
        String bestSource = null;
        String bestDestination = null;
        for (Map.Entry<String, String> entry : rules.entrySet()) {
            String source = entry.getKey();
            String destination = entry.getValue();
            if (TextUtils.isEmpty(source) || TextUtils.isEmpty(destination))
                continue;
            if (!isAtOrBelow(path, source))
                continue;
            if (bestSource == null || source.length() > bestSource.length()) {
                bestSource = source;
                bestDestination = destination;
            }
        }
        if (bestSource == null)
            return path;
        return bestDestination + path.substring(bestSource.length());
    }

    public File redirectPath(File path, Map<String, String> rule) {
        if (path == null)
            return null;
        String pathStr = path.getAbsolutePath();
        return new File(redirectPath(pathStr, rule));
    }

    // 由于正常情况Application已完成重定向，以下重定向是怕代码写死。
    public void enableRedirect(Context context) {
        Map<String, String> rule = new LinkedHashMap<>();
        Set<String> blackRule = new HashSet<>();
        String packageName = context.getPackageName();

        try {
            // Use the already configured guest ApplicationInfo. Lifecycle
            // callbacks may intentionally select a prepared nativeLibraryDir
            // before this method runs; querying BPackageManager again would
            // discard that process-local namespace configuration.
            ApplicationInfo packageInfo = context.getApplicationInfo();
            int systemUserId = BlackBoxCore.getHostUserId();
            rule.put(String.format("/data/data/%s/lib", packageName), packageInfo.nativeLibraryDir);
            rule.put(String.format("/data/user/%d/%s/lib", systemUserId, packageName), packageInfo.nativeLibraryDir);

            rule.put(String.format("/data/data/%s", packageName), packageInfo.dataDir);
            rule.put(String.format("/data/user/%d/%s", systemUserId, packageName), packageInfo.dataDir);

            if (BlackBoxCore.getContext().getExternalCacheDir() != null && context.getExternalCacheDir() != null) {
                File external = BEnvironment.getExternalUserDir(BActivityThread.getUserId());

                // sdcard
                rule.put("/sdcard", external.getAbsolutePath());
                rule.put(String.format("/storage/emulated/%d", systemUserId), external.getAbsolutePath());

                blackRule.add("/sdcard/Pictures");
                blackRule.add(String.format("/storage/emulated/%d/Pictures", systemUserId));
            }
            if (BlackBoxCore.get().isHideRoot()) {
                hideRoot(rule);
            }
            proc(rule);
        } catch (Exception e) {
            e.printStackTrace();
        }
        for (String key : rule.keySet()) {
            get().addRedirect(key, rule.get(key));
        }
        for (String s : blackRule) {
            get().addBlackRedirect(s);
        }
        NativeCore.enableIO();
    }

    private void hideRoot(Map<String, String> rule) {
        rule.put("/system/app/Superuser.apk", "/system/app/Superuser.apk-fake");
        rule.put("/sbin/su", "/sbin/su-fake");
        rule.put("/system/bin/su", "/system/bin/su-fake");
        rule.put("/system/xbin/su", "/system/xbin/su-fake");
        rule.put("/data/local/xbin/su", "/data/local/xbin/su-fake");
        rule.put("/data/local/bin/su", "/data/local/bin/su-fake");
        rule.put("/system/sd/xbin/su", "/system/sd/xbin/su-fake");
        rule.put("/system/bin/failsafe/su", "/system/bin/failsafe/su-fake");
        rule.put("/data/local/su", "/data/local/su-fake");
        rule.put("/su/bin/su", "/su/bin/su-fake");
    }

    private void proc(Map<String, String> rule) {
        int appPid = BActivityThread.getAppPid();
        int pid = Process.myPid();
        String selfProc = "/proc/self/";
        String proc = "/proc/" + pid + "/";

        String cmdline = new File(BEnvironment.getProcDir(appPid), "cmdline").getAbsolutePath();
        rule.put(proc + "cmdline", cmdline);
        rule.put(selfProc + "cmdline", cmdline);
    }
}
