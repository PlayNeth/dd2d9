// Direct3D 3-6 (d3dim) check and benchmark: dd2d9's CPU vertex pipeline and execute buffers.
//   d3dimbench [seconds]       default 2 s per timed part
// For each part it prints a hash of what came out (ProcessVertices' vertices, the rendered
// pixels) and how fast it ran: two builds that should give the same output are compared by
// their hashes. Results are appended to d3dimbench.txt next to the executable, and each
// rendered image is written as d3dimbench-<part>.raw (256x256, 32-bit).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static FILE* g_out;
static char g_dir[MAX_PATH];
static double g_seconds = 2.0;
// Time inside the Direct3D calls that draw (the CPU's part: Wine's own thread and the GPU
// may take longer, and then decide the frame rate instead).
static double g_callTime;
constexpr int kW = 256, kH = 256;

static double Now() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static uint32_t Fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void Log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(g_out, fmt, a);
    va_end(a);
    fputc('\n', g_out);
    fflush(g_out);
}

// The render target's pixels: hashed, and written to d3dimbench-<name>.raw.
static uint32_t Pixels(IDirectDrawSurface4* s, const char* name) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return 0;
    std::vector<uint32_t> px(kW * kH);
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            px[y * kW + x] = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(d.lpSurface) + y * d.lPitch)[x] & 0xFFFFFF;
    s->Unlock(nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sd3dimbench-%s.raw", g_dir, name);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(px.data(), 4, px.size(), f);
        fclose(f);
    }
    return Fnv(px.data(), px.size() * 4);
}

static DWORD Bits(float f) {
    DWORD d;
    memcpy(&d, &f, 4);
    return d;
}

static D3DMATRIX Identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

static D3DMATRIX Perspective(float fov, float aspect, float zn, float zf) {
    D3DMATRIX m = {};
    const float h = 1.0f / tanf(fov * 0.5f), w = h / aspect, q = zf / (zf - zn);
    m._11 = w;
    m._22 = h;
    m._33 = q;
    m._34 = 1.0f;
    m._43 = -q * zn;
    return m;
}

static D3DMATRIX RotY(float a) {
    D3DMATRIX m = Identity();
    m._11 = cosf(a), m._13 = -sinf(a), m._31 = sinf(a), m._33 = cosf(a);
    return m;
}

// A sphere of radius 1 at (cx, cy, cz): (g+1)^2 vertices, 2 g^2 triangles in rows.
static void Sphere(int g, float cx, float cy, float cz, std::vector<D3DVERTEX>* v, std::vector<WORD>* idx) {
    v->clear();
    idx->clear();
    for (int i = 0; i <= g; i++) {
        const float th = 3.14159265f * float(i) / float(g);
        for (int j = 0; j <= g; j++) {
            const float ph = 2.0f * 3.14159265f * float(j) / float(g);
            D3DVERTEX p = {};
            p.nx = sinf(th) * cosf(ph);
            p.ny = cosf(th);
            p.nz = sinf(th) * sinf(ph);
            p.x = cx + p.nx;
            p.y = cy + p.ny;
            p.z = cz + p.nz;
            p.tu = float(j) / float(g);
            p.tv = float(i) / float(g);
            v->push_back(p);
        }
    }
    for (int i = 0; i < g; i++)
        for (int j = 0; j < g; j++) {
            const WORD a = WORD(i * (g + 1) + j), b = WORD(a + 1), c = WORD(a + g + 1), d = WORD(c + 1);
            idx->insert(idx->end(), {a, c, b, b, c, d});
        }
}

struct Lights {
    D3DLIGHT2 l[3];
};

static Lights MakeLights() {
    Lights s = {};
    for (D3DLIGHT2& l : s.l) {
        l.dwSize = sizeof(D3DLIGHT2);
        l.dwFlags = D3DLIGHT_ACTIVE;
    }
    s.l[0].dltType = D3DLIGHT_DIRECTIONAL;
    s.l[0].dcvColor = {0.6f, 0.5f, 0.4f, 1.0f};
    s.l[0].dvDirection = {0.3f, -0.8f, 0.5f};
    s.l[1].dltType = D3DLIGHT_POINT;
    s.l[1].dcvColor = {0.2f, 0.7f, 0.9f, 1.0f};
    s.l[1].dvPosition = {-1.5f, 1.0f, 2.0f};
    s.l[1].dvRange = 6.0f;
    s.l[1].dvAttenuation0 = 0.5f;
    s.l[1].dvAttenuation1 = 0.4f;
    s.l[1].dvAttenuation2 = 0.1f;
    s.l[2].dltType = D3DLIGHT_SPOT;
    s.l[2].dcvColor = {1.0f, 0.9f, 0.3f, 1.0f};
    s.l[2].dvPosition = {1.5f, 2.0f, 1.0f};
    s.l[2].dvDirection = {-0.5f, -1.0f, 1.0f};
    s.l[2].dvRange = 8.0f;
    s.l[2].dvAttenuation0 = 1.0f;
    s.l[2].dvFalloff = 1.5f;
    s.l[2].dvTheta = 0.4f;
    s.l[2].dvPhi = 1.0f;
    return s;
}

// ---------------------------------------------------------------- Direct3D 6 (Device3)

