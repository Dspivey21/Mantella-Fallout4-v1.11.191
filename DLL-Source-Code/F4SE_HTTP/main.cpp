// F4SE_HTTP — Localhost HTTP client with typed dictionary system for Papyrus
//
// Rebuilt from scratch for Fallout 4 Anniversary Edition (NG).
// Original concept from Mantella's SKSE_HTTP plugin.
//
// Dependencies: CommonLibF4, CPR (libcurl wrapper), nlohmann/json

#include "F4SE/F4SE.h"
#include "F4SE/Interfaces.h"
#include "F4SE/Logger.h"

#include "RE/Fallout.h"

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

using json = nlohmann::json;
using namespace std::literals;

// ============================================================================
// BSFixedString .rdata refcount fix (AE 1.11.191)
//
// On AE, BSFixedString entries can live in .rdata (read-only memory).
// When CommonLibF4's Papyrus callback wrapper converts BSFixedString to
// std::string, it copies the BSFixedString, triggering AddRef (lock inc)
// on the refcount field — which crashes if the entry is in .rdata.
//
// This VEH catches the access violation, makes the page writable, and
// resumes execution. Constrained to only handle faults caused by our DLL
// writing to read-only pages.
// ============================================================================

static std::uintptr_t g_dllBase = 0;
static std::uintptr_t g_dllEnd = 0;

void CacheDLLRange()
{
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&CacheDLLRange),
        &hMod);
    if (!hMod) return;

    g_dllBase = reinterpret_cast<std::uintptr_t>(hMod);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto* ntHeader = reinterpret_cast<IMAGE_NT_HEADERS*>(
        g_dllBase + dosHeader->e_lfanew);
    g_dllEnd = g_dllBase + ntHeader->OptionalHeader.SizeOfImage;
}

