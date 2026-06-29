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

// All offsets verified on `C:\WoW\Octo\WoW.exe` (vanilla 1.12.1, ImageBase
// 0x00400000). The authoritative source is `C:\Git\ClassicAPI\src\Offsets.h`;
// this is the minimal slice the TTS module + its plumbing need.
enum Offsets {
    // ---- Engine init hooks ------------------------------------------------
    // FrameScript Lua-state (re)init. `bool __fastcall()`. Fires on initial
    // boot and on every /reload — invalidate cached event slots here.
    FUN_FRAME_SCRIPT_INITIALIZE = 0x7039E0,
    // Engine "is this function pointer valid" sanity check. Hooked to a
    // no-op so our injected Lua C functions (in MinHook trampoline pages)
    // aren't flagged.
    FUN_INVALID_FUNCTION_PTR_CHECK = 0x42A320,
    // Master in-world Lua function registrar. `void __fastcall()`. After the
    // original runs we fire `Game::RunModuleRegistrations()`.
    FUN_LOAD_SCRIPT_FUNCTIONS = 0x490250,

    // ---- Lua function registration ----------------------------------------
    // Registers a single global Lua function. `__fastcall(name, func)`.
    FUN_FRAMESCRIPT_REGISTER_FUNCTION = 0x00704120,

    // ---- Lua C API (Lua 5.0; fastcall; 16-byte TValue stride) -------------
    // NOTE: `LUA_PUSH_VALUE` and `LUA_REMOVE` are a classic swap trap.
    // `lua_pushvalue` is 0x6F3350 and `lua_remove` is 0x6F30D0 — getting
    // them backwards silently corrupts the globals table (an early
    // ClassicAPI bug). Don't "fix" these from memory.
    LUA_IS_NUMBER = 0x6F34D0,
    LUA_IS_STRING = 0x6F3510,
    LUA_TO_NUMBER = 0x6F3620,
    LUA_TO_STRING = 0x6F3690,
    LUA_PUSH_NUMBER = 0x6F3810,
    LUA_PUSH_NIL = 0x6F37F0,
    LUA_PUSH_STRING = 0x6F3890, // tail-calls pushnil if s == NULL
    LUA_PUSH_VALUE = 0x6F3350,
    LUA_PUSH_CCLOSURE = 0x6F3920,
    LUA_NEW_TABLE = 0x6F3C90,
    LUA_GET_TABLE = 0x6F3A40,
    LUA_SET_TABLE = 0x6F3E20,
    LUA_RAW_SET = 0x6F3EA0,
    LUA_INSERT = 0x6F31A0,
    LUA_SET_TOP = 0x6F3080,
    LUA_TYPE = 0x6F3400, // (L, idx) -> int; 5 = table
    LUA_ERROR = 0x6F4940, // __cdecl variadic; args on stack

    // Global `lua_State *` slot. Read on demand (it changes across /reload).
    VAR_LUA_STATE = 0x00CEEF74,

    // ---- Custom event table + dispatch ------------------------------------
    // `Frame::RegisterEvent` — `__thiscall(this, eventName)`. Hooked so we
    // can claim event-table slots once the engine's table has settled.
    FUN_FRAME_REGISTER_EVENT = 0x00702140,
    // Printf-style event dispatcher. `__cdecl(int eventID, const char *fmt, ...)`.
    FUN_FIRE_EVENT = 0x00703F50,
    // The event-name table: `[VAR_EVENT_TABLE_BASE_PTR]` derefs to a base of
    // 16-byte entries; `[VAR_EVENT_TABLE_COUNT]` is the count. Entry name is
    // at +0x00.
    VAR_EVENT_TABLE_BASE_PTR = 0x00CEEF68,
    VAR_EVENT_TABLE_COUNT = 0x00CEEF64,
    EVENT_ENTRY_STRIDE = 0x10,
    OFF_EVENT_ENTRY_NAME = 0x00,
    // Storm SStrDup — `char *__stdcall(src, file, line)`. Uses SMemAlloc, so
    // names we inject into the event table are freed correctly by the
    // engine's reload teardown (which validates via SMemFree).
    FUN_STORM_SSTRDUP = 0x0064A620,

    // ---- CVar subsystem ---------------------------------------------------
    // Direct cvar lookup — `__fastcall(const char *name) → CVar* | NULL`.
    FUN_FIND_CVAR = 0x0063DEC0,
    // Value string lives at this offset in the CVar struct.
    OFF_CVAR_VALUE_STR = 0x20,
    // Internal registrar. `__fastcall`; ECX=name, EDX=0, then six stack args:
    // flags, defaultValue, changeCallback, categoryId, hiddenBool, userData.
    // Dedups by name; the new-cvar branch forces the archive flag bit on, so
    // script-registered cvars persist to Config.wtf.
    FUN_REGISTER_CVAR = 0x0063DB90,
    // Internal "apply value" — **`__thiscall(cvar /*ecx*/, value, a3, a4, a5,
    // a6)`**. NOT `__fastcall`: declaring it fastcall routes `value` through
    // EDX, pushes only 4 args, and the callee's `RET 0x14` pops 4 bytes too
    // many → ESP corruption → silent crash with no dump. Mirror
    // `Script_SetCVar`: pass a3..a6 = 1, 0, 0, 1.
    FUN_SET_CVAR_VALUE = 0x0063DF50,
    // CVar struct fields (beyond the value string at +0x20).
    OFF_CVAR_FLAGS = 0x1C,
    OFF_CVAR_CALLBACK = 0xBC,
    OFF_CVAR_USERDATA = 0xC0,
    // The categoryId the script registration path passes (cosmetic console
    // grouping); we reuse it so our cvars behave like script-registered ones.
    CVAR_SCRIPT_CATEGORY = 9,
};
