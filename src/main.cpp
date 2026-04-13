#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

constexpr int ID_BTN_RELOAD = 1001;
constexpr int ID_BTN_SELECT = 1002;
constexpr int ID_BTN_START  = 1003;
constexpr int ID_STATUS     = 1004;
constexpr int ID_CHK_INVERT = 1005;

constexpr UINT WM_APP_RECT_SELECTED = WM_APP + 1;
constexpr UINT WM_APP_DRAW_FINISHED = WM_APP + 2;

const wchar_t* MAIN_CLASS    = L"PaintDrawMainWindow2";
const wchar_t* OVERLAY_CLASS = L"PaintDrawOverlayWindow2";
const wchar_t* PREVIEW_CLASS = L"PaintDrawPreviewWindow2";

HINSTANCE g_hInst = nullptr;
HWND g_mainWnd = nullptr;
HWND g_statusWnd = nullptr;
HWND g_previewWnd = nullptr;
HWND g_overlayWnd = nullptr;
HWND g_chkInvert = nullptr;

ULONG_PTR g_gdiplusToken = 0;
Bitmap* g_bitmap = nullptr;
std::wstring g_imagePath;
RECT g_targetRect{ 0, 0, 0, 0 };
bool g_hasTargetRect = false;
bool g_isDrawing = false;
bool g_invert = false;

std::vector<unsigned char> g_previewMask;
int g_previewW = 0;
int g_previewH = 0;

struct OverlayState
{
    bool dragging = false;
    POINT start{ 0, 0 };
    POINT current{ 0, 0 };
};

std::wstring GetExeDirImagePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        path = path.substr(0, pos + 1);
    return path + L"image.png";
}

void SetStatus(const std::wstring& text)
{
    if (g_statusWnd)
        SetWindowTextW(g_statusWnd, text.c_str());
}

void DeleteLoadedBitmap()
{
    if (g_bitmap)
    {
        delete g_bitmap;
        g_bitmap = nullptr;
    }
}

RECT NormalizeRect(POINT a, POINT b)
{
    RECT r{};
    r.left   = min(a.x, b.x);
    r.top    = min(a.y, b.y);
    r.right  = max(a.x, b.x);
    r.bottom = max(a.y, b.y);
    return r;
}

void MoveMouse(int x, int y)
{
    SetCursorPos(x, y);
}

void MouseLeftDown()
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseLeftUp()
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void ClickPixel(int x, int y)
{
    MoveMouse(x, y);
    Sleep(1);
    MouseLeftDown();
    Sleep(1);
    MouseLeftUp();
}

void DrawRun(int x1, int y, int x2)
{
    if (x1 == x2)
    {
        ClickPixel(x1, y);
        return;
    }

    MoveMouse(x1, y);
    Sleep(1);
    MouseLeftDown();
    Sleep(1);
    MoveMouse(x2, y);
    Sleep(1);
    MouseLeftUp();
}

double ClampGray(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 255.0) return 255.0;
    return v;
}

bool BuildScaledDitherMask(Bitmap* src, int outW, int outH, bool invert, std::vector<unsigned char>& mask)
{
    if (!src || outW <= 0 || outH <= 0)
        return false;

    Bitmap scaled(outW, outH, PixelFormat32bppARGB);
    Graphics g(&scaled);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(255, 255, 255, 255));
    g.DrawImage(src, Rect(0, 0, outW, outH));

    std::vector<double> gray(static_cast<size_t>(outW) * static_cast<size_t>(outH), 255.0);
    mask.assign(static_cast<size_t>(outW) * static_cast<size_t>(outH), 0);

    for (int y = 0; y < outH; ++y)
    {
        for (int x = 0; x < outW; ++x)
        {
            Color c;
            if (scaled.GetPixel(x, y, &c) != Ok)
                continue;

            double gval = 255.0;
            if (c.GetAlpha() >= 10)
            {
                gval = 0.299 * c.GetRed() + 0.587 * c.GetGreen() + 0.114 * c.GetBlue();
            }

            if (invert)
                gval = 255.0 - gval;

            gray[static_cast<size_t>(y) * outW + x] = gval;
        }
    }

    for (int y = 0; y < outH; ++y)
    {
        for (int x = 0; x < outW; ++x)
        {
            const size_t idx = static_cast<size_t>(y) * outW + x;
            double oldPixel = ClampGray(gray[idx]);
            double newPixel = (oldPixel < 128.0) ? 0.0 : 255.0;
            double err = oldPixel - newPixel;
            gray[idx] = newPixel;
            mask[idx] = (newPixel == 0.0) ? 1 : 0;

            if (x + 1 < outW)
                gray[idx + 1] += err * 7.0 / 16.0;
            if (y + 1 < outH)
            {
                if (x > 0)
                    gray[idx + outW - 1] += err * 3.0 / 16.0;
                gray[idx + outW] += err * 5.0 / 16.0;
                if (x + 1 < outW)
                    gray[idx + outW + 1] += err * 1.0 / 16.0;
            }
        }
    }

    return true;
}

