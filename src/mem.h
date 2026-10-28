// mem.h: akses memori runtime untuk Lua. Alamat salah -> nil/error, bukan crash.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <ctype.h>
#include <type_traits>

static int g_memFd = -1;
static int MemFd()
{
    if(g_memFd < 0) g_memFd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    return g_memFd;
}
static inline uint64_t Untag(uint64_t a) { return a & 0x00FFFFFFFFFFFFFFULL; }

// alamat: angka atau string "0x..." (pointer bertag 0xB4.. dibersihkan otomatis)
static bool GetAddr(lua_State* L, int i, uint64_t& out)
{
    int t = lua_type(L, i);
    if(t == LUA_TNUMBER)
    {
        double d = lua_tonumber(L, i);
        if(d < 0) return false;
        out = Untag((uint64_t)d);
        return true;
    }
    if(t == LUA_TSTRING)
    {
        const char* s = lua_tostring(L, i);
        char* end = nullptr;
        unsigned long long v = strtoull(s, &end, 0);
        if(end == s) return false;
        out = Untag(v);
        return true;
    }
    return false;
}

static bool RawRead(uint64_t a, void* buf, size_t n)
{
    int fd = MemFd();
    if(fd < 0) return false;
    size_t done = 0;
    while(done < n)
    {
        ssize_t r = pread64(fd, (char*)buf + done, n - done, (off64_t)(a + done));
        if(r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static bool RawWrite(uint64_t a, const void* buf, size_t n)
{
    int fd = MemFd();
    if(fd >= 0)
    {
        ssize_t r = pwrite64(fd, buf, n, (off64_t)a);
        if(r == (ssize_t)n) { __builtin___clear_cache((char*)a, (char*)(a + n)); return true; }
    }
    // cadangan: mprotect (proteksi halaman tetap RWX sesudahnya)
    uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t s = a & ~(pg - 1), e = (a + n + pg - 1) & ~(pg - 1);
    if(mprotect((void*)s, (size_t)(e - s), PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    memcpy((void*)a, buf, n);
    __builtin___clear_cache((char*)a, (char*)(a + n));
    return true;
}

template<typename T> static int L_rd(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    T v;
    if(!RawRead(a, &v, sizeof(T))) { lua_pushnil(L); lua_pushstring(L, "baca gagal"); return 2; }
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

template<typename T> static int L_wr(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    double d = luaL_checknumber(L, 2);
    T v;
    if constexpr(std::is_floating_point<T>::value) v = (T)d; else v = (T)(int64_t)d;
    lua_pushboolean(L, RawWrite(a, &v, sizeof(T)));
    return 1;
}

static int L_rdp(lua_State* L)
{
    uint64_t a, v;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    if(!RawRead(a, &v, 8)) { lua_pushnil(L); lua_pushstring(L, "baca gagal"); return 2; }
    lua_pushnumber(L, (lua_Number)Untag(v));
    return 1;
}

// chain(addr, o1, ..., on): deref di setiap offset kecuali yang terakhir, lalu kembalikan alamat akhir
static int L_chain(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    int n = lua_gettop(L);
    for(int i = 2; i <= n; ++i)
    {
        a += (int64_t)luaL_checknumber(L, i);
        if(i < n)
        {
            uint64_t p;
            if(!RawRead(a, &p, 8)) { lua_pushnil(L); lua_pushstring(L, "rantai putus"); return 2; }
            a = Untag(p);
        }
    }
    lua_pushnumber(L, (lua_Number)a);
    return 1;
}

static int L_rdstr(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    int mx = (int)luaL_optinteger(L, 2, 256);
    if(mx > 4096) mx = 4096;
    std::string s;
    for(int i = 0; i < mx; ++i)
    {
        char c;
        if(!RawRead(a + i, &c, 1))
        {
            if(i == 0) { lua_pushnil(L); lua_pushstring(L, "baca gagal"); return 2; }
            break;
        }
        if(!c) break;
        s.push_back(c);
    }
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

static int L_hex(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    int n = (int)luaL_checkinteger(L, 2);
    if(n < 1) n = 1;
    if(n > 4096) n = 4096;
    std::vector<uint8_t> b(n);
    if(!RawRead(a, b.data(), n)) { lua_pushnil(L); lua_pushstring(L, "baca gagal"); return 2; }
    std::string s;
    char t[4];
    for(int i = 0; i < n; ++i) { snprintf(t, sizeof(t), "%02X ", b[i]); s += t; }
    if(!s.empty()) s.pop_back();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

static bool ParsePat(const char* s, std::vector<int>& pat)
{
    while(*s)
    {
        while(*s == ' ') ++s;
        if(!*s) break;
        if(*s == '?') { pat.push_back(-1); ++s; if(*s == '?') ++s; continue; }
        if(!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return false;
        char t[3] = { s[0], s[1], 0 };
        pat.push_back((int)strtol(t, nullptr, 16));
        s += 2;
    }
    return !pat.empty();
}

static int L_wrhex(lua_State* L)
{
    uint64_t a;
    if(!GetAddr(L, 1, a)) return luaL_error(L, "alamat tidak valid");
    std::vector<int> pat;
    if(!ParsePat(luaL_checkstring(L, 2), pat)) return luaL_error(L, "hex tidak valid");
    std::vector<uint8_t> b;
    for(int v : pat) { if(v < 0) return luaL_error(L, "wildcard tidak boleh di wrhex"); b.push_back((uint8_t)v); }
    lua_pushboolean(L, RawWrite(a, b.data(), b.size()));
    return 1;
}

struct MapRegion { uint64_t s, e; char perm[8]; std::string path; };

static std::vector<MapRegion> ReadMaps()
{
    std::vector<MapRegion> v;
    FILE* f = fopen("/proc/self/maps", "r");
    if(!f) return v;
    char line[1024];
    while(fgets(line, sizeof(line), f))
    {
        unsigned long long s, e, o, ino;
        char perm[8], dev[32], path[512] = "";
        int n = sscanf(line, "%llx-%llx %7s %llx %31s %llu %511[^\n]", &s, &e, perm, &o, dev, &ino, path);
        if(n < 6) continue;
        MapRegion r;
        r.s = s; r.e = e;
        memcpy(r.perm, perm, 8);
        r.path = path;
        v.push_back(r);
    }
    fclose(f);
    return v;
}

static int L_maps(lua_State* L)
{
    const char* flt = luaL_optstring(L, 1, nullptr);
    FILE* f = fopen("/proc/self/maps", "r");
    if(!f) { lua_pushnil(L); return 1; }
    std::string out;
    char line[1024];
    while(fgets(line, sizeof(line), f)) if(!flt || strstr(line, flt)) out += line;
    fclose(f);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int LibsCb(struct dl_phdr_info* info, size_t, void* data)
{
    lua_State* L = (lua_State*)data;
    const char* n = info->dlpi_name;
    if(!n || !*n) return 0;
    const char* b = strrchr(n, '/');
    b = b ? b + 1 : n;
    uintptr_t maxend = 0;
    for(int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if(p.p_type == PT_LOAD) { uintptr_t e = p.p_vaddr + p.p_memsz; if(e > maxend) maxend = e; }
    }
    int idx = (int)lua_objlen(L, -1) + 1;
    lua_newtable(L);
    lua_pushstring(L, b); lua_setfield(L, -2, "name");
    lua_pushstring(L, n); lua_setfield(L, -2, "path");
    lua_pushnumber(L, (lua_Number)info->dlpi_addr); lua_setfield(L, -2, "base");
    lua_pushnumber(L, (lua_Number)maxend); lua_setfield(L, -2, "size");
    lua_rawseti(L, -2, idx);
    return 0;
}

static int L_libs(lua_State* L)
{
    lua_newtable(L);
    dl_iterate_phdr(LibsCb, L);
    return 1;
}

static int L_lib(lua_State* L)
{
    LibInfo li = { luaL_checkstring(L, 1), 0, 0 };
    dl_iterate_phdr(PhdrCb, &li);
    if(!li.base) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)li.base);
    lua_pushnumber(L, (lua_Number)li.size);
    return 2;
}

// scan("AA BB ?? CC" [, "bagian_path_lib"] [, maks_hasil=50]) -> {alamat,...}
static int L_scan(lua_State* L)
{
    std::vector<int> pat;
    if(!ParsePat(luaL_checkstring(L, 1), pat)) return luaL_error(L, "pola tidak valid");
    const char* lib = luaL_optstring(L, 2, nullptr);
    if(lib && !*lib) lib = nullptr;
    int maxr = (int)luaL_optinteger(L, 3, 50);
    size_t m = pat.size();
    const size_t CH = 1 << 20;
    std::vector<uint8_t> buf(CH + m);
    auto t0 = std::chrono::steady_clock::now();
    bool stopped = false;
    int cnt = 0;
    lua_newtable(L);
    for(auto& r : ReadMaps())
    {
        if(stopped || cnt >= maxr) break;
        if(r.perm[0] != 'r') continue;
        if(lib) { if(r.path.find(lib) == std::string::npos) continue; }
        else if(r.path.compare(0, 5, "/dev/") == 0 || r.path.find("[vvar") != std::string::npos
                || r.path.find("[vsyscall") != std::string::npos) continue;
        for(uint64_t a = r.s; a < r.e && cnt < maxr;)
        {
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if(g_abort.load() || (g_cfg.timeout > 0 && ms > g_cfg.timeout)) { stopped = true; break; }
            size_t want = (size_t)std::min<uint64_t>(r.e - a, CH);
            size_t extra = (r.e - a > want) ? (m - 1) : 0;
            size_t n = want + extra;
            if(!RawRead(a, buf.data(), n)) { a += want; continue; }
            for(size_t i = 0; i < want && i + m <= n && cnt < maxr; ++i)
            {
                bool ok = true;
                for(size_t j = 0; j < m; ++j)
                    if(pat[j] >= 0 && buf[i + j] != (uint8_t)pat[j]) { ok = false; break; }
                if(ok) { lua_pushnumber(L, (lua_Number)(a + i)); lua_rawseti(L, -2, ++cnt); }
            }
            a += want;
        }
    }
    if(stopped) { lua_pushstring(L, "dihentikan (timeout/stop), hasil sebagian"); return 2; }
    return 1;
}

static void MemRegister(lua_State* L)
{
#define REG(n, f) lua_pushcfunction(L, f); lua_setglobal(L, n);
    REG("rd8",   L_rd<uint8_t>)
    REG("rd16",  L_rd<uint16_t>)
    REG("rd32",  L_rd<uint32_t>)
    REG("rdi32", L_rd<int32_t>)
    REG("rdf",   L_rd<float>)
    REG("rdd",   L_rd<double>)
    REG("rdp",   L_rdp)
    REG("rdstr", L_rdstr)
    REG("hex",   L_hex)
    REG("wr8",   L_wr<uint8_t>)
    REG("wr16",  L_wr<uint16_t>)
    REG("wr32",  L_wr<uint32_t>)
    REG("wrf",   L_wr<float>)
    REG("wrd",   L_wr<double>)
    REG("wrp",   L_wr<uint64_t>)
    REG("wrhex", L_wrhex)
    REG("chain", L_chain)
    REG("libs",  L_libs)
    REG("lib",   L_lib)
    REG("maps",  L_maps)
    REG("scan",  L_scan)
#undef REG
}
