#define NOMINMAX
#include "pluginscaler/formats/VST2PluginModule.h"
#include <cstdlib>
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/Protocol.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdint>

namespace {

int runVst2Probe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);

    std::cout
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "entry=" << result.entryPoint << '\n'
        << "effect=" << result.effectName << '\n'
        << "vendor=" << result.vendor << '\n'
        << "product=" << result.product << '\n'
        << "uniqueId=" << result.uniqueId << '\n'
        << "version=" << result.version << '\n'
        << "programs=" << result.numPrograms << '\n'
        << "params=" << result.numParams << '\n'
        << "inputs=" << result.numInputs << '\n'
        << "outputs=" << result.numOutputs << '\n'
        << "flags=" << result.flags << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.closed) ? 0 : 2;
}

int writeVst2Manifest(const std::filesystem::path& path,
                      const std::filesystem::path& manifestPath) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);
    if (!(result.loaded && result.opened && result.closed))
        return 20;

    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) return 21;

    out << "format=125A-PluginScaler-VST2-Manifest-1\n"
        << "effect=" << result.effectName << "\n"
        << "vendor=" << result.vendor << "\n"
        << "product=" << result.product << "\n"
        << "uniqueId=" << result.uniqueId << "\n"
        << "version=" << result.version << "\n"
        << "programs=" << result.numPrograms << "\n"
        << "params=" << result.numParams << "\n"
        << "inputs=" << result.numInputs << "\n"
        << "outputs=" << result.numOutputs << "\n"
        << "flags=" << result.flags << "\n";
    for (std::size_t i = 0; i < result.parameterDefaults.size(); ++i)
        out << "param." << i << "=" << result.parameterDefaults[i] << "\n";
    return out ? 0 : 22;
}

int runVst2AudioProbe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probeAudio(path, 48000.0, 64);

    std::cout << std::fixed << std::setprecision(4)
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "configured=" << (result.configured ? 1 : 0) << '\n'
        << "mainsOn=" << (result.mainsOn ? 1 : 0) << '\n'
        << "processed=" << (result.processed ? 1 : 0) << '\n'
        << "mainsOff=" << (result.mainsOff ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "outL=" << result.firstOutputLeft << '\n'
        << "outR=" << result.firstOutputRight << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.configured &&
            result.mainsOn && result.processed && result.mainsOff &&
            result.closed) ? 0 : 3;
}

struct EditorGuiContext {
    pluginscaler::formats::VST2PluginModule* module{nullptr};
    std::mutex* moduleMutex{nullptr};
    HWND surrogate{nullptr};
    HWND editor{nullptr};
    int gdiScalePercent{100};
};

inline constexpr UINT kEditorOpenMessage = WM_APP + 0x125;
inline constexpr UINT kEditorCloseMessage = WM_APP + 0x126;
inline constexpr UINT kEditorShutdownMessage = WM_APP + 0x127;
inline constexpr UINT kEditorCaptureMessage = WM_APP + 0x128;
inline constexpr UINT_PTR kEditorIdleTimer = 0x125A;

struct EditorCaptureRequest {
    std::vector<std::uint8_t>* bytes{nullptr};
};

std::filesystem::path helperDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD len = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size())
        return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(path.data(), len)).parent_path();
}

std::wstring windowText(HWND hwnd) {
    std::array<wchar_t, 512> text{};
    const int len = GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    return len > 0 ? std::wstring(text.data(), static_cast<std::size_t>(len)) : L"";
}

std::wstring windowClass(HWND hwnd) {
    std::array<wchar_t, 512> text{};
    const int len = GetClassNameW(hwnd, text.data(), static_cast<int>(text.size()));
    return len > 0 ? std::wstring(text.data(), static_cast<std::size_t>(len)) : L"";
}

void appendEditorDiagnostic(const std::wstring& line) {
    const auto path = helperDirectory() / L"PluginScaler-EditorDiagnostics.txt";
    std::wofstream out(path, std::ios::app);
    if (out)
        out << line << L"\n";
}

bool saveCaptureBmp(const std::filesystem::path& path,
                    int width, int height,
                    const std::uint8_t* pixels,
                    std::size_t pixelBytes) {
    if (!pixels || width <= 0 || height <= 0)
        return false;

    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(pixelBytes);

    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixelBytes);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    out.write(reinterpret_cast<const char*>(pixels),
              static_cast<std::streamsize>(pixelBytes));
    return static_cast<bool>(out);
}


struct EditorChildCandidate {
    HWND hwnd{nullptr};
    LONG area{0};
};

BOOL CALLBACK largestChildProc(HWND hwnd, LPARAM param) {
    auto* candidate = reinterpret_cast<EditorChildCandidate*>(param);
    if (!candidate || !IsWindow(hwnd))
        return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return TRUE;

    const LONG width = rc.right - rc.left;
    const LONG height = rc.bottom - rc.top;
    const LONG area = width > 0 && height > 0 ? width * height : 0;
    if (area > candidate->area) {
        candidate->hwnd = hwnd;
        candidate->area = area;
    }
    return TRUE;
}

BOOL CALLBACK collectChildProc(HWND hwnd, LPARAM param) {
    auto* windows = reinterpret_cast<std::vector<HWND>*>(param);
    if (windows && IsWindow(hwnd))
        windows->push_back(hwnd);
    return TRUE;
}

std::vector<HWND> editorCaptureCandidates(const EditorGuiContext* ctx) {
    std::vector<HWND> windows;
    if (!ctx) return windows;

    if (ctx->editor && IsWindow(ctx->editor))
        windows.push_back(ctx->editor);

    if (ctx->surrogate && IsWindow(ctx->surrogate)) {
        if (std::find(windows.begin(), windows.end(), ctx->surrogate) == windows.end())
            windows.push_back(ctx->surrogate);

        std::vector<HWND> descendants;
        EnumChildWindows(ctx->surrogate, collectChildProc,
                         reinterpret_cast<LPARAM>(&descendants));
        for (HWND hwnd : descendants) {
            if (std::find(windows.begin(), windows.end(), hwnd) == windows.end())
                windows.push_back(hwnd);
        }
    }
    return windows;
}


using SetDIBitsToDeviceFn = int (WINAPI*)(
    HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
    const VOID*, const BITMAPINFO*, UINT);

std::atomic<int> g_gdiScalePercent{100};
std::atomic<HWND> g_gdiEditorWindow{nullptr};

std::mutex g_gdiFrameMutex;
std::vector<std::uint8_t> g_gdiFramePixels;
std::uint32_t g_gdiFrameWidth{0};
std::uint32_t g_gdiFrameHeight{0};
std::uint32_t g_gdiFrameStride{0};
std::atomic<unsigned> g_gdiBlitDiagCount{0};
std::atomic<unsigned> g_gdiCaptureDiagCount{0};

