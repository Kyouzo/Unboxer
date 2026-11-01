// ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// Unboxer Injector — loads Unboxer.dll (the CS2 case opener) into CS2.
//
// The DLL is baked into this exe (resource IDR_PAYLOAD), so the injector is a single file. Standard
// LoadLibraryW + CreateRemoteThread injection. Flow:
//   1. unpack the built-in DLL to %LOCALAPPDATA%\Unboxer\ (or use a path passed as the first argument /
//      dragged onto the exe, or an Unboxer.dll next to the exe if there's no built-in one)
//   2. wait for cs2.exe
//   3. wait until the game has loaded client.dll (Unboxer pattern-scans it on startup — injecting earlier fails)
//   4. skip if Unboxer is already loaded, otherwise inject and confirm the module is present
// It can run next to another CS2 changer: its own DLL name prefix, unpack folder, menu key (Home) and unload key (Page Down).
// Paths are handled as UTF-16 end to end, so folders with non-English names (a localized Desktop) work.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <conio.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "resource.h"

namespace
{
    constexpr const wchar_t* kGameExe = L"cs2.exe";
    constexpr const wchar_t* kGameModule = L"client.dll";
    constexpr const wchar_t* kDllName = L"Lootline.dll";

    DWORD FindProcess(const wchar_t* exeName)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;

        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        {
            if (_wcsicmp(entry.szExeFile, exeName) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        }

        CloseHandle(snapshot);
        return pid;
    }

