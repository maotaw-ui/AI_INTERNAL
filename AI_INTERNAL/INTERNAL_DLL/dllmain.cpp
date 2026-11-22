// AI INTERNAL USE ONLY - compact Direct2D config editor
//
// GPU-composited ESP overlay: D3D11 + DXGI composition swap chain + DirectComposition.
// Direct2D draws directly into the swap-chain surface; no full-screen DIB or UpdateLayeredWindow transfer.
// The compact menu is cached in a small off-screen Direct2D bitmap.
//
// Build (x64 Developer Command Prompt):
//   cl /nologo /LD /EHsc /std:c++17 /O2 overlay_config_editor.cpp d3d11.lib dxgi.lib dcomp.lib d2d1.lib dwrite.lib user32.lib gdi32.lib ole32.lib /link /OUT:overlay.dll
//
// Changes applied:
//  - Boxes wider (boxHeight * 0.58f)
//  - Menu HWND stays topmost; passive mode is non-activating and click-through
//  - Interactive menu mode clears passive flags and explicitly takes input focus
//  - Entity pawn address cache rebuilt only every 5 seconds

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <cstdio>
#include <windowsx.h>
#include <d2d1_1.h>
#include <dxgiformat.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <array>
#include <cstddef>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <string>
#include <type_traits>
#include <cstring>
#include <cctype>
#include <vector>
#include <iterator>
#include <new>
#include <random>
#include <chrono>
#include "weapon_icons.hpp"
#include "hitmarker_sound_data.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

namespace Offsets
{
    // Build-specific. Update when the game patches.
    constexpr uintptr_t dwViewMatrix = 0x255D3D0;
    constexpr uintptr_t dwEntityList = 0x270D5B0;
    constexpr uintptr_t dwLocalPlayerCtrl = 0x252F300;
    constexpr uintptr_t dwLocalPlayerPawn = 0x2557AB8;
    // Controller / weapon schema offsets for the supplied CS2 build.
    constexpr uintptr_t m_hPawn = 0x92C; // CCSPlayerController::m_hPlayerPawn
    constexpr uintptr_t m_iszPlayerName = 0x6FC; // CBasePlayerController::m_iszPlayerName
    constexpr uintptr_t m_iHealth = 0x34C;
    constexpr std::ptrdiff_t m_ArmorValue = 0x1F00; // int32
    constexpr uintptr_t m_vOldOrigin = 0x14A4;
    constexpr uintptr_t m_pWeaponServices = 0x12F0; // C_BasePlayerPawn::m_pWeaponServices
    constexpr uintptr_t m_hActiveWeapon = 0x60; // CPlayer_WeaponServices::m_hActiveWeapon
    constexpr uintptr_t m_AttributeManager = 0x1290; // C_EconEntity::m_AttributeManager
    constexpr uintptr_t m_Item = 0x50; // C_AttributeContainer::m_Item
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA; // C_EconItemView::m_iItemDefinitionIndex
    // Scene node / skeleton offsets. Bone array pointer is modelState + 0x80.
    constexpr uintptr_t m_pGameSceneNode = 0x330;
    constexpr uintptr_t m_vecAbsOrigin = 0xC8; // CGameSceneNode::m_vecAbsOrigin
    constexpr uintptr_t m_modelState = 0x140;
    constexpr uintptr_t m_iTeamNum = 0x3E7;
    constexpr uintptr_t m_pActionTrackingServices = 0x828; // CCSPlayerController::m_pActionTrackingServices (current client schema fallback)
    constexpr uintptr_t m_flTotalRoundDamageDealt = 0x130; // CCSPlayerController_ActionTrackingServices::m_flTotalRoundDamageDealt

    // Aim / recoil-control offsets from the supplied runtime snippet.
    constexpr uintptr_t dwViewAngles = 0x256DBE8;
    constexpr uintptr_t m_vecViewOffset = 0xF60;
    constexpr uintptr_t m_pAimPunchServices = 0x1598; // schema-confirmed pointer; RCS cache is not present in supplied schema
    constexpr uintptr_t m_iShotsFired = 0x1EE8; // schema-confirmed
}

// Aimbot values supplied for the target build. The aimbot reuses the ESP's
// resolved pawn cache and inline view matrix instead of maintaining a second,
// conflicting entity-list / matrix-pointer path.
namespace AimbotOffsets
{
    constexpr uintptr_t dwEntityList = 0x270D5B0;       // same entity list used by ESP
    constexpr uintptr_t dwLocalPlayerPawn = 0x2557AB8;  // retained for reference; resolved pawn cache is preferred
    constexpr uintptr_t dwViewAngles = 0x256DBE8;
    constexpr uintptr_t dwViewMatrix = 0x255D3D0;       // inline Mat4, matching the ESP reader

    constexpr uintptr_t m_iHealth = 0x34C;
    constexpr uintptr_t m_vecOrigin = 0x14A4;
    constexpr uintptr_t m_iTeamNum = 0x3E7;            // byte-sized team field
    constexpr uintptr_t m_iShotsFired = 0x1EE8;
    constexpr uintptr_t m_pAimPunchServices = 0x1598;
    constexpr uintptr_t m_aimPunchCache = 0x88;
    constexpr uintptr_t m_bDormant = 0x103; // CGameSceneNode dormant flag
}

namespace Menu
{
    inline bool menuOpen = false;
    inline bool box = true;
    inline bool filledBox = false;
    inline bool health = true;
    inline bool armor = false;
    enum class BarPosition { Left, Right, Top, Bottom };
    inline BarPosition healthPosition = BarPosition::Left;
    inline BarPosition armorPosition = BarPosition::Right;
    inline float boxCornerLength = 42.0f;
    inline float healthBarWidth = 1.0f;
    inline float armorBarWidth = 1.0f;
    inline bool snapline = false;
    enum class SnaplineAnchor { Top, Center, Bottom, Left, Right };
    inline SnaplineAnchor snaplineAnchor = SnaplineAnchor::Bottom;
    inline float snaplineColorR = 0.25f;
    inline float snaplineColorG = 0.78f;
    inline float snaplineColorB = 1.00f;
    inline float filledOpacity = 0.60f;
    enum class FilledBoxStyle { Solid, CenterFade, TopToBottom, BottomToTop };
    inline FilledBoxStyle filledBoxStyle = FilledBoxStyle::Solid;
    inline float filledGradientColorR = 0.20f;
    inline float filledGradientColorG = 0.55f;
    inline float filledGradientColorB = 1.00f;
    inline bool healthGradient = false;
    inline float healthGradientColorR = 0.15f;
    inline float healthGradientColorG = 0.75f;
    inline float healthGradientColorB = 0.95f;
    inline float boxColorR = 0.96f;
    inline float boxColorG = 0.12f;
    inline float boxColorB = 0.18f;
    inline float filledColorR = 0.96f;
    inline float filledColorG = 0.12f;
    inline float filledColorB = 0.18f;
    inline float healthColorR = 0.20f;
    inline float healthColorG = 0.88f;
    inline float healthColorB = 0.48f;
    inline float healthBackgroundColorR = 0.0f;
    inline float healthBackgroundColorG = 0.0f;
    inline float healthBackgroundColorB = 0.0f;
    inline float armorColorR = 0.22f;
    inline float armorColorG = 0.62f;
    inline float armorColorB = 1.00f;
    inline bool skeleton = false;
    inline float skeletonColorR = 0.20f;
    inline float skeletonColorG = 0.88f;
    inline float skeletonColorB = 0.98f;
    inline float skeletonOpacity = 1.0f;
    inline bool playerName = true;
    inline bool weaponIcon = true;
    inline bool distance = true;
    inline float playerNameColorR = 0.96f;
    inline float playerNameColorG = 0.96f;
    inline float playerNameColorB = 1.00f;
    inline float weaponIconColorR = 0.20f;
    inline float weaponIconColorG = 0.88f;
    inline float weaponIconColorB = 0.98f;
    inline float distanceColorR = 0.96f;
    inline float distanceColorG = 0.96f;
    inline float distanceColorB = 1.00f;
    inline bool hitMarker = true;
    inline bool hitMarkerSound = true; // enabled by default
    inline float hitMarkerScale = 1.0f; // 0.35..1.00
    inline float hitMarkerVolume = 0.85f; // 0..2, per-sample gain (up to 200%)
    inline float hitMarkerColorR = 1.0f;
    inline float hitMarkerColorG = 1.0f;
    inline float hitMarkerColorB = 1.0f;
    inline bool damagePopup = true;
    inline float damagePopupColorR = 1.0f;
    inline float damagePopupColorG = 0.19f;
    inline float damagePopupColorB = 0.25f;
    inline float damagePopupScale = 1.0f; // 0.65..3.0 base text size
    inline int damagePopupStyle = 0; // legacy config field; no longer used by the popup
    // Legacy config fields retained only to preserve visuals.cfg binary layout.
    // The popup now uses a fixed lifetime and has no duration-growth control.
    inline float damagePopupDurationMs = 750.0f;
    inline float damagePopupGrowth = 0.0f;
    inline float damagePopupBoldness = 0.80f; // 0..1 maps to DirectWrite weight 400..900
    inline bool damagePopupOutline = true;
    inline float damagePopupOutlineColorR = 0.045f;
    inline float damagePopupOutlineColorG = 0.055f;
    inline float damagePopupOutlineColorB = 0.075f;
}

namespace AimConfig
{
    inline bool enabled = false;
    inline bool deathmatchMode = false; // false = enemies only; true = both teams
    // The aim loop is input-gated. Default: hold Left Mouse Button (MOUSE1).
    inline UINT activationKey = VK_LBUTTON;
    inline bool rcsEnabled = true; // supplied standalone recoil loop toggle.
    inline float fovRadius = 55.0f;
    // Aim interpolation rate (per second); the old config slot is reused for compatibility.
    inline float lerpSpeed = 24.0f;
    inline float rcsLerpSpeed = 12.0f; // ~5.0f (very soft/slow) to ~25.0f (snappy)
    inline float rcsDownBias = 0.0f; // legacy config field; no longer exposed or applied
    inline float rcsSmoothing = 3.5f; // fixed internal smoothing
    inline float rcsJitter = 0.0f; // legacy config field; no longer exposed or applied
    inline float rcsStrength = 1.0f;

    inline float sensitivity = 2.11f; // menu-controlled; never read from a game offset
    inline int rcsBullet = 3; // retained for old config compatibility
    inline float rcsScale = 1.0f; // retained for old config compatibility
}

struct Vec3 { float x, y, z; };
struct Mat4 { float m[4][4]; };
struct Viewport { float x, y, w, h; };
struct AimVec2 { float x, y; };
struct UtlVecRaw { uintptr_t count; uintptr_t data; };

namespace
{
    constexpr wchar_t kWindowClass[] = L"Direct2DLayeredOverlayWindow";

    HMODULE g_module = nullptr;
    HWND g_hwnd = nullptr;
    HANDLE g_thread = nullptr;
    std::atomic<bool> g_running{ false };
    std::atomic<uintptr_t> g_clientBase{ 0 };
    DWORD g_hostProcessId = 0;
    bool g_hostProcessValidated = false;
    bool g_targetWindowActive = false;
    bool g_overlayWindowVisible = false;
    bool g_initialTargetStateChecked = false;

    int g_width = 0;
    int g_height = 0;

    ID3D11Device* g_d3dDevice = nullptr;
    ID3D11DeviceContext* g_d3dContext = nullptr;
    IDXGIDevice* g_dxgiDevice = nullptr;
    IDXGIFactory2* g_dxgiFactory = nullptr;
    IDXGISwapChain1* g_swapChain = nullptr;
    IDCompositionDevice* g_dcompDevice = nullptr;
    IDCompositionTarget* g_dcompTarget = nullptr;
    IDCompositionVisual* g_dcompVisual = nullptr;

    ID2D1Bitmap1* g_swapChainBitmap = nullptr;
    constexpr int kMenuSurfaceWidth = 560;
    constexpr int kMenuSurfaceHeight = 440;
    ID2D1Bitmap1* g_menuBitmap = nullptr;
    bool g_menuCacheDirty = true;
    bool g_menuPresentationPending = false;

    bool g_frameDirty = true;
    bool g_lastMenuVisible = false;
    constexpr DWORD kFrameWaitMs = 4;

    struct ScreenBone
    {
        float x = 0.0f;
        float y = 0.0f;
        bool visible = false;
    };

    struct EntityDrawData
    {
        bool valid = false;
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        int health = 0;
        float displayHealth = 100.0f; // smoothed
        int armor = 0;
        float displayArmor = 0.0f;
        uint8_t team = 0;
        char playerName[32]{};
        uint16_t weaponId = 0;
        int distanceMeters = 0;
        bool distanceValid = false;
        Vec3 worldOrigin{}; // captured world position for world-anchored hit feedback
        uintptr_t pawnKey = 0;
        bool skeletonValid = false;
        bool headBoneValid = false; // world-space bone 6 is the configured head target for this rig
        std::array<ScreenBone, 74> skeletonScreen{};
        std::array<Vec3, 74> skeletonWorld{};
    };
    using EntityFrame = std::array<EntityDrawData, 64>;
    EntityFrame g_lastEntityFrame{};
    bool g_hasLastEntityFrame = false;

    // Hitmarker state uses the controller's round-damage accumulator. A short
    // time window is used instead of a render-frame counter so visibility is
    // consistent across different overlay frame rates.
    float g_lastRoundDamage = 0.0f;
    bool g_roundDamageInitialized = false;
    uintptr_t g_hitMarkerController = 0;
    ULONGLONG g_hitMarkerVisibleUntil = 0;
    ULONGLONG g_hitMarkerCaptureUntil = 0;
    Vec3 g_hitMarkerWorldPosition{};
    bool g_hitMarkerWorldPositionValid = false;
    bool g_lastHitMarkerVisible = false;
    Vec3 g_recentDamagedWorldPosition{};
    ULONGLONG g_recentDamagedTime = 0;
    bool g_recentDamagedPositionValid = false;
    constexpr ULONGLONG kRecentDamageCorrelationMs = 350;
    constexpr ULONGLONG kHitMarkerDurationMs = 360;
    constexpr ULONGLONG kHitMarkerCaptureWindowMs = 140;
    // Each hit owns its own short-lived floating number. Keeping previous hits
    // alive avoids the old same-target replacement flicker during rapid fire.
    struct DamagePopup
    {
        Vec3 world{};
        uintptr_t pawnKey = 0;
        int damage = 0;
        ULONGLONG born = 0;
        float horizontalOffset = 0.0f;
        bool active = false;
    };
    std::array<DamagePopup, 12> g_damagePopups{};
    constexpr ULONGLONG kDamagePopupLifetimeMs = 1050;

    // These member offsets are loaded from the same generated schema header
    // consumed by Runtime(6).hpp. A missing schema file simply disables the
    // corresponding labels instead of relying on stale hard-coded offsets.
    struct PlayerSchemaOffsets
    {
        uintptr_t playerName = 0;
        uintptr_t playerPawnHandle = 0;
        uintptr_t weaponServices = 0;
        uintptr_t activeWeapon = 0;
        uintptr_t attributeManager = 0;
        uintptr_t item = 0;
        uintptr_t itemDefinitionIndex = 0;
        uintptr_t actionTrackingServices = 0;
    };
    PlayerSchemaOffsets g_playerSchema{};
    bool g_playerSchemaLoaded = false;
    uintptr_t g_dwLocalPlayerControllerOffset = Offsets::dwLocalPlayerCtrl;

    // Entity address cache – rebuilt every 5 seconds. Skeleton samples are
    // refreshed independently so drawing can reuse the latest valid pose.
    constexpr size_t kSkeletonBoneCount = 74;
    constexpr size_t kSkeletonBoneStride = 0x20;
    constexpr size_t kSkeletonBoneBytes = (kSkeletonBoneCount - 1) * kSkeletonBoneStride + sizeof(Vec3);
    constexpr std::array<size_t, 23> kSkeletonUsedBones{ {
        0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12,
        13, 14, 17, 18, 19, 20, 21, 22, 50, 71, 73
    } };
    constexpr ULONGLONG kSkeletonBoneRefreshMs = 8;
    constexpr ULONGLONG kSkeletonPointerRefreshMs = 5000;

    struct CachedPawn
    {
        uintptr_t pawn = 0;
        uintptr_t controller = 0;
        std::array<char, 32> playerName{};
        ULONGLONG lastNameRefresh = 0;
        uint16_t weaponId = 0;
        ULONGLONG lastWeaponRefresh = 0;
        uintptr_t boneArray = 0;
        ULONGLONG boneArrayLastResolve = 0;
        ULONGLONG boneSampleTime = 0;
        bool bonesValid = false;
        std::array<Vec3, kSkeletonBoneCount> bones{};
    };
    std::array<CachedPawn, 64> g_pawnCache{};
    size_t g_pawnCacheCount = 0;
    uintptr_t g_entityList = 0;
    uintptr_t g_localPlayerPawn = 0;
    AimVec2 g_oldPunch{};
    bool g_oldPunchValid = false;
    uintptr_t g_oldPunchPawn = 0;
    uintptr_t g_oldPunchServices = 0;
    int g_lastRcsShotsFired = 0;
    uintptr_t g_lastAimTargetPawn = 0;
    ULONGLONG g_lastPawnCacheUpdate = 0;
    constexpr ULONGLONG kPawnCacheIntervalMs = 5000;

    void ResetRcsSampleState() noexcept
    {
        g_oldPunch = {};
        g_oldPunchValid = false;
        g_oldPunchPawn = 0;
        g_oldPunchServices = 0;
        g_lastRcsShotsFired = 0;
    }

    ID2D1Factory1* g_d2dFactory = nullptr;
    ID2D1Device* g_d2dDevice = nullptr;
    ID2D1DeviceContext* g_d2dTarget = nullptr;
    IDWriteFactory* g_writeFactory = nullptr;
    IDWriteTextFormat* g_textFormat = nullptr;
    IDWriteTextFormat* g_damageTextFormat = nullptr; // one fixed, extra-bold damage-number face
    IDWriteTextFormat* g_smallTextFormat = nullptr;
    IDWriteTextFormat* g_menuTextFormat = nullptr;
    IDWriteTextFormat* g_menuTitleFormat = nullptr;
    IDWriteTextFormat* g_menuHeroFormat = nullptr;
    IDWriteTextFormat* g_codeTextFormat = nullptr;

    ID2D1SolidColorBrush* g_brushCodeKey = nullptr;
    ID2D1SolidColorBrush* g_brushCodeValue = nullptr;
    ID2D1SolidColorBrush* g_brushMenuBack = nullptr;
    ID2D1SolidColorBrush* g_brushMenuPanel = nullptr;
    ID2D1SolidColorBrush* g_brushMenuBorder = nullptr;
    ID2D1SolidColorBrush* g_brushMenuText = nullptr;
    ID2D1SolidColorBrush* g_brushMenuMuted = nullptr;
    ID2D1SolidColorBrush* g_brushSliderTrack = nullptr;
    ID2D1SolidColorBrush* g_brushSliderFill = nullptr;
    ID2D1SolidColorBrush* g_brushCheckboxBack = nullptr;
    ID2D1SolidColorBrush* g_brushBoxColor = nullptr;
    ID2D1SolidColorBrush* g_brushFilledColor = nullptr;
    ID2D1SolidColorBrush* g_brushHealthColor = nullptr;
    ID2D1SolidColorBrush* g_brushHealthBackgroundColor = nullptr;
    ID2D1SolidColorBrush* g_brushArmorColor = nullptr;
    ID2D1SolidColorBrush* g_brushArmorBack = nullptr;
    ID2D1SolidColorBrush* g_brushSkeleton = nullptr;
    ID2D1SolidColorBrush* g_brushSnaplineColor = nullptr;
    ID2D1SolidColorBrush* g_brushSnapline = nullptr;
    ID2D1SolidColorBrush* g_brushOutline = nullptr;
    ID2D1SolidColorBrush* g_brushAccent = nullptr;
    ID2D1SolidColorBrush* g_brushEntityFill = nullptr;
    ID2D1SolidColorBrush* g_brushText = nullptr;
    ID2D1SolidColorBrush* g_brushHealthBack = nullptr;
    ID2D1SolidColorBrush* g_brushHealthHigh = nullptr;
    ID2D1SolidColorBrush* g_brushHealthMed = nullptr;
    ID2D1SolidColorBrush* g_brushHealthLow = nullptr;
    ID2D1SolidColorBrush* g_brushPlayerName = nullptr;
    ID2D1SolidColorBrush* g_brushWeaponInfo = nullptr;
    ID2D1SolidColorBrush* g_brushDistance = nullptr;
    ID2D1SolidColorBrush* g_brushHitMarker = nullptr;
    ID2D1SolidColorBrush* g_brushDamagePopupOutline = nullptr;

    struct WeaponIconBitmapEntry
    {
        const uint8_t* alpha = nullptr;
        int width = 0;
        int height = 0;
        ID2D1Bitmap* foreground = nullptr;
        ID2D1Bitmap* shadow = nullptr;
        uint32_t tintKey = 0;
    };
    std::array<WeaponIconBitmapEntry, 128> g_weaponIconBitmaps{};
    size_t g_weaponIconBitmapCount = 0;

    ID2D1LinearGradientBrush* g_brushPickerSaturation = nullptr;
    ID2D1LinearGradientBrush* g_brushPickerValue = nullptr;
    ID2D1LinearGradientBrush* g_brushPickerHue = nullptr;
    float g_pickerGradientHue = -1.0f;
    bool g_pickerSaturationGradientDirty = true;

    enum class ActiveSlider { None, BoxCorners, FilledOpacity, HealthWidth, ArmorWidth, SkeletonOpacity, HitMarkerScale, HitMarkerVolume, DamagePopupScale, DamagePopupBoldness, AimFov, AimLerpSpeed, AimSensitivity, RcsStrength, ColorSV, ColorHue };
    enum class MenuTab { Aimbot, ESP, Settings };
    enum class EditorFeature { Box, HealthBar };
    enum class KeyCaptureTarget { None, MenuToggle, ExitOverlay, AimActivation };
    enum class ColorTarget { BoxOutline, FilledBox, HealthBar, HealthBackground, ArmorBar, Snapline };

    struct HotkeyBind { UINT key = 0; UINT modifiers = 0; };

    struct MenuLayout
    {
        D2D1_RECT_F window{};
        D2D1_RECT_F header{};
        D2D1_RECT_F headerDragHit{};
        D2D1_RECT_F tabAimbotHit{};
        D2D1_RECT_F tabEspHit{};
        D2D1_RECT_F aimEnabledHit{};
        D2D1_RECT_F aimFovHit{};
        D2D1_RECT_F aimLerpSpeedHit{};
        D2D1_RECT_F aimKeyHit{};
        D2D1_RECT_F aimSensitivityHit{};
        D2D1_RECT_F rcsEnabledHit{};
        D2D1_RECT_F rcsStrengthHit{};
        D2D1_RECT_F aimDeathmatchHit{};
        D2D1_RECT_F espDeathmatchHit{};
        D2D1_RECT_F boxToggleHit{};
        D2D1_RECT_F healthToggleHit{};
        D2D1_RECT_F armorToggleHit{};
        D2D1_RECT_F armorColorSwatchHit{};
        D2D1_RECT_F healthPositionHit{};
        D2D1_RECT_F armorPositionHit{};
        D2D1_RECT_F armorValueHit{};
        D2D1_RECT_F cornerSliderTrack{};
        D2D1_RECT_F cornerSliderHit{};
        D2D1_RECT_F cornerValueHit{};
        D2D1_RECT_F healthSliderTrack{};
        D2D1_RECT_F healthSliderHit{};
        D2D1_RECT_F healthValueHit{};
        D2D1_RECT_F snaplineToggleHit{};
        D2D1_RECT_F snaplineColorSwatchHit{};
        D2D1_RECT_F skeletonToggleHit{};
        D2D1_RECT_F skeletonColorSwatchHit{};
        D2D1_RECT_F skeletonOpacityValueHit{};
        D2D1_RECT_F playerNameToggleHit{};
        D2D1_RECT_F weaponIconToggleHit{};
        D2D1_RECT_F distanceToggleHit{};
        D2D1_RECT_F playerNameColorSwatchHit{};
        D2D1_RECT_F weaponIconColorSwatchHit{};
        D2D1_RECT_F distanceColorSwatchHit{};
        D2D1_RECT_F hitMarkerToggleHit{};
        D2D1_RECT_F hitMarkerSoundToggleHit{};
        D2D1_RECT_F hitMarkerScaleValueHit{};
        D2D1_RECT_F hitMarkerVolumeValueHit{};
        D2D1_RECT_F hitMarkerColorSwatchHit{};
        D2D1_RECT_F damagePopupToggleHit{};
        D2D1_RECT_F damagePopupColorSwatchHit{};
        D2D1_RECT_F damagePopupScaleValueHit{};
        D2D1_RECT_F damagePopupBoldnessValueHit{};
        D2D1_RECT_F damagePopupOutlineToggleHit{};
        D2D1_RECT_F damagePopupOutlineColorSwatchHit{};
        D2D1_RECT_F filledBoxStyleHit{};
        D2D1_RECT_F filledGradientColorSwatchHit{};
        D2D1_RECT_F healthGradientToggleHit{};
        D2D1_RECT_F healthGradientColorSwatchHit{};
        D2D1_RECT_F contentClip{};
        D2D1_RECT_F scrollTrack{};
        D2D1_RECT_F scrollThumb{};
        D2D1_RECT_F snaplineAnchorHit{};
        D2D1_RECT_F snaplineAnchorLeftHit{};
        D2D1_RECT_F snaplineAnchorRightHit{};
        D2D1_RECT_F menuBindButtonHit{};
        D2D1_RECT_F exitBindButtonHit{};
        D2D1_RECT_F exitButtonHit{};
        D2D1_RECT_F filledBoxToggleHit{};
        D2D1_RECT_F boxColorSwatchHit{};
        D2D1_RECT_F filledColorSwatchHit{};
        D2D1_RECT_F healthColorSwatchHit{};
        D2D1_RECT_F healthBackgroundColorSwatchHit{};
        D2D1_RECT_F colorPickerPanel{};
        D2D1_RECT_F colorSVHit{};
        D2D1_RECT_F colorHueHit{};
        D2D1_RECT_F filledOpacitySliderTrack{};
        D2D1_RECT_F filledOpacitySliderHit{};
        D2D1_RECT_F filledOpacityValueHit{};
    };

    ActiveSlider g_activeSlider = ActiveSlider::None;
    bool g_configDirtyDuringDrag = false;
    float g_sensitivityDragStartValue = 2.11f;
    float g_sensitivityDragStartX = 0.0f;
    MenuTab g_activeTab = MenuTab::ESP;
    bool g_colorPickerOpen = false;
    ColorTarget g_colorPickerTarget = ColorTarget::BoxOutline;
    EditorFeature g_selectedFeature = EditorFeature::Box;
    float g_pickerHue = 0.985f;
    float g_pickerSaturation = 0.88f;
    float g_pickerValue = 0.96f;
    KeyCaptureTarget g_keyCaptureTarget = KeyCaptureTarget::None;
    HotkeyBind g_menuToggleBind{ VK_F11, 0 };
    HotkeyBind g_exitBind{ VK_END, 0 };
    bool g_keyDown[256]{};
    bool g_keyPressed[256]{};
    bool g_aimCaptureWaitForRelease = false;
    bool g_aimActivationWasHeld = false;

    // Use a Windows hotkey registration for the menu toggle instead of
    // relying on sampled GetAsyncKeyState edges. This avoids missed presses
    // when the game/overlay message loop is busy or focus changes briefly.
    constexpr int kMenuHotkeyId = 0xB171;
    bool g_menuHotkeyRegistered = false;
    UINT g_registeredMenuKey = 0;
    UINT g_registeredMenuModifiers = 0;

    ULONGLONG g_lastMenuToggleTime = 0;
    // The overlay is non-activating. Brief DWM/game HWND transitions must not
    // immediately undo a menu toggle. The deadline is extended on every open.
    ULONGLONG g_lastTargetForegroundTime = 0;
    constexpr ULONGLONG kForegroundTransitionGraceMs = 1200;
    POINT g_menuMouse{ -1, -1 };
    bool g_menuDragging = false;
    float g_menuScrollOffset = 0.0f;
    constexpr float kMenuMaxScroll = 400.0f;
    constexpr float kMenuScrollStep = 24.0f;
    float g_dragOffsetX = 0.0f;
    float g_dragOffsetY = 0.0f;
    float g_menuX = 0.0f;
    float g_menuY = 0.0f;
    bool g_menuPositionInitialized = false;
    bool g_endPressArmed = false;
    ULONGLONG g_lastEndPressTime = 0;

    D2D1_COLOR_F Color(float r, float g, float b, float a = 1.0f) noexcept
    {
        return D2D1_COLOR_F{ r, g, b, a };
    }


