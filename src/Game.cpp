// This file is part of VanillaTTS.
//
// VanillaTTS is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// VanillaTTS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// VanillaTTS. If not, see <https://www.gnu.org/licenses/>.

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace Game {

namespace Lua {
// Each entry binds a typed function pointer in `Game::Lua::` to the
// corresponding raw VA in `Offsets`. The X-macro keeps the cast boilerplate
// in one place.
#define VANILLATTS_LUA_BINDINGS(F)              \
    F(IsNumber,    lua_isnumber,    LUA_IS_NUMBER)    \
    F(IsString,    lua_isstring,    LUA_IS_STRING)    \
    F(ToNumber,    lua_tonumber,    LUA_TO_NUMBER)    \
    F(ToString,    lua_tostring,    LUA_TO_STRING)    \
    F(PushNumber,  lua_pushnumber,  LUA_PUSH_NUMBER)  \
    F(PushNil,     lua_pushnil,     LUA_PUSH_NIL)     \
    F(PushString,  lua_pushstring,  LUA_PUSH_STRING)  \
    F(PushValue,   lua_pushvalue,   LUA_PUSH_VALUE)   \
    F(PushCClosure,lua_pushcclosure,LUA_PUSH_CCLOSURE)\
    F(NewTable,    lua_newtable,    LUA_NEW_TABLE)    \
    F(GetTable,    lua_gettable,    LUA_GET_TABLE)    \
    F(SetTable,    lua_settable,    LUA_SET_TABLE)    \
    F(RawSet,      lua_rawset,      LUA_RAW_SET)      \
    F(Insert,      lua_insert,      LUA_INSERT)       \
    F(SetTop,      lua_settop,      LUA_SET_TOP)      \
    F(Type,        lua_type,        LUA_TYPE)         \
    F(Error,       lua_error,       LUA_ERROR)

#define VANILLATTS_BIND_LUA(Name, Typedef, Offset) \
    const Typedef##_t Name = reinterpret_cast<Typedef##_t>(Offsets::Offset);
VANILLATTS_LUA_BINDINGS(VANILLATTS_BIND_LUA)
#undef VANILLATTS_BIND_LUA
#undef VANILLATTS_LUA_BINDINGS

namespace {
using FrameScript_RegisterFunction_t = void(__fastcall *)(const char *name, CFunction func);
} // namespace

void *State() {
    return *reinterpret_cast<void **>(static_cast<uintptr_t>(Offsets::VAR_LUA_STATE));
}

void RegisterGlobalFunction(const char *name, CFunction func) {
    auto fn = reinterpret_cast<FrameScript_RegisterFunction_t>(
        Offsets::FUN_FRAMESCRIPT_REGISTER_FUNCTION);
    fn(name, func);
}

// Looks up `_G[name]`. If absent, creates a fresh table and binds it.
// Leaves the resulting table on top of the stack.
namespace {
void EnsureGlobalTable(void *L, const char *name) {
    PushString(L, name);
    GetTable(L, GLOBALS_INDEX);
    if (Type(L, -1) == TYPE_TABLE)
        return;
    SetTop(L, -2);                 // pop the non-table.        []
    NewTable(L);                   //                           [tbl]
    PushValue(L, -1);              //                           [tbl, tbl]
    PushString(L, name);           //                           [tbl, tbl, name]
    Insert(L, -2);                 //                           [tbl, name, tbl]
    SetTable(L, GLOBALS_INDEX);    // _G[name] = tbl; pops k+v. [tbl]
}
} // namespace

// Registers `func` at `_G[tableName][methodName]`. If the namespace
// doesn't already exist, creates an empty table for it.
void RegisterTableFunction(const char *tableName, const char *methodName, CFunction func) {
    void *L = State();
    if (L == nullptr)
        return;
    EnsureGlobalTable(L, tableName);     // [tbl]
    PushString(L, methodName);           // [tbl, methodName]
    PushCClosure(L, func, 0);            // [tbl, methodName, closure]
    SetTable(L, -3);                     // tbl[m]=c; pops k+v. [tbl]
    SetTop(L, -2);                       // pop tbl. []
}

void SetFieldNumber(void *L, const char *key, double value) {
    PushString(L, key);
    PushNumber(L, value);
    SetTable(L, -3);
}

void SetFieldString(void *L, const char *key, const char *value) {
    PushString(L, key);
    PushString(L, value != nullptr ? value : "");
    SetTable(L, -3);
}
} // namespace Lua

namespace {
ModuleAutoRegister *g_moduleHead = nullptr;
} // namespace

ModuleAutoRegister::ModuleAutoRegister(Fn f) : fn(f), next(g_moduleHead) {
    g_moduleHead = this;
}

void RunModuleRegistrations() {
    for (auto *node = g_moduleHead; node != nullptr; node = node->next)
        node->fn();
}

} // namespace Game
