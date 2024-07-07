#include <chrono>
#include <iomanip>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lua_headers.h"

lua_State* L;
int luahandler;
FILE* f;

int doprint(lua_State* state)
{
    if (f) {
        fprintf(f, "%s", luaL_checkstring(state, 1));
        fflush(f);
    }
    return 0;
}

void print_handler(lua_State* state)
{
    if (!luahandler or !f) return;
    fprintf(f, "\nLua Crash Handler:\n\n");
    lua_rawgeti(state, LUA_REGISTRYINDEX, luahandler);
    lua_pushlightuserdata(state, (void*) f);
    lua_pushcclosure(state, doprint, 1);
    if (lua_pcall(state, 1, 0, 0))
    {
        fprintf(f, "[[ERROR IN CRASH HANDLER: %s]]", lua_tostring(state, -1));
    }
    fprintf(f, "\n");
    fflush(f);
}

void print_traceback(lua_State* state)
{
    if (!f) return;
    fprintf(f, "\nMain Lua stack:\n");
    int n = -1;
    lua_Debug sar;
    while (lua_getstack(state, ++n, &sar)) {
        lua_getinfo(state, "Sln", &sar);
        if (*(sar.what) == 'C') {
            fprintf(f, "#%d\t%s in %s()\n", n, sar.short_src, sar.name);
        } else {
            fprintf(f, "#%d\t%s:%d in %s %s() <%d-%d>\n", n, sar.short_src, sar.currentline, *(sar.namewhat) ? sar.namewhat : "anonymous",
                    sar.name ? sar.name : "function", sar.linedefined, sar.lastlinedefined);
        }
    }
    fflush(f);
}

int dumpstate(lua_State* state) {
    char buffer[64];
    time_t t = time(NULL);
    struct tm& now = *localtime(&t);
    sprintf(buffer, "garrysmod/gcrash/luadump-%04d%02d%02d_%02d%02d%02d.txt",
            now.tm_year + 1900,
            now.tm_mon + 1,
            now.tm_mday,
            now.tm_hour,
            now.tm_min,
            now.tm_sec);
    f = fopen(buffer, "w");
    if (f) {
        fprintf(f, "**Segmentation fault occurred**\n");
        print_traceback(state);
        print_handler(state);
        fclose(f);
    }
    return 0;
}

void handlesigsegv(int signum, siginfo_t* info, void* context)
{
    dumpstate(L);
    abort();
}

int crash(lua_State* state)
{
    *((int*) NULL) = 0;
    return 0;
}

int sethandler(lua_State* state)
{
    if (luahandler) luaL_unref(state, LUA_REGISTRYINDEX, luahandler);
    if (lua_isfunction(state, 1))
    {
        lua_pushvalue(state, 1);
        luahandler = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    return 0;
}

struct watchdog_update {
    std::mutex mtx;
    std::timed_mutex shutdown;
    std::chrono::system_clock::time_point last_call;
    std::chrono::system_clock::duration period;
    bool sleeping;
    lua_State* L;
};

watchdog_update* upd = nullptr;

int watchdogupdate(lua_State* state)
{
    std::lock_guard<std::mutex> lck(upd->mtx);
    upd->last_call = std::chrono::system_clock::now();
    return 0;
}

void watchdog_hookfn(lua_State* state, lua_Debug* ar)
{
    dumpstate(L);
    abort();
}

void watchdog_threadfn()
{
    std::unique_lock<std::mutex> lck(upd->mtx);
    bool panicked = false;
    while (true) {
        auto dowait = upd->last_call + upd->period - std::chrono::system_clock::now();
        lck.unlock();
        if (upd->shutdown.try_lock_for(dowait)) {
            upd->shutdown.unlock();
            return;
        }
        lck.lock();
        if (upd->sleeping) {
            upd->last_call = std::chrono::system_clock::now();
        } else if (upd->last_call + upd->period < std::chrono::system_clock::now()) {
            if (panicked) break;
            lua_sethook(upd->L, watchdog_hookfn, 7, 0);
            upd->last_call = std::chrono::system_clock::now();
            panicked = true;
        }
    }
    dumpstate(upd->L);
    abort();
}

int watchdog_ref;
int startwatchdog(lua_State* state)
{
    if (watchdog_ref) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->sleeping = false;
        lua_pop(state, 1);
        return 0;
    }

    int time = lua_tointeger(state, 1);
    if ( time < 10 )
		time = 30;
    
    upd->period = std::chrono::seconds(time);
    upd->last_call = std::chrono::system_clock::now();
    upd->L = state;
    upd->sleeping = false;
    upd->shutdown.lock();

	watchdog_ref = luaL_ref( state, LUA_REGISTRYINDEX );
    
	lua_getglobal( state, "timer" );
	lua_getfield( state, -1, "Create" );
		lua_pushstring( state, "gcrash.watchdog" );
		lua_pushinteger( state, time / 3 );
		lua_pushinteger( state, 0 );
		lua_pushcclosure(state, watchdogupdate, 0);
		lua_call( state, 4, 0 );
	lua_pop( state, 1 );

    // Start the watchdog thread
    std::thread watchdog{ watchdog_threadfn };
    watchdog.detach();
    return 0;
}


int stopwatchdog(lua_State* state)
{
    if (watchdog_ref)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->sleeping = true;
        lua_pop(state, 1);
    }
    return 0;
}

int destroywatchdog(lua_State* state)
{
    if (watchdog_ref)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        luaL_unref(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->shutdown.unlock();
        watchdog_ref = 0;
        lua_pop(state, 1);
    }
    return 0;
}

DLL_EXPORT int gmod13_open(lua_State* state)
{
    printf("\n-------------------------\n>> GCrash v0.4 - VRP <<\n-------------------------\n");
    mkdir("garrysmod/gcrash", 0755);

    L = state;
    luahandler = 0;
    watchdog_ref = 0;
    
    upd = new watchdog_update{};
    upd->last_call = std::chrono::system_clock::now();
    upd->L = state;
    upd->sleeping = false;

    lua_newtable(state);
    {
        luaD_setcfunction(state, "dumpstate", dumpstate);
        luaD_setcfunction(state, "sethandler", sethandler);
        luaD_setcfunction(state, "startwatchdog", startwatchdog);
        luaD_setcfunction(state, "stopwatchdog", stopwatchdog);
        luaD_setcfunction(state, "destroywatchdog", destroywatchdog);
        luaD_setcfunction(state, "crash", crash);
    }
    lua_setglobal(state, "gcrash");

    struct sigaction action;
    action.sa_sigaction = &handlesigsegv;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);

    return 0;
}

DLL_EXPORT int gmod13_close(lua_State* state)
{
    return destroywatchdog(state);
}
