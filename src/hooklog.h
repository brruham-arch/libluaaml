#pragma once
#define HL_N 4
typedef long (*hl_fn)(long,long,long,long,long,long,long,long,long,long);
static hl_fn hl_orig[HL_N];
static long hl_env[HL_N], hl_thiz[HL_N];
static bool hl_on[HL_N];
static long hl_b[HL_N][256][4]; static unsigned hl_n[HL_N], hl_r[HL_N];
template<int N> static long hl_hook(long a0,long a1,long a2,long a3,long a4,long a5,long a6,long a7,long a8,long a9)
{
    hl_env[N]=a0; hl_thiz[N]=a1;
    { unsigned i=hl_n[N]++%256; hl_b[N][i][0]=a2; hl_b[N][i][1]=a3; hl_b[N][i][2]=a4; hl_b[N][i][3]=a5; }
    if(hl_on[N]) __android_log_print(ANDROID_LOG_INFO,"lhook","[%d] %ld %ld %ld %ld %ld %ld %ld %ld",N,a2,a3,a4,a5,a6,a7,a8,a9);
    return hl_orig[N](a0,a1,a2,a3,a4,a5,a6,a7,a8,a9);
}
static void* hl_tab[HL_N]={(void*)hl_hook<0>,(void*)hl_hook<1>,(void*)hl_hook<2>,(void*)hl_hook<3>};
// hook(slot, addr, log) -> true/false
static int l_hook(lua_State* L){
    int s=(int)luaL_checkinteger(L,1); uintptr_t a=(uintptr_t)lua_tonumber(L,2);
    if(s<0||s>=HL_N||!a||hl_orig[s]){ lua_pushboolean(L,0); return 1; }
    hl_on[s]=lua_toboolean(L,3);
    aml->Hook((void*)a,hl_tab[s],(void**)&hl_orig[s]);
    lua_pushboolean(L,hl_orig[s]!=nullptr); return 1;
}
// hooklog(slot, true/false)
static int l_hooklog(lua_State* L){
    int s=(int)luaL_checkinteger(L,1);
    if(s>=0&&s<HL_N) hl_on[s]=lua_toboolean(L,2);
    return 0;
}
// ncall(slot, a2..a9): panggil fungsi asli dengan env/thiz tersimpan
static int l_ncall(lua_State* L){
    int s=(int)luaL_checkinteger(L,1);
    if(s<0||s>=HL_N||!hl_orig[s]||!hl_env[s]) return 0;
    long v[8]={0}; for(int i=0;i<8;i++) v[i]=(long)luaL_optnumber(L,i+2,0);
    hl_orig[s](hl_env[s],hl_thiz[s],v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
    return 0;
}
static int l_hookarg(lua_State* L){ int s=(int)luaL_checkinteger(L,1); if(s<0||s>=HL_N) return 0; lua_pushnumber(L,(double)(hl_env[s]&0xFFFFFFFFFFFFL)); lua_pushnumber(L,(double)(hl_thiz[s]&0xFFFFFFFFFFFFL)); return 2; }
static int l_hookbuf(lua_State* L){
    int s=(int)luaL_checkinteger(L,1); lua_newtable(L);
    if(s<0||s>=HL_N) return 1;
    unsigned n=hl_n[s], r=hl_r[s]; if(n-r>256) r=n-256;
    int k=1;
    for(unsigned i=r;i<n;i++){ lua_newtable(L);
      for(int j=0;j<4;j++){ lua_pushnumber(L,(double)hl_b[s][i%256][j]); lua_rawseti(L,-2,j+1); }
      lua_rawseti(L,-2,k++); }
    hl_r[s]=n; return 1;
}
static void HookRegister(lua_State* L){
    lua_register(L,"hook",l_hook);
    lua_register(L,"hooklog",l_hooklog);
    lua_register(L,"ncall",l_ncall); lua_register(L,"hookarg",l_hookarg); lua_register(L,"hookbuf",l_hookbuf);
}
