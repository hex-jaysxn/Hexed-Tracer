// HexedTracer.cpp  -  Hexed bullet tracer, all-in-one AML mod (HEXED Studio)
// Replaces libVC_Tracer_Nezuko.so + libVCTracerMenu.so (delete both).
//
// It hooks the game's CBulletTraces::AddTrace (to capture every shot) and
// CBulletTraces::Render (to draw our own textured ribbons instead of the stock ones).
// Textures are loaded with the game's own PNG reader from:
//     <Android/data/PACKAGE/files>/HexedTracer/*.png
//
// Config block (exported float[16], written by HexedTracer.lua):
//   [0] enabled        [1] thickness multiplier   [2] fade seconds
//   [3] r  [4] g  [5] b  [6] a   (0..1 tint / opacity)
//   [7] texture index  [8] glow (additive) 0/1    [9] tile length in meters (0 = stretch)
//   [10] status bits   [11] active traces         [12] texture count
//   [13] list generation   [14] rescan request    [15] Im3D flags override (0 = default)
// Status bits: 1 AddTrace(5) hook, 2 AddTrace(4) hook, 4 Render hook,
//              8 camera ok, 16 texture ready, 32 drew at least once

#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

// ---------------------------------------------------------------- AML mod info
struct ModInfo {
    char     id[0x30];
    char     name[0x30];
    char     version[0x18];
    char     author[0x30];
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(ModInfo) == 176, "ModInfo layout must be 176 bytes");

static ModInfo g_info = { "hexed_tracer", "Hexed Tracer", "1.0.0", "HEXED Studio", 1, 0 };

EXPORT ModInfo* modinfo = &g_info;
EXPORT ModInfo* __GetModInfo() { return &g_info; }

