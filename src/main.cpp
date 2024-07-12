#include <chrono>
#include <mutex>
#include <thread>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include "lua_headers.h"

// Globals
lua_State* L = nullptr;
std::ofstream f;
int luahandler = 0;
int frozen = 0;

int doprint(lua_State* state) {
    if (f.is_open()) {
        f << luaL_checkstring(state, 1);
        f.flush();
    }
    return 0;
}

void print_handler(lua_State* state) {
    if (!luahandler || !f.is_open()) return;
    f << "\nLua Crash Handler:\n\n";
    lua_rawgeti(state, LUA_REGISTRYINDEX, luahandler);
    lua_pushlightuserdata(state, (void*) &f);
    lua_pushcclosure(state, doprint, 1);
    if (lua_pcall(state, 1, 0, 0))
        f << "[[ERROR IN CRASH HANDLER: " << lua_tostring(state, -1) << "]]";
    f << "\n";
    f.flush();
}

void print_traceback(lua_State* state) {
    if (!f.is_open()) return;
    f << "\nMain Lua stack:\n";
    lua_Debug sar;
    int n = -1;
    while (lua_getstack(state, ++n, &sar)) {
        lua_getinfo(state, "Sln", &sar);
        if (*(sar.what) == 'C')
            f << "#" << n << "\t" << sar.short_src << " in " << sar.name << "()\n";
        else
            f << "#" << n << "\t" << sar.short_src << ":" << sar.currentline << " in " << (sar.namewhat && *(sar.namewhat) ? sar.namewhat : "anonymous") << " "
              << (sar.name ? sar.name : "function") << " <" << sar.linedefined << "-" << sar.lastlinedefined << ">\n";
    }
    f.flush();
}

int dumpstate(lua_State* state, const char* message = "** Segmentation fault occurred **\n") {
    char buffer[64];
    time_t t = time(NULL);
    struct tm now = *localtime(&t);
    snprintf(buffer, sizeof(buffer), "garrysmod/gcrash/luadump-%04d%02d%02d_%02d%02d%02d.txt",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);

    f.open(buffer, std::ios::out);
    if (f.is_open()) {
        f << message << "\n";
        print_traceback(state);
        print_handler(state);
        f.close();
    }
    return 0;
}

int dumpstate_caller(lua_State* state) {
    printf("GCrash >> Dump stack called\n");
    dumpstate(state, "** Dump stack call **\n");
    return 0;
}

void handlesigsegv(int signum, siginfo_t* info, void* context) {
    if (!frozen) {
        printf("GCrash >> Segmentation fault detected\n");
        dumpstate(L, "** Segmentation fault occurred **\n");
    }
    std::abort();
}

int crash(lua_State* state) { *((int*) NULL) = 0; return 0; }

int sethandler(lua_State* state) {
    if (luahandler) luaL_unref(state, LUA_REGISTRYINDEX, luahandler);
    if (lua_isfunction(state, 1)) {
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
};

watchdog_update* upd = nullptr;

int watchdogupdate(lua_State* state) {
    std::lock_guard<std::mutex> lck(upd->mtx);
    upd->last_call = std::chrono::system_clock::now();
    return 0;
}

void watchdog_threadfn() {
    std::unique_lock<std::mutex> lck(upd->mtx);
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
            printf("GCrash >> Server freeze / hang / infinite loop detected\n");
            upd->last_call = std::chrono::system_clock::now();
            frozen = 1;
            dumpstate(L, "** Server freeze / hang / infinite loop **");
            std::abort();
        }
    }
}

int watchdog_ref = 0;

int startwatchdog(lua_State* state) {
    printf("GCrash >> Watchdog started\n");
    if (watchdog_ref) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->sleeping = false;
        lua_pop(state, 1);
        return 0;
    }

    int time = lua_tointeger(state, 1);
    if (time < 10) time = 30;

    upd->period = std::chrono::seconds(time);
    upd->last_call = std::chrono::system_clock::now();
    upd->sleeping = false;
    upd->shutdown.lock();

    watchdog_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    lua_getglobal(state, "timer");
    lua_getfield(state, -1, "Create");
    lua_pushstring(state, "gcrash.watchdog");
    lua_pushinteger(state, time / 3);
    lua_pushinteger(state, 0);
    lua_pushcclosure(state, watchdogupdate, 0);
    lua_call(state, 4, 0);
    lua_pop(state, 1);

    // Start the watchdog thread
    std::thread watchdog{ watchdog_threadfn };
    watchdog.detach();
    return 0;
}

int stopwatchdog(lua_State* state) {
    printf("GCrash >> Watchdog stopped\n");
    if (watchdog_ref) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->sleeping = true;
        lua_pop(state, 1);
    }
    return 0;
}

int destroywatchdog(lua_State* state) {
    printf("GCrash >> Watchdog destroyed\n");
    if (watchdog_ref) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, watchdog_ref);
        luaL_unref(state, LUA_REGISTRYINDEX, watchdog_ref);
        std::lock_guard<std::mutex> lck(upd->mtx);
        upd->shutdown.unlock();
        watchdog_ref = 0;
        lua_pop(state, 1);
    }
    return 0;
}

extern "C" {
    DLL_EXPORT int gmod13_open(lua_State* state) {
        printf("\n-------------------------\n>> GCrash v0.4.2 - VRP <<\n-------------------------\n");
        mkdir("garrysmod/gcrash", 0755);

        L = state;
        luahandler = 0;
        watchdog_ref = 0;

        upd = new watchdog_update{};
        upd->last_call = std::chrono::system_clock::now();
        upd->sleeping = false;

        lua_newtable(state);
        luaD_setcfunction(state, "dumpstate", dumpstate_caller);
        luaD_setcfunction(state, "sethandler", sethandler);
        luaD_setcfunction(state, "startwatchdog", startwatchdog);
        luaD_setcfunction(state, "stopwatchdog", stopwatchdog);
        luaD_setcfunction(state, "destroywatchdog", destroywatchdog);
        luaD_setcfunction(state, "crash", crash);
        lua_setglobal(state, "gcrash");

        struct sigaction action;
        action.sa_sigaction = &handlesigsegv;
        action.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &action, NULL);

        return 0;
    }

    DLL_EXPORT int gmod13_close(lua_State* state) {
        return destroywatchdog(state);
    }
}
