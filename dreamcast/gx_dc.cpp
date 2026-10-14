// A simple GX renderer for the Dreamcast, straight onto the PowerVR (KOS pvr).
//
// The PC's sms_gx front end is shared: gx_api.cpp and gx_fifo.cpp turn every
// GX call, display list and write-gather byte into register writes and
// decode each primitive's vertices into a batch. This file draws those
// batches in place of the PC's OpenGL 3.3 renderer, approximately:
//
// - transform: position matrix, projection and viewport on the CPU, with
//   clipping at the near plane; depth is 1/w, as the PowerVR wants.
// - colour: channel 0/1 lighting per vertex (material, ambient, diffuse
//   lights), then the TEV stages per vertex with every texture as white.
//   No textures, fog or indirect stages.
// - opaque triangles go straight to the tile accelerator (direct rendering,
//   no RAM buffer); blended ones wait in a small buffer for the translucent
//   list, which the PowerVR takes after the opaque one.
// - EFB copies: a display copy ends the frame. Copies to textures are not
//   made; peeks and pixel metrics read nothing.
#include "gx_internal.h"
#include "sms_gx/gx_pc.h"

#include <dc/pvr.h>
#include <dc/sq.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace gx {

bool g_gxStats = false;
double g_flushSeconds = 0;
uint32_t g_traceLastVat = 0;
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}

static bool s_ready;
bool rendererReady() { return s_ready; }

// ------------------------------------------------------------------ batch
static std::vector<uint8_t> s_bdata;  // packed vertices (gx_fifo.cpp's layout)
static std::vector<uint32_t> s_bidx;  // triangle indices into the batch
// Vertices per batch before it is drawn: keeps the buffers small (RAM).
static const uint32_t kMaxBatch = 4096;
static uint32_t s_bcount, s_bfmt, s_bstride = 12;
static PrimClass s_bclass = PRIM_TRIS;

void onStateChange() {
    if (!s_bidx.empty()) flushBatch();
}

PrimClass primClass(uint8_t op) { return op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS; }

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride) {
    PrimClass cls = primClass(op);
    if (s_bcount && (cls != s_bclass || fmt != s_bfmt || s_bcount + n > kMaxBatch)) flushBatch();
    s_bclass = cls;
    s_bfmt = fmt;
    s_bstride = stride;
    s_bdata.resize(size_t(s_bcount + n) * stride);
    return s_bdata.data() + size_t(s_bcount) * stride;
}

// Triangle indices only: lines and points are not drawn.
uint32_t primitiveIndices(uint8_t op, uint32_t n, uint32_t base, uint32_t* w) {
    uint32_t* const w0 = w;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        w[0] = base + a;
        w[1] = base + b;
        w[2] = base + c;
        w += 3;
    };
    switch (op & 0xF8) {
    case 0x80:
    case 0x88:  // quads
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
        break;
    case 0x90:  // triangles
        for (uint32_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2);
        break;
    case 0x98:  // strip
        for (uint32_t i = 2; i < n; i++) {
            if (i & 1) tri(i - 1, i - 2, i);
            else tri(i - 2, i - 1, i);
        }
        break;
    case 0xA0:  // fan
        for (uint32_t i = 2; i < n; i++) tri(0, i - 1, i);
        break;
    }
    return uint32_t(w - w0);
}

void primitiveEnd(uint8_t op, uint32_t n) {
    if (!s_ready || n == 0) return;
    uint32_t base = s_bcount;
    s_bcount += n;
    if (s_bclass != PRIM_TRIS) return;
    size_t at = s_bidx.size();
    s_bidx.resize(at + size_t(n) * 3);
    s_bidx.resize(at + primitiveIndices(op, n, base, s_bidx.data() + at));
}