void captureNativeGdiFrame(const VOID* bits, const BITMAPINFO* bmi) {
    if (!bits || !bmi)
        return;

    const auto& hdr = bmi->bmiHeader;
    const int width = std::abs(hdr.biWidth);
    const int height = std::abs(hdr.biHeight);
    if (width <= 0 || height <= 0 ||
        width > 8192 || height > 8192 ||
        hdr.biCompression != BI_RGB ||
        (hdr.biBitCount != 24 && hdr.biBitCount != 32))
        return;

    const std::size_t srcStride =
        ((static_cast<std::size_t>(width) * hdr.biBitCount + 31u) / 32u) * 4u;
    const std::size_t dstStride = static_cast<std::size_t>(width) * 4u;
    std::vector<std::uint8_t> frame(
        dstStride * static_cast<std::size_t>(height));

    const auto* src = static_cast<const std::uint8_t*>(bits);
    for (int y = 0; y < height; ++y) {
        const int srcY = hdr.biHeight > 0 ? (height - 1 - y) : y;
        const auto* srcRow = src + static_cast<std::size_t>(srcY) * srcStride;
        auto* dstRow = frame.data() + static_cast<std::size_t>(y) * dstStride;

        if (hdr.biBitCount == 32) {
            std::memcpy(dstRow, srcRow, dstStride);
        } else {
            for (int x = 0; x < width; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                dstRow[x * 4 + 3] = 0xFF;
            }
        }
    }

    // Do not publish a transient all-black initialization frame.
    // Some legacy editors paint a black/empty DIB first and the real GUI on
    // the following blit. In that case the wrapper should simply wait for
    // the next useful native frame instead of treating black as valid content.
    std::size_t sampledNonBlack = 0;
    std::uint8_t sampledMax = 0;
    {
        const std::size_t total =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t step = (std::max<std::size_t>)(1, total / 4096u);
        for (std::size_t px = 0; px < total; px += step) {
            const std::size_t i = px * 4u;
            if (i + 2 >= frame.size())
                break;
            const std::uint8_t b = frame[i + 0];
            const std::uint8_t g = frame[i + 1];
            const std::uint8_t r = frame[i + 2];
            const std::uint8_t v = (std::max)({r, g, b});
            sampledMax = (std::max)(sampledMax, v);
            if (v > 8)
                ++sampledNonBlack;
        }
    }

    if (sampledNonBlack == 0 && sampledMax <= 8) {
        appendEditorDiagnostic(L"GDI FRAME ignored transient black frame");
        return;
    }

    std::lock_guard<std::mutex> lock(g_gdiFrameMutex);
    g_gdiFramePixels = std::move(frame);
    g_gdiFrameWidth = static_cast<std::uint32_t>(width);
    g_gdiFrameHeight = static_cast<std::uint32_t>(height);
    g_gdiFrameStride = static_cast<std::uint32_t>(dstStride);
}

thread_local HWND g_gdiCreateParent{nullptr};
thread_local int g_gdiCreateScalePercent{100};

LRESULT CALLBACK gdiCreateCbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND &&
        g_gdiCreateParent &&
        g_gdiCreateScalePercent > 100) {
        auto* create = reinterpret_cast<CBT_CREATEWND*>(lp);
        if (create && create->lpcs &&
            create->lpcs->hwndParent == g_gdiCreateParent) {
            if (create->lpcs->cx > 0)
                create->lpcs->cx = MulDiv(
                    create->lpcs->cx, g_gdiCreateScalePercent, 100);
            if (create->lpcs->cy > 0)
                create->lpcs->cy = MulDiv(
                    create->lpcs->cy, g_gdiCreateScalePercent, 100);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

using ScreenToClientFn = BOOL (WINAPI*)(HWND, LPPOINT);

BOOL WINAPI scaledScreenToClient(HWND hwnd, LPPOINT point) {
    static const auto original = reinterpret_cast<ScreenToClientFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ScreenToClient"));
    if (!original)
        return FALSE;

    POINT before{};
    if (point)
        before = *point;

    const BOOL ok = original(hwnd, point);
    if (!ok || !point)
        return ok;

    const int scale = g_gdiScalePercent.load(std::memory_order_relaxed);
    const HWND editor = g_gdiEditorWindow.load(std::memory_order_relaxed);

    POINT native = *point;
    if (scale > 100 && editor && hwnd == editor) {
        native.x = MulDiv(native.x, 100, scale);
        native.y = MulDiv(native.y, 100, scale);
        *point = native;

        static std::atomic<unsigned> diagCount{0};
        const unsigned n = diagCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 32) {
            POINT unscaledClient = before;
            (void)original(hwnd, &unscaledClient);

            std::wstringstream ss;
            ss << L"INPUT ScreenToClient #" << n
               << L" screen=(" << before.x << L"," << before.y << L")"
               << L" client=(" << unscaledClient.x << L"," << unscaledClient.y << L")"
               << L" mapped=(" << point->x << L"," << point->y << L")";
            appendEditorDiagnostic(ss.str());
        }
    }
    return ok;
}

using ClientToScreenFn = BOOL (WINAPI*)(HWND, LPPOINT);

BOOL WINAPI scaledClientToScreen(HWND hwnd, LPPOINT point) {
    static const auto original = reinterpret_cast<ClientToScreenFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ClientToScreen"));
    if (!original || !point)
        return FALSE;

    const int scale = g_gdiScalePercent.load(std::memory_order_relaxed);
    const HWND editor = g_gdiEditorWindow.load(std::memory_order_relaxed);
    if (scale > 100 && editor && hwnd == editor) {
        point->x = MulDiv(point->x, scale, 100);
        point->y = MulDiv(point->y, scale, 100);
    }
    return original(hwnd, point);
}

int WINAPI scaledSetDIBitsToDevice(
    HDC hdc, int xDest, int yDest, DWORD width, DWORD height,
    int xSrc, int ySrc, UINT startScan, UINT scanLines,
    const VOID* bits, const BITMAPINFO* bmi, UINT colorUse) {
    static const auto original = reinterpret_cast<SetDIBitsToDeviceFn>(
        GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "SetDIBitsToDevice"));
    if (!original)
        return 0;

    const int scale = g_gdiScalePercent.load(std::memory_order_relaxed);

    {
        const unsigned n = g_gdiBlitDiagCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 48) {
            std::wstringstream ss;
            ss << L"GDI BLIT #" << n
               << L" dst=(" << xDest << L"," << yDest << L"," << width << L"x" << height << L")"
               << L" src=(" << xSrc << L"," << ySrc << L")"
               << L" scans=" << startScan << L"+" << scanLines
               << L" bits=" << (bits ? 1 : 0)
               << L" bmi=" << (bmi ? 1 : 0);

            if (bmi) {
                const auto& hdr = bmi->bmiHeader;
                const int dibW = std::abs(hdr.biWidth);
                const int dibH = std::abs(hdr.biHeight);
                ss << L" dib=" << dibW
                   << L"x" << dibH
                   << L" bpp=" << hdr.biBitCount
                   << L" comp=" << hdr.biCompression;

                if (bits && hdr.biCompression == BI_RGB &&
                    (hdr.biBitCount == 24 || hdr.biBitCount == 32) &&
                    dibW > 0 && dibH > 0) {
                    const std::size_t stride =
                        ((static_cast<std::size_t>(dibW) * hdr.biBitCount + 31u) / 32u) * 4u;
                    const auto* raw = static_cast<const std::uint8_t*>(bits);
                    const std::size_t total = static_cast<std::size_t>(dibW) * dibH;
                    const std::size_t step = (std::max<std::size_t>)(1, total / 4096u);
                    std::size_t nonBlack = 0;
                    std::uint32_t hash = 2166136261u;
                    std::uint8_t minV = 255, maxV = 0;

                    for (std::size_t px = 0; px < total; px += step) {
                        const std::size_t y = px / static_cast<std::size_t>(dibW);
                        const std::size_t x = px % static_cast<std::size_t>(dibW);
                        const auto* p = raw + y * stride +
                            x * static_cast<std::size_t>(hdr.biBitCount / 8);
                        const std::uint8_t b = p[0], g = p[1], r = p[2];
                        const std::uint8_t v = (std::max)({r, g, b});
                        minV = (std::min)(minV, v);
                        maxV = (std::max)(maxV, v);
                        if (v > 8) ++nonBlack;
                        hash ^= b; hash *= 16777619u;
                        hash ^= g; hash *= 16777619u;
                        hash ^= r; hash *= 16777619u;
                    }
                    ss << L" sampledNonBlack=" << nonBlack
                       << L" min=" << static_cast<unsigned>(minV)
                       << L" max=" << static_cast<unsigned>(maxV)
                       << L" hash=0x" << std::hex << hash << std::dec;
                }
            }
            appendEditorDiagnostic(ss.str());
        }
    }

    // TV-style scaler source: keep a native BGRA copy of the plugin's own DIB.
    // This avoids PrintWindow/BitBlt entirely for legacy plugins such as Pro-53.
    captureNativeGdiFrame(bits, bmi);

    if (scale <= 100 || !bits || !bmi || width == 0 || height == 0)
        return original(hdc, xDest, yDest, width, height,
                        xSrc, ySrc, startScan, scanLines,
                        bits, bmi, colorUse);

    const int dibWidth = std::abs(bmi->bmiHeader.biWidth);
    const int dibHeight = std::abs(bmi->bmiHeader.biHeight);
    if (dibWidth <= 0 || dibHeight <= 0)
        return original(hdc, xDest, yDest, width, height,
                        xSrc, ySrc, startScan, scanLines,
                        bits, bmi, colorUse);

    const int scaledDibWidth = (std::max)(1, MulDiv(dibWidth, scale, 100));
    const int scaledDibHeight = (std::max)(1, MulDiv(dibHeight, scale, 100));

    // Pro-53's first repaint after the host enlarges its HWND asks
    // SetDIBitsToDevice for the already-scaled window size, while the DIB
    // itself remains native-sized. SetDIBitsToDevice cannot stretch that DIB,
    // so map this exact full-editor case to StretchDIBits.
    const bool fullScaledEditorBlit =
        xDest == 0 && yDest == 0 &&
        static_cast<int>(width) == scaledDibWidth &&
        static_cast<int>(height) == scaledDibHeight &&
        xSrc == 0 &&
        ySrc == dibHeight - static_cast<int>(height) &&
        startScan == 0 &&
        scanLines >= static_cast<UINT>(dibHeight);

    int result = 0;
    SetStretchBltMode(hdc, COLORONCOLOR);

    if (fullScaledEditorBlit) {
        result = StretchDIBits(
            hdc,
            0, 0, scaledDibWidth, scaledDibHeight,
            0, 0, dibWidth, dibHeight,
            bits, bmi, colorUse, SRCCOPY);
    } else {
        // Normal Pro-53 invalidation rectangles remain expressed in native
        // editor coordinates. Scale only the destination rectangle; the
        // source rectangle stays in the native 762x358 DIB coordinate space.
        const int dstX = MulDiv(xDest, scale, 100);
        const int dstY = MulDiv(yDest, scale, 100);
        const int dstW = (std::max)(1, MulDiv(static_cast<int>(width), scale, 100));
        const int dstH = (std::max)(1, MulDiv(static_cast<int>(height), scale, 100));

        result = StretchDIBits(
            hdc,
            dstX, dstY, dstW, dstH,
            xSrc, ySrc, static_cast<int>(width), static_cast<int>(height),
            bits, bmi, colorUse, SRCCOPY);
    }

    if (result == 0 || result == GDI_ERROR)
        return 0;

    // Preserve SetDIBitsToDevice-style success semantics for the legacy code.
    return static_cast<int>(scanLines);
}

