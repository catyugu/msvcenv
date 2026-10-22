// msvcenv: capture one real MSVC developer environment and re-inject it fast.
//
//   msvcenv print   [--shell ps1|bat] [--arch x64]   emit injection script
//   msvcenv refresh [--arch x64]                     force a new capture
//   msvcenv status                                   show discovery and cache state
//
// The cache is shell-agnostic: one `key=value` file per architecture, raw values,
// no quoting. Only `print` knows about ps1/bat syntax.
//
// Discovery goes through vswhere.exe, so no toolchain path is hardcoded.

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct NoCase {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

using EnvMap = std::map<std::wstring, std::wstring, NoCase>;

const wchar_t* kIgnoredVars[] = {L"PROMPT"};
const wchar_t* kCacheHeader = L"# msvcenv cache 2";

std::wstring Trim(std::wstring s) {
    size_t begin = s.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return {};
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::wstring Widen(const std::string& bytes) {
    if (bytes.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (size == 0) {
        codepage = CP_ACP;
        size = MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), size);
    return wide;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string bytes(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), size,
                        nullptr, nullptr);
    return bytes;
}

void WriteStdout(const std::wstring& text) {
    std::string bytes = Narrow(text);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
}

void WriteStderr(const std::wstring& text) {
    std::string bytes = Narrow(text + L"\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
}

bool RunCapture(const std::wstring& cmdline, std::string& out, DWORD& exit_code) {
    SECURITY_ATTRIBUTES attrs{sizeof(attrs), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &attrs, 1 << 20)) return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring mutable_cmdline = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_pipe);
    if (!ok) {
        CloseHandle(read_pipe);
        return false;
    }

    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        out.append(buffer, read);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    return true;
}

std::wstring ExecutableDir() {
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, size)).parent_path().wstring();
}

fs::path CacheDir() {
    return fs::path(ExecutableDir()) / L"cache";
}

fs::path CacheFile(const std::wstring& arch) {
    return CacheDir() / (arch + L".env");
}

std::wstring FindVswhere() {
    wchar_t program_files_x86[MAX_PATH * 2]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", program_files_x86,
                                 static_cast<DWORD>(std::size(program_files_x86)))) {
        return {};
    }
    fs::path candidate = fs::path(program_files_x86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
    return fs::exists(candidate) ? candidate.wstring() : std::wstring{};
}

std::wstring FindVcvarsall() {
    std::wstring vswhere = FindVswhere();
    if (vswhere.empty()) return {};
    std::wstring cmdline = L"\"" + vswhere +
                           L"\" -latest -products * -find \"VC\\Auxiliary\\Build\\vcvarsall.bat\"";
    std::string out;
    DWORD exit_code = 0;
    if (!RunCapture(cmdline, out, exit_code) || exit_code != 0) return {};
    std::wstring text = Trim(Widen(out));
    size_t newline = text.find_first_of(L"\r\n");
    if (newline != std::wstring::npos) text = Trim(text.substr(0, newline));
    return fs::exists(text) ? text : std::wstring{};
}

std::wstring FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return L"0";
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return std::to_wstring(stamp.QuadPart);
}

bool Ignored(const std::wstring& name) {
    return std::any_of(std::begin(kIgnoredVars), std::end(kIgnoredVars),
                       [&](const wchar_t* candidate) { return _wcsicmp(name.c_str(), candidate) == 0; });
}

void ParseSetOutput(const std::wstring& text, EnvMap& base, EnvMap& dev) {
    EnvMap* current = &base;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        std::wstring line = Trim(text.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos));
        pos = (end == std::wstring::npos) ? text.size() + 1 : end + 1;
        if (line == L"@@SPLIT@@") {
            current = &dev;
            continue;
        }
        if (line.empty() || line[0] == L'=') continue;
        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos || equals == 0) continue;
        (*current)[line.substr(0, equals)] = line.substr(equals + 1);
    }
}

std::wstring NormalizeDir(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        if (ch == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out += ch;
    }
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

std::vector<std::wstring> SplitPath(const std::wstring& value) {
    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t end = value.find(L';', pos);
        std::wstring part = value.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (!part.empty()) parts.push_back(part);
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    return parts;
}

bool ContainsPath(const std::vector<std::wstring>& paths, const std::wstring& value) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::wstring& entry) {
        return _wcsicmp(entry.c_str(), value.c_str()) == 0;
    });
}