// Display-list cache runs: copied into the batch (no arena on the PowerVR).
void appendDecoded(PrimClass cls, uint32_t fmt, uint32_t stride, const uint8_t* verts, uint32_t n, const uint32_t* idx,
                   uint32_t nidx) {
    if (s_bcount && (cls != s_bclass || fmt != s_bfmt || s_bcount + n > kMaxBatch)) flushBatch();
    s_bclass = cls;
    s_bfmt = fmt;
    s_bstride = stride;
    if (!s_ready || n == 0) return;
    s_bdata.resize(size_t(s_bcount + n) * stride);
    memcpy(s_bdata.data() + size_t(s_bcount) * stride, verts, size_t(n) * stride);
    uint32_t base = s_bcount;
    s_bcount += n;
    if (cls != PRIM_TRIS) return;
    size_t at = s_bidx.size();
    s_bidx.resize(at + nidx);
    for (uint32_t i = 0; i < nidx; i++) s_bidx[at + i] = idx[i] + base;
}
bool arenaEnabled() { return false; }
uint32_t arenaGeneration() { return 1; }
bool arenaUpload(const uint8_t*, uint32_t, uint32_t, const uint32_t*, uint32_t, ArenaRef&) { return false; }
void appendArena(PrimClass, uint32_t, uint32_t, const ArenaRef&, uint32_t, uint32_t, const uint8_t*) {}
bool batchUsesArena() { return false; }
FILE* traceFile() { return nullptr; }
uint32_t g_displayFrames;

// ------------------------------------------------------------------ XF helpers
static inline float xfm(uint32_t w) {
    float f;
    memcpy(&f, &g.xfMem[w], 4);
    return f;
}
static inline float xfr(uint32_t r) {
    float f;
    memcpy(&f, &g.xfReg[r], 4);
    return f;
}
static inline void unpackColor(uint32_t v, float* o) {
    o[0] = float((v >> 24) & 255) / 255.0f;
    o[1] = float((v >> 16) & 255) / 255.0f;
    o[2] = float((v >> 8) & 255) / 255.0f;
    o[3] = float(v & 255) / 255.0f;
}
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Lit colour of one channel part (rgb or alpha), as the PC's vertex shader.
static void lightChannel(uint32_t ctrl, const float* vtxClr, const float* matReg, const float* ambReg,
                         const float* vpos, const float* N, bool alpha, float* out) {
    const float* mat = (ctrl & 1) ? vtxClr : matReg;
    int c0 = alpha ? 3 : 0, c1 = alpha ? 4 : 3;
    if (!(ctrl & 2)) {
        for (int c = c0; c < c1; c++) out[c] = mat[c];
        return;
    }
    const float* amb = (ctrl & 64) ? vtxClr : ambReg;
    float sum[4] = {amb[0], amb[1], amb[2], amb[3]};
    uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
    uint32_t diff = (ctrl >> 7) & 3;
    bool attn = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
    for (int l = 0; l < 8; l++) {
        if (!(mask & (1u << l))) continue;
        uint32_t lb = 0x600 + l * 16;
        float lc[4];
        unpackColor(g.xfMem[lb + 3], lc);
        float A[3] = {xfm(lb + 4), xfm(lb + 5), xfm(lb + 6)};
        float K[3] = {xfm(lb + 7), xfm(lb + 8), xfm(lb + 9)};
        float P[3] = {xfm(lb + 10), xfm(lb + 11), xfm(lb + 12)};
        float D[3] = {xfm(lb + 13), xfm(lb + 14), xfm(lb + 15)};
        float L[3], at = 1.0f;
        if (!attn || !spot) {
            float* src = attn ? P : P;  // attenuation off and specular both aim at the light's position
            float d = sqrtf(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
            if (!attn) {
                L[0] = P[0] - vpos[0];
                L[1] = P[1] - vpos[1];
                L[2] = P[2] - vpos[2];
                d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            } else {
                L[0] = P[0];
                L[1] = P[1];
                L[2] = P[2];
            }
            if (d > 0) {
                L[0] /= d;
                L[1] /= d;
                L[2] /= d;
            }
            if (attn) {  // specular
                float nl = N[0] * L[0] + N[1] * L[1] + N[2] * L[2];
                float t = nl > 0 ? N[0] * D[0] + N[1] * D[1] + N[2] * D[2] : 0;
                if (t < 0) t = 0;
                float num = A[0] + A[1] * t + A[2] * t * t, den = K[0] + K[1] * t + K[2] * t * t;
                at = (num > 0 ? num : 0) / (den > 1e-30f ? den : 1e-30f);
            }
        } else {  // spot / distance attenuation
            L[0] = P[0] - vpos[0];
            L[1] = P[1] - vpos[1];
            L[2] = P[2] - vpos[2];
            float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], d = sqrtf(d2);
            if (d > 0) {
                L[0] /= d;
                L[1] /= d;
                L[2] /= d;
            }
            float cs = L[0] * D[0] + L[1] * D[1] + L[2] * D[2];
            if (cs < 0) cs = 0;
            float num = A[0] + A[1] * cs + A[2] * cs * cs, den = K[0] + K[1] * d + K[2] * d2;
            at = (num > 0 ? num : 0) / (den > 1e-30f ? den : 1e-30f);
        }
        float nl = N[0] * L[0] + N[1] * L[1] + N[2] * L[2];
        float df = diff == 0 ? 1.0f : diff == 1 ? nl : (nl > 0 ? nl : 0);
        for (int c = c0; c < c1; c++) sum[c] += at * df * lc[c];
    }
    for (int c = c0; c < c1; c++) out[c] = mat[c] * clamp01(sum[c]);
}