LONG WINAPI BSFixedStringRdataVEH(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionInformation[0] != 1)  // 1 = write access
        return EXCEPTION_CONTINUE_SEARCH;

    // Only handle faults where the instruction pointer is inside our DLL
    auto rip = ep->ContextRecord->Rip;
    if (g_dllBase == 0 || rip < g_dllBase || rip >= g_dllEnd)
        return EXCEPTION_CONTINUE_SEARCH;

    // Only handle writes to read-only pages
    auto faultAddr = ep->ExceptionRecord->ExceptionInformation[1];
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(faultAddr), &mbi, sizeof(mbi)) == 0)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE_READ)))
        return EXCEPTION_CONTINUE_SEARCH;

    // Make the page writable so the refcount operation can proceed
    DWORD oldProtect;
    if (VirtualProtect(reinterpret_cast<void*>(faultAddr), 4,
        PAGE_READWRITE, &oldProtect))
    {
        F4SE::log::warn("[VEH] Made .rdata page writable at {:016X} (RIP={:016X})",
            faultAddr, rip);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// Typed Dictionary System
// ============================================================================
//
// Papyrus scripts create dictionaries by handle (int), then set/get typed
// values by string key. The dictionary serializes to JSON for HTTP requests
// and deserializes JSON responses back into dictionaries.

namespace
{
    // A single typed value in a dictionary
    using DictValue = std::variant<
        std::string,
        int,
        float,
        bool,
        int,                      // nested dictionary handle
        std::vector<int>,
        std::vector<float>,
        std::vector<std::string>,
        std::vector<bool>,
        std::vector<int>          // nested dictionary handles array
    >;

    // Type tags to disambiguate the variant (since int and nested-handle are both int)
    enum class ValueType : int {
        String = 0,
        Int,
        Float,
        Bool,
        NestedDict,
        IntArray,
        FloatArray,
        StringArray,
        BoolArray,
        NestedDictArray
    };

    struct TypedEntry {
        ValueType type;
        DictValue value;
    };

    struct TypedDictionary {
        std::unordered_map<std::string, TypedEntry> entries;
    };

    // Global dictionary store
    std::shared_mutex g_dictMutex;
    std::unordered_map<int, TypedDictionary> g_dictionaries;
    std::atomic<int> g_nextHandle{ 1 };

    // HTTP reply store: completed replies waiting for Papyrus to pick up
    std::mutex g_replyMutex;
    int g_replyHandle = -1;   // -1 = no reply waiting
    bool g_replyIsError = false;

    // ========================================================================
    // Dictionary helpers
    // ========================================================================

    int AllocHandle()
    {
        return g_nextHandle.fetch_add(1);
    }

    TypedDictionary* GetDict(int handle)
    {
        auto it = g_dictionaries.find(handle);
        return (it != g_dictionaries.end()) ? &it->second : nullptr;
    }

    // ========================================================================
    // JSON serialization: Dictionary -> JSON
    // ========================================================================

    json DictToJson(int handle)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return json::object();

        json obj = json::object();
        for (auto& [key, entry] : dict->entries) {
            switch (entry.type) {
            case ValueType::String:
                obj[key] = std::get<0>(entry.value);
                break;
            case ValueType::Int:
                obj[key] = std::get<1>(entry.value);
                break;
            case ValueType::Float:
                obj[key] = std::get<2>(entry.value);
                break;
            case ValueType::Bool:
                obj[key] = std::get<3>(entry.value);
                break;
            case ValueType::NestedDict:
            {
                int nestedHandle = std::get<4>(entry.value);
                // Recursive — release our lock, serialize nested, re-lock
                lock.unlock();
                obj[key] = DictToJson(nestedHandle);
                lock.lock();
                // dict pointer may be invalid after re-lock, but we're done with it
                break;
            }
            case ValueType::IntArray:
                obj[key] = std::get<5>(entry.value);
                break;
            case ValueType::FloatArray:
                obj[key] = std::get<6>(entry.value);
                break;
            case ValueType::StringArray:
                obj[key] = std::get<7>(entry.value);
                break;
            case ValueType::BoolArray:
                obj[key] = std::get<8>(entry.value);
                break;
            case ValueType::NestedDictArray:
            {
                auto& handles = std::get<9>(entry.value);
                json arr = json::array();
                lock.unlock();
                for (int h : handles) {
                    arr.push_back(DictToJson(h));
                }
                lock.lock();
                obj[key] = std::move(arr);
                break;
            }
            }
        }
        return obj;
    }

    // ========================================================================
    // JSON deserialization: JSON -> Dictionary (creates new handles)
    // ========================================================================

    int JsonToDict(const json& obj)
    {
        int handle = AllocHandle();
        TypedDictionary dict;

        if (!obj.is_object()) {
            std::unique_lock lock(g_dictMutex);
            g_dictionaries[handle] = std::move(dict);
            return handle;
        }

        for (auto& [key, val] : obj.items()) {
            TypedEntry entry;
            if (val.is_string()) {
                entry.type = ValueType::String;
                entry.value.emplace<0>(val.get<std::string>());
            } else if (val.is_boolean()) {
                entry.type = ValueType::Bool;
                entry.value.emplace<3>(val.get<bool>());
            } else if (val.is_number_integer()) {
                entry.type = ValueType::Int;
                entry.value.emplace<1>(val.get<int>());
            } else if (val.is_number_float()) {
                entry.type = ValueType::Float;
                entry.value.emplace<2>(val.get<float>());
            } else if (val.is_object()) {
                entry.type = ValueType::NestedDict;
                int nested = JsonToDict(val);
                entry.value.emplace<4>(nested);
            } else if (val.is_array() && val.size() > 0) {
                // Determine array type from first element
                auto& first = val[0];
                if (first.is_string()) {
                    entry.type = ValueType::StringArray;
                    entry.value.emplace<7>(val.get<std::vector<std::string>>());
                } else if (first.is_boolean()) {
                    entry.type = ValueType::BoolArray;
                    entry.value.emplace<8>(val.get<std::vector<bool>>());
                } else if (first.is_number_integer()) {
                    entry.type = ValueType::IntArray;
                    entry.value.emplace<5>(val.get<std::vector<int>>());
                } else if (first.is_number_float()) {
                    entry.type = ValueType::FloatArray;
                    entry.value.emplace<6>(val.get<std::vector<float>>());
                } else if (first.is_object()) {
                    entry.type = ValueType::NestedDictArray;
                    std::vector<int> handles;
                    for (auto& item : val) {
                        handles.push_back(JsonToDict(item));
                    }
                    entry.value.emplace<9>(std::move(handles));
                } else {
                    continue; // skip unknown array types
                }
            } else if (val.is_array() && val.size() == 0) {
                // Empty array — default to string array
                entry.type = ValueType::StringArray;
                entry.value.emplace<7>(std::vector<std::string>{});
            } else {
                continue; // skip null or unknown
            }
            dict.entries[key] = std::move(entry);
        }

        std::unique_lock lock(g_dictMutex);
        g_dictionaries[handle] = std::move(dict);
        return handle;
    }

    // ========================================================================
    // HTTP: send request on background thread
    // ========================================================================

    void DoHttpRequest(int dictHandle, int port, std::string route, int timeout)
    {
        try {
            json body = DictToJson(dictHandle);
            // Papyrus engine capitalizes property default strings (e.g. "mantella" -> "Mantella").
            // Mantella v0.13's FastAPI routes are case-sensitive and expect lowercase.
            std::transform(route.begin(), route.end(), route.begin(),
                [](unsigned char c){ return std::tolower(c); });
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/" + route;

            F4SE::log::info("[HTTP] POST {} body={}", url, body.dump().substr(0, 200));

            auto response = cpr::Post(
                cpr::Url{ url },
                cpr::Header{ {"Content-Type", "application/json"}, {"accept", "application/json"} },
                cpr::Body{ body.dump() },
                cpr::ConnectTimeout{ timeout > 0 ? timeout * 1000 : 10000 },
                cpr::Timeout{ timeout > 0 ? timeout * 1000 : 0 }
            );

            if (response.status_code == 200) {
                json replyJson = json::parse(response.text);
                int replyHandle = JsonToDict(replyJson);

                F4SE::log::info("[HTTP] Reply OK, handle={}", replyHandle);

                {
                    std::lock_guard lock(g_replyMutex);
                    g_replyHandle = replyHandle;
                    g_replyIsError = false;
                }
            } else {
                // Store error
                int errHandle = AllocHandle();
                {
                    std::unique_lock lock(g_dictMutex);
                    TypedDictionary errDict;
                    TypedEntry entry;
                    entry.type = ValueType::String;
                    std::string errMsg = "HTTP error: status=" + std::to_string(response.status_code)
                        + " " + response.error.message;
                    entry.value.emplace<0>(errMsg);
                    errDict.entries["error"] = std::move(entry);
                    g_dictionaries[errHandle] = std::move(errDict);
                }

                F4SE::log::warn("[HTTP] Error: status={} msg={}", response.status_code, response.error.message);

                {
                    std::lock_guard lock(g_replyMutex);
                    g_replyHandle = errHandle;
                    g_replyIsError = true;
                }
            }

            // Signal Papyrus by injecting keycode 0x97
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = 0x97;
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            SendInput(1, &input, sizeof(INPUT));
            // Key up
            input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));

        } catch (const std::exception& e) {
            F4SE::log::error("[HTTP] Exception: {}", e.what());

            int errHandle = AllocHandle();
            {
                std::unique_lock lock(g_dictMutex);
                TypedDictionary errDict;
                TypedEntry entry;
                entry.type = ValueType::String;
                entry.value.emplace<0>(std::string("Exception: ") + e.what());
                errDict.entries["error"] = std::move(entry);
                g_dictionaries[errHandle] = std::move(errDict);
            }
            {
                std::lock_guard lock(g_replyMutex);
                g_replyHandle = errHandle;
                g_replyIsError = true;
            }
        }
    }

    // ========================================================================
    // Papyrus native functions
    // ========================================================================

    // --- Dictionary management ---

    int PapyrusCreateDictionary(std::monostate)
    {
        int handle = AllocHandle();
        {
            std::unique_lock lock(g_dictMutex);
            g_dictionaries[handle] = TypedDictionary{};
        }
        F4SE::log::trace("[DICT] Created handle={}", handle);
        return handle;
    }

    void PapyrusClearAllDictionaries(std::monostate)
    {
        std::unique_lock lock(g_dictMutex);
        g_dictionaries.clear();
        F4SE::log::info("[DICT] Cleared all dictionaries");
    }

    // --- Getters ---

    std::string PapyrusGetString(std::monostate, int handle, std::string key, std::string defaultVal)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return defaultVal;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::String) return defaultVal;
        return std::get<0>(it->second.value);
    }

    int PapyrusGetInt(std::monostate, int handle, std::string key, int defaultVal)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return defaultVal;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::Int) return defaultVal;
        return std::get<1>(it->second.value);
    }

    float PapyrusGetFloat(std::monostate, int handle, std::string key, float defaultVal)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return defaultVal;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::Float) return defaultVal;
        return std::get<2>(it->second.value);
    }

    bool PapyrusGetBool(std::monostate, int handle, std::string key, bool defaultVal)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return defaultVal;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::Bool) return defaultVal;
        return std::get<3>(it->second.value);
    }

    int PapyrusGetNestedDictionary(std::monostate, int handle, std::string key, int defaultVal)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return defaultVal;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::NestedDict) return defaultVal;
        return std::get<4>(it->second.value);
    }

    std::vector<int> PapyrusGetIntArray(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return {};
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::IntArray) return {};
        return std::get<5>(it->second.value);
    }

    std::vector<float> PapyrusGetFloatArray(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return {};
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::FloatArray) return {};
        return std::get<6>(it->second.value);
    }

    // NOTE: PapyrusGetStringArray (std::vector<std::string> return) is BROKEN on
    // AE 1.11.191 — CommonLibF4's callback wrapper crashes in the array type
    // lookup (call [rax+0xC8] with garbage pointer). Replaced with element-wise
    // getters below. The native binding is removed; getStringArray is now a
    // Papyrus wrapper in F4SE_HTTP.psc that calls these safe getters.

    int PapyrusGetStringArraySize(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return 0;
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::StringArray) return 0;
        return static_cast<int>(std::get<7>(it->second.value).size());
    }

    std::string PapyrusGetStringArrayElement(std::monostate, int handle, std::string key, int index)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return "";
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::StringArray) return "";
        auto& arr = std::get<7>(it->second.value);
        if (index < 0 || index >= static_cast<int>(arr.size())) return "";
        return arr[index];
    }

    std::vector<bool> PapyrusGetBoolArray(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return {};
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::BoolArray) return {};
        return std::get<8>(it->second.value);
    }

    std::vector<int> PapyrusGetNestedDictionariesArray(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return {};
        auto it = dict->entries.find(key);
        if (it == dict->entries.end() || it->second.type != ValueType::NestedDictArray) return {};
        return std::get<9>(it->second.value);
    }

    // --- Setters ---

    bool PapyrusSetString(std::monostate, int handle, std::string key, std::string value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return false;
        dict->entries[key] = TypedEntry{ ValueType::String, DictValue{std::in_place_index<0>, std::move(value)} };
        return true;
    }

    void PapyrusSetInt(std::monostate, int handle, std::string key, int value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::Int, DictValue{std::in_place_index<1>, value} };
    }

    void PapyrusSetFloat(std::monostate, int handle, std::string key, float value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::Float, DictValue{std::in_place_index<2>, value} };
    }

    void PapyrusSetBool(std::monostate, int handle, std::string key, bool value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::Bool, DictValue{std::in_place_index<3>, value} };
    }

    void PapyrusSetNestedDictionary(std::monostate, int handle, std::string key, int value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::NestedDict, DictValue{std::in_place_index<4>, value} };
    }

    void PapyrusSetIntArray(std::monostate, int handle, std::string key, std::vector<int> value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::IntArray, DictValue{std::in_place_index<5>, std::move(value)} };
    }

    void PapyrusSetFloatArray(std::monostate, int handle, std::string key, std::vector<float> value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::FloatArray, DictValue{std::in_place_index<6>, std::move(value)} };
    }

    bool PapyrusSetStringArray(std::monostate, int handle, std::string key, std::vector<std::string> value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return false;
        dict->entries[key] = TypedEntry{ ValueType::StringArray, DictValue{std::in_place_index<7>, std::move(value)} };
        return true;
    }

    void PapyrusSetBoolArray(std::monostate, int handle, std::string key, std::vector<bool> value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::BoolArray, DictValue{std::in_place_index<8>, std::move(value)} };
    }

    void PapyrusSetNestedDictionariesArray(std::monostate, int handle, std::string key, std::vector<int> value)
    {
        std::unique_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return;
        dict->entries[key] = TypedEntry{ ValueType::NestedDictArray, DictValue{std::in_place_index<9>, std::move(value)} };
    }

    // --- hasKey ---

    bool PapyrusHasKey(std::monostate, int handle, std::string key)
    {
        std::shared_lock lock(g_dictMutex);
        auto* dict = GetDict(handle);
        if (!dict) return false;
        return dict->entries.contains(key);
    }

    // --- GetHandle: returns reply handle, or -1 if no reply waiting ---
    //     Handles >= 100000 indicate error (Mantella convention)

    int PapyrusGetHandle(std::monostate)
    {
        std::lock_guard lock(g_replyMutex);
        if (g_replyHandle == -1) return -1;

        int result = g_replyHandle;
        if (g_replyIsError) {
            result += 100000;  // Mantella error convention
        }
        g_replyHandle = -1;
        g_replyIsError = false;
        return result;
    }

    // --- HTTP request ---

    void PapyrusSendLocalhostHttpRequest(std::monostate, int handle, int port, std::string route, int timeout)
    {
        F4SE::log::info("[HTTP] sendLocalhostHttpRequest handle={} port={} route={} timeout={}", handle, port, route, timeout);

        // Fire and forget on a detached thread
        std::thread(DoHttpRequest, handle, port, std::move(route), timeout).detach();
    }

    // ========================================================================
    // Papyrus registration
    // ========================================================================

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) return false;

        // Script name must match F4SE_HTTP.psc
        constexpr auto script = "F4SE_HTTP"sv;

        // Dictionary management
        a_vm->BindNativeMethod(script, "createDictionary"sv, PapyrusCreateDictionary);
        a_vm->BindNativeMethod(script, "clearAllDictionaries"sv, PapyrusClearAllDictionaries);

        // Getters
        a_vm->BindNativeMethod(script, "getString"sv, PapyrusGetString);
        a_vm->BindNativeMethod(script, "getInt"sv, PapyrusGetInt);
        a_vm->BindNativeMethod(script, "getFloat"sv, PapyrusGetFloat);
        a_vm->BindNativeMethod(script, "getBool"sv, PapyrusGetBool);
        a_vm->BindNativeMethod(script, "getNestedDictionary"sv, PapyrusGetNestedDictionary);
        a_vm->BindNativeMethod(script, "getIntArray"sv, PapyrusGetIntArray);
        a_vm->BindNativeMethod(script, "getFloatArray"sv, PapyrusGetFloatArray);
        // getStringArray: REMOVED — crashes on AE (broken array type lookup)
        // Now implemented as Papyrus wrapper using element-wise getters:
        a_vm->BindNativeMethod(script, "getStringArraySize"sv, PapyrusGetStringArraySize);
        a_vm->BindNativeMethod(script, "getStringArrayElement"sv, PapyrusGetStringArrayElement);
        a_vm->BindNativeMethod(script, "getBoolArray"sv, PapyrusGetBoolArray);
        a_vm->BindNativeMethod(script, "getNestedDictionariesArray"sv, PapyrusGetNestedDictionariesArray);

        // Setters
        a_vm->BindNativeMethod(script, "setString"sv, PapyrusSetString);
        a_vm->BindNativeMethod(script, "setInt"sv, PapyrusSetInt);
        a_vm->BindNativeMethod(script, "setFloat"sv, PapyrusSetFloat);
        a_vm->BindNativeMethod(script, "setBool"sv, PapyrusSetBool);
        a_vm->BindNativeMethod(script, "setNestedDictionary"sv, PapyrusSetNestedDictionary);
        a_vm->BindNativeMethod(script, "setIntArray"sv, PapyrusSetIntArray);
        a_vm->BindNativeMethod(script, "setFloatArray"sv, PapyrusSetFloatArray);
        a_vm->BindNativeMethod(script, "setStringArray"sv, PapyrusSetStringArray);
        a_vm->BindNativeMethod(script, "setBoolArray"sv, PapyrusSetBoolArray);
        a_vm->BindNativeMethod(script, "setNestedDictionariesArray"sv, PapyrusSetNestedDictionariesArray);

        // Utility
        a_vm->BindNativeMethod(script, "hasKey"sv, PapyrusHasKey);
        a_vm->BindNativeMethod(script, "GetHandle"sv, PapyrusGetHandle);

        // HTTP
        a_vm->BindNativeMethod(script, "sendLocalhostHttpRequest"sv, PapyrusSendLocalhostHttpRequest);

        F4SE::log::info("Registered 25 Papyrus native functions on F4SE_HTTP");
        return true;
    }
}