static int RunD3D6(IDirectDraw4* dd, IDirectDrawSurface4* rt) {
    IDirect3D3* d3d = nullptr;
    if (FAILED(dd->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d)))) return Log("FAIL IDirect3D3"), 1;
    IDirect3DDevice3* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(IID_IDirect3DHALDevice, rt, &dev, nullptr);
    if (FAILED(hr)) return Log("FAIL CreateDevice3 0x%08lx", hr), 1;
    IDirect3DViewport3* vp = nullptr;
    if (FAILED(d3d->CreateViewport(&vp, nullptr))) return Log("FAIL CreateViewport"), 1;
    dev->AddViewport(vp);
    D3DVIEWPORT2 v2 = {sizeof(v2), 0, 0, kW, kH, -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f};
    vp->SetViewport2(&v2);
    dev->SetCurrentViewport(vp);
    Lights ls = MakeLights();
    IDirect3DLight* lights[3] = {};
    for (int i = 0; i < 3; i++) {
        if (FAILED(d3d->CreateLight(&lights[i], nullptr))) return Log("FAIL CreateLight"), 1;
        lights[i]->SetLight(reinterpret_cast<D3DLIGHT*>(&ls.l[i]));
        vp->AddLight(lights[i]);
    }
    IDirect3DMaterial3* mat = nullptr;
    if (FAILED(d3d->CreateMaterial(&mat, nullptr))) return Log("FAIL CreateMaterial"), 1;
    D3DMATERIAL m = {};
    m.dwSize = sizeof(m);
    m.diffuse = {0.8f, 0.8f, 0.8f, 0.75f};
    m.ambient = {0.3f, 0.3f, 0.3f, 1.0f};
    m.specular = {0.9f, 0.9f, 0.9f, 1.0f};
    m.emissive = {0.05f, 0.0f, 0.1f, 0.0f};
    m.power = 12.0f;
    mat->SetMaterial(&m);
    D3DMATERIALHANDLE mh = 0;
    mat->GetHandle(dev, &mh);
    dev->SetLightState(D3DLIGHTSTATE_MATERIAL, mh);
    dev->SetLightState(D3DLIGHTSTATE_AMBIENT, 0x00303040);
    D3DMATRIX world = RotY(0.3f), view = Identity(), proj = Perspective(1.0f, 1.0f, 0.5f, 20.0f);
    view._43 = 4.0f;
    dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &world);
    dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
    dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
    dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_CW);
    dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, TRUE);

    // ProcessVertices: lit, fogged, clipped, a sphere half out of the view.
    std::vector<D3DVERTEX> sv;
    std::vector<WORD> si;
    Sphere(96, 1.2f, 0.4f, 0.0f, &sv, &si);
    const DWORD n = DWORD(sv.size());
    D3DVERTEXBUFFERDESC vd = {sizeof(vd), D3DVBCAPS_SYSTEMMEMORY, D3DFVF_VERTEX, n};
    IDirect3DVertexBuffer *src = nullptr, *dst = nullptr;
    if (FAILED(d3d->CreateVertexBuffer(&vd, &src, 0, nullptr))) return Log("FAIL CreateVertexBuffer"), 1;
    vd.dwFVF = D3DFVF_TLVERTEX;
    if (FAILED(d3d->CreateVertexBuffer(&vd, &dst, 0, nullptr))) return Log("FAIL CreateVertexBuffer"), 1;
    void* p = nullptr;
    DWORD sz = 0;
    src->Lock(DDLOCK_WAIT, &p, &sz);
    memcpy(p, sv.data(), n * sizeof(D3DVERTEX));
    src->Unlock();
    struct Config {
        const char* name;
        DWORD fogMode, specular, lightsOn;
    } configs[] = {{"lit+spec+fog", D3DFOG_LINEAR, TRUE, 3}, {"lit+expfog", D3DFOG_EXP, FALSE, 3},
                   {"dironly", D3DFOG_NONE, FALSE, 1}, {"unlit", D3DFOG_NONE, FALSE, 0}};
    int fails = 0;
    for (const Config& c : configs) {
        dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, c.fogMode != D3DFOG_NONE);
        dev->SetLightState(D3DLIGHTSTATE_FOGMODE, c.fogMode);
        dev->SetLightState(D3DLIGHTSTATE_FOGSTART, Bits(3.0f));
        dev->SetLightState(D3DLIGHTSTATE_FOGEND, Bits(6.0f));
        dev->SetLightState(D3DLIGHTSTATE_FOGDENSITY, Bits(0.2f));
        dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, c.specular);
        for (int i = 0; i < 3; i++) {
            ls.l[i].dwFlags = i < int(c.lightsOn) ? D3DLIGHT_ACTIVE : 0;
            lights[i]->SetLight(reinterpret_cast<D3DLIGHT*>(&ls.l[i]));
        }
        const DWORD op = D3DVOP_TRANSFORM | D3DVOP_CLIP | D3DVOP_EXTENTS | (c.lightsOn ? D3DVOP_LIGHT : 0);
        D3DCLIPSTATUS cs = {D3DCLIPSTATUS_EXTENTS2, 0, 1e9f, -1e9f, 1e9f, -1e9f, 0, 0};
        dev->SetClipStatus(&cs);
        hr = dst->ProcessVertices(op, 0, n, src, 0, dev, 0);
        dst->Lock(DDLOCK_WAIT | DDLOCK_READONLY, &p, &sz);
        uint32_t h = Fnv(p, n * sizeof(D3DTLVERTEX));
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sd3dimbench-tl-%u.bin", g_dir, unsigned(&c - configs));
        if (FILE* f = fopen(path, "wb")) {  // the vertices, for comparing two builds' output
            fwrite(p, sizeof(D3DTLVERTEX), n, f);
            fclose(f);
        }
        dst->Unlock();
        dev->GetClipStatus(&cs);
        h = Fnv(&cs, sizeof(cs), h);
        int iters = 0;
        const double t0 = Now();
        double t1 = t0;
        while (t1 - t0 < g_seconds) {
            for (int k = 0; k < 16; k++) dst->ProcessVertices(op, 0, n, src, 0, dev, 0);
            iters += 16;
            t1 = Now();
        }
        Log("tl %-14s hash %08x  %7.2f Mvert/s (hr 0x%08lx)", c.name, h, double(iters) * n / (t1 - t0) / 1e6, hr);
        if (FAILED(hr)) fails++;
    }

    // The less common paths, hashed only: vertex colours (COLORVERTEX) with specular, a
    // parallel-point light, a spot light with falloff 1, range-based EXP2 fog, without
    // clipping or extents, and odd vertices (a zero normal, NaN, infinities, huge values).
    {
        struct CVert {
            float x, y, z, nx, ny, nz;
            DWORD diffuse, specular;
            float tu, tv;
        };
        std::vector<CVert> cv;
        for (size_t i = 0; i < sv.size(); i += 3) {
            const D3DVERTEX& s = sv[i];
            cv.push_back({s.x, s.y, s.z, s.nx, s.ny, s.nz, 0x80000000u | DWORD(i * 2654435761u >> 8), DWORD(i * 40503u), s.tu, s.tv});
        }
        const float inf = INFINITY, nan = NAN;
        const CVert odd[] = {{0, 0, 0, 0, 0, 0, 0xFFFFFFFF, 0, 0, 0},       {nan, 0, 0, 0, 1, 0, 0xFF00FF00, 0, 0, 0},
                             {0, 0, 0, nan, 0, 0, 0xFF00FF00, 0, 0, 0},      {inf, 1, 1, 0, 0, 1, 0xFF0000FF, 0, 0, 0},
                             {1e30f, -1e30f, 1e30f, 1, 1, 1, 0x12345678, 0, 0, 0}, {-1.5f, 1.0f, -2.0f, 0, 1, 0, 0xFFFFFFFF, 0, 0, 0},
                             {0, 0, -4.0f, 0, 0, -1, 0xFFFFFFFF, 0, 0, 0}};  // (the last two: at a light, at the eye)
        for (int k = 0; k < 3; k++) cv.insert(cv.begin() + 5 + k * 1000, std::begin(odd), std::end(odd));
        cv.insert(cv.end(), std::begin(odd), std::begin(odd) + 3);  // (and among the last, done one by one)
        const DWORD cn = DWORD(cv.size()), cfvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
        D3DVERTEXBUFFERDESC cd = {sizeof(cd), D3DVBCAPS_SYSTEMMEMORY, cfvf, cn};
        IDirect3DVertexBuffer *csrc = nullptr, *cdst = nullptr;
        if (FAILED(d3d->CreateVertexBuffer(&cd, &csrc, 0, nullptr))) return Log("FAIL CreateVertexBuffer"), 1;
        cd.dwFVF = D3DFVF_TLVERTEX;
        if (FAILED(d3d->CreateVertexBuffer(&cd, &cdst, 0, nullptr))) return Log("FAIL CreateVertexBuffer"), 1;
        csrc->Lock(DDLOCK_WAIT, &p, &sz);
        memcpy(p, cv.data(), cn * sizeof(CVert));
        csrc->Unlock();
        D3DLIGHT2 l[3] = {ls.l[0], ls.l[1], ls.l[2]};
        l[0].dltType = D3DLIGHT_PARALLELPOINT;
        l[0].dvPosition = {3.0f, 2.0f, -1.0f};
        l[2].dvFalloff = 1.0f;
        for (int i = 0; i < 3; i++) {
            l[i].dwFlags = D3DLIGHT_ACTIVE;
            lights[i]->SetLight(reinterpret_cast<D3DLIGHT*>(&l[i]));
        }
        dev->SetLightState(D3DLIGHTSTATE_COLORVERTEX, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, TRUE);
        dev->SetLightState(D3DLIGHTSTATE_FOGMODE, D3DFOG_EXP2);
        uint32_t h = 2166136261u;
        for (DWORD op : {DWORD(D3DVOP_TRANSFORM | D3DVOP_LIGHT | D3DVOP_CLIP | D3DVOP_EXTENTS), DWORD(D3DVOP_TRANSFORM | D3DVOP_LIGHT)}) {
            D3DCLIPSTATUS cs = {D3DCLIPSTATUS_EXTENTS2, 0, 1e9f, -1e9f, 1e9f, -1e9f, 0, 0};
            dev->SetClipStatus(&cs);
            hr = cdst->ProcessVertices(op, 0, cn, csrc, 0, dev, 0);
            if (FAILED(hr)) fails++;
            cdst->Lock(DDLOCK_WAIT | DDLOCK_READONLY, &p, &sz);
            h = Fnv(p, cn * sizeof(D3DTLVERTEX), h);
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%sd3dimbench-tl-odd%lu.bin", g_dir, op & D3DVOP_CLIP ? 1ul : 0ul);
            if (FILE* f = fopen(path, "wb")) {
                fwrite(p, sizeof(D3DTLVERTEX), cn, f);
                fclose(f);
            }
            cdst->Unlock();
            dev->GetClipStatus(&cs);
            h = Fnv(&cs, sizeof(cs), h);
        }
        Log("tl odd paths     hash %08x  (%lu vertices, hr 0x%08lx)", h, cn, hr);
        dev->SetLightState(D3DLIGHTSTATE_COLORVERTEX, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, FALSE);
        csrc->Release();
        cdst->Release();
    }

    // DrawIndexedPrimitive of the sphere (lit, fogged, clipped against the view).
    dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE);
    dev->SetLightState(D3DLIGHTSTATE_FOGMODE, D3DFOG_LINEAR);
    dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, TRUE);
    for (int i = 0; i < 3; i++) {
        ls.l[i].dwFlags = D3DLIGHT_ACTIVE;
        lights[i]->SetLight(reinterpret_cast<D3DLIGHT*>(&ls.l[i]));
    }
    auto drawOnce = [&] {
        D3DRECT r = {0, 0, kW, kH};
        vp->Clear2(1, &r, D3DCLEAR_TARGET, 0x00101010, 1.0f, 0);
        dev->BeginScene();
        const double c0 = Now();
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_VERTEX, sv.data(), n, si.data(), DWORD(si.size()), 0);
        g_callTime += Now() - c0;
        dev->EndScene();
    };
    drawOnce();
    const uint32_t hp = Pixels(rt, "dip");
    g_callTime = 0;
    int frames = 0;
    const double t0 = Now();
    double t1 = t0;
    while (t1 - t0 < g_seconds) {
        drawOnce();
        frames++;
        if (!(frames & 7)) Pixels(rt, "dip-t");  // (waits for the GPU now and then)
        t1 = Now();
    }
    Log("dip sphere      hash %08x  %7.1f frames/s, %7.1f us a frame in the call (%lu vertices, %lu triangles)", hp,
        frames / (t1 - t0), g_callTime / frames * 1e6, n, DWORD(si.size() / 3));

    // The same sphere through IDirect3DDevice2, one DrawIndexedPrimitive per row of it (a
    // game's sub-meshes of one vertex array): fewer indices than half the vertices, so only
    // those used are transformed (B §8.1). The clip status after the first row and after
    // all of them is part of the hash.
    IDirect3DDevice2* dev2 = nullptr;
    if (FAILED(dev->QueryInterface(IID_IDirect3DDevice2, reinterpret_cast<void**>(&dev2)))) return Log("FAIL Device2"), 1;
    D3DCLIPSTATUS rowStatus[2] = {};
    auto drawRows = [&] {
        D3DRECT r = {0, 0, kW, kH};
        vp->Clear2(1, &r, D3DCLEAR_TARGET, 0x00101010, 1.0f, 0);
        D3DCLIPSTATUS cs = {D3DCLIPSTATUS_STATUS | D3DCLIPSTATUS_EXTENTS2, 0, 1e9f, -1e9f, 1e9f, -1e9f, 0, 0};
        dev->SetClipStatus(&cs);
        dev->BeginScene();
        const DWORD row = 96 * 6;
        const double c0 = Now();
        for (DWORD at = 0; at < si.size(); at += row) {
            dev2->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DVT_VERTEX, sv.data(), n, si.data() + at, row, 0);
            if (!at) dev->GetClipStatus(&rowStatus[0]);
        }
        g_callTime += Now() - c0;
        dev->GetClipStatus(&rowStatus[1]);
        dev->EndScene();
    };
    drawRows();
    const uint32_t hr2 = Fnv(rowStatus, sizeof(rowStatus), Pixels(rt, "dip-rows"));
    g_callTime = 0;
    frames = 0;
    const double r0 = Now();
    double r1 = r0;
    while (r1 - r0 < g_seconds) {
        drawRows();
        frames++;
        if (!(frames & 7)) Pixels(rt, "dip-rows-t");
        r1 = Now();
    }
    Log("dip rows (dev2) hash %08x status %08lx  %7.1f frames/s, %7.1f us a frame in the calls (%lu draws; first row's "
        "extents %g..%g x %g..%g, all rows' status %08lx)",
        hr2, rowStatus[0].dwStatus, frames / (r1 - r0), g_callTime / frames * 1e6, DWORD(si.size() / (96 * 6)),
        double(rowStatus[0].minx), double(rowStatus[0].maxx), double(rowStatus[0].miny), double(rowStatus[0].maxy),
        rowStatus[1].dwStatus);
    dev2->Release();

    // Many small draws of transformed vertices (a 2D game's sprites, a HUD): 2048 quads a
    // frame, each its own DrawPrimitive, half textured, at two depths and rhw (the per-draw
    // work: binding, the transformed vertices' checks). And one quad with a bad rhw (0) and z
    // past 1, which the checks repair.
    {
        dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        DDSURFACEDESC2 td = {};
        td.dwSize = sizeof(td);
        td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        td.dwWidth = td.dwHeight = 8;
        td.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        td.ddpfPixelFormat.dwSize = sizeof(td.ddpfPixelFormat);
        td.ddpfPixelFormat.dwFlags = DDPF_RGB;
        td.ddpfPixelFormat.dwRGBBitCount = 32;
        td.ddpfPixelFormat.dwRBitMask = 0xFF0000;
        td.ddpfPixelFormat.dwGBitMask = 0xFF00;
        td.ddpfPixelFormat.dwBBitMask = 0xFF;
        IDirectDrawSurface4* ts = nullptr;
        IDirect3DTexture2* tex = nullptr;
        if (SUCCEEDED(dd->CreateSurface(&td, &ts, nullptr))) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            ts->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(l.lpSurface) + y * l.lPitch)[x] =
                        ((x ^ y) & 1) ? 0xFFE0A040u : 0xFF2040C0u;
            ts->Unlock(nullptr);
            ts->QueryInterface(IID_IDirect3DTexture2, reinterpret_cast<void**>(&tex));
        }
        auto frame = [&] {
            D3DRECT r = {0, 0, kW, kH};
            vp->Clear2(1, &r, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            dev->BeginScene();
            const double c0 = Now();
            for (int q = 0; q < 2048; q++) {
                const float x = float((q * 37) % 248), y = float((q * 53) % 248), z = (q & 2) ? 0.25f : 0.5f;
                const float w = (q & 2) ? 1.0f : 0.5f;
                const DWORD c = 0xFF000000u | DWORD(q * 2654435761u >> 8);
                D3DTLVERTEX v[4] = {{x, y, z, w, c, 0, 0, 0}, {x + 8, y, z, w, c, 0, 1, 0},
                                    {x, y + 8, z, w, c, 0, 0, 1}, {x + 8, y + 8, z, w, c, 0, 1, 1}};
                dev->SetTexture(0, (q & 1) ? tex : nullptr);
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, v, 4, 0);
            }
            D3DTLVERTEX bad[4] = {{100, 100, 1.5f, 0, 0xFFFFFFFF, 0, 0, 0}, {140, 100, 1.5f, 0, 0xFFFFFFFF, 0, 1, 0},
                                  {100, 140, -0.5f, 1, 0xFF00FF00, 0, 0, 1}, {140, 140, 2.0f, 1, 0xFF00FF00, 0, 1, 1}};
            dev->SetTexture(0, nullptr);
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, bad, 4, 0);
            g_callTime += Now() - c0;
            dev->EndScene();
        };
        frame();
        const uint32_t hq = Pixels(rt, "quads");
        g_callTime = 0;
        frames = 0;
        const double q0 = Now();
        double q1 = q0;
        while (q1 - q0 < g_seconds) {
            frame();
            frames++;
            if (!(frames & 7)) Pixels(rt, "quads-t");
            q1 = Now();
        }
        Log("quads 2048      hash %08x  %7.1f frames/s, %7.1f us a frame in the calls", hq, frames / (q1 - q0),
            g_callTime / frames * 1e6);
        if (tex) tex->Release();
        if (ts) ts->Release();
    }

    // One colour-keyed 16-bit texture drawn with D3DRENDERSTATE_COLORKEYENABLE on and off by
    // turns, 256 quads a frame (N.I.C.E. 2 switches it around two of its race textures several
    // times a frame). Its left half is the key colour: keyed out with the key on, red with it
    // off. Timed, and checked in a quad of each.
    {
        DDSURFACEDESC2 td = {};
        td.dwSize = sizeof(td);
        td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        td.dwWidth = td.dwHeight = 256;
        td.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        td.ddpfPixelFormat.dwSize = sizeof(td.ddpfPixelFormat);
        td.ddpfPixelFormat.dwFlags = DDPF_RGB;
        td.ddpfPixelFormat.dwRGBBitCount = 16;
        td.ddpfPixelFormat.dwRBitMask = 0xF800;
        td.ddpfPixelFormat.dwGBitMask = 0x07E0;
        td.ddpfPixelFormat.dwBBitMask = 0x001F;
        IDirectDrawSurface4* ts = nullptr;
        IDirect3DTexture2* tex = nullptr;
        if (SUCCEEDED(dd->CreateSurface(&td, &ts, nullptr))) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            ts->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++)
                    reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(l.lpSurface) + y * l.lPitch)[x] = x < 128 ? 0xF800 : 0x07E0;
            ts->Unlock(nullptr);
            DDCOLORKEY key = {0xF800, 0xF800};
            ts->SetColorKey(DDCKEY_SRCBLT, &key);
            ts->QueryInterface(IID_IDirect3DTexture2, reinterpret_cast<void**>(&tex));
        }
        auto frame = [&] {
            D3DRECT r = {0, 0, kW, kH};
            vp->Clear2(1, &r, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            dev->BeginScene();
            dev->SetTexture(0, tex);
            const double c0 = Now();
            for (int q = 0; q < 256; q++) {
                const float x = float((q % 16) * 16), y = float((q / 16) * 16);
                D3DTLVERTEX v[4] = {{x, y, 0.5f, 1, 0xFFFFFFFF, 0, 0, 0}, {x + 16, y, 0.5f, 1, 0xFFFFFFFF, 0, 1, 0},
                                    {x, y + 16, 0.5f, 1, 0xFFFFFFFF, 0, 0, 1}, {x + 16, y + 16, 0.5f, 1, 0xFFFFFFFF, 0, 1, 1}};
                dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, (q & 1) ? FALSE : TRUE);
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, v, 4, 0);
            }
            g_callTime += Now() - c0;
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            dev->SetTexture(0, nullptr);
            dev->EndScene();
        };
        frame();
        const uint32_t hk = Pixels(rt, "keyed");
        DWORD on = 0xFFFFFFFF, off = 0xFFFFFFFF;  // quad 0 (key on) and quad 1 (off), in their left half
        {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            if (SUCCEEDED(rt->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(d.lpSurface) + 8 * d.lPitch);
                on = row[4] & 0xFFFFFF;
                off = row[16 + 4] & 0xFFFFFF;
                rt->Unlock(nullptr);
            }
        }
        const bool right = on == 0 && (off >> 16) >= 0xF0 && (off & 0xFFFF) == 0;
        if (!right) fails++;
        g_callTime = 0;
        frames = 0;
        const double k0 = Now();
        double k1 = k0;
        while (k1 - k0 < g_seconds) {
            frame();
            frames++;
            k1 = Now();
        }
        Log("keyed toggle    hash %08x  %7.1f frames/s, %7.1f us a frame in the calls (key on %06lx, off %06lx)%s", hk,
            frames / (k1 - k0), g_callTime / frames * 1e6, on, off, right ? "" : " FAIL");
        if (tex) tex->Release();
        if (ts) ts->Release();
    }

    src->Release();
    dst->Release();
    mat->Release();
    for (IDirect3DLight* l : lights) {
        vp->DeleteLight(l);
        l->Release();
    }
    dev->DeleteViewport(vp);
    vp->Release();
    dev->Release();
    d3d->Release();
    return fails;
}

