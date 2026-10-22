// libluaaml: mod AML (64 bit) yang menanam LuaJIT. Lib target murni dari config.
#include <amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

MYMOD(net.brruham.libluaaml, libluaaml, 1.0.0, brruham)

struct Cfg { std::string target = "libGTASA.so"; std::string scripts = "lua"; };
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

static void LoadCfg(Cfg& c)
{
    std::string p = g_root + "/config.cfg";
    FILE* f = fopen(p.c_str(), "r");
    if(!f)
    {
        f = fopen(p.c_str(), "w");
        if(f)
        {
            fputs("# nama file lib game yang jadi target (base address dikirim ke skrip)\n"
                  "target=libGTASA.so\n"
                  "# folder skrip di dalam folder ini; semua *.lua dimuat berurutan\n"
                  "scripts=lua\n", f);
            fclose(f);
        }
        return;
    }
    char line[512];
    while(fgets(line, sizeof(line), f))
    {
        std::string s = Trim(line);
        if(s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if(eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
        if(v.empty()) continue;
        if(k == "target") c.target = v;
        else if(k == "scripts") c.scripts = v;
    }
    fclose(f);
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

SDECL_HOOKi(SwapBuffers, void* dpy, void* surf)
{
    if(!g_started)
    {
        if((g_tick++ % 60) == 0)
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
    Log("libluaaml 1.0.0 | target=%s scripts=%s root=%s", g_cfg.target.c_str(), g_cfg.scripts.c_str(), g_root.c_str());
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* fn = egl ? dlsym(egl, "eglSwapBuffers") : NULL;
    if(!fn) fn = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if(!fn) { Log("eglSwapBuffers tidak ketemu: %s", dlerror()); return; }
    HOOK(SwapBuffers, fn);
    Log("hook eglSwapBuffers terpasang");
}
