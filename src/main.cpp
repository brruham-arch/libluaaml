// libluaaml 1.1.0: mod AML (64 bit) + LuaJIT. Skrip per-frame + shell Lua (UI Android native).
#include <amlmod.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <link.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luajit.h>
}
#include "dex_data.h"

MYMOD(net.brruham.libluaaml, libluaaml, 1.1.0, brruham)

struct Cfg { std::string target; std::string scripts = "lua"; int timeout = 5000; int ui = 1; };
struct LibInfo { const char* name; uintptr_t base; size_t size; };

static std::string g_root, g_logpath;
static Cfg g_cfg;
static LibInfo g_lib = {nullptr, 0, 0};
static lua_State* gL = nullptr;
static std::vector<int> g_frames;
static bool g_started = false;
static unsigned g_tick = 0;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "libluaaml", "%s", buf);
    if(!g_logpath.empty())
    {
        FILE* f = fopen(g_logpath.c_str(), "a");
        if(f) { fprintf(f, "%s\n", buf); fclose(f); }
    }
}

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static const char* kDefaultCfg =
    "# target: (opsional) lib yang ditunggu sebelum skrip per-frame dimulai. Kosong = langsung mulai.\n"
    "# Shell tidak butuh target: pakai libs(), lib(\"nama.so\"), maps(), rd*/wr*/scan().\n"
    "# target=librio.so\n"
    "scripts=lua\n"
    "# batas waktu satu eksekusi shell (ms)\n"
    "timeout=5000\n"
    "# 1 = tampilkan UI shell, 0 = matikan\n"
    "ui=1\n";