void UpdatePreview()
{
    g_previewMask.clear();
    g_previewW = 0;
    g_previewH = 0;

    if (!g_bitmap)
    {
        if (g_previewWnd)
            InvalidateRect(g_previewWnd, nullptr, TRUE);
        return;
    }

    int srcW = static_cast<int>(g_bitmap->GetWidth());
    int srcH = static_cast<int>(g_bitmap->GetHeight());

    if (g_hasTargetRect)
    {
        srcW = g_targetRect.right - g_targetRect.left;
        srcH = g_targetRect.bottom - g_targetRect.top;
    }

    if (srcW <= 0 || srcH <= 0)
    {
        if (g_previewWnd)
            InvalidateRect(g_previewWnd, nullptr, TRUE);
        return;
    }

    const int maxPreview = 320;
    double scale = 1.0;
    if (srcW > maxPreview || srcH > maxPreview)
        scale = min(static_cast<double>(maxPreview) / srcW, static_cast<double>(maxPreview) / srcH);

    g_previewW = max(1, static_cast<int>(std::round(srcW * scale)));
    g_previewH = max(1, static_cast<int>(std::round(srcH * scale)));

    BuildScaledDitherMask(g_bitmap, g_previewW, g_previewH, g_invert, g_previewMask);

    if (g_previewWnd)
        InvalidateRect(g_previewWnd, nullptr, TRUE);
}

bool LoadImageFromDisk(std::wstring& error)
{
    DeleteLoadedBitmap();

    g_bitmap = new Bitmap(g_imagePath.c_str());
    if (!g_bitmap || g_bitmap->GetLastStatus() != Ok)
    {
        DeleteLoadedBitmap();
        error = L"Could not open image.png. Put it next to the EXE.";
        UpdatePreview();
        return false;
    }

    UpdatePreview();

    std::wstringstream ss;
    ss << L"Loaded image.png: " << g_bitmap->GetWidth() << L"x" << g_bitmap->GetHeight();
    if (g_hasTargetRect)
    {
        ss << L" | Target: "
           << (g_targetRect.right - g_targetRect.left) << L"x"
           << (g_targetRect.bottom - g_targetRect.top);
    }
    ss << L" | Mode: dither";
    SetStatus(ss.str());
    return true;
}

DWORD WINAPI DrawThreadProc(LPVOID)
{
    if (!g_bitmap || !g_hasTargetRect)
    {
        PostMessageW(g_mainWnd, WM_APP_DRAW_FINISHED, FALSE, 0);
        return 0;
    }

    RECT target = g_targetRect;
    int drawW = target.right - target.left;
    int drawH = target.bottom - target.top;

    std::vector<unsigned char> mask;
    if (!BuildScaledDitherMask(g_bitmap, drawW, drawH, g_invert, mask))
    {
        PostMessageW(g_mainWnd, WM_APP_DRAW_FINISHED, FALSE, 0);
        return 0;
    }

    Sleep(2500);

    bool stopped = false;

    for (int y = 0; y < drawH; ++y)
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            stopped = true;
            break;
        }

        int x = 0;
        while (x < drawW)
        {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                stopped = true;
                break;
            }

            if (mask[static_cast<size_t>(y) * drawW + x] == 0)
            {
                ++x;
                continue;
            }

            int startX = x;
            while ((x + 1) < drawW && mask[static_cast<size_t>(y) * drawW + (x + 1)] == 1)
                ++x;
            int endX = x;

            DrawRun(target.left + startX, target.top + y, target.left + endX);
            ++x;
        }

        if (stopped)
            break;
    }

    PostMessageW(g_mainWnd, WM_APP_DRAW_FINISHED, stopped ? 2 : 1, 0);
    return 0;
}

void StartOverlaySelection(HWND owner)
{
    if (g_overlayWnd)
        return;

    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_overlayWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        OVERLAY_CLASS,
        L"",
        WS_POPUP,
        x, y, w, h,
        owner,
        nullptr,
        g_hInst,
        nullptr
    );

    if (!g_overlayWnd)
        return;

    SetLayeredWindowAttributes(g_overlayWnd, 0, 90, LWA_ALPHA);
    ShowWindow(g_overlayWnd, SW_SHOW);
    UpdateWindow(g_overlayWnd);
    SetForegroundWindow(g_overlayWnd);
}

