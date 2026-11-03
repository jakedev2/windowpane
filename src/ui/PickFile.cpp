#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PickFile.hpp"

#include <Geode/loader/Loader.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

namespace wp::ui {

namespace {

std::atomic<bool> g_open{false};                       // a dialog is open right now
std::chrono::steady_clock::time_point g_closedAt{};    // when the last one closed (main thread only)

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Claims the single dialog slot. Ignores requests while a dialog is open and for a moment after one closed
// (a stray repeat of the click / Enter key that closed the dialog must not open it again).
bool tryBegin() {
    using namespace std::chrono;
    if (g_open.load()) return false;
    if (g_closedAt.time_since_epoch().count() != 0 && steady_clock::now() - g_closedAt < milliseconds(600)) return false;
    bool expected = false;
    return g_open.compare_exchange_strong(expected, true);
}

// Runs `show` (which opens the dialog and returns the path or nullopt) on a worker thread, then calls `done` on the
// game's main thread.
void runDialog(std::function<std::optional<std::filesystem::path>(HWND)> show, PickDone done) {
    HWND owner = GetActiveWindow();   // must be read on the thread that owns the game window
    if (!owner) owner = GetForegroundWindow();
    std::thread([owner, show = std::move(show), done = std::move(done)]() mutable {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::optional<std::filesystem::path> result;
        try {
            result = show(owner);
        } catch (...) {
            result = std::nullopt;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        geode::Loader::get()->queueInMainThread([result = std::move(result), done = std::move(done)]() mutable {
            g_closedAt = std::chrono::steady_clock::now();
            g_open.store(false);
            if (done) done(std::move(result));
        });
    }).detach();
}

}  // namespace

bool nativePickFileAsync(std::string const& description, std::vector<std::string> const& patterns, PickDone done) {
    if (!tryBegin()) return false;
    // filter string: "Description\0*.a;*.b\0All files\0*.*\0\0"
    std::wstring spec;
    for (size_t i = 0; i < patterns.size(); ++i) {
        if (i) spec += L';';
        spec += widen(patterns[i]);
    }
    if (spec.empty()) spec = L"*.*";
    std::wstring filter = widen(description);
    filter.push_back(L'\0');
    filter += spec;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    runDialog([filter = std::move(filter)](HWND owner) -> std::optional<std::filesystem::path> {
        std::wstring buffer(4096, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter.c_str();
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&ofn)) return std::nullopt;   // cancelled (or failed)
        return std::filesystem::path(std::wstring(buffer.c_str()));
    }, std::move(done));
    return true;
}

bool nativeSaveFileAsync(std::string const& defaultName, std::string const& extension, std::string const& description, PickDone done) {
    if (!tryBegin()) return false;
    std::wstring ext = widen(extension);
    std::wstring filter = widen(description);
    filter.push_back(L'\0');
    filter += L"*." + ext;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::wstring def = widen(defaultName);
    for (auto& ch : def) if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') ch = L'_';

    runDialog([filter = std::move(filter), ext = std::move(ext), def = std::move(def)](HWND owner) -> std::optional<std::filesystem::path> {
        std::wstring buffer(4096, L'\0');
        def.copy(buffer.data(), std::min(def.size(), buffer.size() - 1));
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter.c_str();
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrDefExt = ext.c_str();
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if (!GetSaveFileNameW(&ofn)) return std::nullopt;
        return std::filesystem::path(std::wstring(buffer.c_str()));
    }, std::move(done));
    return true;
}

}  // namespace wp::ui

#else

namespace wp::ui {
bool nativePickFileAsync(std::string const&, std::vector<std::string> const&, PickDone done) { if (done) done(std::nullopt); return true; }
bool nativeSaveFileAsync(std::string const&, std::string const&, std::string const&, PickDone done) { if (done) done(std::nullopt); return true; }
}  // namespace wp::ui

#endif