// ------------------------------------------------------------------ TEV, per vertex
static inline int sext11(uint32_t v) { return int32_t(v << 21) >> 21; }

struct TevSetup {
    uint32_t nst;
    uint32_t cenv[16], aenv[16], order[16], kc[16], ka[16];
    float reg[4][4], konst[4][4];
    int texStage;  // the first stage that samples a texture, or -1
};

static void tevSetup(TevSetup& t) {
    uint32_t gen = g.bp[BP_GENMODE];
    t.nst = ((gen >> 10) & 15) + 1;
    t.texStage = -1;
    for (uint32_t s = 0; s < t.nst; s++) {
        t.cenv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s];
        t.aenv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1];
        t.order[s] = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
        uint32_t k = (s & 1) ? (ks >> 14) & 0x3FF : (ks >> 4) & 0x3FF;
        t.kc[s] = k & 31;
        t.ka[s] = (k >> 5) & 31;
        if (t.texStage < 0 && ((t.order[s] >> 6) & 1)) t.texStage = int(s);
    }
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        t.reg[i][0] = sext11(ra & 0x7FF) / 255.0f;
        t.reg[i][1] = sext11((bg >> 12) & 0x7FF) / 255.0f;
        t.reg[i][2] = sext11(bg & 0x7FF) / 255.0f;
        t.reg[i][3] = sext11((ra >> 12) & 0x7FF) / 255.0f;
        ra = g.kreg[2 * i];
        bg = g.kreg[2 * i + 1];
        t.konst[i][0] = float(ra & 0xFF) / 255.0f;
        t.konst[i][1] = float((bg >> 12) & 0xFF) / 255.0f;
        t.konst[i][2] = float(bg & 0xFF) / 255.0f;
        t.konst[i][3] = float((ra >> 12) & 0xFF) / 255.0f;
    }
}

static float konstSel(const TevSetup& t, uint32_t sel, int comp) {
    static const float frac[8] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
    if (sel < 8) return frac[sel];
    if (sel >= 12 && sel < 16) return t.konst[sel - 12][comp];
    if (sel >= 16) return t.konst[(sel - 16) & 3][(sel - 16) >> 2];  // K0_R .. K3_A
    return 1.0f;
}

