// TopicInfoPatcher — Rebuilt for AE/NG v0.9.8
//
// Original plugin by peteben.
// Rebuilt for AE 1.11.191 using CommonLibF4 NG + Address Library.
//
// v0.9.8: CommonLibF4 NG's hardcoded Address Library IDs for singletons
// (2689028, 2690301, 2690919, 2689178) do NOT exist in the 1.11.191
// Address Library database — they were valid only for 1.10.980/984.
// This version bypasses CommonLibF4's broken GetSingleton() calls with
// direct Address Library ID lookups using the CORRECT IDs found through
// binary analysis of the AE 1.11.191 executable.
//
// SEH wrappers MUST be __declspec(noinline) — if MSVC inlines them into
// callers that have C++ objects (std::string, lock_guard, etc.), the SEH
// handler is silently dropped and crashes are not caught.

#include "F4SE/F4SE.h"
#include "F4SE/Interfaces.h"
#include "F4SE/Logger.h"

#include "RE/Fallout.h"

#include <Windows.h>
#include <unordered_map>
#include <mutex>
#include <cctype>

using namespace std::literals;

namespace
{
    // =========================================================================
    // Pointer validation — reject obviously-garbage pointers
    // =========================================================================

    bool IsPlausiblePointer(const void* ptr)
    {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
        return (mbi.State == MEM_COMMIT);
    }

    // =========================================================================
    // Direct singleton resolution — bypass CommonLibF4's broken IDs
    //
    // CommonLibF4 NG uses these AE IDs which DO NOT EXIST in 1.11.191:
    //   PlayerCharacter:       2690919 (was valid in 1.10.980/984)
    //   UI:                    2689028
    //   GameSettingCollection: 2690301
    //   TESForm::GetAllForms:  2689178
    //
    // Correct IDs found via binary analysis of Fallout4.exe AE 1.11.191:
    //   PlayerCharacter:       4798212 (data RVA 0x31E2D50, confirmed via
    //                          vtable trace from RTTI)
    //
    // For UI, GameSettingCollection, and TESForm::GetAllForms, the correct
    // IDs have not yet been confirmed. This version includes a runtime
    // diagnostic scanner that logs candidate addresses on first use.
    // =========================================================================

    // Cached singleton pointers (resolved once at first use)
    RE::PlayerCharacter* g_cachedPlayer = nullptr;
    RE::UI* g_cachedUI = nullptr;
    RE::GameSettingCollection* g_cachedSettings = nullptr;

    // GetAllForms — REMOVED: form ID lookup causes crashes on AE 1.11.191.
    // All form references are now passed directly from Papyrus.

    bool g_singletonsResolved = false;

    // =========================================================================
    // Runtime singleton scanner
    //
    // Scans the loaded Fallout4.exe .text section for RIP-relative
    // instructions that reference .data addresses. Uses heuristics and
    // validation to identify singleton pointers.
    // =========================================================================

    struct ModuleInfo {
        std::uintptr_t base;
        std::uintptr_t textStart;
        std::size_t    textSize;
        std::uintptr_t dataStart;
        std::size_t    dataSize;
        bool           valid;
    };

    __declspec(noinline) ModuleInfo GetModuleLayout()
    {
        ModuleInfo info{};
        __try {
            info.base = REL::Module::get().base();
            auto seg = REL::Module::get().segment(REL::Segment::text);
            info.textStart = seg.address();
            info.textSize = seg.size();
            // .data section: known from PE analysis
            info.dataStart = info.base + 0x2ECD000;
            info.dataSize = 0xF92330;
            info.valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            info.valid = false;
        }
        return info;
    }