    void HSVToRGB(float h, float s, float v, float& r, float& g, float& b) noexcept
    {
        h = h - floorf(h);
        s = std::clamp(s, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
        const float c = v * s;
        const float h6 = h * 6.0f;
        const float x = c * (1.0f - fabsf(fmodf(h6, 2.0f) - 1.0f));
        float rp = 0, gp = 0, bp = 0;
        if (h6 < 1) { rp = c; gp = x; }
        else if (h6 < 2) { rp = x; gp = c; }
        else if (h6 < 3) { gp = c; bp = x; }
        else if (h6 < 4) { gp = x; bp = c; }
        else if (h6 < 5) { rp = x; bp = c; }
        else { rp = c; bp = x; }
        const float m = v - c;
        r = rp + m; g = gp + m; b = bp + m;
    }

    void RGBToHSV(float r, float g, float b, float& h, float& s, float& v) noexcept
    {
        const float mx = std::max({ r, g, b });
        const float mn = std::min({ r, g, b });
        const float d = mx - mn;
        v = mx;
        s = mx <= 1e-4f ? 0.0f : d / mx;
        if (d <= 1e-4f) h = 0.0f;
        else if (mx == r) h = fmodf((g - b) / d, 6.0f) / 6.0f;
        else if (mx == g) h = ((b - r) / d + 2.0f) / 6.0f;
        else h = ((r - g) / d + 4.0f) / 6.0f;
        if (h < 0.0f) h += 1.0f;
    }

    // -------------------- AppData config auto-save / load --------------------
    std::wstring GetConfigPath() noexcept
    {
        wchar_t* appData = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        {
            path = appData;
            path += L"\\AIInternalOverlay";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\visuals.cfg";
            CoTaskMemFree(appData);
        }
        return path;
    }

    void SaveConfigToAppData() noexcept
    {
        const std::wstring path = GetConfigPath();
        if (path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
        // simple binary blob of current Menu values
        fwrite(&Menu::box, sizeof(Menu::box), 1, f);
        fwrite(&Menu::filledBox, sizeof(Menu::filledBox), 1, f);
        fwrite(&Menu::health, sizeof(Menu::health), 1, f);
        fwrite(&Menu::snapline, sizeof(Menu::snapline), 1, f);
        fwrite(&Menu::boxCornerLength, sizeof(Menu::boxCornerLength), 1, f);
        fwrite(&Menu::healthBarWidth, sizeof(Menu::healthBarWidth), 1, f);
        fwrite(&Menu::filledOpacity, sizeof(Menu::filledOpacity), 1, f);
        fwrite(&Menu::boxColorR, sizeof(float), 1, f);
        fwrite(&Menu::boxColorG, sizeof(float), 1, f);
        fwrite(&Menu::boxColorB, sizeof(float), 1, f);
        fwrite(&Menu::filledColorR, sizeof(float), 1, f);
        fwrite(&Menu::filledColorG, sizeof(float), 1, f);
        fwrite(&Menu::filledColorB, sizeof(float), 1, f);
        fwrite(&Menu::healthColorR, sizeof(float), 1, f);
        fwrite(&Menu::healthColorG, sizeof(float), 1, f);
        fwrite(&Menu::healthColorB, sizeof(float), 1, f);
        fwrite(&Menu::snaplineColorR, sizeof(float), 1, f);
        fwrite(&Menu::snaplineColorG, sizeof(float), 1, f);
        fwrite(&Menu::snaplineColorB, sizeof(float), 1, f);
        int anchor = static_cast<int>(Menu::snaplineAnchor);
        fwrite(&anchor, sizeof(anchor), 1, f);
        // Appended fields keep older config files readable.
        fwrite(&Menu::armor, sizeof(Menu::armor), 1, f);
        fwrite(&Menu::armorBarWidth, sizeof(Menu::armorBarWidth), 1, f);
        fwrite(&Menu::armorColorR, sizeof(float), 1, f);
        fwrite(&Menu::armorColorG, sizeof(float), 1, f);
        fwrite(&Menu::armorColorB, sizeof(float), 1, f);
        int healthPos = static_cast<int>(Menu::healthPosition);
        int armorPos = static_cast<int>(Menu::armorPosition);
        fwrite(&healthPos, sizeof(healthPos), 1, f);
        fwrite(&armorPos, sizeof(armorPos), 1, f);
        fwrite(&Menu::skeleton, sizeof(Menu::skeleton), 1, f);
        fwrite(&Menu::skeletonColorR, sizeof(float), 1, f);
        fwrite(&Menu::skeletonColorG, sizeof(float), 1, f);
        fwrite(&Menu::skeletonColorB, sizeof(float), 1, f);
        fwrite(&Menu::skeletonOpacity, sizeof(float), 1, f);
        // Appended independently controllable player-info options.
        fwrite(&Menu::playerName, sizeof(Menu::playerName), 1, f);
        fwrite(&Menu::weaponIcon, sizeof(Menu::weaponIcon), 1, f);
        fwrite(&Menu::distance, sizeof(Menu::distance), 1, f);
        // Appended player-info colors and hitmarker setting.
        fwrite(&Menu::playerNameColorR, sizeof(float), 1, f);
        fwrite(&Menu::playerNameColorG, sizeof(float), 1, f);
        fwrite(&Menu::playerNameColorB, sizeof(float), 1, f);
        fwrite(&Menu::weaponIconColorR, sizeof(float), 1, f);
        fwrite(&Menu::weaponIconColorG, sizeof(float), 1, f);
        fwrite(&Menu::weaponIconColorB, sizeof(float), 1, f);
        fwrite(&Menu::distanceColorR, sizeof(float), 1, f);
        fwrite(&Menu::distanceColorG, sizeof(float), 1, f);
        fwrite(&Menu::distanceColorB, sizeof(float), 1, f);
        fwrite(&Menu::hitMarker, sizeof(Menu::hitMarker), 1, f);
        fwrite(&Menu::hitMarkerSound, sizeof(Menu::hitMarkerSound), 1, f);
        fwrite(&Menu::hitMarkerScale, sizeof(Menu::hitMarkerScale), 1, f);
        fwrite(&Menu::hitMarkerVolume, sizeof(Menu::hitMarkerVolume), 1, f);
        fwrite(&Menu::hitMarkerColorR, sizeof(float), 1, f);
        fwrite(&Menu::hitMarkerColorG, sizeof(float), 1, f);
        fwrite(&Menu::hitMarkerColorB, sizeof(float), 1, f);
        fwrite(&Menu::damagePopup, sizeof(Menu::damagePopup), 1, f);
        fwrite(&Menu::damagePopupColorR, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupColorG, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupColorB, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupScale, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupStyle, sizeof(Menu::damagePopupStyle), 1, f); // legacy config compatibility
        fwrite(&Menu::damagePopupDurationMs, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupGrowth, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupBoldness, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupOutline, sizeof(Menu::damagePopupOutline), 1, f);
        // Gradient controls appended to preserve compatibility with older configs.
        const int filledStyle = static_cast<int>(Menu::filledBoxStyle);
        fwrite(&filledStyle, sizeof(filledStyle), 1, f);
        fwrite(&Menu::filledGradientColorR, sizeof(float), 1, f);
        fwrite(&Menu::filledGradientColorG, sizeof(float), 1, f);
        fwrite(&Menu::filledGradientColorB, sizeof(float), 1, f);
        fwrite(&Menu::healthGradient, sizeof(Menu::healthGradient), 1, f);
        fwrite(&Menu::healthGradientColorR, sizeof(float), 1, f);
        fwrite(&Menu::healthGradientColorG, sizeof(float), 1, f);
        fwrite(&Menu::healthGradientColorB, sizeof(float), 1, f);
        // Appended popup-outline color keeps all existing config offsets intact.
        fwrite(&Menu::damagePopupOutlineColorR, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupOutlineColorG, sizeof(float), 1, f);
        fwrite(&Menu::damagePopupOutlineColorB, sizeof(float), 1, f);
        // Appended aimbot fields; older visuals.cfg files keep disabled defaults.
        fwrite(&AimConfig::enabled, sizeof(AimConfig::enabled), 1, f);
        fwrite(&AimConfig::fovRadius, sizeof(AimConfig::fovRadius), 1, f);
        fwrite(&AimConfig::lerpSpeed, sizeof(AimConfig::lerpSpeed), 1, f);
        fwrite(&AimConfig::rcsEnabled, sizeof(AimConfig::rcsEnabled), 1, f);
        fwrite(&AimConfig::rcsBullet, sizeof(AimConfig::rcsBullet), 1, f);
        fwrite(&AimConfig::rcsScale, sizeof(AimConfig::rcsScale), 1, f);
        fwrite(&AimConfig::activationKey, sizeof(AimConfig::activationKey), 1, f);
        // Appended menu sensitivity and supplied-aim settings preserve old config offsets.
        fwrite(&AimConfig::sensitivity, sizeof(AimConfig::sensitivity), 1, f);
        fwrite(&AimConfig::rcsSmoothing, sizeof(AimConfig::rcsSmoothing), 1, f);
        fwrite(&AimConfig::rcsJitter, sizeof(AimConfig::rcsJitter), 1, f);
        fwrite(&AimConfig::rcsDownBias, sizeof(AimConfig::rcsDownBias), 1, f);
        fwrite(&AimConfig::rcsStrength, sizeof(AimConfig::rcsStrength), 1, f);
        fwrite(&AimConfig::deathmatchMode, sizeof(AimConfig::deathmatchMode), 1, f);
        fclose(f);
    }

    void LoadConfigFromAppData() noexcept
    {
        const std::wstring path = GetConfigPath();
        if (path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
        auto rd = [&](void* p, size_t n) { return fread(p, n, 1, f) == 1; };
        if (!rd(&Menu::box, sizeof(Menu::box))) { fclose(f); return; }
        rd(&Menu::filledBox, sizeof(Menu::filledBox));
        rd(&Menu::health, sizeof(Menu::health));
        rd(&Menu::snapline, sizeof(Menu::snapline));
        rd(&Menu::boxCornerLength, sizeof(Menu::boxCornerLength));
        rd(&Menu::healthBarWidth, sizeof(Menu::healthBarWidth));
        rd(&Menu::filledOpacity, sizeof(Menu::filledOpacity));
        rd(&Menu::boxColorR, sizeof(float));
        rd(&Menu::boxColorG, sizeof(float));
        rd(&Menu::boxColorB, sizeof(float));
        rd(&Menu::filledColorR, sizeof(float));
        rd(&Menu::filledColorG, sizeof(float));
        rd(&Menu::filledColorB, sizeof(float));
        rd(&Menu::healthColorR, sizeof(float));
        rd(&Menu::healthColorG, sizeof(float));
        rd(&Menu::healthColorB, sizeof(float));
        rd(&Menu::snaplineColorR, sizeof(float));
        rd(&Menu::snaplineColorG, sizeof(float));
        rd(&Menu::snaplineColorB, sizeof(float));
        int anchor = 0;
        if (rd(&anchor, sizeof(anchor)) && anchor >= 0 && anchor <= 4)
            Menu::snaplineAnchor = static_cast<Menu::SnaplineAnchor>(anchor);
        // New settings are optional so pre-existing config files still load.
        rd(&Menu::armor, sizeof(Menu::armor));
        rd(&Menu::armorBarWidth, sizeof(Menu::armorBarWidth));
        rd(&Menu::armorColorR, sizeof(float));
        rd(&Menu::armorColorG, sizeof(float));
        rd(&Menu::armorColorB, sizeof(float));
        int healthPos = static_cast<int>(Menu::healthPosition);
        int armorPos = static_cast<int>(Menu::armorPosition);
        if (rd(&healthPos, sizeof(healthPos)) && healthPos >= 0 && healthPos <= 3)
            Menu::healthPosition = static_cast<Menu::BarPosition>(healthPos);
        if (rd(&armorPos, sizeof(armorPos)) && armorPos >= 0 && armorPos <= 3)
            Menu::armorPosition = static_cast<Menu::BarPosition>(armorPos);
        // Skeleton fields are appended, so older visuals.cfg files remain valid.
        rd(&Menu::skeleton, sizeof(Menu::skeleton));
        rd(&Menu::skeletonColorR, sizeof(float));
        rd(&Menu::skeletonColorG, sizeof(float));
        rd(&Menu::skeletonColorB, sizeof(float));
        rd(&Menu::skeletonOpacity, sizeof(float));
        // Optional appended fields keep existing visuals.cfg files compatible.
        rd(&Menu::playerName, sizeof(Menu::playerName));
        rd(&Menu::weaponIcon, sizeof(Menu::weaponIcon));
        rd(&Menu::distance, sizeof(Menu::distance));
        rd(&Menu::playerNameColorR, sizeof(float));
        rd(&Menu::playerNameColorG, sizeof(float));
        rd(&Menu::playerNameColorB, sizeof(float));
        rd(&Menu::weaponIconColorR, sizeof(float));
        rd(&Menu::weaponIconColorG, sizeof(float));
        rd(&Menu::weaponIconColorB, sizeof(float));
        rd(&Menu::distanceColorR, sizeof(float));
        rd(&Menu::distanceColorG, sizeof(float));
        rd(&Menu::distanceColorB, sizeof(float));
        rd(&Menu::hitMarker, sizeof(Menu::hitMarker));
        rd(&Menu::hitMarkerSound, sizeof(Menu::hitMarkerSound));
        rd(&Menu::hitMarkerScale, sizeof(Menu::hitMarkerScale));
        rd(&Menu::hitMarkerVolume, sizeof(Menu::hitMarkerVolume));
        rd(&Menu::hitMarkerColorR, sizeof(float));
        rd(&Menu::hitMarkerColorG, sizeof(float));
        rd(&Menu::hitMarkerColorB, sizeof(float));
        rd(&Menu::damagePopup, sizeof(Menu::damagePopup));
        rd(&Menu::damagePopupColorR, sizeof(float));
        rd(&Menu::damagePopupColorG, sizeof(float));
        rd(&Menu::damagePopupColorB, sizeof(float));
        rd(&Menu::damagePopupScale, sizeof(float));
        rd(&Menu::damagePopupStyle, sizeof(Menu::damagePopupStyle)); // legacy config compatibility
        rd(&Menu::damagePopupDurationMs, sizeof(float));
        rd(&Menu::damagePopupGrowth, sizeof(float));
        rd(&Menu::damagePopupBoldness, sizeof(float));
        rd(&Menu::damagePopupOutline, sizeof(Menu::damagePopupOutline));
        int filledStyle = static_cast<int>(Menu::filledBoxStyle);
        if (rd(&filledStyle, sizeof(filledStyle)) && filledStyle >= 0 && filledStyle <= 3)
            Menu::filledBoxStyle = static_cast<Menu::FilledBoxStyle>(filledStyle);
        rd(&Menu::filledGradientColorR, sizeof(float));
        rd(&Menu::filledGradientColorG, sizeof(float));
        rd(&Menu::filledGradientColorB, sizeof(float));
        rd(&Menu::healthGradient, sizeof(Menu::healthGradient));
        rd(&Menu::healthGradientColorR, sizeof(float));
        rd(&Menu::healthGradientColorG, sizeof(float));
        rd(&Menu::healthGradientColorB, sizeof(float));
        // Optional appended values: older visuals.cfg files retain defaults.
        rd(&Menu::damagePopupOutlineColorR, sizeof(float));
        rd(&Menu::damagePopupOutlineColorG, sizeof(float));
        rd(&Menu::damagePopupOutlineColorB, sizeof(float));
        // Optional appended fields preserve compatibility with existing configs.
        rd(&AimConfig::enabled, sizeof(AimConfig::enabled));
        rd(&AimConfig::fovRadius, sizeof(AimConfig::fovRadius));
        rd(&AimConfig::lerpSpeed, sizeof(AimConfig::lerpSpeed));
        rd(&AimConfig::rcsEnabled, sizeof(AimConfig::rcsEnabled));
        rd(&AimConfig::rcsBullet, sizeof(AimConfig::rcsBullet));
        rd(&AimConfig::rcsScale, sizeof(AimConfig::rcsScale));
        UINT savedAimKey = AimConfig::activationKey;
        if (rd(&savedAimKey, sizeof(savedAimKey)) && savedAimKey > 0 && savedAimKey < 256)
            AimConfig::activationKey = savedAimKey;
        else
            AimConfig::activationKey = VK_LBUTTON;
        // Missing field in an older config leaves the requested 2.11 default.
        rd(&AimConfig::sensitivity, sizeof(AimConfig::sensitivity));
        rd(&AimConfig::rcsSmoothing, sizeof(AimConfig::rcsSmoothing));
        rd(&AimConfig::rcsJitter, sizeof(AimConfig::rcsJitter));
        rd(&AimConfig::rcsDownBias, sizeof(AimConfig::rcsDownBias));
        rd(&AimConfig::rcsStrength, sizeof(AimConfig::rcsStrength));
        rd(&AimConfig::deathmatchMode, sizeof(AimConfig::deathmatchMode));
        // RCS defaults off for fresh configs. A saved user toggle is respected;
        // runtime reads below are guarded and fail closed if the supplied layout
        // does not validate in the current process.
        fclose(f);
        Menu::boxCornerLength = std::clamp(Menu::boxCornerLength, 10.0f, 100.0f);
        Menu::healthBarWidth = std::clamp(Menu::healthBarWidth, 1.0f, 12.0f);
        Menu::armorBarWidth = std::clamp(Menu::armorBarWidth, 1.0f, 12.0f);
        Menu::filledOpacity = std::clamp(Menu::filledOpacity, 0.05f, 1.0f);
        Menu::skeletonOpacity = std::clamp(Menu::skeletonOpacity, 0.05f, 1.0f);
        Menu::hitMarkerScale = std::clamp(Menu::hitMarkerScale, 0.35f, 1.0f);
        Menu::hitMarkerVolume = std::clamp(Menu::hitMarkerVolume, 0.0f, 2.0f);
        Menu::hitMarkerColorR = std::clamp(Menu::hitMarkerColorR, 0.0f, 1.0f);
        Menu::hitMarkerColorG = std::clamp(Menu::hitMarkerColorG, 0.0f, 1.0f);
        Menu::hitMarkerColorB = std::clamp(Menu::hitMarkerColorB, 0.0f, 1.0f);
        Menu::damagePopupColorR = std::clamp(Menu::damagePopupColorR, 0.0f, 1.0f);
        Menu::damagePopupColorG = std::clamp(Menu::damagePopupColorG, 0.0f, 1.0f);
        Menu::damagePopupColorB = std::clamp(Menu::damagePopupColorB, 0.0f, 1.0f);
        // Migrate the former built-in gold color to the new red damage style,
        // while leaving any user-selected custom color untouched.
        if (Menu::damagePopupColorR > 0.995f &&
            std::fabs(Menu::damagePopupColorG - 0.82f) < 0.005f &&
            std::fabs(Menu::damagePopupColorB - 0.22f) < 0.005f)
        {
            Menu::damagePopupColorG = 0.19f;
            Menu::damagePopupColorB = 0.25f;
        }
        Menu::damagePopupScale = std::clamp(Menu::damagePopupScale, 0.65f, 3.0f);
        Menu::damagePopupStyle = std::clamp(Menu::damagePopupStyle, 0, 4); // ignored legacy value
        Menu::damagePopupDurationMs = std::clamp(Menu::damagePopupDurationMs, 300.0f, 1800.0f);
        Menu::damagePopupGrowth = std::clamp(Menu::damagePopupGrowth, 0.0f, 2.0f);
        Menu::damagePopupBoldness = std::clamp(Menu::damagePopupBoldness, 0.0f, 1.0f);
        Menu::filledGradientColorR = std::clamp(Menu::filledGradientColorR, 0.0f, 1.0f);
        Menu::filledGradientColorG = std::clamp(Menu::filledGradientColorG, 0.0f, 1.0f);
        Menu::filledGradientColorB = std::clamp(Menu::filledGradientColorB, 0.0f, 1.0f);
        Menu::healthGradientColorR = std::clamp(Menu::healthGradientColorR, 0.0f, 1.0f);
        Menu::healthGradientColorG = std::clamp(Menu::healthGradientColorG, 0.0f, 1.0f);
        Menu::healthGradientColorB = std::clamp(Menu::healthGradientColorB, 0.0f, 1.0f);
        Menu::damagePopupOutlineColorR = std::clamp(Menu::damagePopupOutlineColorR, 0.0f, 1.0f);
        Menu::damagePopupOutlineColorG = std::clamp(Menu::damagePopupOutlineColorG, 0.0f, 1.0f);
        Menu::damagePopupOutlineColorB = std::clamp(Menu::damagePopupOutlineColorB, 0.0f, 1.0f);
        Menu::playerNameColorR = std::clamp(Menu::playerNameColorR, 0.0f, 1.0f);
        Menu::playerNameColorG = std::clamp(Menu::playerNameColorG, 0.0f, 1.0f);
        Menu::playerNameColorB = std::clamp(Menu::playerNameColorB, 0.0f, 1.0f);
        Menu::weaponIconColorR = std::clamp(Menu::weaponIconColorR, 0.0f, 1.0f);
        Menu::weaponIconColorG = std::clamp(Menu::weaponIconColorG, 0.0f, 1.0f);
        Menu::weaponIconColorB = std::clamp(Menu::weaponIconColorB, 0.0f, 1.0f);
        Menu::distanceColorR = std::clamp(Menu::distanceColorR, 0.0f, 1.0f);
        Menu::distanceColorG = std::clamp(Menu::distanceColorG, 0.0f, 1.0f);
        Menu::distanceColorB = std::clamp(Menu::distanceColorB, 0.0f, 1.0f);
        AimConfig::fovRadius = std::clamp(AimConfig::fovRadius, 20.0f, 400.0f);
        AimConfig::lerpSpeed = std::clamp(AimConfig::lerpSpeed, 1.0f, 60.0f);
        AimConfig::sensitivity = std::clamp(AimConfig::sensitivity, 0.10f, 10.0f);
        AimConfig::rcsSmoothing = std::clamp(AimConfig::rcsSmoothing, 1.0f, 20.0f);
        AimConfig::rcsDownBias = 0.0f;
        AimConfig::rcsJitter = 0.0f;
        AimConfig::rcsStrength = std::clamp(AimConfig::rcsStrength, 0.0f, 2.0f);
        AimConfig::rcsBullet = std::clamp(AimConfig::rcsBullet, 0, 10);
        AimConfig::rcsScale = std::clamp(AimConfig::rcsScale, 0.0f, 2.0f);
    }


    void Log(const wchar_t* message) noexcept
    {
        OutputDebugStringW(L"[Overlay] ");
        OutputDebugStringW(message);
        OutputDebugStringW(L"\n");
    }

    void LogLastError(const wchar_t* operation) noexcept
    {
        const DWORD error = GetLastError();
        wchar_t buffer[256]{};
        swprintf_s(buffer, L"%s failed (Win32 error %lu)", operation, error);
        Log(buffer);
    }

    void LogHr(const wchar_t* operation, HRESULT hr) noexcept
    {
        wchar_t buffer[256]{};
        swprintf_s(buffer, L"%s failed (HRESULT 0x%08X)", operation, static_cast<unsigned int>(hr));
        Log(buffer);
    }

    template <typename T>
    void SafeRelease(T*& object) noexcept
    {
        if (object) { object->Release(); object = nullptr; }
    }

    void GetScreenSize() noexcept
    {
        g_width = GetSystemMetrics(SM_CXSCREEN);
        g_height = GetSystemMetrics(SM_CYSCREEN);
        if (g_width <= 0) g_width = 1920;
        if (g_height <= 0) g_height = 1080;
    }

    bool CreateCompositionResources(HWND hwnd) noexcept
    {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL selected{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3U, D3D11_SDK_VERSION,
            &g_d3dDevice, &selected, &g_d3dContext);
        if (FAILED(hr)) { LogHr(L"D3D11CreateDevice", hr); return false; }

        hr = g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&g_dxgiDevice));
        if (FAILED(hr) || !g_dxgiDevice) { LogHr(L"QueryInterface(IDXGIDevice)", hr); return false; }

        IDXGIAdapter* adapter = nullptr;
        hr = g_dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr) || !adapter) { LogHr(L"IDXGIDevice::GetAdapter", hr); return false; }
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&g_dxgiFactory));
        adapter->Release();
        if (FAILED(hr) || !g_dxgiFactory) { LogHr(L"IDXGIAdapter::GetParent(IDXGIFactory2)", hr); return false; }
        g_dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(g_width);
        desc.Height = static_cast<UINT>(g_height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        hr = g_dxgiFactory->CreateSwapChainForComposition(g_d3dDevice, &desc, nullptr, &g_swapChain);
        if (FAILED(hr) || !g_swapChain) { LogHr(L"CreateSwapChainForComposition", hr); return false; }

        hr = DCompositionCreateDevice(g_dxgiDevice, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&g_dcompDevice));
        if (FAILED(hr) || !g_dcompDevice) { LogHr(L"DCompositionCreateDevice", hr); return false; }

        hr = g_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_dcompTarget);
        if (FAILED(hr) || !g_dcompTarget) { LogHr(L"CreateTargetForHwnd", hr); return false; }

        hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
        if (FAILED(hr) || !g_dcompVisual) { LogHr(L"CreateVisual", hr); return false; }

        hr = g_dcompVisual->SetContent(g_swapChain);
        if (SUCCEEDED(hr)) hr = g_dcompTarget->SetRoot(g_dcompVisual);
        if (SUCCEEDED(hr)) hr = g_dcompDevice->Commit();
        if (FAILED(hr)) { LogHr(L"DirectComposition Commit", hr); return false; }
        return true;
    }

    // Commit the DirectComposition visual tree after window visibility/placement
    // changes. Swap-chain content is still submitted by Present(); this commits
    // only the composition tree state.
    void CommitComposition() noexcept
    {
        if (!g_dcompDevice) return;
        const HRESULT hr = g_dcompDevice->Commit();
        if (FAILED(hr)) LogHr(L"IDCompositionDevice::Commit", hr);
    }

    void DestroyCompositionResources() noexcept
    {
        if (g_dcompTarget) g_dcompTarget->SetRoot(nullptr);
        if (g_dcompDevice) g_dcompDevice->Commit();
        SafeRelease(g_dcompVisual);
        SafeRelease(g_dcompTarget);
        SafeRelease(g_dcompDevice);
        SafeRelease(g_swapChain);
        SafeRelease(g_dxgiFactory);
        SafeRelease(g_dxgiDevice);
        SafeRelease(g_d3dContext);
        SafeRelease(g_d3dDevice);
    }

    bool CreateSwapChainTargetBitmap() noexcept
    {
        if (!g_swapChain || !g_d2dTarget) return false;
        IDXGISurface* surface = nullptr;
        HRESULT hr = g_swapChain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(&surface));
        if (FAILED(hr) || !surface) { LogHr(L"IDXGISwapChain1::GetBuffer", hr); return false; }

        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f; props.dpiY = 96.0f;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        SafeRelease(g_swapChainBitmap);
        hr = g_d2dTarget->CreateBitmapFromDxgiSurface(surface, &props, &g_swapChainBitmap);
        surface->Release();
        if (FAILED(hr) || !g_swapChainBitmap) { LogHr(L"CreateBitmapFromDxgiSurface", hr); return false; }
        g_d2dTarget->SetTarget(g_swapChainBitmap);
        return true;
    }

    bool ResizeCompositionSurface() noexcept
    {
        if (!g_swapChain || !g_d2dTarget) return false;
        g_d2dTarget->SetTarget(nullptr);
        SafeRelease(g_swapChainBitmap);
        const HRESULT hr = g_swapChain->ResizeBuffers(0, static_cast<UINT>(g_width), static_cast<UINT>(g_height), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { LogHr(L"IDXGISwapChain1::ResizeBuffers", hr); return false; }
        if (!CreateSwapChainTargetBitmap()) return false;
        CommitComposition();
        g_frameDirty = true;
        g_menuCacheDirty = true;
        return true;
    }

    template <typename T>
    bool SafeRead(uintptr_t address, T& out) noexcept
    {
        static_assert(std::is_trivially_copyable<T>::value, "SafeRead only for trivially-copyable");
        if (address < 0x10000ULL) return false;
#ifdef _WIN64
        if (address > 0x00007FFFFFFFFFFFULL) return false;
#endif
        if ((address & (alignof(T) - 1U)) != 0) return false;
#if defined(_MSC_VER)
        __try { out = *reinterpret_cast<const T*>(address); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
        return false;
#endif
    }

    bool SafeReadBytes(uintptr_t address, void* destination, size_t size) noexcept
    {
        if (address < 0x10000ULL || !destination || !size) return false;
#ifdef _WIN64
        if (address > 0x00007FFFFFFFFFFFULL || size > 0x00007FFFFFFFFFFFULL - address) return false;
#endif
#if defined(_MSC_VER)
        __try
        {
            std::memcpy(destination, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        return false;
#endif
    }

    void ApplyPlayerSchemaOffset(const char* key, uintptr_t value) noexcept
    {
        if (!key || !value) return;
        if (std::strcmp(key, "m_iszPlayerName") == 0) g_playerSchema.playerName = value;
        else if (std::strcmp(key, "m_hPlayerPawn") == 0) g_playerSchema.playerPawnHandle = value;
        else if (std::strcmp(key, "m_pWeaponServices") == 0) g_playerSchema.weaponServices = value;
        else if (std::strcmp(key, "m_hActiveWeapon") == 0) g_playerSchema.activeWeapon = value;
        else if (std::strcmp(key, "m_AttributeManager") == 0) g_playerSchema.attributeManager = value;
        else if (std::strcmp(key, "m_Item") == 0) g_playerSchema.item = value;
        else if (std::strcmp(key, "m_iItemDefinitionIndex") == 0) g_playerSchema.itemDefinitionIndex = value;
        else if (std::strcmp(key, "m_pActionTrackingServices") == 0) g_playerSchema.actionTrackingServices = value;
    }

    bool ParsePlayerSchemaFile(const wchar_t* path) noexcept
    {
        if (!path || !*path) return false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"rt") != 0 || !file) return false;

        char line[2048]{};
        while (std::fgets(line, static_cast<int>(sizeof(line)), file))
        {
            if (char* comment = std::strstr(line, "//")) *comment = '\0';
            char* equals = std::strchr(line, '=');
            if (!equals) continue;

            char* keyEnd = equals;
            while (keyEnd > line && std::isspace(static_cast<unsigned char>(keyEnd[-1]))) --keyEnd;
            char* keyBegin = keyEnd;
            while (keyBegin > line)
            {
                const unsigned char c = static_cast<unsigned char>(keyBegin[-1]);
                if (!(std::isalnum(c) || c == '_')) break;
                --keyBegin;
            }
            if (keyBegin == keyEnd) continue;

            char* valueBegin = equals + 1;
            while (*valueBegin && std::isspace(static_cast<unsigned char>(*valueBegin))) ++valueBegin;
            if (!*valueBegin) continue;
            char* valueEnd = nullptr;
            const unsigned long long parsed = std::strtoull(valueBegin, &valueEnd, 0);
            if (valueEnd == valueBegin) continue;

            char key[96]{};
            const size_t keyLength = static_cast<size_t>(keyEnd - keyBegin);
            if (keyLength == 0 || keyLength >= sizeof(key)) continue;
            std::memcpy(key, keyBegin, keyLength);
            ApplyPlayerSchemaOffset(key, static_cast<uintptr_t>(parsed));
        }
        std::fclose(file);
        return true;
    }

    bool ParseClientOffsetsFile(const wchar_t* path) noexcept
    {
        if (!path || !*path) return false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"rt") != 0 || !file) return false;

        char line[2048]{};
        while (std::fgets(line, static_cast<int>(sizeof(line)), file))
        {
            if (char* comment = std::strstr(line, "//")) *comment = '\0';
            char* equals = std::strchr(line, '=');
            if (!equals) continue;

            char* keyEnd = equals;
            while (keyEnd > line && std::isspace(static_cast<unsigned char>(keyEnd[-1]))) --keyEnd;
            char* keyBegin = keyEnd;
            while (keyBegin > line)
            {
                const unsigned char c = static_cast<unsigned char>(keyBegin[-1]);
                if (!(std::isalnum(c) || c == '_')) break;
                --keyBegin;
            }
            constexpr char wanted[] = "dwLocalPlayerController";
            if (static_cast<size_t>(keyEnd - keyBegin) != sizeof(wanted) - 1 ||
                std::memcmp(keyBegin, wanted, sizeof(wanted) - 1) != 0)
                continue;

            char* valueBegin = equals + 1;
            while (*valueBegin && std::isspace(static_cast<unsigned char>(*valueBegin))) ++valueBegin;
            char* valueEnd = nullptr;
            const unsigned long long parsed = std::strtoull(valueBegin, &valueEnd, 0);
            if (valueEnd != valueBegin && parsed != 0)
                g_dwLocalPlayerControllerOffset = static_cast<uintptr_t>(parsed);
        }
        std::fclose(file);
        return true;
    }

    bool TryLoadClientOffsetsBeside(HMODULE module) noexcept
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (!slash) return false;
        slash[1] = L'\0';
        if (wcscat_s(modulePath, MAX_PATH, L"output\\offsets.hpp") != 0) return false;
        return ParseClientOffsetsFile(modulePath);
    }

    bool TryLoadPlayerSchemaBeside(HMODULE module) noexcept
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (!slash) return false;
        slash[1] = L'\0';
        if (wcscat_s(modulePath, MAX_PATH, L"output\\client_dll.hpp") != 0) return false;
        return ParsePlayerSchemaFile(modulePath);
    }

    bool LoadPlayerSchemaOffsets() noexcept
    {
        // Start with the build-specific offsets. The optional generated header
        // is still parsed for compatibility, but these supplied values take
        // precedence so a missing/stale output/client_dll.hpp cannot disable
        // the name or weapon labels.
        g_playerSchema = {};

        // Keep the existing lookup path available for projects that ship the
        // generated schema beside the executable or DLL.
        bool opened = TryLoadPlayerSchemaBeside(nullptr);
        bool globalOffsetsOpened = TryLoadClientOffsetsBeside(nullptr);
        if (g_module)
        {
            opened = TryLoadPlayerSchemaBeside(g_module) || opened;
            globalOffsetsOpened = TryLoadClientOffsetsBeside(g_module) || globalOffsetsOpened;
        }
        (void)opened;
        (void)globalOffsetsOpened;

        // Controller fields.
        g_playerSchema.playerName = Offsets::m_iszPlayerName;
        g_playerSchema.playerPawnHandle = Offsets::m_hPawn;

        // Active weapon -> C_EconEntity::m_AttributeManager -> m_Item ->
        // C_EconItemView::m_iItemDefinitionIndex.
        g_playerSchema.weaponServices = Offsets::m_pWeaponServices;
        g_playerSchema.activeWeapon = Offsets::m_hActiveWeapon;
        g_playerSchema.attributeManager = Offsets::m_AttributeManager;
        g_playerSchema.item = Offsets::m_Item;
        g_playerSchema.itemDefinitionIndex = Offsets::m_iItemDefinitionIndex;

        // Use the generated client schema when present; otherwise fall back to
        // the current client-build offset. This field moves between CS2 builds.
        if (!g_playerSchema.actionTrackingServices)
            g_playerSchema.actionTrackingServices = Offsets::m_pActionTrackingServices;

        g_playerSchemaLoaded = true;
        return true;
    }

    uintptr_t PlayerPawnHandleOffset() noexcept
    {
        return g_playerSchema.playerPawnHandle ? g_playerSchema.playerPawnHandle : Offsets::m_hPawn;
    }

    float Vec3DistanceSquared(const Vec3& a, const Vec3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    bool UpdateCachedSkeleton(CachedPawn& cached, uintptr_t pawn, const Vec3& origin,
        ULONGLONG now, std::array<Vec3, kSkeletonBoneCount>& out) noexcept
    {
        if (!pawn) return false;

        const bool retryPointer = !cached.boneArray &&
            (cached.boneArrayLastResolve == 0 || now - cached.boneArrayLastResolve >= 250ULL);
        const bool refreshPointer = retryPointer ||
            (cached.boneArray && (now - cached.boneArrayLastResolve >= kSkeletonPointerRefreshMs));
        if (refreshPointer)
        {
            cached.boneArrayLastResolve = now;
            uintptr_t sceneNode = 0;
            uintptr_t boneArray = 0;
            if (!SafeRead(pawn + Offsets::m_pGameSceneNode, sceneNode) || !sceneNode ||
                !SafeRead(sceneNode + Offsets::m_modelState + 0x80ULL, boneArray) || !boneArray)
            {
                cached.boneArray = 0;
                cached.bonesValid = false;
                return false;
            }
            cached.boneArray = boneArray;
            cached.bonesValid = false;
        }

        if (!cached.boneArray) return false;
        if (!cached.bonesValid || now - cached.boneSampleTime >= kSkeletonBoneRefreshMs)
        {
            std::array<std::byte, kSkeletonBoneBytes> block{};
            if (!SafeReadBytes(cached.boneArray, block.data(), block.size()))
            {
                cached.boneArray = 0;
                cached.boneArrayLastResolve = now;
                cached.bonesValid = false;
                return false;
            }

            auto candidate = cached.bones;
            for (const size_t index : kSkeletonUsedBones)
            {
                const size_t offset = index * kSkeletonBoneStride;
                if (offset + sizeof(Vec3) > block.size()) return false;
                std::memcpy(&candidate[index], block.data() + offset, sizeof(Vec3));
                const Vec3& bone = candidate[index];
                if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.z) ||
                    Vec3DistanceSquared(bone, origin) > 155.0f * 155.0f)
                {
                    cached.bonesValid = false;
                    return false;
                }
            }
            cached.bones = candidate;
            cached.bonesValid = true;
            cached.boneSampleTime = now;
        }

        if (!cached.bonesValid) return false;
        out = cached.bones;
        return true;
    }

    struct EntityListReadCache
    {
        uintptr_t entries[64]{};
        bool loaded[64]{};
    };

    uintptr_t GetCachedEntityListEntry(uintptr_t entityList, uint32_t index, EntityListReadCache& cache) noexcept
    {
        if (!entityList) return 0;
        const uint32_t bucket = (index & 0x7FFFU) >> 9U;
        if (bucket >= 64U) return 0;
        if (!cache.loaded[bucket])
        {
            cache.loaded[bucket] = true;
            SafeRead(entityList + 8ULL * bucket + 16ULL, cache.entries[bucket]);
        }
        return cache.entries[bucket];
    }

    uintptr_t ResolvePawn(uintptr_t entityList, uint32_t handle, EntityListReadCache& cache) noexcept
    {
        if (!entityList || !handle || handle == 0xFFFFFFFFu) return 0;
        const uintptr_t listEntry = GetCachedEntityListEntry(entityList, handle, cache);
        if (!listEntry) return 0;
        uintptr_t pawn = 0;
        const uintptr_t pawnAddress = listEntry + 112ULL * (handle & 0x1FFULL);
        if (!SafeRead(pawnAddress, pawn)) return 0;
        return pawn;
    }

    bool WorldToScreen(const Mat4& matrix, const Vec3& position, const Viewport& viewport,
        float& screenX, float& screenY, float& clipDepth) noexcept
    {
        clipDepth = matrix.m[3][0] * position.x + matrix.m[3][1] * position.y +
            matrix.m[3][2] * position.z + matrix.m[3][3];
        if (!std::isfinite(clipDepth) || clipDepth <= 0.05f) return false;

        const float clipX = matrix.m[0][0] * position.x + matrix.m[0][1] * position.y +
            matrix.m[0][2] * position.z + matrix.m[0][3];
        const float clipY = matrix.m[1][0] * position.x + matrix.m[1][1] * position.y +
            matrix.m[1][2] * position.z + matrix.m[1][3];
        const float inverseW = 1.0f / clipDepth;
        screenX = viewport.x + (viewport.w * 0.5f) * (1.0f + clipX * inverseW);
        screenY = viewport.y + (viewport.h * 0.5f) * (1.0f - clipY * inverseW);
        return std::isfinite(screenX) && std::isfinite(screenY);
    }

    bool CreateBrush(ID2D1SolidColorBrush** brush, const D2D1_COLOR_F& color) noexcept
    {
        if (!g_d2dTarget || !brush) return false;
        const HRESULT hr = g_d2dTarget->CreateSolidColorBrush(color, brush);
        if (FAILED(hr)) LogHr(L"CreateSolidColorBrush", hr);
        return SUCCEEDED(hr);
    }

    [[nodiscard]] const MaotawRuntime::WeaponIcons::IconSet* WeaponIconForId(uint16_t id) noexcept
    {
        using namespace MaotawRuntime::WeaponIcons;
        switch (id)
        {
        case 1: return &deagle; case 2: return &elite;
        case 3: return &fiveseven; case 4: return &glock;
        case 7: return &ak47; case 8: return &aug;
        case 9: return &awp; case 10: return &famas;
        case 11: return &g3sg1; case 13: return &galilar;
        case 14: return &m249; case 16: return &m4a1;
        case 17: return &mac10; case 19: return &p90;
        case 23: return &mp5sd; case 24: return &ump45;
        case 25: return &xm1014; case 26: return &bizon;
        case 27: return &mag7; case 28: return &nova;
        case 29: return &mag7; case 30: return &tec9;
        case 31: return &taser; case 32: return &p2000;
        case 33: return &mp7; case 34: return &mp9;
        case 35: return &nova; case 36: return &p250;
        case 38: return &scar20; case 39: return &sg556;
        case 40: return &ssg08;
        case 41: case 42: case 59: return &knife;
        case 43: case 44: case 45: case 46: case 47: case 48: return &knife;
        case 49: return &c4;
        case 60: return &m4a1_silencer; case 61: return &usp_silencer;
        case 63: return &cz75a; case 64: return &revolver;
        case 500: case 503: case 505: case 506: case 507: case 508:
        case 509: case 512: case 514: case 515: case 516: case 517:
        case 518: case 519: case 520: case 521: case 522: case 523:
        case 525: case 526:
            return &knife;
        default: return nullptr;
        }
    }

    [[nodiscard]] const char* WeaponLabelForId(uint16_t id) noexcept
    {
        switch (id)
        {
        case 1: return "DEAGLE"; case 2: return "DUALIES";
        case 3: return "FIVESEVEN"; case 4: return "GLOCK";
        case 7: return "AK-47"; case 8: return "AUG";
        case 9: return "AWP"; case 10: return "FAMAS";
        case 11: return "G3SG1"; case 13: return "GALIL";
        case 14: return "M249"; case 16: return "M4A4";
        case 17: return "MAC-10"; case 19: return "P90";
        case 23: return "MP5"; case 24: return "UMP-45";
        case 25: return "XM1014"; case 26: return "BIZON";
        case 27: return "MAG-7"; case 28: return "NEGEV";
        case 29: return "SAWED-OFF"; case 30: return "TEC-9";
        case 31: return "ZEUS"; case 32: return "P2000";
        case 33: return "MP7"; case 34: return "MP9";
        case 35: return "NOVA"; case 36: return "P250";
        case 38: return "SCAR-20"; case 39: return "SG553";
        case 40: return "SSG-08"; case 49: return "C4";
        case 60: return "M4A1-S"; case 61: return "USP-S";
        case 63: return "CZ75"; case 64: return "R8";
        default:
            if (id == 42 || id == 59 || (id >= 500 && id <= 530)) return "KNIFE";
            return nullptr;
        }
    }

    [[nodiscard]] const MaotawRuntime::WeaponIcons::AlphaSprite& WeaponSpriteForBoxHeight(
        const MaotawRuntime::WeaponIcons::IconSet& set, int boxHeight) noexcept
    {
        if (boxHeight >= 78) return set.nearIcon;
        if (boxHeight >= 44) return set.midIcon;
        return set.farIcon;
    }

    WeaponIconBitmapEntry* GetWeaponIconBitmaps(
        const MaotawRuntime::WeaponIcons::AlphaSprite& sprite) noexcept
    {
        if (!g_d2dTarget || !sprite.alpha || sprite.width <= 0 || sprite.height <= 0)
            return nullptr;

        const uint32_t red = static_cast<uint32_t>(std::lround(std::clamp(Menu::weaponIconColorR, 0.0f, 1.0f) * 255.0f));
        const uint32_t green = static_cast<uint32_t>(std::lround(std::clamp(Menu::weaponIconColorG, 0.0f, 1.0f) * 255.0f));
        const uint32_t blue = static_cast<uint32_t>(std::lround(std::clamp(Menu::weaponIconColorB, 0.0f, 1.0f) * 255.0f));
        const uint32_t tintKey = (red << 16) | (green << 8) | blue;
        for (size_t i = 0; i < g_weaponIconBitmapCount; ++i)
        {
            auto& entry = g_weaponIconBitmaps[i];
            if (entry.alpha == sprite.alpha && entry.width == sprite.width &&
                entry.height == sprite.height && entry.tintKey == tintKey && entry.foreground)
                return &entry;
        }
        if (g_weaponIconBitmapCount >= g_weaponIconBitmaps.size()) return nullptr;

        const size_t pixelCount = static_cast<size_t>(sprite.width) *
            static_cast<size_t>(sprite.height);
        std::vector<uint32_t> foregroundPixels(pixelCount);
        std::vector<uint32_t> shadowPixels(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint32_t a = sprite.alpha[i];
            const uint32_t r = (red * a + 127U) / 255U;
            const uint32_t g = (green * a + 127U) / 255U;
            const uint32_t b = (blue * a + 127U) / 255U;
            // D2D premultiplied BGRA pixel layout.
            foregroundPixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
            shadowPixels[i] = (a << 24); // premultiplied black shadow
        }

        D2D1_BITMAP_PROPERTIES properties{};
        properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        properties.dpiX = 96.0f;
        properties.dpiY = 96.0f;

        WeaponIconBitmapEntry entry{};
        entry.alpha = sprite.alpha;
        entry.width = sprite.width;
        entry.height = sprite.height;
        entry.tintKey = tintKey;
        const UINT32 pitch = static_cast<UINT32>(sprite.width * sizeof(uint32_t));
        HRESULT hr = g_d2dTarget->CreateBitmap(
            D2D1_SIZE_U{ static_cast<UINT32>(sprite.width), static_cast<UINT32>(sprite.height) },
            foregroundPixels.data(), pitch, &properties, &entry.foreground);
        if (FAILED(hr) || !entry.foreground) return nullptr;

        hr = g_d2dTarget->CreateBitmap(
            D2D1_SIZE_U{ static_cast<UINT32>(sprite.width), static_cast<UINT32>(sprite.height) },
            shadowPixels.data(), pitch, &properties, &entry.shadow);
        if (FAILED(hr)) entry.shadow = nullptr;

        g_weaponIconBitmaps[g_weaponIconBitmapCount] = entry;
        return &g_weaponIconBitmaps[g_weaponIconBitmapCount++];
    }

    void DrawWeaponSpriteCentered(float centerX, float y,
        const MaotawRuntime::WeaponIcons::AlphaSprite& sprite) noexcept
    {
        WeaponIconBitmapEntry* bitmaps = GetWeaponIconBitmaps(sprite);
        if (!bitmaps || !g_d2dTarget) return;
        const float left = centerX - static_cast<float>(sprite.width) * 0.5f;
        const D2D1_RECT_F destination{
            left, y, left + static_cast<float>(sprite.width),
            y + static_cast<float>(sprite.height)
        };
        if (bitmaps->shadow)
        {
            const D2D1_RECT_F shadowDestination{
                destination.left + 1.0f, destination.top + 1.0f,
                destination.right + 1.0f, destination.bottom + 1.0f
            };
            g_d2dTarget->DrawBitmap(bitmaps->shadow, &shadowDestination, 1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        }
        g_d2dTarget->DrawBitmap(bitmaps->foreground, &destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    }

    void DrawInfoTextCentered(const wchar_t* text, float centerX, float y,
        float maxWidth, ID2D1SolidColorBrush* textBrush) noexcept
    {
        if (!text || !*text || !g_d2dTarget || !g_smallTextFormat ||
            !g_brushOutline)
            return;
        if (!textBrush) textBrush = g_brushText;
        if (!textBrush) return;
        const float halfWidth = maxWidth * 0.5f;
        const D2D1_RECT_F shadowRect{
            centerX - halfWidth + 1.0f, y + 1.0f,
            centerX + halfWidth + 1.0f, y + 14.0f
        };
        const D2D1_RECT_F textRect{
            centerX - halfWidth, y,
            centerX + halfWidth, y + 13.0f
        };
        const UINT32 length = static_cast<UINT32>(wcslen(text));
        g_d2dTarget->DrawText(text, length, g_smallTextFormat, shadowRect,
            g_brushOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        g_d2dTarget->DrawText(text, length, g_smallTextFormat, textRect,
            textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void DrawPlayerInfo(const EntityDrawData& entity) noexcept
    {
        if ((!Menu::playerName && !Menu::weaponIcon && !Menu::distance) ||
            !entity.valid || !g_d2dTarget) return;
        const float centerX = (entity.left + entity.right) * 0.5f;
        const float boxHeight = entity.bottom - entity.top;
        const float textWidth = std::clamp((entity.right - entity.left) * 2.6f, 72.0f, 180.0f);

        if (Menu::playerName && entity.playerName[0])
        {
            wchar_t name[32]{};
            const int maxChars = boxHeight >= 78.0f ? 18 : (boxHeight >= 44.0f ? 15 : 12);
            int n = 0;
            for (; n < maxChars && entity.playerName[n]; ++n)
                name[n] = static_cast<wchar_t>(std::toupper(
                    static_cast<unsigned char>(entity.playerName[n])));
            name[n] = L'\0';
            DrawInfoTextCentered(name, centerX, entity.top - 15.0f, textWidth, g_brushPlayerName);
        }

        float infoY = entity.bottom + 3.0f;
        if (Menu::weaponIcon && entity.weaponId != 0)
        {
            if (const auto* icon = WeaponIconForId(entity.weaponId))
            {
                const auto& sprite = WeaponSpriteForBoxHeight(*icon,
                    static_cast<int>(std::lround(boxHeight)));
                DrawWeaponSpriteCentered(centerX, infoY, sprite);
                infoY += static_cast<float>(sprite.height) + 2.0f;
            }
            else if (const char* label = WeaponLabelForId(entity.weaponId))
            {
                wchar_t weaponLabel[24]{};
                size_t i = 0;
                for (; label[i] && i + 1 < std::size(weaponLabel); ++i)
                    weaponLabel[i] = static_cast<wchar_t>(label[i]);
                weaponLabel[i] = L'\0';
                DrawInfoTextCentered(weaponLabel, centerX, infoY, textWidth, g_brushWeaponInfo);
                infoY += 14.0f;
            }
        }

        if (Menu::distance && entity.distanceValid)
        {
            wchar_t distanceLabel[16]{};
            swprintf_s(distanceLabel, L"%dM", (std::max)(0, entity.distanceMeters));
            DrawInfoTextCentered(distanceLabel, centerX, infoY, textWidth, g_brushDistance);
        }
    }

    void ReleaseWeaponIconBitmaps() noexcept
    {
        for (auto& entry : g_weaponIconBitmaps)
        {
            SafeRelease(entry.foreground);
            SafeRelease(entry.shadow);
            entry.alpha = nullptr;
            entry.width = 0;
            entry.height = 0;
        }
        g_weaponIconBitmapCount = 0;
    }

    void ReleaseDrawingResources() noexcept
    {
        if (g_d2dTarget) g_d2dTarget->SetTarget(nullptr);
        ReleaseWeaponIconBitmaps();
        SafeRelease(g_menuBitmap);
        SafeRelease(g_swapChainBitmap);
        SafeRelease(g_brushPickerSaturation);
        SafeRelease(g_brushPickerValue);
        SafeRelease(g_brushPickerHue);
        g_pickerGradientHue = -1.0f;
        g_pickerSaturationGradientDirty = true;
        g_menuCacheDirty = true;
        SafeRelease(g_brushBoxColor);
        SafeRelease(g_brushFilledColor);
        SafeRelease(g_brushHealthColor);
        SafeRelease(g_brushHealthBackgroundColor);
        SafeRelease(g_brushArmorColor);
        SafeRelease(g_brushArmorBack);
        SafeRelease(g_brushSkeleton);
        SafeRelease(g_brushSnaplineColor);
        SafeRelease(g_brushSnapline);
        SafeRelease(g_brushCheckboxBack);
        SafeRelease(g_brushSliderFill);
        SafeRelease(g_brushSliderTrack);
        SafeRelease(g_brushMenuMuted);
        SafeRelease(g_brushMenuText);
        SafeRelease(g_brushMenuBorder);
        SafeRelease(g_brushMenuPanel);
        SafeRelease(g_brushMenuBack);
        SafeRelease(g_brushCodeValue);
        SafeRelease(g_brushCodeKey);
        SafeRelease(g_brushHitMarker);
        SafeRelease(g_brushDamagePopupOutline);
        SafeRelease(g_brushDistance);
        SafeRelease(g_brushWeaponInfo);
        SafeRelease(g_brushPlayerName);
        SafeRelease(g_brushHealthLow);
        SafeRelease(g_brushHealthMed);
        SafeRelease(g_brushHealthHigh);
        SafeRelease(g_brushHealthBack);
        SafeRelease(g_brushText);
        SafeRelease(g_brushEntityFill);
        SafeRelease(g_brushAccent);
        SafeRelease(g_brushOutline);
        SafeRelease(g_codeTextFormat);
        SafeRelease(g_menuHeroFormat);
        SafeRelease(g_menuTitleFormat);
        SafeRelease(g_menuTextFormat);
        SafeRelease(g_smallTextFormat);
        SafeRelease(g_damageTextFormat);
        SafeRelease(g_textFormat);
        SafeRelease(g_writeFactory);
        SafeRelease(g_d2dTarget);
        SafeRelease(g_d2dDevice);
        SafeRelease(g_d2dFactory);
    }

    bool CreatePickerGradientResources() noexcept;
    bool RebuildPickerSaturationGradient() noexcept;

    bool CreateDrawingResources() noexcept
    {
        if (!g_dxgiDevice || !g_swapChain) return false;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), reinterpret_cast<void**>(&g_d2dFactory));
        if (FAILED(hr)) { LogHr(L"D2D1CreateFactory", hr); return false; }

        hr = g_d2dFactory->CreateDevice(g_dxgiDevice, &g_d2dDevice);
        if (FAILED(hr) || !g_d2dDevice) { LogHr(L"CreateDevice", hr); return false; }

        hr = g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dTarget);
        if (FAILED(hr) || !g_d2dTarget) { LogHr(L"CreateDeviceContext", hr); return false; }
        g_d2dTarget->SetDpi(96.0f, 96.0f);
        g_d2dTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_d2dTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        if (!CreateSwapChainTargetBitmap()) return false;

        D2D1_BITMAP_PROPERTIES1 menuProps{};
        menuProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        menuProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        menuProps.dpiX = 96.0f; menuProps.dpiY = 96.0f;
        menuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        hr = g_d2dTarget->CreateBitmap(D2D1_SIZE_U{ kMenuSurfaceWidth, kMenuSurfaceHeight }, nullptr, 0, &menuProps, &g_menuBitmap);
        if (FAILED(hr) || !g_menuBitmap) { LogHr(L"CreateBitmap(menu)", hr); return false; }
        g_menuCacheDirty = true;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_writeFactory));
        if (FAILED(hr)) { LogHr(L"DWriteCreateFactory", hr); return false; }

        auto makeFormat = [&](const wchar_t* face, DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** out) -> bool {
            hr = g_writeFactory->CreateTextFormat(face, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
            return SUCCEEDED(hr);
            };

        if (!makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 13.0f, &g_textFormat) ||
            !makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, &g_smallTextFormat) ||
            !makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 10.0f, &g_menuTextFormat) ||
            !makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, &g_menuTitleFormat) ||
            !makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 20.0f, &g_menuHeroFormat) ||
            !makeFormat(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 10.5f, &g_codeTextFormat))
        {
            Log(L"CreateTextFormat failed");
            return false;
        }

        // One fixed damage-number face: Bahnschrift ExtraBold gives numbers
        // a compact, crisp gaming-HUD look without a runtime font selector.
        if (!makeFormat(L"Bahnschrift", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 21.0f, &g_damageTextFormat))
        {
            Log(L"Create damage text format failed");
            return false;
        }
        g_damageTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_damageTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        g_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_smallTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_smallTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_menuTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_menuTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_menuTitleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_menuTitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_menuHeroFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_menuHeroFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_codeTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_codeTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (!CreateBrush(&g_brushMenuBack, Color(0.003f, 0.004f, 0.006f, 0.998f)) ||
            !CreateBrush(&g_brushMenuPanel, Color(0.007f, 0.009f, 0.012f, 0.998f)) ||
            !CreateBrush(&g_brushMenuBorder, Color(0.085f, 0.105f, 0.125f, 0.72f)) ||
            !CreateBrush(&g_brushMenuText, Color(0.87f, 0.90f, 0.93f, 1.00f)) ||
            !CreateBrush(&g_brushMenuMuted, Color(0.34f, 0.40f, 0.45f, 1.00f)) ||
            !CreateBrush(&g_brushSliderTrack, Color(0.025f, 0.045f, 0.040f, 1.00f)) ||
            !CreateBrush(&g_brushSliderFill, Color(0.18f, 0.82f, 0.57f, 1.00f)) ||
            !CreateBrush(&g_brushCheckboxBack, Color(0.008f, 0.011f, 0.014f, 1.00f)) ||
            !CreateBrush(&g_brushCodeKey, Color(0.48f, 0.73f, 0.88f, 1.00f)) ||
            !CreateBrush(&g_brushCodeValue, Color(0.55f, 0.86f, 0.68f, 1.00f)) ||
            !CreateBrush(&g_brushBoxColor, Color(Menu::boxColorR, Menu::boxColorG, Menu::boxColorB, 1.0f)) ||
            !CreateBrush(&g_brushFilledColor, Color(Menu::filledColorR, Menu::filledColorG, Menu::filledColorB, 1.0f)) ||
            !CreateBrush(&g_brushHealthColor, Color(Menu::healthColorR, Menu::healthColorG, Menu::healthColorB, 1.0f)) ||
            !CreateBrush(&g_brushHealthBackgroundColor, Color(Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB, 1.0f)) ||
            !CreateBrush(&g_brushArmorColor, Color(Menu::armorColorR, Menu::armorColorG, Menu::armorColorB, 1.0f)) ||
            !CreateBrush(&g_brushArmorBack, Color(Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB, 0.92f)) ||
            !CreateBrush(&g_brushSkeleton, Color(Menu::skeletonColorR, Menu::skeletonColorG, Menu::skeletonColorB, Menu::skeletonOpacity)) ||
            !CreateBrush(&g_brushSnaplineColor, Color(Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB, 1.0f)) ||
            !CreateBrush(&g_brushSnapline, Color(Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB, 1.0f)) ||
            !CreateBrush(&g_brushOutline, Color(0.015f, 0.018f, 0.025f, 0.96f)) ||
            !CreateBrush(&g_brushAccent, Color(Menu::boxColorR, Menu::boxColorG, Menu::boxColorB, 1.00f)) ||
            !CreateBrush(&g_brushEntityFill, Color(Menu::filledColorR, Menu::filledColorG, Menu::filledColorB, std::clamp(Menu::filledOpacity, 0.05f, 1.0f))) ||
            !CreateBrush(&g_brushText, Color(0.97f, 0.98f, 1.00f, 0.98f)) ||
            !CreateBrush(&g_brushHealthBack, Color(Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB, 0.92f)) ||
            !CreateBrush(&g_brushHealthHigh, Color(0.20f, 0.88f, 0.48f, 1.00f)) ||
            !CreateBrush(&g_brushHealthMed, Color(1.00f, 0.72f, 0.20f, 1.00f)) ||
            !CreateBrush(&g_brushHealthLow, Color(0.98f, 0.20f, 0.24f, 1.00f)) ||
            !CreateBrush(&g_brushPlayerName, Color(Menu::playerNameColorR, Menu::playerNameColorG, Menu::playerNameColorB, 1.00f)) ||
            !CreateBrush(&g_brushWeaponInfo, Color(Menu::weaponIconColorR, Menu::weaponIconColorG, Menu::weaponIconColorB, 1.00f)) ||
            !CreateBrush(&g_brushDistance, Color(Menu::distanceColorR, Menu::distanceColorG, Menu::distanceColorB, 1.00f)) ||
            !CreateBrush(&g_brushHitMarker, Color(1.00f, 1.00f, 1.00f, 1.00f)) ||
            !CreateBrush(&g_brushDamagePopupOutline, Color(
                Menu::damagePopupOutlineColorR, Menu::damagePopupOutlineColorG,
                Menu::damagePopupOutlineColorB, 1.0f)))
            return false;

        if (!CreatePickerGradientResources()) { Log(L"Could not init color-picker gradients."); return false; }
        return true;
    }

    // -------------------- Drawing primitives --------------------

    void FillGradientBands(
        const D2D1_RECT_F& bounds,
        ID2D1SolidColorBrush* brush,
        float r0, float g0, float b0,
        float r1, float g1, float b1,
        float opacity,
        bool vertical,
        bool centerFade = false,
        bool reverse = false,
        unsigned segmentCount = 32U) noexcept
    {
        if (!g_d2dTarget || !brush || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return;
        const unsigned segments = std::clamp(segmentCount, 8U, 64U);
        for (unsigned i = 0; i < segments; ++i)
        {
            float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
            if (centerFade)
            {
                // Outer edges use the first color; the center eases into the second.
                t = 1.0f - std::fabs(t * 2.0f - 1.0f);
                t = t * t * (3.0f - 2.0f * t);
            }
            else if (reverse)
            {
                t = 1.0f - t;
            }
            const float r = r0 + (r1 - r0) * t;
            const float g = g0 + (g1 - g0) * t;
            const float b = b0 + (b1 - b0) * t;
            brush->SetColor(Color(r, g, b, opacity));

            D2D1_RECT_F band = bounds;
            if (vertical)
            {
                const float bandTop = bounds.top + (bounds.bottom - bounds.top) *
                    (static_cast<float>(i) / static_cast<float>(segments));
                const float bandBottom = bounds.top + (bounds.bottom - bounds.top) *
                    (static_cast<float>(i + 1U) / static_cast<float>(segments));
                band.top = bandTop;
                band.bottom = bandBottom + 0.35f; // overlap fractional edges to avoid seams
            }
            else
            {
                const float bandLeft = bounds.left + (bounds.right - bounds.left) *
                    (static_cast<float>(i) / static_cast<float>(segments));
                const float bandRight = bounds.left + (bounds.right - bounds.left) *
                    (static_cast<float>(i + 1U) / static_cast<float>(segments));
                band.left = bandLeft;
                band.right = bandRight + 0.35f;
            }
            g_d2dTarget->FillRectangle(&band, brush);
        }
        brush->SetColor(Color(r0, g0, b0, opacity));
    }

    // Animated gradient sweep used by both filled boxes and health bars.
    // The highlight travels along the long axis of the filled rectangle.
    void FillAnimatedGradientBands(
        const D2D1_RECT_F& bounds,
        ID2D1SolidColorBrush* brush,
        float r0, float g0, float b0,
        float r1, float g1, float b1,
        float opacity,
        Menu::FilledBoxStyle style,
        bool vertical = true,
        bool reverse = false) noexcept
    {
        if (!g_d2dTarget || !brush || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return;

        constexpr unsigned kBands = 36U;
        const float seconds = static_cast<float>(GetTickCount64()) * 0.001f;
        const float rawPhase = std::fmod(seconds * 0.31f, 1.0f);
        const bool styleReverse = style == Menu::FilledBoxStyle::BottomToTop;
        const bool travelReverse = reverse != styleReverse;
        const float phase = travelReverse ? 1.0f - rawPhase : rawPhase;
        const float alpha = std::clamp(opacity, 0.05f, 1.0f);

        for (unsigned i = 0; i < kBands; ++i)
        {
            const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBands);
            float styleT = t;
            if (style == Menu::FilledBoxStyle::CenterFade)
            {
                styleT = 1.0f - std::fabs(t * 2.0f - 1.0f);
                styleT = styleT * styleT * (3.0f - 2.0f * styleT);
            }
            else if (styleReverse || reverse)
            {
                styleT = 1.0f - t;
            }

            float distance = std::fabs(t - phase);
            distance = (std::min)(distance, 1.0f - distance);
            const float pulse = std::exp(-(distance * distance) / (2.0f * 0.105f * 0.105f));
            const float blend = std::clamp(0.08f + styleT * 0.30f + pulse * 0.56f, 0.05f, 0.96f);

            brush->SetColor(Color(
                r0 + (r1 - r0) * blend,
                g0 + (g1 - g0) * blend,
                b0 + (b1 - b0) * blend,
                alpha * (0.90f + pulse * 0.10f)));

            D2D1_RECT_F band = bounds;
            if (vertical)
            {
                band.top = bounds.top + (bounds.bottom - bounds.top) *
                    (static_cast<float>(i) / static_cast<float>(kBands));
                band.bottom = bounds.top + (bounds.bottom - bounds.top) *
                    (static_cast<float>(i + 1U) / static_cast<float>(kBands)) + 0.45f;
            }
            else
            {
                band.left = bounds.left + (bounds.right - bounds.left) *
                    (static_cast<float>(i) / static_cast<float>(kBands));
                band.right = bounds.left + (bounds.right - bounds.left) *
                    (static_cast<float>(i + 1U) / static_cast<float>(kBands)) + 0.45f;
            }
            g_d2dTarget->FillRectangle(&band, brush);
        }

        brush->SetColor(Color(r0, g0, b0, alpha));
    }

    void DrawEntityBox(float left, float top, float right, float bottom) noexcept
    {
        if (!g_d2dTarget || right <= left || bottom <= top) return;
        const float width = right - left;
        const float height = bottom - top;
        const float amount = std::clamp(Menu::boxCornerLength, 10.0f, 100.0f) / 100.0f;

        if (Menu::filledBox && g_brushEntityFill)
        {
            const D2D1_RECT_F fillRect{ left, top, right, bottom };
            const auto style = Menu::filledBoxStyle;
            if (style == Menu::FilledBoxStyle::Solid)
            {
                g_d2dTarget->FillRectangle(&fillRect, g_brushEntityFill);
            }
            else
            {
                FillAnimatedGradientBands(fillRect, g_brushEntityFill,
                    Menu::filledColorR, Menu::filledColorG, Menu::filledColorB,
                    Menu::filledGradientColorR, Menu::filledGradientColorG, Menu::filledGradientColorB,
                    std::clamp(Menu::filledOpacity, 0.05f, 1.0f), style);
            }
        }

        // 1 px crisp lines
        const float outlineThickness = 1.0f;
        const float accentThickness = 1.0f;

        if (amount >= 0.999f)
        {
            const D2D1_RECT_F rect{ left, top, right, bottom };
            g_d2dTarget->DrawRectangle(rect, g_brushOutline, outlineThickness);
            g_d2dTarget->DrawRectangle(rect, g_brushAccent, accentThickness);
            return;
        }

        const float horizontalLength = width * 0.5f * amount;
        const float verticalLength = height * 0.5f * amount;
        auto drawCorners = [&](ID2D1Brush* brush, float thickness)
            {
                g_d2dTarget->DrawLine({ left, top }, { left + horizontalLength, top }, brush, thickness);
                g_d2dTarget->DrawLine({ left, top }, { left, top + verticalLength }, brush, thickness);
                g_d2dTarget->DrawLine({ right - horizontalLength, top }, { right, top }, brush, thickness);
                g_d2dTarget->DrawLine({ right, top }, { right, top + verticalLength }, brush, thickness);
                g_d2dTarget->DrawLine({ left, bottom }, { left + horizontalLength, bottom }, brush, thickness);
                g_d2dTarget->DrawLine({ left, bottom - verticalLength }, { left, bottom }, brush, thickness);
                g_d2dTarget->DrawLine({ right - horizontalLength, bottom }, { right, bottom }, brush, thickness);
                g_d2dTarget->DrawLine({ right, bottom - verticalLength }, { right, bottom }, brush, thickness);
            };
        drawCorners(g_brushOutline, outlineThickness);
        drawCorners(g_brushAccent, accentThickness);
    }

    void DrawStatusBar(
        float boxLeft, float boxTop, float boxRight, float boxBottom,
        float value, float thickness, Menu::BarPosition position,
        ID2D1SolidColorBrush* fillBrush, ID2D1SolidColorBrush* backgroundBrush,
        unsigned lane = 0U,
        bool useGradient = false,
        float gradientR = 0.0f, float gradientG = 0.0f, float gradientB = 0.0f) noexcept
    {
        if (!g_d2dTarget || !fillBrush || !backgroundBrush || boxRight <= boxLeft || boxBottom <= boxTop)
            return;

        const float amount = std::clamp(value, 0.0f, 100.0f) / 100.0f;
        const float barThickness = std::clamp(thickness, 1.0f, 12.0f);
        const float laneStride = std::max(
            std::clamp(Menu::healthBarWidth, 1.0f, 12.0f),
            std::clamp(Menu::armorBarWidth, 1.0f, 12.0f)) + 2.0f;
        const float gap = 3.0f + static_cast<float>(lane) * laneStride;
        D2D1_RECT_F background{};
        D2D1_RECT_F fill{};

        switch (position)
        {
        case Menu::BarPosition::Left:
        {
            const float right = boxLeft - gap;
            const float left = right - barThickness;
            background = { left - 1.0f, boxTop - 1.0f, right + 1.0f, boxBottom + 1.0f };
            const float fillTop = boxBottom - (boxBottom - boxTop) * amount;
            fill = { left, fillTop, right, boxBottom };
            break;
        }
        case Menu::BarPosition::Right:
        {
            const float left = boxRight + gap;
            const float right = left + barThickness;
            background = { left - 1.0f, boxTop - 1.0f, right + 1.0f, boxBottom + 1.0f };
            const float fillTop = boxBottom - (boxBottom - boxTop) * amount;
            fill = { left, fillTop, right, boxBottom };
            break;
        }
        case Menu::BarPosition::Top:
        {
            const float bottom = boxTop - gap;
            const float top = bottom - barThickness;
            background = { boxLeft - 1.0f, top - 1.0f, boxRight + 1.0f, bottom + 1.0f };
            const float fillRight = boxLeft + (boxRight - boxLeft) * amount;
            fill = { boxLeft, top, fillRight, bottom };
            break;
        }
        case Menu::BarPosition::Bottom:
        {
            const float top = boxBottom + gap;
            const float bottom = top + barThickness;
            background = { boxLeft - 1.0f, top - 1.0f, boxRight + 1.0f, bottom + 1.0f };
            const float fillRight = boxLeft + (boxRight - boxLeft) * amount;
            fill = { boxLeft, top, fillRight, bottom };
            break;
        }
        }

        g_d2dTarget->FillRectangle(&background, backgroundBrush);
        if (amount > 0.0f)
        {
            if (useGradient)
            {
                const bool verticalGradient = position == Menu::BarPosition::Left || position == Menu::BarPosition::Right;
                // Vertical bars fill bottom-up; horizontal bars fill left-to-right.
                // Keep the animated sweep aligned with the direction in which the bar fills.
                FillAnimatedGradientBands(fill, fillBrush,
                    Menu::healthColorR, Menu::healthColorG, Menu::healthColorB,
                    gradientR, gradientG, gradientB, 1.0f,
                    Menu::FilledBoxStyle::TopToBottom, verticalGradient, verticalGradient);
            }
            else
            {
                g_d2dTarget->FillRectangle(&fill, fillBrush);
            }
        }
    }

    void DrawHealthBar(const EntityDrawData& entity, unsigned lane = 0U) noexcept
    {
        if (!g_d2dTarget)
            return;

        // Keep the bar fill and its numeric label in sync. displayHealth is
        // eased toward the live health value by BuildEntityFrame(), so the
        // number visibly ticks down along with the animated fill.
        const float hp = std::clamp(entity.displayHealth, 0.0f, 100.0f);
        DrawStatusBar(entity.left, entity.top, entity.right, entity.bottom,
            hp, Menu::healthBarWidth, Menu::healthPosition,
            g_brushHealthHigh, g_brushHealthBack, lane,
            Menu::healthGradient,
            Menu::healthGradientColorR, Menu::healthGradientColorG, Menu::healthGradientColorB);

        // Keep the health bar visible at full health, but only show its
        // numeric text after health drops below 100.
        if (hp >= 100.0f)
            return;

        if (!g_smallTextFormat || !g_brushText || !g_brushOutline)
            return;

        const float boxHeight = entity.bottom - entity.top;
        if (boxHeight < 22.0f)
            return; // avoid clutter on very distant/small projections

        wchar_t label[8]{};
        swprintf_s(label, L"%d", static_cast<int>(std::lround(hp)));

        constexpr float labelWidth = 26.0f;
        constexpr float labelHeight = 13.0f;
        constexpr float barGap = 3.0f;
        const float thickness = std::clamp(Menu::healthBarWidth, 1.0f, 12.0f);
        const float amount = hp / 100.0f;
        float labelX = 0.0f;
        float labelY = 0.0f;

        // Position the value at the moving end of the filled segment. For
        // vertical bars it follows the fill's upper edge; for horizontal bars
        // it follows the fill's right edge.
        switch (Menu::healthPosition)
        {
        case Menu::BarPosition::Left:
        {
            const float barCenterX = entity.left - barGap - thickness * 0.5f;
            const float fillEdgeY = entity.bottom - boxHeight * amount;
            labelX = barCenterX - labelWidth * 0.5f;
            labelY = fillEdgeY - labelHeight - 2.0f;
            break;
        }
        case Menu::BarPosition::Right:
        {
            const float barCenterX = entity.right + barGap + thickness * 0.5f;
            const float fillEdgeY = entity.bottom - boxHeight * amount;
            labelX = barCenterX - labelWidth * 0.5f;
            labelY = fillEdgeY - labelHeight - 2.0f;
            break;
        }
        case Menu::BarPosition::Top:
        {
            const float barTop = entity.top - barGap - thickness;
            const float fillEdgeX = entity.left + (entity.right - entity.left) * amount;
            labelX = fillEdgeX - labelWidth * 0.5f;
            labelY = barTop - labelHeight - 1.0f;
            break;
        }
        case Menu::BarPosition::Bottom:
        {
            const float barTop = entity.bottom + barGap;
            const float fillEdgeX = entity.left + (entity.right - entity.left) * amount;
            labelX = fillEdgeX - labelWidth * 0.5f;
            labelY = barTop + thickness + 1.0f;
            break;
        }
        }

        // Keep the label inside the monitor when the entity is near an edge.
        labelX = std::clamp(labelX, 0.0f,
            std::max(0.0f, static_cast<float>(g_width) - labelWidth));
        labelY = std::clamp(labelY, 0.0f,
            std::max(0.0f, static_cast<float>(g_height) - labelHeight));

        const D2D1_RECT_F shadowRect{
            labelX + 1.0f, labelY + 1.0f,
            labelX + labelWidth + 1.0f, labelY + labelHeight + 1.0f };
        const D2D1_RECT_F textRect{
            labelX, labelY,
            labelX + labelWidth, labelY + labelHeight };
        g_d2dTarget->DrawText(label, static_cast<UINT32>(wcslen(label)),
            g_smallTextFormat, shadowRect, g_brushOutline,
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        g_d2dTarget->DrawText(label, static_cast<UINT32>(wcslen(label)),
            g_smallTextFormat, textRect, g_brushText,
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void DrawArmorBar(const EntityDrawData& entity, unsigned lane = 0U) noexcept
    {
        if (entity.armor <= 0) return;
        DrawStatusBar(entity.left, entity.top, entity.right, entity.bottom,
            entity.displayArmor, Menu::armorBarWidth, Menu::armorPosition,
            g_brushArmorColor, g_brushArmorBack, lane);
    }

    void DrawSnapline(const EntityDrawData& entity) noexcept
    {
        if (!g_d2dTarget || !g_brushSnapline || !Menu::snapline || !entity.valid) return;
        const float screenCenterX = static_cast<float>(g_width) * 0.5f;
        const float screenCenterY = static_cast<float>(g_height) * 0.5f;
        const float entityCenterX = (entity.left + entity.right) * 0.5f;
        const float entityCenterY = (entity.top + entity.bottom) * 0.5f;

        D2D1_POINT_2F start{ screenCenterX, 0.0f };
        D2D1_POINT_2F end{ entityCenterX, entity.top };
        switch (Menu::snaplineAnchor)
        {
        case Menu::SnaplineAnchor::Top:    start = { screenCenterX, 0.0f }; end = { entityCenterX, entity.top }; break;
        case Menu::SnaplineAnchor::Center: start = { screenCenterX, screenCenterY }; end = { entityCenterX, entityCenterY }; break;
        case Menu::SnaplineAnchor::Bottom: start = { screenCenterX, static_cast<float>(g_height) }; end = { entityCenterX, entity.bottom }; break;
        case Menu::SnaplineAnchor::Left:   start = { 0.0f, screenCenterY }; end = { entity.left, entityCenterY }; break;
        case Menu::SnaplineAnchor::Right:  start = { static_cast<float>(g_width), screenCenterY }; end = { entity.right, entityCenterY }; break;
        }
        g_d2dTarget->DrawLine(start, end, g_brushSnapline, 1.0f);
    }

    // Electric skeleton pulse. A dim, stable skeleton remains visible while a
    // bright current starts at the torso and travels outward along every branch.
    void DrawSkeleton(const EntityDrawData& entity) noexcept
    {
        if (!g_d2dTarget || !g_brushSkeleton || !Menu::skeleton ||
            !entity.valid || !entity.skeletonValid)
            return;

        using Link = std::array<size_t, 2>;
        static constexpr std::array<Link, 20> terrorist{ {
            { { 1, 2 } },{ { 2, 3 } },{ { 3, 4 } },{ { 4, 12 } },
            { { 12, 5 } },{ { 5, 6 } },{ { 6, 7 } },
            { { 12, 13 } },{ { 13, 14 } },{ { 14, 71 } },
            { { 12, 9 } },{ { 9, 50 } },{ { 50, 10 } },{ { 10, 11 } },
            { { 1, 17 } },{ { 17, 18 } },{ { 18, 19 } },
            { { 1, 20 } },{ { 20, 21 } },{ { 21, 22 } }
        } };
        static constexpr std::array<Link, 20> counterTerrorist{ {
            { { 1, 2 } },{ { 2, 3 } },{ { 3, 4 } },{ { 4, 12 } },
            { { 12, 5 } },{ { 5, 6 } },{ { 6, 7 } },
            { { 12, 13 } },{ { 13, 14 } },{ { 14, 73 } },
            { { 12, 9 } },{ { 9, 50 } },{ { 50, 10 } },{ { 10, 11 } },
            { { 1, 17 } },{ { 17, 18 } },{ { 18, 19 } },
            { { 1, 20 } },{ { 20, 21 } },{ { 21, 22 } }
        } };

        const float boxWidth = entity.right - entity.left;
        const float boxHeight = entity.bottom - entity.top;
        if (boxWidth <= 1.0f || boxHeight <= 1.0f) return;

        const float centerX = (entity.left + entity.right) * 0.5f;
        const float allowance = (std::max)(boxWidth, boxHeight * 0.68f);
        const float minX = centerX - allowance;
        const float maxX = centerX + allowance;
        const float minY = entity.top - boxHeight * 0.18f;
        const float maxY = entity.bottom + boxHeight * 0.12f;
        const float maxSegment = (std::max)(18.0f, boxHeight * 0.62f);
        const float maxSegmentSq = maxSegment * maxSegment;

        auto sane = [&](const ScreenBone& point) noexcept {
            return point.visible && point.x >= minX && point.x <= maxX &&
                point.y >= minY && point.y <= maxY;
            };

        auto drawLinks = [&](const auto& links) noexcept {
            // Resolve graph distance from bone 12 (torso centre). The topology is
            // a tree, so repeated relaxation is small, deterministic and allocation-free.
            std::array<int, 74> depth{};
            depth.fill(1000);
            depth[12] = 0;
            for (size_t pass = 0; pass < 24; ++pass)
            {
                bool changed = false;
                for (const auto& link : links)
                {
                    const size_t a = link[0], b = link[1];
                    if (depth[a] < 1000 && depth[b] > depth[a] + 1) {
                        depth[b] = depth[a] + 1;
                        changed = true;
                    }
                    if (depth[b] < 1000 && depth[a] > depth[b] + 1) {
                        depth[a] = depth[b] + 1;
                        changed = true;
                    }
                }
                if (!changed) break;
            }

            const ULONGLONG now = GetTickCount64();
            constexpr ULONGLONG kCycleMs = 1540ULL;
            constexpr float kEdgeDelayMs = 112.0f;
            constexpr float kTravelMs = 132.0f;
            constexpr float kTailMs = 105.0f;
            const float entityPhase = static_cast<float>(entity.pawnKey % 260ULL);
            const ULONGLONG phaseOffset = static_cast<ULONGLONG>(entityPhase);
            const float cycleAge = static_cast<float>((now + phaseOffset) % kCycleMs);

            const float baseR = std::clamp(Menu::skeletonColorR, 0.0f, 1.0f);
            const float baseG = std::clamp(Menu::skeletonColorG, 0.0f, 1.0f);
            const float baseB = std::clamp(Menu::skeletonColorB, 0.0f, 1.0f);
            const float userAlpha = std::clamp(Menu::skeletonOpacity, 0.05f, 1.0f);
            const float electricR = baseR * 0.35f + 0.65f;
            const float electricG = baseG * 0.35f + 0.65f;
            const float electricB = baseB * 0.35f + 0.65f;

            for (const auto& link : links)
            {
                size_t fromIndex = link[0];
                size_t toIndex = link[1];
                if (depth[fromIndex] > depth[toIndex])
                    std::swap(fromIndex, toIndex);

                const ScreenBone& from = entity.skeletonScreen[fromIndex];
                const ScreenBone& to = entity.skeletonScreen[toIndex];
                if (!sane(from) || !sane(to)) continue;
                if (Vec3DistanceSquared(entity.skeletonWorld[fromIndex],
                    entity.skeletonWorld[toIndex]) > 55.0f * 55.0f)
                    continue;

                const float dx = to.x - from.x;
                const float dy = to.y - from.y;
                const float lengthSq = dx * dx + dy * dy;
                if (lengthSq > maxSegmentSq || lengthSq < 1.0f) continue;

                // Quiet underlay keeps the full pose readable between electric pulses.
                g_brushSkeleton->SetColor(Color(baseR, baseG, baseB, userAlpha * 0.34f));
                g_d2dTarget->DrawLine({ from.x, from.y }, { to.x, to.y }, g_brushSkeleton, 1.05f);

                const float edgeStart = static_cast<float>(depth[fromIndex]) * kEdgeDelayMs;
                const float edgeAge = cycleAge - edgeStart;
                if (edgeAge < 0.0f || edgeAge > kTravelMs + kTailMs) continue;

                const float travel = std::clamp(edgeAge / kTravelMs, 0.0f, 1.0f);
                const float trailStart = (std::max)(0.0f, travel - 0.43f);
                float pulseAlpha = 1.0f;
                if (edgeAge > kTravelMs)
                {
                    const float fade = std::clamp((edgeAge - kTravelMs) / kTailMs, 0.0f, 1.0f);
                    pulseAlpha = 1.0f - fade * fade * (3.0f - 2.0f * fade);
                }
                if (pulseAlpha <= 0.01f || travel <= 0.001f) continue;

                auto pointAt = [&](float t) noexcept -> D2D1_POINT_2F {
                    return { from.x + dx * t, from.y + dy * t };
                    };
                const D2D1_POINT_2F p0 = pointAt(trailStart);
                const D2D1_POINT_2F p2 = pointAt(travel);
                const float midT = (trailStart + travel) * 0.5f;
                const float wave = std::sin(static_cast<float>(now) * 0.043f +
                    static_cast<float>(fromIndex * 7U + toIndex * 3U) + entityPhase * 0.03f);
                const float invLength = 1.0f / std::sqrt(lengthSq);
                const float jitter = wave * (1.4f + 1.2f * (1.0f - travel));
                const D2D1_POINT_2F p1{
                    from.x + dx * midT - dy * invLength * jitter,
                    from.y + dy * midT + dx * invLength * jitter
                };

                // Three passes produce a soft electrical halo, saturated body and hot core.
                const float glowA = userAlpha * pulseAlpha;
                g_brushSkeleton->SetColor(Color(baseR, baseG, baseB, glowA * 0.22f));
                g_d2dTarget->DrawLine(p0, p1, g_brushSkeleton, 5.2f);
                g_d2dTarget->DrawLine(p1, p2, g_brushSkeleton, 5.2f);
                g_brushSkeleton->SetColor(Color(electricR, electricG, electricB, glowA * 0.68f));
                g_d2dTarget->DrawLine(p0, p1, g_brushSkeleton, 2.25f);
                g_d2dTarget->DrawLine(p1, p2, g_brushSkeleton, 2.25f);
                g_brushSkeleton->SetColor(Color(0.96f, 0.99f, 1.0f, glowA * 0.92f));
                g_d2dTarget->DrawLine(p0, p1, g_brushSkeleton, 0.95f);
                g_d2dTarget->DrawLine(p1, p2, g_brushSkeleton, 0.95f);

                // Small fork at the moving tip gives the pulse an electric, branching edge.
                const float forkLength = 3.2f + 2.2f * (0.5f + 0.5f * wave);
                const D2D1_POINT_2F forkEnd{
                    p2.x - dy * invLength * forkLength,
                    p2.y + dx * invLength * forkLength
                };
                g_brushSkeleton->SetColor(Color(baseR, baseG, baseB, glowA * 0.28f));
                g_d2dTarget->DrawLine(p2, forkEnd, g_brushSkeleton, 3.2f);
                g_brushSkeleton->SetColor(Color(0.96f, 0.99f, 1.0f, glowA * 0.82f));
                g_d2dTarget->DrawLine(p2, forkEnd, g_brushSkeleton, 1.0f);
            }

            // A brief core flash makes the outward origin visibly start at the torso.
            const ScreenBone& core = entity.skeletonScreen[12];
            if (sane(core) && cycleAge < 112.0f)
            {
                const float t = std::clamp(cycleAge / 112.0f, 0.0f, 1.0f);
                const float flash = 1.0f - t;
                g_brushSkeleton->SetColor(Color(baseR, baseG, baseB, userAlpha * flash * 0.42f));
                g_d2dTarget->DrawEllipse(D2D1::Ellipse({ core.x, core.y }, 5.0f + t * 5.0f, 5.0f + t * 5.0f),
                    g_brushSkeleton, 3.4f);
                g_brushSkeleton->SetColor(Color(0.97f, 0.99f, 1.0f, userAlpha * flash * 0.92f));
                g_d2dTarget->DrawEllipse(D2D1::Ellipse({ core.x, core.y }, 2.0f + t * 3.5f, 2.0f + t * 3.5f),
                    g_brushSkeleton, 1.2f);
            }
            };

        if (entity.team == 2) drawLinks(terrorist);
        else drawLinks(counterTerrorist);

        // Restore the configured brush colour for all other overlay drawing.
        g_brushSkeleton->SetColor(Color(
            Menu::skeletonColorR, Menu::skeletonColorG,
            Menu::skeletonColorB, Menu::skeletonOpacity));
    }

    // ===================== CACHED ENTITY FRAME =====================
    void BuildEntityFrame(EntityFrame& frame) noexcept
    {
        frame = {};
        if (!Menu::box && !Menu::health && !Menu::armor && !Menu::snapline && !Menu::skeleton &&
            !Menu::playerName && !Menu::weaponIcon && !Menu::distance && !Menu::hitMarker && !Menu::damagePopup &&
            !AimConfig::enabled && !AimConfig::rcsEnabled) return;
        static const bool schemaAttempted = LoadPlayerSchemaOffsets();
        (void)schemaAttempted;

        HMODULE clientModule = GetModuleHandleW(L"client.dll");
        if (!clientModule)
        {
            g_clientBase.store(0, std::memory_order_relaxed);
            g_pawnCacheCount = 0;
            g_localPlayerPawn = 0;
            ResetRcsSampleState();
            return;
        }

        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(clientModule);
        uintptr_t clientBase = g_clientBase.load(std::memory_order_relaxed);
        if (clientBase != moduleBase)
        {
            clientBase = moduleBase;
            g_clientBase.store(clientBase, std::memory_order_relaxed);
            g_pawnCacheCount = 0;
            g_entityList = 0;
            g_localPlayerPawn = 0;
            ResetRcsSampleState();
        }

        const ULONGLONG now = GetTickCount64();
        // Keep the original ESP cache cadence. Aimbot reads the same cached
        // pawn addresses but does not force the ESP scanner to rebuild every frame.
        const bool needCacheRefresh = (g_pawnCacheCount == 0) ||
            (now - g_lastPawnCacheUpdate >= kPawnCacheIntervalMs);

        if (needCacheRefresh)
        {
            g_pawnCache = {};
            g_pawnCacheCount = 0;
            g_lastPawnCacheUpdate = now;

            Mat4 viewMatrix{};
            uintptr_t entityList = 0;
            if (!SafeRead(clientBase + Offsets::dwViewMatrix, viewMatrix) ||
                !SafeRead(clientBase + Offsets::dwEntityList, entityList) || !entityList)
                return;
            g_entityList = entityList;

            uintptr_t localController = 0;
            SafeRead(clientBase + g_dwLocalPlayerControllerOffset, localController);
            uint32_t localPawnHandle = 0;
            if (localController) SafeRead(localController + PlayerPawnHandleOffset(), localPawnHandle);

            EntityListReadCache listCache{};
            const uintptr_t localPawn = ResolvePawn(entityList, localPawnHandle, listCache);
            const uintptr_t controllerEntry = GetCachedEntityListEntry(entityList, 0U, listCache);
            if (!controllerEntry) return;

            // Cache controller addresses, not pawn addresses. A controller normally
            // survives death/respawn while its pawn handle changes. The handle is
            // resolved again in the per-frame loop below.
            for (uint32_t i = 0; i < 64; ++i)
            {
                uintptr_t controller = 0;
                if (!SafeRead(controllerEntry + 112ULL * i, controller) || !controller)
                    continue;
                if (controller == localController)
                    continue;

                bool alreadyCached = false;
                for (size_t j = 0; j < g_pawnCacheCount; ++j)
                {
                    if (g_pawnCache[j].controller == controller)
                    {
                        alreadyCached = true;
                        break;
                    }
                }
                if (alreadyCached || g_pawnCacheCount >= g_pawnCache.size())
                    continue;

                CachedPawn& entry = g_pawnCache[g_pawnCacheCount++];
                entry = {};
                entry.controller = controller;
            }
        }

        // Refresh the local origin once per frame for the distance labels.
        uintptr_t entityList = g_entityList;
        if (!entityList && SafeRead(clientBase + Offsets::dwEntityList, entityList))
            g_entityList = entityList;
        if (!entityList)
        {
            g_localPlayerPawn = 0;
            return;
        }

        EntityListReadCache frameListCache{};
        uintptr_t localController = 0;
        SafeRead(clientBase + g_dwLocalPlayerControllerOffset, localController);
        uint32_t localPawnHandle = 0;
        if (localController)
            SafeRead(localController + PlayerPawnHandleOffset(), localPawnHandle);
        const uintptr_t localPawn = ResolvePawn(entityList, localPawnHandle, frameListCache);
        g_localPlayerPawn = localPawn;
        uint8_t localTeam = 0;
        if (localPawn) SafeRead(localPawn + Offsets::m_iTeamNum, localTeam);
        Vec3 localOrigin{};
        const bool localOriginValid = localPawn &&
            SafeRead(localPawn + Offsets::m_vOldOrigin, localOrigin) &&
            std::isfinite(localOrigin.x) && std::isfinite(localOrigin.y) &&
            std::isfinite(localOrigin.z);

        // Fast path – only read health + origin from cached addresses
        const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height) };
        Mat4 viewMatrix{};
        if (!SafeRead(clientBase + Offsets::dwViewMatrix, viewMatrix)) return;

        size_t outIndex = 0;
        for (size_t i = 0; i < g_pawnCacheCount && outIndex < frame.size(); ++i)
        {
            CachedPawn& cachedPawn = g_pawnCache[i];
            const uintptr_t controller = cachedPawn.controller;
            if (!controller) continue;

            // Resolve the current pawn from the cached controller every frame.
            // This picks up the new pawn immediately after a respawn without
            // rebuilding the controller list or waiting for the 5-second scan.
            uint32_t pawnHandle = 0;
            if (!SafeRead(controller + PlayerPawnHandleOffset(), pawnHandle) ||
                !pawnHandle || pawnHandle == 0xFFFFFFFFu)
            {
                cachedPawn.pawn = 0;
                cachedPawn.boneArray = 0;
                cachedPawn.bonesValid = false;
                continue;
            }

            const uintptr_t pawn = ResolvePawn(entityList, pawnHandle, frameListCache);
            if (!pawn || pawn == localPawn)
            {
                cachedPawn.pawn = 0;
                cachedPawn.boneArray = 0;
                cachedPawn.bonesValid = false;
                continue;
            }

            // Invalidate pawn-specific data when this controller receives a new
            // pawn after death/respawn; keep controller/name association intact.
            if (cachedPawn.pawn != pawn)
            {
                cachedPawn.pawn = pawn;
                cachedPawn.weaponId = 0;
                cachedPawn.lastWeaponRefresh = 0;
                cachedPawn.boneArray = 0;
                cachedPawn.boneArrayLastResolve = 0;
                cachedPawn.boneSampleTime = 0;
                cachedPawn.bonesValid = false;
            }

            int healthValue = 0;
            // A dead player must disappear from the current ESP frame immediately.
            // Do not keep drawing the corpse using its last valid cached pose.
            if (!SafeRead(pawn + Offsets::m_iHealth, healthValue) || healthValue <= 0 || healthValue > 100)
            {
                cachedPawn.boneArray = 0;
                cachedPawn.boneArrayLastResolve = 0;
                cachedPawn.bonesValid = false;
                cachedPawn.boneSampleTime = 0;
                continue;
            }
            int armorValue = 0;
            if (Menu::armor)
                SafeRead(pawn + static_cast<uintptr_t>(Offsets::m_ArmorValue), armorValue);
            armorValue = std::clamp(armorValue, 0, 100);

            Vec3 origin{};
            if (!SafeRead(pawn + Offsets::m_vOldOrigin, origin) ||
                !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
                continue;

            uint8_t teamNumber = 0;
            SafeRead(pawn + Offsets::m_iTeamNum, teamNumber);
            // Default: render enemies only. Deathmatch mode includes both teams.
            if (!AimConfig::deathmatchMode &&
                (localTeam == 2 || localTeam == 3) && teamNumber == localTeam)
                continue;

            if (cachedPawn.controller && g_playerSchema.playerName &&
                (cachedPawn.lastNameRefresh == 0 || now - cachedPawn.lastNameRefresh >= 500ULL))
            {
                std::array<char, 32> refreshedName{};
                if (SafeReadBytes(cachedPawn.controller + g_playerSchema.playerName,
                    refreshedName.data(), refreshedName.size() - 1))
                {
                    for (auto& ch : refreshedName)
                    {
                        if (!ch) break;
                        const unsigned char c = static_cast<unsigned char>(ch);
                        if (c < 32 || c > 126) ch = '?';
                    }
                    cachedPawn.playerName = refreshedName;
                }
                cachedPawn.lastNameRefresh = now;
            }

            const bool weaponSchemaReady = g_playerSchema.weaponServices &&
                g_playerSchema.activeWeapon && g_playerSchema.attributeManager &&
                g_playerSchema.item && g_playerSchema.itemDefinitionIndex;
            if (weaponSchemaReady &&
                (cachedPawn.lastWeaponRefresh == 0 || now - cachedPawn.lastWeaponRefresh >= 180ULL))
            {
                uint16_t itemDefinition = 0;
                uintptr_t weaponServices = 0;
                uint32_t weaponHandle = 0;
                if (SafeRead(pawn + g_playerSchema.weaponServices, weaponServices) && weaponServices &&
                    SafeRead(weaponServices + g_playerSchema.activeWeapon, weaponHandle) &&
                    weaponHandle && weaponHandle != 0xFFFFFFFFu)
                {
                    const uintptr_t weapon = ResolvePawn(entityList, weaponHandle, frameListCache);
                    if (weapon)
                    {
                        const uintptr_t itemView = weapon + g_playerSchema.attributeManager + g_playerSchema.item;
                        const uintptr_t definitionAddress = itemView + g_playerSchema.itemDefinitionIndex;
                        SafeRead(definitionAddress, itemDefinition);
                        if (itemDefinition == 0 || itemDefinition > 6000)
                        {
                            uint16_t alternate = 0;
                            if (SafeRead(itemView + 0x1BA, alternate) && alternate > 0 && alternate < 6000)
                                itemDefinition = alternate;
                            else if (SafeRead(itemView + 0x38, alternate) && alternate > 0 && alternate < 6000)
                                itemDefinition = alternate;
                        }
                    }
                }
                cachedPawn.weaponId = itemDefinition;
                cachedPawn.lastWeaponRefresh = now;
            }

            const Vec3 head{ origin.x, origin.y, origin.z + 75.0f };
            float footX = 0.0f, footY = 0.0f, footDepth = 0.0f;
            float headX = 0.0f, headY = 0.0f, headDepth = 0.0f;
            if (!WorldToScreen(viewMatrix, origin, viewport, footX, footY, footDepth) ||
                !WorldToScreen(viewMatrix, head, viewport, headX, headY, headDepth))
                continue;

            const float shallowDepth = std::min(footDepth, headDepth);
            const float deepDepth = std::max(footDepth, headDepth);
            if (shallowDepth <= 0.05f || deepDepth / shallowDepth > 2.5f) continue;

            const float centerX = (footX + headX) * 0.5f;
            const float topY = std::min(footY, headY);
            const float bottomY = std::max(footY, headY);
            const float boxHeight = bottomY - topY;
            if (!std::isfinite(centerX) || !std::isfinite(boxHeight) || boxHeight < 9.0f || boxHeight > viewport.h * 0.70f)
                continue;

            // ★ WIDER BOXES
            const float boxWidth = boxHeight * 0.58f;
            if (boxWidth < 2.0f || boxWidth > viewport.w * 0.22f) continue;

            const float left = centerX - boxWidth * 0.5f;
            const float right = centerX + boxWidth * 0.5f;
            if (right <= 0.0f || left >= viewport.w || bottomY <= 0.0f || topY >= viewport.h) continue;

            float smoothed = static_cast<float>(healthValue);
            // Smooth toward real HP using the previous frame's nearby box.
            if (g_hasLastEntityFrame)
            {
                for (const auto& prev : g_lastEntityFrame)
                {
                    if (!prev.valid) continue;
                    if (std::fabs(prev.left - left) < 40.0f && std::fabs(prev.top - topY) < 40.0f)
                    {
                        smoothed = prev.displayHealth + (static_cast<float>(healthValue) - prev.displayHealth) * 0.12f;
                        break;
                    }
                }
            }
            EntityDrawData drawData{};
            drawData.valid = true;
            drawData.left = left;
            drawData.top = topY;
            drawData.right = right;
            drawData.bottom = bottomY;
            drawData.health = healthValue;
            drawData.displayHealth = smoothed;
            drawData.armor = armorValue;
            drawData.displayArmor = static_cast<float>(armorValue);
            drawData.team = teamNumber;
            drawData.worldOrigin = origin;
            drawData.pawnKey = pawn;
            std::memcpy(drawData.playerName, cachedPawn.playerName.data(),
                sizeof(drawData.playerName));
            drawData.weaponId = cachedPawn.weaponId;
            if (localOriginValid)
            {
                const float distanceSq = Vec3DistanceSquared(origin, localOrigin);
                drawData.distanceMeters = (std::max)(0, static_cast<int>(
                    std::lround(std::sqrt(distanceSq) / 39.37f)));
                drawData.distanceValid = std::isfinite(distanceSq);
            }

            // Sample the exact same cached skeleton for ESP drawing and aim
            // selection. Use the project's requested head target, bone 6. Store it in
            // world space even if its screen projection is outside the viewport.
            if (Menu::skeleton || AimConfig::enabled)
            {
                std::array<Vec3, kSkeletonBoneCount> boneWorld{};
                if (UpdateCachedSkeleton(cachedPawn, pawn, origin, now, boneWorld))
                {
                    drawData.skeletonWorld[6] = boneWorld[6];
                    drawData.headBoneValid =
                        std::isfinite(boneWorld[6].x) && std::isfinite(boneWorld[6].y) &&
                        std::isfinite(boneWorld[6].z) &&
                        Vec3DistanceSquared(boneWorld[6], origin) <= 155.0f * 155.0f;

                    size_t visibleBoneCount = 0;
                    for (const size_t boneIndex : kSkeletonUsedBones)
                    {
                        drawData.skeletonWorld[boneIndex] = boneWorld[boneIndex];
                        float boneX = 0.0f, boneY = 0.0f, boneDepth = 0.0f;
                        if (!WorldToScreen(viewMatrix, boneWorld[boneIndex], viewport,
                            boneX, boneY, boneDepth))
                            continue;
                        drawData.skeletonScreen[boneIndex] = { boneX, boneY, true };
                        ++visibleBoneCount;
                    }
                    drawData.skeletonValid = visibleBoneCount >= 6;
                }
            }
            frame[outIndex++] = drawData;
        }
    }

    Vec3 CalcAimAngle(const Vec3& from, const Vec3& to) noexcept
    {
        constexpr float kPi = 3.14159265358979323846f;
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float yaw = std::atan2(dy, dx) * 180.0f / kPi;
        const float pitch = -std::atan2(dz, std::hypot(dx, dy)) * 180.0f / kPi;
        return { pitch, yaw, 0.0f };
    }

    float NormalizeAimAngle(float angle) noexcept
    {
        while (angle > 180.0f) angle -= 360.0f;
        while (angle < -180.0f) angle += 360.0f;
        return angle;
    }

    bool IsSaneAimPointer(uintptr_t pointer) noexcept
    {
#ifdef _WIN64
        return pointer >= 0x10000ULL && pointer <= 0x00007FFFFFFFFFFFULL;
#else
        return pointer >= 0x10000UL;
#endif
    }

    // Keep aim and recoil movement queues independent. A large target swap can
     // therefore never consume, overwrite, or delay the local recoil correction.
    enum class MouseMoveChannel : size_t { Aim = 0, Recoil = 1 };
    struct MouseMoveRemainder { float x = 0.0f; float y = 0.0f; };
    std::array<MouseMoveRemainder, 2> g_mouseMoveRemainders{};

    struct RcsPunchState {
        bool valid = false;
        int lastShotsFired = 0;
        uintptr_t pawn = 0;
        uintptr_t services = 0;
        AimVec2 lastPunch{ 0.0f, 0.0f };
        float smoothX = 0.0f;
        float smoothY = 0.0f;
    };

    static RcsPunchState g_rcsState{};

    void ResetRcsState() noexcept
    {
        g_rcsState.valid = false;
        g_rcsState.lastShotsFired = 0;
        g_rcsState.pawn = 0;
        g_rcsState.services = 0;
        g_rcsState.lastPunch = { 0.0f, 0.0f };
        g_rcsState.smoothX = 0.0f;
        g_rcsState.smoothY = 0.0f;
    }

    void ResetMouseMoveRemainder(MouseMoveChannel channel) noexcept
    {
        g_mouseMoveRemainders[static_cast<size_t>(channel)] = {};
    }

    void ResetMouseMoveRemainder() noexcept
    {
        g_mouseMoveRemainders = {};
    }

    void SendRelativeMouseMove(float dx, float dy, MouseMoveChannel channel) noexcept
    {
        if (!std::isfinite(dx) || !std::isfinite(dy)) return;
        auto& remainder = g_mouseMoveRemainders[static_cast<size_t>(channel)];

        remainder.x = std::clamp(remainder.x + dx, -240.0f, 240.0f);
        remainder.y = std::clamp(remainder.y + dy, -240.0f, 240.0f);

        const LONG moveX = static_cast<LONG>(std::clamp(std::lround(remainder.x), -40L, 40L));
        const LONG moveY = static_cast<LONG>(std::clamp(std::lround(remainder.y), -40L, 40L));
        if (moveX == 0 && moveY == 0) return;

        remainder.x -= static_cast<float>(moveX);
        remainder.y -= static_cast<float>(moveY);

        mouse_event(MOUSEEVENTF_MOVE, static_cast<DWORD>(moveX), static_cast<DWORD>(moveY), 0, 0);
    }

    // Ring buffer element structure for aim punch cache
    struct AimPunchElement {
        int tickBase = 0;
        int unk = 0; // padding / internal
        float pitch = 0.0f; // [0] vertical (X)
        float yaw = 0.0f;   // [1] horizontal (Y)
        float roll = 0.0f;  // [2]
    };

    struct RcsLerpState
    {
        int lastShotsFired = 0;
        float lastPitch = 0.0f;
        float lastYaw = 0.0f;
        float pendingMouseX = 0.0f;
        float pendingMouseY = 0.0f;
    };

    namespace
    {
        RcsLerpState g_rcsLerpState{};

        inline float ComputeRcsLerpAlpha(float speed, float dt) noexcept
        {
            const float clampedDt = std::clamp(dt, 0.0f, 0.1f);
            const float clampedSpeed = std::clamp(speed, 0.5f, 60.0f);
            return 1.0f - std::exp(-clampedSpeed * clampedDt);
        }

        inline void ResetRcsLerpState() noexcept
        {
            g_rcsLerpState.lastShotsFired = 0;
            g_rcsLerpState.lastPitch = 0.0f;
            g_rcsLerpState.lastYaw = 0.0f;
            g_rcsLerpState.pendingMouseX = 0.0f;
            g_rcsLerpState.pendingMouseY = 0.0f;
        }
    }

    void ApplyAimRCS(uintptr_t pawn, float sensitivity, float dt) noexcept
    {
        if (!AimConfig::rcsEnabled || !IsSaneAimPointer(pawn) ||
            !std::isfinite(sensitivity) || sensitivity <= 0.001f || sensitivity >= 20.0f)
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        // 1. Read m_pAimPunchServices pointer
        uintptr_t punchServices = 0;
        if (!SafeRead(pawn + AimbotOffsets::m_pAimPunchServices, punchServices) ||
            !IsSaneAimPointer(punchServices))
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        // 2. Read the punch cache array — last element (count - 1)
        int count = 0;
        if (!SafeRead(punchServices + 0x10, count) || count <= 0 || count > 0xFFFF)
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        uintptr_t dataPtr = 0;
        if (!SafeRead(punchServices + 0x18, dataPtr) || !IsSaneAimPointer(dataPtr))
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        const uintptr_t lastEntry = dataPtr + static_cast<uintptr_t>(count - 1) * sizeof(AimPunchElement);
        AimPunchElement lastPunch{};
        if (!SafeRead(lastEntry, lastPunch) ||
            !std::isfinite(lastPunch.pitch) || !std::isfinite(lastPunch.yaw))
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        // 3. Read shots fired to detect new shots
        int shotsFired = 0;
        if (!SafeRead(pawn + AimbotOffsets::m_iShotsFired, shotsFired) || shotsFired <= 0)
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
            return;
        }

        // 4. Accumulate new punch delta whenever shotsFired increments
        if (shotsFired != g_rcsLerpState.lastShotsFired)
        {
            float deltaPitch = lastPunch.pitch - g_rcsLerpState.lastPitch;
            float deltaYaw = lastPunch.yaw - g_rcsLerpState.lastYaw;

            if (g_rcsLerpState.lastShotsFired == 0)
            {
                deltaPitch = lastPunch.pitch;
                deltaYaw = lastPunch.yaw;
            }

            const float degPerCount = 0.022f * sensitivity;
            const float shotMouseX = -(deltaYaw * 2.0f) / degPerCount;
            const float shotMouseY = -(deltaPitch * 2.0f) / degPerCount;

            if (std::isfinite(shotMouseX) && std::isfinite(shotMouseY))
            {
                g_rcsLerpState.pendingMouseX += shotMouseX;
                g_rcsLerpState.pendingMouseY += shotMouseY;
            }

            g_rcsLerpState.lastShotsFired = shotsFired;
            g_rcsLerpState.lastPitch = lastPunch.pitch;
            g_rcsLerpState.lastYaw = lastPunch.yaw;

            // Keep legacy g_rcsState synced if external code inspects it
            g_rcsState.lastShotsFired = shotsFired;
            g_rcsState.lastPunch = { lastPunch.pitch, lastPunch.yaw };
        }

        // 5. Lerp a smooth fraction of pending compensation every tick
        const float lerpSpeed = std::isfinite(AimConfig::rcsLerpSpeed) ? AimConfig::rcsLerpSpeed : 12.0f;
        const float alpha = ComputeRcsLerpAlpha(lerpSpeed, dt);

        const float stepX = g_rcsLerpState.pendingMouseX * alpha;
        const float stepY = g_rcsLerpState.pendingMouseY * alpha;

        g_rcsLerpState.pendingMouseX -= stepX;
        g_rcsLerpState.pendingMouseY -= stepY;

        if (std::fabs(g_rcsLerpState.pendingMouseX) < 0.001f) g_rcsLerpState.pendingMouseX = 0.0f;
        if (std::fabs(g_rcsLerpState.pendingMouseY) < 0.001f) g_rcsLerpState.pendingMouseY = 0.0f;

        if (std::isfinite(stepX) && std::isfinite(stepY) &&
            (std::fabs(stepX) > 0.0001f || std::fabs(stepY) > 0.0001f))
        {
            SendRelativeMouseMove(stepX, stepY, MouseMoveChannel::Recoil);
        }
    }

    void TickAimbotAndRCS(const EntityFrame& currentFrame, float dt) noexcept
    {
        if (Menu::menuOpen || !g_targetWindowActive)
        {
            ResetRcsState();
            ResetRcsLerpState();
            g_lastAimTargetPawn = 0;
            g_aimActivationWasHeld = false;
            ResetMouseMoveRemainder();
            return;
        }

        if (!AimConfig::enabled && !AimConfig::rcsEnabled)
        {
            ResetRcsState();
            ResetRcsLerpState();
            g_lastAimTargetPawn = 0;
            g_aimActivationWasHeld = false;
            ResetMouseMoveRemainder();
            return;
        }

        const uintptr_t clientBase = g_clientBase.load(std::memory_order_relaxed);
        const bool clientReady = IsSaneAimPointer(clientBase);

        uintptr_t localPawn = g_localPlayerPawn;
        if (!IsSaneAimPointer(localPawn) && clientReady)
        {
            uintptr_t directPawn = 0;
            if (SafeRead(clientBase + AimbotOffsets::dwLocalPlayerPawn, directPawn) &&
                IsSaneAimPointer(directPawn))
            {
                localPawn = directPawn;
            }
        }

        if (!IsSaneAimPointer(localPawn))
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder();
            g_lastAimTargetPawn = 0;
            g_aimActivationWasHeld = false;
            return;
        }

        const float sensitivity = std::clamp(static_cast<float>(AimConfig::sensitivity), 0.10f, 10.0f);
        const UINT activationKey = AimConfig::activationKey;

        const bool activationHeld =
            activationKey > 0 &&
            activationKey < 256 &&
            (GetAsyncKeyState(static_cast<int>(activationKey)) & 0x8000) != 0;

        if (!activationHeld || !AimConfig::enabled)
        {
            g_lastAimTargetPawn = 0;
            g_aimActivationWasHeld = false;
            ResetMouseMoveRemainder(MouseMoveChannel::Aim);
        }

        // AIMBOT
        if (activationHeld && AimConfig::enabled)
        {
            g_aimActivationWasHeld = true;

            int localHealth = 0;
            uint8_t localTeam = 0;
            Vec3 localOrigin{};
            Vec3 localViewOffset{};
            Vec3 viewAngles{};

            const bool localStateValid =
                clientReady &&
                SafeRead(localPawn + AimbotOffsets::m_iHealth, localHealth) &&
                localHealth > 0 &&
                localHealth <= 100 &&
                SafeRead(localPawn + AimbotOffsets::m_iTeamNum, localTeam) &&
                (localTeam == 2 || localTeam == 3) &&
                SafeRead(localPawn + AimbotOffsets::m_vecOrigin, localOrigin) &&
                SafeRead(localPawn + Offsets::m_vecViewOffset, localViewOffset) &&
                SafeRead(clientBase + AimbotOffsets::dwViewAngles, viewAngles);

            auto validVector = [](const Vec3& value) noexcept
                {
                    return
                        std::isfinite(value.x) &&
                        std::isfinite(value.y) &&
                        std::isfinite(value.z) &&
                        std::fabs(value.x) < 100000.0f &&
                        std::fabs(value.y) < 100000.0f &&
                        std::fabs(value.z) < 100000.0f;
                };

            const bool localVectorsValid =
                localStateValid &&
                validVector(localOrigin) &&
                validVector(localViewOffset) &&
                std::isfinite(viewAngles.x) &&
                std::isfinite(viewAngles.y) &&
                std::fabs(viewAngles.x) <= 180.0f &&
                std::fabs(viewAngles.y) <= 720.0f;

            if (localVectorsValid && g_width > 0 && g_height > 0)
            {
                const Vec3 localEye{
                    localOrigin.x + localViewOffset.x,
                    localOrigin.y + localViewOffset.y,
                    localOrigin.z + localViewOffset.z
                };

                const float centerX = static_cast<float>(g_width) * 0.5f;
                const float centerY = static_cast<float>(g_height) * 0.5f;

                float bestDistance = std::clamp(static_cast<float>(AimConfig::fovRadius), 20.0f, 400.0f);
                Vec3 bestPosition{};
                uintptr_t bestTargetPawn = 0;
                bool foundTarget = false;

                for (const EntityDrawData& target : currentFrame)
                {
                    if (!target.pawnKey)
                        continue;

                    uintptr_t sceneNode = 0;
                    bool dormant = true;
                    if (!SafeRead(target.pawnKey + Offsets::m_pGameSceneNode, sceneNode) ||
                        !sceneNode ||
                        !SafeRead(sceneNode + AimbotOffsets::m_bDormant, dormant) ||
                        dormant)
                    {
                        continue;
                    }

                    if (!target.valid ||
                        target.pawnKey == 0 ||
                        target.pawnKey == localPawn ||
                        target.health <= 0 ||
                        target.health > 100 ||
                        (target.team != 2 && target.team != 3) ||
                        (!AimConfig::deathmatchMode && target.team == localTeam) ||
                        !target.headBoneValid ||
                        !target.skeletonScreen[6].visible)
                    {
                        continue;
                    }

                    const Vec3 headPosition = target.skeletonWorld[6];
                    if (!validVector(headPosition) ||
                        Vec3DistanceSquared(headPosition, target.worldOrigin) > 155.0f * 155.0f)
                    {
                        continue;
                    }

                    const float screenX = target.skeletonScreen[6].x;
                    const float screenY = target.skeletonScreen[6].y;
                    const float distance = std::hypot(screenX - centerX, screenY - centerY);

                    if (!std::isfinite(distance) || distance > bestDistance)
                    {
                        continue;
                    }

                    bestDistance = distance;
                    bestPosition = headPosition;
                    bestTargetPawn = target.pawnKey;
                    foundTarget = true;
                }

                if (foundTarget)
                {
                    if (bestTargetPawn != g_lastAimTargetPawn)
                    {
                        g_lastAimTargetPawn = bestTargetPawn;
                        ResetMouseMoveRemainder(MouseMoveChannel::Aim);
                    }

                    const Vec3 aim = CalcAimAngle(localEye, bestPosition);
                    const float deltaPitch = NormalizeAimAngle(aim.x - viewAngles.x);
                    const float deltaYaw = NormalizeAimAngle(aim.y - viewAngles.y);
                    const float lerpSpeed = std::clamp(
                        std::isfinite(AimConfig::lerpSpeed) ? AimConfig::lerpSpeed : 24.0f,
                        1.0f, 60.0f);
                    const float alpha = 1.0f - std::exp(-lerpSpeed * std::clamp(dt, 0.0f, 0.05f));

                    const float mouseX = (-deltaYaw / (0.022f * sensitivity)) * alpha;
                    const float mouseY = (deltaPitch / (0.022f * sensitivity)) * alpha;

                    if (std::isfinite(mouseX) && std::isfinite(mouseY))
                    {
                        SendRelativeMouseMove(mouseX, mouseY, MouseMoveChannel::Aim);
                    }
                }
                else if (g_lastAimTargetPawn != 0)
                {
                    g_lastAimTargetPawn = 0;
                    ResetMouseMoveRemainder(MouseMoveChannel::Aim);
                }
            }
            else
            {
                g_lastAimTargetPawn = 0;
                ResetMouseMoveRemainder(MouseMoveChannel::Aim);
            }
        }

        // RECOIL PROCESSING IS INDEPENDENT OF AIM ACTIVATION.
        if (AimConfig::rcsEnabled)
        {
            ApplyAimRCS(localPawn, sensitivity, dt);
        }
        else
        {
            ResetRcsState();
            ResetRcsLerpState();
            ResetMouseMoveRemainder(MouseMoveChannel::Recoil);
        }
    }

    // Call this where your tick function runs:
    void RunAimAndRcsFrame(const EntityFrame& currentFrame) noexcept
    {
        static auto previousTick = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previousTick).count();
        previousTick = now;

        TickAimbotAndRCS(currentFrame, dt);
    }
    bool SameEntityFrame(const EntityFrame& a, const EntityFrame& b) noexcept
    {
        constexpr float kPixelEpsilon = 0.35f;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].valid != b[i].valid) return false;
            if (!a[i].valid) continue;
            if (a[i].pawnKey != b[i].pawnKey || a[i].health != b[i].health || a[i].armor != b[i].armor ||
                a[i].weaponId != b[i].weaponId || a[i].distanceMeters != b[i].distanceMeters ||
                a[i].distanceValid != b[i].distanceValid ||
                std::strcmp(a[i].playerName, b[i].playerName) != 0 ||
                a[i].skeletonValid != b[i].skeletonValid ||
                std::fabs(a[i].left - b[i].left) > kPixelEpsilon ||
                std::fabs(a[i].top - b[i].top) > kPixelEpsilon ||
                std::fabs(a[i].right - b[i].right) > kPixelEpsilon ||
                std::fabs(a[i].bottom - b[i].bottom) > kPixelEpsilon)
                return false;
            if (Menu::skeleton && a[i].skeletonValid)
            {
                for (const size_t boneIndex : kSkeletonUsedBones)
                {
                    const auto& pa = a[i].skeletonScreen[boneIndex];
                    const auto& pb = b[i].skeletonScreen[boneIndex];
                    if (pa.visible != pb.visible ||
                        (pa.visible && (std::fabs(pa.x - pb.x) > kPixelEpsilon ||
                            std::fabs(pa.y - pb.y) > kPixelEpsilon)))
                        return false;
                }
            }
        }
        return true;
    }

    struct HitMarkerSoundRequest
    {
        WAVEFORMATEX format{};
        std::vector<unsigned char> pcm;
    };

    std::atomic<unsigned> g_activeHitMarkerVoices{ 0 };

    DWORD WINAPI HitMarkerSoundWorker(LPVOID parameter) noexcept
    {
        auto* request = static_cast<HitMarkerSoundRequest*>(parameter);
        if (!request)
        {
            g_activeHitMarkerVoices.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }

        HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HWAVEOUT output = nullptr;
        WAVEHDR header{};
        bool prepared = false;

        if (completionEvent &&
            waveOutOpen(&output, WAVE_MAPPER, &request->format,
                reinterpret_cast<DWORD_PTR>(completionEvent), 0, CALLBACK_EVENT) == MMSYSERR_NOERROR)
        {
            header.lpData = reinterpret_cast<LPSTR>(request->pcm.data());
            header.dwBufferLength = static_cast<DWORD>(request->pcm.size());
            if (waveOutPrepareHeader(output, &header, sizeof(header)) == MMSYSERR_NOERROR)
            {
                prepared = true;
                ResetEvent(completionEvent);
                if (waveOutWrite(output, &header, sizeof(header)) == MMSYSERR_NOERROR)
                {
                    const DWORD waitResult = WaitForSingleObject(completionEvent, 5000);
                    if (waitResult != WAIT_OBJECT_0)
                    {
                        waveOutReset(output);
                        WaitForSingleObject(completionEvent, 1000);
                    }
                }
            }
        }

        if (output)
        {
            if (prepared)
            {
                // A timeout/reset marks queued data complete before unprepare.
                waveOutUnprepareHeader(output, &header, sizeof(header));
            }
            waveOutClose(output);
        }
        if (completionEvent) CloseHandle(completionEvent);
        delete request;
        g_activeHitMarkerVoices.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    void PlayHitMarkerSound() noexcept
    {
        if (!Menu::hitMarkerSound) return;
        const float volume = std::clamp(Menu::hitMarkerVolume, 0.0f, 2.0f);
        if (volume <= 0.001f) return;

        // Keep the WAV embedded for manual-map reliability, but give every hit
        // its own waveOut stream. Windows' shared audio engine mixes the streams.
        const auto* wav = HitMarkerAudio::kWavData;
        constexpr size_t wavSize = sizeof(HitMarkerAudio::kWavData);
        if (wavSize < 44 || std::memcmp(wav, "RIFF", 4) != 0 || std::memcmp(wav + 8, "WAVE", 4) != 0)
            return;

        auto readU16 = [](const unsigned char* p) noexcept -> uint16_t
            {
                return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
            };
        auto readU32 = [](const unsigned char* p) noexcept -> uint32_t
            {
                return static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) |
                    (static_cast<uint32_t>(p[3]) << 24);
            };

        WAVEFORMATEX format{};
        const unsigned char* pcmData = nullptr;
        size_t pcmBytes = 0;
        bool foundFormat = false;
        for (size_t offset = 12; offset + 8 <= wavSize;)
        {
            const uint32_t chunkBytes = readU32(wav + offset + 4);
            const size_t chunkData = offset + 8;
            if (chunkBytes > wavSize - chunkData) break;

            if (std::memcmp(wav + offset, "fmt ", 4) == 0 && chunkBytes >= 16)
            {
                format.wFormatTag = readU16(wav + chunkData + 0);
                format.nChannels = readU16(wav + chunkData + 2);
                format.nSamplesPerSec = readU32(wav + chunkData + 4);
                format.nAvgBytesPerSec = readU32(wav + chunkData + 8);
                format.nBlockAlign = readU16(wav + chunkData + 12);
                format.wBitsPerSample = readU16(wav + chunkData + 14);
                format.cbSize = 0;
                foundFormat = true;
            }
            else if (std::memcmp(wav + offset, "data", 4) == 0)
            {
                pcmData = wav + chunkData;
                pcmBytes = chunkBytes;
            }

            offset = chunkData + static_cast<size_t>(chunkBytes) + (chunkBytes & 1u);
        }

        // This embedded asset is standard mono/stereo 16-bit PCM.
        if (!foundFormat || !pcmData || pcmBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM ||
            format.wBitsPerSample != 16 || format.nChannels == 0 || format.nBlockAlign == 0)
            return;

        unsigned active = g_activeHitMarkerVoices.load(std::memory_order_relaxed);
        do
        {
            if (active >= 16) return; // cap runaway allocations while retaining rapid-hit overlap
        } while (!g_activeHitMarkerVoices.compare_exchange_weak(
            active, active + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

        auto* request = new (std::nothrow) HitMarkerSoundRequest{};
        if (!request)
        {
            g_activeHitMarkerVoices.fetch_sub(1, std::memory_order_relaxed);
            return;
        }

        request->format = format;
        try
        {
            request->pcm.assign(pcmData, pcmData + pcmBytes);
        }
        catch (...)
        {
            delete request;
            g_activeHitMarkerVoices.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        for (size_t i = 0; i + 1 < request->pcm.size(); i += 2)
        {
            int16_t sample = 0;
            std::memcpy(&sample, request->pcm.data() + i, sizeof(sample));
            sample = static_cast<int16_t>(std::clamp(
                std::lround(static_cast<float>(sample) * volume), -32768L, 32767L));
            std::memcpy(request->pcm.data() + i, &sample, sizeof(sample));
        }

        HANDLE worker = CreateThread(nullptr, 0, HitMarkerSoundWorker, request, 0, nullptr);
        if (worker)
            CloseHandle(worker);
        else
        {
            delete request;
            g_activeHitMarkerVoices.fetch_sub(1, std::memory_order_relaxed);
            OutputDebugStringW(L"[Overlay] Could not start an overlapping hitmarker audio voice.\n");
        }
    }

    void RememberRecentDamagedEntity(const EntityFrame& currentFrame, ULONGLONG now) noexcept
    {
        if (!g_hasLastEntityFrame) return;

        int largestHealthDrop = 0;
        const EntityDrawData* damagedEntity = nullptr;

        for (const EntityDrawData& current : currentFrame)
        {
            if (!current.valid || !current.pawnKey) continue;

            for (const EntityDrawData& previous : g_lastEntityFrame)
            {
                if (!previous.valid || previous.pawnKey != current.pawnKey) continue;

                const int healthDrop = previous.health - current.health;
                if (healthDrop > largestHealthDrop)
                {
                    largestHealthDrop = healthDrop;
                    damagedEntity = &current;
                }
                break;
            }
        }

        if (!damagedEntity) return;

        // Spawn an independent number above the damaged player's upper body.
        // Recent hits on the same pawn use small alternating screen-space lanes,
        // so consecutive values remain readable instead of drawing on top of each other.
        if (Menu::damagePopup && largestHealthDrop > 0)
        {
            int sameTargetCount = 0;
            for (const auto& existing : g_damagePopups)
            {
                if (existing.active && existing.pawnKey == damagedEntity->pawnKey &&
                    now - existing.born < kDamagePopupLifetimeMs)
                    ++sameTargetCount;
            }

            DamagePopup* slot = nullptr;
            for (auto& popup : g_damagePopups)
            {
                if (!popup.active || now - popup.born >= kDamagePopupLifetimeMs)
                {
                    slot = &popup;
                    break;
                }
            }
            if (!slot)
            {
                // The pool is full: recycle the oldest visible number.
                slot = &g_damagePopups[0];
                for (auto& popup : g_damagePopups)
                    if (popup.born < slot->born) slot = &popup;
            }

            static constexpr float kPopupLanes[] = { 0.0f, -15.0f, 15.0f, -27.0f, 27.0f };
            slot->world = damagedEntity->worldOrigin;
            slot->world.z += 75.0f;
            slot->pawnKey = damagedEntity->pawnKey;
            slot->damage = largestHealthDrop;
            slot->born = now;
            slot->horizontalOffset = kPopupLanes[sameTargetCount % (sizeof(kPopupLanes) / sizeof(kPopupLanes[0]))];
            slot->active = true;
        }

        g_recentDamagedWorldPosition = damagedEntity->worldOrigin;
        g_recentDamagedWorldPosition.z += 42.0f; // approximate torso/center-mass height
        g_recentDamagedTime = now;
        g_recentDamagedPositionValid =
            std::isfinite(g_recentDamagedWorldPosition.x) &&
            std::isfinite(g_recentDamagedWorldPosition.y) &&
            std::isfinite(g_recentDamagedWorldPosition.z);
    }

    bool CaptureHitMarkerWorldPosition(const EntityFrame& currentFrame) noexcept
    {
        if (!g_hasLastEntityFrame) return false;

        int largestHealthDrop = 0;
        const EntityDrawData* damagedEntity = nullptr;
        for (const EntityDrawData& current : currentFrame)
        {
            if (!current.valid || !current.pawnKey) continue;
            for (const EntityDrawData& previous : g_lastEntityFrame)
            {
                if (!previous.valid || previous.pawnKey != current.pawnKey) continue;
                const int healthDrop = previous.health - current.health;
                if (healthDrop > largestHealthDrop)
                {
                    largestHealthDrop = healthDrop;
                    damagedEntity = &current;
                }
                break;
            }
        }

        if (!damagedEntity) return false;

        // TotalRoundDamageDealt identifies that local damage occurred, while
        // the health delta identifies the likely victim. The damage accumulator
        // does not expose a hitbox/impact coordinate, so anchor to torso height.
        g_hitMarkerWorldPosition = damagedEntity->worldOrigin;
        g_hitMarkerWorldPosition.z += 42.0f;
        g_hitMarkerWorldPositionValid =
            std::isfinite(g_hitMarkerWorldPosition.x) &&
            std::isfinite(g_hitMarkerWorldPosition.y) &&
            std::isfinite(g_hitMarkerWorldPosition.z);
        return g_hitMarkerWorldPositionValid;
    }

    bool UpdateHitMarkerState(const EntityFrame& currentFrame) noexcept
    {
        if (!g_playerSchemaLoaded)
            (void)LoadPlayerSchemaOffsets();

        // Keep a short-lived victim candidate. The game can publish pawn health
        // and the local round-damage counter on different frames.
        const ULONGLONG sampleNow = GetTickCount64();
        RememberRecentDamagedEntity(currentFrame, sampleNow);

        // Continue sampling with the visual disabled so re-enabling it does not
        // replay old damage. Sound remains independently controlled.
        const HMODULE clientModule = GetModuleHandleW(L"client.dll");
        if (!clientModule) return Menu::hitMarker && g_hitMarkerWorldPositionValid &&
            GetTickCount64() < g_hitMarkerVisibleUntil;

        const uintptr_t clientBase = reinterpret_cast<uintptr_t>(clientModule);
        uintptr_t localController = 0;
        if (!SafeRead(clientBase + g_dwLocalPlayerControllerOffset, localController) || !localController)
            return Menu::hitMarker && g_hitMarkerWorldPositionValid &&
            GetTickCount64() < g_hitMarkerVisibleUntil;

        if (localController != g_hitMarkerController)
        {
            g_hitMarkerController = localController;
            g_roundDamageInitialized = false;
            g_hitMarkerVisibleUntil = 0;
            g_hitMarkerCaptureUntil = 0;
            g_hitMarkerWorldPositionValid = false;
            g_recentDamagedPositionValid = false;
        }

        uintptr_t actionTrackingServices = 0;
        const uintptr_t actionTrackingOffset = g_playerSchema.actionTrackingServices
            ? g_playerSchema.actionTrackingServices : Offsets::m_pActionTrackingServices;
        if (!SafeRead(localController + actionTrackingOffset, actionTrackingServices) ||
            !actionTrackingServices)
            return Menu::hitMarker && g_hitMarkerWorldPositionValid &&
            GetTickCount64() < g_hitMarkerVisibleUntil;

        float currentDamage = 0.0f;
        if (!SafeRead(actionTrackingServices + Offsets::m_flTotalRoundDamageDealt, currentDamage) ||
            !std::isfinite(currentDamage) || currentDamage < 0.0f)
            return Menu::hitMarker && g_hitMarkerWorldPositionValid &&
            GetTickCount64() < g_hitMarkerVisibleUntil;

        const ULONGLONG now = GetTickCount64();
        if (!g_roundDamageInitialized)
        {
            g_lastRoundDamage = currentDamage;
            g_roundDamageInitialized = true;
        }
        else if (currentDamage + 0.001f < g_lastRoundDamage)
        {
            // Round reset: establish a new baseline without generating a hit.
            g_lastRoundDamage = currentDamage;
        }
        else
        {
            if (currentDamage > g_lastRoundDamage + 0.001f)
            {
                g_hitMarkerVisibleUntil = now + kHitMarkerDurationMs;
                g_hitMarkerCaptureUntil = now + kHitMarkerCaptureWindowMs;

                // The victim's health may have changed one or more render
                // frames before the round-damage accumulator updates.
                if (g_recentDamagedPositionValid &&
                    now >= g_recentDamagedTime &&
                    now - g_recentDamagedTime <= kRecentDamageCorrelationMs)
                {
                    g_hitMarkerWorldPosition = g_recentDamagedWorldPosition;
                    g_hitMarkerWorldPositionValid = true;
                }
                else
                {
                    g_hitMarkerWorldPositionValid = false;
                }

                PlayHitMarkerSound();
            }
            g_lastRoundDamage = currentDamage;
        }

        // Health can update one render after the damage accumulator. Keep a
        // short capture window to correlate the damage tick with the victim.
        if (!g_hitMarkerWorldPositionValid && now <= g_hitMarkerCaptureUntil)
            (void)CaptureHitMarkerWorldPosition(currentFrame);

        return Menu::hitMarker && g_hitMarkerWorldPositionValid &&
            now < g_hitMarkerVisibleUntil;
    }

    void DrawHitMarker() noexcept
    {
        if (!g_d2dTarget || !g_brushHitMarker || !g_hitMarkerWorldPositionValid) return;

        const ULONGLONG now = GetTickCount64();
        if (now >= g_hitMarkerVisibleUntil) return;

        const HMODULE clientModule = GetModuleHandleW(L"client.dll");
        if (!clientModule) return;

        Mat4 viewMatrix{};
        const uintptr_t clientBase = reinterpret_cast<uintptr_t>(clientModule);
        if (!SafeRead(clientBase + Offsets::dwViewMatrix, viewMatrix)) return;

        const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height) };
        float markerX = 0.0f, markerY = 0.0f, depth = 0.0f;
        if (!WorldToScreen(viewMatrix, g_hitMarkerWorldPosition, viewport, markerX, markerY, depth))
            return;

        if (markerX < -40.0f || markerX > viewport.w + 40.0f ||
            markerY < -40.0f || markerY > viewport.h + 40.0f)
            return;

        // Larger base dimensions make the marker easy to see. Perspective
        // scaling still makes distant world impacts smaller than nearby ones.
        const float distanceScale = std::clamp(std::sqrt(420.0f / (std::max)(depth, 1.0f)), 0.72f, 1.35f);
        const float scale = distanceScale * std::clamp(Menu::hitMarkerScale, 0.35f, 1.0f);
        const float gap = 4.0f * scale;
        const float arm = 10.0f * scale;
        const float thickness = 2.0f * scale;

        const D2D1_POINT_2F points[][2] = {
            { { markerX - gap - arm, markerY - gap - arm },{ markerX - gap, markerY - gap } },
            { { markerX + gap, markerY - gap },{ markerX + gap + arm, markerY - gap - arm } },
            { { markerX - gap - arm, markerY + gap + arm },{ markerX - gap, markerY + gap } },
            { { markerX + gap, markerY + gap },{ markerX + gap + arm, markerY + gap + arm } }
        };

        // Explicitly force solid white with no opacity animation.
        g_brushHitMarker->SetColor(Color(Menu::hitMarkerColorR, Menu::hitMarkerColorG, Menu::hitMarkerColorB, 1.0f));
        g_brushHitMarker->SetOpacity(1.0f);

        for (const auto& line : points)
        {
            if (g_brushOutline)
            {
                g_brushOutline->SetOpacity(1.0f);
                g_d2dTarget->DrawLine(
                    { line[0].x + 0.6f * scale, line[0].y + 0.6f * scale },
                    { line[1].x + 0.6f * scale, line[1].y + 0.6f * scale },
                    g_brushOutline, thickness + 1.4f * scale);
            }

            g_d2dTarget->DrawLine(line[0], line[1], g_brushHitMarker, thickness);
        }
    }

    void DrawDamagePopups() noexcept
    {
        if (!Menu::damagePopup || !g_d2dTarget || !g_brushHitMarker || !g_brushOutline ||
            !g_brushDamagePopupOutline || !g_damageTextFormat || !g_writeFactory) return;

        const HMODULE clientModule = GetModuleHandleW(L"client.dll");
        if (!clientModule) return;

        Mat4 viewMatrix{};
        if (!SafeRead(reinterpret_cast<uintptr_t>(clientModule) + Offsets::dwViewMatrix, viewMatrix))
            return;

        const ULONGLONG now = GetTickCount64();
        const float duration = static_cast<float>(kDamagePopupLifetimeMs);
        const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height) };
        g_brushHitMarker->SetColor(Color(
            Menu::damagePopupColorR,
            Menu::damagePopupColorG,
            Menu::damagePopupColorB,
            1.0f));
        g_brushDamagePopupOutline->SetColor(Color(
            Menu::damagePopupOutlineColorR,
            Menu::damagePopupOutlineColorG,
            Menu::damagePopupOutlineColorB,
            1.0f));

        const auto smoothStep = [](float value) noexcept -> float
            {
                value = std::clamp(value, 0.0f, 1.0f);
                return value * value * (3.0f - 2.0f * value);
            };

        for (auto& popup : g_damagePopups)
        {
            if (!popup.active)
                continue;

            const ULONGLONG age = now - popup.born;
            if (age >= kDamagePopupLifetimeMs)
            {
                popup.active = false;
                continue;
            }

            const float life = std::clamp(static_cast<float>(age) / duration, 0.0f, 1.0f);

            float x = 0.0f;
            float y = 0.0f;
            float depth = 0.0f;
            if (!WorldToScreen(viewMatrix, popup.world, viewport, x, y, depth))
                continue;

            // Fast initial lift that eases naturally as the number travels up.
            const float riseProgress = 1.0f - std::pow(1.0f - life, 2.15f);
            y -= 58.0f * riseProgress;
            x += popup.horizontalOffset * (0.68f + 0.32f * riseProgress);

            // A compact pop-in with a restrained overshoot, then a stable size.
            const float popT = std::clamp(static_cast<float>(age) / 135.0f, 0.0f, 1.0f);
            constexpr float backC1 = 1.35f;
            constexpr float backC3 = backC1 + 1.0f;
            const float backDelta = popT - 1.0f;
            const float backEase = 1.0f + backC3 * backDelta * backDelta * backDelta +
                backC1 * backDelta * backDelta;
            const float popScale = 0.80f + 0.20f * backEase;
            const float distanceScale = std::clamp(
                std::sqrt(420.0f / (std::max)(depth, 1.0f)), 0.75f, 1.25f);
            const float userScale = std::clamp(Menu::damagePopupScale, 0.65f, 3.0f);
            const float scale = distanceScale * userScale * popScale;

            // Brief entrance fade, a readable hold, then a smooth fade-out.
            const float fadeIn = smoothStep(static_cast<float>(age) / 75.0f);
            constexpr float fadeStart = 0.56f;
            const float fadeOutT = std::clamp((life - fadeStart) / (1.0f - fadeStart), 0.0f, 1.0f);
            const float fadeOut = 1.0f - smoothStep(fadeOutT);
            const float opacity = fadeIn * fadeOut;
            if (opacity <= 0.005f)
                continue;

            wchar_t label[24]{};
            swprintf_s(label, L"%d", popup.damage);
            const UINT32 labelLength = static_cast<UINT32>(wcslen(label));
            if (labelLength == 0)
                continue;

            D2D1_MATRIX_3X2_F oldTransform{};
            g_d2dTarget->GetTransform(&oldTransform);
            const D2D1_MATRIX_3X2_F popupTransform =
                D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(x, y));
            g_d2dTarget->SetTransform(popupTransform * oldTransform);

            constexpr float textWidth = 176.0f;
            constexpr float textHeight = 52.0f;
            const D2D1_RECT_F rect{
                x - textWidth * 0.5f,
                y - textHeight * 0.5f,
                x + textWidth * 0.5f,
                y + textHeight * 0.5f
            };

            IDWriteTextLayout* textLayout = nullptr;
            if (SUCCEEDED(g_writeFactory->CreateTextLayout(
                label, labelLength, g_damageTextFormat, textWidth, textHeight, &textLayout)) && textLayout)
            {
                textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                textLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                const float boldness = std::clamp(Menu::damagePopupBoldness, 0.0f, 1.0f);
                const int weightValue = 400 + static_cast<int>(std::lround(boldness * 5.0f)) * 100;
                const DWRITE_TEXT_RANGE allText{ 0, labelLength };
                textLayout->SetFontWeight(static_cast<DWRITE_FONT_WEIGHT>(weightValue), allText);

                const D2D1_POINT_2F origin{ rect.left, rect.top };

                // A subtle offset shadow and tight dark edge keep the red face
                // legible over bright maps without making it look like a badge.
                g_brushOutline->SetOpacity(opacity * 0.82f);
                g_brushHitMarker->SetOpacity(opacity);
                g_d2dTarget->DrawTextLayout(
                    { origin.x + 1.5f, origin.y + 2.0f }, textLayout,
                    g_brushOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP);

                if (Menu::damagePopupOutline)
                {
                    constexpr float outline = 1.05f;
                    g_brushDamagePopupOutline->SetOpacity(opacity);
                    g_d2dTarget->DrawTextLayout(
                        { origin.x - outline, origin.y }, textLayout,
                        g_brushDamagePopupOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    g_d2dTarget->DrawTextLayout(
                        { origin.x + outline, origin.y }, textLayout,
                        g_brushDamagePopupOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    g_d2dTarget->DrawTextLayout(
                        { origin.x, origin.y - outline }, textLayout,
                        g_brushDamagePopupOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    g_d2dTarget->DrawTextLayout(
                        { origin.x, origin.y + outline }, textLayout,
                        g_brushDamagePopupOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }

                g_d2dTarget->DrawTextLayout(
                    origin, textLayout, g_brushHitMarker, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textLayout->Release();
            }

            g_d2dTarget->SetTransform(oldTransform);
            g_brushOutline->SetOpacity(1.0f);
            g_brushHitMarker->SetOpacity(1.0f);
            g_brushDamagePopupOutline->SetOpacity(1.0f);
        }
    }

    void DrawEntityFrame(const EntityFrame& frame) noexcept
    {
        if (!Menu::box && !Menu::health && !Menu::armor && !Menu::snapline && !Menu::skeleton &&
            !Menu::playerName && !Menu::weaponIcon && !Menu::distance) return;
        for (const EntityDrawData& entity : frame)
        {
            if (!entity.valid) continue;
            if (Menu::snapline) DrawSnapline(entity);
            if (Menu::skeleton) DrawSkeleton(entity);
            if (Menu::box) DrawEntityBox(entity.left, entity.top, entity.right, entity.bottom);
            const bool shareSide = Menu::health && Menu::armor && Menu::healthPosition == Menu::armorPosition;
            if (Menu::health) DrawHealthBar(entity, 0U);
            if (Menu::armor) DrawArmorBar(entity, shareSide ? 1U : 0U);
            if (Menu::playerName || Menu::weaponIcon || Menu::distance)
                DrawPlayerInfo(entity);
        }
    }

    // Forward declarations (needed because some functions call each other)
    void ApplyMenuInputMode(bool requestFocus = false, bool forcePassive = false) noexcept;
    bool RenderFrame() noexcept;

    // -------------------- HWND focus hand-off --------------------
    // The overlay does not manipulate the cursor position, visibility counter,
    // clipping rectangle, or cursor shape. Opening the menu activates this HWND;
    // closing it returns foreground/input focus to the main HWND owned by CS2.
    struct ProcessWindowSearch
    {
        DWORD processId = 0;
        HWND excludedWindow = nullptr;
        HWND bestWindow = nullptr;
        long long bestScore = -1;
    };

    BOOL CALLBACK FindProcessWindowEnumProc(HWND window, LPARAM parameter) noexcept
    {
        auto* search = reinterpret_cast<ProcessWindowSearch*>(parameter);
        if (!search || !IsWindow(window) || window == search->excludedWindow || !IsWindowVisible(window))
            return TRUE;

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != search->processId)
            return TRUE;

        const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0)
            return TRUE;

        RECT client{};
        if (!GetClientRect(window, &client))
            return TRUE;
        const long long width = std::max<LONG>(0, client.right - client.left);
        const long long height = std::max<LONG>(0, client.bottom - client.top);
        const long long area = width * height;
        if (area <= 0)
            return TRUE;

        // Prefer an unowned application window; retain an owned-window fallback
        // for games whose visible client window is hosted by a frame/owner HWND.
        const bool unowned = GetWindow(window, GW_OWNER) == nullptr;
        const long long score = area + (unowned ? (1LL << 50) : 0);
        if (score > search->bestScore)
        {
            search->bestScore = score;
            search->bestWindow = window;
        }
        return TRUE;
    }

    HWND FindGameProcessWindow() noexcept
    {
        if (!g_hostProcessValidated || g_hostProcessId == 0)
            return nullptr;
        ProcessWindowSearch search{};
        search.processId = g_hostProcessId;
        search.excludedWindow = g_hwnd;
        EnumWindows(FindProcessWindowEnumProc, reinterpret_cast<LPARAM>(&search));
        return search.bestWindow;
    }

    bool FocusGameProcessWindow() noexcept
    {
        HWND target = FindGameProcessWindow();
        if (!target || !IsWindow(target))
        {
            Log(L"Could not find a visible main window for the game process.");
            return false;
        }

        const DWORD currentThread = GetCurrentThreadId();
        const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
        const bool attached = targetThread != 0 && targetThread != currentThread &&
            AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;

        BringWindowToTop(target);
        SetForegroundWindow(target);
        SetActiveWindow(target);
        SetFocus(target);

        if (attached)
            AttachThreadInput(currentThread, targetThread, FALSE);

        return GetForegroundWindow() == target;
    }

    bool FocusMenuWindow() noexcept
    {
        if (!g_hwnd || !IsWindow(g_hwnd))
            return false;

        HWND foreground = GetForegroundWindow();
        const DWORD currentThread = GetCurrentThreadId();
        const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
            AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;

        BringWindowToTop(g_hwnd);
        SetForegroundWindow(g_hwnd);
        SetActiveWindow(g_hwnd);
        SetFocus(g_hwnd);

        if (attached)
            AttachThreadInput(currentThread, foregroundThread, FALSE);

        return GetForegroundWindow() == g_hwnd;
    }

    // -------------------- Minimal stubs for remaining UI (so it compiles) --------------------
    // NOTE: The full menu drawing, color picker, layout, input handling etc.
    // from your original second paste must be present for a complete UI.
    // Below are the critical runtime pieces for rendering, input focus, and cache.

    std::wstring GetAimKeyDisplayName(UINT key)
    {
        switch (key)
        {
        case VK_LBUTTON: return L"MOUSE1";
        case VK_RBUTTON: return L"MOUSE2";
        case VK_MBUTTON: return L"MOUSE3";
        case VK_XBUTTON1: return L"MOUSE4";
        case VK_XBUTTON2: return L"MOUSE5";
        case VK_SPACE: return L"SPACE";
        case VK_TAB: return L"TAB";
        case VK_SHIFT: return L"SHIFT";
        case VK_CONTROL: return L"CTRL";
        case VK_MENU: return L"ALT";
        case VK_CAPITAL: return L"CAPS";
        case VK_ESCAPE: return L"ESC";
        case VK_RETURN: return L"ENTER";
        case VK_BACK: return L"BACKSPACE";
        default:
            break;
        }

        const UINT scanCode = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        LONG keyNameParam = static_cast<LONG>(scanCode << 16);
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
            key == VK_PRIOR || key == VK_NEXT || key == VK_HOME || key == VK_END ||
            key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE)
            keyNameParam |= 1L << 24;
        wchar_t name[64]{};
        if (scanCode && GetKeyNameTextW(keyNameParam, name, static_cast<int>(_countof(name))) > 0)
            return name;
        wchar_t fallback[24]{};
        swprintf_s(fallback, L"VK 0x%02X", key);
        return fallback;
    }

    bool ContainsPoint(const D2D1_RECT_F& rect, float x, float y) noexcept
    {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    MenuLayout BuildMenuLayout() noexcept
    {
        constexpr float menuWidth = 560.0f;
        constexpr float menuHeight = 440.0f;
        if (!g_menuPositionInitialized)
        {
            g_menuX = std::max(8.0f, static_cast<float>(g_width) - menuWidth - 24.0f);
            g_menuY = 24.0f;
            g_menuPositionInitialized = true;
        }
        const float maxX = std::max(8.0f, static_cast<float>(g_width) - menuWidth - 8.0f);
        const float maxY = std::max(8.0f, static_cast<float>(g_height) - menuHeight - 8.0f);
        g_menuX = std::clamp(g_menuX, 8.0f, maxX);
        g_menuY = std::clamp(g_menuY, 8.0f, maxY);
        if (g_activeTab != MenuTab::ESP) g_menuScrollOffset = 0.0f;
        else g_menuScrollOffset = std::clamp(g_menuScrollOffset, 0.0f, kMenuMaxScroll);

        const float x = g_menuX, y = g_menuY;
        const float rowShift = -g_menuScrollOffset;
        auto hit = [&](float left, float top, float right, float bottom) {
            return D2D1_RECT_F{ x + left + 100.0f, y + top + 44.0f + rowShift,
                                x + right + 100.0f, y + bottom + 44.0f + rowShift };
            };

        MenuLayout layout{};
        layout.window = { x, y, x + menuWidth, y + menuHeight };
        layout.header = { x + 1.0f, y + 1.0f, x + menuWidth - 1.0f, y + 32.0f };
        layout.headerDragHit = layout.header;
        // Compact navigation cards: small hit targets with even spacing,
        // leaving the content area visually separate from the page selector.
        layout.tabAimbotHit = { x + 22.0f, y + 39.0f, x + 122.0f, y + 68.0f };
        layout.tabEspHit = { x + 130.0f, y + 39.0f, x + 230.0f, y + 68.0f };
        layout.contentClip = { x + 22.0f, y + 86.0f, x + menuWidth - 14.0f, y + 410.0f };

        // Match the ESP page's exact control column, widths and 24px row rhythm.
        layout.aimEnabledHit = { x + 350.0f, y + 92.0f, x + 410.0f, y + 110.0f };
        layout.aimFovHit = { x + 350.0f, y + 116.0f, x + 440.0f, y + 134.0f };
        layout.aimLerpSpeedHit = { x + 350.0f, y + 140.0f, x + 440.0f, y + 158.0f };
        layout.aimSensitivityHit = { x + 350.0f, y + 164.0f, x + 520.0f, y + 182.0f };
        layout.rcsEnabledHit = { x + 350.0f, y + 188.0f, x + 410.0f, y + 206.0f };
        layout.rcsStrengthHit = { x + 350.0f, y + 212.0f, x + 520.0f, y + 230.0f };
        layout.aimDeathmatchHit = { x + 350.0f, y + 236.0f, x + 430.0f, y + 254.0f };
        layout.espDeathmatchHit = hit(250.0f, 24.0f, 310.0f, 42.0f);

        layout.boxToggleHit = hit(250.0f, 48.0f, 310.0f, 66.0f);
        layout.boxColorSwatchHit = hit(320.0f, 48.0f, 400.0f, 66.0f);
        layout.filledBoxToggleHit = hit(250.0f, 72.0f, 310.0f, 90.0f);
        layout.filledColorSwatchHit = hit(320.0f, 72.0f, 400.0f, 90.0f);
        layout.cornerValueHit = hit(250.0f, 144.0f, 340.0f, 162.0f);
        layout.healthToggleHit = hit(250.0f, 168.0f, 310.0f, 186.0f);
        layout.healthColorSwatchHit = hit(320.0f, 168.0f, 400.0f, 186.0f);
        layout.healthValueHit = hit(250.0f, 240.0f, 340.0f, 258.0f);
        layout.healthPositionHit = hit(250.0f, 264.0f, 400.0f, 282.0f);
        layout.armorToggleHit = hit(250.0f, 288.0f, 310.0f, 306.0f);
        layout.armorColorSwatchHit = hit(320.0f, 288.0f, 400.0f, 306.0f);
        layout.armorValueHit = hit(250.0f, 312.0f, 340.0f, 330.0f);
        layout.armorPositionHit = hit(250.0f, 336.0f, 400.0f, 354.0f);
        layout.snaplineToggleHit = hit(250.0f, 360.0f, 310.0f, 378.0f);
        layout.snaplineColorSwatchHit = hit(320.0f, 360.0f, 400.0f, 378.0f);
        layout.skeletonToggleHit = hit(250.0f, 384.0f, 310.0f, 402.0f);
        layout.skeletonColorSwatchHit = hit(320.0f, 384.0f, 400.0f, 402.0f);
        layout.skeletonOpacityValueHit = hit(250.0f, 408.0f, 340.0f, 426.0f);
        layout.playerNameToggleHit = hit(250.0f, 432.0f, 310.0f, 450.0f);
        layout.playerNameColorSwatchHit = hit(320.0f, 432.0f, 400.0f, 450.0f);
        layout.weaponIconToggleHit = hit(250.0f, 456.0f, 310.0f, 474.0f);
        layout.weaponIconColorSwatchHit = hit(320.0f, 456.0f, 400.0f, 474.0f);
        layout.distanceToggleHit = hit(250.0f, 480.0f, 310.0f, 498.0f);
        layout.distanceColorSwatchHit = hit(320.0f, 480.0f, 400.0f, 498.0f);
        layout.hitMarkerToggleHit = hit(250.0f, 504.0f, 310.0f, 522.0f);
        layout.hitMarkerSoundToggleHit = hit(250.0f, 528.0f, 310.0f, 546.0f);
        layout.hitMarkerScaleValueHit = hit(250.0f, 552.0f, 340.0f, 570.0f);
        layout.hitMarkerVolumeValueHit = hit(250.0f, 576.0f, 340.0f, 594.0f);
        layout.hitMarkerColorSwatchHit = hit(320.0f, 600.0f, 400.0f, 618.0f);
        layout.damagePopupToggleHit = hit(250.0f, 624.0f, 310.0f, 642.0f);
        layout.damagePopupColorSwatchHit = hit(320.0f, 648.0f, 400.0f, 666.0f);
        layout.damagePopupScaleValueHit = hit(250.0f, 672.0f, 340.0f, 690.0f);
        layout.damagePopupBoldnessValueHit = hit(250.0f, 696.0f, 400.0f, 714.0f);
        layout.damagePopupOutlineToggleHit = hit(250.0f, 720.0f, 310.0f, 738.0f);
        layout.damagePopupOutlineColorSwatchHit = hit(320.0f, 744.0f, 400.0f, 762.0f);
        layout.filledBoxStyleHit = hit(250.0f, 96.0f, 400.0f, 114.0f);
        layout.filledGradientColorSwatchHit = hit(320.0f, 120.0f, 400.0f, 138.0f);
        layout.healthGradientToggleHit = hit(250.0f, 192.0f, 310.0f, 210.0f);
        layout.healthGradientColorSwatchHit = hit(320.0f, 216.0f, 400.0f, 234.0f);

        layout.scrollTrack = { x + menuWidth - 5.0f, layout.contentClip.top,
                               x + menuWidth - 4.0f, layout.contentClip.bottom };
        const float trackHeight = layout.scrollTrack.bottom - layout.scrollTrack.top;
        const float thumbHeight = trackHeight - 22.0f;
        const float thumbTravel = trackHeight - thumbHeight;
        const float scrollRatio = kMenuMaxScroll > 0.0f ? g_menuScrollOffset / kMenuMaxScroll : 0.0f;
        const float thumbTop = layout.scrollTrack.top + thumbTravel * scrollRatio;
        layout.scrollThumb = { layout.scrollTrack.left, thumbTop,
                               layout.scrollTrack.right, thumbTop + thumbHeight };

        layout.exitButtonHit = { 0,0,0,0 };
        layout.colorPickerPanel = { x + 360.0f, y + 80.0f, x + 540.0f, y + 150.0f };
        layout.colorSVHit = { 0,0,0,0 };
        layout.colorHueHit = { 0,0,0,0 };
        return layout;
    }

    bool CreatePickerGradientResources() noexcept { return true; }
    bool RebuildPickerSaturationGradient() noexcept { return true; }

    void DrawMenu() noexcept
    {
        if (!g_d2dTarget || !Menu::menuOpen) return;
        const MenuLayout layout = BuildMenuLayout();

        g_d2dTarget->FillRectangle(&layout.window, g_brushMenuBack);
        g_d2dTarget->DrawRectangle(layout.window, g_brushMenuBorder, 1.0f);
        g_d2dTarget->FillRectangle(&layout.header, g_brushMenuPanel);
        g_d2dTarget->DrawLine({ layout.window.left + 1.0f, layout.header.bottom },
            { layout.window.right - 1.0f, layout.header.bottom }, g_brushMenuBorder, 0.7f);

        if (g_menuTitleFormat && g_brushMenuText)
            g_d2dTarget->DrawText(L"OVERLAY CONTROL", 15, g_menuTitleFormat,
                { layout.header.left + 16.0f, layout.header.top + 3.0f,
                  layout.header.left + 250.0f, layout.header.bottom - 2.0f },
                g_brushMenuText, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

        auto drawTab = [&](const D2D1_RECT_F& rect, const wchar_t* label, bool active)
            {
                const bool hovered = ContainsPoint(rect,
                    static_cast<float>(g_menuMouse.x), static_cast<float>(g_menuMouse.y));
                ID2D1SolidColorBrush* bg = active ? g_brushMenuPanel
                    : (hovered ? g_brushCheckboxBack : g_brushMenuBack);
                ID2D1SolidColorBrush* fg = active ? g_brushMenuText : g_brushMenuMuted;
                const D2D1_ROUNDED_RECT card{ rect, 6.0f, 6.0f };
                if (bg) g_d2dTarget->FillRoundedRectangle(&card, bg);
                if (g_smallTextFormat && fg)
                    g_d2dTarget->DrawText(label, static_cast<UINT32>(wcslen(label)), g_smallTextFormat,
                        { rect.left + 6.0f, rect.top + 1.0f, rect.right - 6.0f, rect.bottom - 1.0f },
                        fg, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                if (active && g_brushSliderFill)
                {
                    const D2D1_ROUNDED_RECT indicator{
                        { rect.left + 12.0f, rect.bottom - 3.0f, rect.right - 12.0f, rect.bottom - 1.0f },
                        1.0f, 1.0f
                    };
                    g_d2dTarget->FillRoundedRectangle(&indicator, g_brushSliderFill);
                }
            };
        drawTab(layout.tabAimbotHit, L"AIMBOT", g_activeTab == MenuTab::Aimbot);
        drawTab(layout.tabEspHit, L"ESP", g_activeTab == MenuTab::ESP);

        auto drawLabel = [&](const wchar_t* label, float y)
            {
                if (!g_menuTextFormat || !g_brushMenuText) return;
                g_d2dTarget->DrawText(label, static_cast<UINT32>(wcslen(label)), g_menuTextFormat,
                    { layout.window.left + 28.0f, y, layout.window.left + 325.0f, y + 22.0f },
                    g_brushMenuText, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        auto drawOnOff = [&](bool on, const D2D1_RECT_F& rect)
            {
                if (g_brushCheckboxBack) g_d2dTarget->FillRectangle(&rect, g_brushCheckboxBack);
                ID2D1SolidColorBrush* fg = on ? g_brushSliderFill : g_brushMenuMuted;
                if (g_brushMenuBorder) g_d2dTarget->DrawRectangle(rect, g_brushMenuBorder, 0.7f);
                const wchar_t* text = on ? L"ON" : L"OFF";
                if (g_menuTextFormat && fg)
                    g_d2dTarget->DrawText(text, static_cast<UINT32>(wcslen(text)), g_menuTextFormat,
                        { rect.left + 8.0f, rect.top + 2.0f, rect.right - 4.0f, rect.bottom - 1.0f },
                        fg, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        auto drawNumber = [&](float value, const wchar_t* suffix, const D2D1_RECT_F& rect)
            {
                wchar_t buf[32]{};
                if (suffix && wcscmp(suffix, L"%") == 0) swprintf_s(buf, L"%.0f%%", value);
                else if (suffix && wcscmp(suffix, L"x") == 0) swprintf_s(buf, L"%.2fx", value);
                else if (suffix && wcscmp(suffix, L"shots") == 0) swprintf_s(buf, L"%.0f shots", value);
                else if (suffix && wcscmp(suffix, L"sens") == 0) swprintf_s(buf, L"%.2f", value);
                else swprintf_s(buf, L"%.0f px", value);
                if (g_codeTextFormat && g_brushCodeValue)
                    g_d2dTarget->DrawText(buf, static_cast<UINT32>(wcslen(buf)), g_codeTextFormat,
                        { rect.left + 5.0f, rect.top, rect.right - 5.0f, rect.bottom },
                        g_brushCodeValue, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        auto drawHex = [&](float r, float g, float b, const D2D1_RECT_F& rect)
            {
                D2D1_RECT_F swatch{ rect.left + 2.0f, rect.top + 3.0f, rect.left + 15.0f, rect.bottom - 3.0f };
                if (g_brushBoxColor)
                {
                    g_brushBoxColor->SetColor(Color(r, g, b, 1.0f));
                    g_d2dTarget->FillRectangle(&swatch, g_brushBoxColor);
                }
                if (g_brushMenuBorder) g_d2dTarget->DrawRectangle(swatch, g_brushMenuBorder, 0.8f);
                wchar_t hex[16]{};
                swprintf_s(hex, L"#%02X%02X%02X",
                    static_cast<unsigned>(std::round(r * 255.0f)),
                    static_cast<unsigned>(std::round(g * 255.0f)),
                    static_cast<unsigned>(std::round(b * 255.0f)));
                if (g_codeTextFormat && g_brushCodeValue)
                    g_d2dTarget->DrawText(hex, static_cast<UINT32>(wcslen(hex)), g_codeTextFormat,
                        { rect.left + 21.0f, rect.top, rect.right - 2.0f, rect.bottom },
                        g_brushCodeValue, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        auto drawPosition = [&](Menu::BarPosition position, const D2D1_RECT_F& rect)
            {
                static const wchar_t* names[] = { L"Left", L"Right", L"Top", L"Bottom" };
                const int index = std::clamp(static_cast<int>(position), 0, 3);
                if (g_codeTextFormat && g_brushCodeValue)
                    g_d2dTarget->DrawText(names[index], static_cast<UINT32>(wcslen(names[index])), g_codeTextFormat,
                        { rect.left + 5.0f, rect.top, rect.right - 4.0f, rect.bottom },
                        g_brushCodeValue, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        auto drawFilledStyle = [&](Menu::FilledBoxStyle style, const D2D1_RECT_F& rect)
            {
                static const wchar_t* names[] = { L"Solid", L"Center fade", L"Top to bottom", L"Bottom to top" };
                const int index = std::clamp(static_cast<int>(style), 0, 3);
                if (g_codeTextFormat && g_brushCodeValue)
                    g_d2dTarget->DrawText(names[index], static_cast<UINT32>(wcslen(names[index])), g_codeTextFormat,
                        { rect.left + 5.0f, rect.top, rect.right - 4.0f, rect.bottom },
                        g_brushCodeValue, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            };
        g_d2dTarget->PushAxisAlignedClip(layout.contentClip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (g_activeTab == MenuTab::Aimbot)
        {
            // These are deliberately identical to the ESP rows: label on the left,
            // compact ON/OFF or value control in the same right-hand column.
            drawLabel(L"Enable aimbot", layout.aimEnabledHit.top);
            drawOnOff(AimConfig::enabled, layout.aimEnabledHit);
            drawLabel(L"Aim FOV", layout.aimFovHit.top);
            drawNumber(AimConfig::fovRadius, L"px", layout.aimFovHit);
            drawLabel(L"Lerp speed", layout.aimLerpSpeedHit.top);
            drawNumber(AimConfig::lerpSpeed, L"/s", layout.aimLerpSpeedHit);
            drawLabel(L"Sensitivity", layout.aimSensitivityHit.top);
            drawNumber(AimConfig::sensitivity, L"sens", layout.aimSensitivityHit);
            drawLabel(L"Recoil control", layout.rcsEnabledHit.top);
            drawOnOff(AimConfig::rcsEnabled, layout.rcsEnabledHit);
            drawLabel(L"RCS strength", layout.rcsStrengthHit.top);
            drawNumber(AimConfig::rcsStrength, L"x", layout.rcsStrengthHit);
            drawLabel(L"Deathmatch (target both teams)", layout.aimDeathmatchHit.top);
            drawOnOff(AimConfig::deathmatchMode, layout.aimDeathmatchHit);
        }
        else
        {
            auto drawRow = [&](const wchar_t* label, const D2D1_RECT_F& valueRect)
                {
                    drawLabel(label, valueRect.top);
                };

            drawRow(L"Deathmatch (show both teams)", layout.espDeathmatchHit); drawOnOff(AimConfig::deathmatchMode, layout.espDeathmatchHit);
            drawRow(L"Box ESP", layout.boxToggleHit); drawOnOff(Menu::box, layout.boxToggleHit);
            drawHex(Menu::boxColorR, Menu::boxColorG, Menu::boxColorB, layout.boxColorSwatchHit);
            drawRow(L"Filled boxes", layout.filledBoxToggleHit); drawOnOff(Menu::filledBox, layout.filledBoxToggleHit);
            drawHex(Menu::filledColorR, Menu::filledColorG, Menu::filledColorB, layout.filledColorSwatchHit);
            drawRow(L"Fill style", layout.filledBoxStyleHit); drawFilledStyle(Menu::filledBoxStyle, layout.filledBoxStyleHit);
            if (Menu::filledBoxStyle != Menu::FilledBoxStyle::Solid)
            {
                drawRow(L"Fill gradient color", layout.filledGradientColorSwatchHit);
                drawHex(Menu::filledGradientColorR, Menu::filledGradientColorG, Menu::filledGradientColorB, layout.filledGradientColorSwatchHit);
            }
            drawRow(L"Box corner length", layout.cornerValueHit); drawNumber(Menu::boxCornerLength, L"px", layout.cornerValueHit);
            drawRow(L"Health bar", layout.healthToggleHit); drawOnOff(Menu::health, layout.healthToggleHit);
            drawHex(Menu::healthColorR, Menu::healthColorG, Menu::healthColorB, layout.healthColorSwatchHit);
            drawRow(L"Health gradient", layout.healthGradientToggleHit); drawOnOff(Menu::healthGradient, layout.healthGradientToggleHit);
            if (Menu::healthGradient)
            {
                drawRow(L"Health gradient color", layout.healthGradientColorSwatchHit);
                drawHex(Menu::healthGradientColorR, Menu::healthGradientColorG, Menu::healthGradientColorB, layout.healthGradientColorSwatchHit);
            }
            drawRow(L"Health bar width", layout.healthValueHit); drawNumber(Menu::healthBarWidth, L"px", layout.healthValueHit);
            drawRow(L"Health bar position", layout.healthPositionHit); drawPosition(Menu::healthPosition, layout.healthPositionHit);
            drawRow(L"Armor bar", layout.armorToggleHit); drawOnOff(Menu::armor, layout.armorToggleHit);
            drawHex(Menu::armorColorR, Menu::armorColorG, Menu::armorColorB, layout.armorColorSwatchHit);
            drawRow(L"Armor bar width", layout.armorValueHit); drawNumber(Menu::armorBarWidth, L"px", layout.armorValueHit);
            drawRow(L"Armor bar position", layout.armorPositionHit); drawPosition(Menu::armorPosition, layout.armorPositionHit);
            drawRow(L"Snaplines", layout.snaplineToggleHit); drawOnOff(Menu::snapline, layout.snaplineToggleHit);
            drawHex(Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB, layout.snaplineColorSwatchHit);
            drawRow(L"Skeleton", layout.skeletonToggleHit); drawOnOff(Menu::skeleton, layout.skeletonToggleHit);
            drawHex(Menu::skeletonColorR, Menu::skeletonColorG, Menu::skeletonColorB, layout.skeletonColorSwatchHit);
            drawRow(L"Skeleton opacity", layout.skeletonOpacityValueHit); drawNumber(Menu::skeletonOpacity * 100.0f, L"%", layout.skeletonOpacityValueHit);
            drawRow(L"Player names", layout.playerNameToggleHit); drawOnOff(Menu::playerName, layout.playerNameToggleHit);
            drawHex(Menu::playerNameColorR, Menu::playerNameColorG, Menu::playerNameColorB, layout.playerNameColorSwatchHit);
            drawRow(L"Weapon icons", layout.weaponIconToggleHit); drawOnOff(Menu::weaponIcon, layout.weaponIconToggleHit);
            drawHex(Menu::weaponIconColorR, Menu::weaponIconColorG, Menu::weaponIconColorB, layout.weaponIconColorSwatchHit);
            drawRow(L"Distance", layout.distanceToggleHit); drawOnOff(Menu::distance, layout.distanceToggleHit);
            drawHex(Menu::distanceColorR, Menu::distanceColorG, Menu::distanceColorB, layout.distanceColorSwatchHit);
            drawRow(L"Hitmarker", layout.hitMarkerToggleHit); drawOnOff(Menu::hitMarker, layout.hitMarkerToggleHit);
            drawRow(L"Hitmarker sound", layout.hitMarkerSoundToggleHit); drawOnOff(Menu::hitMarkerSound, layout.hitMarkerSoundToggleHit);
            drawRow(L"Hitmarker scale", layout.hitMarkerScaleValueHit); drawNumber(Menu::hitMarkerScale * 100.0f, L"%", layout.hitMarkerScaleValueHit);
            drawRow(L"Hitmarker volume", layout.hitMarkerVolumeValueHit); drawNumber(Menu::hitMarkerVolume * 100.0f, L"%", layout.hitMarkerVolumeValueHit);
            drawRow(L"Hitmarker color", layout.hitMarkerColorSwatchHit); drawHex(Menu::hitMarkerColorR, Menu::hitMarkerColorG, Menu::hitMarkerColorB, layout.hitMarkerColorSwatchHit);
            drawRow(L"Damage popup", layout.damagePopupToggleHit); drawOnOff(Menu::damagePopup, layout.damagePopupToggleHit);
            drawRow(L"Damage popup color", layout.damagePopupColorSwatchHit); drawHex(Menu::damagePopupColorR, Menu::damagePopupColorG, Menu::damagePopupColorB, layout.damagePopupColorSwatchHit);
            drawRow(L"Damage text size", layout.damagePopupScaleValueHit); drawNumber(Menu::damagePopupScale * 100.0f, L"%", layout.damagePopupScaleValueHit);
            drawRow(L"Damage text boldness", layout.damagePopupBoldnessValueHit); drawNumber(Menu::damagePopupBoldness * 100.0f, L"%", layout.damagePopupBoldnessValueHit);
            drawRow(L"Damage outline", layout.damagePopupOutlineToggleHit); drawOnOff(Menu::damagePopupOutline, layout.damagePopupOutlineToggleHit);
            drawRow(L"Damage outline color", layout.damagePopupOutlineColorSwatchHit);
            drawHex(Menu::damagePopupOutlineColorR, Menu::damagePopupOutlineColorG, Menu::damagePopupOutlineColorB, layout.damagePopupOutlineColorSwatchHit);
        }
        g_d2dTarget->PopAxisAlignedClip();

        if (g_activeTab == MenuTab::ESP && g_brushMenuBorder && g_brushSliderFill)
        {
            g_d2dTarget->FillRectangle(&layout.scrollTrack, g_brushMenuBorder);
            g_d2dTarget->FillRectangle(&layout.scrollThumb, g_brushSliderFill);
        }

        if (g_codeTextFormat && g_brushMenuMuted)
        {
            const wchar_t* footer = (g_activeTab == MenuTab::Aimbot)
                ? L"Aim and recoil settings  |  changes save automatically"
                : L"Click a toggle or color  |  drag values  |  scroll for more";
            g_d2dTarget->DrawText(footer, static_cast<UINT32>(wcslen(footer)), g_codeTextFormat,
                { layout.window.left + 24.0f, layout.window.bottom - 24.0f,
                  layout.window.right - 14.0f, layout.window.bottom - 8.0f },
                g_brushMenuMuted, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    bool RenderMenuCache() noexcept
    {
        if (!Menu::menuOpen) return true;
        if (g_menuBitmap && !g_menuCacheDirty) return true;
        if (!g_d2dTarget || !g_menuBitmap) return false;

        // IMPORTANT: BuildMenuLayout initializes/clamps g_menuX/g_menuY.
        // Do this BEFORE creating the local-cache translation; otherwise the
        // first render translates by (0,0), then DrawMenu() centers the menu
        // in screen coordinates and paints it outside this small bitmap.
        const MenuLayout cacheLayout = BuildMenuLayout();

        ID2D1Image* previousTarget = nullptr;
        g_d2dTarget->GetTarget(&previousTarget);
        g_d2dTarget->SetTarget(g_menuBitmap);

        D2D1_MATRIX_3X2_F originalTransform{};
        g_d2dTarget->GetTransform(&originalTransform);
        g_d2dTarget->SetTransform(D2D1::Matrix3x2F::Identity());

        g_d2dTarget->BeginDraw();
        g_d2dTarget->Clear(Color(0.0f, 0.0f, 0.0f, 0.0f));

        // Translate from screen-space layout coordinates into the local
        // menu bitmap. Use the same layout built above, not unseeded
        // g_menuX/g_menuY values from the first frame.
        const D2D1_MATRIX_3X2_F localTransform{
            1.0f, 0.0f, 0.0f, 1.0f,
            -cacheLayout.window.left, -cacheLayout.window.top
        };
        g_d2dTarget->SetTransform(localTransform);
        DrawMenu();
        g_d2dTarget->SetTransform(originalTransform);

        const HRESULT hr = g_d2dTarget->EndDraw();

        if (previousTarget)
            g_d2dTarget->SetTarget(previousTarget);
        else
            g_d2dTarget->SetTarget(g_swapChainBitmap);
        SafeRelease(previousTarget);

        if (FAILED(hr))
        {
            if (hr == D2DERR_RECREATE_TARGET)
            {
                ReleaseDrawingResources();
                CreateDrawingResources();
            }
            return false;
        }

        g_menuCacheDirty = false;
        return true;
    }

    void PollOneKey(UINT key) noexcept
    {
        if (key >= 256U) return;
        const SHORT keyState = GetAsyncKeyState(static_cast<int>(key));
        const bool down = (keyState & 0x8000) != 0;
        // Use a clean high-bit transition. The low bit is shared/volatile and
        // can be consumed by another GetAsyncKeyState caller in the process.
        g_keyPressed[key] = down && !g_keyDown[key];
        g_keyDown[key] = down;
    }

    void PollKeyboardState() noexcept
    {
        for (int key = 0; key < 256; ++key) g_keyPressed[key] = false;
        if (g_keyCaptureTarget != KeyCaptureTarget::None)
        {
            for (UINT key = 0x01; key <= 0xFE; ++key) PollOneKey(key);
            return;
        }
        PollOneKey(g_menuToggleBind.key);
        PollOneKey(g_exitBind.key);
        constexpr UINT modifiers[] = { VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
        for (UINT key : modifiers) PollOneKey(key);
    }

    UINT GetHeldModifiers() noexcept
    {
        UINT modifiers = 0;
        if (g_keyDown[VK_CONTROL] || g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL]) modifiers |= MOD_CONTROL;
        if (g_keyDown[VK_SHIFT] || g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT]) modifiers |= MOD_SHIFT;
        if (g_keyDown[VK_MENU] || g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU]) modifiers |= MOD_ALT;
        if (g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN]) modifiers |= MOD_WIN;
        return modifiers;
    }

    bool IsModifierKey(UINT key) noexcept
    {
        return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
            key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
            key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
            key == VK_LWIN || key == VK_RWIN;
    }

    void CaptureHotkeyIfPressed() noexcept
    {
        if (g_keyCaptureTarget == KeyCaptureTarget::None) return;
        if (g_keyCaptureTarget == KeyCaptureTarget::AimActivation && g_aimCaptureWaitForRelease)
        {
            for (UINT key = 0x01; key <= 0xFE; ++key)
            {
                if (g_keyDown[key]) return;
            }
            g_aimCaptureWaitForRelease = false;
            return;
        }

        for (UINT key = 0x01; key <= 0xFE; ++key)
        {
            if (!g_keyPressed[key] || IsModifierKey(key) || key == VK_APPS)
                continue;
            const bool mouseButton = key == VK_LBUTTON || key == VK_RBUTTON ||
                key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2;
            if (mouseButton && g_keyCaptureTarget != KeyCaptureTarget::AimActivation)
                continue;

            if (g_keyCaptureTarget == KeyCaptureTarget::AimActivation)
            {
                AimConfig::activationKey = key;
                g_aimCaptureWaitForRelease = false;
                SaveConfigToAppData();
            }
            else
            {
                HotkeyBind bind{};
                bind.key = key;
                bind.modifiers = GetHeldModifiers();
                if (g_keyCaptureTarget == KeyCaptureTarget::MenuToggle) g_menuToggleBind = bind;
                else if (g_keyCaptureTarget == KeyCaptureTarget::ExitOverlay) g_exitBind = bind;
            }
            g_keyCaptureTarget = KeyCaptureTarget::None;
            g_menuCacheDirty = true;
            g_frameDirty = true;
            return;
        }
    }

    bool IsHotkeyPressed(const HotkeyBind& bind) noexcept
    {
        if (!bind.key || bind.key >= 256 || !g_keyPressed[bind.key]) return false;
        // If bind has no modifiers, ignore accidental shift/ctrl held by the game
        if (bind.modifiers == 0)
            return true;
        return GetHeldModifiers() == bind.modifiers;
    }

    void UnregisterMenuHotkey() noexcept
    {
        if (g_menuHotkeyRegistered)
            UnregisterHotKey(g_hwnd, kMenuHotkeyId);
        g_menuHotkeyRegistered = false;
        g_registeredMenuKey = 0;
        g_registeredMenuModifiers = 0;
    }

    void SyncMenuHotkeyRegistration() noexcept
    {
        // Bind the shortcut to the overlay HWND. Hidden windows still receive
        // WM_HOTKEY; the WndProc is the single menu-toggle event source.

        // Temporarily unregister while the user is choosing a replacement key.
        if (g_keyCaptureTarget != KeyCaptureTarget::None)
        {
            UnregisterMenuHotkey();
            return;
        }

        const UINT key = g_menuToggleBind.key;
        const UINT modifiers = g_menuToggleBind.modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
        if (g_menuHotkeyRegistered && key == g_registeredMenuKey && modifiers == g_registeredMenuModifiers)
            return;

        UnregisterMenuHotkey();
        if (!key || key >= 256U) return;

        if (g_hwnd && RegisterHotKey(g_hwnd, kMenuHotkeyId, modifiers | MOD_NOREPEAT, key))
        {
            g_menuHotkeyRegistered = true;
            g_registeredMenuKey = key;
            g_registeredMenuModifiers = modifiers;
            Log(L"Window-bound menu hotkey registered; WM_HOTKEY is handled by OverlayWndProc.");
        }
        else
        {
            // The message-loop edge sampler remains active even if another
            // process owns this global shortcut.
            LogLastError(L"RegisterHotKey(menu toggle); sampled key fallback remains enabled");
        }
    }

    bool IsExpectedHostExecutable() noexcept
    {
        wchar_t imagePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, imagePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        const wchar_t* fileName = wcsrchr(imagePath, L'\\');
        fileName = fileName ? fileName + 1 : imagePath;
        return CompareStringOrdinal(fileName, -1, L"cs2.exe", -1, TRUE) == CSTR_EQUAL;
    }

    DWORD GetWindowProcessIdSafe(HWND window) noexcept
    {
        if (!window || !IsWindow(window)) return 0;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        return processId;
    }

    bool IsTargetGameForeground() noexcept
    {
        // The validated host process and its client module define the lifetime
        // of this overlay. Never keep it alive after either is unavailable.
        if (!g_hostProcessValidated || !GetModuleHandleW(L"client.dll"))
            return false;

        const ULONGLONG now = GetTickCount64();
        const HWND foreground = GetForegroundWindow();
        if (foreground)
        {
            const DWORD foregroundPid = GetWindowProcessIdSafe(foreground);
            HWND root = GetAncestor(foreground, GA_ROOTOWNER);
            if (!root) root = GetAncestor(foreground, GA_ROOT);
            const DWORD rootPid = GetWindowProcessIdSafe(root);
            if (foregroundPid == g_hostProcessId || rootPid == g_hostProcessId)
            {
                g_lastTargetForegroundTime = now;
                return true;
            }
        }

        if (g_lastTargetForegroundTime != 0 &&
            now - g_lastTargetForegroundTime <= kForegroundTransitionGraceMs)
            return true;

        return false;
    }

    void ApplyMenuInputMode(bool requestFocus, bool forcePassive) noexcept
    {
        if (!g_hwnd || !IsWindow(g_hwnd))
            return;

        // This is the only function that mutates the overlay's input-related
        // extended styles. A pending menu-open remains passive until its first
        // frame has rendered successfully.
        //
        // Click-through needs WS_EX_TRANSPARENT. WS_EX_LAYERED improves
        // hit-testing reliability with DirectComposition + NOREDIRECTIONBITMAP.
        // WS_EX_NOACTIVATE stops the overlay from stealing activation while ESP
        // is visible with the menu closed.
        const bool interactive = Menu::menuOpen && !g_menuPresentationPending && !forcePassive;
        constexpr LONG_PTR kClickThroughFlags =
            static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED);

        LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        LONG_PTR desiredStyle = style;
        if (interactive)
        {
            // Menu must receive mouse input: drop transparent/noactivate only.
            // Keep WS_EX_LAYERED; it does not block client hits once TRANSPARENT is gone.
            desiredStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
            desiredStyle |= WS_EX_LAYERED;
        }
        else
        {
            desiredStyle |= kClickThroughFlags;
        }

        if (desiredStyle != style)
        {
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR previous = SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, desiredStyle);
            if (previous == 0 && GetLastError() != ERROR_SUCCESS)
                LogLastError(L"SetWindowLongPtrW(GWL_EXSTYLE)");
        }

        const LONG_PTR applied = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        const bool hasClickThrough =
            (applied & static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) ==
            static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        if (interactive == hasClickThrough)
            Log(L"Overlay input style did not match the requested menu mode.");

        if (interactive)
        {
            // Interactive mode: styles already allow hits; raise/show and
            // activate so the menu HWND receives mouse input.
            ShowWindow(g_hwnd, SW_SHOW);
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_width, g_height,
                SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            if (requestFocus && !FocusMenuWindow())
                Log(L"Menu is interactive, but foreground activation was not confirmed.");
        }
        else
        {
            // Passive / click-through: never activate, release any capture, keep
            // ESP topmost. SWP_FRAMECHANGED is required after style changes so
            // WS_EX_TRANSPARENT takes effect for hit-testing.
            if (GetCapture() == g_hwnd)
                ReleaseCapture();
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            if (requestFocus && !FocusGameProcessWindow())
                Log(L"Menu closed; game-window focus hand-off was not confirmed.");
        }

        CommitComposition();
    }

    void DeactivateOverlayForFocusLoss() noexcept
    {
        // Preserve the user's requested menu state, but force the hidden HWND
        // into passive mode so it cannot intercept desktop input while the game
        // is not foreground. Reopening the game will present the menu again.
        if (GetCapture() == g_hwnd) ReleaseCapture();
        g_activeSlider = ActiveSlider::None;
        g_menuDragging = false;
        g_keyCaptureTarget = KeyCaptureTarget::None;
        g_aimCaptureWaitForRelease = false;
        g_aimActivationWasHeld = false;
        g_colorPickerOpen = false;
        g_menuCacheDirty = true;
        g_frameDirty = true;
        g_hasLastEntityFrame = false;
        ApplyMenuInputMode(false, true);
        if (g_hwnd && IsWindow(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
        CommitComposition();
        g_overlayWindowVisible = false;
        g_targetWindowActive = false;
        g_lastMenuVisible = false;
        g_menuPresentationPending = Menu::menuOpen;
        Log(L"Target loss: overlay hidden in passive mode; requested menu state preserved.");
    }

    bool UpdateOverlayTargetVisibility() noexcept
    {
        const bool active = IsTargetGameForeground();
        if (!active)
        {
            if (g_targetWindowActive || g_overlayWindowVisible || !g_initialTargetStateChecked)
                DeactivateOverlayForFocusLoss();
            g_initialTargetStateChecked = true;
            return false;
        }
        // The first menu frame must be presented before the menu HWND becomes
        // interactive. PresentPendingMenuOpen() completes this transition.
        if (Menu::menuOpen && g_menuPresentationPending)
            return true;

        if (!g_targetWindowActive || !g_overlayWindowVisible)
        {
            g_targetWindowActive = true;
            g_overlayWindowVisible = true;
            g_initialTargetStateChecked = true;
            if (g_hwnd && IsWindow(g_hwnd))
            {
                // Reveal without activation first. If the menu is open, its
                // pending presentation below will activate it after a frame.
                ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
                SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_width, g_height,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
                CommitComposition();
            }
            ApplyMenuInputMode(Menu::menuOpen && !g_menuPresentationPending, false);
            g_frameDirty = true;
            g_menuCacheDirty = true;
            g_hasLastEntityFrame = false;
            g_lastMenuVisible = !Menu::menuOpen;
        }
        return true;
    }

    void ToggleMenuFromHotkey() noexcept
    {
        if (!g_hostProcessValidated || !GetModuleHandleW(L"client.dll"))
        {
            Log(L"Menu hotkey ignored: host process or client.dll is unavailable.");
            return;
        }
        if (g_keyCaptureTarget != KeyCaptureTarget::None)
            return;

        const ULONGLONG now = GetTickCount64();
        // WM_HOTKEY and a polling fallback should never produce two transitions
        // from one physical key press. Ignore duplicate edges within 180 ms.
        if (g_lastMenuToggleTime != 0 && now - g_lastMenuToggleTime < 180ULL)
            return;

        const bool opening = !Menu::menuOpen;
        if (opening && !IsTargetGameForeground())
        {
            Log(L"Menu hotkey ignored because the game is not the foreground application.");
            return;
        }
        Log(opening ? L"Menu toggle: CLOSED -> OPEN." : L"Menu toggle: OPEN -> CLOSED.");

        // Commit the logical transition once. Opening remains passive until the
        // first complete frame is presented; closing is immediate.
        Menu::menuOpen = opening;
        if (opening) g_aimActivationWasHeld = false;
        g_menuPresentationPending = opening;
        g_lastMenuToggleTime = now;
        g_lastTargetForegroundTime = now;
        g_menuCacheDirty = true;
        g_frameDirty = true;
        g_lastMenuVisible = !opening;
        g_activeSlider = ActiveSlider::None;
        g_menuDragging = false;
        g_colorPickerOpen = false;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();

        if (!opening)
        {
            // Close immediately: make the still-visible ESP window passive first,
            // then return foreground focus to the game's own main HWND.
            ApplyMenuInputMode(true, false);
            if (g_hwnd && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd))
            {
                g_frameDirty = true;
                if (!RenderFrame())
                    Log(L"Menu-close presentation will retry on the next frame.");
            }
            Log(L"Menu CLOSED; game focus hand-off requested.");
            return;
        }

        // Do not reset target/visibility bookkeeping here. That created an
        // unnecessary hide/show cycle while the first frame was being prepared.
        Log(L"Menu OPEN pending first complete frame.");
    }

    bool RenderFrame() noexcept
    {
        if (!g_d2dTarget || !g_swapChain || !g_swapChainBitmap) return false;

        EntityFrame currentFrame{};
        BuildEntityFrame(currentFrame);
        // Aimbot/RCS now runs in the supplied standalone worker thread.
        const bool hitMarkerActive = UpdateHitMarkerState(currentFrame);
        const bool hitMarkerVisibilityChanged = (hitMarkerActive != g_lastHitMarkerVisible);
        bool entitiesChanged = !g_hasLastEntityFrame || !SameEntityFrame(currentFrame, g_lastEntityFrame);
        if (!entitiesChanged)
        {
            for (const auto& e : currentFrame)
            {
                if (e.valid && (std::fabs(e.displayHealth - static_cast<float>(e.health)) > 0.15f ||
                    std::fabs(e.displayArmor - static_cast<float>(e.armor)) > 0.15f))
                {
                    entitiesChanged = true;
                    break;
                }
            }
        }
        const bool menuVisibilityChanged = (Menu::menuOpen != g_lastMenuVisible);
        const bool menuContentChanged = Menu::menuOpen && g_menuCacheDirty;
        const bool animatedVisualsActive =
            (Menu::box && Menu::filledBox && Menu::filledBoxStyle != Menu::FilledBoxStyle::Solid) ||
            (Menu::health && Menu::healthGradient) ||
            Menu::skeleton;

        bool damagePopupActive = false;
        const ULONGLONG popupNow = GetTickCount64();
        const ULONGLONG popupDurationNow = kDamagePopupLifetimeMs;
        for (const auto& popup : g_damagePopups)
            if (popup.active && popupNow - popup.born < popupDurationNow) { damagePopupActive = true; break; }
        if (!g_frameDirty && !entitiesChanged && !menuVisibilityChanged && !menuContentChanged &&
            !hitMarkerVisibilityChanged && !hitMarkerActive && !damagePopupActive && !animatedVisualsActive)
            return true;

        if (Menu::menuOpen && !RenderMenuCache()) return false;

        g_d2dTarget->SetTarget(g_swapChainBitmap);
        const D2D1_MATRIX_3X2_F identity{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        g_d2dTarget->SetTransform(identity);

        static float lastBoxR = -1.0f, lastBoxG = -1.0f, lastBoxB = -1.0f;
        static float lastFillR = -1.0f, lastFillG = -1.0f, lastFillB = -1.0f, lastFillA = -1.0f;
        static float lastHealthR = -1.0f, lastHealthG = -1.0f, lastHealthB = -1.0f;
        static float lastHealthBackR = -1.0f, lastHealthBackG = -1.0f, lastHealthBackB = -1.0f;
        static float lastSnapR = -1.0f, lastSnapG = -1.0f, lastSnapB = -1.0f;

        if (lastBoxR != Menu::boxColorR || lastBoxG != Menu::boxColorG || lastBoxB != Menu::boxColorB)
        {
            if (g_brushAccent) g_brushAccent->SetColor(Color(Menu::boxColorR, Menu::boxColorG, Menu::boxColorB, 1.0f));
            lastBoxR = Menu::boxColorR; lastBoxG = Menu::boxColorG; lastBoxB = Menu::boxColorB;
        }
        const float fillAlpha = std::clamp(Menu::filledOpacity, 0.05f, 1.0f);
        if (lastFillR != Menu::filledColorR || lastFillG != Menu::filledColorG || lastFillB != Menu::filledColorB || lastFillA != fillAlpha)
        {
            if (g_brushEntityFill) g_brushEntityFill->SetColor(Color(Menu::filledColorR, Menu::filledColorG, Menu::filledColorB, fillAlpha));
            lastFillR = Menu::filledColorR; lastFillG = Menu::filledColorG; lastFillB = Menu::filledColorB; lastFillA = fillAlpha;
        }
        if (lastHealthR != Menu::healthColorR || lastHealthG != Menu::healthColorG || lastHealthB != Menu::healthColorB)
        {
            const D2D1_COLOR_F c = Color(Menu::healthColorR, Menu::healthColorG, Menu::healthColorB, 1.0f);
            if (g_brushHealthHigh) g_brushHealthHigh->SetColor(c);
            if (g_brushHealthMed)  g_brushHealthMed->SetColor(c);
            if (g_brushHealthLow)  g_brushHealthLow->SetColor(c);
            lastHealthR = Menu::healthColorR; lastHealthG = Menu::healthColorG; lastHealthB = Menu::healthColorB;
        }
        if (lastHealthBackR != Menu::healthBackgroundColorR || lastHealthBackG != Menu::healthBackgroundColorG || lastHealthBackB != Menu::healthBackgroundColorB)
        {
            if (g_brushHealthBack) g_brushHealthBack->SetColor(Color(Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB, 0.92f));
            if (g_brushArmorBack) g_brushArmorBack->SetColor(Color(Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB, 0.92f));
            lastHealthBackR = Menu::healthBackgroundColorR; lastHealthBackG = Menu::healthBackgroundColorG; lastHealthBackB = Menu::healthBackgroundColorB;
        }
        static float lastArmorR = -1.0f, lastArmorG = -1.0f, lastArmorB = -1.0f;
        if (lastArmorR != Menu::armorColorR || lastArmorG != Menu::armorColorG || lastArmorB != Menu::armorColorB)
        {
            if (g_brushArmorColor) g_brushArmorColor->SetColor(Color(Menu::armorColorR, Menu::armorColorG, Menu::armorColorB, 1.0f));
            lastArmorR = Menu::armorColorR; lastArmorG = Menu::armorColorG; lastArmorB = Menu::armorColorB;
        }
        if (lastSnapR != Menu::snaplineColorR || lastSnapG != Menu::snaplineColorG || lastSnapB != Menu::snaplineColorB)
        {
            const D2D1_COLOR_F c = Color(Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB, 1.0f);
            if (g_brushSnaplineColor) g_brushSnaplineColor->SetColor(c);
            if (g_brushSnapline) g_brushSnapline->SetColor(c);
            lastSnapR = Menu::snaplineColorR; lastSnapG = Menu::snaplineColorG; lastSnapB = Menu::snaplineColorB;
        }
        static float lastSkeletonR = -1.0f, lastSkeletonG = -1.0f, lastSkeletonB = -1.0f, lastSkeletonA = -1.0f;
        if (lastSkeletonR != Menu::skeletonColorR || lastSkeletonG != Menu::skeletonColorG || lastSkeletonB != Menu::skeletonColorB || lastSkeletonA != Menu::skeletonOpacity)
        {
            if (g_brushSkeleton) g_brushSkeleton->SetColor(Color(Menu::skeletonColorR, Menu::skeletonColorG, Menu::skeletonColorB, Menu::skeletonOpacity));
            lastSkeletonR = Menu::skeletonColorR; lastSkeletonG = Menu::skeletonColorG; lastSkeletonB = Menu::skeletonColorB; lastSkeletonA = Menu::skeletonOpacity;
        }
        static float lastNameR = -1.0f, lastNameG = -1.0f, lastNameB = -1.0f;
        if (lastNameR != Menu::playerNameColorR || lastNameG != Menu::playerNameColorG || lastNameB != Menu::playerNameColorB)
        {
            if (g_brushPlayerName) g_brushPlayerName->SetColor(Color(Menu::playerNameColorR, Menu::playerNameColorG, Menu::playerNameColorB, 1.0f));
            lastNameR = Menu::playerNameColorR; lastNameG = Menu::playerNameColorG; lastNameB = Menu::playerNameColorB;
        }
        static float lastWeaponR = -1.0f, lastWeaponG = -1.0f, lastWeaponB = -1.0f;
        if (lastWeaponR != Menu::weaponIconColorR || lastWeaponG != Menu::weaponIconColorG || lastWeaponB != Menu::weaponIconColorB)
        {
            if (g_brushWeaponInfo) g_brushWeaponInfo->SetColor(Color(Menu::weaponIconColorR, Menu::weaponIconColorG, Menu::weaponIconColorB, 1.0f));
            lastWeaponR = Menu::weaponIconColorR; lastWeaponG = Menu::weaponIconColorG; lastWeaponB = Menu::weaponIconColorB;
        }
        static float lastDistanceR = -1.0f, lastDistanceG = -1.0f, lastDistanceB = -1.0f;
        if (lastDistanceR != Menu::distanceColorR || lastDistanceG != Menu::distanceColorG || lastDistanceB != Menu::distanceColorB)
        {
            if (g_brushDistance) g_brushDistance->SetColor(Color(Menu::distanceColorR, Menu::distanceColorG, Menu::distanceColorB, 1.0f));
            lastDistanceR = Menu::distanceColorR; lastDistanceG = Menu::distanceColorG; lastDistanceB = Menu::distanceColorB;
        }

        g_d2dTarget->BeginDraw();
        g_d2dTarget->Clear(Color(0.0f, 0.0f, 0.0f, 0.0f));
        DrawEntityFrame(currentFrame);
        if (hitMarkerActive) DrawHitMarker();
        DrawDamagePopups();

        if (Menu::menuOpen && g_menuBitmap)
        {
            const D2D1_RECT_F dest{ g_menuX, g_menuY, g_menuX + static_cast<float>(kMenuSurfaceWidth), g_menuY + static_cast<float>(kMenuSurfaceHeight) };
            g_d2dTarget->DrawBitmap(g_menuBitmap, &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        }

        const HRESULT hr = g_d2dTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            Log(L"Direct2D target lost; rebuilding.");
            ReleaseDrawingResources();
            if (!CreateDrawingResources()) return false;
            g_frameDirty = true;
            g_menuCacheDirty = true;
            return false;
        }
        if (FAILED(hr)) { LogHr(L"EndDraw", hr); return false; }

        const HRESULT presentHr = g_swapChain->Present(0, 0);
        if (FAILED(presentHr)) { LogHr(L"Present", presentHr); return false; }

        g_lastEntityFrame = currentFrame;
        g_hasLastEntityFrame = true;
        g_lastMenuVisible = Menu::menuOpen;
        g_lastHitMarkerVisible = hitMarkerActive;
        g_frameDirty = false;
        return true;
    }

    bool PresentPendingMenuOpen() noexcept
    {
        if (!g_menuPresentationPending || !Menu::menuOpen)
            return true;
        if (!g_hwnd || !IsWindow(g_hwnd) || !g_d2dTarget || !g_swapChain)
            return false;

        // Render while the window is still hidden/passive. If Direct2D needs
        // target recreation, keep the HWND passive and retry on the next loop.
        g_menuCacheDirty = true;
        g_frameDirty = true;
        if (!RenderMenuCache())
        {
            Log(L"Menu first-frame cache not ready; retrying.");
            return false;
        }
        if (!RenderFrame())
        {
            Log(L"Menu first-frame presentation not ready; retrying.");
            return false;
        }

        // Only after a fully presented frame do we remove the passive styles and
        // give the menu HWND input focus. ApplyMenuInputMode is the only style writer.
        g_targetWindowActive = true;
        g_overlayWindowVisible = true;
        g_initialTargetStateChecked = true;
        // Clear the pending gate immediately before applying interactive styles;
        // ApplyMenuInputMode deliberately keeps pending opens passive.
        g_menuPresentationPending = false;
        ApplyMenuInputMode(true, false);
        g_overlayWindowVisible = IsWindowVisible(g_hwnd) != FALSE;
        // IDCompositionVisual has no SetOpacity method; visuals default to opacity 1.0.
        if (g_dcompDevice) g_dcompDevice->Commit();

        g_frameDirty = false;
        Log(L"Menu OPEN presented; menu input focus enabled after first frame.");
        return true;
    }

    void Cleanup() noexcept
    {
        ReleaseDrawingResources();
        DestroyCompositionResources();
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_HOTKEY:
            if (wParam == static_cast<WPARAM>(kMenuHotkeyId))
            {
                Log(L"WM_HOTKEY received by overlay window.");
                ToggleMenuFromHotkey();
                return 0;
            }
            break;
        case WM_NCHITTEST:
            // Menu-open mode must be a genuine client hit target. When closed
            // (or still waiting on the first presented frame), pass hit testing
            // through as a second safeguard on top of WS_EX_TRANSPARENT.
            return (Menu::menuOpen && !g_menuPresentationPending) ? HTCLIENT : HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            if (Menu::menuOpen && !g_menuPresentationPending)
            {
                // MA_ACTIVATE activates the menu without discarding the click.
                return MA_ACTIVATE;
            }
            return MA_NOACTIVATE;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_SETCURSOR:
            if (Menu::menuOpen && !g_menuPresentationPending)
                return DefWindowProcW(hwnd, message, wParam, lParam);
            // Passive mode is click-through; never replace the game's cursor.
            return TRUE;
        case WM_LBUTTONDOWN:
        {
            // Ignore all mouse input while passive / pending presentation so a
            // stray message cannot capture the cursor or block the game.
            if (!Menu::menuOpen || g_menuPresentationPending) return 0;
            g_menuCacheDirty = true;
            g_frameDirty = true;

            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            g_menuMouse = { static_cast<LONG>(mx), static_cast<LONG>(my) };
            const MenuLayout layout = BuildMenuLayout();

            if (ContainsPoint(layout.tabAimbotHit, mx, my))
            {
                g_activeTab = MenuTab::Aimbot;
                g_menuScrollOffset = 0.0f;
                g_activeSlider = ActiveSlider::None;
                g_menuCacheDirty = g_frameDirty = true;
                return 0;
            }
            if (ContainsPoint(layout.tabEspHit, mx, my))
            {
                g_activeTab = MenuTab::ESP;
                g_menuScrollOffset = 0.0f;
                g_activeSlider = ActiveSlider::None;
                g_menuCacheDirty = g_frameDirty = true;
                return 0;
            }

            if (ContainsPoint(layout.headerDragHit, mx, my))
            {
                g_menuDragging = true;
                g_dragOffsetX = mx - layout.window.left;
                g_dragOffsetY = my - layout.window.top;
                SetCapture(hwnd);
                return 0;
            }

            if (g_activeTab == MenuTab::Aimbot)
            {
                auto updateAimSlider = [&](ActiveSlider slider, const D2D1_RECT_F& rect,
                    float minValue, float maxValue, auto&& setter)
                    {
                        g_activeSlider = slider;
                        SetCapture(hwnd);
                        const float t = std::clamp((mx - rect.left) / (rect.right - rect.left), 0.0f, 1.0f);
                        setter(minValue + t * (maxValue - minValue));
                        g_configDirtyDuringDrag = true;
                        g_menuCacheDirty = g_frameDirty = true;
                    };
                if (ContainsPoint(layout.aimEnabledHit, mx, my))
                {
                    AimConfig::enabled = !AimConfig::enabled;
                    SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
                }
                if (ContainsPoint(layout.aimFovHit, mx, my))
                {
                    updateAimSlider(ActiveSlider::AimFov, layout.aimFovHit, 20.0f, 400.0f,
                        [](float value) { AimConfig::fovRadius = value; });
                    return 0;
                }
                if (ContainsPoint(layout.aimLerpSpeedHit, mx, my))
                {
                    updateAimSlider(ActiveSlider::AimLerpSpeed, layout.aimLerpSpeedHit, 1.0f, 60.0f,
                        [](float value) { AimConfig::lerpSpeed = value; });
                    return 0;
                }
                if (ContainsPoint(layout.aimSensitivityHit, mx, my))
                {
                    updateAimSlider(ActiveSlider::AimSensitivity, layout.aimSensitivityHit, 0.10f, 10.0f,
                        [](float value) { AimConfig::sensitivity = std::round(value * 100.0f) / 100.0f; });
                    g_sensitivityDragStartX = mx;
                    g_sensitivityDragStartValue = AimConfig::sensitivity;
                    return 0;
                }
                if (ContainsPoint(layout.rcsEnabledHit, mx, my))
                {
                    AimConfig::rcsEnabled = !AimConfig::rcsEnabled;
                    ResetRcsSampleState();
                    SaveConfigToAppData();
                    g_menuCacheDirty = g_frameDirty = true;
                    return 0;
                }
                if (ContainsPoint(layout.aimDeathmatchHit, mx, my))
                {
                    AimConfig::deathmatchMode = !AimConfig::deathmatchMode;
                    SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true;
                    return 0;
                }
                if (ContainsPoint(layout.rcsStrengthHit, mx, my))
                {
                    updateAimSlider(ActiveSlider::RcsStrength, layout.rcsStrengthHit, 0.0f, 2.0f,
                        [](float value) { AimConfig::rcsStrength = std::round(value * 100.0f) / 100.0f; });
                    return 0;
                }
                return 0;
            }

            // ESP toggles
            if (ContainsPoint(layout.espDeathmatchHit, mx, my)) { AimConfig::deathmatchMode = !AimConfig::deathmatchMode; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.boxToggleHit, mx, my)) { Menu::box = !Menu::box; SaveConfigToAppData(); return 0; }
            if (ContainsPoint(layout.filledBoxToggleHit, mx, my)) { Menu::filledBox = !Menu::filledBox; SaveConfigToAppData(); return 0; }
            if (ContainsPoint(layout.healthToggleHit, mx, my)) { Menu::health = !Menu::health; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.armorToggleHit, mx, my)) { Menu::armor = !Menu::armor; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.snaplineToggleHit, mx, my)) { Menu::snapline = !Menu::snapline; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.skeletonToggleHit, mx, my)) { Menu::skeleton = !Menu::skeleton; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.playerNameToggleHit, mx, my)) { Menu::playerName = !Menu::playerName; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.weaponIconToggleHit, mx, my)) { Menu::weaponIcon = !Menu::weaponIcon; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.distanceToggleHit, mx, my)) { Menu::distance = !Menu::distance; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.hitMarkerToggleHit, mx, my)) { Menu::hitMarker = !Menu::hitMarker; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.hitMarkerSoundToggleHit, mx, my)) { Menu::hitMarkerSound = !Menu::hitMarkerSound; SaveConfigToAppData(); if (Menu::hitMarkerSound) PlayHitMarkerSound(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.damagePopupToggleHit, mx, my)) { Menu::damagePopup = !Menu::damagePopup; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.damagePopupOutlineToggleHit, mx, my)) { Menu::damagePopupOutline = !Menu::damagePopupOutline; SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0; }
            if (ContainsPoint(layout.filledBoxStyleHit, mx, my))
            {
                Menu::filledBoxStyle = static_cast<Menu::FilledBoxStyle>((static_cast<int>(Menu::filledBoxStyle) + 1) % 4);
                SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.healthGradientToggleHit, mx, my))
            {
                Menu::healthGradient = !Menu::healthGradient;
                SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.hitMarkerScaleValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::HitMarkerScale;
                SetCapture(hwnd);
                const float t = std::clamp((mx - layout.hitMarkerScaleValueHit.left) /
                    (layout.hitMarkerScaleValueHit.right - layout.hitMarkerScaleValueHit.left), 0.0f, 1.0f);
                Menu::hitMarkerScale = 0.35f + t * 0.65f;
                SaveConfigToAppData();
                g_menuCacheDirty = g_frameDirty = true;
                return 0;
            }
            if (ContainsPoint(layout.hitMarkerVolumeValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::HitMarkerVolume; SetCapture(hwnd);
                Menu::hitMarkerVolume = std::clamp((mx - layout.hitMarkerVolumeValueHit.left) / (layout.hitMarkerVolumeValueHit.right - layout.hitMarkerVolumeValueHit.left), 0.0f, 1.0f) * 2.0f;
                SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.damagePopupScaleValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::DamagePopupScale; SetCapture(hwnd);
                Menu::damagePopupScale = 0.65f + std::clamp((mx - layout.damagePopupScaleValueHit.left) / (layout.damagePopupScaleValueHit.right - layout.damagePopupScaleValueHit.left), 0.0f, 1.0f) * 2.35f;
                SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.damagePopupBoldnessValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::DamagePopupBoldness; SetCapture(hwnd);
                Menu::damagePopupBoldness = std::clamp((mx - layout.damagePopupBoldnessValueHit.left) /
                    (layout.damagePopupBoldnessValueHit.right - layout.damagePopupBoldnessValueHit.left), 0.0f, 1.0f);
                SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }

            // colors -> cycle next color in list (no popup)
            auto cycleColor = [&](float& r, float& g, float& b)
                {
                    static const float kPalette[][3] = {
                        { 1.00f, 1.00f, 1.00f }, // white
                        { 0.85f, 0.85f, 0.85f }, // light gray
                        { 0.55f, 0.55f, 0.55f }, // gray
                        { 0.25f, 0.25f, 0.25f }, // dark gray
                        { 0.00f, 0.00f, 0.00f }, // black
                        { 0.96f, 0.12f, 0.18f }, // red
                        { 1.00f, 0.30f, 0.30f }, // light red
                        { 1.00f, 0.45f, 0.10f }, // orange
                        { 1.00f, 0.70f, 0.20f }, // amber
                        { 1.00f, 0.85f, 0.15f }, // yellow
                        { 0.85f, 1.00f, 0.20f }, // lime
                        { 0.20f, 0.88f, 0.48f }, // green
                        { 0.10f, 0.70f, 0.35f }, // dark green
                        { 0.15f, 0.75f, 0.95f }, // cyan
                        { 0.20f, 0.55f, 1.00f }, // sky
                        { 0.25f, 0.45f, 1.00f }, // blue
                        { 0.15f, 0.20f, 0.80f }, // deep blue
                        { 0.55f, 0.30f, 1.00f }, // violet
                        { 0.70f, 0.30f, 1.00f }, // purple
                        { 1.00f, 0.35f, 0.70f }, // pink
                        { 0.90f, 0.20f, 0.55f }, // magenta
                        { 0.55f, 0.35f, 0.20f }, // brown
                        { 0.40f, 0.25f, 0.10f }, // dark brown
                        { 0.95f, 0.75f, 0.60f }, // tan
                    };
                    constexpr int n = 24;
                    int best = 0;
                    float bestD = 1e9f;
                    for (int i = 0; i < n; ++i)
                    {
                        const float dr = r - kPalette[i][0];
                        const float dg = g - kPalette[i][1];
                        const float db = b - kPalette[i][2];
                        const float d = dr * dr + dg * dg + db * db;
                        if (d < bestD) { bestD = d; best = i; }
                    }
                    const int next = (best + 1) % n;
                    r = kPalette[next][0];
                    g = kPalette[next][1];
                    b = kPalette[next][2];
                    SaveConfigToAppData();
                };
            if (ContainsPoint(layout.boxColorSwatchHit, mx, my))
            {
                cycleColor(Menu::boxColorR, Menu::boxColorG, Menu::boxColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.filledColorSwatchHit, mx, my))
            {
                cycleColor(Menu::filledColorR, Menu::filledColorG, Menu::filledColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (Menu::filledBoxStyle != Menu::FilledBoxStyle::Solid &&
                ContainsPoint(layout.filledGradientColorSwatchHit, mx, my))
            {
                cycleColor(Menu::filledGradientColorR, Menu::filledGradientColorG, Menu::filledGradientColorB);
                g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.healthColorSwatchHit, mx, my))
            {
                cycleColor(Menu::healthColorR, Menu::healthColorG, Menu::healthColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (Menu::healthGradient && ContainsPoint(layout.healthGradientColorSwatchHit, mx, my))
            {
                cycleColor(Menu::healthGradientColorR, Menu::healthGradientColorG, Menu::healthGradientColorB);
                g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.armorColorSwatchHit, mx, my))
            {
                cycleColor(Menu::armorColorR, Menu::armorColorG, Menu::armorColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.healthPositionHit, mx, my))
            {
                Menu::healthPosition = static_cast<Menu::BarPosition>((static_cast<int>(Menu::healthPosition) + 1) % 4); SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.armorPositionHit, mx, my))
            {
                Menu::armorPosition = static_cast<Menu::BarPosition>((static_cast<int>(Menu::armorPosition) + 1) % 4); SaveConfigToAppData(); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.snaplineColorSwatchHit, mx, my))
            {
                cycleColor(Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.skeletonColorSwatchHit, mx, my))
            {
                cycleColor(Menu::skeletonColorR, Menu::skeletonColorG, Menu::skeletonColorB); g_colorPickerOpen = false; g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.playerNameColorSwatchHit, mx, my))
            {
                cycleColor(Menu::playerNameColorR, Menu::playerNameColorG, Menu::playerNameColorB); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.weaponIconColorSwatchHit, mx, my))
            {
                cycleColor(Menu::weaponIconColorR, Menu::weaponIconColorG, Menu::weaponIconColorB);
                ReleaseWeaponIconBitmaps();
                g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.distanceColorSwatchHit, mx, my))
            {
                cycleColor(Menu::distanceColorR, Menu::distanceColorG, Menu::distanceColorB); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.hitMarkerColorSwatchHit, mx, my))
            {
                cycleColor(Menu::hitMarkerColorR, Menu::hitMarkerColorG, Menu::hitMarkerColorB); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.damagePopupColorSwatchHit, mx, my))
            {
                cycleColor(Menu::damagePopupColorR, Menu::damagePopupColorG, Menu::damagePopupColorB); g_menuCacheDirty = g_frameDirty = true; return 0;
            }
            if (ContainsPoint(layout.damagePopupOutlineColorSwatchHit, mx, my))
            {
                cycleColor(Menu::damagePopupOutlineColorR, Menu::damagePopupOutlineColorG,
                    Menu::damagePopupOutlineColorB);
                g_menuCacheDirty = g_frameDirty = true; return 0;
            }

            // numbers -> start slider drag, or wrap on simple click edges
            if (ContainsPoint(layout.cornerValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::BoxCorners;
                SetCapture(hwnd);
                // map x into 10..100
                const float t = std::clamp((mx - layout.cornerValueHit.left) /
                    (layout.cornerValueHit.right - layout.cornerValueHit.left), 0.0f, 1.0f);
                Menu::boxCornerLength = 10.0f + t * 90.0f;
                return 0;
            }
            if (ContainsPoint(layout.healthValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::HealthWidth;
                SetCapture(hwnd);
                const float t = std::clamp((mx - layout.healthValueHit.left) /
                    (layout.healthValueHit.right - layout.healthValueHit.left), 0.0f, 1.0f);
                Menu::healthBarWidth = 1.0f + t * 11.0f;
                return 0;
            }
            if (ContainsPoint(layout.armorValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::ArmorWidth;
                SetCapture(hwnd);
                const float t = std::clamp((mx - layout.armorValueHit.left) /
                    (layout.armorValueHit.right - layout.armorValueHit.left), 0.0f, 1.0f);
                Menu::armorBarWidth = 1.0f + t * 11.0f;
                return 0;
            }
            if (ContainsPoint(layout.skeletonOpacityValueHit, mx, my))
            {
                g_activeSlider = ActiveSlider::SkeletonOpacity;
                SetCapture(hwnd);
                const float t = std::clamp((mx - layout.skeletonOpacityValueHit.left) /
                    (layout.skeletonOpacityValueHit.right - layout.skeletonOpacityValueHit.left), 0.0f, 1.0f);
                Menu::skeletonOpacity = 0.05f + t * 0.95f;
                SaveConfigToAppData();
                g_menuCacheDirty = g_frameDirty = true;
                return 0;
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            if (!Menu::menuOpen || g_menuPresentationPending) return 0;
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &point);
            const MenuLayout layout = BuildMenuLayout();
            if (g_activeTab == MenuTab::Aimbot &&
                ContainsPoint(layout.aimSensitivityHit, static_cast<float>(point.x), static_cast<float>(point.y)))
            {
                const int wheel = GET_WHEEL_DELTA_WPARAM(wParam);
                if (wheel != 0)
                {
                    const float step = (wheel > 0) ? 0.01f : -0.01f;
                    AimConfig::sensitivity = std::clamp(
                        std::round((AimConfig::sensitivity + step) * 100.0f) / 100.0f,
                        0.10f, 10.00f);
                    SaveConfigToAppData();
                    g_menuCacheDirty = g_frameDirty = true;
                }
            }
            else if (g_activeTab == MenuTab::ESP &&
                ContainsPoint(layout.contentClip, static_cast<float>(point.x), static_cast<float>(point.y)))
            {
                const int wheel = GET_WHEEL_DELTA_WPARAM(wParam);
                if (wheel != 0)
                {
                    const float direction = wheel > 0 ? -1.0f : 1.0f;
                    g_menuScrollOffset = std::clamp(
                        g_menuScrollOffset + direction * kMenuScrollStep,
                        0.0f, kMenuMaxScroll);
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!Menu::menuOpen || g_menuPresentationPending) return 0;
            g_menuMouse.x = GET_X_LPARAM(lParam);
            g_menuMouse.y = GET_Y_LPARAM(lParam);
            const float mx = static_cast<float>(g_menuMouse.x);
            const float my = static_cast<float>(g_menuMouse.y);

            if (g_menuDragging && (wParam & MK_LBUTTON))
            {
                constexpr float menuWidth = 560.0f;
                constexpr float menuHeight = 440.0f;
                const float maxX = std::max(8.0f, static_cast<float>(g_width) - menuWidth - 8.0f);
                const float maxY = std::max(8.0f, static_cast<float>(g_height) - menuHeight - 8.0f);
                g_menuX = std::clamp(mx - g_dragOffsetX, 8.0f, maxX);
                g_menuY = std::clamp(my - g_dragOffsetY, 8.0f, maxY);
                g_menuCacheDirty = true;
                g_frameDirty = true;
            }
            else if (g_activeSlider != ActiveSlider::None && (wParam & MK_LBUTTON))
            {
                const MenuLayout layout = BuildMenuLayout();
                if (g_activeSlider == ActiveSlider::AimFov)
                {
                    const float t = std::clamp((mx - layout.aimFovHit.left) /
                        (layout.aimFovHit.right - layout.aimFovHit.left), 0.0f, 1.0f);
                    AimConfig::fovRadius = 20.0f + t * 380.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::AimLerpSpeed)
                {
                    const float t = std::clamp((mx - layout.aimLerpSpeedHit.left) /
                        (layout.aimLerpSpeedHit.right - layout.aimLerpSpeedHit.left), 0.0f, 1.0f);
                    AimConfig::lerpSpeed = 1.0f + t * 59.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::AimSensitivity)
                {
                    // Fine drag: one pixel changes sensitivity by exactly 0.01.
                    // The wheel over this control also changes it in 0.01 steps.
                    AimConfig::sensitivity = std::clamp(
                        std::round((g_sensitivityDragStartValue + (mx - g_sensitivityDragStartX) * 0.01f) * 100.0f) / 100.0f,
                        0.10f, 10.00f);
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::RcsStrength)
                {
                    const float t = std::clamp((mx - layout.rcsStrengthHit.left) /
                        (layout.rcsStrengthHit.right - layout.rcsStrengthHit.left), 0.0f, 1.0f);
                    AimConfig::rcsStrength = std::round(t * 2.0f * 100.0f) / 100.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::BoxCorners)
                {
                    const float t = std::clamp((mx - layout.cornerValueHit.left) /
                        (layout.cornerValueHit.right - layout.cornerValueHit.left), 0.0f, 1.0f);
                    Menu::boxCornerLength = 10.0f + t * 90.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::HealthWidth)
                {
                    const float t = std::clamp((mx - layout.healthValueHit.left) /
                        (layout.healthValueHit.right - layout.healthValueHit.left), 0.0f, 1.0f);
                    Menu::healthBarWidth = 1.0f + t * 11.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::ArmorWidth)
                {
                    const float t = std::clamp((mx - layout.armorValueHit.left) /
                        (layout.armorValueHit.right - layout.armorValueHit.left), 0.0f, 1.0f);
                    Menu::armorBarWidth = 1.0f + t * 11.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::SkeletonOpacity)
                {
                    const float t = std::clamp((mx - layout.skeletonOpacityValueHit.left) /
                        (layout.skeletonOpacityValueHit.right - layout.skeletonOpacityValueHit.left), 0.0f, 1.0f);
                    Menu::skeletonOpacity = 0.05f + t * 0.95f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::HitMarkerScale)
                {
                    const float t = std::clamp((mx - layout.hitMarkerScaleValueHit.left) /
                        (layout.hitMarkerScaleValueHit.right - layout.hitMarkerScaleValueHit.left), 0.0f, 1.0f);
                    Menu::hitMarkerScale = 0.35f + t * 0.65f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::HitMarkerVolume)
                {
                    Menu::hitMarkerVolume = std::clamp((mx - layout.hitMarkerVolumeValueHit.left) / (layout.hitMarkerVolumeValueHit.right - layout.hitMarkerVolumeValueHit.left), 0.0f, 1.0f) * 2.0f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::DamagePopupScale)
                {
                    const float t = std::clamp((mx - layout.damagePopupScaleValueHit.left) / (layout.damagePopupScaleValueHit.right - layout.damagePopupScaleValueHit.left), 0.0f, 1.0f);
                    Menu::damagePopupScale = 0.65f + t * 2.35f;
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::DamagePopupBoldness)
                {
                    Menu::damagePopupBoldness = std::clamp((mx - layout.damagePopupBoldnessValueHit.left) /
                        (layout.damagePopupBoldnessValueHit.right - layout.damagePopupBoldnessValueHit.left), 0.0f, 1.0f);
                    g_configDirtyDuringDrag = true;
                }
                else if (g_activeSlider == ActiveSlider::ColorSV || g_activeSlider == ActiveSlider::ColorHue)
                {
                    if (g_activeSlider == ActiveSlider::ColorSV)
                    {
                        g_pickerSaturation = std::clamp((mx - layout.colorSVHit.left) / (layout.colorSVHit.right - layout.colorSVHit.left), 0.0f, 1.0f);
                        g_pickerValue = 1.0f - std::clamp((my - layout.colorSVHit.top) / (layout.colorSVHit.bottom - layout.colorSVHit.top), 0.0f, 1.0f);
                    }
                    else
                    {
                        g_pickerHue = std::clamp((my - layout.colorHueHit.top) / (layout.colorHueHit.bottom - layout.colorHueHit.top), 0.0f, 1.0f);
                    }
                    float rr, gg, bb;
                    HSVToRGB(g_pickerHue, g_pickerSaturation, g_pickerValue, rr, gg, bb);
                    if (g_colorPickerTarget == ColorTarget::BoxOutline) { Menu::boxColorR = rr; Menu::boxColorG = gg; Menu::boxColorB = bb; }
                    else if (g_colorPickerTarget == ColorTarget::FilledBox) { Menu::filledColorR = rr; Menu::filledColorG = gg; Menu::filledColorB = bb; }
                    else if (g_colorPickerTarget == ColorTarget::HealthBar) { Menu::healthColorR = rr; Menu::healthColorG = gg; Menu::healthColorB = bb; }
                    else if (g_colorPickerTarget == ColorTarget::ArmorBar) { Menu::armorColorR = rr; Menu::armorColorG = gg; Menu::armorColorB = bb; }
                    else if (g_colorPickerTarget == ColorTarget::Snapline) { Menu::snaplineColorR = rr; Menu::snaplineColorG = gg; Menu::snaplineColorB = bb; }
                    g_configDirtyDuringDrag = true;
                }
                g_menuCacheDirty = true;
                g_frameDirty = true;
            }
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_configDirtyDuringDrag)
            {
                SaveConfigToAppData();
                g_configDirtyDuringDrag = false;
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            g_activeSlider = ActiveSlider::None;
            g_menuDragging = false;
            g_menuCacheDirty = true;
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: g_running.store(false); DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // Prevent multiple manually-mapped copies in the same CS2 process from
    // running independent hotkey/cursor state machines. Multiple copies are a
    // common cause of alternating cursor/menu behaviour.
    class ProcessOverlaySingleton final
    {
    public:
        ProcessOverlaySingleton() noexcept
        {
            wchar_t name[96]{};
            swprintf_s(name, L"Local\\AIInternalOverlay_%lu", GetCurrentProcessId());
            SetLastError(ERROR_SUCCESS);
            handle_ = CreateMutexW(nullptr, TRUE, name);
            if (!handle_)
            {
                LogLastError(L"CreateMutexW(overlay singleton)");
                return;
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                CloseHandle(handle_);
                handle_ = nullptr;
                Log(L"Another overlay instance owns the process singleton. This build did not start; fully exit CS2 before injecting a replacement.");
            }
        }

        ~ProcessOverlaySingleton() noexcept
        {
            if (handle_)
            {
                ReleaseMutex(handle_);
                CloseHandle(handle_);
                handle_ = nullptr;
            }
        }

        bool Acquired() const noexcept { return handle_ != nullptr; }

        ProcessOverlaySingleton(const ProcessOverlaySingleton&) = delete;
        ProcessOverlaySingleton& operator=(const ProcessOverlaySingleton&) = delete;

    private:
        HANDLE handle_ = nullptr;
    };

    namespace supplied_aim
    {
        struct v3 { float x, y, z; };
        struct vm { float m[16]; };
        struct utlv { uint64_t cnt, ptr; };

        namespace mem
        {
            uintptr_t clientBase = 0;
            template <typename T>
            bool tryRead(uintptr_t addr, T& out)
            {
                if (!addr || (addr % alignof(T)) != 0) return false;
                __try
                {
                    out = *reinterpret_cast<const T*>(addr);
                    return true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }
        }

        constexpr uintptr_t ENT_LIST = 0x270D5B0;
        constexpr uintptr_t VIEW_MAT = 0x255D3D0;
        constexpr uintptr_t LOCAL_PAWN = 0x2557AB8;
        constexpr uintptr_t OLD_ORIGIN = 0x14A4;
        constexpr uintptr_t HEALTH = 0x34C;
        constexpr uintptr_t TEAM = 0x3E7;
        constexpr uintptr_t SHOTS = 0x1EE8;
        constexpr uintptr_t AIM_PUNCH_SVC = 0x1598;
        constexpr uintptr_t PUNCH_CACHE = 0x88;
        constexpr uintptr_t SCENE_NODE = 0x330;
        constexpr uintptr_t DORMANT = 0x103;
        constexpr uintptr_t H_PLAYER_PAWN = 0x92C;
        constexpr uintptr_t VIEW_OFFSET = 0xF60;

        uintptr_t getPawnCached(
            uintptr_t list,
            uint32_t handle,
            std::array<uintptr_t, 64>& bucketCache,
            std::array<bool, 64>& bucketResolved) noexcept
        {
            const uint32_t entityIndex = handle & 0x7FFFu;
            const uint32_t bucketIndex = entityIndex >> 9;
            if (bucketIndex >= bucketCache.size())
                return 0;

            if (!bucketResolved[bucketIndex])
            {
                bucketResolved[bucketIndex] = true;
                if (!mem::tryRead<uintptr_t>(
                    list + 8u * bucketIndex + 16u, bucketCache[bucketIndex]))
                {
                    bucketCache[bucketIndex] = 0;
                }
            }

            const uintptr_t bucket = bucketCache[bucketIndex];
            if (!bucket)
                return 0;

            uintptr_t pawn = 0;
            if (!mem::tryRead<uintptr_t>(
                bucket + 112u * (entityIndex & 0x1FFu), pawn))
                return 0;
            return pawn;
        }

        bool w2s(const vm& mat, const v3& pos, int screenW, int screenH, float& x, float& y)
        {
            const float w = mat.m[12] * pos.x + mat.m[13] * pos.y + mat.m[14] * pos.z + mat.m[15];
            if (w < 0.01f) return false;
            const float iw = 1.f / w;
            const float halfW = screenW * 0.5f;
            const float halfH = screenH * 0.5f;
            x = halfW + (mat.m[0] * pos.x + mat.m[1] * pos.y + mat.m[2] * pos.z + mat.m[3]) * iw * halfW;
            y = halfH - (mat.m[4] * pos.x + mat.m[5] * pos.y + mat.m[6] * pos.z + mat.m[7]) * iw * halfH;
            return true;
        }

        void doMove(float dx, float dy, float& remainderX, float& remainderY) noexcept
        {
            if (!std::isfinite(dx) || !std::isfinite(dy))
                return;

            remainderX += dx;
            remainderY += dy;

            const LONG mx = static_cast<LONG>(std::lround(remainderX));
            const LONG my = static_cast<LONG>(std::lround(remainderY));
            if (mx == 0 && my == 0)
                return;

            remainderX -= static_cast<float>(mx);
            remainderY -= static_cast<float>(my);
            mouse_event(MOUSEEVENTF_MOVE, static_cast<DWORD>(mx), static_cast<DWORD>(my), 0, 0);
        }

        inline float ComputeAimLerpAlpha(float speed, float dt) noexcept
        {
            const float safeSpeed = std::clamp(std::isfinite(speed) ? speed : 24.0f, 1.0f, 60.0f);
            const float safeDt = std::clamp(std::isfinite(dt) ? dt : 0.001f, 0.0f, 0.05f);
            return 1.0f - std::exp(-safeSpeed * safeDt);
        }

        DWORD WINAPI InternalThread(LPVOID) noexcept
        {
            while (g_running.load(std::memory_order_relaxed) && !mem::clientBase)
            {
                mem::clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
                Sleep(250);
            }

            HWND gw = FindWindowW(L"Valve001", L"Counter-Strike 2");
            if (!gw) gw = FindWindowW(nullptr, L"Counter-Strike 2");

            v3 lastPunch{ 0.f, 0.f, 0.f };
            float rcsX = 0.f, rcsY = 0.f;
            float aimRemainderX = 0.0f, aimRemainderY = 0.0f;
            auto previousTick = std::chrono::steady_clock::now();
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            unsigned metricRefreshCounter = 0;
            if (screenW <= 0) screenW = 1920;
            if (screenH <= 0) screenH = 1080;

            while (g_running.load(std::memory_order_relaxed))
            {
                Sleep(1);
                const auto tickNow = std::chrono::steady_clock::now();
                const float dt = std::clamp(
                    std::chrono::duration<float>(tickNow - previousTick).count(),
                    0.0001f, 0.05f);
                previousTick = tickNow;
                if (!mem::clientBase)
                {
                    mem::clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
                    continue;
                }
                if (!gw || !IsWindow(gw))
                {
                    gw = FindWindowW(L"Valve001", L"Counter-Strike 2");
                    if (!gw) gw = FindWindowW(nullptr, L"Counter-Strike 2");
                }
                if (!gw || GetForegroundWindow() != gw || Menu::menuOpen)
                {
                    aimRemainderX = aimRemainderY = 0.0f;
                    continue;
                }
                if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                {
                    aimRemainderX = aimRemainderY = 0.0f;
                    continue;
                }
                if (!AimConfig::enabled && !AimConfig::rcsEnabled)
                {
                    aimRemainderX = aimRemainderY = 0.0f;
                    continue;
                }

                uintptr_t el = 0, lp = 0;
                vm vmat{};
                if (!mem::tryRead<uintptr_t>(mem::clientBase + ENT_LIST, el) || !el) continue;
                if (!mem::tryRead<uintptr_t>(mem::clientBase + LOCAL_PAWN, lp) || !lp) continue;
                uint8_t localTeam = 0;
                if (!mem::tryRead<uint8_t>(lp + TEAM, localTeam)) continue;
                if (!mem::tryRead<vm>(mem::clientBase + VIEW_MAT, vmat)) continue;

                if ((metricRefreshCounter++ & 0xFFu) == 0u)
                {
                    const int refreshedW = GetSystemMetrics(SM_CXSCREEN);
                    const int refreshedH = GetSystemMetrics(SM_CYSCREEN);
                    if (refreshedW > 0) screenW = refreshedW;
                    if (refreshedH > 0) screenH = refreshedH;
                }
                const float centerX = screenW * 0.5f;
                const float centerY = screenH * 0.5f;
                const float sens = std::clamp(AimConfig::sensitivity, 0.10f, 10.0f);
                const float fov = std::clamp(AimConfig::fovRadius, 1.0f, 400.0f);
                const float aimAlpha = ComputeAimLerpAlpha(AimConfig::lerpSpeed, dt);
                const float fovSquared = fov * fov;
                constexpr float rcsSm = 3.5f;
                const float rcsStrength = std::clamp(AimConfig::rcsStrength, 0.0f, 2.0f);

                int shots = 0;
                mem::tryRead<int>(lp + SHOTS, shots);
                float fdx = 0.f, fdy = 0.f;
                bool moved = false;

                if (AimConfig::enabled)
                {
                    // Entity indices 0..63 all use controller bucket zero.
                    // Resolve it once rather than performing the same pointer read 64 times.
                    uintptr_t controllerBucket = 0;
                    if (mem::tryRead<uintptr_t>(el + 16u, controllerBucket) && controllerBucket)
                    {
                        std::array<uintptr_t, 64> pawnBucketCache{};
                        std::array<bool, 64> pawnBucketResolved{};
                        float bestDistanceSquared = 1.0e30f;
                        float targetDx = 0.0f, targetDy = 0.0f;
                        uintptr_t bestTargetPawn = 0;

                        for (uint32_t i = 0; i < 64; ++i)
                        {
                            uintptr_t controller = 0;
                            if (!mem::tryRead<uintptr_t>(controllerBucket + 112u * i, controller) || !controller)
                                continue;

                            uint32_t handle = 0;
                            if (!mem::tryRead<uint32_t>(controller + H_PLAYER_PAWN, handle) ||
                                !handle || handle == 0xFFFFFFFFu)
                                continue;

                            const uintptr_t pawn = getPawnCached(el, handle, pawnBucketCache, pawnBucketResolved);
                            if (!pawn || pawn == lp)
                                continue;

                            int hp = 0;
                            if (!mem::tryRead<int>(pawn + HEALTH, hp) || hp <= 0 || hp > 100)
                                continue;

                            uint8_t targetTeam = 0;
                            if (!mem::tryRead<uint8_t>(pawn + TEAM, targetTeam))
                                continue;
                            if (!AimConfig::deathmatchMode && targetTeam == localTeam)
                                continue;

                            uintptr_t sceneNode = 0;
                            bool dormant = true;
                            if (!mem::tryRead<uintptr_t>(pawn + SCENE_NODE, sceneNode) || !sceneNode ||
                                !mem::tryRead<bool>(sceneNode + DORMANT, dormant) || dormant)
                                continue;

                            v3 origin{}, viewOffset{};
                            if (!mem::tryRead<v3>(pawn + OLD_ORIGIN, origin) ||
                                !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
                                !mem::tryRead<v3>(pawn + VIEW_OFFSET, viewOffset))
                                continue;

                            const v3 head{ origin.x + viewOffset.x, origin.y + viewOffset.y, origin.z + viewOffset.z };
                            float headX = 0.0f, headY = 0.0f;
                            if (!w2s(vmat, head, screenW, screenH, headX, headY))
                                continue;

                            const float dx = headX - centerX;
                            const float dy = headY - centerY;
                            const float distanceSquared = dx * dx + dy * dy;
                            if (distanceSquared <= fovSquared && distanceSquared < bestDistanceSquared)
                            {
                                bestDistanceSquared = distanceSquared;
                                targetDx = dx;
                                targetDy = dy;
                                bestTargetPawn = pawn;
                            }
                        }

                        if (bestTargetPawn)
                        {
                            fdx += targetDx * aimAlpha;
                            if (shots <= 1)
                                fdy += targetDy * aimAlpha;
                            moved = true;
                        }
                    }
                }

                if (AimConfig::rcsEnabled && shots > 1)
                {
                    uintptr_t aps = 0;
                    if (mem::tryRead<uintptr_t>(lp + AIM_PUNCH_SVC, aps) && aps)
                    {
                        utlv pc{};
                        if (mem::tryRead<utlv>(aps + PUNCH_CACHE, pc) && pc.cnt > 0 && pc.cnt < 0xFFFF && pc.ptr)
                        {
                            v3 ap{};
                            if (mem::tryRead<v3>(static_cast<uintptr_t>(pc.ptr) + (pc.cnt - 1) * sizeof(v3), ap))
                            {
                                if (ap.x != lastPunch.x || ap.y != lastPunch.y)
                                {
                                    const float pdx = (ap.x - lastPunch.x) * -1.f;
                                    const float pdy = (ap.y - lastPunch.y) * -1.f;
                                    const float cmx = (pdy * 2.f / sens) / -0.023f;
                                    const float cmy = (pdx * 2.f / sens) / 0.023f;
                                    rcsX += (cmx - rcsX) / rcsSm;
                                    rcsY += (cmy - rcsY) / rcsSm;
                                    fdx += rcsX * rcsStrength;
                                    fdy += rcsY * rcsStrength;
                                    lastPunch = { ap.x, ap.y, 0.f };
                                    moved = true;
                                }
                            }
                        }
                    }
                }
                else if (shots <= 1)
                {
                    lastPunch = { 0.f, 0.f, 0.f };
                    rcsX = 0.f;
                    rcsY = 0.f;
                }

                if (moved) doMove(fdx, fdy, aimRemainderX, aimRemainderY);
            }
            return 0;
        }
    }

    DWORD WINAPI OverlayThread(LPVOID) noexcept
    {
        ProcessOverlaySingleton singleton;
        if (!singleton.Acquired())
        {
            g_running.store(false);
            if (g_thread)
            {
                CloseHandle(g_thread);
                g_thread = nullptr;
            }
            return 20;
        }

        g_hostProcessId = GetCurrentProcessId();
        g_hostProcessValidated = IsExpectedHostExecutable();
        if (!g_hostProcessValidated)
        {
            Log(L"Overlay disabled: host is not cs2.exe.");
            g_running.store(false);
            return 10;
        }

        for (int attempt = 0; attempt < 300 && g_running.load() && !GetModuleHandleW(L"client.dll"); ++attempt)
            Sleep(50);
        if (!GetModuleHandleW(L"client.dll"))
        {
            Log(L"Overlay disabled: client.dll not loaded.");
            g_running.store(false);
            return 11;
        }

        // Seed focus timing before the first menu event can arrive.
        if (IsTargetGameForeground())
            g_lastTargetForegroundTime = GetTickCount64();

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInitialized = SUCCEEDED(comHr);

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        GetScreenSize();
        // Seed the draggable menu origin before the first hotkey/render cycle.
        // RenderMenuCache() also calls this defensively before its first draw.
        (void)BuildMenuLayout();
        srand(static_cast<unsigned>(time(nullptr)));
        LoadConfigFromAppData();
        HANDLE suppliedAimThread = nullptr;
        Menu::menuOpen = false; // closed on launch; game retains input focus
        g_menuCacheDirty = true;
        g_frameDirty = true;
        g_lastMenuVisible = false;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = g_module;
        // Native Windows arrow only; no custom cursor or cursor API calls.
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc))
        {
            LogLastError(L"RegisterClassExW");
            g_running.store(false);
            if (comInitialized) CoUninitialize();
            return 1;
        }

        // Start fully click-through. ApplyMenuInputMode keeps these bits in
        // sync when the menu opens/closes; creating without them left a window
        // that could still eat hits until the first style rewrite.
        g_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW |
            WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            kWindowClass, L"AI INTERNAL USE ONLY", WS_POPUP,
            0, 0, g_width, g_height, nullptr, nullptr, g_module, nullptr);
        if (!g_hwnd)
        {
            LogLastError(L"CreateWindowExW");
            UnregisterClassW(kWindowClass, g_module);
            g_running.store(false);
            if (comInitialized) CoUninitialize();
            return 2;
        }

        // Initialize the HWND's passive input state through the same centralized
        // function used for all later menu transitions. It does not show the window.
        ApplyMenuInputMode(false, true);

        if (!CreateCompositionResources(g_hwnd))
        {
            Log(L"Could not create composition resources.");
            Cleanup();
            DestroyWindow(g_hwnd); g_hwnd = nullptr;
            UnregisterClassW(kWindowClass, g_module);
            g_running.store(false);
            if (comInitialized) CoUninitialize();
            return 3;
        }

        if (!CreateDrawingResources())
        {
            Log(L"Could not create drawing resources.");
            Cleanup();
            DestroyWindow(g_hwnd); g_hwnd = nullptr;
            UnregisterClassW(kWindowClass, g_module);
            g_running.store(false);
            if (comInitialized) CoUninitialize();
            return 4;
        }

        // Register the shortcut on the overlay HWND. WM_HOTKEY is dispatched
        // by its WndProc even while the window itself is hidden.
        SyncMenuHotkeyRegistration();
        if (!g_menuHotkeyRegistered)
            Log(L"Menu hotkey uses polling fallback; RegisterHotKey was unavailable.");

        ShowWindow(g_hwnd, SW_HIDE);
        CommitComposition();
        g_overlayWindowVisible = false;
        g_targetWindowActive = false;
        Menu::menuOpen = false;
        g_initialTargetStateChecked = false;
        Log(L"CS2-bound DirectComposition overlay ready.");
        Log(g_menuHotkeyRegistered
            ? L"Menu shortcut is bound to the overlay HWND; expect one WM_HOTKEY per press."
            : L"Menu shortcut is using sampled key-state fallback.");

        suppliedAimThread = CreateThread(nullptr, 0, supplied_aim::InternalThread, nullptr, 0, nullptr);
        if (!suppliedAimThread) Log(L"Supplied aim worker could not be started.");

        MSG msg{};
        while (g_running.load())
        {
            // Drain input first. In particular, process the thread-queue F11
            // message before focus/visibility bookkeeping can hide the HWND.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) { g_running.store(false); break; }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!g_running.load()) break;

            // Focus/visibility is evaluated once after the input edge is processed
            // below, avoiding duplicate foreground/window queries every frame.

            // A settings click may have entered key-capture mode while pumping
            // messages. Unregister the menu shortcut before polling that key.
            SyncMenuHotkeyRegistration();

            // Keep keyboard edges current even while the overlay is hidden.
            // If RegisterHotKey is unavailable, this prevents the fallback
            // path from depending on the overlay visibility state.
            PollKeyboardState();

            if (g_keyCaptureTarget != KeyCaptureTarget::None)
            {
                CaptureHotkeyIfPressed();
                SyncMenuHotkeyRegistration();
            }
            else if (!g_menuHotkeyRegistered && IsHotkeyPressed(g_menuToggleBind))
            {
                // Use sampled key edges only when Windows could not register
                // the global shortcut. Never run both input paths at once.
                ToggleMenuFromHotkey();
            }
            else if (IsHotkeyPressed(g_exitBind) && IsTargetGameForeground())
            {
                const ULONGLONG now = GetTickCount64();
                if (g_endPressArmed && now - g_lastEndPressTime <= 1500ULL)
                {
                    g_running.store(false);
                    break;
                }
                g_endPressArmed = true;
                g_lastEndPressTime = now;
            }
            if (g_endPressArmed && GetTickCount64() - g_lastEndPressTime > 1500ULL)
                g_endPressArmed = false;

            if (!UpdateOverlayTargetVisibility())
            {
                MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }

            const int currentWidth = GetSystemMetrics(SM_CXSCREEN);
            const int currentHeight = GetSystemMetrics(SM_CYSCREEN);
            if (currentWidth > 0 && currentHeight > 0 && (currentWidth != g_width || currentHeight != g_height))
            {
                g_width = currentWidth;
                g_height = currentHeight;
                if (!ResizeCompositionSurface())
                {
                    Log(L"Failed to resize swap chain.");
                    g_running.store(false);
                    break;
                }
                g_hasLastEntityFrame = false;
                SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_width, g_height,
                    SWP_NOACTIVATE | (g_overlayWindowVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
                CommitComposition();
            }

            if (g_menuPresentationPending)
                PresentPendingMenuOpen();
            else
                RenderFrame();
            MsgWaitForMultipleObjectsEx(0, nullptr, kFrameWaitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        g_running.store(false, std::memory_order_relaxed);
        if (suppliedAimThread)
        {
            WaitForSingleObject(suppliedAimThread, 1500);
            CloseHandle(suppliedAimThread);
            suppliedAimThread = nullptr;
        }
        const bool gameStillForeground = IsTargetGameForeground();
        UnregisterMenuHotkey();
        Cleanup();
        if (gameStillForeground)
        {
            FocusGameProcessWindow();
        }
        else
        {
            // Do not override the desktop/game cursor state during shutdown.
        }
        if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(kWindowClass, g_module);
        g_running.store(false);
        Log(L"Overlay stopped.");
        if (comInitialized) CoUninitialize();
        return 0;
    }
}

extern "C" __declspec(dllexport) void StopOverlay()
{
    g_running.store(false);
    HWND hwnd = g_hwnd;
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    HANDLE thread = g_thread;
    if (thread && GetCurrentThreadId() != GetThreadId(thread))
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        g_thread = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        g_running.store(true);
        g_thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
        if (!g_thread)
        {
            g_running.store(false);
            OutputDebugStringW(L"[Overlay] CreateThread failed.\n");
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running.store(false);
    }
    return TRUE;
}