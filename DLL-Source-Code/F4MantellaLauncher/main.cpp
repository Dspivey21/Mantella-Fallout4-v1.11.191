// F4MantellaLauncher — Rebuilt for AE/NG
//
// Original plugin by peteben (closed-source).
// Functionality reconstructed from binary analysis — no original source code was used.
//
// What this plugin does:
//   1. On kGameDataReady: checks if Mantella.exe is already running
//   2. If not running, launches it from the Documents\My Games\Fallout4\ area
//   3. Provides Papyrus native function MantellaLauncher.LaunchMantellaExe()
//      so the MCM menu can restart Mantella.exe on demand
//   4. Works around OneDrive redirecting the temp folder

#include "F4SE/F4SE.h"
#include "F4SE/Interfaces.h"
#include "F4SE/Logger.h"

#include "RE/Fallout.h"

#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <filesystem>

using namespace std::literals;

namespace
{
    // =========================================================================
    // OneDrive temp path workaround
    // =========================================================================
    // If OneDrive is syncing the user's Documents folder, the default TEMP
    // path may be inside OneDrive, causing issues. This detects that case
    // and redirects TEMP/TMP to the system temp directory instead.

    bool SetEnvironmentTempPath()
    {
        // Get the user's Documents folder
        PWSTR documentsPath = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath);
        if (FAILED(hr)) {
            F4SE::log::error("Failed to get Documents folder path.");
            CoTaskMemFree(documentsPath);
            return false;
        }

        std::wstring docsStr(documentsPath);
        CoTaskMemFree(documentsPath);

        // Check if OneDrive is in the path
        std::wstring docsLower = docsStr;
        // Convert to lowercase for comparison
        for (auto& c : docsLower) {
            c = towlower(c);
        }

        if (docsLower.find(L"onedrive") != std::wstring::npos) {
            F4SE::log::warn("OneDrive detected!");

            // Get the system temp directory
            wchar_t sysTempPath[MAX_PATH];
            DWORD len = GetTempPathW(MAX_PATH, sysTempPath);
            if (len == 0 || len > MAX_PATH) {
                F4SE::log::error("Failed to get system temporary directory.");
                return false;
            }

            // Create a Mantella-specific temp subfolder
            std::filesystem::path mantellaTemp = std::filesystem::path(sysTempPath) / L"Mantella";
            std::error_code ec;
            std::filesystem::create_directories(mantellaTemp, ec);

            std::string tempPathStr = mantellaTemp.string();
            F4SE::log::info("Mantella temp files in {}", tempPathStr);

            // Override TEMP and TMP environment variables
            std::wstring tempPathW = mantellaTemp.wstring();
            if (!SetEnvironmentVariableW(L"TEMP", tempPathW.c_str()) ||
                !SetEnvironmentVariableW(L"TMP", tempPathW.c_str())) {
                F4SE::log::error("Failed to set TEMP/TMP environment variables.");
                return false;
            }
        }