// ---------------------------------------------------------------- config / status
EXPORT volatile float HexedTracerCfg[16] = { 1, 1, 0.6f, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static volatile float* const cfg = HexedTracerCfg;

static unsigned g_status = 0;
static void SetBit(unsigned bit, bool on) {
    unsigned old = g_status;
    if (on) g_status |= bit; else g_status &= ~bit;
    if (old != g_status) cfg[10] = (float)g_status;
}

// ---------------------------------------------------------------- RenderWare bits
struct RxVert {                       // RxObjSpace3DVertex (36 bytes)
    float   x, y, z;
    float   nx, ny, nz;
    uint8_t rgba[4];
    float   u, v;
};
static_assert(sizeof(RxVert) == 36, "RxObjSpace3DVertex must be 36 bytes");

enum {
    rwRENDERSTATETEXTURERASTER   = 1,
    rwRENDERSTATEZTESTENABLE     = 6,
    rwRENDERSTATEZWRITEENABLE    = 8,
    rwRENDERSTATESRCBLEND        = 10,
    rwRENDERSTATEDESTBLEND       = 11,
    rwRENDERSTATEVERTEXALPHAENABLE = 12,
    rwRENDERSTATECULLMODE        = 20,
};
enum { rwBLENDONE = 2, rwBLENDSRCALPHA = 5, rwBLENDINVSRCALPHA = 6 };
enum { rwCULLMODECULLNONE = 1, rwCULLMODECULLBACK = 2 };
enum { rwPRIMTYPETRILIST = 3 };
static const int rwRASTERTYPETEXTURE = 4;

typedef void  RwImage;
typedef void  RwRaster;
typedef void  RwTexture;
typedef void  RwFrame;

typedef void*      (*Transform_t)(RxVert*, unsigned, void*, unsigned);
typedef int        (*RenderIdx_t)(int, uint16_t*, int);
typedef int        (*End_t)();
typedef int        (*StateSet_t)(int, void*);
typedef int        (*StateGet_t)(int, void*);
typedef RwImage*   (*PngRead_t)(const char*);
typedef int        (*ImgDestroy_t)(RwImage*);
typedef int        (*ImgFindFmt_t)(RwImage*, int, int*, int*, int*, int*);
typedef RwImage*   (*ImgResample_t)(const RwImage*, int, int);
typedef RwRaster*  (*RasterCreate_t)(int, int, int, int);
typedef int        (*RasterDestroy_t)(RwRaster*);
typedef RwRaster*  (*RasterSetImg_t)(RwRaster*, RwImage*);
typedef RwTexture* (*TexCreate_t)(RwRaster*);
typedef int        (*TexDestroy_t)(RwTexture*);
typedef void*      (*FrameLTM_t)(RwFrame*);
typedef int        (*DobbyHook_t)(void*, void*, void**);

static Transform_t     rwTransform;
static RenderIdx_t     rwRenderIdx;
static End_t           rwEnd;
static StateSet_t      rwSet;
static StateGet_t      rwGet;
static PngRead_t       rwPngRead;
static ImgDestroy_t    rwImgDestroy;
static ImgFindFmt_t    rwImgFindFmt;
static ImgResample_t   rwImgResample;
static RasterCreate_t  rwRasterCreate;
static RasterDestroy_t rwRasterDestroy;
static RasterSetImg_t  rwRasterSetImg;
static TexCreate_t     rwTexCreate;
static TexDestroy_t    rwTexDestroy;
static FrameLTM_t      rwFrameLTM;
static void**          rwEngineInstancePtr;   // &RwEngineInstance (a pointer variable)
static bool            g_rwOk = false;

static inline void RS(int state, int value) { rwSet(state, (void*)(intptr_t)value); }

// ---------------------------------------------------------------- shots (ring buffer)
struct Trace { float s[3]; float e[3]; double t; };
static const int MAXT = 256;
static Trace g_tr[MAXT];
static int   g_head = 0, g_cnt = 0;
static volatile int g_lock = 0;
static inline void Lock()   { while (__sync_lock_test_and_set(&g_lock, 1)) {} }
static inline void Unlock() { __sync_lock_release(&g_lock); }

static double Now() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static bool Finite3(const float* p) {
    for (int i = 0; i < 3; i++) if (!(fabsf(p[i]) < 1e6f)) return false;
    return true;
}

static void AddRecord(const float* s, const float* e) {
    if (!s || !e || !Finite3(s) || !Finite3(e)) return;
    double now = Now();
    Lock();
    if (g_cnt > 0) {                                   // drop the duplicate from the 2nd AddTrace overload
        const Trace& l = g_tr[(g_head + MAXT - 1) % MAXT];
        if (now - l.t < 0.05 &&
            fabsf(l.s[0]-s[0]) < 0.01f && fabsf(l.s[1]-s[1]) < 0.01f && fabsf(l.s[2]-s[2]) < 0.01f &&
            fabsf(l.e[0]-e[0]) < 0.01f && fabsf(l.e[1]-e[1]) < 0.01f && fabsf(l.e[2]-e[2]) < 0.01f) {
            Unlock(); return;
        }
    }
    Trace& t = g_tr[g_head];
    memcpy(t.s, s, 12); memcpy(t.e, e, 12); t.t = now;
    g_head = (g_head + 1) % MAXT;
    if (g_cnt < MAXT) g_cnt++;
    Unlock();
}

// ---------------------------------------------------------------- textures / folder
static const int MAXTEX = 32;
static char       g_dir[256];
static char       g_names[MAXTEX][64];
static int        g_nameCount = 0;
static RwTexture* g_tex[MAXTEX];
static uint8_t    g_fail[MAXTEX];

static bool IsDir(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void FindDir() {
    char pkg[128] = "com.nezukosamp.game";
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, pkg, sizeof(pkg) - 1);
        close(fd);
        if (n > 0) { pkg[n] = 0; char* c = strchr(pkg, ':'); if (c) *c = 0; }
        else strcpy(pkg, "com.nezukosamp.game");
    }
    const char* roots[] = { "/storage/emulated/0", "/sdcard", "/storage/self/primary" };
    for (int i = 0; i < 3; i++) {
        snprintf(g_dir, sizeof(g_dir), "%s/Android/data/%s/files/HexedTracer", roots[i], pkg);
        if (IsDir(g_dir)) return;
    }
    snprintf(g_dir, sizeof(g_dir), "%s/Android/data/%s/files/HexedTracer", roots[0], pkg);
    mkdir(g_dir, 0777);                                // create it so the user can drop PNGs in
}