void LayoutMainWindow(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);

    int margin = 12;
    int btnW = 150;
    int btnH = 34;
    int gap = 8;

    HWND btnReload = GetDlgItem(hwnd, ID_BTN_RELOAD);
    HWND btnSelect = GetDlgItem(hwnd, ID_BTN_SELECT);
    HWND btnStart  = GetDlgItem(hwnd, ID_BTN_START);

    MoveWindow(btnReload, margin, margin, btnW, btnH, TRUE);
    MoveWindow(btnSelect, margin + btnW + gap, margin, btnW, btnH, TRUE);
    MoveWindow(btnStart,  margin + (btnW + gap) * 2, margin, btnW, btnH, TRUE);
    MoveWindow(g_chkInvert, margin + (btnW + gap) * 3, margin + 7, 120, 24, TRUE);

    int top = margin + btnH + 12;
    int previewW = 360;
    int previewH = rc.bottom - top - margin;
    if (previewH < 160)
        previewH = 160;

    MoveWindow(g_previewWnd, margin, top, previewW, previewH, TRUE);
    MoveWindow(g_statusWnd, margin + previewW + 12, top, rc.right - (margin + previewW + 24), previewH, TRUE);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH back = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, back);
        DeleteObject(back);

        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);

        if (!g_previewMask.empty() && g_previewW > 0 && g_previewH > 0)
        {
            std::vector<unsigned int> pixels(static_cast<size_t>(g_previewW) * static_cast<size_t>(g_previewH), 0xFFFFFFFFu);
            for (int y = 0; y < g_previewH; ++y)
            {
                for (int x = 0; x < g_previewW; ++x)
                {
                    if (g_previewMask[static_cast<size_t>(y) * g_previewW + x])
                        pixels[static_cast<size_t>(y) * g_previewW + x] = 0xFF000000u;
                }
            }

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_previewW;
            bmi.bmiHeader.biHeight = -g_previewH;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            int availW = (rc.right - rc.left) - 20;
            int availH = (rc.bottom - rc.top) - 20;
            double scale = min(static_cast<double>(availW) / g_previewW, static_cast<double>(availH) / g_previewH);
            if (scale <= 0.0)
                scale = 1.0;
            int drawW = max(1, static_cast<int>(std::round(g_previewW * scale)));
            int drawH = max(1, static_cast<int>(std::round(g_previewH * scale)));
            int dx = rc.left + ((rc.right - rc.left) - drawW) / 2;
            int dy = rc.top + ((rc.bottom - rc.top) - drawH) / 2;

            StretchDIBits(
                hdc,
                dx, dy, drawW, drawH,
                0, 0, g_previewW, g_previewH,
                pixels.data(),
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );
        }
        else
        {
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, L"Preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    OverlayState* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* s = new OverlayState();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        if (!state) return 0;
        state->dragging = true;
        state->start.x = GET_X_LPARAM(lParam);
        state->start.y = GET_Y_LPARAM(lParam);
        state->current = state->start;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!state || !state->dragging) return 0;
        state->current.x = GET_X_LPARAM(lParam);
        state->current.y = GET_Y_LPARAM(lParam);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!state || !state->dragging) return 0;
        state->dragging = false;
        state->current.x = GET_X_LPARAM(lParam);
        state->current.y = GET_Y_LPARAM(lParam);
        ReleaseCapture();

        RECT localRect = NormalizeRect(state->start, state->current);
        POINT tl{ localRect.left, localRect.top };
        POINT br{ localRect.right, localRect.bottom };
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);

        g_targetRect.left = tl.x;
        g_targetRect.top = tl.y;
        g_targetRect.right = br.x;
        g_targetRect.bottom = br.y;
        g_hasTargetRect = ((g_targetRect.right - g_targetRect.left) > 2 && (g_targetRect.bottom - g_targetRect.top) > 2);

        PostMessageW(g_mainWnd, WM_APP_RECT_SELECTED, 0, 0);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
    {
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH back = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, back);
        DeleteObject(back);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT textRc = rc;
        textRc.left += 16;
        textRc.top += 12;
        DrawTextW(hdc, L"Drag to select the Paint area. ESC cancels.", -1, &textRc, DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (state && state->dragging)
        {
            RECT sel = NormalizeRect(state->start, state->current);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            std::wstringstream ss;
            ss << (sel.right - sel.left) << L" x " << (sel.bottom - sel.top);
            std::wstring sizeText = ss.str();
            RECT sizeRc{ sel.left + 6, sel.top + 6, sel.left + 180, sel.top + 30 };
            DrawTextW(hdc, sizeText.c_str(), -1, &sizeRc, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
    {
        auto* s = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        delete s;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        g_overlayWnd = nullptr;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateWindowW(L"BUTTON", L"Reload image.png", WS_CHILD | WS_VISIBLE,
                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_RELOAD, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Select area", WS_CHILD | WS_VISIBLE,
                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_SELECT, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Start drawing", WS_CHILD | WS_VISIBLE,
                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_START, g_hInst, nullptr);

        g_chkInvert = CreateWindowW(L"BUTTON", L"Invert", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_CHK_INVERT, g_hInst, nullptr);

        g_statusWnd = CreateWindowW(
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0,
            hwnd,
            (HMENU)(INT_PTR)ID_STATUS,
            g_hInst,
            nullptr
        );

        g_previewWnd = CreateWindowW(
            PREVIEW_CLASS,
            L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0,
            hwnd,
            nullptr,
            g_hInst,
            nullptr
        );

        LayoutMainWindow(hwnd);

        std::wstring error;
        if (!LoadImageFromDisk(error))
            SetStatus(error);

        return 0;
    }
    case WM_SIZE:
    {
        LayoutMainWindow(hwnd);
        return 0;
    }
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case ID_BTN_RELOAD:
        {
            if (g_isDrawing)
                return 0;
            std::wstring error;
            if (!LoadImageFromDisk(error))
                MessageBoxW(hwnd, error.c_str(), L"Error", MB_ICONERROR);
            return 0;
        }
        case ID_BTN_SELECT:
        {
            if (g_isDrawing)
                return 0;
            StartOverlaySelection(hwnd);
            return 0;
        }
        case ID_BTN_START:
        {
            if (g_isDrawing)
                return 0;
            if (!g_bitmap)
            {
                MessageBoxW(hwnd, L"image.png is not loaded.", L"Error", MB_ICONERROR);
                return 0;
            }
            if (!g_hasTargetRect)
            {
                MessageBoxW(hwnd, L"Select the Paint drawing area first.", L"Error", MB_ICONERROR);
                return 0;
            }

            MessageBoxW(
                hwnd,
                L"Before starting:\n\n- Open Paint\n- Select Pencil\n- Select black color\n- After you press OK, switch to Paint\n- Drawing starts after 2.5 seconds\n- ESC stops it",
                L"Info",
                MB_ICONINFORMATION
            );

            g_isDrawing = true;
            SetStatus(L"Drawing... ESC stops it.");
            ShowWindow(hwnd, SW_MINIMIZE);
            HANDLE hThread = CreateThread(nullptr, 0, DrawThreadProc, nullptr, 0, nullptr);
            if (hThread)
                CloseHandle(hThread);
            return 0;
        }
        case ID_CHK_INVERT:
        {
            g_invert = (SendMessageW(g_chkInvert, BM_GETCHECK, 0, 0) == BST_CHECKED);
            UpdatePreview();
            SetStatus(g_invert ? L"Invert ON | Mode: dither" : L"Invert OFF | Mode: dither");
            return 0;
        }
        }
        return 0;
    }
    case WM_APP_RECT_SELECTED:
    {
        if (!g_hasTargetRect)
        {
            SetStatus(L"Area was not selected.");
            return 0;
        }

        UpdatePreview();

        std::wstringstream ss;
        ss << L"Area selected: "
           << (g_targetRect.right - g_targetRect.left) << L"x"
           << (g_targetRect.bottom - g_targetRect.top)
           << L" | Dither preview updated";
        SetStatus(ss.str());
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_APP_DRAW_FINISHED:
    {
        g_isDrawing = false;
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);

        if (wParam == 1)
            SetStatus(L"Done.");
        else if (wParam == 2)
            SetStatus(L"Stopped with ESC.");
        else
            SetStatus(L"Could not start drawing.");
        return 0;
    }
    case WM_DESTROY:
    {
        DeleteLoadedBitmap();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    SetProcessDPIAware();
    g_hInst = hInstance;
    g_imagePath = GetExeDirImagePath();

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Ok)
    {
        MessageBoxW(nullptr, L"Could not start GDI+", L"Error", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.hInstance = hInstance;
    wc.lpszClassName = MAIN_CLASS;
    wc.lpfnWndProc = MainWndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);

    WNDCLASSW pwc{};
    pwc.hInstance = hInstance;
    pwc.lpszClassName = PREVIEW_CLASS;
    pwc.lpfnWndProc = PreviewWndProc;
    pwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pwc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&pwc);

    WNDCLASSW owc{};
    owc.hInstance = hInstance;
    owc.lpszClassName = OVERLAY_CLASS;
    owc.lpfnWndProc = OverlayWndProc;
    owc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    owc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&owc);

    g_mainWnd = CreateWindowW(
        MAIN_CLASS,
        L"Paint Drawer - Dither",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        860, 500,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_mainWnd)
    {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