static void LoadCfg(Cfg& c)
{
    std::string p = g_root + "/config.cfg";
    FILE* f = fopen(p.c_str(), "r");
    if(!f)
    {
        int e = errno;
        if(e == ENOENT)
        {
            FILE* w = fopen(p.c_str(), "w");
            if(w) { fputs(kDefaultCfg, w); fclose(w); Log("[cfg] config.cfg belum ada, default dibuat"); }
            else Log("[cfg] gagal membuat config.cfg: errno=%d (%s)", errno, strerror(errno));
        }
        else Log("[cfg] gagal membuka config.cfg: errno=%d (%s), file TIDAK ditimpa, pakai default", e, strerror(e));
        return;
    }
    char line[512];
    bool first = true;
    while(fgets(line, sizeof(line), f))
    {
        char* s0 = line;
        if(first)
        {
            first = false;
            if((unsigned char)s0[0] == 0xEF && (unsigned char)s0[1] == 0xBB && (unsigned char)s0[2] == 0xBF) s0 += 3;
        }
        std::string s = Trim(s0);
        if(s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if(eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
        if(v.empty()) continue;
        if(k == "target") c.target = v;
        else if(k == "scripts") c.scripts = v;
        else if(k == "timeout") c.timeout = atoi(v.c_str());
        else if(k == "ui") c.ui = atoi(v.c_str());
    }
    fclose(f);
    Log("[cfg] terbaca: target=%s scripts=%s timeout=%d ui=%d",
        c.target.empty() ? "(none)" : c.target.c_str(), c.scripts.c_str(), c.timeout, c.ui);
}

static int PhdrCb(struct dl_phdr_info* info, size_t, void* data)
{
    LibInfo* li = (LibInfo*)data;
    const char* n = info->dlpi_name;
    if(!n || !*n) return 0;
    const char* b = strrchr(n, '/');
    b = b ? b + 1 : n;
    if(strcmp(b, li->name) != 0) return 0;
    uintptr_t maxend = 0;
    for(int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if(p.p_type == PT_LOAD)
        {
            uintptr_t e = p.p_vaddr + p.p_memsz;
            if(e > maxend) maxend = e;
        }
    }
    li->base = info->dlpi_addr;
    li->size = maxend;
    return 1;
}

static std::atomic<bool> g_abort(false);
#include "mem.h"
#include "hooklog.h"

// ===================== Skrip per-frame =====================
static int l_print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string out;
    for(int i = 1; i <= n; ++i)
    {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        if(i > 1) out += "\t";
        out += s ? std::string(s, len) : "nil";
        lua_pop(L, 1);
    }
    Log("[lua] %s", out.c_str());
    return 0;
}

static void RunScript(const std::string& path)
{
    if(luaL_loadfile(gL, path.c_str()) != 0)
    {
        Log("[err] load %s: %s", path.c_str(), lua_tostring(gL, -1));
        lua_pop(gL, 1);
        return;
    }
    lua_newtable(gL);
    lua_pushstring(gL, g_cfg.target.c_str()); lua_setfield(gL, -2, "lib");
    lua_pushnumber(gL, (lua_Number)g_lib.base); lua_setfield(gL, -2, "base");
    lua_pushnumber(gL, (lua_Number)g_lib.size); lua_setfield(gL, -2, "size");
    lua_pushstring(gL, g_root.c_str()); lua_setfield(gL, -2, "root");
    lua_pushstring(gL, path.c_str()); lua_setfield(gL, -2, "script");
    if(lua_pcall(gL, 1, 1, 0) != 0)
    {
        Log("[err] run %s: %s", path.c_str(), lua_tostring(gL, -1));
        lua_pop(gL, 1);
        return;
    }
    if(lua_isfunction(gL, -1))
    {
        g_frames.push_back(luaL_ref(gL, LUA_REGISTRYINDEX));
        Log("[ok] %s (per-frame)", path.c_str());
    }
    else
    {
        lua_pop(gL, 1);
        Log("[ok] %s", path.c_str());
    }
}

static void StartScripts()
{
    std::string dir = g_root + "/" + g_cfg.scripts;
    aml->CreateDirRecursive(dir.c_str());
    gL = luaL_newstate();
    luaL_openlibs(gL);
    lua_register(gL, "print", l_print);
    MemRegister(gL); HookRegister(gL);
    std::string pp = "package.path=\"" + dir + "/?.lua;\"..package.path";
    luaL_dostring(gL, pp.c_str());

    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if(d)
    {
        struct dirent* e;
        while((e = readdir(d)))
        {
            std::string n = e->d_name;
            if(n.size() > 4 && n.compare(n.size() - 4, 4, ".lua") == 0) files.push_back(n);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    Log("%d skrip di %s", (int)files.size(), dir.c_str());
    for(auto& n : files) RunScript(dir + "/" + n);
}

static void RunFrames()
{
    for(size_t i = 0; i < g_frames.size();)
    {
        lua_rawgeti(gL, LUA_REGISTRYINDEX, g_frames[i]);
        if(lua_pcall(gL, 0, 0, 0) != 0)
        {
            Log("[err] frame: %s (dinonaktifkan)", lua_tostring(gL, -1));
            lua_pop(gL, 1);
            luaL_unref(gL, LUA_REGISTRYINDEX, g_frames[i]);
            g_frames.erase(g_frames.begin() + i);
        }
        else ++i;
    }
}

// ===================== Shell Lua =====================
static std::mutex g_shMx;
static lua_State* sL = nullptr;
static std::string g_out;
static std::chrono::steady_clock::time_point g_runStart;

static std::string ToStr(lua_State* L, int idx) // idx harus positif
{
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, idx);
    if(lua_pcall(L, 1, 1, 0) != 0) { lua_pop(L, 1); return "?"; }
    size_t n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    std::string r = s ? std::string(s, n) : "?";
    lua_pop(L, 1);
    return r;
}

static void OutAdd(const std::string& s) { if(g_out.size() < 65536) g_out += s; }

static int sh_print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for(int i = 1; i <= n; ++i) { if(i > 1) line += "\t"; line += ToStr(L, i); }
    OutAdd(line + "\n");
    return 0;
}

static void ShellHook(lua_State* L, lua_Debug*)
{
    if(g_abort.load()) luaL_error(L, "dihentikan");
    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - g_runStart).count();
    if(g_cfg.timeout > 0 && ms > g_cfg.timeout)
        luaL_error(L, "timeout %d ms (loop terlalu lama)", g_cfg.timeout);
}

static void RefreshCtx(lua_State* L)
{
    LibInfo li = {g_cfg.target.c_str(), 0, 0};
    if(!g_cfg.target.empty()) dl_iterate_phdr(PhdrCb, &li);
    lua_newtable(L);
    lua_pushstring(L, g_cfg.target.c_str()); lua_setfield(L, -2, "lib");
    lua_pushnumber(L, (lua_Number)li.base); lua_setfield(L, -2, "base");
    lua_pushnumber(L, (lua_Number)li.size); lua_setfield(L, -2, "size");
    lua_pushstring(L, g_root.c_str()); lua_setfield(L, -2, "root");
    lua_setglobal(L, "ctx");
}