static bool EndsWithPng(const char* n) {
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".png") == 0;
}

static void ScanFolder() {
    char tmp[MAXTEX][64]; int cnt = 0;
    DIR* d = opendir(g_dir);
    if (d) {
        while (dirent* e = readdir(d)) {
            if (cnt >= MAXTEX) break;
            if (e->d_name[0] == '.' || !EndsWithPng(e->d_name) || strlen(e->d_name) >= 64) continue;
            strcpy(tmp[cnt++], e->d_name);
        }
        closedir(d);
    }
    for (int i = 1; i < cnt; i++) {                    // alphabetical
        char k[64]; strcpy(k, tmp[i]); int j = i - 1;
        while (j >= 0 && strcmp(tmp[j], k) > 0) { strcpy(tmp[j + 1], tmp[j]); j--; }
        strcpy(tmp[j + 1], k);
    }
    for (int i = 0; i < cnt; i++) strcpy(g_names[i], tmp[i]);
    g_nameCount = cnt;
    cfg[12] = (float)cnt;
}

static int PrevPow2(int v) { int p = 1; while (p * 2 <= v && p < 512) p *= 2; return p; }

static RwTexture* LoadTexture(const char* file) {
    char path[360]; snprintf(path, sizeof(path), "%s/%s", g_dir, file);
    RwImage* img = rwPngRead(path);
    if (!img) return nullptr;
    int w = 0, h = 0, dep = 0, fmt = 0;
    if (!rwImgFindFmt(img, rwRASTERTYPETEXTURE, &w, &h, &dep, &fmt) || w <= 0 || h <= 0) {
        rwImgDestroy(img); return nullptr;
    }
    int tw = PrevPow2(w), th = PrevPow2(h);            // power-of-two, max 512
    int iw = ((int*)img)[1], ih = ((int*)img)[2];      // RwImage: flags,width,height,...
    if (iw != tw || ih != th) {
        RwImage* r = rwImgResample ? rwImgResample(img, tw, th) : nullptr;
        rwImgDestroy(img);
        if (!r) return nullptr;
        img = r;
    }
    RwRaster* ras = rwRasterCreate(tw, th, dep, fmt);
    if (!ras) { rwImgDestroy(img); return nullptr; }
    rwRasterSetImg(ras, img);
    rwImgDestroy(img);
    RwTexture* t = rwTexCreate(ras);
    if (!t) { rwRasterDestroy(ras); return nullptr; }
    return t;
}

static RwTexture* GetTexture(int idx) {
    if (idx < 0 || idx >= g_nameCount) return nullptr;
    if (g_tex[idx]) return g_tex[idx];
    if (g_fail[idx]) return nullptr;
    g_tex[idx] = LoadTexture(g_names[idx]);
    if (!g_tex[idx]) g_fail[idx] = 1;
    return g_tex[idx];
}

static void Rescan() {
    for (int i = 0; i < MAXTEX; i++) {
        if (g_tex[i] && g_rwOk) rwTexDestroy(g_tex[i]);
        g_tex[i] = nullptr; g_fail[i] = 0;
    }
    ScanFolder();
    cfg[13] = cfg[13] + 1.0f;                          // tell the menu the list changed
}

EXPORT const char* HexedTracerGetTextureName(int i) { return (i >= 0 && i < g_nameCount) ? g_names[i] : nullptr; }
EXPORT const char* HexedTracerGetDir() { return g_dir; }

// ---------------------------------------------------------------- camera
static bool GetCameraPos(float* pos) {
    if (!rwEngineInstancePtr) return false;
    void* eng = *rwEngineInstancePtr;                  // RwGlobals*
    if (!eng) return false;
    void* cam = *(void**)eng;                          // RwGlobals::curCamera
    if (!cam) return false;
    RwFrame* frame = *(RwFrame**)((char*)cam + 4);     // RwObjectHasFrame::object.parent
    if (!frame) return false;
    const float* m = (const float*)rwFrameLTM(frame);  // RwMatrix: pos at +0x30
    if (!m) return false;
    pos[0] = m[12]; pos[1] = m[13]; pos[2] = m[14];
    return Finite3(pos);
}