    bool IsProcessAlive(DWORD pid)
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process)
            return false;

        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
    }

    // True if `moduleName` is loaded in the process. Retries briefly: module snapshots can fail transiently
    // (ERROR_BAD_LENGTH) while the target is loading modules.
    bool HasModule(DWORD pid, const wchar_t* moduleName)
    {
        HANDLE snapshot = INVALID_HANDLE_VALUE;

        for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
        {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot == INVALID_HANDLE_VALUE)
                Sleep(50);
        }

        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;

        for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
        {
            if (_wcsicmp(entry.szModule, moduleName) == 0)
            {
                found = true;
                break;
            }
        }

        CloseHandle(snapshot);
        return found;
    }

    std::filesystem::path DllNextToExe()
    {
        wchar_t exePath[MAX_PATH * 4]{};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        return std::filesystem::path(exePath).parent_path() / kDllName;
    }

    // True if any module whose name starts with "lootline" (or "unboxer", its old name) is loaded (the unpacked DLL's name carries a build hash).
    bool HasUnboxer(DWORD pid)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;

        for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
        {
            if (_wcsnicmp(entry.szModule, L"lootline", 8) == 0 || _wcsnicmp(entry.szModule, L"unboxer", 7) == 0)
            {
                found = true;
                break;
            }
        }

        CloseHandle(snapshot);
        return found;
    }

    // Unpacks the built-in DLL to %LOCALAPPDATA%\Unboxer\unboxer_<hash>.dll. The name comes from the content, so
    // the same build is reused and a new build never collides with an old one CS2 still has loaded. Older builds
    // are deleted when they're not in use. Returns false if this exe has no built-in DLL.
    bool UnpackPayload(std::filesystem::path& out, std::wstring& error)
    {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);

        if (!resource)
            return false;

        HGLOBAL handle = LoadResource(nullptr, resource);
        const DWORD size = SizeofResource(nullptr, resource);
        const auto* data = handle ? static_cast<const std::uint8_t*>(LockResource(handle)) : nullptr;

        if (!data || size == 0)
        {
            error = L"The built-in DLL is empty.";
            return false;
        }

        std::uint64_t hash = 1469598103934665603ULL;   // FNV-1a
        for (DWORD i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }

        wchar_t localAppData[MAX_PATH * 2]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData))))
            GetTempPathW(static_cast<DWORD>(std::size(localAppData)), localAppData);

        std::error_code ec;
        const std::filesystem::path folder = std::filesystem::path(localAppData) / L"Lootline";
        std::filesystem::create_directories(folder, ec);

        wchar_t name[64]{};
        swprintf_s(name, L"lootline_%016llx.dll", static_cast<unsigned long long>(hash));
        out = folder / name;

        // older unpacked builds: remove the ones nothing has loaded
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (entry.path() != out && entry.path().extension() == L".dll")
                std::filesystem::remove(entry.path(), ec);
        }

        if (std::filesystem::exists(out, ec) && std::filesystem::file_size(out, ec) == size)
            return true;   // this build is already unpacked (maybe even loaded right now)

        std::ofstream file(out, std::ios::binary | std::ios::trunc);

        if (!file.write(reinterpret_cast<const char*>(data), size))
        {
            error = L"Couldn't write " + out.wstring();
            return false;
        }

        return true;
    }

    // What the payload imports that this PC can't provide: whole DLLs (e.g. the Visual Studio debug runtime on a PC
    // without Visual Studio) or single functions (an older Windows). Reads the import table from the file bytes.
    std::vector<std::wstring> MissingDependencies(const std::vector<std::uint8_t>& image)
    {
        std::vector<std::wstring> missing;

        if (image.size() < sizeof(IMAGE_DOS_HEADER))
            return missing;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());

        if (dos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size())
            return missing;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);

        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return missing;

        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        const WORD sectionCount = nt->FileHeader.NumberOfSections;

        const auto toOffset = [&](DWORD rva) -> std::size_t
        {
            for (WORD i = 0; i < sectionCount; ++i)
            {
                const IMAGE_SECTION_HEADER& section = sections[i];

                if (rva >= section.VirtualAddress && rva < section.VirtualAddress + (std::max)(section.SizeOfRawData, section.Misc.VirtualSize))
                    return static_cast<std::size_t>(rva - section.VirtualAddress) + section.PointerToRawData;
            }

            return 0;
        };

        const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        std::size_t offset = toOffset(directory.VirtualAddress);

        for (; offset && offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= image.size(); offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
        {
            const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.data() + offset);

            if (descriptor->Name == 0)
                break;

            const std::size_t nameOffset = toOffset(descriptor->Name);

            if (!nameOffset || nameOffset >= image.size())
                continue;

            const char* name = reinterpret_cast<const char*>(image.data() + nameOffset);
            const std::wstring wideName(name, name + std::strlen(name));

            // Every import is a Windows system DLL, so loading it here is harmless.
            HMODULE module = LoadLibraryA(name);

            if (!module)
            {
                missing.push_back(wideName);
                continue;
            }

            // ...and every function taken from it must exist on this Windows version.
            std::size_t thunk = toOffset(descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk);

            for (; thunk && thunk + sizeof(IMAGE_THUNK_DATA64) <= image.size(); thunk += sizeof(IMAGE_THUNK_DATA64))
            {
                const auto* data = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image.data() + thunk);

                if (data->u1.AddressOfData == 0)
                    break;

                if (IMAGE_SNAP_BY_ORDINAL64(data->u1.Ordinal))
                    continue;

                const std::size_t byName = toOffset(static_cast<DWORD>(data->u1.AddressOfData));

                if (!byName || byName + sizeof(IMAGE_IMPORT_BY_NAME) >= image.size())
                    continue;

                const char* function = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image.data() + byName)->Name;

                if (!GetProcAddress(module, function))
                    missing.push_back(wideName + L" -> " + std::wstring(function, function + std::strlen(function)));
            }
        }

        return missing;
    }

    // Plain-words reason for a LoadLibrary error code from inside CS2.
    std::wstring ExplainLoadError(DWORD code)
    {
        switch (code)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return L"The DLL file disappeared - almost always the antivirus deleting it.";
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return L"Access denied - an antivirus / security program is blocking the DLL.";
        case ERROR_MOD_NOT_FOUND:
            return L"A DLL it needs isn't on this PC.";
        case ERROR_PROC_NOT_FOUND:
            return L"This Windows is missing a function the DLL needs - run Windows Update.";
        case ERROR_BAD_EXE_FORMAT:
            return L"The DLL file is damaged or not 64-bit.";
        case 225:   // ERROR_VIRUS_INFECTED
        case 226:   // ERROR_VIRUS_DELETED
            return L"Windows Defender (or another antivirus) flagged the DLL and blocked it.";
        case 577:   // ERROR_INVALID_IMAGE_HASH
        case 4551:  // ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION
        case 4553:
        case 4556:
            return L"Windows blocked the unsigned DLL (Smart App Control / code integrity).";
        case ERROR_DLL_INIT_FAILED:
            return L"The DLL crashed while starting.";
        default:
            return L"";
        }
    }

    std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file)
            return {};

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);

        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return {};

        return bytes;
    }

    std::wstring LastErrorText(DWORD code)
    {
        wchar_t* buffer = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

        std::wstring text = buffer ? buffer : L"unknown error";
        if (buffer)
            LocalFree(buffer);

        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
            text.pop_back();

        return text + L" (" + std::to_wstring(code) + L")";
    }

    // Loads the DLL inside CS2 with a tiny stub that returns 0 on success or GetLastError() on failure — so a failure
    // tells us WHY (antivirus, missing function, ...) instead of just "NULL".
    //   sub rsp,28h / mov rcx,path / mov rax,LoadLibraryW / call rax / test rax,rax / jz fail
    //   xor eax,eax / add rsp,28h / ret
    //   fail: mov rax,GetLastError / call rax / add rsp,28h / ret
    std::vector<std::uint8_t> BuildLoaderStub(std::uint64_t path, std::uint64_t loadLibrary, std::uint64_t getLastError)
    {
        std::vector<std::uint8_t> code = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB9 };
        const auto imm64 = [&](std::uint64_t value)
        {
            for (int i = 0; i < 8; ++i)
                code.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        };

        imm64(path);
        code.insert(code.end(), { 0x48, 0xB8 });
        imm64(loadLibrary);
        code.insert(code.end(), { 0xFF, 0xD0, 0x48, 0x85, 0xC0, 0x74, 0x07, 0x31, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x48, 0xB8 });
        imm64(getLastError);
        code.insert(code.end(), { 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3 });
        return code;
    }

    bool Inject(DWORD pid, const std::wstring& dllPath, std::wstring& error)
    {
        HANDLE process = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE, pid);

        if (!process)
        {
            error = L"OpenProcess failed: " + LastErrorText(GetLastError()) + L". Try running the injector as administrator.";
            return false;
        }

        // [ path (UTF-16) | padding | stub ] in one block
        const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
        const SIZE_T stubOffset = (pathBytes + 15) & ~static_cast<SIZE_T>(15);
        const SIZE_T totalBytes = stubOffset + 128;

        auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(process, nullptr, totalBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        if (!remote)
        {
            error = L"VirtualAllocEx failed: " + LastErrorText(GetLastError());
            CloseHandle(process);
            return false;
        }

        // kernel32 is mapped at the same address in every process of a session, so these addresses are valid in CS2.
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto loadLibrary = reinterpret_cast<std::uint64_t>(GetProcAddress(kernel32, "LoadLibraryW"));
        const auto getLastError = reinterpret_cast<std::uint64_t>(GetProcAddress(kernel32, "GetLastError"));
        const std::vector<std::uint8_t> stub = BuildLoaderStub(reinterpret_cast<std::uint64_t>(remote), loadLibrary, getLastError);

        bool ok = false;

        if (!WriteProcessMemory(process, remote, dllPath.c_str(), pathBytes, nullptr) ||
            !WriteProcessMemory(process, remote + stubOffset, stub.data(), stub.size(), nullptr))
        {
            error = L"WriteProcessMemory failed: " + LastErrorText(GetLastError());
        }
        else
        {
            FlushInstructionCache(process, remote + stubOffset, stub.size());

            HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + stubOffset), nullptr, 0, nullptr);

            if (!thread)
            {
                error = L"CreateRemoteThread failed: " + LastErrorText(GetLastError());
            }
            else
            {
                // A big DLL can take a while the first time (antivirus scans it). If it's still loading after a
                // minute, something is blocking it — usually an error box hidden behind the game window.
                if (WaitForSingleObject(thread, 60000) == WAIT_TIMEOUT)
                {
                    error = L"CS2 is still loading the DLL after 60 seconds. Alt-Tab and look for an error window "
                            L"behind the game, or an antivirus prompt. Don't close this window yet.";
                    CloseHandle(thread);
                    CloseHandle(process);
                    return false;   // the remote thread still uses the block — don't free it
                }

                DWORD loadError = 0;
                GetExitCodeThread(thread, &loadError);   // 0 = loaded, otherwise LoadLibraryW's error code
                CloseHandle(thread);

                ok = loadError == 0;

                if (!ok)
                {
                    const std::wstring reason = ExplainLoadError(loadError);
                    error = L"CS2 couldn't load the DLL: " + LastErrorText(loadError);

                    if (!reason.empty())
                        error += L"\n    -> " + reason;

                    if (loadError == ERROR_FILE_NOT_FOUND || loadError == ERROR_ACCESS_DENIED || loadError == 225 || loadError == 226)
                    {
                        error += L"\n    Fix: Windows Security -> Virus & threat protection -> Manage settings -> Exclusions ->"
                                 L"\n         add the folder %LOCALAPPDATA%\\Lootline (and the folder with this exe), then run it again.";
                    }
                }
            }
        }

        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return ok;
    }

    int Finish(int code)
    {
        if (code == 0)
        {
            wprintf(L"\nThis window closes in 3 seconds...\n");
            Sleep(3000);
        }
        else
        {
            wprintf(L"\nPress any key to close...\n");
            _getwch();
        }

        return code;
    }
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);   // print non-ASCII paths correctly
#ifdef _DEBUG
    constexpr const wchar_t* kTitle = L"UB Debug";      // debug build (bigger, slower, extra checks)