static std::string RunShell(const std::string& code)
{
    std::lock_guard<std::mutex> lk(g_shMx);
    g_out.clear();
    g_abort.store(false);
    if(!sL)
    {
        sL = luaL_newstate();
        luaL_openlibs(sL);
        lua_getglobal(sL, "os"); lua_pushnil(sL); lua_setfield(sL, -2, "exit"); lua_pop(sL, 1);
        lua_register(sL, "print", sh_print);
        MemRegister(sL); HookRegister(sL);
    }
    RefreshCtx(sL);

    int top0 = lua_gettop(sL);
    std::string expr = "return " + code;
    int st = luaL_loadbuffer(sL, expr.c_str(), expr.size(), "=shell");
    if(st != 0)
    {
        lua_pop(sL, 1);
        st = luaL_loadbuffer(sL, code.c_str(), code.size(), "=shell");
    }
    if(st != 0)
    {
        const char* m = lua_tostring(sL, -1);
        OutAdd(std::string("! ") + (m ? m : "?") + "\n");
        lua_settop(sL, top0);
        return g_out;
    }
    g_runStart = std::chrono::steady_clock::now();
    lua_sethook(sL, ShellHook, LUA_MASKCOUNT, 100000);
    int rc = lua_pcall(sL, 0, LUA_MULTRET, 0);
    lua_sethook(sL, nullptr, 0, 0);
    if(rc != 0)
    {
        const char* m = lua_tostring(sL, -1);
        OutAdd(std::string("! ") + (m ? m : "?") + "\n");
    }
    else
    {
        int n = lua_gettop(sL) - top0;
        if(n > 0)
        {
            std::string r = "=>";
            for(int i = 1; i <= n; ++i) r += (i > 1 ? "\t" : " ") + ToStr(sL, top0 + i);
            OutAdd(r + "\n");
        }
    }
    lua_settop(sL, top0);
    return g_out;
}

static jbyteArray ToBytes(JNIEnv* e, const std::string& s)
{
    jbyteArray a = e->NewByteArray((jsize)s.size());
    if(!s.empty()) e->SetByteArrayRegion(a, 0, (jsize)s.size(), (const jbyte*)s.data());
    return a;
}

static jbyteArray N_run(JNIEnv* e, jclass, jbyteArray code)
{
    jsize n = e->GetArrayLength(code);
    std::string c((size_t)n, '\0');
    if(n > 0) e->GetByteArrayRegion(code, 0, n, (jbyte*)&c[0]);
    return ToBytes(e, RunShell(c));
}
static void N_stop(JNIEnv*, jclass) { g_abort.store(true); }
static void N_reset(JNIEnv*, jclass)
{
    g_abort.store(true);
    std::lock_guard<std::mutex> lk(g_shMx);
    if(sL) { lua_close(sL); sL = nullptr; }
    g_abort.store(false);
}
static jbyteArray N_info(JNIEnv* e, jclass)
{
    std::string s = std::string("libluaaml 1.1.0 | ") + LUAJIT_VERSION + " | target=" +
                    (g_cfg.target.empty() ? "(tidak ada)" : g_cfg.target);
    return ToBytes(e, s);
}

// ===================== UI (dex di-embed, dimuat in-memory) =====================
static JavaVM* g_vm = nullptr;
static jclass g_shell = nullptr;
static std::atomic<bool> g_uiReady(false);

static bool ClearExc(JNIEnv* e, const char* what)
{
    if(e->ExceptionCheck())
    {
        e->ExceptionDescribe();
        e->ExceptionClear();
        Log("[ui] exception: %s", what);
        return true;
    }
    return false;
}