bool IsPathName(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"PATH") == 0;
}

struct Cache {
    std::wstring arch;
    std::wstring vcvarsall;
    std::wstring toolsdir;
    std::wstring file_stamp;
    std::wstring toolset;
    std::wstring sdk;
    std::wstring vscmd;
    std::vector<std::pair<std::wstring, std::wstring>> sets;
    std::vector<std::wstring> unsets;
    std::vector<std::wstring> prepend;
    std::vector<std::wstring> drop;

    bool Empty() const { return arch.empty(); }
};

std::wstring Field(const EnvMap& env, const wchar_t* name) {
    auto it = env.find(name);
    return it == env.end() ? std::wstring{} : it->second;
}

bool LoadCache(const fs::path& file, Cache& cache) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::wstring text = Widen(raw);
    if (text.rfind(kCacheHeader, 0) != 0) return false;

    size_t pos = text.find(L'\n');
    pos = (pos == std::wstring::npos) ? text.size() : pos + 1;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        pos = (end == std::wstring::npos) ? text.size() : end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        std::wstring key = line.substr(0, equals);
        std::wstring value = line.substr(equals + 1);
        if (key == L"arch") cache.arch = value;
        else if (key == L"vcvarsall") cache.vcvarsall = value;
        else if (key == L"toolsdir") cache.toolsdir = value;
        else if (key == L"file_stamp") cache.file_stamp = value;
        else if (key == L"toolset") cache.toolset = value;
        else if (key == L"sdk") cache.sdk = value;
        else if (key == L"vs") cache.vscmd = value;
        else if (key == L"path.prepend") cache.prepend.push_back(value);
        else if (key == L"path.drop") cache.drop.push_back(value);
        else if (key == L"unset") cache.unsets.push_back(value);
        else if (key.rfind(L"env.", 0) == 0) cache.sets.emplace_back(key.substr(4), value);
    }
    return !cache.arch.empty();
}