#else
    constexpr const wchar_t* kTitle = L"Lootline";
#endif

    SetConsoleTitleW(kTitle);
    wprintf(L"%ls\n==========\n\n", kTitle);

    std::error_code ec;
    std::filesystem::path dll;
    std::wstring unpackError;

    if (argc > 1)
        dll = argv[1];                          // explicit path wins (drag a DLL onto the exe)
    else if (UnpackPayload(dll, unpackError))
        wprintf(L"Using the built-in Lootline DLL.\n");
    else if (!unpackError.empty())
    {
        wprintf(L"[!] %ls\n", unpackError.c_str());
        return Finish(1);
    }
    else
        dll = DllNextToExe();                   // no built-in DLL: fall back to the file next to the exe

    dll = std::filesystem::absolute(dll, ec);

    if (!std::filesystem::exists(dll, ec))
    {
        wprintf(L"[!] DLL not found:\n    %ls\n\n", dll.c_str());
        wprintf(L"    Build Lootline first, or put %ls next to this exe.\n", kDllName);
        wprintf(L"    (If it was just built, Windows Defender may have quarantined it.)\n");
        return Finish(1);
    }

    wprintf(L"DLL:  %ls\n\n", dll.c_str());

    const std::vector<std::uint8_t> dllBytes = ReadFileBytes(dll);

    if (dllBytes.size() < 4096)
    {
        wprintf(L"[!] The DLL file can't be read (%zu bytes) - your antivirus probably removed or locked it.\n", dllBytes.size());
        wprintf(L"    Add %%LOCALAPPDATA%%\\Lootline to the antivirus exclusions and run this again.\n");
        return Finish(1);
    }

    if (const std::vector<std::wstring> missing = MissingDependencies(dllBytes); !missing.empty())
    {
        wprintf(L"[!] This PC is missing things the DLL needs:\n");

        for (const std::wstring& name : missing)
            wprintf(L"      %ls\n", name.c_str());

        wprintf(L"\n    Names ending in \"D\" (VCRUNTIME140D, MSVCP140D, ucrtbased) come from Visual Studio: rebuild\n");
        wprintf(L"    Lootline with the static runtime, or install the Visual C++ Redistributable for the others.\n");
        return Finish(1);
    }

    DWORD pid = FindProcess(kGameExe);
    if (!pid)
    {
        wprintf(L"Waiting for CS2 to start...\n");
        while (!(pid = FindProcess(kGameExe)))
            Sleep(1000);
    }

    wprintf(L"CS2 found (PID %lu). Waiting for the game to finish loading...\n", pid);

    while (!HasModule(pid, kGameModule))
    {
        if (!IsProcessAlive(pid))
        {
            wprintf(L"[!] CS2 closed before it finished loading.\n");
            return Finish(1);
        }

        Sleep(500);
    }

    // client.dll being mapped doesn't mean the game is done initialising — give it a moment.
    Sleep(3000);

    const std::wstring dllFileName = dll.filename().wstring();

    if (HasModule(pid, dllFileName.c_str()) || HasUnboxer(pid))
    {
        wprintf(L"[i] Lootline is already loaded in CS2. Press Page Down in game (or Unload in the menu) to unload it first.\n");
        return Finish(0);
    }

    wprintf(L"Injecting...\n");

    std::wstring error;
    if (!Inject(pid, dll.wstring(), error))
    {
        wprintf(L"[!] Injection failed: %ls\n", error.c_str());
        return Finish(1);
    }

    // The module list can lag a moment behind LoadLibrary.
    bool listed = false;

    for (int attempt = 0; attempt < 20 && !listed; ++attempt)
    {
        listed = HasModule(pid, dllFileName.c_str()) || HasUnboxer(pid);

        if (!listed)
            Sleep(250);
    }

    if (!listed)
    {
        wprintf(L"[!] Lootline loaded, but CS2 removed it right away.\n\n");
        wprintf(L"    CS2's Trusted Mode does that to third-party DLLs. In Steam: right-click Counter-Strike 2 ->\n");
        wprintf(L"    Properties -> Launch Options, add  -allow_third_party_software  and restart CS2.\n");
        wprintf(L"    (Valve's own switch - it can lower your Trust Factor.)\n");
        return Finish(1);
    }

    wprintf(L"[+] Injected! Press Home in game to open the Lootline menu (or the key you set in its Settings).\n");
    return Finish(0);
}