bool patchSetDIBitsImport(void* nativeModule, int scalePercent) {
    if (!nativeModule || scalePercent < 100)
        return false;

    auto* base = static_cast<std::uint8_t*>(nativeModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size)
        return false;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);
    const FARPROC target = GetProcAddress(
        GetModuleHandleW(L"gdi32.dll"), "SetDIBitsToDevice");
    if (!target)
        return false;

    for (; imports->Name; ++imports) {
        const char* dllName =
            reinterpret_cast<const char*>(base + imports->Name);
        if (_stricmp(dllName, "GDI32.dll") != 0 &&
            _stricmp(dllName, "gdi32.dll") != 0)
            continue;

        auto* first = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + imports->FirstThunk);
        IMAGE_THUNK_DATA* original = imports->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                  base + imports->OriginalFirstThunk)
            : nullptr;

        for (std::size_t i = 0; first[i].u1.Function; ++i) {
            bool match = false;
            if (original && original[i].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL(original[i].u1.Ordinal)) {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + original[i].u1.AddressOfData);
                match = std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    "SetDIBitsToDevice") == 0;
            } else {
                match = reinterpret_cast<FARPROC>(
                    static_cast<std::uintptr_t>(first[i].u1.Function)) == target;
            }

            if (!match)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    &first[i].u1.Function, sizeof(first[i].u1.Function),
                    PAGE_READWRITE, &oldProtect))
                return false;

            first[i].u1.Function = static_cast<decltype(first[i].u1.Function)>(
                reinterpret_cast<std::uintptr_t>(&scaledSetDIBitsToDevice));
            FlushInstructionCache(
                GetCurrentProcess(), &first[i].u1.Function,
                sizeof(first[i].u1.Function));

            DWORD ignored = 0;
            VirtualProtect(
                &first[i].u1.Function, sizeof(first[i].u1.Function),
                oldProtect, &ignored);

            g_gdiScalePercent.store(scalePercent, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool patchScreenToClientImport(void* nativeModule, int scalePercent) {
    if (!nativeModule || scalePercent <= 100)
        return scalePercent <= 100;

    auto* base = static_cast<std::uint8_t*>(nativeModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size)
        return false;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);
    const FARPROC target = GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "ScreenToClient");
    if (!target)
        return false;

    for (; imports->Name; ++imports) {
        const char* dllName =
            reinterpret_cast<const char*>(base + imports->Name);
        if (_stricmp(dllName, "USER32.dll") != 0 &&
            _stricmp(dllName, "user32.dll") != 0)
            continue;

        auto* first = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + imports->FirstThunk);
        IMAGE_THUNK_DATA* original = imports->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                  base + imports->OriginalFirstThunk)
            : nullptr;

        for (std::size_t i = 0; first[i].u1.Function; ++i) {
            bool match = false;
            if (original && original[i].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL(original[i].u1.Ordinal)) {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + original[i].u1.AddressOfData);
                match = std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    "ScreenToClient") == 0;
            } else {
                match = reinterpret_cast<FARPROC>(
                    static_cast<std::uintptr_t>(first[i].u1.Function)) == target;
            }

            if (!match)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    &first[i].u1.Function, sizeof(first[i].u1.Function),
                    PAGE_READWRITE, &oldProtect))
                return false;

            first[i].u1.Function = static_cast<decltype(first[i].u1.Function)>(
                reinterpret_cast<std::uintptr_t>(&scaledScreenToClient));
            FlushInstructionCache(
                GetCurrentProcess(), &first[i].u1.Function,
                sizeof(first[i].u1.Function));

            DWORD ignored = 0;
            VirtualProtect(
                &first[i].u1.Function, sizeof(first[i].u1.Function),
                oldProtect, &ignored);

            g_gdiScalePercent.store(scalePercent, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}