    // Scan .data for a pointer that looks like a valid object with a vtable
    // in .rdata. Returns the address of the .data global (not the object).
    __declspec(noinline) void* ScanForSingletonByID(std::uint64_t addrLibID)
    {
        __try {
            REL::Relocation<void**> singleton{ REL::ID(addrLibID) };
            void* ptr = *singleton;
            if (ptr && IsPlausiblePointer(ptr)) {
                return ptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return nullptr;
    }

    // Try multiple candidate IDs, return the first one that resolves to a
    // plausible pointer that isn't a duplicate of an already-resolved singleton.
    // Logs ALL candidates for diagnostics.
    __declspec(noinline) void* TryCandidateIDs(const std::uint64_t* ids,
        std::size_t count, const char* name,
        const void* exclude1 = nullptr, const void* exclude2 = nullptr)
    {
        void* firstValid = nullptr;
        std::uint64_t firstValidID = 0;
        for (std::size_t i = 0; i < count; ++i) {
            void* result = ScanForSingletonByID(ids[i]);
            if (result) {
                bool excluded = (result == exclude1 || result == exclude2);
                F4SE::log::info("{}: ID {} -> {:p}{}",
                    name, ids[i], result, excluded ? " (EXCLUDED: duplicate)" : "");
                if (!excluded && !firstValid) {
                    firstValid = result;
                    firstValidID = ids[i];
                }
            } else {
                F4SE::log::info("{}: ID {} -> null/invalid", name, ids[i]);
            }
        }
        if (firstValid) {
            F4SE::log::info("{}: SELECTED AddrLib ID {} -> {:p}",
                name, firstValidID, firstValid);
        } else {
            F4SE::log::error("{}: all candidate IDs failed or were duplicates", name);
        }
        return firstValid;
    }

    // Dump diagnostic info about .data addresses near a predicted location
    __declspec(noinline) void DiagnosticDump(const char* name,
        std::uintptr_t baseAddr, std::uintptr_t predictedRVA)
    {
        std::uintptr_t addr = baseAddr + predictedRVA;
        F4SE::log::info("=== DIAGNOSTIC: {} (predicted at base+0x{:07X}) ===",
            name, predictedRVA);

        // Read pointers in a range around the predicted address
        for (int delta = -0x100; delta <= 0x100; delta += 8) {
            auto* slot = reinterpret_cast<void**>(addr + delta);
            __try {
                void* val = *slot;
                if (val && IsPlausiblePointer(val)) {
                    // Check if the value looks like a vtable pointer
                    // (points into .rdata or .text)
                    auto valAddr = reinterpret_cast<std::uintptr_t>(val);
                    auto rva = valAddr - baseAddr;
                    // Plausible vtable targets are in .rdata (0x2438000-0x2ECCB42)
                    // or the value itself is a data pointer
                    if (rva > 0x1000 && rva < 0x4000000) {
                        F4SE::log::info("  [base+0x{:07X}] = {:p} (RVA 0x{:07X})",
                            predictedRVA + delta, val, rva);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }

    void ResolveSingletons()
    {
        if (g_singletonsResolved) return;
        g_singletonsResolved = true;

        auto modInfo = GetModuleLayout();
        if (!modInfo.valid) {
            F4SE::log::error("ResolveSingletons: failed to get module layout");
            return;
        }

        F4SE::log::info("Module base: {:p}", reinterpret_cast<void*>(modInfo.base));

        // -----------------------------------------------------------------
        // PlayerCharacter — CONFIRMED: Address Library ID 4798212
        // Data RVA 0x31E2D50 verified via RTTI vtable trace
        // -----------------------------------------------------------------
        {
            auto* ptr = reinterpret_cast<RE::PlayerCharacter**>(
                modInfo.base + 0x31E2D50);
            __try {
                g_cachedPlayer = *ptr;
                if (g_cachedPlayer && IsPlausiblePointer(g_cachedPlayer)) {
                    F4SE::log::info("PlayerCharacter: {:p} (ID 4798212, RVA 0x31E2D50)",
                        static_cast<void*>(g_cachedPlayer));
                } else {
                    F4SE::log::error("PlayerCharacter: pointer at 0x31E2D50 is null/invalid");
                    g_cachedPlayer = nullptr;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                F4SE::log::error("PlayerCharacter: access violation at 0x31E2D50");
                g_cachedPlayer = nullptr;
            }
        }

        // -----------------------------------------------------------------
        // UI — NOT YET CONFIRMED
        // CommonLibF4 NG ID 2689028 does not exist in 1.11.191 database.
        // Try candidate IDs from the data region analysis.
        // The UI singleton is a pointer stored in .data.
        // -----------------------------------------------------------------
        {
            // Candidates from binary analysis — try each one
            // These are IDs for heavily-referenced .data globals that could be UI
            // We validate by checking if the result looks like a valid object
            const std::uint64_t uiCandidates[] = {
                2698073,  // 0x32D2260 — 2878 refs, simple getter target
                2698069,  // 0x32D2248 — related, also a simple getter
                2713462,  // 0x3E5CC60 — 4600 refs
                2707353,  // 0x3DA7480 — 4535 refs
            };
            g_cachedUI = reinterpret_cast<RE::UI*>(
                TryCandidateIDs(uiCandidates, 4, "UI", g_cachedPlayer));

            if (!g_cachedUI) {
                // Diagnostic: dump what's at the predicted location
                DiagnosticDump("UI", modInfo.base, 0x30CF140);
                // Also try the top-referenced data addresses
                DiagnosticDump("UI-alt", modInfo.base, 0x32D2260);
            }
        }

        // -----------------------------------------------------------------
        // GameSettingCollection — NOT YET CONFIRMED
        // CommonLibF4 NG ID 2690301 does not exist in 1.11.191 database.
        // Candidates from reference analysis near predicted area.
        // -----------------------------------------------------------------
        {
            const std::uint64_t gscCandidates[] = {
                4796314,  // 0x30DD830 — 629 refs, highest in predicted area
                4796377,  // 0x30DDA20 — 246 refs
                4796297,  // 0x30DD7B0 — 232 refs
                4796420,  // 0x30E0208 — 225 refs
            };
            g_cachedSettings = reinterpret_cast<RE::GameSettingCollection*>(
                TryCandidateIDs(gscCandidates, 4, "GameSettingCollection",
                    g_cachedPlayer, g_cachedUI));

            if (!g_cachedSettings) {
                DiagnosticDump("GameSettingCollection", modInfo.base, 0x30DF120);
                DiagnosticDump("GameSettingCollection-alt", modInfo.base, 0x30DD830);
            }
        }

        // GetAllForms — REMOVED: form ID lookup causes crashes on AE 1.11.191.
        // All form references are now passed directly from Papyrus.

        F4SE::log::info("Singleton resolution complete: Player={:p} UI={:p} Settings={:p}",
            static_cast<void*>(g_cachedPlayer),
            static_cast<void*>(g_cachedUI),
            static_cast<void*>(g_cachedSettings));
    }

    // =========================================================================
    // SEH-safe singleton access (using cached resolved pointers)
    // CRITICAL: __declspec(noinline) prevents MSVC from breaking SEH.
    // =========================================================================

    __declspec(noinline) RE::UI* SEH_GetUI()
    {
        if (!g_singletonsResolved) ResolveSingletons();
        return g_cachedUI;
    }

    __declspec(noinline) RE::GameSettingCollection* SEH_GetSettings()
    {
        if (!g_singletonsResolved) ResolveSingletons();
        return g_cachedSettings;
    }

    __declspec(noinline) RE::PlayerCharacter* SEH_GetPlayer()
    {
        if (!g_singletonsResolved) ResolveSingletons();
        return g_cachedPlayer;
    }

    __declspec(noinline) RE::Setting* SEH_GetSetting(RE::GameSettingCollection* settings, const char* key)
    {
        RE::Setting* result = nullptr;
        __try {
            result = settings->GetSetting(key);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (result && !IsPlausiblePointer(result)) return nullptr;
        return result;
    }

    __declspec(noinline) bool SEH_SetFloat(RE::Setting* setting, float value)
    {
        __try {
            setting->SetFloat(value);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    __declspec(noinline) bool SEH_SetInt(RE::Setting* setting, std::int32_t value)
    {
        __try {
            setting->SetInt(value);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct ActorCoordsResult { float x; float y; float z; bool valid; };

    // =========================================================================
    // Crosshair tracking
    // =========================================================================

    struct CrosshairData
    {
        std::mutex lock;
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
        bool valid{ false };
    };
    CrosshairData g_crosshairData;

    // =========================================================================
    // Topic Info patching — cache and state
    // =========================================================================

    struct PatchedTopicInfo
    {
        RE::TESTopicInfo* info{ nullptr };
        std::string originalText;
        std::string patchedText;
    };

    std::mutex g_patchCacheLock;
    std::vector<PatchedTopicInfo> g_patchCache;

    // =========================================================================
    // Voice filename override cache
    // =========================================================================

    std::mutex g_overrideLock;
    std::unordered_map<RE::TESTopicInfo*, std::string> g_filenameOverrides;

    // =========================================================================
    // Game settings save/restore cache
    // =========================================================================

    std::mutex g_settingsLock;
    std::unordered_map<std::string, float> g_savedFloats;
    std::unordered_map<std::string, std::int32_t> g_savedInts;

    // =========================================================================
    // Safe form lookup via GetAllForms map at known RVA (avoids GetFormByID)
    // MUST be __declspec(noinline) for SEH to work
    // =========================================================================

    // =========================================================================
    // Form lookup — call the game's own GetFormByID directly
    //
    // CONFIRMED via capstone disassembly of Fallout4.exe AE 1.11.191:
    //   GetFormByID is at .text RVA 0x311850
    //   It uses map pointer at .data RVA 0x030E0DC8 (AddrLib ID 4796465)
    //   and lock at .data RVA 0x030E0E18 (AddrLib ID 4796476)
    //
    // The old code used RVA 0x030E0DD0 / 0x030E0E20 — those are 8 bytes
    // off and correspond to GetAllFormsByEditorID (BSFixedString keys),
    // NOT the form ID map. That's why lookup always failed.
    //
    // Instead of reimplementing the hash map lookup, we call the game's
    // own function directly at its confirmed RVA. This is safe because:
    //   - The function handles its own locking
    //   - The hash function matches exactly
    //   - The map structure offsets are correct
    // =========================================================================

    using GetFormByID_fn = RE::TESForm* (__fastcall*)(std::uint32_t);
    GetFormByID_fn g_GetFormByID = nullptr;
    bool g_formLookupResolved = false;

    __declspec(noinline) void ResolveFormLookup()
    {
        if (g_formLookupResolved) return;
        g_formLookupResolved = true;

        F4SE::log::info("=== Resolving GetFormByID function ===");

        auto base = REL::Module::get().base();

        // The function at RVA 0x311850 is GetFormByID
        // Verify it exists by checking the first few bytes match expected prologue:
        //   48 89 5C 24 08  mov [rsp+8], rbx
        //   57              push rdi
        //   48 83 EC 20     sub rsp, 20h
        auto* funcAddr = reinterpret_cast<std::uint8_t*>(base + 0x311850);
        __try {
            if (funcAddr[0] == 0x48 && funcAddr[1] == 0x89 &&
                funcAddr[2] == 0x5C && funcAddr[3] == 0x24 &&
                funcAddr[4] == 0x08 && funcAddr[5] == 0x57) {
                g_GetFormByID = reinterpret_cast<GetFormByID_fn>(funcAddr);
                F4SE::log::info("GetFormByID: confirmed at {:p} (RVA 0x311850)",
                    static_cast<void*>(funcAddr));
            } else {
                F4SE::log::error("GetFormByID: prologue mismatch at RVA 0x311850! "
                    "bytes: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    funcAddr[0], funcAddr[1], funcAddr[2],
                    funcAddr[3], funcAddr[4], funcAddr[5]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            F4SE::log::error("GetFormByID: exception reading RVA 0x311850");
        }

        if (!g_GetFormByID) {
            F4SE::log::error("GetFormByID: FAILED to resolve!");
        }
    }

    __declspec(noinline) RE::TESForm* SEH_LookupForm(std::uint32_t formID)
    {
        if (!g_formLookupResolved) ResolveFormLookup();

        if (!g_GetFormByID) {
            F4SE::log::error("SEH_LookupForm: GetFormByID not resolved");
            return nullptr;
        }

        RE::TESForm* form = nullptr;
        __try {
            form = g_GetFormByID(formID);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            F4SE::log::error("SEH_LookupForm: exception calling GetFormByID({:08X})", formID);
        }
        return form;
    }

    // =========================================================================
    // PatchTopicInfo — Patches dialogue response text in memory
    // =========================================================================

    int PatchTopicInfoNative(std::monostate, std::int32_t a_formID, std::string a_text)
    {
        if (a_formID == 0) {
            F4SE::log::error("PatchTopicInfo: zero form ID passed from Papyrus");
            return -1;
        }

        F4SE::log::info("PatchTopicInfo: FormID {:08X} Text: {}", static_cast<std::uint32_t>(a_formID), a_text);

        if (!g_singletonsResolved) ResolveSingletons();

        RE::TESForm* form = SEH_LookupForm(static_cast<std::uint32_t>(a_formID));
        if (!form) {
            F4SE::log::error("PatchTopicInfo: form {:08X} not found", static_cast<std::uint32_t>(a_formID));
            return -1;
        }
        auto* a_topic = static_cast<RE::TESTopic*>(form);
        F4SE::log::info("PatchTopicInfo: Topic {:08X} resolved", a_topic->GetFormID());

        if (!a_topic->topicInfos || a_topic->numTopicInfos == 0) {
            F4SE::log::error("PatchTopicInfo: Topic {:08X} has no topic infos", a_topic->GetFormID());
            return -1;
        }

        RE::TESTopicInfo* info = a_topic->topicInfos[0];
        if (!info) {
            F4SE::log::error("PatchTopicInfo: Topic {:08X} topicInfos[0] is null", a_topic->GetFormID());
            return -1;
        }

        F4SE::log::info("PatchTopicInfo: Topic {:08X} TopicInfo {:08X} Text: {}",
            a_topic->GetFormID(), info->GetFormID(), a_text);

        RE::TESResponse* response = info->responses.head;
        if (!response) {
            F4SE::log::error("PatchTopicInfo: TopicInfo {:08X} has no responses", info->GetFormID());
            return -1;
        }

        // Cache original text before patching
        {
            std::lock_guard lock(g_patchCacheLock);

            bool found = false;
            for (auto& entry : g_patchCache) {
                if (entry.info == info) {
                    entry.patchedText = a_text;
                    found = true;
                    break;
                }
            }

            if (!found) {
                PatchedTopicInfo entry;
                entry.info = info;
                const char* currentText = response->responseText.data();
                entry.originalText = currentText ? currentText : "";
                entry.patchedText = a_text;
                g_patchCache.push_back(entry);
            }
        }

        // Patch the response text
        response->responseText = std::string_view(a_text);

        const char* newText = response->responseText.data();
        if (newText && a_text == newText) {
            F4SE::log::info("PatchTopicInfo: OK {:08X}", info->GetFormID());
        } else {
            F4SE::log::warn("PatchTopicInfo: Verify mismatch {:08X}", info->GetFormID());
        }

        return 0;
    }

    // =========================================================================
    // ClearCache
    // =========================================================================

    void ClearCache(std::monostate)
    {
        std::lock_guard lock(g_patchCacheLock);
        F4SE::log::info("ClearCache: Clearing {} entries", g_patchCache.size());

        for (auto& entry : g_patchCache) {
            if (entry.info) {
                RE::TESResponse* response = entry.info->responses.head;
                if (response) {
                    response->responseText = std::string_view(entry.originalText);
                }
            }
        }

        g_patchCache.clear();
    }

    // =========================================================================
    // SetOverrideFileName
    // =========================================================================

    void SetOverrideFileName_internal(RE::TESTopicInfo* a_info, const char* a_name,
        std::size_t a_nameLen, std::size_t a_bufLen)
    {
        if (!a_info || !a_name) return;

        F4SE::log::info("SetOverrideFileName_internal: Info {:p} name: {}",
            static_cast<void*>(a_info), a_name);

        std::lock_guard lock(g_overrideLock);
        g_filenameOverrides[a_info] = std::string(a_name, a_nameLen);
    }

    void SetOverrideFileNameNative(std::monostate, std::int32_t a_formID, std::string a_name)
    {
        if (a_formID == 0) {
            F4SE::log::error("SetOverrideFileName: zero form ID passed from Papyrus");
            return;
        }

        F4SE::log::info("SetOverrideFileName: FormID {:08X} name: {}", static_cast<std::uint32_t>(a_formID), a_name);

        if (!g_singletonsResolved) ResolveSingletons();

        RE::TESForm* form = SEH_LookupForm(static_cast<std::uint32_t>(a_formID));
        if (!form) {
            F4SE::log::error("SetOverrideFileName: form {:08X} not found", static_cast<std::uint32_t>(a_formID));
            return;
        }
        auto* a_topic = static_cast<RE::TESTopic*>(form);
        F4SE::log::info("SetOverrideFileName: Topic {:08X} name: {}", a_topic->GetFormID(), a_name);

        if (!a_topic->topicInfos || a_topic->numTopicInfos == 0) {
            F4SE::log::error("SetOverrideFileName: Topic {:08X} has no topic infos", a_topic->GetFormID());
            return;
        }

        RE::TESTopicInfo* info = a_topic->topicInfos[0];
        if (!info) return;

        SetOverrideFileName_internal(info, a_name.c_str(), a_name.size(), a_name.size() + 1);
    }

    // =========================================================================
    // StringRemoveWhiteSpace
    // =========================================================================

    std::string StringRemoveWhiteSpace(std::monostate, std::string a_text)
    {
        std::string result;
        result.reserve(a_text.size());
        for (char c : a_text) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                result += c;
            }
        }
        return result;
    }

    // =========================================================================
    // TakeScreenShot
    // =========================================================================

    void TakeScreenShot(std::monostate, std::string a_filename, std::int32_t a_filetype, std::int32_t a_sstype)
    {
        F4SE::log::info("TakeScreenShot: filename='{}' filetype={} sstype={}", a_filename, a_filetype, a_sstype);
        auto* ui = SEH_GetUI();
        if (ui) {
            ui->freezeFrameScreenshotReady = true;
        } else {
            F4SE::log::error("TakeScreenShot: UI singleton unavailable");
        }
    }

    // =========================================================================
    // isMenuModeActive
    // =========================================================================

    bool isMenuModeActive(std::monostate)
    {
        auto* ui = SEH_GetUI();
        if (!ui) return false;
        return ui->menuMode > 0;
    }

    // =========================================================================
    // Game Settings save/restore
    // =========================================================================

    bool saveFloat(std::monostate, std::string a_key)
    {
        auto* settings = SEH_GetSettings();
        if (!settings) {
            F4SE::log::error("saveFloat: GameSettingCollection unavailable");
            return false;
        }

        auto* setting = SEH_GetSetting(settings, a_key.c_str());
        if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) return false;

        std::lock_guard lock(g_settingsLock);
        g_savedFloats[a_key] = setting->GetFloat();
        F4SE::log::info("saveFloat: '{}' = {:.6f}", a_key, setting->GetFloat());
        return true;
    }

    bool saveInt(std::monostate, std::string a_key)
    {
        auto* settings = SEH_GetSettings();
        if (!settings) {
            F4SE::log::error("saveInt: GameSettingCollection unavailable");
            return false;
        }

        auto* setting = SEH_GetSetting(settings, a_key.c_str());
        if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kInt) return false;

        std::lock_guard lock(g_settingsLock);
        g_savedInts[a_key] = setting->GetInt();
        F4SE::log::info("saveInt: '{}' = {}", a_key, setting->GetInt());
        return true;
    }

    bool restoreFloat(std::monostate, std::string a_key)
    {
        float savedValue;
        {
            std::lock_guard lock(g_settingsLock);
            auto it = g_savedFloats.find(a_key);
            if (it == g_savedFloats.end()) return false;
            savedValue = it->second;
        }

        auto* settings = SEH_GetSettings();
        if (!settings) return false;

        auto* setting = SEH_GetSetting(settings, a_key.c_str());
        if (!setting) return false;

        if (!SEH_SetFloat(setting, savedValue)) {
            F4SE::log::error("restoreFloat: SetFloat('{}') crashed", a_key);
            return false;
        }
        F4SE::log::info("restoreFloat: '{}' = {:.6f}", a_key, savedValue);
        return true;
    }

    bool restoreInt(std::monostate, std::string a_key)
    {
        std::int32_t savedValue;
        {
            std::lock_guard lock(g_settingsLock);
            auto it = g_savedInts.find(a_key);
            if (it == g_savedInts.end()) return false;
            savedValue = it->second;
        }

        auto* settings = SEH_GetSettings();
        if (!settings) return false;

        auto* setting = SEH_GetSetting(settings, a_key.c_str());
        if (!setting) return false;

        if (!SEH_SetInt(setting, savedValue)) {
            F4SE::log::error("restoreInt: SetInt('{}') crashed", a_key);
            return false;
        }
        F4SE::log::info("restoreInt: '{}' = {}", a_key, savedValue);
        return true;
    }

    // =========================================================================
    // GetLastActorCoords
    // =========================================================================

    void UpdateCrosshairActorCoords()
    {
        auto* player = SEH_GetPlayer();
        if (!player) return;

        auto refHandle = player->dialogueItemTarget;
        if (!refHandle) return;

        auto refPtr = refHandle.get();
        if (!refPtr) return;

        auto pos = refPtr->GetPosition();
        std::lock_guard lock(g_crosshairData.lock);
        g_crosshairData.x = pos.x;
        g_crosshairData.y = pos.y;
        g_crosshairData.z = pos.z;
        g_crosshairData.valid = true;
    }

    // Split into 3 functions returning float — std::vector<float> crashes on AE
    // because GetScriptObjectType for float[] returns a .rdata pointer
    float GetLastActorCoordX(std::monostate)
    {
        UpdateCrosshairActorCoords();
        std::lock_guard lock(g_crosshairData.lock);
        return g_crosshairData.x;
    }

    float GetLastActorCoordY(std::monostate)
    {
        UpdateCrosshairActorCoords();
        std::lock_guard lock(g_crosshairData.lock);
        return g_crosshairData.y;
    }

    float GetLastActorCoordZ(std::monostate)
    {
        UpdateCrosshairActorCoords();
        std::lock_guard lock(g_crosshairData.lock);
        return g_crosshairData.z;
    }

    // =========================================================================
    // Papyrus registration
    // =========================================================================

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) return false;

        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "PatchTopicInfoNative"sv, PatchTopicInfoNative, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "ClearCache"sv, ClearCache, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "SetOverrideFileNameNative"sv, SetOverrideFileNameNative, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "StringRemoveWhiteSpace"sv, StringRemoveWhiteSpace, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "TakeScreenShot"sv, TakeScreenShot, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "isMenuModeActive"sv, isMenuModeActive, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "saveFloat"sv, saveFloat, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "saveInt"sv, saveInt, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "restoreFloat"sv, restoreFloat, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "restoreInt"sv, restoreInt, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "GetLastActorCoordX"sv, GetLastActorCoordX, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "GetLastActorCoordY"sv, GetLastActorCoordY, true);
        a_vm->BindNativeMethod("TopicInfoPatcher"sv, "GetLastActorCoordZ"sv, GetLastActorCoordZ, true);

        F4SE::log::info("Registered 13 Papyrus native functions.");
        return true;
    }
}

// =========================================================================
// F4SE Plugin Version Declaration
// =========================================================================

extern "C" __declspec(dllexport) constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginVersion({ 0, 9, 8, 0 });
    data.PluginName("TopicInfoPatcher");
    data.AuthorName("peteben (rebuilt)");
    data.UsesAddressLibrary(true);
    data.IsLayoutDependent(true);
    data.addressIndependence |= (1 << 2);
    data.structureIndependence |= (1 << 2);
    return data;
}();

// =========================================================================
// F4SE Plugin Load
// =========================================================================

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);
    F4SE::log::info("TopicInfoPatcher v0.9.8 loaded (singleton bypass + diagnostics)");

    const auto papyrus = reinterpret_cast<const F4SE::PapyrusInterface*>(
        a_f4se->QueryInterface(F4SE::LoadInterface::kPapyrus));
    if (!papyrus || !papyrus->Register(RegisterPapyrusFunctions)) {
        F4SE::log::error("Failed to register Papyrus functions!");
    } else {
        F4SE::log::info("Registered Papyrus");
    }

    return true;
}
