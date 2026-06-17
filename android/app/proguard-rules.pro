# SyncClipboard R8/ProGuard rules

# --- App ---
# Service / Activity / Receiver / Provider names are referenced from the manifest.
-keep class com.syncclipboard.android.service.** { *; }
-keep class com.syncclipboard.android.ui.** { *; }

# DTOs serialised via Gson reflection.
-keep class com.syncclipboard.android.util.** { *; }

# --- OkHttp / Okio ---
-dontwarn okhttp3.internal.platform.ConscryptPlatform
-dontwarn org.codehaus.mojo.animal_sniffer.IgnoreJRERequirement
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**

# --- Gson (TypeToken / generic types) ---
-keepattributes Signature
-keepattributes *Annotation*
-keep class com.google.gson.** { *; }
-keepclassmembers,allowobfuscation class * {
    @com.google.gson.annotations.SerializedName <fields>;
}

# --- Coroutines ---
-keepnames class kotlinx.coroutines.internal.MainDispatcherFactory {}
-keepnames class kotlinx.coroutines.CoroutineExceptionHandler {}
-keep class kotlinx.coroutines.android.** { *; }

# --- WorkManager ---
-keep class * extends androidx.work.Worker
-keep class * extends androidx.work.ListenableWorker {
    public <init>(...);
}