bool patchClientToScreenImport(void* nativeModule, int scalePercent) {
    if (!nativeModule || scalePercent <= 100)
        return scalePercent <= 100;

    auto* base = static_cast<std::uint8_t*>(nativeModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size)
        return false;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);
    const FARPROC target = GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "ClientToScreen");
    if (!target)
        return false;

    for (; imports->Name; ++imports) {
        const char* dllName =
            reinterpret_cast<const char*>(base + imports->Name);
        if (_stricmp(dllName, "USER32.dll") != 0 &&
            _stricmp(dllName, "user32.dll") != 0)
            continue;

        auto* first = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + imports->FirstThunk);
        IMAGE_THUNK_DATA* original = imports->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                  base + imports->OriginalFirstThunk)
            : nullptr;

        for (std::size_t i = 0; first[i].u1.Function; ++i) {
            bool match = false;
            if (original && original[i].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL(original[i].u1.Ordinal)) {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + original[i].u1.AddressOfData);
                match = std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    "ClientToScreen") == 0;
            } else {
                match = reinterpret_cast<FARPROC>(
                    static_cast<std::uintptr_t>(first[i].u1.Function)) == target;
            }

            if (!match)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    &first[i].u1.Function, sizeof(first[i].u1.Function),
                    PAGE_READWRITE, &oldProtect))
                return false;

            first[i].u1.Function = static_cast<decltype(first[i].u1.Function)>(
                reinterpret_cast<std::uintptr_t>(&scaledClientToScreen));
            FlushInstructionCache(
                GetCurrentProcess(), &first[i].u1.Function,
                sizeof(first[i].u1.Function));

            DWORD ignored = 0;
            VirtualProtect(
                &first[i].u1.Function, sizeof(first[i].u1.Function),
                oldProtect, &ignored);

            g_gdiScalePercent.store(scalePercent, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

struct GdiMouseScaleState {
    WNDPROC original{nullptr};
    int scale{100};
    bool leftDrag{false};
    POINT surfaceStart{};
    POINT nativeStart{};
};

LPARAM mapMouseCoordinates(GdiMouseScaleState* state, UINT msg, LPARAM lp) {
    if (!state || state->scale <= 100)
        return lp;

    const int x = GET_X_LPARAM(lp);
    const int y = GET_Y_LPARAM(lp);

    int nativeX = MulDiv(x, 100, state->scale);
    int nativeY = MulDiv(y, 100, state->scale);

    if (msg == WM_LBUTTONDOWN) {
        state->leftDrag = true;
        state->surfaceStart = {x, y};
        state->nativeStart = {nativeX, nativeY};
    } else if (msg == WM_MOUSEMOVE && state->leftDrag) {
        nativeX = state->nativeStart.x +
                  MulDiv(x - state->surfaceStart.x, 100, state->scale);
        nativeY = state->nativeStart.y +
                  MulDiv(y - state->surfaceStart.y, 100, state->scale);
    } else if (msg == WM_LBUTTONUP && state->leftDrag) {
        nativeX = state->nativeStart.x +
                  MulDiv(x - state->surfaceStart.x, 100, state->scale);
        nativeY = state->nativeStart.y +
                  MulDiv(y - state->surfaceStart.y, 100, state->scale);
        state->leftDrag = false;
    }

    return MAKELPARAM(
        static_cast<short>(nativeX),
        static_cast<short>(nativeY));
}

LRESULT CALLBACK gdiScaledEditorProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<GdiMouseScaleState*>(
        GetPropA(hwnd, "125A.PluginScaler.GdiMouseScale"));
    if (!state || !state->original)
        return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        [[fallthrough]];
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK: {
        const int rawX = GET_X_LPARAM(lp);
        const int rawY = GET_Y_LPARAM(lp);
        const LPARAM mapped = mapMouseCoordinates(state, msg, lp);

        static std::atomic<unsigned> diagCount{0};
        const unsigned n = diagCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 48) {
            std::wstringstream ss;
            ss << L"INPUT WM #" << n
               << L" msg=0x" << std::hex << msg << std::dec
               << L" raw=(" << rawX << L"," << rawY << L")"
               << L" mapped=(" << GET_X_LPARAM(mapped)
               << L"," << GET_Y_LPARAM(mapped) << L")";
            appendEditorDiagnostic(ss.str());
        }

        lp = mapped;
        break;
    }
    default:
        break;
    }

    const WNDPROC original = state->original;
    const LRESULT result = CallWindowProcA(original, hwnd, msg, wp, lp);

    // Pro-53 frequently repaints only tiny dirty rectangles after mouse input.
    // The legacy SetDIBitsToDevice source-rectangle semantics do not map
    // cleanly to our scaled StretchDIBits path, which can leave keys/knobs
    // visually frozen even when the control itself reacted. Request a full
    // editor repaint after real mouse interaction so the already-proven
    // 762x358 -> 1143x537 full-frame path refreshes the visible state.
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
        if (state->leftDrag || msg != WM_MOUSEMOVE) {
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        break;
    default:
        break;
    }

    if (msg == WM_NCDESTROY) {
        RemovePropA(hwnd, "125A.PluginScaler.GdiMouseScale");
        delete state;
    }
    return result;
}

bool installGdiMouseScaling(HWND editor, int scalePercent) {
    if (!editor || !IsWindow(editor) || scalePercent <= 100)
        return scalePercent <= 100;
    if (GetPropA(editor, "125A.PluginScaler.GdiMouseScale"))
        return true;

    auto* state = new (std::nothrow) GdiMouseScaleState{};
    if (!state)
        return false;
    state->scale = scalePercent;

    SetLastError(0);
    const auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(
            editor, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&gdiScaledEditorProc)));
    if (!previous && GetLastError() != 0) {
        delete state;
        return false;
    }

    state->original = previous;
    if (!SetPropA(editor, "125A.PluginScaler.GdiMouseScale", state)) {
        SetWindowLongPtrA(
            editor, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(previous));
        delete state;
        return false;
    }
    return true;
}

