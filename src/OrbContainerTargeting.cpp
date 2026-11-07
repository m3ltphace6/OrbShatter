// Orb Container Targeting -- native DSDLL, runtime-only (no persistent state, nothing written to saves).
//
// Mechanism (reuses the ORIGINAL orb Skrit loop, adds no shot of its own):
//  1. The orb Skrit calls GoMind::GetVisible(QtColl{ENEMY,ALIVE_CONSCIOUS}, GopColl) through FuBi: SkritMachine reads
//     FunctionSpec::m_FunctionPtr at call time (SkritMachine.cpp:599-647). The PE export table is only read ONCE at
//     startup (FuBi.cpp ImportFunction), so patching it afterwards has no effect (proven: 0 Skrit calls seen).
//     We therefore repoint the live FunctionSpec::m_FunctionPtr copies (found by memory scan, runtime only, restored at
//     detach). Native engine code does not use them and is unaffected.
//  2. Inside that window, AIQuery::Is(QtColl) is observed. When the original answer is "no" and the candidate is an
//     admissible destructible, the answer becomes "yes" and the candidate is remembered for a very short time.
//  3. The Skrit then asks Go::HasMind / Go::GetMind about that candidate (again through the export table). For the
//     remembered candidates only, HasMind answers true and GetMind answers with the caster's own mind, so the
//     unmodified Skrit reaches its normal SiegeFX shot (damage, cadence, projectile, mod parameters all original).
// Everything is reverted at detach. Any profile mismatch or already-patched target refuses installation cleanly.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

namespace {

// ---------------- configuration (INI next to the DLL; read once at startup) ----------------
struct Config { bool enabled; bool containers; bool nonContainers; bool log; bool hookHasMind; bool hookGetMind; };
Config g_cfg = { true, true, false, true, true, true };
char g_dir[MAX_PATH] = "";

HANDLE g_log = INVALID_HANDLE_VALUE;
volatile LONG g_lines = 0;
const LONG kMaxLines = 600;

void Log(const char* format, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (InterlockedIncrement(&g_lines) > kMaxLines) return;
    char text[1024]; va_list args; va_start(args, format);
    int n = _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args); if (n < 0) n = (int)strlen(text);
    DWORD written; WriteFile(g_log, text, (DWORD)n, &written, NULL);
}

bool ReadBool(const char* ini, const char* section, const char* key, bool def) {
    char value[32] = "";
    GetPrivateProfileStringA(section, key, "", value, sizeof(value), ini);
    if (!value[0]) return def;
    if (!_stricmp(value, "true") || !_stricmp(value, "1") || !_stricmp(value, "yes") || !_stricmp(value, "on")) return true;
    if (!_stricmp(value, "false") || !_stricmp(value, "0") || !_stricmp(value, "no") || !_stricmp(value, "off")) return false;
    Log("INI_INVALID [%s] %s='%s' -> default %d\r\n", section, key, value, (int)def);
    return def;
}

void LoadConfig() {
    char ini[MAX_PATH]; sprintf_s(ini, "%sOrbContainerTargeting.ini", g_dir);
    DWORD attrs = GetFileAttributesA(ini);
    if (attrs == INVALID_FILE_ATTRIBUTES) { Log("INI_ABSENT defaults used\r\n"); return; }
    g_cfg.enabled = ReadBool(ini, "General", "Enabled", true);
    g_cfg.containers = ReadBool(ini, "Targeting", "AttackContainers", true);
    g_cfg.nonContainers = ReadBool(ini, "Targeting", "AttackNonContainerDestructibles", false);
    g_cfg.log = ReadBool(ini, "Diagnostics", "Log", true);
    g_cfg.hookHasMind = ReadBool(ini, "Experimental", "SubstituteHasMind", true);
    g_cfg.hookGetMind = ReadBool(ini, "Experimental", "SubstituteGetMind", true);
}

// ---------------- engine access ----------------
typedef const char* (__fastcall *NameFn)(const void*, void*);
typedef bool (__fastcall *BoolFn)(const void*, void*);
typedef void* (__fastcall *PtrFn)(void*, void*);
typedef float (__fastcall *FloatFn)(const void*, void*);
typedef bool (__fastcall *IsFn)(void*, void*, const void*, const void*, const void*);
typedef bool (__fastcall *GetVisibleFn)(void*, void*, void*, void*);
typedef bool (__fastcall *LosFn)(const void*, void*, void*);

