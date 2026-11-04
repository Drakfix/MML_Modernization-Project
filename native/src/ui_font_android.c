/* ui_font.h on Android: the platform's own text drawing, through Java (MmlActivity.rasterizeFont
 * draws both fonts into our atlas memory and returns the metrics, laid out as ui_font_gdi.c does
 * on Windows). Called from our threads, which SDL attaches to Java as needed. */
#ifdef __ANDROID__
#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include "log.h"
#include "ui_font.h"

#define CHARS (UI_LAST_CHAR - UI_FIRST_CHAR + 1)
#define PER_FONT (1 + CHARS * 6)

static int failed(JNIEnv *env, const char *what)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);   /* (to logcat) */
        (*env)->ExceptionClear(env);
    }
    log_printf("ui: font through Java failed (%s)", what);
    return 0;
}

int ui_font_rasterize(int px, uint8_t *cov, int aw, int ah, ui_glyph glyph[UI_FONTS][UI_LAST_CHAR + 1],
                      float line[UI_FONTS])
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity, buf;
    jclass cls;
    jmethodID mid;
    jfloatArray res;
    float m[UI_FONTS * PER_FONT];
    int f, c;
    if (!env)
        return 0;
    activity = (jobject)SDL_GetAndroidActivity();
    if (!activity)
        return failed(env, "no activity");
    cls = (*env)->GetObjectClass(env, activity);
    (*env)->DeleteLocalRef(env, activity);
    mid = (*env)->GetStaticMethodID(env, cls, "rasterizeFont", "(IIILjava/nio/ByteBuffer;)[F");
    if (!mid) {
        (*env)->DeleteLocalRef(env, cls);
        return failed(env, "no rasterizeFont");
    }
    memset(cov, 0, (size_t)aw * ah);
    buf = (*env)->NewDirectByteBuffer(env, cov, (jlong)aw * ah);
    res = buf ? (jfloatArray)(*env)->CallStaticObjectMethod(env, cls, mid, (jint)px, (jint)aw, (jint)ah, buf) : NULL;
    if (buf)
        (*env)->DeleteLocalRef(env, buf);
    (*env)->DeleteLocalRef(env, cls);
    if (!res || (*env)->GetArrayLength(env, res) != UI_FONTS * PER_FONT) {
        if (res)
            (*env)->DeleteLocalRef(env, res);
        return failed(env, "no atlas");
    }
    (*env)->GetFloatArrayRegion(env, res, 0, UI_FONTS * PER_FONT, m);
    (*env)->DeleteLocalRef(env, res);
    for (f = 0; f < UI_FONTS; f++) {
        const float *p = m + f * PER_FONT;
        line[f] = p[0];
        for (c = UI_FIRST_CHAR; c <= UI_LAST_CHAR; c++) {
            const float *g = p + 1 + (c - UI_FIRST_CHAR) * 6;
            if (g[2] <= 0)
                continue;   /* (didn't fit) */
            glyph[f][c].u0 = g[0] / aw;
            glyph[f][c].v0 = g[1] / ah;
            glyph[f][c].u1 = (g[0] + g[2]) / aw;
            glyph[f][c].v1 = (g[1] + g[3]) / ah;
            glyph[f][c].w = g[2];
            glyph[f][c].h = g[3];
            glyph[f][c].adv = g[4];
            glyph[f][c].lead = g[5];
        }
    }
    return 1;
}
#else
typedef int ui_font_android_unused;   /* elsewhere: ui_font_gdi.c (Windows), ui_font_none.c */
#endif