LRESULT CALLBACK editorSurrogateProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<EditorGuiContext*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ctx = static_cast<EditorGuiContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) ctx->surrogate = hwnd;
    }

    if (!ctx)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case kEditorOpenMessage: {
        std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
        {
            std::lock_guard<std::mutex> frameLock(g_gdiFrameMutex);
            g_gdiFramePixels.clear();
            g_gdiFrameWidth = 0;
            g_gdiFrameHeight = 0;
            g_gdiFrameStride = 0;
            g_gdiBlitDiagCount.store(0, std::memory_order_relaxed);
            g_gdiCaptureDiagCount.store(0, std::memory_order_relaxed);
        }
        if (ctx->editor && IsWindow(ctx->editor))
            return reinterpret_cast<LRESULT>(ctx->editor);

        if (ctx->gdiScalePercent >= 100) {
            if (!patchSetDIBitsImport(
                    ctx->module->nativeModuleHandle(),
                    ctx->gdiScalePercent)) {
                appendEditorDiagnostic(L"GDI-SCALE SetDIBitsToDevice hook failed");
                return 0;
            }
        }
        if (ctx->gdiScalePercent > 100) {
            if (!patchScreenToClientImport(
                    ctx->module->nativeModuleHandle(),
                    ctx->gdiScalePercent)) {
                appendEditorDiagnostic(L"GDI-SCALE ScreenToClient hook failed");
                return 0;
            }
            if (!patchClientToScreenImport(
                    ctx->module->nativeModuleHandle(),
                    ctx->gdiScalePercent)) {
                appendEditorDiagnostic(L"GDI-SCALE ClientToScreen hook failed");
                return 0;
            }
        }

        HHOOK createHook = nullptr;
        if (ctx->gdiScalePercent > 100) {
            // DPI-style virtualization for the legacy editor: establish the
            // scaled parent geometry before effEditOpen and resize the direct
            // child in the CBT create callback, before its first visible paint.
            pluginscaler::formats::vst2abi::VstRect nativeRect{};
            if (ctx->module->editorRect(nativeRect)) {
                const int nativeWidth = nativeRect.right - nativeRect.left;
                const int nativeHeight = nativeRect.bottom - nativeRect.top;
                const int scaledWidth = MulDiv(
                    nativeWidth, ctx->gdiScalePercent, 100);
                const int scaledHeight = MulDiv(
                    nativeHeight, ctx->gdiScalePercent, 100);
                if (scaledWidth > 0 && scaledHeight > 0) {
                    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0,
                                 scaledWidth, scaledHeight,
                                 SWP_NOACTIVATE | SWP_NOZORDER);
                }
            }

            g_gdiCreateParent = hwnd;
            g_gdiCreateScalePercent = ctx->gdiScalePercent;
            createHook = SetWindowsHookExW(
                WH_CBT, gdiCreateCbtProc, nullptr, GetCurrentThreadId());
            if (!createHook) {
                g_gdiCreateParent = nullptr;
                g_gdiCreateScalePercent = 100;
                appendEditorDiagnostic(L"GDI-SCALE CBT create hook failed");
                return 0;
            }
        }

        const bool editorOpened = ctx->module->openEditor(hwnd);

        if (createHook)
            UnhookWindowsHookEx(createHook);
        g_gdiCreateParent = nullptr;
        g_gdiCreateScalePercent = 100;

        if (!editorOpened)
            return 0;

        EditorChildCandidate candidate{};
        EnumChildWindows(hwnd, largestChildProc,
                         reinterpret_cast<LPARAM>(&candidate));
        HWND child = candidate.hwnd;
        if (!child || !IsWindow(child))
            return 0;

        RECT rc{};
        if (GetClientRect(child, &rc)) {
            const int width = static_cast<int>((std::max)(1L, rc.right - rc.left));
            const int height = static_cast<int>((std::max)(1L, rc.bottom - rc.top));
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, width, height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(child, SW_SHOWNA);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            UpdateWindow(child);
        }

        ctx->editor = child;

        // Re-opened legacy editors do not always receive a second spontaneous
        // paint after their HWND is recreated. Publish one normal paint now so
        // the SetDIBits hook can seed a fresh native framebuffer for every
        // editor-open cycle. This stays on the GUI thread and avoids all
        // PrintWindow/WM_PRINT/BitBlt fallback capture.
        if (ctx->gdiScalePercent > 0 && IsWindow(child)) {
            InvalidateRect(child, nullptr, FALSE);
            UpdateWindow(child);
        }

        if (ctx->gdiScalePercent > 100) {
            g_gdiEditorWindow.store(child, std::memory_order_relaxed);

            // Pro-53 receives real Windows mouse messages on its actual editor
            // HWND. With the editor enlarged to 150%, their LPARAM coordinates
            // are physical/scaled coordinates, while Pro-53's hit-test layout
            // remains in the native 762x358 coordinate space. Subclass only the
            // real editor window and translate those coordinates before calling
            // its original WindowProc. Capture, focus and keyboard routing stay
            // native Windows behaviour.
            if (!installGdiMouseScaling(child, ctx->gdiScalePercent)) {
                appendEditorDiagnostic(L"GDI-SCALE editor mouse-coordinate hook failed");
                return 0;
            }
        }

        {
            std::wstringstream ss;
            RECT er{};
            GetClientRect(child, &er);
            ss << L"OPEN primary hwnd=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(child) << std::dec
               << L" class='" << windowClass(child)
               << L"' title='" << windowText(child)
               << L"' size=" << (er.right-er.left) << L"x" << (er.bottom-er.top)
               << L" style=0x" << std::hex
               << static_cast<unsigned long>(GetWindowLongPtrW(child, GWL_STYLE))
               << L" ex=0x"
               << static_cast<unsigned long>(GetWindowLongPtrW(child, GWL_EXSTYLE));
            appendEditorDiagnostic(ss.str());

            std::vector<HWND> diagnosticChildren;
            EnumChildWindows(hwnd, collectChildProc,
                             reinterpret_cast<LPARAM>(&diagnosticChildren));
            for (HWND w : diagnosticChildren) {
                RECT cr{};
                GetClientRect(w, &cr);
                std::wstringstream cs;
                cs << L"CHILD hwnd=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(w) << std::dec
                   << L" class='" << windowClass(w)
                   << L"' title='" << windowText(w)
                   << L"' size=" << (cr.right-cr.left) << L"x" << (cr.bottom-cr.top)
                   << L" style=0x" << std::hex
                   << static_cast<unsigned long>(GetWindowLongPtrW(w, GWL_STYLE))
                   << L" ex=0x"
                   << static_cast<unsigned long>(GetWindowLongPtrW(w, GWL_EXSTYLE));
                appendEditorDiagnostic(cs.str());
            }
        }

        SetTimer(hwnd, kEditorIdleTimer, 30, nullptr);
        return reinterpret_cast<LRESULT>(child);
    }
    case kEditorCloseMessage: {
        KillTimer(hwnd, kEditorIdleTimer);
        {
            std::lock_guard<std::mutex> frameLock(g_gdiFrameMutex);
            g_gdiFramePixels.clear();
            g_gdiFrameWidth = 0;
            g_gdiFrameHeight = 0;
            g_gdiFrameStride = 0;
        }
        std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
        g_gdiEditorWindow.store(nullptr, std::memory_order_relaxed);
        const bool ok = ctx->module->closeEditor();
        ctx->editor = nullptr;
        ShowWindow(hwnd, SW_HIDE);
        return ok ? 1 : 0;
    }
    case WM_TIMER:
        if (wp == kEditorIdleTimer && ctx->editor && IsWindow(ctx->editor)) {
            std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
            (void)ctx->module->editorIdle();
            UpdateWindow(ctx->editor);
            return 0;
        }
        break;
    case kEditorCaptureMessage: {
        auto* request = reinterpret_cast<EditorCaptureRequest*>(lp);
        if (!request || !request->bytes)
            return 0;

        {
            std::lock_guard<std::mutex> lock(g_gdiFrameMutex);
            const unsigned n = g_gdiCaptureDiagCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 24) {
                std::wstringstream ss;
                ss << L"CAPTURE REQUEST #" << n
                   << L" native=" << (!g_gdiFramePixels.empty() ? 1 : 0)
                   << L" size=" << g_gdiFrameWidth << L"x" << g_gdiFrameHeight
                   << L" stride=" << g_gdiFrameStride
                   << L" bytes=" << g_gdiFramePixels.size();
                appendEditorDiagnostic(ss.str());
            }
            if (!g_gdiFramePixels.empty() &&
                g_gdiFrameWidth > 0 && g_gdiFrameHeight > 0 &&
                g_gdiFrameStride >= g_gdiFrameWidth * 4u) {
                pluginscaler::ipc::EditorBitmapHeader header{};
                header.width = g_gdiFrameWidth;
                header.height = g_gdiFrameHeight;
                header.strideBytes = g_gdiFrameStride;
                request->bytes->resize(
                    sizeof(header) + g_gdiFramePixels.size());
                std::memcpy(request->bytes->data(), &header, sizeof(header));
                std::memcpy(request->bytes->data() + sizeof(header),
                            g_gdiFramePixels.data(), g_gdiFramePixels.size());
                return 1;
            }
        }

        if (ctx->gdiScalePercent > 0) {
            // jBridge's FORCE_GUI_REFRESH compatibility path ultimately uses
            // InvalidateRect on the legacy editor HWND. Mirror that behavior
            // conservatively for TV/GDI mode: when no native DIB has arrived
            // yet, request one normal repaint and return. Do not fall through
            // to PrintWindow/WM_PRINT/BitBlt capture, which proved both blank
            // and destabilizing with Pro-53.
            if (ctx->editor && IsWindow(ctx->editor)) {
                InvalidateRect(ctx->editor, nullptr, FALSE);
                UpdateWindow(ctx->editor);
            }
            return 1;
        }

        const auto candidates = editorCaptureCandidates(ctx);
        if (candidates.empty())
            return 0;

        struct CaptureResult {
            std::vector<std::uint8_t> pixels;
            int width{0};
            int height{0};
            std::size_t score{0};
        };

        CaptureResult best{};

        for (HWND captureWindow : candidates) {
            if (!captureWindow || !IsWindow(captureWindow))
                continue;

            RedrawWindow(captureWindow, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            UpdateWindow(captureWindow);

            RECT rc{};
            if (!GetClientRect(captureWindow, &rc))
                continue;
            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            if (width <= 8 || height <= 8)
                continue;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS,
                                              &bits, nullptr, 0);
            ReleaseDC(nullptr, screen);
            if (!mem || !bitmap || !bits) {
                if (bitmap) DeleteObject(bitmap);
                if (mem) DeleteDC(mem);
                continue;
            }

            HGDIOBJ old = SelectObject(mem, bitmap);
            const std::size_t stride = static_cast<std::size_t>(width) * 4u;
            const std::size_t pixelBytes = stride * static_cast<std::size_t>(height);

            auto clearBits = [&] { std::memset(bits, 0, pixelBytes); };
            auto scorePixels = [&]() -> std::size_t {
                const auto* p = static_cast<const std::uint8_t*>(bits);
                if (pixelBytes < 4) return 0;
                std::size_t score = 0;
                const std::size_t totalPixels =
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
                const std::size_t pixelStep =
                    (std::max<std::size_t>)(1, totalPixels / 8192u);
                std::uint8_t lastR = 0, lastG = 0, lastB = 0;
                bool haveLast = false;
                for (std::size_t pixel = 0; pixel < totalPixels; pixel += pixelStep) {
                    const std::size_t i = pixel * 4u;
                    const std::uint8_t b = p[i + 0];
                    const std::uint8_t g = p[i + 1];
                    const std::uint8_t r = p[i + 2];
                    if (r > 8 || g > 8 || b > 8)
                        score += 2;
                    if (haveLast &&
                        (std::abs(static_cast<int>(r) - static_cast<int>(lastR)) > 4 ||
                         std::abs(static_cast<int>(g) - static_cast<int>(lastG)) > 4 ||
                         std::abs(static_cast<int>(b) - static_cast<int>(lastB)) > 4))
                        ++score;
                    lastR = r; lastG = g; lastB = b; haveLast = true;
                }
                return score;
            };

            std::size_t localBestScore = 0;
            std::vector<std::uint8_t> localBest;
            std::wstring localBestMethod = L"none";

            const auto dumpMethod = [&](const wchar_t* method, std::size_t score) {
                std::wstringstream ss;
                ss << L"CAPTURE hwnd=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(captureWindow) << std::dec
                   << L" class='" << windowClass(captureWindow)
                   << L"' size=" << width << L"x" << height
                   << L" method=" << method << L" score=" << score;
                appendEditorDiagnostic(ss.str());

                std::wstringstream name;
                name << L"PluginScaler-Capture-"
                     << std::hex << reinterpret_cast<std::uintptr_t>(captureWindow)
                     << L"-" << method << L".bmp";
                saveCaptureBmp(helperDirectory() / name.str(),
                               width, height,
                               static_cast<const std::uint8_t*>(bits),
                               pixelBytes);
            };

            clearBits();
            if (PrintWindow(captureWindow, mem, PW_CLIENTONLY | 0x00000002)) {
                const auto score = scorePixels();
                dumpMethod(L"PrintWindow", score);
                if (score > localBestScore) {
                    localBestScore = score;
                    localBestMethod = L"PrintWindow";
                    localBest.assign(static_cast<const std::uint8_t*>(bits),
                                     static_cast<const std::uint8_t*>(bits) + pixelBytes);
                }
            }

            clearBits();
            SendMessageW(captureWindow, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                         PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            {
                const auto score = scorePixels();
                dumpMethod(L"WM_PRINT", score);
                if (score > localBestScore) {
                    localBestScore = score;
                    localBestMethod = L"WM_PRINT";
                    localBest.assign(static_cast<const std::uint8_t*>(bits),
                                     static_cast<const std::uint8_t*>(bits) + pixelBytes);
                }
            }

            clearBits();
            HDC source = GetDC(captureWindow);
            if (source) {
                if (BitBlt(mem, 0, 0, width, height, source, 0, 0, SRCCOPY)) {
                    const auto score = scorePixels();
                    dumpMethod(L"BitBlt", score);
                    if (score > localBestScore) {
                        localBestScore = score;
                        localBestMethod = L"BitBlt";
                        localBest.assign(static_cast<const std::uint8_t*>(bits),
                                         static_cast<const std::uint8_t*>(bits) + pixelBytes);
                    }
                }
                ReleaseDC(captureWindow, source);
            }

            SelectObject(mem, old);
            DeleteObject(bitmap);
            DeleteDC(mem);

            if (localBestScore > best.score && !localBest.empty()) {
                best.score = localBestScore;
                best.width = width;
                best.height = height;
                best.pixels = std::move(localBest);

                std::wstringstream ss;
                ss << L"BEST hwnd=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(captureWindow) << std::dec
                   << L" method=" << localBestMethod
                   << L" score=" << localBestScore;
                appendEditorDiagnostic(ss.str());
            }
        }

        if (best.pixels.empty() || best.score == 0)
            return 0;

        const std::size_t stride = static_cast<std::size_t>(best.width) * 4u;
        pluginscaler::ipc::EditorBitmapHeader header{};
        header.width = static_cast<std::uint32_t>(best.width);
        header.height = static_cast<std::uint32_t>(best.height);
        header.strideBytes = static_cast<std::uint32_t>(stride);

        request->bytes->resize(sizeof(header) + best.pixels.size());
        std::memcpy(request->bytes->data(), &header, sizeof(header));
        std::memcpy(request->bytes->data() + sizeof(header),
                    best.pixels.data(), best.pixels.size());
        return 1;
    }
    case kEditorShutdownMessage:
        KillTimer(hwnd, kEditorIdleTimer);
        DestroyWindow(hwnd);
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runSharedVst2Server(const std::filesystem::path& path,
                        const std::wstring& mappingName,
                        const std::wstring& inputEvent,
                        const std::wstring& outputEvent,
                        const std::wstring& controlPipeName) {
    using namespace pluginscaler;

    ipc::AudioSharedChannel channel;
    if (!channel.open(mappingName, inputEvent, outputEvent)) {
        std::cerr << "error=shared-channel-open\n";
        return 10;
    }

    auto* block = channel.block();
    if (!block) return 11;

    formats::VST2PluginModule module;
    std::mutex moduleMutex;
    std::vector<std::uint8_t> persistedChunk;
    std::int32_t persistedChunkIndex = 0;
    std::atomic<bool> controlStop{false};

    std::int32_t configuredBlockSize = 512;
    std::uint32_t configuredSampleRate = 48000;
    std::uint32_t appliedParameterGeneration = 0;

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        std::string error;
        if (!module.openForProcessing(path, 48000.0, 512, error)) {
            std::cerr << "error=" << error << '\n';
            return 12;
        }
    }

    EditorGuiContext guiContext{};
    guiContext.module = &module;
    guiContext.moduleMutex = &moduleMutex;

    HANDLE guiReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!guiReady)
        return 15;

    std::thread guiThread([&] {
        static const wchar_t* kClassName = L"125A_PluginScaler_EditorSurrogate";
        WNDCLASSW wc{};
        wc.lpfnWndProc = editorSurrogateProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        ATOM atom = RegisterClassW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SetEvent(guiReady);
            return;
        }

        HWND surrogate = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
            L"125A PluginScaler Editor Surrogate",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 32, 32,
            nullptr, nullptr, GetModuleHandleW(nullptr), &guiContext);
        SetEvent(guiReady);
        if (!surrogate)
            return;

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });

    WaitForSingleObject(guiReady, 3000);
    CloseHandle(guiReady);
    if (!guiContext.surrogate) {
        if (guiThread.joinable()) guiThread.join();
        return 16;
    }

    auto readExact = [](HANDLE pipe, void* data, DWORD bytes) -> bool {
        auto* p = static_cast<std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD got = 0;
            if (!ReadFile(pipe, p + done, bytes - done, &got, nullptr) || got == 0)
                return false;
            done += got;
        }
        return true;
    };
    auto writeExact = [](HANDLE pipe, const void* data, DWORD bytes) -> bool {
        const auto* p = static_cast<const std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD sent = 0;
            if (!WriteFile(pipe, p + done, bytes - done, &sent, nullptr) || sent == 0)
                return false;
            done += sent;
        }
        return true;
    };

    HANDLE controlServerPipe = CreateNamedPipeW(
        controlPipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (controlServerPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "error=control-pipe-create\n";
        return 14;
    }

    std::thread controlThread([&] {
        HANDLE pipe = controlServerPipe;

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            return;
        }

        while (!controlStop.load(std::memory_order_acquire)) {
            ipc::ControlMessageHeader req{};
            if (!readExact(pipe, &req, sizeof(req)))
                break;

            ipc::ControlMessageHeader resp{};
            resp.command = req.command;
            resp.arg0 = req.arg0;

            if (req.magic != ipc::kControlMagic ||
                req.version != ipc::kControlVersion ||
                req.payloadBytes > ipc::kMaxControlPayload) {
                resp.status = ipc::ControlStatus::InvalidRequest;
                if (!writeExact(pipe, &resp, sizeof(resp))) break;
                continue;
            }

            std::vector<std::uint8_t> payload(req.payloadBytes);
            if (req.payloadBytes &&
                !readExact(pipe, payload.data(), req.payloadBytes))
                break;

            std::vector<std::uint8_t> reply;

            if (req.command == ipc::ControlCommand::OpenEditor) {
                if (!payload.empty()) {
                    resp.status = ipc::ControlStatus::InvalidRequest;
                } else {
                    guiContext.gdiScalePercent =
                        req.arg0 >= 100 && req.arg0 <= 400
                            ? static_cast<int>(req.arg0)
                            : 0;
                    const LRESULT editorResult = SendMessageW(
                        guiContext.surrogate, kEditorOpenMessage, 0, 0);
                    HWND editor = reinterpret_cast<HWND>(editorResult);
                    if (!editor || !IsWindow(editor)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        ipc::EditorOpenResult result{};
                        result.surrogateWindow = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(guiContext.surrogate));
                        result.editorWindow = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(editor));
                        reply.resize(sizeof(result));
                        std::memcpy(reply.data(), &result, sizeof(result));
                    }
                }
            } else if (req.command == ipc::ControlCommand::CloseEditor) {
                if (!SendMessageW(guiContext.surrogate,
                                  kEditorCloseMessage, 0, 0))
                    resp.status = ipc::ControlStatus::PluginError;
            } else if (req.command == ipc::ControlCommand::CaptureEditor) {
                EditorCaptureRequest capture{};
                capture.bytes = &reply;
                if (!SendMessageW(guiContext.surrogate,
                                  kEditorCaptureMessage, 0,
                                  reinterpret_cast<LPARAM>(&capture)))
                    resp.status = ipc::ControlStatus::PluginError;
            } else if (req.command == ipc::ControlCommand::SendEditorMouse) {
                if (payload.size() != sizeof(ipc::EditorMousePayload) ||
                    !guiContext.editor || !IsWindow(guiContext.editor)) {
                    resp.status = ipc::ControlStatus::InvalidRequest;
                } else {
                    ipc::EditorMousePayload mouse{};
                    std::memcpy(&mouse, payload.data(), sizeof(mouse));
                    const UINT message = static_cast<UINT>(mouse.message);
                    const bool allowed =
                        message == WM_MOUSEMOVE ||
                        message == WM_LBUTTONDOWN ||
                        message == WM_LBUTTONUP ||
                        message == WM_RBUTTONDOWN ||
                        message == WM_RBUTTONUP;
                    if (!allowed) {
                        resp.status = ipc::ControlStatus::InvalidRequest;
                    } else {
                        const LPARAM coords = MAKELPARAM(
                            static_cast<short>(mouse.x),
                            static_cast<short>(mouse.y));
                        if (message == WM_LBUTTONDOWN ||
                            message == WM_RBUTTONDOWN) {
                            SetFocus(guiContext.editor);
                        }
                        SendMessageW(guiContext.editor, message,
                                     static_cast<WPARAM>(mouse.keyFlags), coords);
                    }
                }
            } else if (req.command == ipc::ControlCommand::Shutdown) {
                controlStop.store(true, std::memory_order_release);
            } else {
                std::lock_guard<std::mutex> lock(moduleMutex);
                switch (req.command) {
                case ipc::ControlCommand::GetState: {
                    if (!module.getChunk(req.arg0, reply)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = reply;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                }
                case ipc::ControlCommand::SetState:
                    if (payload.empty() ||
                        !module.setChunk(req.arg0, payload.data(), payload.size())) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = payload;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                case ipc::ControlCommand::GetParameters: {
                    const auto count = std::max<std::int32_t>(0, module.numParams());
                    reply.resize(static_cast<std::size_t>(count) * sizeof(float));
                    auto* values = reinterpret_cast<float*>(reply.data());
                    for (std::int32_t i = 0; i < count; ++i)
                        values[i] = module.getParameter(i);
                    break;
                }
                case ipc::ControlCommand::GetEditorRect: {
                    formats::vst2abi::VstRect rect{};
                    if (!module.editorRect(rect)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        ipc::EditorRectPayload out{};
                        out.left = rect.left;
                        out.top = rect.top;
                        out.right = rect.right;
                        out.bottom = rect.bottom;
                        reply.resize(sizeof(out));
                        std::memcpy(reply.data(), &out, sizeof(out));
                    }
                    break;
                }
                default:
                    resp.status = ipc::ControlStatus::InvalidRequest;
                    break;
                }
            }

            if (reply.size() > ipc::kMaxControlPayload) {
                reply.clear();
                resp.status = ipc::ControlStatus::PayloadTooLarge;
            }
            resp.responseBytes = static_cast<std::uint32_t>(reply.size());

            if (!writeExact(pipe, &resp, sizeof(resp))) break;
            if (!reply.empty() &&
                !writeExact(pipe, reply.data(), resp.responseBytes))
                break;

            if (req.command == ipc::ControlCommand::Shutdown)
                break;
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });

    int resultCode = 0;
    for (;;) {
        if (!channel.waitForInput(std::chrono::seconds(10))) {
            if (controlStop.load(std::memory_order_acquire))
                break;
            std::cerr << "error=input-timeout\n";
            resultCode = 13;
            break;
        }

        const auto state = static_cast<ipc::AudioBlockState>(
            block->header.state.load(std::memory_order_acquire));

        if (state == ipc::AudioBlockState::Shutdown)
            break;

        if (state != ipc::AudioBlockState::InputReady) {
            block->header.errorCode = 101;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        if (block->header.frames == 0 || block->header.frames > ipc::kMaxAudioFrames ||
            block->header.sampleRateHz == 0 ||
            block->header.inputChannels > ipc::kMaxAudioChannels ||
            block->header.outputChannels > ipc::kMaxAudioChannels ||
            block->header.midiEventCount > ipc::kMaxMidiEvents ||
            block->header.parameterCount > ipc::kMaxParameters) {
            block->header.errorCode = 102;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Processing),
                                  std::memory_order_release);

        bool ok = true;
        {
            std::lock_guard<std::mutex> lock(moduleMutex);

            const auto requestedBlockSize = static_cast<std::int32_t>(block->header.frames);
            const auto requestedSampleRate = block->header.sampleRateHz;
            if (configuredBlockSize != requestedBlockSize ||
                configuredSampleRate != requestedSampleRate) {
                module.close();

                std::string error;
                if (!module.openForProcessing(path, static_cast<double>(requestedSampleRate),
                                              requestedBlockSize, error)) {
                    std::cerr << "error=" << error << '\n';
                    ok = false;
                } else {
                    if (guiContext.gdiScalePercent > 100) {
                        (void)patchSetDIBitsImport(
                            module.nativeModuleHandle(),
                            guiContext.gdiScalePercent);
                        (void)patchScreenToClientImport(
                            module.nativeModuleHandle(),
                            guiContext.gdiScalePercent);
                        (void)patchClientToScreenImport(
                            module.nativeModuleHandle(),
                            guiContext.gdiScalePercent);
                    }
                    configuredBlockSize = requestedBlockSize;
                    configuredSampleRate = requestedSampleRate;
                    appliedParameterGeneration = 0;
                    if (!persistedChunk.empty())
                        ok = module.setChunk(persistedChunkIndex,
                                             persistedChunk.data(),
                                             persistedChunk.size());
                }
            }

            if (ok && block->header.parameterCount > 0 &&
                appliedParameterGeneration != block->header.parameterGeneration) {
                for (std::uint32_t i = 0; i < block->header.parameterCount; ++i) {
                    if (!module.setParameter(static_cast<std::int32_t>(i),
                                             block->parameterValues[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    appliedParameterGeneration = block->header.parameterGeneration;
            }

            if (ok && block->header.midiEventCount > 0) {
                std::vector<formats::vst2abi::VstMidiEvent> midi(
                    static_cast<std::size_t>(block->header.midiEventCount));
                for (std::uint32_t i = 0; i < block->header.midiEventCount; ++i) {
                    auto& dst = midi[static_cast<std::size_t>(i)];
                    const auto& src = block->midiEvents[i];
                    dst.type = formats::vst2abi::kVstMidiType;
                    dst.byteSize = sizeof(formats::vst2abi::VstMidiEvent);
                    dst.deltaFrames = src.deltaFrames;
                    dst.flags = src.flags;
                    for (int b = 0; b < 4; ++b)
                        dst.midiData[b] = static_cast<char>(src.data[b]);
                }
                ok = module.processMidiEvents(midi.data(),
                                              static_cast<std::int32_t>(midi.size()));
            }

            if (ok) {
                const auto inCount = std::max<std::uint32_t>(1, block->header.inputChannels);
                const auto outCount = std::max<std::uint32_t>(1, block->header.outputChannels);
                std::vector<float*> inputs(inCount);
                std::vector<float*> outputs(outCount);
                for (std::uint32_t ch = 0; ch < inCount; ++ch)
                    inputs[ch] = block->inputs[ch];
                for (std::uint32_t ch = 0; ch < outCount; ++ch)
                    outputs[ch] = block->outputs[ch];

                ok = module.processReplacing(
                    inputs.data(), outputs.data(),
                    static_cast<std::int32_t>(block->header.frames));
            }
        }

        block->header.errorCode = ok ? 0u : 103u;
        block->header.state.store(static_cast<std::uint32_t>(
                                      ok ? ipc::AudioBlockState::OutputReady
                                         : ipc::AudioBlockState::Error),
                                  std::memory_order_release);
        channel.signalOutput();
    }

    controlStop.store(true, std::memory_order_release);

    // Wake a blocked control pipe server during normal audio shutdown.
    HANDLE wake = CreateFileW(controlPipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE)
        CloseHandle(wake);

    if (controlThread.joinable())
        controlThread.join();

    if (guiContext.surrogate && IsWindow(guiContext.surrogate))
        SendMessageW(guiContext.surrogate, kEditorShutdownMessage, 0, 0);
    if (guiThread.joinable())
        guiThread.join();

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        module.close();
    }
    return resultCode;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2")
        return runVst2Probe(argv[2]);

    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2-audio")
        return runVst2AudioProbe(argv[2]);

    if (argc == 4 && std::wstring_view(argv[1]) == L"--write-vst2-manifest")
        return writeVst2Manifest(argv[2], argv[3]);

    if (argc == 7 && std::wstring_view(argv[1]) == L"--serve-vst2-shm")
        return runSharedVst2Server(argv[2], argv[3], argv[4], argv[5], argv[6]);

    std::cout << "125A PluginScaler Helper\n"
              << "protocol=" << pluginscaler::ipc::kProtocolMajor << "."
              << pluginscaler::ipc::kProtocolMinor << "\n"
              << "usage:\n"
              << "  PluginScalerHelper --probe-vst2 <plugin.dll>\n"
              << "  PluginScalerHelper --probe-vst2-audio <plugin.dll>\n"
              << "  PluginScalerHelper --write-vst2-manifest <plugin.dll> <manifest.txt>\n"
              << "  PluginScalerHelper --serve-vst2-shm <plugin.dll> <map> <in-event> <out-event> <control-pipe>\n";
    return 0;
}