bool SaveCache(const fs::path& file, const Cache& cache) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::wstring text = std::wstring(kCacheHeader) + L"\n";
    text += L"arch=" + cache.arch + L"\n";
    text += L"vcvarsall=" + cache.vcvarsall + L"\n";
    text += L"toolsdir=" + cache.toolsdir + L"\n";
    text += L"file_stamp=" + cache.file_stamp + L"\n";
    text += L"toolset=" + cache.toolset + L"\n";
    text += L"sdk=" + cache.sdk + L"\n";
    text += L"vs=" + cache.vscmd + L"\n";
    for (const auto& dir : cache.prepend) text += L"path.prepend=" + dir + L"\n";
    for (const auto& dir : cache.drop) text += L"path.drop=" + dir + L"\n";
    for (const auto& name : cache.unsets) text += L"unset=" + name + L"\n";
    for (const auto& entry : cache.sets) text += L"env." + entry.first + L"=" + entry.second + L"\n";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    std::string bytes = Narrow(text);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool CaptureEnvironment(const std::wstring& arch, Cache& cache, std::wstring& error) {
    std::wstring vcvarsall = FindVcvarsall();
    if (vcvarsall.empty()) {
        error = L"vcvarsall.bat not found. Install the component "
                L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64 and retry.";
        return false;
    }

    wchar_t temp_path[MAX_PATH * 2]{};
    GetTempPathW(static_cast<DWORD>(std::size(temp_path)), temp_path);
    fs::path script = fs::path(temp_path) / (L"msvcenv-capture-" + std::to_wstring(GetCurrentProcessId()) + L".cmd");
    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = L"cannot write capture script";
            return false;
        }
        std::wstring body = L"@echo off\r\n"
                            L"chcp 65001 >nul 2>&1\r\n"
                            L"set\r\n"
                            L"echo @@SPLIT@@\r\n"
                            L"call \"" + vcvarsall + L"\" " + arch + L" >nul 2>&1\r\n"
                            L"if errorlevel 1 exit /b 3\r\n"
                            L"set\r\n";
        std::string bytes = Narrow(body);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::string out;
    DWORD exit_code = 0;
    // /d: skip AutoRun, so a cmd startup hook cannot pre-activate the environment
    // and shrink the delta we are trying to measure.
    bool ran = RunCapture(L"cmd.exe /d /c \"" + script.wstring() + L"\"", out, exit_code);
    std::error_code ec;
    fs::remove(script, ec);
    if (!ran) {
        error = L"failed to start cmd.exe";
        return false;
    }
    if (exit_code == 3) {
        error = L"vcvarsall.bat failed for arch " + arch;
        return false;
    }

    EnvMap base;
    EnvMap dev;
    ParseSetOutput(Widen(out), base, dev);
    if (dev.empty()) {
        error = L"no environment was produced by vcvarsall.bat";
        return false;
    }

    Cache fresh;
    fresh.arch = arch;
    fresh.vcvarsall = vcvarsall;
    fresh.toolsdir = NormalizeDir(Field(dev, L"VCToolsInstallDir"));
    fresh.file_stamp = FileStamp(vcvarsall);
    fresh.toolset = Field(dev, L"VCToolsVersion");
    fresh.sdk = Field(dev, L"WindowsSDKVersion");
    fresh.vscmd = Field(dev, L"VSCMD_VER");

    for (const auto& entry : dev) {
        if (IsPathName(entry.first) || Ignored(entry.first)) continue;
        auto it = base.find(entry.first);
        if (it == base.end() || it->second != entry.second) fresh.sets.emplace_back(entry);
    }
    for (const auto& entry : base) {
        if (IsPathName(entry.first) || Ignored(entry.first)) continue;
        if (dev.find(entry.first) == dev.end()) fresh.unsets.push_back(entry.first);
    }

    std::vector<std::wstring> base_paths = SplitPath(Field(base, L"PATH"));
    for (const auto& raw : SplitPath(Field(dev, L"PATH"))) {
        const std::wstring dir = NormalizeDir(raw);
        if (!ContainsPath(base_paths, raw) && !ContainsPath(base_paths, dir) &&
            !ContainsPath(fresh.prepend, dir)) {
            fresh.prepend.push_back(dir);
        }
    }

    Cache previous;
    if (LoadCache(CacheFile(arch), previous)) {
        for (const auto& dir : previous.prepend) {
            if (!ContainsPath(fresh.prepend, dir)) fresh.drop.push_back(dir);
        }
    }

    cache = std::move(fresh);
    return true;
}

bool CacheIsStale(const Cache& cache, const std::wstring& arch) {
    if (cache.Empty() || cache.arch != arch) return true;
    if (cache.vcvarsall.empty() || !fs::exists(cache.vcvarsall)) return true;
    if (!cache.toolsdir.empty() && !fs::exists(cache.toolsdir)) return true;
    return FileStamp(cache.vcvarsall) != cache.file_stamp;
}

std::wstring PsQuote(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        out += ch;
        if (ch == L'\'') out += L'\'';
    }
    return out;
}

std::wstring BatQuote(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        out += ch;
        if (ch == L'%') out += L'%';
    }
    return out;
}

std::wstring SessionPath() {
    wchar_t buffer[32768]{};
    DWORD size = GetEnvironmentVariableW(L"PATH", buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, size);
}

// PATH for the target session: prepended toolchain directories first, stale entries
// of an earlier capture removed, duplicates dropped. Resolved here because the
// result depends on the PATH this process inherited.
std::wstring ResolvePath(const Cache& cache) {
    std::vector<std::wstring> parts;
    for (const auto& dir : cache.prepend) {
        if (!ContainsPath(parts, dir)) parts.push_back(dir);
    }
    for (const auto& dir : SplitPath(SessionPath())) {
        if (ContainsPath(cache.drop, dir) || ContainsPath(parts, dir)) continue;
        parts.push_back(dir);
    }
    std::wstring text;
    for (const auto& part : parts) {
        if (!text.empty()) text += L';';
        text += part;
    }
    return text;
}