// Runs the TEV stages with the texture as white; ras[2][4] are the lit
// channels. Writes the final colour.
static void tevEval(const TevSetup& t, const float ras[2][4], float* out) {
    float r[4][4];
    memcpy(r, t.reg, sizeof r);
    for (uint32_t s = 0; s < t.nst; s++) {
        uint32_t chan = (t.order[s] >> 7) & 7;
        const float zero[4] = {0, 0, 0, 0};
        const float* rc = chan == 0 ? ras[0] : chan == 1 ? ras[1] : zero;
        float kc[3], ka = konstSel(t, t.ka[s], 3);
        for (int c = 0; c < 3; c++) kc[c] = konstSel(t, t.kc[s], c);
        // colour
        uint32_t ce = t.cenv[s];
        uint32_t sel[4] = {(ce >> 12) & 15, (ce >> 8) & 15, (ce >> 4) & 15, ce & 15};
        float in[4][3];
        for (int i = 0; i < 4; i++) {
            for (int c = 0; c < 3; c++) {
                float v;
                switch (sel[i]) {
                case 0: v = r[0][c]; break;
                case 1: v = r[0][3]; break;
                case 2: v = r[1][c]; break;
                case 3: v = r[1][3]; break;
                case 4: v = r[2][c]; break;
                case 5: v = r[2][3]; break;
                case 6: v = r[3][c]; break;
                case 7: v = r[3][3]; break;
                case 8: case 9: case 12: v = 1.0f; break;  // TEXC, TEXA (white), ONE
                case 10: v = rc[c]; break;
                case 11: v = rc[3]; break;
                case 13: v = 0.5f; break;
                case 14: v = kc[c]; break;
                default: v = 0.0f; break;
                }
                in[i][c] = i < 3 ? clamp01(v) : v;
            }
        }
        // alpha
        uint32_t ae = t.aenv[s];
        uint32_t asel[4] = {(ae >> 13) & 7, (ae >> 10) & 7, (ae >> 7) & 7, (ae >> 4) & 7};
        float ain[4];
        for (int i = 0; i < 4; i++) {
            float v;
            switch (asel[i]) {
            case 0: v = r[0][3]; break;
            case 1: v = r[1][3]; break;
            case 2: v = r[2][3]; break;
            case 3: v = r[3][3]; break;
            case 4: v = 1.0f; break;  // TEXA (white)
            case 5: v = rc[3]; break;
            case 6: v = ka; break;
            default: v = 0.0f; break;
            }
            ain[i] = i < 3 ? clamp01(v) : v;
        }
        static const float kBias[4] = {0.0f, 0.5f, -0.5f, 0.0f};
        static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
        float res[4];
        for (int c = 0; c < 3; c++) {
            float lerp = in[0][c] * (1.0f - in[2][c]) + in[1][c] * in[2][c];
            float v = ((ce >> 16) & 3) == 3 ? in[3][c] : (in[3][c] + (((ce >> 18) & 1) ? -lerp : lerp) + kBias[(ce >> 16) & 3]) * kScale[(ce >> 20) & 3];
            res[c] = ((ce >> 19) & 1) ? clamp01(v) : v;
        }
        {
            float lerp = ain[0] * (1.0f - ain[2]) + ain[1] * ain[2];
            float v = ((ae >> 16) & 3) == 3 ? ain[3] : (ain[3] + (((ae >> 18) & 1) ? -lerp : lerp) + kBias[(ae >> 16) & 3]) * kScale[(ae >> 20) & 3];
            res[3] = ((ae >> 19) & 1) ? clamp01(v) : v;
        }
        uint32_t cd = (ce >> 22) & 3, ad = (ae >> 22) & 3;
        r[cd][0] = res[0];
        r[cd][1] = res[1];
        r[cd][2] = res[2];
        r[ad][3] = res[3];
    }
    for (int c = 0; c < 4; c++) out[c] = clamp01(r[0][c]);
}


// ------------------------------------------------------------------ PVR
// The tile accelerator's vertex buffer, two of them in video RAM (which holds
// only these and the frame buffers: no textures).
static const int kVertexBuf = 2560 * 1024;
static const uint32_t kTaBudget = kVertexBuf - 64 * 1024;  // per frame, with room for the headers
static uint32_t s_taBytes;
// Blended triangles, for the translucent list.
static const uint32_t kTrBuf = 256 * 1024;
static uint8_t* s_tr;
static uint32_t s_trUsed;
static bool s_trFull;

static void sceneBegin() {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    // An open list holds the store queues' lock, which other KOS code (and
    // the game's other threads) need while a frame is drawn: taken only while
    // a batch goes out (flushBatch), and again for pvr_list_finish to drop.
    sq_unlock();
    s_taBytes = 0;
    s_trUsed = 0;
    s_trFull = false;
}

static void sceneEnd() {
    sq_lock((void*)PVR_TA_INPUT);
    pvr_list_finish();
    if (s_trUsed) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_prim(s_tr, s_trUsed);
        pvr_list_finish();
    }
    pvr_scene_finish();
}

// Where the current batch's header and vertices go.
static bool s_trBatch;

static inline void put32(const void* p) {
    if (s_trBatch) {
        memcpy(s_tr + s_trUsed, p, 32);
        s_trUsed += 32;
    } else {
        uint32_t* d = (uint32_t*)pvr_dr_target();
        const uint32_t* w = (const uint32_t*)p;  // whole words into the store queue
        for (int i = 0; i < 8; i++) d[i] = w[i];
        pvr_dr_commit(d);
    }
    s_taBytes += 32;
}