// ---------------------------------------------------------------- geometry
static inline void Cross(const float* a, const float* b, float* o) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static inline float Len(const float* a) { return __builtin_sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

static void SetV(RxVert& v, float x, float y, float z, float u, float vv,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    v.x = x; v.y = y; v.z = z; v.nx = v.ny = v.nz = 0.0f;
    v.rgba[0] = r; v.rgba[1] = g; v.rgba[2] = b; v.rgba[3] = a;
    v.u = u; v.v = vv;
}

// camera-facing ribbon from S to E; cam == nullptr -> use world-up as the reference
static bool BuildQuad(const float* S, const float* E, const float* cam, float halfW, float vEnd,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a, RxVert* out) {
    float d[3] = { E[0]-S[0], E[1]-S[1], E[2]-S[2] };
    float len = Len(d);
    if (len < 0.02f) return false;
    d[0] /= len; d[1] /= len; d[2] /= len;

    float ref[3];
    if (cam) { ref[0] = cam[0] - (S[0]+E[0])*0.5f; ref[1] = cam[1] - (S[1]+E[1])*0.5f; ref[2] = cam[2] - (S[2]+E[2])*0.5f; }
    else     { ref[0] = 0; ref[1] = 0; ref[2] = 1; }
    float side[3]; Cross(d, ref, side);
    float sl = Len(side);
    if (sl < 1e-4f) { float up[3] = { 0, 0, 1 }; Cross(d, up, side); sl = Len(side); if (sl < 1e-4f) { float x[3] = { 1, 0, 0 }; Cross(d, x, side); sl = Len(side); if (sl < 1e-4f) return false; } }
    float k = halfW / sl;
    side[0] *= k; side[1] *= k; side[2] *= k;

    SetV(out[0], S[0]-side[0], S[1]-side[1], S[2]-side[2], 0.0f, 0.0f, r, g, b, a);
    SetV(out[1], S[0]+side[0], S[1]+side[1], S[2]+side[2], 1.0f, 0.0f, r, g, b, a);
    SetV(out[2], E[0]-side[0], E[1]-side[1], E[2]-side[2], 0.0f, vEnd, r, g, b, a);
    SetV(out[3], E[0]+side[0], E[1]+side[1], E[2]+side[2], 1.0f, vEnd, r, g, b, a);
    return true;
}

static uint8_t To8(float f) { return (uint8_t)(f <= 0.0f ? 0 : f >= 1.0f ? 255 : f * 255.0f + 0.5f); }

// ---------------------------------------------------------------- Render hook
static void (*g_origRender)() = nullptr;
static const float kBaseWidth = 0.08f;      // meters at 1.0x thickness
static const int   BATCH = 32;              // quads per Im3D submit

static bool g_drewOnce = false;

__attribute__((noinline))
static void Hook_Render() {
    if (cfg[14] >= 0.5f) { cfg[14] = 0.0f; if (g_rwOk) Rescan(); }

    if (cfg[0] < 0.5f || !g_rwOk || g_nameCount <= 0) { g_origRender(); return; }

    int idx = (int)(cfg[7] + 0.5f);
    if (idx < 0 || idx >= g_nameCount) idx = 0;
    RwTexture* tex = GetTexture(idx);
    SetBit(16, tex != nullptr);
    if (!tex) { g_origRender(); return; }               // texture failed: fall back to the stock tracers

    // snapshot the shots
    static Trace snap[MAXT];
    int n; double now = Now();
    Lock();
    n = g_cnt;
    for (int i = 0; i < n; i++) snap[i] = g_tr[(g_head - n + i + MAXT * 2) % MAXT];
    Unlock();

    float fade = cfg[2]; if (fade < 0.02f) fade = 0.02f;
    int active = 0;
    for (int i = 0; i < n; i++) if (now - snap[i].t < fade) active++;
    cfg[11] = (float)active;
    if (active == 0) return;

    float camPos[3]; bool camOk = GetCameraPos(camPos);
    SetBit(8, camOk);

    float thick = cfg[1]; if (thick < 0.05f) thick = 0.05f;
    float halfW = 0.5f * kBaseWidth * thick;
    float tile  = cfg[9];
    bool  glow  = cfg[8] >= 0.5f;
    uint8_t R = To8(cfg[3]), G = To8(cfg[4]), B = To8(cfg[5]);
    float   A0 = cfg[6];
    unsigned flags = cfg[15] >= 0.5f ? (unsigned)(cfg[15] + 0.5f) : 1u;

    // render state (restored afterwards)
    void* oldRaster = nullptr; rwGet(rwRENDERSTATETEXTURERASTER, &oldRaster);
    RS(rwRENDERSTATEZWRITEENABLE, 0);
    RS(rwRENDERSTATEZTESTENABLE, 1);
    RS(rwRENDERSTATEVERTEXALPHAENABLE, 1);
    RS(rwRENDERSTATESRCBLEND, rwBLENDSRCALPHA);
    RS(rwRENDERSTATEDESTBLEND, glow ? rwBLENDONE : rwBLENDINVSRCALPHA);
    RS(rwRENDERSTATECULLMODE, rwCULLMODECULLNONE);
    rwSet(rwRENDERSTATETEXTURERASTER, *(void**)tex);

    static RxVert   verts[BATCH * 4];
    static uint16_t inds[BATCH * 6];
    int nq = 0;
    bool any = false;
    for (int i = 0; i < n; i++) {
        float age = (float)(now - snap[i].t);
        if (age >= fade) continue;
        float a = A0 * (1.0f - age / fade);
        if (a <= 0.003f) continue;

        float dx = snap[i].e[0]-snap[i].s[0], dy = snap[i].e[1]-snap[i].s[1], dz = snap[i].e[2]-snap[i].s[2];
        float vEnd = tile > 0.01f ? __builtin_sqrtf(dx*dx + dy*dy + dz*dz) / tile : 1.0f;

        if (!BuildQuad(snap[i].s, snap[i].e, camOk ? camPos : nullptr, halfW, vEnd, R, G, B, To8(a), &verts[nq * 4]))
            continue;
        uint16_t b0 = (uint16_t)(nq * 4);
        uint16_t* ix = &inds[nq * 6];
        ix[0] = b0;     ix[1] = b0 + 1; ix[2] = b0 + 2;
        ix[3] = b0 + 1; ix[4] = b0 + 3; ix[5] = b0 + 2;
        nq++;

        if (nq == BATCH) {
            if (rwTransform(verts, nq * 4, nullptr, flags)) { rwRenderIdx(rwPRIMTYPETRILIST, inds, nq * 6); rwEnd(); any = true; }
            nq = 0;
        }
    }
    if (nq > 0) {
        if (rwTransform(verts, nq * 4, nullptr, flags)) { rwRenderIdx(rwPRIMTYPETRILIST, inds, nq * 6); rwEnd(); any = true; }
    }

    RS(rwRENDERSTATEZWRITEENABLE, 1);
    RS(rwRENDERSTATESRCBLEND, rwBLENDSRCALPHA);
    RS(rwRENDERSTATEDESTBLEND, rwBLENDINVSRCALPHA);
    RS(rwRENDERSTATECULLMODE, rwCULLMODECULLBACK);
    rwSet(rwRENDERSTATETEXTURERASTER, oldRaster);

    if (any && !g_drewOnce) { g_drewOnce = true; SetBit(32, true); }
}

// ---------------------------------------------------------------- AddTrace hooks
typedef void (*Add5_t)(float*, float*, float, uint32_t, uint8_t);
typedef void (*Add4_t)(float*, float*, int, void*);
static Add5_t g_origAdd5 = nullptr;
static Add4_t g_origAdd4 = nullptr;

__attribute__((noinline))
static void Hook_Add5(float* s, float* e, float th, uint32_t life, uint8_t vis) {
    g_origAdd5(s, e, th, life, vis);
    AddRecord(s, e);
}
__attribute__((noinline))
static void Hook_Add4(float* s, float* e, int weapon, void* owner) {
    g_origAdd4(s, e, weapon, owner);
    AddRecord(s, e);
}

// ---------------------------------------------------------------- install
static void Install() {
    FindDir();
    ScanFolder();

    void* gta = dlopen("libGTASA.so", RTLD_NOW);
    void* dob = dlopen("libdobby.so", RTLD_NOW);
    if (!gta || !dob) return;

    DobbyHook_t dobbyHook = (DobbyHook_t)dlsym(dob, "DobbyHook");

    rwTransform     = (Transform_t)    dlsym(gta, "_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj");
    rwRenderIdx     = (RenderIdx_t)    dlsym(gta, "_Z28RwIm3DRenderIndexedPrimitive15RwPrimitiveTypePti");
    rwEnd           = (End_t)          dlsym(gta, "_Z9RwIm3DEndv");
    rwSet           = (StateSet_t)     dlsym(gta, "_Z16RwRenderStateSet13RwRenderStatePv");
    rwGet           = (StateGet_t)     dlsym(gta, "_Z16RwRenderStateGet13RwRenderStatePv");
    rwPngRead       = (PngRead_t)      dlsym(gta, "_Z14RtPNGImageReadPKc");
    rwImgDestroy    = (ImgDestroy_t)   dlsym(gta, "_Z14RwImageDestroyP7RwImage");
    rwImgFindFmt    = (ImgFindFmt_t)   dlsym(gta, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_");
    rwImgResample   = (ImgResample_t)  dlsym(gta, "_Z21RwImageCreateResamplePK7RwImageii");
    rwRasterCreate  = (RasterCreate_t) dlsym(gta, "_Z14RwRasterCreateiiii");
    rwRasterDestroy = (RasterDestroy_t)dlsym(gta, "_Z15RwRasterDestroyP8RwRaster");
    rwRasterSetImg  = (RasterSetImg_t) dlsym(gta, "_Z20RwRasterSetFromImageP8RwRasterP7RwImage");
    rwTexCreate     = (TexCreate_t)    dlsym(gta, "_Z15RwTextureCreateP8RwRaster");
    rwTexDestroy    = (TexDestroy_t)   dlsym(gta, "_Z16RwTextureDestroyP9RwTexture");
    rwFrameLTM      = (FrameLTM_t)     dlsym(gta, "_Z13RwFrameGetLTMP7RwFrame");
    rwEngineInstancePtr = (void**)     dlsym(gta, "RwEngineInstance");

    g_rwOk = rwTransform && rwRenderIdx && rwEnd && rwSet && rwGet && rwPngRead && rwImgDestroy &&
             rwImgFindFmt && rwRasterCreate && rwRasterDestroy && rwRasterSetImg && rwTexCreate &&
             rwTexDestroy && rwFrameLTM && rwEngineInstancePtr;

    void* tAdd5   = dlsym(gta, "_ZN13CBulletTraces8AddTraceEP7CVectorS1_fjh");
    void* tAdd4   = dlsym(gta, "_ZN13CBulletTraces8AddTraceEP7CVectorS1_iP7CEntity");
    void* tRender = dlsym(gta, "_ZN13CBulletTraces6RenderEv");
    if (!dobbyHook || !tRender || !g_rwOk) return;

    if (tAdd5 && dobbyHook(tAdd5, (void*)Hook_Add5, (void**)&g_origAdd5) == 0 && g_origAdd5) SetBit(1, true);
    if (tAdd4 && dobbyHook(tAdd4, (void*)Hook_Add4, (void**)&g_origAdd4) == 0 && g_origAdd4) SetBit(2, true);
    if (dobbyHook(tRender, (void*)Hook_Render, (void**)&g_origRender) == 0 && g_origRender) SetBit(4, true);
}

EXPORT void OnModLoad() { Install(); }
