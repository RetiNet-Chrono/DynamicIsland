#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dwmapi.h>
#include <math.h>
#pragma comment(lib, "dwmapi.lib")

// ==================== 数学工具 ====================
inline float LerpF(float start, float end, float t) {
    return start + (end - start) * t;
}
inline int Lerp(int start, int end, float t) {
    return start + (int)((end - start) * t);
}
inline float EaseOutQuad(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t);
}
inline COLORREF ColorLerp(COLORREF c1, COLORREF c2, float t) {
    int r = (int)((1 - t) * GetRValue(c1) + t * GetRValue(c2));
    int g = (int)((1 - t) * GetGValue(c1) + t * GetGValue(c2));
    int b = (int)((1 - t) * GetBValue(c1) + t * GetBValue(c2));
    return RGB(r, g, b);
}

// ==================== 颜色主题 ====================
typedef struct {
    COLORREF bgIdle;
    COLORREF bgHover;
    COLORREF bgActive;
    COLORREF bgToolHover;
    COLORREF textNormal;
    COLORREF textActive;
} ColorTheme;

// 直接定义（不用extern），避免链接麻烦
static const ColorTheme g_themeLight = {
    RGB(255, 255, 255),     // bgIdle
    RGB(242, 242, 242),     // bgHover
    RGB(0, 120, 212),       // bgActive
    RGB(240, 240, 240),     // bgToolHover
    RGB(30, 30, 30),        // textNormal
    RGB(255, 255, 255)      // textActive
};

static const ColorTheme g_themeDark = {
    RGB(43, 43, 43),        // bgIdle
    RGB(55, 55, 55),        // bgHover
    RGB(0, 120, 212),       // bgActive
    RGB(60, 60, 60),        // bgToolHover
    RGB(255, 255, 255),     // textNormal
    RGB(255, 255, 255)      // textActive
};

// ==================== 按钮系统 ====================
#define MAX_BTN_TEXT 64
typedef void (*ButtonCallback)(int state);

typedef struct {
    RECT rect;
    WCHAR text[MAX_BTN_TEXT];
    BOOL isToggle;
    BOOL isActive;
    BOOL isHover;
    BOOL isPressed;
    float animHover;
    float animPress;
    ButtonCallback onClick;
} Button;

inline void BtnInit(Button* btn, int x, int y, int w, int h,
    const WCHAR* text, BOOL isToggle, ButtonCallback callback) {
    SetRect(&btn->rect, x, y, x + w, y + h);
    wcsncpy_s(btn->text, MAX_BTN_TEXT, text, _TRUNCATE);
    btn->isToggle = isToggle;
    btn->isActive = FALSE;
    btn->isHover = FALSE;
    btn->isPressed = FALSE;
    btn->animHover = 0.0f;
    btn->animPress = 0.0f;
    btn->onClick = callback;
}

inline void BtnUpdate(Button* btn) {
    float targetHover = btn->isHover ? 1.0f : 0.0f;
    if (btn->isToggle) {
        btn->animHover += (targetHover - btn->animHover) * 0.2f;
        float targetPress = btn->isPressed ? 1.0f : 0.0f;
        btn->animPress += (targetPress - btn->animPress) * 0.3f;
        if (fabs(btn->animHover) < 0.001f) btn->animHover = 0;
        if (fabs(btn->animHover - 1.0f) < 0.001f) btn->animHover = 1;
    }
    else {
        btn->animHover = targetHover;
        btn->animPress = 0.0f;
    }
}