// ------------------------------------------------------------------ state
static const pvr_depthcmp_mode_t kDepthCmp[8] = {  // GX's on z, PVR's on 1/w
    PVR_DEPTHCMP_NEVER,   PVR_DEPTHCMP_GREATER,  PVR_DEPTHCMP_EQUAL,  PVR_DEPTHCMP_GEQUAL,
    PVR_DEPTHCMP_LESS,    PVR_DEPTHCMP_NOTEQUAL, PVR_DEPTHCMP_LEQUAL, PVR_DEPTHCMP_ALWAYS};

// The batch's polygon header; false when it draws nothing visible.
static bool batchHeader(pvr_poly_hdr_t* hdr) {
    uint32_t cull = (g.bp[BP_GENMODE] >> 14) & 3;
    if (cull == 3) return false;
    uint32_t cm = g.bp[BP_CMODE0];
    if (!((cm >> 3) & 1)) return false;  // no colour writes (alpha or depth only)
    bool blend = (cm & 1) && !((cm >> 11) & 1);
    uint32_t sf = (cm >> 8) & 7, df = (cm >> 5) & 7;
    if (blend && sf == 1 && df == 0) blend = false;
    if (blend && sf == 0 && df == 1) return false;  // leaves the frame as it is
    s_trBatch = blend;
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, blend ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    uint32_t z = g.bp[BP_ZMODE];
    if (z & 1) {
        cxt.depth.comparison = kDepthCmp[(z >> 1) & 7];
        cxt.depth.write = (z >> 4) & 1;
    } else {
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = false;
    }
    if (blend) {  // GX's factors are in the PowerVR's order
        cxt.blend.src = (pvr_blend_mode_t)sf;
        cxt.blend.dst = (pvr_blend_mode_t)df;
    }
    pvr_poly_compile(hdr, &cxt);
    return true;
}

// ------------------------------------------------------------------ draw
struct ClipV {
    float x, y, z, w;  // GX clip space
    float c[4];
};
struct Screen {
    float sx, sy, sz, ox, oy, oz, yoff;
    bool ortho;
    float nearW;
};
static Screen s_scr;
static float s_proj[6];
static int s_efbH = 448;  // the height of the last display copy