BYTE* g_engine = NULL;
BYTE* g_tramp = NULL;
BYTE* g_isTarget = NULL;
BYTE g_isOriginal[9];
bool g_isPatched = false;

NameFn g_templateName; BoolFn g_isBreakable, g_hasActor, g_isContainer, g_isInsideInventory, g_isItem;
LosFn g_isLosClear = NULL;   // optional: GoMind::IsLosClear(Go*), same check the orb Skrit applies to each candidate
PtrFn g_getAspect; FloatFn g_getCurrentLife;

const int QT_ALIVE_CONSCIOUS = 13, QT_ENEMY = 47;   // measured in retail (probe, 2026-11-07)

// ---------------- FuBi FunctionSpec interposition (Skrit/FuBi call path only) ----------------
struct SpecPatch { DWORD* slots[4]; int count; DWORD original; DWORD rva; bool active; const char* name; };
SpecPatch g_specGetVisible = { {NULL, NULL, NULL, NULL}, 0, 0, 0x00099082, false, "GoMind::GetVisible(QtColl&,GopColl&)" };
SpecPatch g_specHasMind = { {NULL, NULL, NULL, NULL}, 0, 0, 0x0007c22d, false, "Go::HasMind" };
SpecPatch g_specGetMind = { {NULL, NULL, NULL, NULL}, 0, 0, 0x0007c229, false, "Go::GetMind" };