static bool LoadShell(JNIEnv* env)
{
    jclass clCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if(!clCls || ClearExc(env, "InMemoryDexClassLoader (butuh Android 8+)")) return false;
    jmethodID ctor = env->GetMethodID(clCls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jclass cl2 = env->FindClass("java/lang/ClassLoader");
    jmethodID sys = env->GetStaticMethodID(cl2, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(cl2, sys);
    if(ClearExc(env, "getSystemClassLoader")) return false;

    jbyteArray arr = env->NewByteArray((jsize)kDexLen);
    env->SetByteArrayRegion(arr, 0, (jsize)kDexLen, (const jbyte*)kDex);
    jclass bbCls = env->FindClass("java/nio/ByteBuffer");
    jmethodID wrap = env->GetStaticMethodID(bbCls, "wrap", "([B)Ljava/nio/ByteBuffer;");
    jobject bb = env->CallStaticObjectMethod(bbCls, wrap, arr);
    if(ClearExc(env, "ByteBuffer.wrap")) return false;

    jobject loader = env->NewObject(clCls, ctor, bb, parent);
    if(!loader || ClearExc(env, "new InMemoryDexClassLoader")) return false;
    jmethodID lc = env->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring nm = env->NewStringUTF("com.brruham.luaaml.Shell");
    jclass sh = (jclass)env->CallObjectMethod(loader, lc, nm);
    if(!sh || ClearExc(env, "loadClass Shell")) return false;

    jclass gsh = (jclass)env->NewGlobalRef(sh);
    JNINativeMethod methods[] = {
        {"nativeRun",   "([B)[B", (void*)N_run},
        {"nativeStop",  "()V",    (void*)N_stop},
        {"nativeReset", "()V",    (void*)N_reset},
        {"nativeInfo",  "()[B",   (void*)N_info},
    };
    if(env->RegisterNatives(gsh, methods, 4) != 0 || ClearExc(env, "RegisterNatives")) return false;
    g_shell = gsh;
    Log("[ui] dex dimuat (%u byte)", kDexLen);
    return true;
}

static void UiBoot(void*)
{
    static bool wasReady = false;
    JNIEnv* env = nullptr;
    if(!g_vm) return;
    if(g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if(g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    }
    if(!g_shell && !LoadShell(env)) return;
    jobject act = aml->GetCurrentActivity();
    ClearExc(env, "GetCurrentActivity");
    jmethodID m = env->GetStaticMethodID(g_shell, "boot", "(Landroid/app/Activity;)Z");
    jboolean ok = env->CallStaticBooleanMethod(g_shell, m, act);
    if(ClearExc(env, "Shell.boot")) ok = JNI_FALSE;
    g_uiReady.store(ok == JNI_TRUE);
    if(ok && !wasReady) { wasReady = true; Log("[ui] panel terpasang"); }
}

static void UiLoop()
{
    int tries = 0;
    for(;;)
    {
        aml->PushToJavaUIThread(UiBoot, nullptr);
        sleep(g_uiReady.load() ? 3 : 1);
        if(!g_uiReady.load() && ++tries == 60)
            Log("[ui] belum terpasang setelah 60 detik (cek logcat: libluaaml-ui)");
    }
}

// ===================== Hook frame =====================
SDECL_HOOKi(SwapBuffers, void* dpy, void* surf)
{
    if(!g_started)
    {
        if(g_cfg.target.empty())
        {
            g_started = true;
            g_lib.base = 0; g_lib.size = 0;
            Log("tanpa target, skrip dimulai tanpa base");
            StartScripts();
        }
        else if((g_tick++ % 60) == 0)
        {
            g_lib.name = g_cfg.target.c_str();
            g_lib.base = 0; g_lib.size = 0;
            dl_iterate_phdr(PhdrCb, &g_lib);
            if(g_lib.base)
            {
                g_started = true;
                Log("target %s base=0x%lx size=0x%lx", g_cfg.target.c_str(), (unsigned long)g_lib.base, (unsigned long)g_lib.size);
                StartScripts();
            }
            else if(g_tick == 1) Log("menunggu lib %s ...", g_cfg.target.c_str());
        }
    }
    else RunFrames();
    return SwapBuffers(dpy, surf);
}

ON_MOD_LOAD()
{
    std::string base = aml->GetAndroidDataPath();
    while(!base.empty() && base.back() == '/') base.pop_back();
    g_root = base + "/libluaaml";
    aml->CreateDirRecursive(g_root.c_str());
    g_logpath = g_root + "/log.txt";
    { FILE* f = fopen(g_logpath.c_str(), "w"); if(f) fclose(f); }
    LoadCfg(g_cfg);
    Log("libluaaml 1.1.0 | target=%s scripts=%s timeout=%d ui=%d root=%s",
        g_cfg.target.empty() ? "(none)" : g_cfg.target.c_str(), g_cfg.scripts.c_str(),
        g_cfg.timeout, g_cfg.ui, g_root.c_str());

    if(g_cfg.ui)
    {
        g_vm = aml->GetJavaVM();
        if(g_vm) std::thread(UiLoop).detach();
        else Log("[ui] JavaVM tidak tersedia");
    }

    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* fn = egl ? dlsym(egl, "eglSwapBuffers") : NULL;
    if(!fn) fn = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if(!fn) { Log("eglSwapBuffers tidak ketemu: %s (skrip per-frame nonaktif, shell tetap jalan)", dlerror()); return; }
    HOOK(SwapBuffers, fn);
    Log("hook eglSwapBuffers terpasang");
}