std::wstring EmitPs1(const Cache& cache) {
    std::wstring text = L"# generated by msvcenv - do not edit\n";
    for (const auto& entry : cache.sets) {
        if (IsPathName(entry.first)) continue;
        text += L"$env:" + entry.first + L" = '" + PsQuote(entry.second) + L"'\n";
    }
    text += L"$env:PATH = '" + PsQuote(ResolvePath(cache)) + L"'\n";
    for (const auto& name : cache.unsets) {
        text += L"Remove-Item -Path ('Env:' + '" + name + L"') -ErrorAction SilentlyContinue\n";
    }
    return text;
}

std::wstring EmitBat(const Cache& cache) {
    std::wstring text = L"@rem generated by msvcenv - do not edit\r\n";
    for (const auto& entry : cache.sets) {
        if (IsPathName(entry.first)) continue;
        text += L"@set \"" + entry.first + L"=" + BatQuote(entry.second) + L"\"\r\n";
    }
    text += L"@set \"PATH=" + BatQuote(ResolvePath(cache)) + L"\"\r\n";
    for (const auto& name : cache.unsets) text += L"@set \"" + name + L"=\"\r\n";
    return text;
}

struct Options {
    std::wstring command = L"print";
    std::wstring shell = L"ps1";
    std::wstring arch = L"x64";
};

Options Parse(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&](std::wstring& target) {
            if (i + 1 < argc) target = argv[++i];
        };
        if (arg == L"--shell") next(options.shell);
        else if (arg == L"--arch") next(options.arch);
        else if (!arg.empty() && arg[0] != L'-') options.command = arg;
    }
    return options;
}

bool EnsureCache(const std::wstring& arch, bool force, Cache& cache, std::wstring& error) {
    if (!force && LoadCache(CacheFile(arch), cache) && !CacheIsStale(cache, arch)) return true;
    if (!CaptureEnvironment(arch, cache, error)) return false;
    if (!SaveCache(CacheFile(arch), cache)) {
        error = L"cannot write cache file " + CacheFile(arch).wstring();
        return false;
    }
    return true;
}

int CommandStatus(const Options& options) {
    std::wstring out;
    std::wstring vswhere = FindVswhere();
    std::wstring vcvarsall = FindVcvarsall();
    out += L"vswhere:   " + (vswhere.empty() ? std::wstring(L"(not found)") : vswhere) + L"\n";
    out += L"vcvarsall: " + (vcvarsall.empty() ? std::wstring(L"(not found)") : vcvarsall) + L"\n";
    Cache cache;
    if (!LoadCache(CacheFile(options.arch), cache)) {
        out += L"cache:     (empty) - run 'msvcenv refresh'\n";
        WriteStdout(out);
        return 1;
    }
    out += L"cache:     " + CacheFile(options.arch).wstring() + L"\n";
    out += L"arch:      " + cache.arch + L"\n";
    out += L"toolset:   " + cache.toolset + L"\n";
    out += L"sdk:       " + cache.sdk + L"\n";
    out += L"vs:        " + cache.vscmd + L"\n";
    out += L"vars:      " + std::to_wstring(cache.sets.size()) + L" set, " +
           std::to_wstring(cache.unsets.size()) + L" unset, " +
           std::to_wstring(cache.prepend.size()) + L" path entries\n";
    out += std::wstring(L"stale:     ") + (CacheIsStale(cache, options.arch) ? L"yes" : L"no") + L"\n";
    WriteStdout(out);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options = Parse(argc, argv);

    if (options.command == L"status") return CommandStatus(options);
    if (options.command != L"print" && options.command != L"refresh") {
        WriteStderr(L"msvcenv: unknown command '" + options.command + L"'");
        return 2;
    }

    Cache cache;
    std::wstring error;
    if (!EnsureCache(options.arch, options.command == L"refresh", cache, error)) {
        WriteStderr(L"msvcenv: " + error);
        return 2;
    }

    if (options.command == L"refresh") {
        WriteStderr(L"refreshed: toolset=" + cache.toolset + L" sdk=" + cache.sdk + L" vars=" +
                    std::to_wstring(cache.sets.size()) + L" path=" +
                    std::to_wstring(cache.prepend.size()));
        return 0;
    }

    WriteStdout(options.shell == L"bat" ? EmitBat(cache) : EmitPs1(cache));
    return 0;
}
