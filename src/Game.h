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

#pragma once

// Minimal engine glue — the slice the TTS module needs. Trimmed from
// ClassicAPI's `Game.{h,cpp}`: the Lua C API wrappers used by Tts.cpp, the
// global/table function registrars, and the module self-registration list.

namespace Game {

using FrameScript_Initialize_t = bool(__fastcall *)();
using LoadScriptFunctions_t = void(__fastcall *)();

namespace Lua {
using CFunction = int(__fastcall *)(void *L);

// Lua 5.0 pseudo-index used to read/write entries on the globals table.
constexpr int GLOBALS_INDEX = -10001;

// Type tag values returned by `Type()` (lua_type).
constexpr int TYPE_NIL = 0;
constexpr int TYPE_BOOLEAN = 1;
constexpr int TYPE_LIGHTUSERDATA = 2;
constexpr int TYPE_NUMBER = 3;
constexpr int TYPE_STRING = 4;
constexpr int TYPE_TABLE = 5;

using lua_isnumber_t = bool(__fastcall *)(void *L, int index);
using lua_isstring_t = bool(__fastcall *)(void *L, int index);
using lua_tonumber_t = double(__fastcall *)(void *L, int index);
using lua_tostring_t = const char *(__fastcall *)(void *L, int index);
using lua_pushnumber_t = void(__fastcall *)(void *L, double n);
using lua_pushnil_t = void(__fastcall *)(void *L);
using lua_pushstring_t = void(__fastcall *)(void *L, const char *s);
using lua_pushvalue_t = void(__fastcall *)(void *L, int idx);
using lua_pushcclosure_t = void(__fastcall *)(void *L, CFunction fn, int upvals);
using lua_newtable_t = void(__fastcall *)(void *L);
using lua_gettable_t = void(__fastcall *)(void *L, int idx);
using lua_settable_t = void(__fastcall *)(void *L, int idx);
using lua_rawset_t = void(__fastcall *)(void *L, int idx);
using lua_insert_t = void(__fastcall *)(void *L, int idx);
using lua_settop_t = void(__fastcall *)(void *L, int idx);
using lua_type_t = int(__fastcall *)(void *L, int index);
// The 1.12 `lua_error` wrapper is variadic; forward user-supplied strings
// as `Error(L, "%s", msg)` to avoid format-string interpretation.
using lua_error_t = void(__cdecl *)(void *L, const char *fmt, ...);

extern const lua_isnumber_t IsNumber;
extern const lua_isstring_t IsString;
extern const lua_tonumber_t ToNumber;
extern const lua_tostring_t ToString;
extern const lua_pushnumber_t PushNumber;
extern const lua_pushnil_t PushNil;
extern const lua_pushstring_t PushString;
extern const lua_pushvalue_t PushValue;
extern const lua_pushcclosure_t PushCClosure;
extern const lua_newtable_t NewTable;
extern const lua_gettable_t GetTable;
extern const lua_settable_t SetTable;
extern const lua_rawset_t RawSet;
extern const lua_insert_t Insert;
extern const lua_settop_t SetTop;
extern const lua_type_t Type;
extern const lua_error_t Error;

// Returns the global `lua_State *` (read on demand from the engine's global).
void *State();

// Registers a single global Lua function. ABI: `int __fastcall(void *L)`.
void RegisterGlobalFunction(const char *name, CFunction func);

// Registers `func` at `_G[tableName][methodName]`, creating the namespace
// table if it doesn't already exist. This is how the C_*-style APIs bind.
void RegisterTableFunction(const char *tableName, const char *methodName,
                           CFunction func);

// Set `t[key] = value` on the table currently at stack[-1]. NULL strings
// coerce to "". Leaves the table on the stack so calls can chain.
void SetFieldNumber(void *L, const char *key, double value);
void SetFieldString(void *L, const char *key, const char *value);
} // namespace Lua

// Self-registration for API modules. Each module .cpp declares a file-scope
// `static const Game::ModuleAutoRegister _r{&RegisterLuaFunctions};`, which
// chains itself onto a global list at DLL-load time. `RunModuleRegistrations`
// is called once from the LoadScriptFunctions post-hook to fire them all, so
// DllMain.cpp doesn't need to know the modules exist.
struct ModuleAutoRegister {
    using Fn = void (*)();
    explicit ModuleAutoRegister(Fn fn);
    Fn fn;
    ModuleAutoRegister *next;
};

void RunModuleRegistrations();

} // namespace Game