// ---------------------------------------------------------------- Direct3D 3 execute buffers

// Writes one instruction header; returns where its records go.
static uint8_t* Ins(uint8_t* at, BYTE op, BYTE size, WORD count) {
    D3DINSTRUCTION* i = reinterpret_cast<D3DINSTRUCTION*>(at);
    i->bOpcode = op;
    i->bSize = size;
    i->wCount = count;
    return at + sizeof(D3DINSTRUCTION);
}

static int RunExecute(IDirectDraw* dd1, IDirectDrawSurface* rt1, IDirectDrawSurface4* rt) {
    IDirect3D* d3d = nullptr;
    if (FAILED(dd1->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d)))) return Log("FAIL IDirect3D"), 1;
    IDirect3DDevice* dev = nullptr;
    HRESULT hr = rt1->QueryInterface(IID_IDirect3DHALDevice, reinterpret_cast<void**>(&dev));
    if (FAILED(hr)) return Log("FAIL Device1 0x%08lx", hr), 1;
    IDirect3DViewport* vp = nullptr;
    if (FAILED(d3d->CreateViewport(&vp, nullptr))) return Log("FAIL CreateViewport"), 1;
    dev->AddViewport(vp);
    D3DVIEWPORT v1 = {};
    v1.dwSize = sizeof(v1);
    v1.dwWidth = kW;
    v1.dwHeight = kH;
    v1.dvScaleX = kW / 2.0f;
    v1.dvScaleY = kH / 2.0f;
    v1.dvMaxX = 1.0f;
    v1.dvMaxY = 1.0f;
    v1.dvMaxZ = 1.0f;
    vp->SetViewport(&v1);
    Lights ls = MakeLights();
    IDirect3DLight* lights[3] = {};
    for (int i = 0; i < 3; i++) {
        D3DLIGHT l = {};
        memcpy(&l, &ls.l[i], sizeof(l));
        l.dwSize = sizeof(D3DLIGHT);
        if (FAILED(d3d->CreateLight(&lights[i], nullptr))) return Log("FAIL CreateLight"), 1;
        lights[i]->SetLight(&l);
        vp->AddLight(lights[i]);
    }
    IDirect3DMaterial* mat = nullptr;
    if (FAILED(d3d->CreateMaterial(&mat, nullptr))) return Log("FAIL CreateMaterial"), 1;
    D3DMATERIAL m = {};
    m.dwSize = sizeof(m);
    m.diffuse = {0.8f, 0.7f, 0.6f, 1.0f};
    m.ambient = {0.3f, 0.3f, 0.3f, 1.0f};
    mat->SetMaterial(&m);
    D3DMATERIALHANDLE mh = 0;
    mat->GetHandle(dev, &mh);
    D3DMATRIXHANDLE hw = 0, hv = 0, hp = 0;
    dev->CreateMatrix(&hw);
    dev->CreateMatrix(&hv);
    dev->CreateMatrix(&hp);
    D3DMATRIX world = RotY(0.5f), view = Identity(), proj = Perspective(1.0f, 1.0f, 0.5f, 20.0f);
    view._43 = 3.5f;
    dev->SetMatrix(hw, &world);
    dev->SetMatrix(hv, &view);
    dev->SetMatrix(hp, &proj);

    std::vector<D3DVERTEX> sv;
    std::vector<WORD> si;
    const int g = 64;
    Sphere(g, 0.6f, 0.2f, 0.0f, &sv, &si);
    const DWORD n = DWORD(sv.size());
    // Three layouts of the same triangles: one TRIANGLE instruction per row (as most games),
    // one per triangle with a render state between every 64 (the worst case), and that again
    // with the sphere cut by the near plane (clipped primitives among the batched ones).
    int fails = 0;
    for (int layout = 0; layout < 3; layout++) {
        D3DMATRIX v = view;
        if (layout == 2) v._43 = 1.2f;  // (the sphere's front half nearer than the near plane, 0.5)
        dev->SetMatrix(hv, &v);
        const DWORD tris = DWORD(si.size() / 3);
        const DWORD bytes = n * sizeof(D3DVERTEX) + 64 * 1024 + tris * (sizeof(D3DTRIANGLE) + 2 * sizeof(D3DINSTRUCTION) + 16);
        D3DEXECUTEBUFFERDESC ed = {sizeof(ed), D3DDEB_BUFSIZE, 0, bytes, nullptr};
        IDirect3DExecuteBuffer* eb = nullptr;
        hr = dev->CreateExecuteBuffer(&ed, &eb, nullptr);
        if (FAILED(hr)) return Log("FAIL CreateExecuteBuffer 0x%08lx", hr), 1;
        eb->Lock(&ed);
        uint8_t* base = static_cast<uint8_t*>(ed.lpData);
        memcpy(base, sv.data(), n * sizeof(D3DVERTEX));
        uint8_t* at = base + n * sizeof(D3DVERTEX);
        const uint8_t* insStart = at;
        at = Ins(at, D3DOP_STATETRANSFORM, sizeof(D3DSTATE), 3);
        D3DSTATE* st = reinterpret_cast<D3DSTATE*>(at);
        st[0].dtstTransformStateType = D3DTRANSFORMSTATE_WORLD, st[0].dwArg[0] = hw;
        st[1].dtstTransformStateType = D3DTRANSFORMSTATE_VIEW, st[1].dwArg[0] = hv;
        st[2].dtstTransformStateType = D3DTRANSFORMSTATE_PROJECTION, st[2].dwArg[0] = hp;
        at += 3 * sizeof(D3DSTATE);
        at = Ins(at, D3DOP_STATELIGHT, sizeof(D3DSTATE), 2);
        st = reinterpret_cast<D3DSTATE*>(at);
        st[0].dlstLightStateType = D3DLIGHTSTATE_MATERIAL, st[0].dwArg[0] = mh;
        st[1].dlstLightStateType = D3DLIGHTSTATE_AMBIENT, st[1].dwArg[0] = 0x00202020;
        at += 2 * sizeof(D3DSTATE);
        at = Ins(at, D3DOP_STATERENDER, sizeof(D3DSTATE), 2);
        st = reinterpret_cast<D3DSTATE*>(at);
        st[0].drstRenderStateType = D3DRENDERSTATE_CULLMODE, st[0].dwArg[0] = layout == 2 ? D3DCULL_NONE : D3DCULL_CW;
        st[1].drstRenderStateType = D3DRENDERSTATE_ZENABLE, st[1].dwArg[0] = FALSE;
        at += 2 * sizeof(D3DSTATE);
        at = Ins(at, D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
        D3DPROCESSVERTICES* pv = reinterpret_cast<D3DPROCESSVERTICES*>(at);
        pv->dwFlags = D3DPROCESSVERTICES_TRANSFORMLIGHT;
        pv->wStart = 0;
        pv->wDest = 0;
        pv->dwCount = n;
        pv->dwReserved = 0;
        at += sizeof(D3DPROCESSVERTICES);
        auto tri = [&](DWORD t) {
            D3DTRIANGLE* r = reinterpret_cast<D3DTRIANGLE*>(at);
            r->v1 = si[3 * t], r->v2 = si[3 * t + 1], r->v3 = si[3 * t + 2];
            r->wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            at += sizeof(D3DTRIANGLE);
        };
        if (layout == 0) {
            for (int row = 0; row < g; row++) {
                // (D3DTRIANGLE records start DWORD-aligned, as games align them.)
                if (reinterpret_cast<uintptr_t>(at) & 3) at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 0);
                at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), WORD(2 * g));
                for (int t = 0; t < 2 * g; t++) tri(DWORD(row * 2 * g + t));
            }
        } else {
            for (DWORD t = 0; t < tris; t++) {
                if (t && !(t % 64)) {
                    at = Ins(at, D3DOP_STATERENDER, sizeof(D3DSTATE), 1);
                    st = reinterpret_cast<D3DSTATE*>(at);
                    st[0].drstRenderStateType = D3DRENDERSTATE_SHADEMODE, st[0].dwArg[0] = D3DSHADE_GOURAUD;
                    at += sizeof(D3DSTATE);
                }
                at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
                tri(t);
            }
        }
        at = Ins(at, D3DOP_EXIT, 0, 0);
        eb->Unlock();
        D3DEXECUTEDATA ex = {sizeof(ex), 0, n, DWORD(insStart - base), DWORD(at - insStart), {}};
        eb->SetExecuteData(&ex);
        auto drawOnce = [&] {
            D3DRECT r = {0, 0, kW, kH};
            vp->Clear(1, &r, D3DCLEAR_TARGET);
            dev->BeginScene();
            const double c0 = Now();
            const HRESULT h = dev->Execute(eb, vp, D3DEXECUTE_CLIPPED);
            g_callTime += Now() - c0;
            dev->EndScene();
            return h;
        };
        hr = drawOnce();
        const char* name = layout == 2 ? "exec-near" : layout ? "exec-pertri" : "exec-rows";
        const uint32_t hpx = Pixels(rt, name);
        D3DEXECUTEDATA back = {sizeof(back)};
        eb->GetExecuteData(&back);
        const uint32_t hs = Fnv(&back.dsStatus, sizeof(back.dsStatus));
        g_callTime = 0;
        int frames = 0;
        const double t0 = Now();
        double t1 = t0;
        while (t1 - t0 < g_seconds) {
            drawOnce();
            frames++;
            if (!(frames & 7)) Pixels(rt, "exec-t");
            t1 = Now();
        }
        Log("%-15s hash %08x status %08x  %7.1f frames/s, %7.1f us a frame in the call (hr 0x%08lx)", name, hpx, hs,
            frames / (t1 - t0), g_callTime / frames * 1e6, hr);
        if (FAILED(hr)) fails++;
        eb->Release();
    }
    // Consecutive TRIANGLE instructions are drawn together; the picture must be the one drawn
    // instruction by instruction (a render state between them draws each alone): a near and
    // a far untextured triangle (rhw 1 and 5e-5: an untextured draw whose rhw differ over
    // 10000 times is drawn with flat rhw), and two triangles with vertices of rhw 0 between
    // them, unused (a draw's bad rhw).
    for (int kase = 0; kase < 2; kase++) {
        const DWORD nv = 103;
        std::vector<D3DTLVERTEX> tv(nv);
        auto put = [&](DWORD i, float x, float y, float rhw, DWORD c) {
            tv[i] = {};
            tv[i].sx = x, tv[i].sy = y, tv[i].sz = 0.5f, tv[i].rhw = rhw, tv[i].color = c, tv[i].specular = 0xFF000000;
        };
        const float farRhw = kase ? 1.0f : 5e-5f;
        put(0, 10, 10, 1.0f, 0xFFFF0000), put(1, 240, 30, 0.5f, 0xFF00FF00), put(2, 30, 240, 0.25f, 0xFF0000FF);
        for (DWORD i = 3; i < 100; i++) put(i, 128, 128, kase ? 0.0f : 1.0f, 0xFFFFFFFF);  // (used by neither)
        put(100, 250, 250, farRhw * 0.5f, 0xFFFFFF00), put(101, 120, 250, farRhw, 0xFF00FFFF), put(102, 250, 120, farRhw * 0.25f, 0xFFFF00FF);
        const WORD tris[2][3] = {{0, 1, 2}, {100, 101, 102}};
        uint32_t hashes[2] = {};
        for (int split = 0; split < 2; split++) {
            const DWORD bytes = nv * sizeof(D3DTLVERTEX) + 1024;
            D3DEXECUTEBUFFERDESC ed = {sizeof(ed), D3DDEB_BUFSIZE, 0, bytes, nullptr};
            IDirect3DExecuteBuffer* eb = nullptr;
            hr = dev->CreateExecuteBuffer(&ed, &eb, nullptr);
            if (FAILED(hr)) return Log("FAIL CreateExecuteBuffer 0x%08lx", hr), fails + 1;
            eb->Lock(&ed);
            uint8_t* base = static_cast<uint8_t*>(ed.lpData);
            memcpy(base, tv.data(), nv * sizeof(D3DTLVERTEX));
            uint8_t* at = base + nv * sizeof(D3DTLVERTEX);
            const uint8_t* insStart = at;
            at = Ins(at, D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            D3DPROCESSVERTICES* pv = reinterpret_cast<D3DPROCESSVERTICES*>(at);
            *pv = {D3DPROCESSVERTICES_COPY, 0, 0, nv, 0};
            at += sizeof(D3DPROCESSVERTICES);
            at = Ins(at, D3DOP_STATERENDER, sizeof(D3DSTATE), 4);
            D3DSTATE* st = reinterpret_cast<D3DSTATE*>(at);
            st[0].drstRenderStateType = D3DRENDERSTATE_CULLMODE, st[0].dwArg[0] = D3DCULL_NONE;
            st[1].drstRenderStateType = D3DRENDERSTATE_ZENABLE, st[1].dwArg[0] = FALSE;
            st[2].drstRenderStateType = D3DRENDERSTATE_TEXTUREHANDLE, st[2].dwArg[0] = 0;
            st[3].drstRenderStateType = D3DRENDERSTATE_SHADEMODE, st[3].dwArg[0] = D3DSHADE_GOURAUD;
            at += 4 * sizeof(D3DSTATE);
            for (int t = 0; t < 2; t++) {
                if (t && split) {
                    at = Ins(at, D3DOP_STATERENDER, sizeof(D3DSTATE), 1);
                    st = reinterpret_cast<D3DSTATE*>(at);
                    st[0].drstRenderStateType = D3DRENDERSTATE_SHADEMODE, st[0].dwArg[0] = D3DSHADE_GOURAUD;
                    at += sizeof(D3DSTATE);
                }
                at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
                D3DTRIANGLE* r = reinterpret_cast<D3DTRIANGLE*>(at);
                r->v1 = tris[t][0], r->v2 = tris[t][1], r->v3 = tris[t][2];
                r->wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
                at += sizeof(D3DTRIANGLE);
            }
            at = Ins(at, D3DOP_EXIT, 0, 0);
            eb->Unlock();
            D3DEXECUTEDATA ex = {sizeof(ex), 0, nv, DWORD(insStart - base), DWORD(at - insStart), {}};
            eb->SetExecuteData(&ex);
            D3DRECT r = {0, 0, kW, kH};
            vp->Clear(1, &r, D3DCLEAR_TARGET);
            dev->BeginScene();
            hr = dev->Execute(eb, vp, D3DEXECUTE_UNCLIPPED);
            dev->EndScene();
            eb->Release();
            char name[32];
            snprintf(name, sizeof(name), "exec-join%d-%s", kase, split ? "split" : "joined");
            hashes[split] = Pixels(rt, name);
            if (FAILED(hr)) fails++;
        }
        const bool same = hashes[0] == hashes[1];
        Log("%-15s joined %08x split %08x %s", kase ? "exec-join-gap" : "exec-join-rhw", hashes[0], hashes[1],
            same ? "same" : "FAIL: differ");
        if (!same) fails++;
    }
    dev->DeleteMatrix(hw);
    dev->DeleteMatrix(hv);
    dev->DeleteMatrix(hp);
    mat->Release();
    for (IDirect3DLight* l : lights) {
        vp->DeleteLight(l);
        l->Release();
    }
    dev->DeleteViewport(vp);
    vp->Release();
    dev->Release();
    d3d->Release();
    return fails;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
    strrchr(g_dir, '\\')[1] = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sd3dimbench.txt", g_dir);
    g_out = fopen(path, "a");
    if (!g_out) return 1;
    sscanf(cmd, "%lf", &g_seconds);
    Log("== d3dimbench");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = "d3dimbench";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "d3dimbench", "d3dimbench", WS_OVERLAPPEDWINDOW, 0, 0, 320, 320, nullptr, nullptr, inst,
                                nullptr);
    IDirectDraw* dd1 = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd1, nullptr))) return Log("FAIL DirectDrawCreate"), 1;
    dd1->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    IDirectDraw4* dd = nullptr;
    dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd));
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = kW;
    d.dwHeight = kH;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    d.ddpfPixelFormat.dwGBitMask = 0xFF00;
    d.ddpfPixelFormat.dwBBitMask = 0xFF;
    IDirectDrawSurface4* rt = nullptr;
    HRESULT hr = dd->CreateSurface(&d, &rt, nullptr);
    if (FAILED(hr)) return Log("FAIL render target 0x%08lx", hr), 1;
    int fails = RunD3D6(dd, rt);
    IDirectDrawSurface* rt1 = nullptr;
    rt->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&rt1));
    fails += RunExecute(dd1, rt1, rt);
    rt1->Release();
    rt->Release();
    dd->Release();
    dd1->Release();
    DestroyWindow(hwnd);
    Log("== %d failure(s)", fails);
    fclose(g_out);
    return fails;
}