// Collect addresses of committed read/write DWORDs equal to `value` (outside this DLL's own image).
int ScanValue(DWORD value, DWORD** out, int cap, HMODULE self) {
    int n = 0; MEMORY_BASIC_INFORMATION mbi; BYTE* p = NULL;
    while (n < cap && p < (BYTE*)0x7ff00000 && VirtualQuery(p, &mbi, sizeof(mbi))) {
        BYTE* base = (BYTE*)mbi.BaseAddress; SIZE_T size = mbi.RegionSize;
        bool ours = (BYTE*)self >= base && (BYTE*)self < base + size;
        if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE) && !ours) {
            __try { for (DWORD* q = (DWORD*)base; (BYTE*)(q + 1) <= base + size && n < cap; ++q) if (*q == value) out[n++] = q; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = base + size; if (p <= base) break;
    }
    return n;
}
const int kMaxHits = 16;
DWORD* g_hits[3][kMaxHits]; int g_hitCount[3];
bool LocateSpecs(HMODULE self) {
    SpecPatch* all[3] = { &g_specGetVisible, &g_specHasMind, &g_specGetMind };
    for (int i = 0; i < 3; ++i) {
        DWORD value = (DWORD)(g_engine + all[i]->rva);
        g_hitCount[i] = ScanValue(value, g_hits[i], kMaxHits, self);
        Log("SPEC_SCAN %s value=%08lx hits=%d\r\n", all[i]->name, value, g_hitCount[i]);
        if (i < 2) for (int h = 0; h < g_hitCount[i]; ++h) {
            char line[400]; int len = 0; line[0] = 0;
            __try { for (int k = -6; k <= 10; ++k) len += sprintf_s(line + len, sizeof(line) - len, "%08lx ", g_hits[i][h][k]); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            Log("  HIT %p [-6..+10]: %s\r\n", g_hits[i][h], line);
        }
    }
    // Measured layout (retail dump, 2026-11-07): std::map node {..., key=function address, FunctionSpec{ m_Name(8 bytes),
    // m_Parent, m_Flags, m_FunctionPtr, m_SerialID, ...}}. So m_FunctionPtr is exactly 20 bytes after the key copy and
    // m_SerialID follows it. Stack/low-address hits and anything without a small serial id are rejected.
    const int d = 20;
    for (int i = 0; i < 3; ++i) {
        int matches = 0;
        for (int a = 0; a < g_hitCount[i]; ++a) for (int b = 0; b < g_hitCount[i]; ++b)
            if ((BYTE*)g_hits[i][b] - (BYTE*)g_hits[i][a] == d && (DWORD)g_hits[i][a] > 0x01000000 && g_hits[i][b][1] < 0x10000 && matches < 4) {
                all[i]->slots[matches++] = g_hits[i][b];
                Log("SPEC_LOCATED %s slot=%p serial_id=%lu flags=%08lx\r\n", all[i]->name, g_hits[i][b], g_hits[i][b][1], g_hits[i][b][-1]);
            }
        if (matches < 1) { Log("SPEC_NOT_LOCATED %s\r\n", all[i]->name); return false; }
        all[i]->count = matches; all[i]->original = (DWORD)(g_engine + all[i]->rva);
    }
    return true;
}
bool PatchSpec(SpecPatch& p, void* hook) {
    if (p.count < 1) return false;
    for (int i = 0; i < p.count; ++i) if (*p.slots[i] != p.original) return false;
    for (int i = 0; i < p.count; ++i) *p.slots[i] = (DWORD)hook;
    p.active = true; return true;
}
void RestoreSpec(SpecPatch& p) { if (p.active) for (int i = 0; i < p.count; ++i) *p.slots[i] = p.original; p.active = false; }
// ---------------- runtime state (memory only) ----------------
volatile LONG g_depth = 0;                 // >0 while a Skrit GetVisible is running
enum Category { CAT_NONE = 0, CAT_CONTAINER = 1, CAT_OTHER = 2 };
struct Injected { const void* go; void* casterMind; DWORD tick; int cat; };
const int kMaxInjected = 16; const DWORD kTtlMs = 400;
Injected g_inj[kMaxInjected]; int g_injCount = 0;
LONGLONG g_qpf = 0; LONGLONG g_visTotal = 0, g_visMax = 0, g_modTotal = 0, g_modMax = 0, g_isTotal = 0, g_isMax = 0, g_isCalls = 0, g_lastPerf = 0; LONG g_perfLines = 0;
inline LONGLONG Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
inline double Us(LONGLONG ticks) { return g_qpf ? (double)ticks * 1e6 / (double)g_qpf : 0.0; }
volatile LONG g_nWindows = 0, g_nAccepted = 0, g_nHasMind = 0, g_nGetMind = 0, g_nRefusedCat = 0, g_nPriority = 0;

Injected* FindInjected(const void* go) {
    DWORD now = GetTickCount();
    for (int i = 0; i < g_injCount; ++i) if (g_inj[i].go == go && (now - g_inj[i].tick) < kTtlMs) return &g_inj[i];
    return NULL;
}

Category Classify(const void* go) {
    __try {
        if (!g_isBreakable(go, NULL) || g_hasActor(go, NULL) || g_isInsideInventory(go, NULL)) return CAT_NONE;   // note: barrels report IsItem()=1 in retail, so IsItem is NOT a filter
        void* aspect = g_getAspect(const_cast<void*>(go), NULL);
        if (!aspect || g_getCurrentLife(aspect, NULL) <= 0.0f) return CAT_NONE;
        return g_isContainer(go, NULL) ? CAT_CONTAINER : CAT_OTHER;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return CAT_NONE; }
}

void LogCandidate(const void* go, Category c) {
    static char seen[48][48]; static int ns = 0;
    if (!g_cfg.log || ns >= 48) return;
    char name[48] = "?";
    __try { const char* a = g_templateName(go, NULL); if (a) strncpy_s(name, sizeof(name), a, _TRUNCATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < ns; ++i) if (!strcmp(seen[i], name)) return;
    strcpy_s(seen[ns++], name);
    int act = -1, inv = -1, item = -1, cont = -1, brk = -1; float life = -1.0f;
    __try {
        brk = g_isBreakable(go, NULL); act = g_hasActor(go, NULL); inv = g_isInsideInventory(go, NULL); item = g_isItem(go, NULL); cont = g_isContainer(go, NULL);
        void* as = g_getAspect(const_cast<void*>(go), NULL); if (as) life = g_getCurrentLife(as, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("CANDIDATE go=%s breakable=%d actor=%d inside_inventory=%d item=%d container=%d life=%.1f category=%d\r\n", name, brk, act, inv, item, cont, life, (int)c);
}
// Priority: monsters (natural results of the engine query) > non-container destructibles (cracks, walls) > containers (crates, barrels).
// Applied on the result list of the offensive orb's GetVisible; the Skrit then picks the closest remaining entry.
bool ApplyPriority(void* casterMind, DWORD** vec, int start, bool original) {
    __try {
        DWORD** b = (DWORD**)vec[0]; int total = (int)((DWORD**)vec[1] - b);
        int natural = 0, viable = 0, other = 0, cont = 0;
        for (int i = start; i < total; ++i) {
            Injected* inj = FindInjected(b[i]);
            if (!inj) {
                ++natural;
                // An enemy the orb cannot actually shoot (no clear line of sight) must not hide a destructible: the Skrit would skip it anyway.
                bool los = true;
                if (g_isLosClear) { __try { los = g_isLosClear(casterMind, NULL, b[i]); } __except (EXCEPTION_EXECUTE_HANDLER) { los = true; } }
                if (los) ++viable;
            } else if (inj->cat == CAT_OTHER) ++other; else ++cont;
        }
        int keepCat = viable ? -1 : (other ? CAT_OTHER : CAT_CONTAINER);   // -1 = keep only natural entries
        int w = start, removed = 0;
        for (int i = start; i < total; ++i) {
            Injected* inj = FindInjected(b[i]);
            bool keep = inj ? (keepCat != -1 && inj->cat == keepCat) : true;
            if (keep) b[w++] = b[i]; else ++removed;
        }
        vec[1] = (DWORD*)(b + w);
        if (removed) {
            LONG n = InterlockedIncrement(&g_nPriority);
            if (n <= 6) Log("PRIORITY n=%ld natural=%d viable=%d other=%d containers=%d removed=%d kept=%s\r\n", n, natural, viable, other, cont, removed, viable ? "monsters" : (other ? "non_container_destructibles" : "containers"));
        }
        return w > start || original;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return original; }
}
// ---------------- hooks ----------------
bool __fastcall HkGetVisible(void* self, void* edx, void* q, void* out) {
    LONG w = InterlockedIncrement(&g_nWindows);
    if (w <= 4) { int t0 = -1, t1 = -1, cnt = -1; __try { const int* fq = *(const int* const*)((const BYTE*)q + 4); const int* lq = *(const int* const*)((const BYTE*)q + 8); cnt = (int)(lq - fq); if (cnt > 0) t0 = fq[0]; if (cnt > 1) t1 = fq[1]; } __except (EXCEPTION_EXECUTE_HANDLER) {} Log("SKRIT_GETVISIBLE n=%ld traits=%d:[%d,%d] ret_rva=%08lx\r\n", w, cnt, t0, t1, (unsigned long)((const BYTE*)_ReturnAddress() - g_engine)); }
    g_injCount = 0;
    InterlockedIncrement(&g_depth);
    // GopColl = fast_vector<Go*>: {Go** begin; Go** end}. Layout validated at runtime before any change.
    DWORD** vec = (DWORD**)out; int start = -1;
    __try { if (vec[0] && vec[1] >= vec[0] && (vec[1] - vec[0]) < 4096 && !(((DWORD)vec[0]) & 3)) start = (int)(vec[1] - vec[0]); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    LONGLONG t0 = Qpc();
    bool r = ((GetVisibleFn)g_specGetVisible.original)(self, edx, q, out);
    LONGLONG t1 = Qpc();
    InterlockedDecrement(&g_depth);
    if (start >= 0 && g_injCount > 0) r = ApplyPriority(self, vec, start, r);
    LONGLONG t2 = Qpc();
    g_visTotal += (t1 - t0); if (t1 - t0 > g_visMax) g_visMax = t1 - t0;
    g_modTotal += (t2 - t1); if (t2 - t1 > g_modMax) g_modMax = t2 - t1;
    if (!g_lastPerf) g_lastPerf = t2;
    else if (g_cfg.log && (t2 - g_lastPerf) > g_qpf * 5 && g_perfLines < 40) {
        ++g_perfLines; g_lastPerf = t2;
        Log("PERF windows=%ld GetVisible_engine_total_us=%.0f max_us=%.0f | mod_priority_total_us=%.0f max_us=%.0f | Is_calls=%I64d Is_mod_extra_total_us=%.0f max_us=%.0f\r\n",
            g_nWindows, Us(g_visTotal), Us(g_visMax), Us(g_modTotal), Us(g_modMax), g_isCalls, Us(g_isTotal), Us(g_isMax));
        g_visTotal = g_visMax = g_modTotal = g_modMax = g_isTotal = g_isMax = 0; g_isCalls = 0;
    }
    return r;
}
bool __fastcall HkHasMind(const void* go, void* edx) {
    bool r = ((BoolFn)g_specHasMind.original)(go, edx);
    if (!r && g_injCount) {
        if (Injected* inj = FindInjected(go)) {
            LONG n = InterlockedIncrement(&g_nHasMind); (void)inj;
            if (n <= 3) Log("HASMIND_OVERRIDE n=%ld\r\n", n);
            return true;
        }
    }
    return r;
}
void* __fastcall HkGetMind(void* go, void* edx) {
    void* r = ((PtrFn)g_specGetMind.original)(go, edx);
    if (!r && g_injCount) {
        if (Injected* inj = FindInjected(go)) {
            LONG n = InterlockedIncrement(&g_nGetMind);
            if (n <= 3) Log("GETMIND_OVERRIDE n=%ld\r\n", n);
            return inj->casterMind;
        }
    }
    return r;
}
bool __fastcall HkIsInner(void* self, void* edx, const void* ref, const void* go, const void* q, bool r) { (void)self; (void)edx;
    if (r || g_depth <= 0 || !g_cfg.enabled) return r;
    __try {
        const int* first = *(const int* const*)((const BYTE*)q + 4);
        const int* last = *(const int* const*)((const BYTE*)q + 8);
        if (!first || (last - first) != 2 || first[0] != QT_ENEMY || first[1] != QT_ALIVE_CONSCIOUS) return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return r; }
    Category c = Classify(go);
    __try { if (g_isBreakable(go, NULL)) LogCandidate(go, c); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (c == CAT_NONE) return r;
    if ((c == CAT_CONTAINER && !g_cfg.containers) || (c == CAT_OTHER && !g_cfg.nonContainers)) { InterlockedIncrement(&g_nRefusedCat); return r; }
    void* casterMind = NULL;
    __try { casterMind = ((PtrFn)g_specGetMind.original)(const_cast<void*>(ref), NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { return r; }
    if (!casterMind || g_injCount >= kMaxInjected) return r;
    g_inj[g_injCount].go = go; g_inj[g_injCount].casterMind = casterMind; g_inj[g_injCount].tick = GetTickCount(); g_inj[g_injCount].cat = (int)c; ++g_injCount;
    LONG n = InterlockedIncrement(&g_nAccepted);
    static char acc[48][48]; static int nacc = 0;
    bool firstOfType = false;
    if (g_cfg.log) {
        char nm[48] = "?"; __try { const char* a = g_templateName(go, NULL); if (a) strncpy_s(nm, sizeof(nm), a, _TRUNCATE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        firstOfType = true; for (int i = 0; i < nacc; ++i) if (!strcmp(acc[i], nm)) { firstOfType = false; break; }
        if (firstOfType && nacc < 48) strcpy_s(acc[nacc++], nm); else firstOfType = false;
    }
    if (firstOfType) {
        char name[48] = "?", caster[40] = "?";
        __try {
            const char* a = g_templateName(go, NULL); if (a) strncpy_s(name, sizeof(name), a, _TRUNCATE);
            const char* b = g_templateName(ref, NULL); if (b) strncpy_s(caster, sizeof(caster), b, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("TARGET_ACCEPTED n=%ld go=%s category=%s caster=%s\r\n", n, name, c == CAT_CONTAINER ? "container" : "other_destructible", caster);
    }
    return true;
}

bool __fastcall HkIs(void* self, void* edx, const void* ref, const void* go, const void* q) {
    bool r = ((IsFn)g_tramp)(self, edx, ref, go, q);
    if (g_depth <= 0) return r;
    LONGLONG a = Qpc();
    bool res = HkIsInner(self, edx, ref, go, q, r);
    LONGLONG d = Qpc() - a; g_isTotal += d; if (d > g_isMax) g_isMax = d; ++g_isCalls;
    return res;
}

void UninstallInline() {
    if (!g_isPatched) return; DWORD old;
    if (VirtualProtect(g_isTarget, 9, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(g_isTarget, g_isOriginal, 9); VirtualProtect(g_isTarget, 9, old, &old); FlushInstructionCache(GetCurrentProcess(), g_isTarget, 9);
    }
    g_isPatched = false;
}
void UninstallAll() { UninstallInline(); RestoreSpec(g_specGetVisible); RestoreSpec(g_specHasMind); RestoreSpec(g_specGetMind); }

bool Install(HMODULE engine, HMODULE self) {
    BYTE* fn = (BYTE*)GetProcAddress(engine, "?Is@AIQuery@@QBE_NPBVGo@@0ABUQtColl@@@Z");
    const BYTE expected[9] = {0x8b,0x44,0x24,0x0c,0x8b,0x50,0x04,0x85,0xd2};
    if (!fn || fn != g_engine + 0x1ea909 || memcmp(fn, expected, 9)) { Log("PROFILE_REFUSED Is symbol_or_bytes (another module may already hook it)\r\n"); return false; }
    g_templateName = (NameFn)GetProcAddress(engine, "?GetTemplateName@Go@@QBEPBDXZ");
    g_isBreakable = (BoolFn)GetProcAddress(engine, "?IsBreakable@Go@@QBE_NXZ");
    g_hasActor = (BoolFn)GetProcAddress(engine, "?HasActor@Go@@QBE_NXZ");
    g_isContainer = (BoolFn)GetProcAddress(engine, "?IsContainer@Go@@QBE_NXZ");
    g_isInsideInventory = (BoolFn)GetProcAddress(engine, "?IsInsideInventory@Go@@QBE_NXZ");
    g_isItem = (BoolFn)GetProcAddress(engine, "?IsItem@Go@@QBE_NXZ");
    g_getAspect = (PtrFn)GetProcAddress(engine, "?GetAspect@Go@@QAEPAVGoAspect@@XZ");
    g_isLosClear = (LosFn)GetProcAddress(engine, "?IsLosClear@GoMind@@QBE_NPAVGo@@@Z");
    Log("LOS_EXPORT %s\r\n", g_isLosClear ? "found" : "missing (priority treats every enemy as reachable)");
    g_getCurrentLife = (FloatFn)GetProcAddress(engine, "?GetCurrentLife@GoAspect@@QBEMXZ");
    if (!g_templateName || !g_isBreakable || !g_hasActor || !g_isContainer || !g_isInsideInventory || !g_isItem || !g_getAspect || !g_getCurrentLife) {
        Log("PROFILE_REFUSED helper_exports\r\n"); return false;
    }
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); g_qpf = fq.QuadPart;
    if (!LocateSpecs(self)) return false;
    bool ok = PatchSpec(g_specGetVisible, (void*)&HkGetVisible)
           && (!g_cfg.hookHasMind || PatchSpec(g_specHasMind, (void*)&HkHasMind))
           && (!g_cfg.hookGetMind || PatchSpec(g_specGetMind, (void*)&HkGetMind));    if (!ok) { Log("SPEC_PATCH_FAILED\r\n"); UninstallAll(); return false; }
    g_tramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp) { UninstallAll(); return false; }
    memcpy(g_tramp, fn, 9); g_tramp[9] = 0xE9; *(LONG*)(g_tramp + 10) = (LONG)((fn + 9) - (g_tramp + 14));
    DWORD old;
    if (!VirtualProtect(fn, 9, PAGE_EXECUTE_READWRITE, &old)) { UninstallAll(); return false; }
    memcpy(g_isOriginal, fn, 9); g_isTarget = fn;
    fn[0] = 0xE9; *(LONG*)(fn + 1) = (LONG)((BYTE*)&HkIs - (fn + 5));
    fn[5] = fn[6] = fn[7] = fn[8] = 0x90;
    VirtualProtect(fn, 9, old, &old); FlushInstructionCache(GetCurrentProcess(), fn, 9);
    g_isPatched = true;
    return true;
}

void Attach(HMODULE self) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(self, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\'); if (!slash) return;
    slash[1] = 0; strcpy_s(g_dir, path);
    char logPath[MAX_PATH]; sprintf_s(logPath, "%sOrbContainerTargeting.log", g_dir);
    g_log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LoadConfig();
    if (!g_cfg.log && g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    Log("ATTACH pid=%lu enabled=%d containers=%d non_container_destructibles=%d\r\n", GetCurrentProcessId(), (int)g_cfg.enabled, (int)g_cfg.containers, (int)g_cfg.nonContainers);
    if (!g_cfg.enabled) { Log("DISABLED by INI: no engine change\r\n"); return; }
    HMODULE engine = GetModuleHandleA(NULL); g_engine = (BYTE*)engine;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(engine);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_engine + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != 0x3f8ffe7f || nt->OptionalHeader.SizeOfImage != 0x6ae000) { Log("PROFILE_REFUSED identity (unsupported executable)\r\n"); return; }
    if (Install(engine, self)) Log("INSTALLED inline_is=1 spec_patches=getvisible%s%s\r\n", g_cfg.hookHasMind ? "+hasmind" : "", g_cfg.hookGetMind ? "+getmind" : ""); else Log("NOT_INSTALLED (engine untouched)\r\n");
}

} // namespace

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) Attach(self);
    if (reason == DLL_PROCESS_DETACH) {
        UninstallAll();
        if (g_log != INVALID_HANDLE_VALUE) {
            g_lines = 0;
            Log("SUMMARY skrit_windows=%ld accepted=%ld refused_by_category=%ld hasmind_override=%ld getmind_override=%ld\r\n", g_nWindows, g_nAccepted, g_nRefusedCat, g_nHasMind, g_nGetMind);
            CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