static uint32_t argb(const float* c) {
    auto b = [](float v) { return uint32_t(clamp01(v) * 255.0f + 0.5f); };
    return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

static void putVertex(const ClipV& v, bool eol) {
    alignas(32) pvr_vertex_t o;
    float iw = 1.0f / v.w;
    o.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    o.x = s_scr.sx * v.x * iw + s_scr.ox;
    o.y = s_scr.sy * v.y * iw + s_scr.oy + s_scr.yoff;
    if (s_scr.ortho) {
        float d = clamp01((s_scr.sz * v.z + s_scr.oz) / 16777215.0f);
        o.z = 1.0f / (1.0f + d);  // nearer is larger, as with 1/w
    } else {
        o.z = iw;
    }
    o.u = o.v = 0.0f;
    o.argb = argb(v.c);
    o.oargb = 0;
    put32(&o);
}

static void putTri(const ClipV& a, const ClipV& b, const ClipV& c) {
    if (s_taBytes + 3 * 32 > kTaBudget) return;
    if (s_trBatch && s_trUsed + 3 * 32 > kTrBuf) {
        s_trFull = true;
        return;
    }
    putVertex(a, false);
    putVertex(b, false);
    putVertex(c, true);
}

static ClipV lerpV(const ClipV& a, const ClipV& b, float t) {
    ClipV r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    for (int k = 0; k < 4; k++) r.c[k] = a.c[k] + (b.c[k] - a.c[k]) * t;
    return r;
}

// One triangle, clipped to the near plane (w >= near) when perspective.
static void drawTri(const ClipV& a, const ClipV& b, const ClipV& c) {
    float n = s_scr.nearW;
    bool ia = a.w >= n, ib = b.w >= n, ic = c.w >= n;
    if (ia && ib && ic) {
        // all three off the same edge of the screen: nothing to draw
        float ax = s_scr.sx * a.x / a.w + s_scr.ox, bx = s_scr.sx * b.x / b.w + s_scr.ox, cx = s_scr.sx * c.x / c.w + s_scr.ox;
        if ((ax < 0 && bx < 0 && cx < 0) || (ax > 640 && bx > 640 && cx > 640)) return;
        float ay = s_scr.sy * a.y / a.w + s_scr.oy + s_scr.yoff, by = s_scr.sy * b.y / b.w + s_scr.oy + s_scr.yoff,
              cy = s_scr.sy * c.y / c.w + s_scr.oy + s_scr.yoff;
        if ((ay < 0 && by < 0 && cy < 0) || (ay > 480 && by > 480 && cy > 480)) return;
        putTri(a, b, c);
        return;
    }
    if (!ia && !ib && !ic) return;
    const ClipV* in[3] = {&a, &b, &c};
    ClipV poly[4];
    int np = 0;
    for (int i = 0; i < 3; i++) {
        const ClipV& p = *in[i];
        const ClipV& q = *in[(i + 1) % 3];
        bool pin = p.w >= n, qin = q.w >= n;
        if (pin) poly[np++] = p;
        if (pin != qin) poly[np++] = lerpV(p, q, (n - p.w) / (q.w - p.w));
    }
    for (int i = 2; i < np; i++) putTri(poly[0], poly[i - 1], poly[i]);
}

static void loadScreen() {
    for (int i = 0; i < 6; i++) s_proj[i] = xfr(XFR_PROJ + i);
    s_scr.ortho = g.xfReg[XFR_PROJ + 6] & 1;
    s_scr.sx = xfr(XFR_VIEWPORT);
    s_scr.sy = xfr(XFR_VIEWPORT + 1);
    s_scr.sz = xfr(XFR_VIEWPORT + 2);
    s_scr.ox = xfr(XFR_VIEWPORT + 3) - 342.0f;
    s_scr.oy = xfr(XFR_VIEWPORT + 4) - 342.0f;
    s_scr.oz = xfr(XFR_VIEWPORT + 5);
    s_scr.yoff = float(480 - s_efbH) * 0.5f;
    s_scr.nearW = 1e-4f;
    if (!s_scr.ortho) {  // GX perspective: p4 = -n/(f-n), p5 = -fn/(f-n), so n = p5/(p4-1)
        float n = s_proj[5] / (s_proj[4] - 1.0f);
        if (n > 1e-4f) s_scr.nearW = n;
    }
}

static std::vector<ClipV> s_verts;

void flushBatch() {
    if (s_bidx.empty() || !s_ready || s_bclass != PRIM_TRIS) {
        s_bidx.clear();
        s_bcount = 0;
        return;
    }
    alignas(32) pvr_poly_hdr_t hdr;
    if (!batchHeader(&hdr) || s_taBytes + 32 > kTaBudget || (s_trBatch && s_trUsed + 32 > kTrBuf)) {
        s_bidx.clear();
        s_bcount = 0;
        return;
    }
    TevSetup tev;
    tevSetup(tev);
    loadScreen();

    uint32_t matA = g.xfReg[XFR_MATIDX_A], matB = g.xfReg[XFR_MATIDX_B];
    uint8_t defMtx[12] = {uint8_t(matA & 63), uint8_t((matA >> 6) & 63), uint8_t((matA >> 12) & 63),
                          uint8_t((matA >> 18) & 63), uint8_t((matA >> 24) & 63), uint8_t(matB & 63),
                          uint8_t((matB >> 6) & 63), uint8_t((matB >> 12) & 63), uint8_t((matB >> 18) & 63), 0, 0, 0};
    uint32_t numChans = g.xfReg[XFR_NUMCHANS] & 3;
    float amb[2][4], mat[2][4];
    for (int c = 0; c < 2; c++) {
        unpackColor(g.xfReg[XFR_AMB0 + c], amb[c]);
        unpackColor(g.xfReg[XFR_MAT0 + c], mat[c]);
    }
    const float* P = s_proj;
    s_verts.resize(s_bcount);
    HostVertex hv;
    for (uint32_t i = 0; i < s_bcount; i++) {
        unpackVertex(s_bfmt, s_bdata.data() + size_t(i) * s_bstride, defMtx, hv);
        ClipV& o = s_verts[i];
        uint32_t pm = hv.mtx[0] * 4;
        float vp[3];
        for (int r = 0; r < 3; r++)
            vp[r] = xfm(pm + 4 * r) * hv.pos[0] + xfm(pm + 4 * r + 1) * hv.pos[1] + xfm(pm + 4 * r + 2) * hv.pos[2] +
                    xfm(pm + 4 * r + 3);
        if (!s_scr.ortho) {
            o.x = P[0] * vp[0] + P[1] * vp[2];
            o.y = P[2] * vp[1] + P[3] * vp[2];
            o.z = P[4] * vp[2] + P[5];
            o.w = -vp[2];
        } else {
            o.x = P[0] * vp[0] + P[1];
            o.y = P[2] * vp[1] + P[3];
            o.z = P[4] * vp[2] + P[5];
            o.w = 1.0f;
        }

        float ras[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
        float N[3] = {0, 0, 1};
        if (numChans && (s_bfmt & VF_NRM)) {
            uint32_t nm = 0x400 + hv.mtx[0] * 3;
            for (int r = 0; r < 3; r++)
                N[r] = xfm(nm + 3 * r) * hv.nrm[0] + xfm(nm + 3 * r + 1) * hv.nrm[1] + xfm(nm + 3 * r + 2) * hv.nrm[2];
            float l = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
            if (l > 0) {
                N[0] /= l;
                N[1] /= l;
                N[2] /= l;
            }
        }
        for (uint32_t c = 0; c < numChans && c < 2; c++) {
            float vc[4];
            for (int k = 0; k < 4; k++) vc[k] = float(hv.clr[c][k]) / 255.0f;
            lightChannel(g.xfReg[XFR_COLOR0CTRL + c], vc, mat[c], amb[c], vp, N, false, ras[c]);
            lightChannel(g.xfReg[XFR_ALPHA0CTRL + c], vc, mat[c], amb[c], vp, N, true, ras[c]);
        }
        tevEval(tev, ras, o.c);
    }

    if (!s_trBatch) sq_lock((void*)PVR_TA_INPUT);
    put32(&hdr);
    for (size_t i = 0; i + 2 < s_bidx.size(); i += 3)
        drawTri(s_verts[s_bidx[i]], s_verts[s_bidx[i + 1]], s_verts[s_bidx[i + 2]]);
    if (!s_trBatch) sq_unlock();
    s_bidx.clear();
    s_bcount = 0;
}

void markXfMemDirty() {}
void textureInvalidateAll() {}

// ------------------------------------------------------------------ copies
void executeCopy(uint32_t ctrl) {
    if (!s_ready) return;
    flushBatch();
    bool disp = (ctrl >> 14) & 1, clear = (ctrl >> 11) & 1;
    if (!disp) return;  // copies to textures are not made
    s_efbH = int((g.bp[BP_COPY_SRC_WH] >> 10) & 0x3FF) + 1;
    if (clear) {
        uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB];
        pvr_set_bg_color(float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f, float(gb & 255) / 255.0f);
    }
    sceneEnd();
    g_displayFrames++;
    static uint32_t frames;
    if (++frames % 300 == 0)
        logmsg("frame %u: %u KiB to the PVR%s", (unsigned)frames, (unsigned)(s_taBytes >> 10),
               s_trFull ? ", translucent buffer full" : "");
    sceneBegin();
}

uint32_t peekColor(int, int) { return 0; }
uint32_t peekZ(int, int) { return 0xFFFFFF; }
void pixMetricClear() {}
uint32_t pixMetricRead() { return 0; }

}  // namespace gx

using namespace gx;

extern "C" int GXPC_InitAuto(float) {
    if (s_ready) return 1;
    pvr_init_params_t params = {
        {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0},  // opaque, translucent
        kVertexBuf, 0, 0, 0, 3, 0};
    pvr_init(&params);
    pvr_set_bg_color(0, 0, 0);
    s_tr = (uint8_t*)memalign(32, kTrBuf);
    sceneBegin();
    s_ready = true;
    logmsg("PowerVR, simple untextured renderer");
    return 1;
}

// Widescreen and HUD placement are PC options.
extern "C" void GXPC_SetHud(int) {}
extern "C" void GXPC_HudPaneBegin(float, float) {}
extern "C" void GXPC_HudPaneEnd(void) {}
extern "C" void GXPC_SetStretch2D(int) {}

// Controller-specific button glyphs (gx_prompts.cpp) are a PC option: the
// font's own glyphs are drawn.
extern "C" const void* port_button_glyph(int) { return nullptr; }