        return true;
    }

    // =========================================================================
    // Process enumeration — check if Mantella.exe is already running
    // =========================================================================

    bool IsMantellaRunning()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }

        PROCESSENTRY32W pe32{};
        pe32.dwSize = sizeof(pe32);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (_wcsicmp(pe32.szExeFile, L"Mantella.exe") == 0) {
                    CloseHandle(snapshot);
                    return true;
                }
            } while (Process32NextW(snapshot, &pe32));
        }

        CloseHandle(snapshot);
        return false;
    }

    // =========================================================================
    // Launch Mantella.exe
    // =========================================================================
    // Finds Mantella.exe relative to the F4SE plugin directory, checks if
    // an old instance is running (and kills it), then launches a new one.

    bool LaunchMantellaExe()
    {
        // Build path: <game>\Data\F4SE\Plugins\MantellaSoftware\Mantella.exe
        wchar_t modulePath[MAX_PATH];
        HMODULE hModule = nullptr;

        // Get our own DLL's path to find the Plugins folder
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LaunchMantellaExe),
                &hModule)) {
            F4SE::log::error("Failed to get module handle.");
            return false;
        }

        GetModuleFileNameW(hModule, modulePath, MAX_PATH);
        std::filesystem::path dllPath(modulePath);
        std::filesystem::path pluginsDir = dllPath.parent_path();
        std::filesystem::path exePath = pluginsDir / L"MantellaSoftware" / L"Mantella.exe";

        F4SE::log::info("EXE path: {}", exePath.string());

        if (!std::filesystem::exists(exePath)) {
            F4SE::log::error("Mantella.exe not found at expected path.");
            return false;
        }

        // Check if already running — if so, terminate the old instance
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe32{};
            pe32.dwSize = sizeof(pe32);

            if (Process32FirstW(snapshot, &pe32)) {
                DWORD myPid = GetCurrentProcessId();
                do {
                    if (_wcsicmp(pe32.szExeFile, L"Mantella.exe") == 0 &&
                        pe32.th32ProcessID != myPid) {
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe32.th32ProcessID);
                        if (hProc) {
                            if (TerminateProcess(hProc, 0)) {
                                F4SE::log::info("Existing Mantella.exe process terminated.");
                            } else {
                                DWORD err = GetLastError();
                                F4SE::log::error("Failed to terminate existing Mantella.exe process. TerminateProcess error: {}", err);
                            }
                            CloseHandle(hProc);
                        }
                    }
                } while (Process32NextW(snapshot, &pe32));
            }
            CloseHandle(snapshot);
        }

        // Launch new instance
        F4SE::log::info("Attempting to launch: {}", exePath.string());

        std::wstring exePathW = exePath.wstring();
        std::wstring workDir = exePath.parent_path().wstring();

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        BOOL created = CreateProcessW(
            exePathW.c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE,
            nullptr,
            workDir.c_str(),
            &si,
            &pi);

        if (!created) {
            DWORD err = GetLastError();
            F4SE::log::error("Failed to launch Mantella.exe. CreateProcess error: {}", err);
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        F4SE::log::info("Mantella.exe launched successfully!");
        return true;
    }

    // =========================================================================
    // Papyrus native function
    // =========================================================================

    bool PapyrusLaunchMantellaExe(std::monostate)
    {
        return LaunchMantellaExe();
    }

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) return false;

        a_vm->BindNativeMethod("MantellaLauncher"sv, "LaunchMantellaExe"sv, PapyrusLaunchMantellaExe);

        F4SE::log::info("Registered Papyrus functions.");
        return true;
    }

    // =========================================================================
    // Messaging callback
    // =========================================================================

    void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) return;

        // kGameDataReady = 10 (0xA)
        if (a_msg->type == static_cast<std::uint32_t>(F4SE::MessagingInterface::kGameDataReady)) {
            if (IsMantellaRunning()) {
                F4SE::log::info("Found running instance of Mantella.exe. Not starting a new one. You can still restart it from the MCM.");
            } else {
                F4SE::log::info("Launching EXE");
                if (LaunchMantellaExe()) {
                    F4SE::log::info("Mantella.exe launched successfully!");
                } else {
                    F4SE::log::error("Failed to launch Mantella.exe.");
                }
            }
        }
    }
}

// =========================================================================
// F4SE Plugin Version Declaration
// =========================================================================

extern "C" __declspec(dllexport) constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginVersion({ 0, 9, 0, 64 });
    data.PluginName("MantellaLauncher");
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
    F4SE::log::info("F4MantellaLauncher loaded (rebuilt for AE/NG)");

    // OneDrive temp path workaround
    SetEnvironmentTempPath();

    // Register Papyrus native functions
    const auto papyrus = reinterpret_cast<const F4SE::PapyrusInterface*>(
        a_f4se->QueryInterface(F4SE::LoadInterface::kPapyrus));
    if (!papyrus || !papyrus->Register(RegisterPapyrusFunctions)) {
        F4SE::log::error("Failed to register Papyrus functions");
    } else {
        F4SE::log::info("Registered Papyrus");
    }

    // Register messaging listener
    const auto messaging = reinterpret_cast<const F4SE::MessagingInterface*>(
        a_f4se->QueryInterface(F4SE::LoadInterface::kMessaging));
    if (messaging) {
        messaging->RegisterListener(OnF4SEMessage);
    }

    return true;
}
