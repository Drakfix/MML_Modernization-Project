/* gamefiles.h's importer on Android: MmlActivity (Java) shows the system's folder picker and
 * copies the player's game folder into the app's storage on a thread of its own; this polls it.
 * The bundled intro comes out of the app's assets. */
#ifdef __ANDROID__
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <windows.h>   /* (native/compat) */

#include "gamefiles.h"
#include "log.h"

/* MmlActivity's class and a static method of it (NULL if not there) */
static jclass activity_class(JNIEnv *env)
{
    jobject a = (jobject)SDL_GetAndroidActivity();
    jclass c;
    if (!a)
        return NULL;
    c = (*env)->GetObjectClass(env, a);
    (*env)->DeleteLocalRef(env, a);
    return c;
}

static void clear_exception(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

int gamefiles_can_import(void)
{
    return 1;
}

void gamefiles_import_begin(const char *dir, int jp)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? activity_class(env) : NULL;
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "importGame", "(Ljava/lang/String;Z)V") : NULL;
    char real[MAX_PATH];
    if (m) {
        jstring s = (*env)->NewStringUTF(env, compat_path(dir, real, sizeof real));
        (*env)->CallStaticVoidMethod(env, c, m, s, jp ? JNI_TRUE : JNI_FALSE);
        (*env)->DeleteLocalRef(env, s);
        log_printf("gamefiles: the folder picker (the %s version, into %s)", jp ? "Japanese" : "US", real);
    }
    clear_exception(env);
    if (c)
        (*env)->DeleteLocalRef(env, c);
}

int gamefiles_import_poll(char *status, size_t n, float *progress)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? activity_class(env) : NULL;
    jmethodID ms = c ? (*env)->GetStaticMethodID(env, c, "importStatus", "()[J") : NULL;
    jmethodID mm = c ? (*env)->GetStaticMethodID(env, c, "importMessage", "()Ljava/lang/String;") : NULL;
    jlongArray arr;
    jlong v[3] = { IMPORT_IDLE, 0, 0 };
    jstring msg;
    *progress = 0;
    if (n)
        status[0] = 0;
    if (!ms || !mm) {
        clear_exception(env);
        if (c)
            (*env)->DeleteLocalRef(env, c);
        return IMPORT_IDLE;
    }
    arr = (jlongArray)(*env)->CallStaticObjectMethod(env, c, ms);
    if (arr) {
        (*env)->GetLongArrayRegion(env, arr, 0, 3, v);
        (*env)->DeleteLocalRef(env, arr);
    }
    msg = (jstring)(*env)->CallStaticObjectMethod(env, c, mm);
    if (msg) {
        const char *s = (*env)->GetStringUTFChars(env, msg, NULL);
        if (s && n) {
            snprintf(status, n, "%s", s);
            (*env)->ReleaseStringUTFChars(env, msg, s);
        }
        (*env)->DeleteLocalRef(env, msg);
    }
    clear_exception(env);
    (*env)->DeleteLocalRef(env, c);
    if (v[2] > 0)
        *progress = (float)((double)v[1] / (double)v[2]);
    return (int)v[0];
}

/* name from the app's assets into dir (sub-folders as in name, '\\' or '/') */
int gamefiles_bundled_file(const char *name, const char *dir)
{
    char real[MAX_PATH], dest[MAX_PATH], asset[MAX_PATH], *p;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? activity_class(env) : NULL;
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "extractAsset", "(Ljava/lang/String;Ljava/lang/String;)Z") : NULL;
    int ok = 0;
    snprintf(asset, sizeof asset, "%s", strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name);
    /* (dir exists; the rest of the path may not yet: compat_path keeps it as given) */
    snprintf(dest, sizeof dest, "%s/%s", compat_path(dir, real, sizeof real), name);
    for (p = dest; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (m) {
        jstring a = (*env)->NewStringUTF(env, asset), d = (*env)->NewStringUTF(env, dest);
        ok = (*env)->CallStaticBooleanMethod(env, c, m, a, d) != JNI_FALSE;
        (*env)->DeleteLocalRef(env, a);
        (*env)->DeleteLocalRef(env, d);
    }
    clear_exception(env);
    if (c)
        (*env)->DeleteLocalRef(env, c);
    return ok;
}

int gamefiles_bundled_intro(const char *dir)
{
    char path[MAX_PATH];
    int ok;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Modernization\\intro.mpg", dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return 1;
    ok = gamefiles_bundled_file("Modernization\\intro.mpg", dir);
    log_printf("gamefiles: the bundled intro %s", ok ? "put in place" : "isn't in this build");
    return ok;
}
#else
typedef int android_files_unused;   /* elsewhere: gamefiles.c's own (no importer) */
#endif
