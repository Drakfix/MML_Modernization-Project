/* updater.h on Android: MmlActivity (Java) does the work, this calls it and reads its status the
 * way android_files.c does for the game-files import. */
#include "updater.h"

#ifdef __ANDROID__
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include "log.h"

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

/* a static String method of MmlActivity into out */
static void get_string(JNIEnv *env, jclass c, const char *name, char *out, size_t n)
{
    jmethodID m = (*env)->GetStaticMethodID(env, c, name, "()Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallStaticObjectMethod(env, c, m) : NULL;
    if (n)
        out[0] = 0;
    if (s) {
        const char *u = (*env)->GetStringUTFChars(env, s, NULL);
        if (u && n) {
            snprintf(out, n, "%s", u);
            (*env)->ReleaseStringUTFChars(env, s, u);
        }
        (*env)->DeleteLocalRef(env, s);
    }
    clear_exception(env);
}

/* a static void method of MmlActivity */
static void call_void(const char *name)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? activity_class(env) : NULL;
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, name, "()V") : NULL;
    if (m)
        (*env)->CallStaticVoidMethod(env, c, m);
    clear_exception(env);
    if (c)
        (*env)->DeleteLocalRef(env, c);
}

int update_can(void)
{
    return 1;
}

const char *update_current(void)
{
    static char ver[48];
    JNIEnv *env;
    jclass c;
    if (ver[0])
        return ver;
    env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    c = env ? activity_class(env) : NULL;
    if (c) {
        get_string(env, c, "updateCurrent", ver, sizeof ver);
        (*env)->DeleteLocalRef(env, c);
    }
    return ver;
}

void update_check_begin(void)
{
    log_printf("update: looking for a new version (this is %s)", update_current());
    call_void("updateCheck");
}

void update_install_begin(void)
{
    log_printf("update: install");
    call_void("updateInstall");
}

int update_poll(char *msg, size_t n, char *ver, size_t vn, float *progress)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? activity_class(env) : NULL;
    jmethodID ms = c ? (*env)->GetStaticMethodID(env, c, "updateStatus", "()[J") : NULL;
    jlong v[3] = { UPD_IDLE, 0, 0 };
    jlongArray arr;
    *progress = 0;
    if (n)
        msg[0] = 0;
    if (vn)
        ver[0] = 0;
    if (!ms) {
        clear_exception(env);
        if (c)
            (*env)->DeleteLocalRef(env, c);
        return UPD_IDLE;
    }
    arr = (jlongArray)(*env)->CallStaticObjectMethod(env, c, ms);
    if (arr) {
        (*env)->GetLongArrayRegion(env, arr, 0, 3, v);
        (*env)->DeleteLocalRef(env, arr);
    }
    get_string(env, c, "updateMessage", msg, n);
    get_string(env, c, "updateVersion", ver, vn);
    clear_exception(env);
    (*env)->DeleteLocalRef(env, c);
    if (v[2] > 0)
        *progress = (float)((double)v[1] / (double)v[2]);
    return (int)v[0];
}
#elif !defined(_WIN32) && !defined(__linux__)   /* (Windows and Linux: updater_net.c) */
int update_can(void)
{
    return 0;
}

const char *update_current(void)
{
    return "";
}

void update_check_begin(void)
{
}

void update_install_begin(void)
{
}

int update_poll(char *msg, size_t n, char *ver, size_t vn, float *progress)
{
    if (n)
        msg[0] = 0;
    if (vn)
        ver[0] = 0;
    *progress = 0;
    return UPD_IDLE;
}
#endif