inline void FillRoundRect(HDC hdc, const RECT* rc, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HRGN rgn = CreateRoundRectRgn(rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    FillRgn(hdc, rgn, brush);
    DeleteObject(rgn);
    DeleteObject(brush);
}

inline void BtnDraw(Button* btn, HDC hdc, const ColorTheme* theme) {
    COLORREF bgColor;
    if (btn->isToggle) {
        if (btn->isActive) {
            bgColor = theme->bgActive;
            if (btn->animHover > 0) bgColor = ColorLerp(bgColor, RGB(40, 140, 232), btn->animHover * 0.3f);
        }
        else {
            bgColor = ColorLerp(theme->bgIdle, theme->bgHover, btn->animHover);
            if (btn->animPress > 0) bgColor = ColorLerp(bgColor, theme->bgActive, btn->animPress * 0.5f);
        }
    }
    else {
        bgColor = (btn->animHover > 0.5f) ? theme->bgToolHover : theme->bgIdle;
    }
    FillRoundRect(hdc, &btn->rect, bgColor, 6);

    HFONT font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Emoji");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    COLORREF textColor = (btn->isToggle && btn->isActive) ? theme->textActive : theme->textNormal;
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRc = btn->rect;
    DrawTextW(hdc, btn->text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

inline BOOL BtnHitTest(Button* btn, int x, int y) {
    return x >= btn->rect.left && x <= btn->rect.right &&
        y >= btn->rect.top && y <= btn->rect.bottom;
}

inline BOOL BtnHandleMouse(Button* btn, UINT msg, int x, int y) {
    BOOL hit = BtnHitTest(btn, x, y);
    BOOL consumed = FALSE;
    switch (msg) {
    case WM_MOUSEMOVE: btn->isHover = hit; break;
    case WM_LBUTTONDOWN:
        if (hit) {
            consumed = TRUE;
            if (btn->isToggle) {
                btn->isPressed = TRUE;
                btn->isActive = !btn->isActive;
                if (btn->onClick) btn->onClick(btn->isActive ? 1 : 0);
            }
            else {
                if (btn->onClick) btn->onClick(0);
            }
        }
        break;
    case WM_LBUTTONUP:
        if (btn->isToggle) btn->isPressed = FALSE;
        break;
    }
    return consumed;
}

// ==================== 滑块系统（新增） ====================
#define MAX_SLIDER_EMOJI 8
typedef void (*SliderCallback)(int value);

typedef struct {
    RECT rect;
    WCHAR emoji[MAX_SLIDER_EMOJI];
    int value;
    int minVal, maxVal;
    BOOL isDragging;
    BOOL isHover;
    float animHover;
    SliderCallback onChange;
} Slider;

inline void SldInit(Slider* sld, int x, int y, int w, int h,
    const WCHAR* emoji, int initialVal, SliderCallback callback) {
    SetRect(&sld->rect, x, y, x + w, y + h);
    wcsncpy_s(sld->emoji, MAX_SLIDER_EMOJI, emoji, _TRUNCATE);
    sld->minVal = 0; sld->maxVal = 100;
    sld->value = initialVal;
    sld->isDragging = FALSE;
    sld->isHover = FALSE;
    sld->animHover = 0.0f;
    sld->onChange = callback;
}

inline void SldUpdate(Slider* sld) {
    float targetHover = sld->isHover ? 1.0f : 0.0f;
    sld->animHover += (targetHover - sld->animHover) * 0.15f;
}

inline void SldDraw(Slider* sld, HDC hdc, const ColorTheme* theme) {
    int x = sld->rect.left, y = sld->rect.top;
    int w = sld->rect.right - sld->rect.left, h = sld->rect.bottom - sld->rect.top;

    // Emoji左侧
    if (sld->emoji[0] != L'\0') {
        HFONT font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Emoji");
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, theme->textNormal);
        SetBkMode(hdc, TRANSPARENT);
        RECT emojiRc = { x + 8, y, x + 32, y + h };
        DrawTextW(hdc, sld->emoji, -1, &emojiRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(font);
    }

    // 轨道
    int trackX = x + 40, trackW = w - 48, trackH = 8;
    int trackY = y + (h - trackH) / 2;
    RECT trackRc = { trackX, trackY, trackX + trackW, trackY + trackH };
    HBRUSH trackBrush = CreateSolidBrush(theme->bgToolHover);
    FillRoundRect(hdc, &trackRc, theme->bgToolHover, 4);
    DeleteObject(trackBrush);

    // 进度条
    int progressW = (trackW * sld->value) / (sld->maxVal - sld->minVal);
    if (progressW > 0) {
        RECT progressRc = { trackX, trackY, trackX + progressW, trackY + trackH };
        COLORREF progressColor = sld->animHover > 0.5f ?
            ColorLerp(theme->bgActive, RGB(60, 160, 255), 0.3f) : theme->bgActive;
        HBRUSH progressBrush = CreateSolidBrush(progressColor);
        FillRoundRect(hdc, &progressRc, progressColor, 4);
        DeleteObject(progressBrush);
    }

    // 滑块头
    int thumbX = trackX + progressW, thumbY = y + h / 2;
    int thumbSize = 6 + (int)(sld->animHover * 2.0f);
    HBRUSH thumbBrush = CreateSolidBrush(RGB(255, 255, 255));
    HPEN thumbPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, thumbBrush);
    HPEN oldPen = (HPEN)SelectObject(hdc, thumbPen);
    Ellipse(hdc, thumbX - thumbSize, thumbY - thumbSize, thumbX + thumbSize, thumbY + thumbSize);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(thumbBrush);
    DeleteObject(thumbPen);
}

inline BOOL SldHitTest(Slider* sld, int x, int y) {
    return x >= sld->rect.left && x <= sld->rect.right &&
        y >= sld->rect.top && y <= sld->rect.bottom;
}

inline int SldXToValue(Slider* sld, int x) {
    int trackX = sld->rect.left + 40;
    int trackW = (sld->rect.right - sld->rect.left) - 48;
    int relX = x - trackX;
    if (relX < 0) return sld->minVal;
    if (relX > trackW) return sld->maxVal;
    return sld->minVal + (relX * (sld->maxVal - sld->minVal)) / trackW;
}

inline BOOL SldHandleMouse(Slider* sld, UINT msg, int x, int y) {
    switch (msg) {
    case WM_MOUSEMOVE:
        sld->isHover = SldHitTest(sld, x, y);
        if (sld->isDragging) {
            int newVal = SldXToValue(sld, x);
            if (newVal != sld->value) {
                sld->value = newVal;
                if (sld->onChange) sld->onChange(sld->value);
            }
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (SldHitTest(sld, x, y)) {
            sld->isDragging = TRUE;
            int newVal = SldXToValue(sld, x);
            if (newVal != sld->value) {
                sld->value = newVal;
                if (sld->onChange) sld->onChange(sld->value);
            }
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        if (sld->isDragging) { sld->isDragging = FALSE; return TRUE; }
        break;
    }
    return FALSE;
}

// ==================== 窗口动画工具 ====================
typedef struct {
    BOOL isActive;
    DWORD startTime;
    int startW, startH;
    int targetW, targetH;
    BOOL isExpanding;
} WindowAnimation;

inline void WinAnimStart(WindowAnimation* anim, HWND hWnd, BOOL expanding,
    int targetExpandedW, int targetExpandedH,
    int targetCollapsedW, int targetCollapsedH) {
    if (anim->isActive && anim->isExpanding == expanding) return;
    RECT rc;
    GetWindowRect(hWnd, &rc);
    anim->isActive = TRUE;
    anim->startTime = GetTickCount();
    anim->isExpanding = expanding;
    anim->startW = rc.right - rc.left;
    anim->startH = rc.bottom - rc.top;
    if (expanding) {
        anim->targetW = targetExpandedW; anim->targetH = targetExpandedH;
    }
    else {
        anim->targetW = targetCollapsedW; anim->targetH = targetCollapsedH;
    }
}

inline BOOL WinAnimUpdate(WindowAnimation* anim, HWND hWnd, int screenW, int marginTop,
    float durationMs) {
    if (!anim->isActive) return FALSE;
    DWORD now = GetTickCount();
    float elapsed = (float)(now - anim->startTime);
    float t = elapsed / durationMs;
    if (t >= 1.0f) { t = 1.0f; anim->isActive = FALSE; }
    float easedT = EaseOutQuad(t);
    int currentW = Lerp(anim->startW, anim->targetW, easedT);
    int currentH = Lerp(anim->startH, anim->targetH, easedT);
    int x = (screenW - currentW) / 2;
    SetWindowPos(hWnd, HWND_TOPMOST, x, marginTop, currentW, currentH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return anim->isActive;
}