// ============================================================================
// F4SE Plugin entry points
// ============================================================================

extern "C" __declspec(dllexport) constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginVersion({ 1, 0, 0, 0 });
    data.PluginName("F4SE_HTTP");
    data.AuthorName("Rebuilt for NG");
    data.UsesAddressLibrary(true);
    data.IsLayoutDependent(true);
    data.addressIndependence |= (1 << 2);
    data.structureIndependence |= (1 << 2);
    return data;
}();

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);
    F4SE::log::info("F4SE_HTTP v1.0 loaded");

    // Install VEH to fix BSFixedString .rdata refcount crashes on AE
    CacheDLLRange();
    if (g_dllBase != 0) {
        AddVectoredExceptionHandler(1, BSFixedStringRdataVEH);
        F4SE::log::info("[VEH] Installed BSFixedString .rdata fix (DLL range {:016X}-{:016X})",
            g_dllBase, g_dllEnd);
    } else {
        F4SE::log::error("[VEH] Failed to cache DLL range — .rdata fix disabled");
    }

    const auto papyrus = reinterpret_cast<const F4SE::PapyrusInterface*>(
        a_f4se->QueryInterface(F4SE::LoadInterface::kPapyrus));
    if (!papyrus) {
        F4SE::log::critical("couldn't get papyrus interface");
        return false;
    }

    if (!papyrus->Register(RegisterPapyrusFunctions)) {
        F4SE::log::critical("couldn't register papyrus functions");
        return false;
    }

    return true;
}
