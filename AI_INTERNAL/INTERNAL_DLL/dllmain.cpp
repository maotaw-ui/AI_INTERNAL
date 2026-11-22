// AI MASTER - Direct2D overlay menu
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
#include <limits>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <string>
#include <type_traits>
#include <cstring>
#include <cctype>
#include <cwctype>
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
    enum class InfoPosition : int { Above, Below, Left, Right };
    inline float playerNameScale = 1.0f;
    inline float weaponIconScale = 1.0f;
    inline float distanceScale = 1.0f;
    inline InfoPosition playerNamePosition = InfoPosition::Above;
    inline InfoPosition weaponIconPosition = InfoPosition::Below;
    inline InfoPosition distancePosition = InfoPosition::Below;
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

    // Optional edge indicators for entities outside the current viewport.
    inline bool offscreenIndicators = false;
    inline float offscreenScale = 1.0f;
    inline float offscreenEdgeMargin = 42.0f;
    inline int offscreenAnimation = 0; // 0 = Normal, 1 = Pulse
    inline int offscreenPosition = 0;  // 0 = Center, 1 = Screen left, 2 = Screen right
    inline float offscreenColorR = 0.20f;
    inline float offscreenColorG = 0.88f;
    inline float offscreenColorB = 0.58f;

    // Compact world-space radar. Position is screen-space and can be dragged
    // from its dotted title-bar handle while the menu is open.
    inline bool radarEnabled = false;
    inline int radarShape = 0; // 0 = Circle, 1 = Rounded rectangle
    inline float radarSize = 184.0f; // full panel size in pixels
    inline float radarRangeMeters = 80.0f;
    inline float radarDotSize = 4.5f;
    inline float radarX = 24.0f;
    inline float radarY = 24.0f;
    inline float radarDotColorR = 0.96f;
    inline float radarDotColorG = 0.20f;
    inline float radarDotColorB = 0.24f;
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

    // Screen-space FOV visual. Radius is shared with the existing FOV slider (pixels).
    inline bool showFovCircle = false;
    inline int fovCircleAnimation = 0; // 0 = Normal, 1 = Pulse
    inline float fovCircleThickness = 1.5f;
    inline float fovCircleColorR = 0.20f;
    inline float fovCircleColorG = 0.88f;
    inline float fovCircleColorB = 0.58f;
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
    constexpr int kMenuSurfaceWidth = 642;  // AI MASTER menu canvas width
    constexpr int kMenuSurfaceHeight = 336; // AI MASTER menu canvas height
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
        bool offscreen = false;
        float offscreenDirectionX = 0.0f; // normalized screen-space direction from the viewport center
        float offscreenDirectionY = 0.0f;
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

    struct RadarEntityData
    {
        Vec3 worldOrigin{};
        uint8_t team = 0;
        bool valid = false;
    };
    std::array<RadarEntityData, 64> g_radarEntities{};
    size_t g_radarEntityCount = 0;
    Vec3 g_radarLocalOrigin{};
    float g_radarViewYaw = 0.0f;
    bool g_radarLocalValid = false;
    std::array<RadarEntityData, 64> g_lastRadarEntities{};
    size_t g_lastRadarEntityCount = 0;
    Vec3 g_lastRadarLocalOrigin{};
    float g_lastRadarViewYaw = 0.0f;
    bool g_lastRadarLocalValid = false;
    bool g_lastRadarEnabled = false;

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
    IDWriteTextFormat* g_aetherIconFormat = nullptr;
    IDWriteTextFormat* g_aetherLogoFormat = nullptr;

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
    enum class MenuTab { Aimbot, ESP, Settings, Misc };
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
    // Aether UI appearance settings. Appended to visuals.cfg to preserve its existing layout.
    int g_aetherAccentIndex = 1; // retained for migration from older config files
    float g_aetherOpacity = 0.92f;
    float g_aetherAccentR = 0.20f;
    float g_aetherAccentG = 0.88f;
    float g_aetherAccentB = 0.58f;
    float g_aetherPanelOpacity = 0.97f;
    float g_aetherBorderOpacity = 0.08f;
    bool g_aetherSearchOpen = false;
    bool g_aetherSearchFocused = false;
    std::wstring g_aetherSearchQuery;
    int g_aetherSearchSelection = 0;
    float g_sensitivityDragStartValue = 2.11f;
    float g_sensitivityDragStartX = 0.0f;
    MenuTab g_activeTab = MenuTab::Aimbot;
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
        fwrite(&g_aetherAccentIndex, sizeof(g_aetherAccentIndex), 1, f);
        fwrite(&g_aetherOpacity, sizeof(g_aetherOpacity), 1, f);
        // Appended custom accent RGB keeps previous visuals.cfg layouts readable.
        fwrite(&g_aetherAccentR, sizeof(g_aetherAccentR), 1, f);
        fwrite(&g_aetherAccentG, sizeof(g_aetherAccentG), 1, f);
        fwrite(&g_aetherAccentB, sizeof(g_aetherAccentB), 1, f);
        // Appended Aether app controls and ESP label presentation settings.
        fwrite(&g_aetherPanelOpacity, sizeof(g_aetherPanelOpacity), 1, f);
        fwrite(&g_aetherBorderOpacity, sizeof(g_aetherBorderOpacity), 1, f);
        fwrite(&g_menuToggleBind.key, sizeof(g_menuToggleBind.key), 1, f);
        fwrite(&g_menuToggleBind.modifiers, sizeof(g_menuToggleBind.modifiers), 1, f);
        fwrite(&g_exitBind.key, sizeof(g_exitBind.key), 1, f);
        fwrite(&g_exitBind.modifiers, sizeof(g_exitBind.modifiers), 1, f);
        fwrite(&Menu::playerNameScale, sizeof(Menu::playerNameScale), 1, f);
        fwrite(&Menu::weaponIconScale, sizeof(Menu::weaponIconScale), 1, f);
        fwrite(&Menu::distanceScale, sizeof(Menu::distanceScale), 1, f);
        // Legacy position fields retained for backwards-compatible config layout.
        const int namePosition = static_cast<int>(Menu::InfoPosition::Above);
        const int weaponPosition = static_cast<int>(Menu::InfoPosition::Below);
        const int distancePosition = static_cast<int>(Menu::InfoPosition::Below);
        fwrite(&namePosition, sizeof(namePosition), 1, f);
        fwrite(&weaponPosition, sizeof(weaponPosition), 1, f);
        fwrite(&distancePosition, sizeof(distancePosition), 1, f);

        // Appended FOV-circle and off-screen indicator settings.
        fwrite(&AimConfig::showFovCircle, sizeof(AimConfig::showFovCircle), 1, f);
        fwrite(&AimConfig::fovCircleAnimation, sizeof(AimConfig::fovCircleAnimation), 1, f);
        fwrite(&AimConfig::fovCircleThickness, sizeof(AimConfig::fovCircleThickness), 1, f);
        fwrite(&AimConfig::fovCircleColorR, sizeof(float), 1, f);
        fwrite(&AimConfig::fovCircleColorG, sizeof(float), 1, f);
        fwrite(&AimConfig::fovCircleColorB, sizeof(float), 1, f);
        fwrite(&Menu::offscreenIndicators, sizeof(Menu::offscreenIndicators), 1, f);
        fwrite(&Menu::offscreenScale, sizeof(Menu::offscreenScale), 1, f);
        fwrite(&Menu::offscreenEdgeMargin, sizeof(Menu::offscreenEdgeMargin), 1, f);
        fwrite(&Menu::offscreenAnimation, sizeof(Menu::offscreenAnimation), 1, f);
        fwrite(&Menu::offscreenColorR, sizeof(float), 1, f);
        fwrite(&Menu::offscreenColorG, sizeof(float), 1, f);
        fwrite(&Menu::offscreenColorB, sizeof(float), 1, f);
        // New presentation options are appended to preserve existing config layouts.
        fwrite(&Menu::offscreenPosition, sizeof(Menu::offscreenPosition), 1, f);
        // Radar settings are appended to preserve old AppData config layouts.
        fwrite(&Menu::radarEnabled, sizeof(Menu::radarEnabled), 1, f);
        fwrite(&Menu::radarShape, sizeof(Menu::radarShape), 1, f);
        fwrite(&Menu::radarSize, sizeof(Menu::radarSize), 1, f);
        fwrite(&Menu::radarRangeMeters, sizeof(Menu::radarRangeMeters), 1, f);
        fwrite(&Menu::radarDotSize, sizeof(Menu::radarDotSize), 1, f);
        fwrite(&Menu::radarX, sizeof(Menu::radarX), 1, f);
        fwrite(&Menu::radarY, sizeof(Menu::radarY), 1, f);
        fwrite(&Menu::radarDotColorR, sizeof(float), 1, f);
        fwrite(&Menu::radarDotColorG, sizeof(float), 1, f);
        fwrite(&Menu::radarDotColorB, sizeof(float), 1, f);
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
        // Optional Aether appearance values are appended for backward compatibility.
        rd(&g_aetherAccentIndex, sizeof(g_aetherAccentIndex));
        rd(&g_aetherOpacity, sizeof(g_aetherOpacity));
        const bool hasAccentR = rd(&g_aetherAccentR, sizeof(g_aetherAccentR));
        const bool hasAccentG = rd(&g_aetherAccentG, sizeof(g_aetherAccentG));
        const bool hasAccentB = rd(&g_aetherAccentB, sizeof(g_aetherAccentB));
        rd(&g_aetherPanelOpacity, sizeof(g_aetherPanelOpacity));
        rd(&g_aetherBorderOpacity, sizeof(g_aetherBorderOpacity));
        UINT savedMenuKey = g_menuToggleBind.key;
        UINT savedMenuModifiers = g_menuToggleBind.modifiers;
        UINT savedExitKey = g_exitBind.key;
        UINT savedExitModifiers = g_exitBind.modifiers;
        if (rd(&savedMenuKey, sizeof(savedMenuKey)) && savedMenuKey > 0 && savedMenuKey < 256)
            g_menuToggleBind.key = savedMenuKey;
        if (rd(&savedMenuModifiers, sizeof(savedMenuModifiers)))
            g_menuToggleBind.modifiers = savedMenuModifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
        if (rd(&savedExitKey, sizeof(savedExitKey)) && savedExitKey > 0 && savedExitKey < 256)
            g_exitBind.key = savedExitKey;
        if (rd(&savedExitModifiers, sizeof(savedExitModifiers)))
            g_exitBind.modifiers = savedExitModifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
        rd(&Menu::playerNameScale, sizeof(Menu::playerNameScale));
        rd(&Menu::weaponIconScale, sizeof(Menu::weaponIconScale));
        rd(&Menu::distanceScale, sizeof(Menu::distanceScale));
        int namePosition = static_cast<int>(Menu::playerNamePosition);
        int weaponPosition = static_cast<int>(Menu::weaponIconPosition);
        int distancePosition = static_cast<int>(Menu::distancePosition);
        if (rd(&namePosition, sizeof(namePosition)) && namePosition >= 0 && namePosition <= 3)
            Menu::playerNamePosition = static_cast<Menu::InfoPosition>(namePosition);
        if (rd(&weaponPosition, sizeof(weaponPosition)) && weaponPosition >= 0 && weaponPosition <= 3)
            Menu::weaponIconPosition = static_cast<Menu::InfoPosition>(weaponPosition);
        if (rd(&distancePosition, sizeof(distancePosition)) && distancePosition >= 0 && distancePosition <= 3)
            Menu::distancePosition = static_cast<Menu::InfoPosition>(distancePosition);
        // Legacy position fields stay in the file only to preserve its binary layout.
        // The ESP information layout is now fixed: name above, icon then distance below.
        Menu::playerNamePosition = Menu::InfoPosition::Above;
        Menu::weaponIconPosition = Menu::InfoPosition::Below;
        Menu::distancePosition = Menu::InfoPosition::Below;

        // Optional appended visual settings; defaults remain Normal / disabled for older configs.
        rd(&AimConfig::showFovCircle, sizeof(AimConfig::showFovCircle));
        rd(&AimConfig::fovCircleAnimation, sizeof(AimConfig::fovCircleAnimation));
        rd(&AimConfig::fovCircleThickness, sizeof(AimConfig::fovCircleThickness));
        rd(&AimConfig::fovCircleColorR, sizeof(float));
        rd(&AimConfig::fovCircleColorG, sizeof(float));
        rd(&AimConfig::fovCircleColorB, sizeof(float));
        rd(&Menu::offscreenIndicators, sizeof(Menu::offscreenIndicators));
        rd(&Menu::offscreenScale, sizeof(Menu::offscreenScale));
        rd(&Menu::offscreenEdgeMargin, sizeof(Menu::offscreenEdgeMargin));
        rd(&Menu::offscreenAnimation, sizeof(Menu::offscreenAnimation));
        rd(&Menu::offscreenColorR, sizeof(float));
        rd(&Menu::offscreenColorG, sizeof(float));
        rd(&Menu::offscreenColorB, sizeof(float));
        rd(&Menu::offscreenPosition, sizeof(Menu::offscreenPosition));
        // Optional radar values; defaults remain active for older config files.
        rd(&Menu::radarEnabled, sizeof(Menu::radarEnabled));
        rd(&Menu::radarShape, sizeof(Menu::radarShape));
        rd(&Menu::radarSize, sizeof(Menu::radarSize));
        rd(&Menu::radarRangeMeters, sizeof(Menu::radarRangeMeters));
        rd(&Menu::radarDotSize, sizeof(Menu::radarDotSize));
        rd(&Menu::radarX, sizeof(Menu::radarX));
        rd(&Menu::radarY, sizeof(Menu::radarY));
        rd(&Menu::radarDotColorR, sizeof(float));
        rd(&Menu::radarDotColorG, sizeof(float));
        rd(&Menu::radarDotColorB, sizeof(float));
        if (!(hasAccentR && hasAccentG && hasAccentB))
        {
            // Migrate the old five-preset accent selection to its matching RGB value.
            static constexpr float oldAccentPalette[5][3] = {
                { 0.91f, 0.91f, 0.94f },{ 0.20f, 0.88f, 0.58f },
                { 0.25f, 0.62f, 1.00f },{ 0.67f, 0.43f, 1.00f },
                { 1.00f, 0.62f, 0.25f }
            };
            const int oldAccent = std::clamp(g_aetherAccentIndex, 0, 4);
            g_aetherAccentR = oldAccentPalette[oldAccent][0];
            g_aetherAccentG = oldAccentPalette[oldAccent][1];
            g_aetherAccentB = oldAccentPalette[oldAccent][2];
        }
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
        AimConfig::fovCircleAnimation = std::clamp(AimConfig::fovCircleAnimation, 0, 1);
        AimConfig::fovCircleThickness = std::clamp(AimConfig::fovCircleThickness, 0.75f, 4.0f);
        AimConfig::fovCircleColorR = std::clamp(AimConfig::fovCircleColorR, 0.0f, 1.0f);
        AimConfig::fovCircleColorG = std::clamp(AimConfig::fovCircleColorG, 0.0f, 1.0f);
        AimConfig::fovCircleColorB = std::clamp(AimConfig::fovCircleColorB, 0.0f, 1.0f);
        Menu::offscreenScale = std::clamp(Menu::offscreenScale, 0.55f, 2.5f);
        Menu::offscreenEdgeMargin = std::clamp(Menu::offscreenEdgeMargin, 20.0f, 120.0f);
        Menu::offscreenAnimation = std::clamp(Menu::offscreenAnimation, 0, 1);
        Menu::offscreenPosition = std::clamp(Menu::offscreenPosition, 0, 2);
        Menu::offscreenColorR = std::clamp(Menu::offscreenColorR, 0.0f, 1.0f);
        Menu::offscreenColorG = std::clamp(Menu::offscreenColorG, 0.0f, 1.0f);
        Menu::offscreenColorB = std::clamp(Menu::offscreenColorB, 0.0f, 1.0f);
        Menu::radarShape = std::clamp(Menu::radarShape, 0, 1);
        Menu::radarSize = std::clamp(Menu::radarSize, 120.0f, 320.0f);
        Menu::radarRangeMeters = std::clamp(Menu::radarRangeMeters, 20.0f, 300.0f);
        Menu::radarDotSize = std::clamp(Menu::radarDotSize, 2.0f, 10.0f);
        Menu::radarX = std::clamp(Menu::radarX, 0.0f, (std::max)(0.0f, static_cast<float>(g_width) - Menu::radarSize));
        Menu::radarY = std::clamp(Menu::radarY, 0.0f, (std::max)(0.0f, static_cast<float>(g_height) - Menu::radarSize));
        Menu::radarDotColorR = std::clamp(Menu::radarDotColorR, 0.0f, 1.0f);
        Menu::radarDotColorG = std::clamp(Menu::radarDotColorG, 0.0f, 1.0f);
        Menu::radarDotColorB = std::clamp(Menu::radarDotColorB, 0.0f, 1.0f);
        g_aetherAccentIndex = std::clamp(g_aetherAccentIndex, 0, 4);
        g_aetherOpacity = std::clamp(g_aetherOpacity, 0.70f, 1.00f);
        g_aetherAccentR = std::clamp(g_aetherAccentR, 0.0f, 1.0f);
        g_aetherAccentG = std::clamp(g_aetherAccentG, 0.0f, 1.0f);
        g_aetherAccentB = std::clamp(g_aetherAccentB, 0.0f, 1.0f);
        g_aetherPanelOpacity = std::clamp(g_aetherPanelOpacity, 0.70f, 1.0f);
        g_aetherBorderOpacity = std::clamp(g_aetherBorderOpacity, 0.0f, 0.35f);
        Menu::playerNameScale = std::clamp(Menu::playerNameScale, 0.50f, 2.00f);
        Menu::weaponIconScale = std::clamp(Menu::weaponIconScale, 0.50f, 2.00f);
        Menu::distanceScale = std::clamp(Menu::distanceScale, 0.50f, 2.00f);
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

    // Computes an unclipped screen-space point for directional edge indicators.
    // Behind-camera points are flipped so their arrow points toward the correct screen edge.
    bool WorldToScreenProjected(const Mat4& matrix, const Vec3& position, const Viewport& viewport,
        float& screenX, float& screenY, bool& inFront) noexcept
    {
        float clipW = matrix.m[3][0] * position.x + matrix.m[3][1] * position.y +
            matrix.m[3][2] * position.z + matrix.m[3][3];
        const float clipXRaw = matrix.m[0][0] * position.x + matrix.m[0][1] * position.y +
            matrix.m[0][2] * position.z + matrix.m[0][3];
        const float clipYRaw = matrix.m[1][0] * position.x + matrix.m[1][1] * position.y +
            matrix.m[1][2] * position.z + matrix.m[1][3];
        if (!std::isfinite(clipW) || !std::isfinite(clipXRaw) || !std::isfinite(clipYRaw) ||
            std::fabs(clipW) < 0.0001f)
            return false;

        inFront = clipW > 0.05f;
        float clipX = clipXRaw;
        float clipY = clipYRaw;
        if (clipW < 0.0f)
        {
            clipW = -clipW;
            clipX = -clipX;
            clipY = -clipY;
        }
        const float inverseW = 1.0f / (std::max)(clipW, 0.0001f);
        screenX = viewport.x + (viewport.w * 0.5f) * (1.0f + clipX * inverseW);
        screenY = viewport.y + (viewport.h * 0.5f) * (1.0f - clipY * inverseW);
        if (!std::isfinite(screenX) || !std::isfinite(screenY)) return false;
        screenX = std::clamp(screenX, -1000000.0f, 1000000.0f);
        screenY = std::clamp(screenY, -1000000.0f, 1000000.0f);
        return true;
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
        const MaotawRuntime::WeaponIcons::AlphaSprite& sprite, float scale = 1.0f) noexcept
    {
        WeaponIconBitmapEntry* bitmaps = GetWeaponIconBitmaps(sprite);
        if (!bitmaps || !g_d2dTarget) return;
        scale = std::clamp(scale, 0.50f, 2.00f);
        const float drawWidth = static_cast<float>(sprite.width) * scale;
        const float drawHeight = static_cast<float>(sprite.height) * scale;
        const float left = centerX - drawWidth * 0.5f;
        const D2D1_RECT_F destination{
            left, y, left + drawWidth,
            y + drawHeight
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
        float maxWidth, ID2D1SolidColorBrush* textBrush, float scale = 1.0f) noexcept
    {
        if (!text || !*text || !g_d2dTarget || !g_smallTextFormat || !g_brushOutline)
            return;
        if (!textBrush) textBrush = g_brushText;
        if (!textBrush) return;
        scale = std::clamp(scale, 0.50f, 2.00f);

        // IDWriteTextFormat has no SetFontSize method. Keep the shared text
        // format immutable with respect to font size and scale this draw call
        // through the Direct2D transform instead. The transform is anchored at
        // the text's top-centre so the label remains horizontally centred and
        // its configured Y position stays stable.
        const float halfWidth = maxWidth * 0.5f;
        constexpr float textHeight = 13.0f;
        const D2D1_RECT_F shadowRect{
            centerX - halfWidth + 1.0f, y + 1.0f,
            centerX + halfWidth + 1.0f, y + textHeight + 1.0f
        };
        const D2D1_RECT_F textRect{
            centerX - halfWidth, y,
            centerX + halfWidth, y + textHeight
        };
        const UINT32 length = static_cast<UINT32>(wcslen(text));
        const DWRITE_TEXT_ALIGNMENT previousAlignment = g_smallTextFormat->GetTextAlignment();
        D2D1_MATRIX_3X2_F previousTransform{};
        g_d2dTarget->GetTransform(&previousTransform);

        g_smallTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const D2D1_MATRIX_3X2_F scaleTransform = D2D1::Matrix3x2F::Scale(
            scale, scale, D2D1::Point2F(centerX, y));
        g_d2dTarget->SetTransform(scaleTransform * previousTransform);

        g_d2dTarget->DrawText(text, length, g_smallTextFormat, shadowRect,
            g_brushOutline, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        g_d2dTarget->DrawText(text, length, g_smallTextFormat, textRect,
            textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

        g_d2dTarget->SetTransform(previousTransform);
        g_smallTextFormat->SetTextAlignment(previousAlignment);
    }

    void DrawPlayerInfo(const EntityDrawData& entity) noexcept
    {
        if ((!Menu::playerName && !Menu::weaponIcon && !Menu::distance) ||
            !entity.valid || !g_d2dTarget) return;

        const float centerX = (entity.left + entity.right) * 0.5f;
        const float centerY = (entity.top + entity.bottom) * 0.5f;
        const float boxHeight = (std::max)(1.0f, entity.bottom - entity.top);
        const float baseTextWidth = std::clamp((entity.right - entity.left) * 2.6f, 72.0f, 180.0f);
        constexpr float kInfoGap = 4.0f;
        constexpr float kEdgePadding = 4.0f;

        enum class InfoKind : uint8_t { Text, WeaponSprite };
        struct InfoElement
        {
            Menu::InfoPosition position = Menu::InfoPosition::Above;
            InfoKind kind = InfoKind::Text;
            const wchar_t* text = nullptr;
            const MaotawRuntime::WeaponIcons::AlphaSprite* sprite = nullptr;
            ID2D1SolidColorBrush* brush = nullptr;
            float scale = 1.0f;
            float width = 0.0f;
            float height = 0.0f;
        };

        // Each enabled item becomes a layout element. Elements sharing an anchor
        // are stacked instead of being drawn at the same coordinates.
        std::array<InfoElement, 3> elements{};
        size_t elementCount = 0;
        auto addText = [&](const wchar_t* text, Menu::InfoPosition position,
            float scale, ID2D1SolidColorBrush* brush) noexcept
            {
                if (!text || !*text || elementCount >= elements.size()) return;
                scale = std::clamp(scale, 0.50f, 2.00f);
                InfoElement& item = elements[elementCount++];
                item.position = position;
                item.kind = InfoKind::Text;
                item.text = text;
                item.brush = brush;
                item.scale = scale;
                item.width = baseTextWidth * scale;
                item.height = 13.0f * scale;
            };
        auto addSprite = [&](const MaotawRuntime::WeaponIcons::AlphaSprite& sprite,
            Menu::InfoPosition position, float scale) noexcept
            {
                if (elementCount >= elements.size()) return;
                scale = std::clamp(scale, 0.50f, 2.00f);
                InfoElement& item = elements[elementCount++];
                item.position = position;
                item.kind = InfoKind::WeaponSprite;
                item.sprite = &sprite;
                item.scale = scale;
                item.width = static_cast<float>(sprite.width) * scale;
                item.height = static_cast<float>(sprite.height) * scale;
            };

        wchar_t playerName[32]{};
        if (Menu::playerName && entity.playerName[0])
        {
            const int maxChars = boxHeight >= 78.0f ? 18 : (boxHeight >= 44.0f ? 15 : 12);
            int n = 0;
            for (; n < maxChars && entity.playerName[n]; ++n)
                playerName[n] = static_cast<wchar_t>(std::toupper(
                    static_cast<unsigned char>(entity.playerName[n])));
            playerName[n] = L'\0';
            // Fixed layout requested: player name stays above the box.
            addText(playerName, Menu::InfoPosition::Above,
                Menu::playerNameScale, g_brushPlayerName);
        }

        wchar_t weaponLabel[24]{};
        if (Menu::weaponIcon && entity.weaponId != 0)
        {
            if (const auto* iconSet = WeaponIconForId(entity.weaponId))
            {
                const auto& sprite = WeaponSpriteForBoxHeight(*iconSet,
                    static_cast<int>(std::lround(boxHeight)));
                // Weapon icon is the first item in the below-box stack.
                addSprite(sprite, Menu::InfoPosition::Below, Menu::weaponIconScale);
            }
            else if (const char* label = WeaponLabelForId(entity.weaponId))
            {
                size_t i = 0;
                for (; label[i] && i + 1 < std::size(weaponLabel); ++i)
                    weaponLabel[i] = static_cast<wchar_t>(label[i]);
                weaponLabel[i] = L'\0';
                if (weaponLabel[0] != L'\0')
                    addText(weaponLabel, Menu::InfoPosition::Below,
                        Menu::weaponIconScale, g_brushWeaponInfo);
            }
        }
        wchar_t distanceLabel[16]{};
        if (Menu::distance && entity.distanceValid)
        {
            swprintf_s(distanceLabel, L"%dM", (std::max)(0, entity.distanceMeters));
            // Distance follows the weapon item in the same below-box stack.
            addText(distanceLabel, Menu::InfoPosition::Below,
                Menu::distanceScale, g_brushDistance);
        }

        if (!elementCount) return;

        // Draw each anchor lane independently. Top/bottom lanes flow vertically
        // away from the box; left/right lanes form a vertically centred column.
        // If a lane would leave the viewport, it is flipped to the opposite edge.
        auto drawElement = [&](const InfoElement& item, float itemCenterX,
            float itemTop) noexcept
            {
                if (item.kind == InfoKind::WeaponSprite && item.sprite)
                    DrawWeaponSpriteCentered(itemCenterX, itemTop, *item.sprite, item.scale);
                else if (item.kind == InfoKind::Text && item.text)
                    DrawInfoTextCentered(item.text, itemCenterX, itemTop,
                        baseTextWidth, item.brush, item.scale);
            };

        for (int lane = 0; lane < 4; ++lane)
        {
            const auto requestedPosition = static_cast<Menu::InfoPosition>(lane);
            std::array<size_t, 3> laneItems{};
            size_t laneCount = 0;
            float totalHeight = 0.0f;
            for (size_t i = 0; i < elementCount; ++i)
            {
                if (elements[i].position != requestedPosition) continue;
                laneItems[laneCount++] = i;
                totalHeight += elements[i].height;
            }
            if (!laneCount) continue;
            totalHeight += kInfoGap * static_cast<float>(laneCount - 1);

            const Menu::InfoPosition finalPosition = requestedPosition;
            float cursorY = 0.0f;
            if (requestedPosition == Menu::InfoPosition::Above)
            {
                cursorY = (std::max)(kEdgePadding,
                    entity.top - kInfoGap - totalHeight);
            }
            else if (requestedPosition == Menu::InfoPosition::Below)
            {
                cursorY = (std::min)(entity.bottom + kInfoGap,
                    (std::max)(kEdgePadding, static_cast<float>(g_height) -
                        kEdgePadding - totalHeight));
            }
            else
            {
                cursorY = centerY - totalHeight * 0.5f;
                cursorY = std::clamp(cursorY, kEdgePadding,
                    (std::max)(kEdgePadding, static_cast<float>(g_height) -
                        kEdgePadding - totalHeight));
            }

            for (size_t n = 0; n < laneCount; ++n)
            {
                const InfoElement& item = elements[laneItems[n]];
                float itemCenterX = centerX;
                if (finalPosition == Menu::InfoPosition::Left)
                    itemCenterX = entity.left - kInfoGap - item.width * 0.5f;
                else if (finalPosition == Menu::InfoPosition::Right)
                    itemCenterX = entity.right + kInfoGap + item.width * 0.5f;

                // Clamp the horizontal centre so long names and large icons stay
                // inside the desktop viewport while remaining centred to the box.
                const float halfWidth = item.width * 0.5f;
                itemCenterX = std::clamp(itemCenterX,
                    kEdgePadding + halfWidth,
                    (std::max)(kEdgePadding + halfWidth,
                        static_cast<float>(g_width) - kEdgePadding - halfWidth));

                drawElement(item, itemCenterX, cursorY);
                cursorY += item.height + kInfoGap;
            }
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
        SafeRelease(g_aetherLogoFormat);
        SafeRelease(g_aetherIconFormat);
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
            !makeFormat(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 10.5f, &g_codeTextFormat) ||
            !makeFormat(L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 10.5f, &g_aetherIconFormat) ||
            !makeFormat(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 14.5f, &g_aetherLogoFormat))
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
        g_aetherIconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_aetherIconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_aetherLogoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_aetherLogoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

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
        g_radarEntityCount = 0;
        g_radarLocalValid = false;
        if (!Menu::box && !Menu::health && !Menu::armor && !Menu::snapline && !Menu::skeleton &&
            !Menu::playerName && !Menu::weaponIcon && !Menu::distance && !Menu::hitMarker && !Menu::damagePopup &&
            !Menu::offscreenIndicators && !Menu::radarEnabled && !AimConfig::enabled && !AimConfig::rcsEnabled) return;
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
        Vec3 localViewAngles{};
        const bool localYawValid = localPawn &&
            SafeRead(clientBase + Offsets::dwViewAngles, localViewAngles) &&
            std::isfinite(localViewAngles.y) && std::fabs(localViewAngles.y) <= 720.0f;
        if (localOriginValid && localYawValid)
        {
            g_radarLocalOrigin = localOrigin;
            g_radarViewYaw = localViewAngles.y;
            g_radarLocalValid = true;
        }

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

            if (Menu::radarEnabled && localOriginValid && g_radarEntityCount < g_radarEntities.size())
            {
                RadarEntityData& radarEntity = g_radarEntities[g_radarEntityCount++];
                radarEntity.worldOrigin = origin;
                radarEntity.team = teamNumber;
                radarEntity.valid = true;
            }

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
            if (Menu::offscreenIndicators)
            {
                float projectedHeadX = 0.0f, projectedHeadY = 0.0f;
                bool headInFront = false;
                if (WorldToScreenProjected(viewMatrix, head, viewport,
                    projectedHeadX, projectedHeadY, headInFront))
                {
                    const bool headOnScreen = headInFront &&
                        projectedHeadX >= 0.0f && projectedHeadX <= viewport.w &&
                        projectedHeadY >= 0.0f && projectedHeadY <= viewport.h;
                    if (!headOnScreen)
                    {
                        float directionX = projectedHeadX - viewport.w * 0.5f;
                        float directionY = projectedHeadY - viewport.h * 0.5f;
                        float directionLength = std::sqrt(directionX * directionX + directionY * directionY);
                        if (!std::isfinite(directionLength) || directionLength <= 0.001f)
                        {
                            // A target almost exactly behind the camera projects near the center.
                            // Keep a visible fallback marker rather than silently dropping it.
                            directionX = 0.0f;
                            directionY = -1.0f;
                            directionLength = 1.0f;
                        }
                        EntityDrawData indicator{};
                        indicator.valid = true;
                        indicator.offscreen = true;
                        indicator.offscreenDirectionX = directionX / directionLength;
                        indicator.offscreenDirectionY = directionY / directionLength;
                        indicator.health = healthValue;
                        indicator.armor = armorValue;
                        indicator.team = teamNumber;
                        indicator.worldOrigin = origin;
                        indicator.pawnKey = pawn;
                        frame[outIndex++] = indicator;
                        continue;
                    }
                }
            }

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
                    // Off-screen indicator records intentionally contain only
                    // direction/identity data; never treat them as aim candidates.
                    if (!target.valid || target.offscreen || !target.pawnKey)
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

                    if (target.pawnKey == localPawn ||
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
            if (a[i].offscreen != b[i].offscreen) return false;
            if (a[i].offscreen)
            {
                if (a[i].pawnKey != b[i].pawnKey ||
                    std::fabs(a[i].offscreenDirectionX - b[i].offscreenDirectionX) > 0.002f ||
                    std::fabs(a[i].offscreenDirectionY - b[i].offscreenDirectionY) > 0.002f)
                    return false;
                continue;
            }
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

    void DrawAimFovCircle() noexcept
    {
        if (!AimConfig::showFovCircle || !g_d2dTarget || !g_brushSnapline) return;
        const float radius = std::clamp(AimConfig::fovRadius, 1.0f, 1000.0f);
        float pulse = 1.0f;
        float alpha = 0.86f;
        if (AimConfig::fovCircleAnimation == 1)
        {
            const float phase = static_cast<float>(GetTickCount64()) * 0.0035f;
            const float wave = 0.5f + 0.5f * std::sin(phase);
            pulse = 0.94f + wave * 0.06f;
            alpha = 0.48f + wave * 0.42f;
        }
        const D2D1_COLOR_F previous = g_brushSnapline->GetColor();
        g_brushSnapline->SetColor(Color(AimConfig::fovCircleColorR,
            AimConfig::fovCircleColorG, AimConfig::fovCircleColorB, alpha));
        const D2D1_ELLIPSE circle = D2D1::Ellipse(
            D2D1::Point2F(static_cast<float>(g_width) * 0.5f,
                static_cast<float>(g_height) * 0.5f), radius * pulse, radius * pulse);
        if (AimConfig::fovCircleAnimation == 1)
            g_d2dTarget->DrawEllipse(circle, g_brushSnapline,
                std::clamp(AimConfig::fovCircleThickness, 0.75f, 4.0f) * 2.2f);
        g_brushSnapline->SetColor(Color(AimConfig::fovCircleColorR,
            AimConfig::fovCircleColorG, AimConfig::fovCircleColorB, 1.0f));
        g_d2dTarget->DrawEllipse(circle, g_brushSnapline,
            std::clamp(AimConfig::fovCircleThickness, 0.75f, 4.0f));
        g_brushSnapline->SetColor(previous);
    }

    void DrawRadar() noexcept
    {
        if (!Menu::radarEnabled || !g_radarLocalValid || !g_d2dTarget ||
            !g_brushMenuBack || !g_brushMenuBorder || !g_brushMenuText || !g_brushSnapline)
            return;

        const float size = std::clamp(Menu::radarSize, 120.0f, 320.0f);
        const float x = std::clamp(Menu::radarX, 0.0f, (std::max)(0.0f, static_cast<float>(g_width) - size));
        const float y = std::clamp(Menu::radarY, 0.0f, (std::max)(0.0f, static_cast<float>(g_height) - size));
        Menu::radarX = x;
        Menu::radarY = y;
        const bool circleShape = Menu::radarShape == 0;
        const D2D1_RECT_F panel = { x, y, x + size, y + size };
        const float centerX = x + size * 0.5f;
        // The radar has no title/header text; use the whole panel for the map.
        const float mapTop = y + 14.0f;
        const float mapBottom = y + size - 14.0f;
        const float mapHeight = mapBottom - mapTop;
        const float mapCenterY = mapTop + mapHeight * 0.5f;
        const float mapHalfW = size * 0.5f - 16.0f;
        const float mapHalfH = mapHeight * 0.5f;
        const float mapRadius = (std::min)(mapHalfW, mapHalfH);
        const float rangeWorld = std::clamp(Menu::radarRangeMeters, 20.0f, 300.0f) * 39.37f;
        const float yaw = g_radarViewYaw * 0.01745329251994329577f;
        const float cosYaw = std::cos(yaw);
        const float sinYaw = std::sin(yaw);

        const D2D1_COLOR_F oldBack = g_brushMenuBack->GetColor();
        const D2D1_COLOR_F oldBorder = g_brushMenuBorder->GetColor();
        const D2D1_COLOR_F oldText = g_brushMenuText->GetColor();
        const D2D1_COLOR_F oldSnap = g_brushSnapline->GetColor();

        g_brushMenuBack->SetColor(Color(0.035f, 0.043f, 0.055f, 0.91f));
        g_brushMenuBorder->SetColor(Color(g_aetherAccentR, g_aetherAccentG, g_aetherAccentB, 0.78f));
        g_brushMenuText->SetColor(Color(0.91f, 0.93f, 0.96f, 0.96f));
        g_brushSnapline->SetColor(Color(0.42f, 0.47f, 0.54f, 0.40f));

        if (circleShape)
        {
            const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(centerX, y + size * 0.5f), size * 0.49f, size * 0.49f);
            g_d2dTarget->FillEllipse(outer, g_brushMenuBack);
            g_d2dTarget->DrawEllipse(outer, g_brushMenuBorder, 1.2f);
        }
        else
        {
            const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(panel, 12.0f, 12.0f);
            g_d2dTarget->FillRoundedRectangle(outer, g_brushMenuBack);
            g_d2dTarget->DrawRoundedRectangle(outer, g_brushMenuBorder, 1.2f);
        }

        // Minimal three-dot move affordance; the entire radar surface is draggable
        // while the menu is open, not just this small grip area.
        g_brushMenuBorder->SetColor(Color(0.76f, 0.80f, 0.86f, 0.82f));
        for (int i = 0; i < 3; ++i)
        {
            const D2D1_ELLIPSE gripDot = D2D1::Ellipse(
                D2D1::Point2F(centerX - 7.0f + i * 7.0f, y + 9.0f), 1.25f, 1.25f);
            g_d2dTarget->FillEllipse(gripDot, g_brushMenuBorder);
        }

        // Radar grid: center axes plus a halfway ring / square guide.
        g_brushSnapline->SetColor(Color(0.58f, 0.63f, 0.70f, 0.28f));
        if (circleShape)
        {
            const D2D1_ELLIPSE ring = D2D1::Ellipse(
                D2D1::Point2F(centerX, mapCenterY), mapRadius * 0.5f, mapRadius * 0.5f);
            g_d2dTarget->DrawEllipse(ring, g_brushSnapline, 0.8f);
            g_d2dTarget->DrawLine(D2D1::Point2F(centerX - mapRadius, mapCenterY),
                D2D1::Point2F(centerX + mapRadius, mapCenterY), g_brushSnapline, 0.8f);
            g_d2dTarget->DrawLine(D2D1::Point2F(centerX, mapCenterY - mapRadius),
                D2D1::Point2F(centerX, mapCenterY + mapRadius), g_brushSnapline, 0.8f);
        }
        else
        {
            const D2D1_RECT_F mapRect = { centerX - mapHalfW, mapCenterY - mapHalfH,
                centerX + mapHalfW, mapCenterY + mapHalfH };
            const D2D1_RECT_F guideRect = {
                mapRect.left + mapHalfW * 0.5f, mapRect.top + mapHalfH * 0.5f,
                mapRect.right - mapHalfW * 0.5f, mapRect.bottom - mapHalfH * 0.5f };
            const D2D1_ROUNDED_RECT guide = D2D1::RoundedRect(guideRect, 5.0f, 5.0f);
            g_d2dTarget->DrawRoundedRectangle(guide, g_brushSnapline, 0.8f);
            g_d2dTarget->DrawLine(D2D1::Point2F(centerX - mapHalfW, mapCenterY),
                D2D1::Point2F(centerX + mapHalfW, mapCenterY), g_brushSnapline, 0.8f);
            g_d2dTarget->DrawLine(D2D1::Point2F(centerX, mapCenterY - mapHalfH),
                D2D1::Point2F(centerX, mapCenterY + mapHalfH), g_brushSnapline, 0.8f);
        }

        const float dotRadius = std::clamp(Menu::radarDotSize, 2.0f, 10.0f);
        const float distanceLimitSq = rangeWorld * rangeWorld;
        g_brushSnapline->SetColor(Color(Menu::radarDotColorR, Menu::radarDotColorG,
            Menu::radarDotColorB, 1.0f));
        for (size_t i = 0; i < g_radarEntityCount; ++i)
        {
            const RadarEntityData& entity = g_radarEntities[i];
            if (!entity.valid) continue;
            const float dx = entity.worldOrigin.x - g_radarLocalOrigin.x;
            const float dy = entity.worldOrigin.y - g_radarLocalOrigin.y;
            const float distanceSq = dx * dx + dy * dy;
            if (!std::isfinite(distanceSq) || distanceSq > distanceLimitSq) continue;

            // Rotate world delta into the player's view. Negate the lateral
            // axis so a target physically on the player's left is drawn on the
            // left side of the radar (and vice versa).
            const float right = dx * sinYaw - dy * cosYaw;
            const float forward = dx * cosYaw + dy * sinYaw;
            float px = centerX;
            float py = mapCenterY;
            if (circleShape)
            {
                px += (right / rangeWorld) * mapRadius;
                py -= (forward / rangeWorld) * mapRadius;
                const float nx = px - centerX;
                const float ny = py - mapCenterY;
                if (nx * nx + ny * ny > mapRadius * mapRadius) continue;
            }
            else
            {
                px += (right / rangeWorld) * mapHalfW;
                py -= (forward / rangeWorld) * mapHalfH;
                if (px < centerX - mapHalfW || px > centerX + mapHalfW ||
                    py < mapCenterY - mapHalfH || py > mapCenterY + mapHalfH) continue;
            }
            const D2D1_ELLIPSE enemyDot = D2D1::Ellipse(
                D2D1::Point2F(px, py), dotRadius, dotRadius);
            g_d2dTarget->FillEllipse(enemyDot, g_brushSnapline);
        }

        // Local player marker: bright centre dot and a short forward pointer.
        g_brushSnapline->SetColor(Color(0.92f, 0.95f, 1.00f, 1.0f));
        const D2D1_ELLIPSE localDot = D2D1::Ellipse(
            D2D1::Point2F(centerX, mapCenterY), dotRadius + 1.1f, dotRadius + 1.1f);
        g_d2dTarget->FillEllipse(localDot, g_brushSnapline);
        g_brushSnapline->SetColor(Color(g_aetherAccentR, g_aetherAccentG, g_aetherAccentB, 0.95f));
        g_d2dTarget->DrawLine(D2D1::Point2F(centerX, mapCenterY - 5.0f),
            D2D1::Point2F(centerX, mapCenterY - mapRadius * 0.22f), g_brushSnapline, 1.5f);

        g_brushMenuBack->SetColor(oldBack);
        g_brushMenuBorder->SetColor(oldBorder);
        g_brushMenuText->SetColor(oldText);
        g_brushSnapline->SetColor(oldSnap);
    }

    void DrawOffscreenIndicator(const EntityDrawData& entity,
        size_t indicatorIndex, size_t indicatorCount) noexcept
    {
        if (!Menu::offscreenIndicators || !entity.offscreen || !g_d2dTarget || !g_brushSnapline)
            return;
        float dx = entity.offscreenDirectionX;
        float dy = entity.offscreenDirectionY;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(len) || len < 0.001f) return;
        dx /= len; dy /= len;

        const float width = static_cast<float>(g_width);
        const float height = static_cast<float>(g_height);
        const float centerX = width * 0.5f;
        const float centerY = height * 0.5f;
        const float inset = std::clamp(Menu::offscreenEdgeMargin, 20.0f,
            (std::min)(width, height) * 0.25f);
        float x = centerX;
        float y = centerY;

        switch (std::clamp(Menu::offscreenPosition, 0, 2))
        {
        case 0: // Center: compact ring around the screen centre / crosshair.
        {
            const float spreadIndex = static_cast<float>(indicatorIndex) -
                static_cast<float>(indicatorCount > 0 ? indicatorCount - 1 : 0) * 0.5f;
            const float radius = std::clamp(inset + 28.0f + spreadIndex * 8.0f,
                46.0f, 116.0f);
            x = centerX + dx * radius;
            y = centerY + dy * radius;
            break;
        }
        case 1: // Screen left: evenly spaced vertical rail; each arrow keeps its direction.
        case 2: // Screen right.
        {
            const float available = (std::max)(40.0f, height - inset * 2.0f);
            const float count = static_cast<float>((std::max)(size_t{ 1 }, indicatorCount));
            const float slot = (static_cast<float>(indicatorIndex) + 1.0f) / (count + 1.0f);
            x = (Menu::offscreenPosition == 1) ? inset : width - inset;
            y = inset + available * slot;
            break;
        }
        }

        float pulse = 1.0f;
        float alpha = 1.0f;
        if (Menu::offscreenAnimation == 1)
        {
            const float phase = static_cast<float>(GetTickCount64()) * 0.0045f;
            const float wave = 0.5f + 0.5f * std::sin(phase);
            pulse = 0.82f + 0.24f * wave;
            alpha = 0.55f + 0.45f * wave;
        }
        const float scale = std::clamp(Menu::offscreenScale, 0.55f, 2.5f);
        const float size = 10.0f * scale * pulse;
        const D2D1_POINT_2F tip{ x + dx * size * 0.72f, y + dy * size * 0.72f };
        const float baseX = x - dx * size * 0.78f;
        const float baseY = y - dy * size * 0.78f;
        const float perpendicularX = -dy;
        const float perpendicularY = dx;
        const float halfBase = size * 0.62f;
        const D2D1_POINT_2F left{ baseX + perpendicularX * halfBase, baseY + perpendicularY * halfBase };
        const D2D1_POINT_2F right{ baseX - perpendicularX * halfBase, baseY - perpendicularY * halfBase };

        const D2D1_COLOR_F previous = g_brushSnapline->GetColor();
        g_brushSnapline->SetColor(Color(Menu::offscreenColorR,
            Menu::offscreenColorG, Menu::offscreenColorB, alpha * 0.28f));
        g_d2dTarget->DrawLine(tip, left, g_brushSnapline, 4.0f * scale);
        g_d2dTarget->DrawLine(tip, right, g_brushSnapline, 4.0f * scale);
        g_d2dTarget->DrawLine(left, right, g_brushSnapline, 4.0f * scale);
        g_brushSnapline->SetColor(Color(Menu::offscreenColorR,
            Menu::offscreenColorG, Menu::offscreenColorB, alpha));
        g_d2dTarget->DrawLine(tip, left, g_brushSnapline, 1.7f * scale);
        g_d2dTarget->DrawLine(tip, right, g_brushSnapline, 1.7f * scale);
        g_d2dTarget->DrawLine(left, right, g_brushSnapline, 1.7f * scale);
        g_brushSnapline->SetColor(previous);
    }

    void DrawEntityFrame(const EntityFrame& frame) noexcept
    {
        if (!Menu::box && !Menu::health && !Menu::armor && !Menu::snapline && !Menu::skeleton &&
            !Menu::playerName && !Menu::weaponIcon && !Menu::distance && !Menu::offscreenIndicators) return;
        size_t offscreenCount = 0;
        if (Menu::offscreenIndicators)
        {
            for (const EntityDrawData& candidate : frame)
                if (candidate.valid && candidate.offscreen) ++offscreenCount;
        }
        size_t offscreenIndex = 0;
        for (const EntityDrawData& entity : frame)
        {
            if (!entity.valid) continue;
            if (entity.offscreen)
            {
                DrawOffscreenIndicator(entity, offscreenIndex++, offscreenCount);
                continue;
            }
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
        constexpr float menuWidth = 642.0f;
        constexpr float menuHeight = 336.0f;
        if (!g_menuPositionInitialized)
        {
            // Match the standalone Aether reference: open centered on the screen.
            g_menuX = std::max(8.0f, (static_cast<float>(g_width) - menuWidth) * 0.5f);
            g_menuY = std::max(8.0f, (static_cast<float>(g_height) - menuHeight) * 0.5f);
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

    // ------------------------------------------------------------------------
    // Aether menu: same 642x336 canvas, sidebar, cards and dot-popups as the
    // supplied reference UI. Every control below is bound to a real Menu or
    // AimConfig field; no placeholder weapon/config/skin controls are shown.
    // ------------------------------------------------------------------------
    enum class AetherPopup : int
    {
        None, AimActivation, Box, Health, Armor, Snapline, Skeleton,
        PlayerName, WeaponIcon, Distance, HitMarker, DamagePopup,
        FovCircle, OffscreenArrows, Radar
    };

    enum class AetherControlKind : int
    {
        Tab, Toggle, Slider, Cycle, Color, Popup, AimKey, ResetAppearance,
        MenuHotkey, ExitHotkey, ExitApp, ResetHotkeys
    };

    struct AetherControl
    {
        D2D1_RECT_F rect{};
        AetherControlKind kind = AetherControlKind::Toggle;
        void* value = nullptr;
        float minimum = 0.0f;
        float maximum = 1.0f;
        float step = 0.0f;
        int cycleCount = 0;
        MenuTab tab = MenuTab::Aimbot;
        AetherPopup popup = AetherPopup::None;
        float* colorR = nullptr;
        float* colorG = nullptr;
        float* colorB = nullptr;
    };

    std::array<AetherControl, 96> g_aetherControls{};
    size_t g_aetherControlCount = 0;
    int g_aetherActiveSlider = -1;
    AetherPopup g_aetherPopup = AetherPopup::None;
    D2D1_RECT_F g_aetherPopupAnchor{};
    D2D1_RECT_F g_aetherPopupRect{};
    bool g_aetherDragging = false;
    bool g_radarDragging = false;
    bool g_radarPositionDirty = false;
    float g_radarDragOffsetX = 0.0f;
    float g_radarDragOffsetY = 0.0f;
    float g_aetherDragOffsetX = 0.0f;
    float g_aetherDragOffsetY = 0.0f;

    enum class AetherColorDrag : int { None, SaturationValue, Hue };
    bool g_aetherColorPickerOpen = false;
    AetherColorDrag g_aetherColorDrag = AetherColorDrag::None;
    D2D1_RECT_F g_aetherColorPickerRect{};
    D2D1_RECT_F g_aetherColorSvRect{};
    D2D1_RECT_F g_aetherColorHueRect{};
    float* g_aetherColorTargetR = nullptr;
    float* g_aetherColorTargetG = nullptr;
    float* g_aetherColorTargetB = nullptr;
    float g_aetherColorHue = 0.0f;
    float g_aetherColorSaturation = 1.0f;
    float g_aetherColorValue = 1.0f;
    bool g_aetherWeaponIconColorDirty = false;

    D2D1_COLOR_F AetherRgb(UINT32 rgb, float alpha = 1.0f) noexcept
    {
        return Color(((rgb >> 16) & 0xFF) / 255.0f,
            ((rgb >> 8) & 0xFF) / 255.0f,
            (rgb & 0xFF) / 255.0f, alpha);
    }

    D2D1_COLOR_F AetherAccent() noexcept
    {
        return Color(std::clamp(g_aetherAccentR, 0.0f, 1.0f),
            std::clamp(g_aetherAccentG, 0.0f, 1.0f),
            std::clamp(g_aetherAccentB, 0.0f, 1.0f), 1.0f);
    }

    D2D1_RECT_F AetherRect(float x, float y, float w, float h) noexcept
    {
        return D2D1_RECT_F{ x, y, x + w, y + h };
    }

    void AetherFill(float x, float y, float w, float h, float radius,
        UINT32 rgb, float alpha = 1.0f) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuBack || w <= 0.0f || h <= 0.0f) return;
        g_brushMenuBack->SetColor(AetherRgb(rgb, alpha));
        const D2D1_ROUNDED_RECT rr{ AetherRect(x, y, w, h), radius, radius };
        g_d2dTarget->FillRoundedRectangle(&rr, g_brushMenuBack);
    }

    void AetherStroke(float x, float y, float w, float h, float radius,
        UINT32 rgb, float alpha = 1.0f, float thickness = 1.0f) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuBorder || w <= 0.0f || h <= 0.0f) return;
        g_brushMenuBorder->SetColor(AetherRgb(rgb, alpha));
        const D2D1_ROUNDED_RECT rr{ AetherRect(x, y, w, h), radius, radius };
        g_d2dTarget->DrawRoundedRectangle(&rr, g_brushMenuBorder, thickness);
    }

    void AetherLine(float x1, float y1, float x2, float y2,
        UINT32 rgb, float alpha = 1.0f, float thickness = 1.0f) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuBorder) return;
        g_brushMenuBorder->SetColor(AetherRgb(rgb, alpha));
        g_d2dTarget->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
            g_brushMenuBorder, thickness);
    }

    void AetherText(const wchar_t* value, float x, float y, float w, float h,
        UINT32 rgb = 0xD8D8DC, IDWriteTextFormat* format = nullptr,
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
        float alpha = 1.0f) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuText || !value) return;
        IDWriteTextFormat* f = format ? format : g_menuTextFormat;
        if (!f) return;
        f->SetTextAlignment(alignment);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_brushMenuText->SetColor(AetherRgb(rgb, alpha));
        g_d2dTarget->DrawText(value, static_cast<UINT32>(wcslen(value)), f,
            AetherRect(x, y, w, h), g_brushMenuText,
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void AetherText(const std::wstring& value, float x, float y, float w, float h,
        UINT32 rgb = 0xD8D8DC, IDWriteTextFormat* format = nullptr,
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
        float alpha = 1.0f) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuText) return;
        IDWriteTextFormat* f = format ? format : g_menuTextFormat;
        if (!f) return;
        f->SetTextAlignment(alignment);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_brushMenuText->SetColor(AetherRgb(rgb, alpha));
        g_d2dTarget->DrawText(value.c_str(), static_cast<UINT32>(value.size()), f,
            AetherRect(x, y, w, h), g_brushMenuText,
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void AetherIcon(UINT32 glyph, float x, float y, float w, float h,
        UINT32 rgb = 0xC8C8CD, float alpha = 1.0f) noexcept
    {
        wchar_t text[2]{ static_cast<wchar_t>(glyph), L'\0' };
        AetherText(text, x, y, w, h, rgb,
            g_aetherIconFormat ? g_aetherIconFormat : g_smallTextFormat,
            DWRITE_TEXT_ALIGNMENT_CENTER, alpha);
    }

    bool AetherHovered(const D2D1_RECT_F& r) noexcept
    {
        const float mx = static_cast<float>(g_menuMouse.x) - g_menuX;
        const float my = static_cast<float>(g_menuMouse.y) - g_menuY;
        return ContainsPoint(r, mx, my);
    }

    void AetherPush(const AetherControl& control) noexcept
    {
        if (g_aetherControlCount < g_aetherControls.size())
            g_aetherControls[g_aetherControlCount++] = control;
    }

    void AetherCard(float x, float y, float w, float h, const wchar_t* title) noexcept
    {
        AetherFill(x, y, w, h, 6.0f, 0x17171A, 1.0f);
        AetherFill(x, y, w, 30.0f, 6.0f, 0x1C1C20, 1.0f);
        AetherFill(x, y + 24.0f, w, 6.0f, 0.0f, 0x1C1C20, 1.0f);
        AetherLine(x, y + 30.0f, x + w, y + 30.0f, 0x28282D, 1.0f, 0.8f);
        AetherStroke(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 6.0f, 0x303036, 0.75f, 0.8f);
        AetherText(title, x + 9.0f, y + 4.0f, w - 18.0f, 22.0f,
            0xF0F0F3, g_menuTitleFormat);
    }

    void AetherAddTab(float y, MenuTab tab, const wchar_t* label, UINT32 glyph) noexcept
    {
        const D2D1_RECT_F hit = AetherRect(3.0f, y - 12.0f, 120.0f, 24.0f);
        const bool active = g_activeTab == tab;
        const bool hover = AetherHovered(hit);
        if (active) AetherFill(hit.left, hit.top, hit.right - hit.left, hit.bottom - hit.top, 6.0f, 0xFFFFFF, 0.075f);
        else if (hover) AetherFill(hit.left, hit.top, hit.right - hit.left, hit.bottom - hit.top, 6.0f, 0xFFFFFF, 0.04f);
        const D2D1_COLOR_F accent = AetherAccent();
        if (active && g_brushSliderFill)
        {
            g_brushSliderFill->SetColor(accent);
            const D2D1_ROUNDED_RECT marker{ AetherRect(3.0f, y - 8.0f, 2.0f, 16.0f), 1.0f, 1.0f };
            g_d2dTarget->FillRoundedRectangle(&marker, g_brushSliderFill);
        }
        AetherText(label, 33.0f, y - 10.0f, 86.0f, 20.0f,
            active ? 0xFFFFFF : (hover ? 0xE8E8EC : 0xA7A7AE), g_menuTextFormat);
        AetherIcon(glyph, 9.0f, y - 10.0f, 18.0f, 20.0f,
            active ? 0xFFFFFF : 0xA0A0A7);
        AetherControl c{};
        c.rect = hit; c.kind = AetherControlKind::Tab; c.tab = tab;
        AetherPush(c);
    }

    void AetherDrawDots(float cx, float cy, AetherPopup popup) noexcept;

    void AetherDrawToggle(const wchar_t* label, float x, float cy, float rowWidth,
        bool& value, bool withDots = false, AetherPopup popup = AetherPopup::None) noexcept
    {
        AetherText(label, x, cy - 9.0f, rowWidth - 74.0f, 18.0f,
            0xD0D0D5, g_menuTextFormat);
        const float toggleX = x + rowWidth - 29.0f;
        const D2D1_RECT_F toggleHit = AetherRect(toggleX - 5.0f, cy - 10.0f, 34.0f, 20.0f);
        const bool hover = AetherHovered(toggleHit);
        const float trackX = toggleX, trackY = cy - 7.0f;
        const float knobX = value ? (trackX + 19.0f) : (trackX + 7.0f);
        // Minimal light switch: ON = white track with a dark knob; OFF = #101010 track with a white knob.
        AetherFill(trackX, trackY, 26.0f, 14.0f, 7.0f,
            value ? 0xFFFFFF : 0x101010, 1.0f);
        if (g_brushMenuText)
        {
            g_brushMenuText->SetColor(AetherRgb(value ? 0x101010 : 0xFFFFFF));
            const D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX, cy), 5.2f, 5.2f);
            g_d2dTarget->FillEllipse(&knob, g_brushMenuText);
        }
        if (hover) AetherStroke(toggleHit.left, toggleHit.top, toggleHit.right - toggleHit.left,
            toggleHit.bottom - toggleHit.top, 5.0f, 0xFFFFFF, 0.12f, 0.7f);
        AetherControl c{}; c.rect = toggleHit; c.kind = AetherControlKind::Toggle; c.value = &value;
        AetherPush(c);
        if (withDots && popup != AetherPopup::None)
            AetherDrawDots(x + rowWidth - 57.0f, cy, popup);
    }

    void AetherDrawDots(float cx, float cy, AetherPopup popup) noexcept
    {
        const D2D1_RECT_F hit = AetherRect(cx - 8.0f, cy - 8.0f, 20.0f, 16.0f);
        const bool hover = AetherHovered(hit) || g_aetherPopup == popup;
        if (hover) AetherFill(hit.left, hit.top, hit.right - hit.left, hit.bottom - hit.top,
            4.0f, 0xFFFFFF, 0.07f);
        const UINT32 color = hover ? 0xE8E8EC : 0x96969D;
        for (int i = 0; i < 3; ++i)
            AetherFill(cx - 3.0f + i * 4.5f, cy - 1.2f, 2.2f, 2.2f, 1.1f, color, 1.0f);
        AetherControl c{}; c.rect = hit; c.kind = AetherControlKind::Popup; c.popup = popup;
        AetherPush(c);
    }

    void AetherDrawSlider(const wchar_t* label, float x, float cy, float trackWidth,
        float& value, float minValue, float maxValue, float step, const wchar_t* suffix) noexcept
    {
        AetherText(label, x, cy - 9.0f, 94.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        const float tx = x + 101.0f;
        const float ty = cy - 1.5f;
        const float t = (maxValue > minValue) ? std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f) : 0.0f;
        AetherFill(tx, ty, trackWidth, 3.0f, 1.5f, 0x38383D, 1.0f);
        const D2D1_COLOR_F accent = AetherAccent();
        if (g_brushSliderFill) g_brushSliderFill->SetColor(accent);
        if (t > 0.0f)
        {
            const D2D1_ROUNDED_RECT fill{ AetherRect(tx, ty, trackWidth * t, 3.0f), 1.5f, 1.5f };
            g_d2dTarget->FillRoundedRectangle(&fill, g_brushSliderFill);
        }
        const float kx = tx + trackWidth * t;
        if (g_brushMenuText)
        {
            g_brushMenuText->SetColor(AetherRgb(0xF1F1F3));
            const D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(kx, cy), 4.5f, 4.5f);
            g_d2dTarget->FillEllipse(&knob, g_brushMenuText);
        }
        wchar_t valueText[48]{};
        if (suffix && wcscmp(suffix, L"%") == 0) swprintf_s(valueText, L"%.0f%%", value * 100.0f);
        else if (suffix && wcscmp(suffix, L"x") == 0) swprintf_s(valueText, L"%.2fx", value);
        else if (suffix && wcscmp(suffix, L"/s") == 0) swprintf_s(valueText, L"%.0f/s", value);
        else if (suffix && wcscmp(suffix, L"px") == 0) swprintf_s(valueText, L"%.0fpx", value);
        else if (suffix && wcscmp(suffix, L"sens") == 0) swprintf_s(valueText, L"%.2f", value);
        else if (suffix && wcscmp(suffix, L"s") == 0) swprintf_s(valueText, L"%.2fs", value);
        else swprintf_s(valueText, L"%.0f", value);
        const float valueX = tx + trackWidth + 8.0f;
        AetherFill(valueX, cy - 8.0f, 35.0f, 16.0f, 3.5f, 0x1D1D20, 1.0f);
        AetherStroke(valueX + 0.5f, cy - 7.5f, 34.0f, 15.0f, 3.5f, 0x2B2B30, 1.0f, 0.6f);
        AetherText(valueText, valueX + 1.0f, cy - 7.0f, 33.0f, 14.0f, 0xDADAE0,
            g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);

        AetherControl c{};
        c.rect = AetherRect(tx - 5.0f, cy - 9.0f, trackWidth + 10.0f, 18.0f);
        c.kind = AetherControlKind::Slider; c.value = &value;
        c.minimum = minValue; c.maximum = maxValue; c.step = step;
        AetherPush(c);
    }

    int AetherReadCycle(const void* ptr) noexcept
    {
        int value = 0;
        if (ptr) std::memcpy(&value, ptr, sizeof(value));
        return value;
    }

    void AetherDrawCycle(const wchar_t* label, float x, float cy, float boxX, float boxW,
        void* value, int count, const wchar_t* const* names) noexcept
    {
        AetherText(label, x, cy - 9.0f, boxX - x - 8.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        int index = (count > 0) ? std::clamp(AetherReadCycle(value), 0, count - 1) : 0;
        AetherFill(boxX, cy - 9.0f, boxW, 18.0f, 4.0f, 0x1C1C20, 1.0f);
        AetherStroke(boxX + 0.5f, cy - 8.5f, boxW - 1.0f, 17.0f, 4.0f, 0x303036, 1.0f, 0.7f);
        AetherText(names && count > 0 ? names[index] : L"-", boxX + 5.0f, cy - 8.0f,
            boxW - 16.0f, 16.0f, 0xE0E0E4, g_smallTextFormat);
        AetherText(L"›", boxX + boxW - 13.0f, cy - 8.0f, 10.0f, 16.0f,
            0xA8A8AE, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);
        AetherControl c{}; c.rect = AetherRect(boxX, cy - 10.0f, boxW, 20.0f);
        c.kind = AetherControlKind::Cycle; c.value = value; c.cycleCount = count;
        AetherPush(c);
    }

    void AetherDrawColor(const wchar_t* label, float x, float cy, float boxX,
        float& r, float& g, float& b) noexcept
    {
        AetherText(label, x, cy - 9.0f, boxX - x - 8.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        const D2D1_RECT_F swatch = AetherRect(boxX, cy - 7.0f, 18.0f, 14.0f);
        if (g_brushBoxColor)
        {
            g_brushBoxColor->SetColor(Color(std::clamp(r, 0.0f, 1.0f),
                std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), 1.0f));
            g_d2dTarget->FillRoundedRectangle(D2D1::RoundedRect(swatch, 3.0f, 3.0f), g_brushBoxColor);
        }
        AetherStroke(swatch.left, swatch.top, swatch.right - swatch.left,
            swatch.bottom - swatch.top, 3.0f, 0xFFFFFF, 0.25f, 0.6f);
        wchar_t hex[16]{};
        swprintf_s(hex, L"#%02X%02X%02X",
            static_cast<unsigned>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
            static_cast<unsigned>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
            static_cast<unsigned>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)));
        AetherText(hex, boxX + 23.0f, cy - 8.0f, 49.0f, 16.0f, 0xBFC0C6,
            g_codeTextFormat);
        AetherControl c{}; c.rect = AetherRect(boxX - 3.0f, cy - 10.0f, 77.0f, 20.0f);
        c.kind = AetherControlKind::Color; c.colorR = &r; c.colorG = &g; c.colorB = &b;
        AetherPush(c);
    }

    void AetherOpenColorPicker(const AetherControl& control) noexcept
    {
        if (!control.colorR || !control.colorG || !control.colorB) return;
        g_aetherColorTargetR = control.colorR;
        g_aetherColorTargetG = control.colorG;
        g_aetherColorTargetB = control.colorB;
        RGBToHSV(*g_aetherColorTargetR, *g_aetherColorTargetG, *g_aetherColorTargetB,
            g_aetherColorHue, g_aetherColorSaturation, g_aetherColorValue);

        constexpr float pickerW = 204.0f;
        constexpr float pickerH = 142.0f;
        const float px = std::clamp(control.rect.left - 224.0f, 138.0f, 426.0f);
        const float py = std::clamp(control.rect.top - 48.0f, 44.0f, 186.0f);
        g_aetherColorPickerRect = AetherRect(px, py, pickerW, pickerH);
        g_aetherColorSvRect = AetherRect(px + 10.0f, py + 34.0f, 140.0f, 78.0f);
        g_aetherColorHueRect = AetherRect(px + 156.0f, py + 34.0f, 13.0f, 78.0f);
        g_aetherColorPickerOpen = true;
        g_aetherColorDrag = AetherColorDrag::None;
        g_aetherWeaponIconColorDirty = false;
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    void AetherApplyPickedColor() noexcept
    {
        if (!g_aetherColorTargetR || !g_aetherColorTargetG || !g_aetherColorTargetB) return;
        HSVToRGB(g_aetherColorHue, g_aetherColorSaturation, g_aetherColorValue,
            *g_aetherColorTargetR, *g_aetherColorTargetG, *g_aetherColorTargetB);
        if (g_aetherColorTargetR == &Menu::weaponIconColorR)
            g_aetherWeaponIconColorDirty = true;
        g_configDirtyDuringDrag = true;
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    void AetherUpdateColorPicker(float localX, float localY) noexcept
    {
        if (g_aetherColorDrag == AetherColorDrag::SaturationValue)
        {
            const float w = g_aetherColorSvRect.right - g_aetherColorSvRect.left;
            const float h = g_aetherColorSvRect.bottom - g_aetherColorSvRect.top;
            if (w > 0.0f && h > 0.0f)
            {
                g_aetherColorSaturation = std::clamp((localX - g_aetherColorSvRect.left) / w, 0.0f, 1.0f);
                g_aetherColorValue = 1.0f - std::clamp((localY - g_aetherColorSvRect.top) / h, 0.0f, 1.0f);
            }
        }
        else if (g_aetherColorDrag == AetherColorDrag::Hue)
        {
            const float h = g_aetherColorHueRect.bottom - g_aetherColorHueRect.top;
            if (h > 0.0f)
                g_aetherColorHue = std::clamp((localY - g_aetherColorHueRect.top) / h, 0.0f, 1.0f);
        }
        AetherApplyPickedColor();
    }

    void AetherFillSolidRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color) noexcept
    {
        if (!g_d2dTarget || !g_brushMenuBack) return;
        g_brushMenuBack->SetColor(color);
        g_d2dTarget->FillRectangle(rect, g_brushMenuBack);
    }

    void AetherDrawColorPicker() noexcept
    {
        if (!g_aetherColorPickerOpen || !g_aetherColorTargetR ||
            !g_aetherColorTargetG || !g_aetherColorTargetB) return;

        const auto& p = g_aetherColorPickerRect;
        const float px = p.left, py = p.top;
        AetherFill(px - 2.0f, py + 2.0f, p.right - p.left + 4.0f,
            p.bottom - p.top + 3.0f, 8.0f, 0x000000, 0.45f);
        AetherCard(px, py, p.right - p.left, p.bottom - p.top, L"Color picker");

        // Saturation/value square, rendered as small solid tiles so it needs no extra D2D resources.
        constexpr int columns = 28;
        constexpr int rows = 16;
        const float cellW = (g_aetherColorSvRect.right - g_aetherColorSvRect.left) / columns;
        const float cellH = (g_aetherColorSvRect.bottom - g_aetherColorSvRect.top) / rows;
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                const float sat = (static_cast<float>(column) + 0.5f) / columns;
                const float val = 1.0f - (static_cast<float>(row) + 0.5f) / rows;
                float rr = 0.0f, gg = 0.0f, bb = 0.0f;
                HSVToRGB(g_aetherColorHue, sat, val, rr, gg, bb);
                const D2D1_RECT_F cell = AetherRect(
                    g_aetherColorSvRect.left + column * cellW,
                    g_aetherColorSvRect.top + row * cellH,
                    cellW + 0.35f, cellH + 0.35f);
                AetherFillSolidRect(cell, Color(rr, gg, bb, 1.0f));
            }
        }
        AetherStroke(g_aetherColorSvRect.left, g_aetherColorSvRect.top,
            g_aetherColorSvRect.right - g_aetherColorSvRect.left,
            g_aetherColorSvRect.bottom - g_aetherColorSvRect.top, 2.0f,
            0xFFFFFF, 0.45f, 0.7f);

        // Hue strip.
        constexpr int hueSteps = 28;
        const float hueStepH = (g_aetherColorHueRect.bottom - g_aetherColorHueRect.top) / hueSteps;
        for (int i = 0; i < hueSteps; ++i)
        {
            float rr = 0.0f, gg = 0.0f, bb = 0.0f;
            HSVToRGB((static_cast<float>(i) + 0.5f) / hueSteps, 1.0f, 1.0f, rr, gg, bb);
            const D2D1_RECT_F cell = AetherRect(g_aetherColorHueRect.left,
                g_aetherColorHueRect.top + i * hueStepH,
                g_aetherColorHueRect.right - g_aetherColorHueRect.left,
                hueStepH + 0.35f);
            AetherFillSolidRect(cell, Color(rr, gg, bb, 1.0f));
        }
        AetherStroke(g_aetherColorHueRect.left, g_aetherColorHueRect.top,
            g_aetherColorHueRect.right - g_aetherColorHueRect.left,
            g_aetherColorHueRect.bottom - g_aetherColorHueRect.top, 2.0f,
            0xFFFFFF, 0.45f, 0.7f);

        // Current color preview and the selected point.
        float rr = 0.0f, gg = 0.0f, bb = 0.0f;
        HSVToRGB(g_aetherColorHue, g_aetherColorSaturation, g_aetherColorValue, rr, gg, bb);
        const D2D1_RECT_F preview = AetherRect(px + 178.0f, py + 34.0f, 16.0f, 78.0f);
        AetherFillSolidRect(preview, Color(rr, gg, bb, 1.0f));
        AetherStroke(preview.left, preview.top, preview.right - preview.left,
            preview.bottom - preview.top, 2.0f, 0xFFFFFF, 0.55f, 0.7f);

        const float markerX = g_aetherColorSvRect.left + g_aetherColorSaturation *
            (g_aetherColorSvRect.right - g_aetherColorSvRect.left);
        const float markerY = g_aetherColorSvRect.top + (1.0f - g_aetherColorValue) *
            (g_aetherColorSvRect.bottom - g_aetherColorSvRect.top);
        if (g_brushMenuBorder && g_brushMenuText)
        {
            const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(markerX, markerY), 5.0f, 5.0f);
            g_brushMenuBorder->SetColor(AetherRgb(0x101010));
            g_d2dTarget->DrawEllipse(&outer, g_brushMenuBorder, 2.0f);
            const D2D1_ELLIPSE inner = D2D1::Ellipse(D2D1::Point2F(markerX, markerY), 3.0f, 3.0f);
            g_brushMenuText->SetColor(AetherRgb(0xFFFFFF));
            g_d2dTarget->DrawEllipse(&inner, g_brushMenuText, 1.0f);
        }
        const float hueMarkerY = g_aetherColorHueRect.top + g_aetherColorHue *
            (g_aetherColorHueRect.bottom - g_aetherColorHueRect.top);
        AetherFill(px + 154.0f, hueMarkerY - 1.0f, 17.0f, 2.0f, 1.0f, 0xFFFFFF, 1.0f);
        AetherStroke(px + 153.5f, hueMarkerY - 1.5f, 18.0f, 3.0f,
            1.0f, 0x101010, 1.0f, 0.6f);

        wchar_t hex[16]{};
        swprintf_s(hex, L"#%02X%02X%02X",
            static_cast<unsigned>(std::round(std::clamp(rr, 0.0f, 1.0f) * 255.0f)),
            static_cast<unsigned>(std::round(std::clamp(gg, 0.0f, 1.0f) * 255.0f)),
            static_cast<unsigned>(std::round(std::clamp(bb, 0.0f, 1.0f) * 255.0f)));
        AetherText(L"Drag to choose", px + 10.0f, py + 116.0f, 100.0f, 16.0f,
            0xA8A8AE, g_smallTextFormat);
        AetherText(hex, px + 112.0f, py + 116.0f, 82.0f, 16.0f,
            0xE0E0E4, g_codeTextFormat, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    void AetherDrawKeyValue(float x, float cy, float w, const std::wstring& value,
        bool activeCapture = false) noexcept
    {
        AetherFill(x, cy - 9.0f, w, 18.0f, 4.0f, 0x1D1D20, 1.0f);
        AetherStroke(x + 0.5f, cy - 8.5f, w - 1.0f, 17.0f, 4.0f,
            activeCapture ? 0x77777F : 0x303036, 1.0f, 0.7f);
        if (activeCapture)
            AetherText(L"Press a key...", x + 4.0f, cy - 8.0f, w - 8.0f, 16.0f,
                0xA8A8AE, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);
        else
            AetherText(value, x + 4.0f, cy - 8.0f, w - 8.0f, 16.0f,
                0xE2E2E6, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void AetherDrawPopupRowToggle(const wchar_t* label, float y, bool& value) noexcept
    {
        AetherDrawToggle(label, g_aetherPopupRect.left + 10.0f, y,
            (g_aetherPopupRect.right - g_aetherPopupRect.left) - 20.0f, value);
    }

    void AetherDrawPopupRowSlider(const wchar_t* label, float y, float& value,
        float minimum, float maximum, float step, const wchar_t* suffix) noexcept
    {
        const float x = g_aetherPopupRect.left + 10.0f;
        const float trackX = g_aetherPopupRect.left + 125.0f;
        const float trackW = 92.0f;
        AetherText(label, x, y - 9.0f, 108.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        const float t = (maximum > minimum) ? std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f) : 0.0f;
        AetherFill(trackX, y - 1.5f, trackW, 3.0f, 1.5f, 0x38383D, 1.0f);
        if (g_brushSliderFill) g_brushSliderFill->SetColor(AetherAccent());
        if (t > 0.0f)
        {
            const D2D1_ROUNDED_RECT fill{ AetherRect(trackX, y - 1.5f, trackW * t, 3.0f), 1.5f, 1.5f };
            g_d2dTarget->FillRoundedRectangle(&fill, g_brushSliderFill);
        }
        if (g_brushMenuText)
        {
            g_brushMenuText->SetColor(AetherRgb(0xF1F1F3));
            const D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(trackX + trackW * t, y), 4.2f, 4.2f);
            g_d2dTarget->FillEllipse(&knob, g_brushMenuText);
        }
        wchar_t text[32]{};
        if (suffix && wcscmp(suffix, L"%") == 0) swprintf_s(text, L"%.0f%%", value * 100.0f);
        else if (suffix && wcscmp(suffix, L"x") == 0) swprintf_s(text, L"%.2f", value);
        else if (suffix && wcscmp(suffix, L"px") == 0) swprintf_s(text, L"%.1fpx", value);
        else if (suffix && wcscmp(suffix, L"m") == 0) swprintf_s(text, L"%.0fm", value);
        else swprintf_s(text, L"%.0f", value);
        AetherText(text, trackX + trackW + 5.0f, y - 8.0f, 36.0f, 16.0f,
            0xDADAE0, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);
        AetherControl c{}; c.rect = AetherRect(trackX - 5.0f, y - 9.0f, trackW + 10.0f, 18.0f);
        c.kind = AetherControlKind::Slider; c.value = &value;
        c.minimum = minimum; c.maximum = maximum; c.step = step;
        AetherPush(c);
    }

    void AetherDrawPopupRowCycle(const wchar_t* label, float y, void* value,
        int count, const wchar_t* const* names) noexcept
    {
        const float x = g_aetherPopupRect.left + 10.0f;
        AetherDrawCycle(label, x, y, g_aetherPopupRect.left + 125.0f,
            138.0f, value, count, names);
    }

    void AetherDrawPopupRowColor(const wchar_t* label, float y,
        float& r, float& g, float& b) noexcept
    {
        AetherDrawColor(label, g_aetherPopupRect.left + 10.0f, y,
            g_aetherPopupRect.left + 205.0f, r, g, b);
    }

    void AetherSetPopup(AetherPopup popup, const D2D1_RECT_F& anchor) noexcept
    {
        g_aetherPopup = popup;
        g_aetherPopupAnchor = anchor;
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    void AetherDrawPopup() noexcept
    {
        if (g_aetherPopup == AetherPopup::None) return;

        static const wchar_t* fillStyles[] = { L"Solid", L"Center fade", L"Top to bottom", L"Bottom to top" };
        static const wchar_t* barPositions[] = { L"Left", L"Right", L"Top", L"Bottom" };
        static const wchar_t* snapAnchors[] = { L"Top", L"Center", L"Bottom", L"Left", L"Right" };
        static const wchar_t* accentNames[] = { L"Neutral", L"Mint", L"Blue", L"Violet", L"Amber" };
        static const wchar_t* pulseModes[] = { L"Normal", L"Pulse" };
        static const wchar_t* indicatorPositions[] = { L"Center", L"Screen left", L"Screen right" };
        static const wchar_t* radarShapes[] = { L"Circle", L"Rounded rect" };
        (void)accentNames;

        const float popupW = 282.0f;
        int rows = 1;
        const wchar_t* title = L"Options";
        switch (g_aetherPopup)
        {
        case AetherPopup::AimActivation: title = L"Aim Activation"; rows = 1; break;
        case AetherPopup::Box: title = L"Box ESP"; rows = (Menu::filledBoxStyle == Menu::FilledBoxStyle::Solid) ? 4 : 5; break;
        case AetherPopup::Health: title = L"Health Bar"; rows = Menu::healthGradient ? 6 : 5; break;
        case AetherPopup::Armor: title = L"Armor Bar"; rows = 3; break;
        case AetherPopup::Snapline: title = L"Snapline"; rows = 2; break;
        case AetherPopup::Skeleton: title = L"Skeleton"; rows = 2; break;
        case AetherPopup::PlayerName: title = L"Player Name"; rows = 2; break;
        case AetherPopup::WeaponIcon: title = L"Weapon Icon"; rows = 2; break;
        case AetherPopup::Distance: title = L"Distance"; rows = 2; break;
        case AetherPopup::HitMarker: title = L"Hitmarker"; rows = 3; break;
        case AetherPopup::DamagePopup: title = L"Damage Popup"; rows = 5; break;
        case AetherPopup::FovCircle: title = L"FOV Circle"; rows = 3; break;
        case AetherPopup::OffscreenArrows: title = L"Off-screen Arrows"; rows = 5; break;
        case AetherPopup::Radar: title = L"Radar Settings"; rows = 5; break;
        default: break;
        }
        const float popupH = 34.0f + rows * 25.0f + 8.0f;
        float px = 340.0f;
        float py = 72.0f;
        if (g_aetherPopupAnchor.left > 0.0f)
        {
            px = std::clamp(g_aetherPopupAnchor.left - popupW - 8.0f, 138.0f, 634.0f - popupW);
            py = std::clamp(g_aetherPopupAnchor.top - 10.0f, 40.0f, 328.0f - popupH);
            if (px < 138.0f) px = 634.0f - popupW - 8.0f;
        }
        g_aetherPopupRect = AetherRect(px, py, popupW, popupH);
        AetherFill(px - 2.0f, py + 2.0f, popupW + 4.0f, popupH + 3.0f, 8.0f, 0x000000, 0.42f);
        AetherCard(px, py, popupW, popupH, title);

        float rowY = py + 43.0f;
        switch (g_aetherPopup)
        {
        case AetherPopup::AimActivation:
            AetherText(L"Activation key", px + 10.0f, rowY - 9.0f, 105.0f, 18.0f,
                0xD0D0D5, g_menuTextFormat);
            AetherDrawKeyValue(px + 125.0f, rowY, 140.0f,
                GetAimKeyDisplayName(AimConfig::activationKey),
                g_keyCaptureTarget == KeyCaptureTarget::AimActivation);
            {
                AetherControl c{}; c.rect = AetherRect(px + 120.0f, rowY - 10.0f, 150.0f, 20.0f);
                c.kind = AetherControlKind::AimKey; AetherPush(c);
            }
            break;
        case AetherPopup::Box:
            AetherDrawPopupRowCycle(L"Fill style", rowY,
                &Menu::filledBoxStyle, 4, fillStyles); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Fill opacity", rowY, Menu::filledOpacity,
                0.05f, 1.0f, 0.01f, L"%"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Corner length", rowY, Menu::boxCornerLength,
                10.0f, 100.0f, 1.0f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Box color", rowY,
                Menu::boxColorR, Menu::boxColorG, Menu::boxColorB); rowY += 25.0f;
            if (Menu::filledBoxStyle != Menu::FilledBoxStyle::Solid)
                AetherDrawPopupRowColor(L"Fill color", rowY,
                    Menu::filledColorR, Menu::filledColorG, Menu::filledColorB);
            break;
        case AetherPopup::Health:
            AetherDrawPopupRowSlider(L"Bar width", rowY, Menu::healthBarWidth,
                1.0f, 12.0f, 0.1f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowCycle(L"Position", rowY,
                &Menu::healthPosition, 4, barPositions); rowY += 25.0f;
            AetherDrawPopupRowToggle(L"Gradient", rowY, Menu::healthGradient); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Health color", rowY,
                Menu::healthColorR, Menu::healthColorG, Menu::healthColorB); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Background", rowY,
                Menu::healthBackgroundColorR, Menu::healthBackgroundColorG, Menu::healthBackgroundColorB); rowY += 25.0f;
            if (Menu::healthGradient)
                AetherDrawPopupRowColor(L"Gradient color", rowY,
                    Menu::healthGradientColorR, Menu::healthGradientColorG, Menu::healthGradientColorB);
            break;
        case AetherPopup::Armor:
            AetherDrawPopupRowSlider(L"Bar width", rowY, Menu::armorBarWidth,
                1.0f, 12.0f, 0.1f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowCycle(L"Position", rowY,
                &Menu::armorPosition, 4, barPositions); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Armor color", rowY,
                Menu::armorColorR, Menu::armorColorG, Menu::armorColorB);
            break;
        case AetherPopup::Snapline:
            AetherDrawPopupRowCycle(L"Anchor", rowY,
                &Menu::snaplineAnchor, 5, snapAnchors); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Line color", rowY,
                Menu::snaplineColorR, Menu::snaplineColorG, Menu::snaplineColorB);
            break;
        case AetherPopup::Skeleton:
            AetherDrawPopupRowSlider(L"Opacity", rowY, Menu::skeletonOpacity,
                0.05f, 1.0f, 0.01f, L"%"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Line color", rowY,
                Menu::skeletonColorR, Menu::skeletonColorG, Menu::skeletonColorB);
            break;
        case AetherPopup::PlayerName:
            AetherDrawPopupRowSlider(L"Text scale", rowY, Menu::playerNameScale,
                0.50f, 2.00f, 0.05f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Text color", rowY,
                Menu::playerNameColorR, Menu::playerNameColorG, Menu::playerNameColorB);
            break;
        case AetherPopup::WeaponIcon:
            AetherDrawPopupRowSlider(L"Icon scale", rowY, Menu::weaponIconScale,
                0.50f, 2.00f, 0.05f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Icon color", rowY,
                Menu::weaponIconColorR, Menu::weaponIconColorG, Menu::weaponIconColorB);
            break;
        case AetherPopup::Distance:
            AetherDrawPopupRowSlider(L"Text scale", rowY, Menu::distanceScale,
                0.50f, 2.00f, 0.05f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Text color", rowY,
                Menu::distanceColorR, Menu::distanceColorG, Menu::distanceColorB);
            break;
        case AetherPopup::HitMarker:
            AetherDrawPopupRowSlider(L"Scale", rowY, Menu::hitMarkerScale,
                0.35f, 1.0f, 0.01f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Volume", rowY, Menu::hitMarkerVolume,
                0.0f, 2.0f, 0.01f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Color", rowY,
                Menu::hitMarkerColorR, Menu::hitMarkerColorG, Menu::hitMarkerColorB);
            break;
        case AetherPopup::DamagePopup:
            AetherDrawPopupRowSlider(L"Text size", rowY, Menu::damagePopupScale,
                0.65f, 3.0f, 0.01f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Boldness", rowY, Menu::damagePopupBoldness,
                0.0f, 1.0f, 0.01f, L"%"); rowY += 25.0f;
            AetherDrawPopupRowToggle(L"Outline", rowY, Menu::damagePopupOutline); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Text color", rowY,
                Menu::damagePopupColorR, Menu::damagePopupColorG, Menu::damagePopupColorB); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Outline color", rowY,
                Menu::damagePopupOutlineColorR, Menu::damagePopupOutlineColorG, Menu::damagePopupOutlineColorB);
            break;
        case AetherPopup::FovCircle:
            AetherDrawPopupRowCycle(L"Animation", rowY,
                &AimConfig::fovCircleAnimation, 2, pulseModes); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Line thickness", rowY, AimConfig::fovCircleThickness,
                0.75f, 4.0f, 0.05f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Circle color", rowY,
                AimConfig::fovCircleColorR, AimConfig::fovCircleColorG, AimConfig::fovCircleColorB);
            break;
        case AetherPopup::OffscreenArrows:
            AetherDrawPopupRowCycle(L"Position", rowY,
                &Menu::offscreenPosition, 3, indicatorPositions); rowY += 25.0f;
            AetherDrawPopupRowCycle(L"Animation", rowY,
                &Menu::offscreenAnimation, 2, pulseModes); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Arrow scale", rowY, Menu::offscreenScale,
                0.55f, 2.5f, 0.05f, L"x"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Position offset", rowY, Menu::offscreenEdgeMargin,
                20.0f, 120.0f, 1.0f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Arrow color", rowY,
                Menu::offscreenColorR, Menu::offscreenColorG, Menu::offscreenColorB);
            break;
        case AetherPopup::Radar:
            AetherDrawPopupRowCycle(L"Shape", rowY,
                &Menu::radarShape, 2, radarShapes); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Radar size", rowY, Menu::radarSize,
                120.0f, 320.0f, 2.0f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Show distance", rowY, Menu::radarRangeMeters,
                20.0f, 300.0f, 5.0f, L"m"); rowY += 25.0f;
            AetherDrawPopupRowSlider(L"Dot size", rowY, Menu::radarDotSize,
                2.0f, 10.0f, 0.5f, L"px"); rowY += 25.0f;
            AetherDrawPopupRowColor(L"Enemy dot color", rowY,
                Menu::radarDotColorR, Menu::radarDotColorG, Menu::radarDotColorB);
            break;
        default: break;
        }
    }

    void AetherDrawAimbotPage() noexcept
    {
        static const wchar_t* teamModes[] = { L"Enemies", L"Both teams" };
        (void)teamModes;
        AetherText(L"GENERAL", 147.0f, 53.0f, 90.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherText(L"CONTROL", 395.0f, 53.0f, 90.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherCard(138.0f, 70.0f, 242.0f, 252.0f, L"Aimbot");
        AetherCard(387.0f, 70.0f, 240.0f, 252.0f, L"Target & Recoil");

        AetherDrawToggle(L"", 146.0f, 85.0f, 228.0f, AimConfig::enabled);
        AetherDrawSlider(L"FOV", 146.0f, 116.0f, 72.0f,
            AimConfig::fovRadius, 20.0f, 400.0f, 1.0f, L"px");
        AetherDrawSlider(L"Lerp speed", 146.0f, 140.0f, 72.0f,
            AimConfig::lerpSpeed, 1.0f, 60.0f, 1.0f, L"/s");
        AetherDrawSlider(L"Sensitivity", 146.0f, 164.0f, 72.0f,
            AimConfig::sensitivity, 0.10f, 10.0f, 0.01f, L"sens");
        AetherText(L"Activation key", 146.0f, 180.0f, 96.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        AetherDrawKeyValue(248.0f, 189.0f, 94.0f,
            GetAimKeyDisplayName(AimConfig::activationKey),
            g_keyCaptureTarget == KeyCaptureTarget::AimActivation);
        AetherDrawDots(354.0f, 189.0f, AetherPopup::AimActivation);
        AetherDrawToggle(L"Show FOV circle", 146.0f, 222.0f, 228.0f,
            AimConfig::showFovCircle, true, AetherPopup::FovCircle);
        AetherText(L"Uses the FOV radius above in pixels.",
            146.0f, 245.0f, 220.0f, 28.0f, 0x77777F, g_smallTextFormat);

        AetherDrawToggle(L"Target both teams", 395.0f, 116.0f, 222.0f, AimConfig::deathmatchMode);
        AetherDrawToggle(L"Recoil control", 395.0f, 140.0f, 222.0f, AimConfig::rcsEnabled);
        AetherDrawSlider(L"RCS strength", 395.0f, 164.0f, 68.0f,
            AimConfig::rcsStrength, 0.0f, 2.0f, 0.01f, L"x");
        AetherLine(397.0f, 190.0f, 617.0f, 190.0f, 0x2B2B30, 1.0f, 0.7f);
        AetherText(L"Aim and recoil options are saved automatically.",
            398.0f, 200.0f, 214.0f, 34.0f, 0x85858C, g_smallTextFormat);
        AetherText(L"Activation key can be changed from the dots menu.",
            398.0f, 238.0f, 214.0f, 34.0f, 0x68686F, g_smallTextFormat);
    }

    void AetherDrawVisualsPage() noexcept
    {
        AetherText(L"VISUALS", 147.0f, 53.0f, 90.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherText(L"FEEDBACK", 395.0f, 53.0f, 90.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherCard(138.0f, 70.0f, 242.0f, 252.0f, L"ESP Elements");
        AetherCard(387.0f, 70.0f, 240.0f, 252.0f, L"Hit Feedback");

        float y = 109.0f;
        AetherDrawToggle(L"Box ESP", 146.0f, y, 220.0f, Menu::box, true, AetherPopup::Box); y += 22.0f;
        AetherDrawToggle(L"Filled boxes", 146.0f, y, 220.0f, Menu::filledBox); y += 22.0f;
        AetherDrawToggle(L"Health bar", 146.0f, y, 220.0f, Menu::health, true, AetherPopup::Health); y += 22.0f;
        AetherDrawToggle(L"Armor bar", 146.0f, y, 220.0f, Menu::armor, true, AetherPopup::Armor); y += 22.0f;
        AetherDrawToggle(L"Snapline", 146.0f, y, 220.0f, Menu::snapline, true, AetherPopup::Snapline); y += 22.0f;
        AetherDrawToggle(L"Skeleton", 146.0f, y, 220.0f, Menu::skeleton, true, AetherPopup::Skeleton); y += 22.0f;
        AetherDrawToggle(L"Player name", 146.0f, y, 220.0f, Menu::playerName, true, AetherPopup::PlayerName); y += 22.0f;
        AetherDrawToggle(L"Weapon icon", 146.0f, y, 220.0f, Menu::weaponIcon, true, AetherPopup::WeaponIcon); y += 22.0f;
        AetherDrawToggle(L"Distance", 146.0f, y, 220.0f, Menu::distance, true, AetherPopup::Distance);

        AetherDrawToggle(L"Hitmarker", 395.0f, 116.0f, 222.0f, Menu::hitMarker, true, AetherPopup::HitMarker);
        AetherDrawToggle(L"Hitmarker sound", 395.0f, 140.0f, 222.0f, Menu::hitMarkerSound);
        AetherDrawToggle(L"Damage popup", 395.0f, 164.0f, 222.0f, Menu::damagePopup, true, AetherPopup::DamagePopup);
        AetherDrawToggle(L"Show both teams", 395.0f, 188.0f, 222.0f, AimConfig::deathmatchMode);
        AetherLine(397.0f, 216.0f, 617.0f, 216.0f, 0x2B2B30, 1.0f, 0.7f);
        AetherText(L"Open the dots beside an ESP item for its extra settings, colors and sizing.",
            398.0f, 224.0f, 212.0f, 54.0f, 0x85858C, g_smallTextFormat);
        AetherText(L"Changes save automatically.", 398.0f, 284.0f, 212.0f, 18.0f,
            0x68686F, g_smallTextFormat);
    }

    void AetherDrawMiscPage() noexcept
    {
        AetherText(L"MISCELLANEOUS", 147.0f, 53.0f, 150.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherText(L"RADAR & INDICATORS", 395.0f, 53.0f, 180.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherCard(138.0f, 70.0f, 242.0f, 252.0f, L"Player Radar");
        AetherCard(387.0f, 70.0f, 240.0f, 252.0f, L"Off-screen Visuals");

        AetherDrawToggle(L"Enable radar", 146.0f, 112.0f, 220.0f,
            Menu::radarEnabled, true, AetherPopup::Radar);
        AetherText(L"Tracks eligible players around your position, including players behind you. Drag the three-dot handle while the menu is open.",
            146.0f, 140.0f, 216.0f, 66.0f, 0x85858C, g_smallTextFormat);
        AetherLine(146.0f, 222.0f, 370.0f, 222.0f, 0x2B2B30, 1.0f, 0.7f);
        AetherText(Menu::radarShape == 0 ? L"Shape: Circle" : L"Shape: Rounded rectangle",
            146.0f, 236.0f, 216.0f, 18.0f, 0xD0D0D5, g_smallTextFormat);
        wchar_t radarSummary[96]{};
        swprintf_s(radarSummary, L"%.0f px  /  %.0f m  /  %.1f px dots",
            Menu::radarSize, Menu::radarRangeMeters, Menu::radarDotSize);
        AetherText(radarSummary, 146.0f, 260.0f, 216.0f, 18.0f, 0xA8A8AE, g_smallTextFormat);
        AetherText(L"Use the dots for shape, size, range and dot color.",
            146.0f, 286.0f, 216.0f, 25.0f, 0x68686F, g_smallTextFormat);

        AetherDrawToggle(L"Off-screen arrows", 395.0f, 112.0f, 220.0f,
            Menu::offscreenIndicators, true, AetherPopup::OffscreenArrows);
        AetherLine(397.0f, 139.0f, 617.0f, 139.0f, 0x2B2B30, 1.0f, 0.7f);
        AetherText(L"Animation", 398.0f, 157.0f, 84.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        AetherText(Menu::offscreenAnimation == 1 ? L"Pulse" : L"Normal",
            488.0f, 157.0f, 126.0f, 18.0f, 0xA8A8AE, g_smallTextFormat);
        static const wchar_t* positionLabels[] = { L"Center", L"Screen left", L"Screen right" };
        AetherText(L"Position", 398.0f, 180.0f, 80.0f, 18.0f, 0xD0D0D5, g_smallTextFormat);
        AetherText(positionLabels[std::clamp(Menu::offscreenPosition, 0, 2)],
            488.0f, 180.0f, 126.0f, 18.0f, 0xA8A8AE, g_smallTextFormat);
        AetherText(L"Use the dots to change placement, color, scale, spacing and animation.",
            398.0f, 210.0f, 212.0f, 48.0f, 0x85858C, g_smallTextFormat);
        AetherText(L"Changes save automatically.", 398.0f, 286.0f, 212.0f, 18.0f,
            0x68686F, g_smallTextFormat);
    }

    std::wstring AetherFormatHotkey(const HotkeyBind& bind)
    {
        std::wstring value;
        if (bind.modifiers & MOD_CONTROL) value += L"CTRL + ";
        if (bind.modifiers & MOD_SHIFT) value += L"SHIFT + ";
        if (bind.modifiers & MOD_ALT) value += L"ALT + ";
        if (bind.modifiers & MOD_WIN) value += L"WIN + ";
        value += GetAimKeyDisplayName(bind.key);
        return value;
    }

    void AetherDrawHotkeyRow(const wchar_t* label, float x, float cy, HotkeyBind& bind,
        KeyCaptureTarget captureTarget) noexcept
    {
        AetherText(label, x, cy - 9.0f, 102.0f, 18.0f, 0xD0D0D5, g_menuTextFormat);
        const D2D1_RECT_F valueRect = AetherRect(x + 108.0f, cy - 9.0f, 112.0f, 18.0f);
        const bool waiting = g_keyCaptureTarget == captureTarget;
        AetherDrawKeyValue(valueRect.left, cy, valueRect.right - valueRect.left,
            AetherFormatHotkey(bind), waiting);
        AetherControl c{}; c.rect = valueRect;
        c.kind = captureTarget == KeyCaptureTarget::MenuToggle
            ? AetherControlKind::MenuHotkey : AetherControlKind::ExitHotkey;
        AetherPush(c);
    }

    void AetherDrawButton(const wchar_t* label, float x, float y, float w, float h,
        AetherControlKind kind, bool danger = false) noexcept
    {
        const D2D1_RECT_F rect = AetherRect(x, y, w, h);
        const bool hover = AetherHovered(rect);
        AetherFill(rect.left, rect.top, w, h, 4.0f,
            danger ? (hover ? 0x5A292D : 0x3B2023) : (hover ? 0x39393F : 0x28282D), 1.0f);
        AetherStroke(rect.left + 0.5f, rect.top + 0.5f, w - 1.0f, h - 1.0f,
            4.0f, danger ? 0xA84A52 : 0x44444B, danger ? 0.45f : 0.7f, 0.7f);
        AetherText(label, rect.left + 5.0f, rect.top + 1.0f, w - 10.0f, h - 2.0f,
            danger ? 0xF0C6C8 : 0xE4E4E8, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_CENTER);
        AetherControl c{}; c.rect = rect; c.kind = kind; AetherPush(c);
    }

    void AetherDrawSettingsPage() noexcept
    {
        // Use the whole content surface for Settings instead of nesting two
        // small cards inside it. This gives the page the same expanded feel as
        // the Aether reference while keeping controls aligned in two columns.
        AetherText(L"SETTINGS", 147.0f, 53.0f, 110.0f, 13.0f, 0x9C9CA2, g_smallTextFormat);
        AetherText(L"APPEARANCE", 147.0f, 79.0f, 150.0f, 14.0f, 0xD5D5DA, g_menuTextFormat);
        AetherText(L"APPLICATION", 395.0f, 79.0f, 150.0f, 14.0f, 0xD5D5DA, g_menuTextFormat);
        AetherLine(146.0f, 96.0f, 370.0f, 96.0f, 0x303036, 1.0f, 0.75f);
        AetherLine(395.0f, 96.0f, 617.0f, 96.0f, 0x303036, 1.0f, 0.75f);
        AetherLine(382.0f, 76.0f, 382.0f, 310.0f, 0x303036, 1.0f, 0.65f);

        AetherDrawColor(L"Accent color", 146.0f, 120.0f, 314.0f,
            g_aetherAccentR, g_aetherAccentG, g_aetherAccentB);
        AetherDrawSlider(L"Menu opacity", 146.0f, 153.0f, 74.0f,
            g_aetherOpacity, 0.70f, 1.0f, 0.01f, L"%");
        AetherDrawSlider(L"Panel opacity", 146.0f, 187.0f, 74.0f,
            g_aetherPanelOpacity, 0.70f, 1.0f, 0.01f, L"%");
        AetherDrawSlider(L"Border strength", 146.0f, 221.0f, 74.0f,
            g_aetherBorderOpacity, 0.0f, 0.35f, 0.01f, L"%");
        AetherDrawButton(L"Reset appearance", 146.0f, 278.0f, 124.0f, 22.0f,
            AetherControlKind::ResetAppearance);

        AetherDrawHotkeyRow(L"Open / close menu", 395.0f, 120.0f,
            g_menuToggleBind, KeyCaptureTarget::MenuToggle);
        AetherDrawHotkeyRow(L"Exit overlay", 395.0f, 157.0f,
            g_exitBind, KeyCaptureTarget::ExitOverlay);
        AetherText(L"Exit shortcut requires two presses.", 395.0f, 180.0f, 220.0f, 16.0f,
            0x77777F, g_smallTextFormat);
        AetherDrawButton(L"Exit overlay now", 395.0f, 210.0f, 112.0f, 22.0f,
            AetherControlKind::ExitApp, true);
        AetherDrawButton(L"Reset hotkeys", 513.0f, 210.0f, 104.0f, 22.0f,
            AetherControlKind::ResetHotkeys);
        AetherLine(395.0f, 248.0f, 617.0f, 248.0f, 0x303036, 1.0f, 0.65f);
        AetherText(L"Settings save automatically to AppData.",
            395.0f, 260.0f, 215.0f, 18.0f, 0x85858C, g_smallTextFormat);
        AetherText(L"Use the search icon to find any setting.",
            395.0f, 285.0f, 215.0f, 18.0f, 0x68686F, g_smallTextFormat);
    }

    struct AetherSearchEntry
    {
        const wchar_t* label;
        const wchar_t* keywords;
        MenuTab tab;
        AetherPopup popup;
    };

    static constexpr AetherSearchEntry kAetherSearchEntries[] = {
        { L"Aimbot", L"enable aim", MenuTab::Aimbot, AetherPopup::None },
        { L"FOV", L"field of view radius", MenuTab::Aimbot, AetherPopup::None },
        { L"Lerp speed", L"interpolation aim smooth speed", MenuTab::Aimbot, AetherPopup::None },
        { L"Sensitivity", L"mouse sens", MenuTab::Aimbot, AetherPopup::None },
        { L"Activation key", L"aim key bind", MenuTab::Aimbot, AetherPopup::AimActivation },
        { L"Target both teams", L"deathmatch team", MenuTab::Aimbot, AetherPopup::None },
        { L"Recoil control", L"rcs recoil", MenuTab::Aimbot, AetherPopup::None },
        { L"RCS strength", L"recoil scale", MenuTab::Aimbot, AetherPopup::None },
        { L"Box ESP", L"boxes corner fill opacity color fill style", MenuTab::ESP, AetherPopup::Box },
        { L"Health bar", L"health position width gradient color background", MenuTab::ESP, AetherPopup::Health },
        { L"Armor bar", L"armor position width color", MenuTab::ESP, AetherPopup::Armor },
        { L"Snapline", L"line anchor color", MenuTab::ESP, AetherPopup::Snapline },
        { L"Skeleton", L"bones line opacity color", MenuTab::ESP, AetherPopup::Skeleton },
        { L"Player name", L"name text size scale position color", MenuTab::ESP, AetherPopup::PlayerName },
        { L"Weapon icon", L"weapon icon size scale position color", MenuTab::ESP, AetherPopup::WeaponIcon },
        { L"Distance", L"meters text size scale position color", MenuTab::ESP, AetherPopup::Distance },
        { L"Hitmarker", L"hit marker scale volume color sound", MenuTab::ESP, AetherPopup::HitMarker },
        { L"Damage popup", L"damage numbers text size boldness outline", MenuTab::ESP, AetherPopup::DamagePopup },
        { L"FOV circle", L"aim fov radius screen center pixels ring pulse thickness color", MenuTab::Aimbot, AetherPopup::FovCircle },
        { L"Off-screen arrows", L"misc offscreen out of view enemy direction center left right screen placement indicators arrow pulse scale color", MenuTab::Misc, AetherPopup::OffscreenArrows },
        { L"Radar", L"misc player radar enemy dots compass circle rounded rectangle size range distance meter pixel dot color drag position", MenuTab::Misc, AetherPopup::Radar },
        { L"Menu hotkey", L"open close toggle key", MenuTab::Settings, AetherPopup::None },
        { L"Exit hotkey", L"close application exit key", MenuTab::Settings, AetherPopup::None },
        { L"Appearance", L"accent menu panel border opacity", MenuTab::Settings, AetherPopup::None }
    };
    std::array<int, 32> g_aetherSearchMatches{};
    size_t g_aetherSearchMatchCount = 0;

    D2D1_RECT_F AetherSearchInputRect() noexcept
    {
        return AetherRect(388.0f, 10.0f, 202.0f, 22.0f);
    }

    D2D1_RECT_F AetherSearchResultsRect() noexcept
    {
        const float rows = static_cast<float>((std::min)(g_aetherSearchMatchCount, size_t(6)));
        return AetherRect(350.0f, 36.0f, 270.0f, 30.0f + (rows > 0.0f ? rows : 1.0f) * 22.0f);
    }

    std::wstring AetherLower(const wchar_t* input)
    {
        std::wstring value = input ? input : L"";
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value;
    }

    void AetherBuildSearchMatches() noexcept
    {
        g_aetherSearchMatchCount = 0;
        if (g_aetherSearchQuery.empty()) return;
        try
        {
            const std::wstring query = AetherLower(g_aetherSearchQuery.c_str());
            for (size_t i = 0; i < _countof(kAetherSearchEntries) &&
                g_aetherSearchMatchCount < g_aetherSearchMatches.size(); ++i)
            {
                const std::wstring label = AetherLower(kAetherSearchEntries[i].label);
                const std::wstring keywords = AetherLower(kAetherSearchEntries[i].keywords);
                if (label.find(query) != std::wstring::npos ||
                    keywords.find(query) != std::wstring::npos)
                    g_aetherSearchMatches[g_aetherSearchMatchCount++] = static_cast<int>(i);
            }
        }
        catch (...) { g_aetherSearchMatchCount = 0; }
        if (g_aetherSearchSelection >= static_cast<int>(g_aetherSearchMatchCount))
            g_aetherSearchSelection = 0;
    }

    void AetherActivateSearchResult(size_t visibleIndex) noexcept
    {
        if (visibleIndex >= g_aetherSearchMatchCount) return;
        const int entryIndex = g_aetherSearchMatches[visibleIndex];
        if (entryIndex < 0 || entryIndex >= static_cast<int>(_countof(kAetherSearchEntries))) return;
        const auto& entry = kAetherSearchEntries[entryIndex];
        g_activeTab = entry.tab;
        g_aetherPopup = entry.popup;
        g_aetherPopupAnchor = AetherRect(618.0f, 76.0f, 1.0f, 1.0f);
        g_aetherSearchOpen = false;
        g_aetherSearchFocused = false;
        g_aetherSearchQuery.clear();
        g_aetherSearchSelection = 0;
        g_menuScrollOffset = 0.0f;
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    void AetherDrawSearchOverlay() noexcept
    {
        if (!g_aetherSearchOpen) return;
        const D2D1_RECT_F input = AetherSearchInputRect();
        AetherFill(input.left, input.top, input.right - input.left,
            input.bottom - input.top, 5.0f, 0x18181B, 1.0f);
        AetherStroke(input.left + 0.5f, input.top + 0.5f,
            input.right - input.left - 1.0f, input.bottom - input.top - 1.0f,
            5.0f, g_aetherSearchFocused ? 0xFFFFFF : 0x45454B, 0.9f, 0.8f);
        AetherIcon(0xE721, input.left + 5.0f, input.top + 3.0f, 16.0f, 16.0f, 0xB8B8BF);
        const wchar_t* placeholder = L"Search settings...";
        if (g_aetherSearchQuery.empty())
            AetherText(placeholder, input.left + 25.0f, input.top + 2.0f,
                input.right - input.left - 32.0f, 18.0f, 0x77777F, g_smallTextFormat);
        else
            AetherText(g_aetherSearchQuery, input.left + 25.0f, input.top + 2.0f,
                input.right - input.left - 32.0f, 18.0f, 0xE4E4E8, g_smallTextFormat);

        AetherBuildSearchMatches();
        const D2D1_RECT_F panel = AetherSearchResultsRect();
        AetherFill(panel.left - 1.0f, panel.top + 1.0f,
            panel.right - panel.left + 2.0f, panel.bottom - panel.top + 2.0f,
            7.0f, 0x000000, 0.42f);
        AetherFill(panel.left, panel.top, panel.right - panel.left,
            panel.bottom - panel.top, 7.0f, 0x19191D, 0.99f);
        AetherStroke(panel.left + 0.5f, panel.top + 0.5f,
            panel.right - panel.left - 1.0f, panel.bottom - panel.top - 1.0f,
            7.0f, 0x34343A, 1.0f, 0.8f);
        AetherText(L"SETTINGS SEARCH", panel.left + 9.0f, panel.top + 4.0f,
            panel.right - panel.left - 18.0f, 18.0f, 0x85858C, g_smallTextFormat);

        if (g_aetherSearchQuery.empty())
        {
            AetherText(L"Type a setting name or keyword.", panel.left + 10.0f,
                panel.top + 27.0f, panel.right - panel.left - 20.0f, 20.0f,
                0xA0A0A7, g_smallTextFormat);
            return;
        }
        if (!g_aetherSearchMatchCount)
        {
            AetherText(L"No matching settings found.", panel.left + 10.0f,
                panel.top + 27.0f, panel.right - panel.left - 20.0f, 20.0f,
                0xA0A0A7, g_smallTextFormat);
            return;
        }
        const size_t visibleCount = (std::min)(g_aetherSearchMatchCount, size_t(6));
        for (size_t i = 0; i < visibleCount; ++i)
        {
            const float rowY = panel.top + 26.0f + static_cast<float>(i) * 22.0f;
            const bool selected = static_cast<int>(i) == g_aetherSearchSelection;
            if (selected || AetherHovered(AetherRect(panel.left + 4.0f, rowY,
                panel.right - panel.left - 8.0f, 21.0f)))
                AetherFill(panel.left + 4.0f, rowY, panel.right - panel.left - 8.0f,
                    21.0f, 4.0f, selected ? 0xFFFFFF : 0xFFFFFF, selected ? 0.09f : 0.045f);
            const auto& entry = kAetherSearchEntries[g_aetherSearchMatches[i]];
            AetherText(entry.label, panel.left + 10.0f, rowY + 1.0f,
                panel.right - panel.left - 42.0f, 19.0f,
                selected ? 0xFFFFFF : 0xD0D0D5, g_smallTextFormat);
            AetherText(entry.tab == MenuTab::Aimbot ? L"AIM" :
                (entry.tab == MenuTab::ESP ? L"VISUALS" :
                    (entry.tab == MenuTab::Settings ? L"APP" : L"MISC")),
                panel.right - 42.0f, rowY + 1.0f, 32.0f, 19.0f,
                0x77777F, g_smallTextFormat, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    void DrawMenu() noexcept
    {
        if (!g_d2dTarget || !Menu::menuOpen) return;
        g_aetherControlCount = 0;

        // Uploaded Aether frame: narrow sidebar + translucent outer shell + inset content panel.
        AetherFill(0.0f, 0.0f, 642.0f, 336.0f, 12.0f, 0x1D1D21, g_aetherOpacity);
        AetherStroke(0.5f, 0.5f, 641.0f, 335.0f, 12.0f, 0xFFFFFF, g_aetherBorderOpacity, 0.8f);
        AetherFill(132.0f, 6.0f, 502.0f, 324.0f, 9.0f, 0x131315, g_aetherPanelOpacity);
        AetherStroke(132.5f, 6.5f, 501.0f, 323.0f, 9.0f, 0xFFFFFF, g_aetherBorderOpacity * 0.56f, 0.8f);

        // Aether's original triangular mark and wordmark.
        AetherLine(17.0f, 32.0f, 27.0f, 14.0f, 0xE9E9EC, 1.0f, 2.0f);
        AetherLine(27.0f, 14.0f, 37.0f, 32.0f, 0xE9E9EC, 1.0f, 2.0f);
        AetherLine(37.0f, 32.0f, 17.0f, 32.0f, 0xE9E9EC, 1.0f, 2.0f);
        AetherLine(23.5f, 32.0f, 27.0f, 25.5f, 0xE9E9EC, 1.0f, 1.5f);
        AetherLine(27.0f, 25.5f, 30.5f, 32.0f, 0xE9E9EC, 1.0f, 1.5f);
        AetherText(L"AI MASTER", 45.0f, 12.0f, 92.0f, 24.0f, 0xF3F3F5, g_aetherLogoFormat);
        AetherText(L"CS2 INTERNAL", 17.0f, 36.0f, 100.0f, 15.0f, 0x77777F, g_smallTextFormat);
        AetherAddTab(67.0f, MenuTab::Aimbot, L"Aimbot", 0xE81D);
        AetherAddTab(96.0f, MenuTab::ESP, L"Visuals", 0xE7B3);
        AetherAddTab(125.0f, MenuTab::Settings, L"Settings", 0xE713);
        AetherAddTab(154.0f, MenuTab::Misc, L"Misc", 0xE71D);
        AetherText(L"v1.0.0", 18.0f, 309.0f, 90.0f, 15.0f, 0x77777F, g_smallTextFormat);

        // Keep search as the only top-right action; Settings lives in the sidebar.
        if (!g_aetherSearchOpen)
            AetherIcon(0xE721, 600.0f, 14.0f, 18.0f, 18.0f, 0xBDBDC3);
        else
            AetherIcon(0xE711, 598.0f, 14.0f, 18.0f, 18.0f, 0xBDBDC3);
        AetherLine(146.0f, 42.0f, 620.0f, 42.0f, 0x28282D, 1.0f, 0.7f);

        if (g_activeTab == MenuTab::Aimbot) AetherDrawAimbotPage();
        else if (g_activeTab == MenuTab::ESP) AetherDrawVisualsPage();
        else if (g_activeTab == MenuTab::Settings) AetherDrawSettingsPage();
        else AetherDrawMiscPage();

        if (g_aetherPopup != AetherPopup::None)
            AetherDrawPopup();
        if (g_aetherColorPickerOpen)
            AetherDrawColorPicker();
        AetherDrawSearchOverlay();
    }

    void AetherUpdateSlider(size_t index, float mouseX) noexcept
    {
        if (index >= g_aetherControlCount) return;
        const AetherControl& c = g_aetherControls[index];
        if (c.kind != AetherControlKind::Slider || !c.value) return;
        float* value = static_cast<float*>(c.value);
        const float width = c.rect.right - c.rect.left;
        if (width <= 1.0f || c.maximum <= c.minimum) return;
        const float t = std::clamp((mouseX - c.rect.left) / width, 0.0f, 1.0f);
        float updated = c.minimum + t * (c.maximum - c.minimum);
        if (c.step > 0.0f) updated = std::round(updated / c.step) * c.step;
        *value = std::clamp(updated, c.minimum, c.maximum);
        g_configDirtyDuringDrag = true;
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    void AetherApplyControl(HWND hwnd, size_t index, float localX) noexcept
    {
        if (index >= g_aetherControlCount) return;
        const AetherControl c = g_aetherControls[index];
        switch (c.kind)
        {
        case AetherControlKind::Tab:
            g_activeTab = c.tab;
            g_aetherPopup = AetherPopup::None;
            g_aetherColorPickerOpen = false;
            g_aetherColorDrag = AetherColorDrag::None;
            g_aetherColorTargetR = g_aetherColorTargetG = g_aetherColorTargetB = nullptr;
            g_aetherActiveSlider = -1;
            break;
        case AetherControlKind::Toggle:
            if (c.value)
            {
                bool* value = static_cast<bool*>(c.value);
                *value = !*value;
                if (c.value == &AimConfig::rcsEnabled && !*value)
                {
                    ResetRcsSampleState();
                    ResetRcsLerpState();
                }
                if (c.value == &Menu::hitMarkerSound && *value)
                    PlayHitMarkerSound();
            }
            break;
        case AetherControlKind::Slider:
            g_aetherActiveSlider = static_cast<int>(index);
            SetCapture(hwnd);
            AetherUpdateSlider(index, localX);
            return; // config is saved once on button release
        case AetherControlKind::Cycle:
            if (c.value && c.cycleCount > 0)
            {
                int current = AetherReadCycle(c.value);
                current = (current + 1) % c.cycleCount;
                std::memcpy(c.value, &current, sizeof(current));
            }
            break;
        case AetherControlKind::Color:
            AetherOpenColorPicker(c);
            return; // color is committed when the drag ends
        case AetherControlKind::Popup:
            if (g_aetherPopup == c.popup) g_aetherPopup = AetherPopup::None;
            else AetherSetPopup(c.popup, c.rect);
            break;
        case AetherControlKind::AimKey:
            g_keyCaptureTarget = KeyCaptureTarget::AimActivation;
            g_aimCaptureWaitForRelease = true;
            break;
        case AetherControlKind::MenuHotkey:
            g_keyCaptureTarget = KeyCaptureTarget::MenuToggle;
            break;
        case AetherControlKind::ExitHotkey:
            g_keyCaptureTarget = KeyCaptureTarget::ExitOverlay;
            break;
        case AetherControlKind::ExitApp:
            g_running.store(false, std::memory_order_relaxed);
            return;
        case AetherControlKind::ResetHotkeys:
            g_menuToggleBind = { VK_F11, 0 };
            g_exitBind = { VK_END, 0 };
            break;
        case AetherControlKind::ResetAppearance:
            g_aetherAccentIndex = 1;
            g_aetherAccentR = 0.20f;
            g_aetherAccentG = 0.88f;
            g_aetherAccentB = 0.58f;
            g_aetherOpacity = 0.92f;
            g_aetherPanelOpacity = 0.97f;
            g_aetherBorderOpacity = 0.08f;
            break;
        default: break;
        }
        SaveConfigToAppData();
        g_menuCacheDirty = true;
        g_frameDirty = true;
    }

    bool HandleAetherMenuInput(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (!Menu::menuOpen || g_menuPresentationPending) return false;

        // The radar is an independent overlay element. While the menu is open,
        // allow dragging from anywhere inside its visible shape. Using the full
        // panel as the hit target avoids making users aim for the tiny dot grip.
        if (message == WM_LBUTTONDOWN && Menu::radarEnabled)
        {
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            const float radarSize = std::clamp(Menu::radarSize, 120.0f, 320.0f);
            const float radarX = std::clamp(Menu::radarX, 0.0f,
                (std::max)(0.0f, static_cast<float>(g_width) - radarSize));
            const float radarY = std::clamp(Menu::radarY, 0.0f,
                (std::max)(0.0f, static_cast<float>(g_height) - radarSize));
            bool insideRadar = false;
            if (Menu::radarShape == 0)
            {
                const float cx = radarX + radarSize * 0.5f;
                const float cy = radarY + radarSize * 0.5f;
                const float radius = radarSize * 0.49f;
                const float ox = mx - cx;
                const float oy = my - cy;
                insideRadar = (ox * ox + oy * oy) <= radius * radius;
            }
            else
            {
                const D2D1_RECT_F radarRect = {
                    radarX, radarY, radarX + radarSize, radarY + radarSize };
                insideRadar = ContainsPoint(radarRect, mx, my);
            }

            if (insideRadar)
            {
                Menu::radarX = radarX;
                Menu::radarY = radarY;
                g_radarDragging = true;
                g_radarPositionDirty = false;
                g_radarDragOffsetX = mx - radarX;
                g_radarDragOffsetY = my - radarY;
                SetCapture(hwnd);
                g_frameDirty = true;
                return true;
            }
        }
        if (message == WM_MOUSEMOVE && g_radarDragging)
        {
            // Read the live cursor and convert it to overlay-client coordinates.
            // This also works reliably while SetCapture keeps the drag alive
            // outside the original radar bounds.
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
            {
                const float radarSize = std::clamp(Menu::radarSize, 120.0f, 320.0f);
                const float mx = static_cast<float>(cursor.x);
                const float my = static_cast<float>(cursor.y);
                const float nextX = std::clamp(mx - g_radarDragOffsetX, 0.0f,
                    (std::max)(0.0f, static_cast<float>(g_width) - radarSize));
                const float nextY = std::clamp(my - g_radarDragOffsetY, 0.0f,
                    (std::max)(0.0f, static_cast<float>(g_height) - radarSize));
                if (std::fabs(Menu::radarX - nextX) > 0.01f ||
                    std::fabs(Menu::radarY - nextY) > 0.01f)
                {
                    Menu::radarX = nextX;
                    Menu::radarY = nextY;
                    g_radarPositionDirty = true;
                    g_frameDirty = true;
                }
            }
            return true;
        }
        if (message == WM_LBUTTONUP && g_radarDragging)
        {
            g_radarDragging = false;
            if (g_radarPositionDirty)
            {
                SaveConfigToAppData();
                g_radarPositionDirty = false;
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            g_frameDirty = true;
            return true;
        }

        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            g_menuMouse = { static_cast<LONG>(mx), static_cast<LONG>(my) };
            const float lx = mx - g_menuX;
            const float ly = my - g_menuY;
            if (lx < 0.0f || ly < 0.0f || lx > 642.0f || ly > 336.0f)
                return true;

            // Give the floating color picker first refusal so clicks/drags never reach controls underneath it.
            if (g_aetherColorPickerOpen)
            {
                if (ContainsPoint(g_aetherColorPickerRect, lx, ly))
                {
                    if (ContainsPoint(g_aetherColorSvRect, lx, ly))
                    {
                        g_aetherColorDrag = AetherColorDrag::SaturationValue;
                        SetCapture(hwnd);
                        AetherUpdateColorPicker(lx, ly);
                    }
                    else if (ContainsPoint(g_aetherColorHueRect, lx, ly))
                    {
                        g_aetherColorDrag = AetherColorDrag::Hue;
                        SetCapture(hwnd);
                        AetherUpdateColorPicker(lx, ly);
                    }
                    return true;
                }
                g_aetherColorPickerOpen = false;
                g_aetherColorDrag = AetherColorDrag::None;
                g_aetherColorTargetR = g_aetherColorTargetG = g_aetherColorTargetB = nullptr;
                g_menuCacheDirty = true;
                g_frameDirty = true;
            }

            // Expanded settings search: the input and result panel receive clicks before page controls.
            if (g_aetherSearchOpen)
            {
                if (ContainsPoint(AetherRect(598.0f, 13.0f, 22.0f, 20.0f), lx, ly))
                {
                    g_aetherSearchOpen = false;
                    g_aetherSearchFocused = false;
                    g_aetherSearchQuery.clear();
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                    return true;
                }
                if (ContainsPoint(AetherSearchInputRect(), lx, ly))
                {
                    g_aetherSearchFocused = true;
                    SetFocus(hwnd);
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                    return true;
                }
                const D2D1_RECT_F searchPanel = AetherSearchResultsRect();
                if (ContainsPoint(searchPanel, lx, ly))
                {
                    const float resultTop = searchPanel.top + 26.0f;
                    if (!g_aetherSearchQuery.empty() && ly >= resultTop && g_aetherSearchMatchCount > 0)
                    {
                        const int row = static_cast<int>((ly - resultTop) / 22.0f);
                        if (row >= 0 && row < static_cast<int>((std::min)(g_aetherSearchMatchCount, size_t(6))))
                            AetherActivateSearchResult(static_cast<size_t>(row));
                    }
                    else
                    {
                        g_aetherSearchFocused = true;
                    }
                    return true;
                }
                g_aetherSearchOpen = false;
                g_aetherSearchFocused = false;
            }

            // Clicking the magnifier expands/collapses the universal settings search.
            if (ContainsPoint(AetherRect(598.0f, 13.0f, 22.0f, 20.0f), lx, ly))
            {
                g_aetherSearchOpen = !g_aetherSearchOpen;
                g_aetherSearchFocused = g_aetherSearchOpen;
                if (g_aetherSearchOpen) SetFocus(hwnd);
                if (!g_aetherSearchOpen) g_aetherSearchQuery.clear();
                g_aetherSearchSelection = 0;
                g_menuCacheDirty = true;
                g_frameDirty = true;
                return true;
            }

            // Popup controls are registered after page controls, so test in reverse order.
            for (size_t n = g_aetherControlCount; n > 0; --n)
            {
                const size_t i = n - 1;
                if (!ContainsPoint(g_aetherControls[i].rect, lx, ly)) continue;
                AetherApplyControl(hwnd, i, lx);
                return true;
            }

            if (g_aetherPopup != AetherPopup::None)
            {
                g_aetherPopup = AetherPopup::None;
                g_menuCacheDirty = true;
                g_frameDirty = true;
                return true;
            }

            // Aether reference: empty header space drags the menu.
            if (ly <= 42.0f)
            {
                g_aetherDragging = true;
                g_aetherDragOffsetX = mx - g_menuX;
                g_aetherDragOffsetY = my - g_menuY;
                SetCapture(hwnd);
            }
            g_menuCacheDirty = true;
            g_frameDirty = true;
            return true;
        }
        case WM_MOUSEMOVE:
        {
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            g_menuMouse = { static_cast<LONG>(mx), static_cast<LONG>(my) };
            if (g_aetherColorDrag != AetherColorDrag::None && (wParam & MK_LBUTTON))
            {
                AetherUpdateColorPicker(mx - g_menuX, my - g_menuY);
            }
            else if (g_aetherDragging && (wParam & MK_LBUTTON))
            {
                constexpr float menuWidth = 642.0f;
                constexpr float menuHeight = 336.0f;
                const float maxX = std::max(8.0f, static_cast<float>(g_width) - menuWidth - 8.0f);
                const float maxY = std::max(8.0f, static_cast<float>(g_height) - menuHeight - 8.0f);
                g_menuX = std::clamp(mx - g_aetherDragOffsetX, 8.0f, maxX);
                g_menuY = std::clamp(my - g_aetherDragOffsetY, 8.0f, maxY);
                g_menuCacheDirty = true;
                g_frameDirty = true;
            }
            else if (g_aetherActiveSlider >= 0 && (wParam & MK_LBUTTON))
            {
                AetherUpdateSlider(static_cast<size_t>(g_aetherActiveSlider), mx - g_menuX);
            }
            else
            {
                // Hover highlights in the Aether template are cache-rendered.
                g_menuCacheDirty = true;
                g_frameDirty = true;
            }
            return true;
        }
        case WM_MOUSEWHEEL:
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &point);
            const float lx = static_cast<float>(point.x) - g_menuX;
            const float ly = static_cast<float>(point.y) - g_menuY;
            const float direction = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.0f : -1.0f;
            for (size_t n = g_aetherControlCount; n > 0; --n)
            {
                const AetherControl& c = g_aetherControls[n - 1];
                if (c.kind != AetherControlKind::Slider || !c.value || !ContainsPoint(c.rect, lx, ly))
                    continue;
                float* value = static_cast<float*>(c.value);
                const float step = c.step > 0.0f ? c.step : (c.maximum - c.minimum) * 0.01f;
                *value = std::clamp(*value + direction * step, c.minimum, c.maximum);
                SaveConfigToAppData();
                g_menuCacheDirty = true;
                g_frameDirty = true;
                break;
            }
            return true;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_radarPositionDirty)
            {
                SaveConfigToAppData();
                g_radarPositionDirty = false;
            }
            if (g_configDirtyDuringDrag)
            {
                if (g_aetherWeaponIconColorDirty)
                {
                    ReleaseWeaponIconBitmaps();
                    g_aetherWeaponIconColorDirty = false;
                }
                SaveConfigToAppData();
                g_configDirtyDuringDrag = false;
            }
            g_aetherColorDrag = AetherColorDrag::None;
            g_aetherActiveSlider = -1;
            g_aetherDragging = false;
            g_radarDragging = false;
            if (message == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
            g_menuCacheDirty = true;
            g_frameDirty = true;
            return true;
        default:
            break;
        }
        return false;
    }


    bool RenderMenuCache() noexcept
    {
        if (!Menu::menuOpen) return true;
        if (g_menuBitmap && !g_menuCacheDirty) return true;
        if (!g_d2dTarget || !g_menuBitmap) return false;

        // The Aether UI is authored entirely in menu-local coordinates
        // (0..642 by 0..336). BuildMenuLayout only initializes/clamps the
        // on-screen destination; it must NOT be applied as a drawing offset
        // while rendering into this local off-screen bitmap.
        (void)BuildMenuLayout();

        ID2D1Image* previousTarget = nullptr;
        g_d2dTarget->GetTarget(&previousTarget);
        g_d2dTarget->SetTarget(g_menuBitmap);

        D2D1_MATRIX_3X2_F originalTransform{};
        g_d2dTarget->GetTransform(&originalTransform);
        g_d2dTarget->SetTransform(D2D1::Matrix3x2F::Identity());

        g_d2dTarget->BeginDraw();
        g_d2dTarget->Clear(Color(0.0f, 0.0f, 0.0f, 0.0f));

        // Draw at the bitmap origin. RenderFrame() later places this 642x336
        // bitmap at (g_menuX, g_menuY) on the full-screen composition surface.
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
                SaveConfigToAppData();
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
        // Register WM_HOTKEY when available. The message loop also polls the
        // configured key as a backup because some game input stacks can miss it.

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
        // WM_HOTKEY and the GetAsyncKeyState backup can observe the same press.
        // Debounce both paths so one physical press causes one state change.
        if (g_lastMenuToggleTime != 0 && now - g_lastMenuToggleTime < 220ULL)
            return;

        const bool opening = !Menu::menuOpen;
        if (opening && !IsTargetGameForeground())
        {
            Log(L"Menu hotkey ignored because the game is not the foreground application.");
            return;
        }

        Log(opening ? L"Menu toggle: CLOSED -> OPEN." : L"Menu toggle: OPEN -> CLOSED.");
        Menu::menuOpen = opening;
        g_menuPresentationPending = false;
        g_lastMenuToggleTime = now;
        g_lastTargetForegroundTime = now;
        g_menuCacheDirty = true;
        g_frameDirty = true;
        g_lastMenuVisible = !opening;
        g_activeSlider = ActiveSlider::None;
        g_menuDragging = false;
        g_colorPickerOpen = false;
        g_aetherPopup = AetherPopup::None;
        g_aetherColorPickerOpen = false;
        g_aetherColorDrag = AetherColorDrag::None;
        g_aetherColorTargetR = g_aetherColorTargetG = g_aetherColorTargetB = nullptr;
        g_aetherActiveSlider = -1;
        // Preserve the last drag position even if the menu is toggled closed
        // before the mouse-up message is processed.
        if (g_radarPositionDirty)
        {
            SaveConfigToAppData();
            g_radarPositionDirty = false;
        }
        g_radarDragging = false;
        g_aetherSearchOpen = false;
        g_aetherSearchFocused = false;
        g_aetherSearchQuery.clear();
        g_aetherSearchSelection = 0;
        if (opening)
            g_aimActivationWasHeld = false;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();

        if (opening)
        {
            // Draw the Aether surface before switching the HWND into interactive
            // mode. Do not leave the window hidden indefinitely if a cache/present
            // attempt fails; the regular frame loop will retry the render.
            g_targetWindowActive = true;
            g_overlayWindowVisible = true;
            g_initialTargetStateChecked = true;

            if (!RenderMenuCache())
                Log(L"Menu cache render failed on open; frame loop will retry.");
            if (!RenderFrame())
                Log(L"Menu frame render failed on open; frame loop will retry.");

            ApplyMenuInputMode(true, false);
            g_overlayWindowVisible = g_hwnd && IsWindowVisible(g_hwnd) != FALSE;
            Log(g_overlayWindowVisible
                ? L"Menu OPEN; overlay window shown."
                : L"Menu OPEN requested, but overlay HWND did not become visible.");
            return;
        }

        // Closing is immediate: leave the ESP overlay passive and return focus
        // to the game. The next frame removes the menu bitmap.
        ApplyMenuInputMode(true, false);
        if (g_hwnd && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd))
        {
            g_frameDirty = true;
            if (!RenderFrame())
                Log(L"Menu-close presentation will retry on the next frame.");
        }
        Log(L"Menu CLOSED; game focus hand-off requested.");
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
        bool radarChanged = (Menu::radarEnabled != g_lastRadarEnabled) ||
            (g_radarLocalValid != g_lastRadarLocalValid) ||
            (g_radarEntityCount != g_lastRadarEntityCount);
        if (!radarChanged && Menu::radarEnabled)
        {
            constexpr float kRadarChangeEpsilon = 0.10f;
            radarChanged = std::fabs(g_radarLocalOrigin.x - g_lastRadarLocalOrigin.x) > kRadarChangeEpsilon ||
                std::fabs(g_radarLocalOrigin.y - g_lastRadarLocalOrigin.y) > kRadarChangeEpsilon ||
                std::fabs(g_radarLocalOrigin.z - g_lastRadarLocalOrigin.z) > kRadarChangeEpsilon ||
                std::fabs(g_radarViewYaw - g_lastRadarViewYaw) > 0.08f;
            for (size_t i = 0; !radarChanged && i < g_radarEntityCount; ++i)
            {
                const RadarEntityData& current = g_radarEntities[i];
                const RadarEntityData& previous = g_lastRadarEntities[i];
                radarChanged = current.valid != previous.valid || current.team != previous.team ||
                    std::fabs(current.worldOrigin.x - previous.worldOrigin.x) > kRadarChangeEpsilon ||
                    std::fabs(current.worldOrigin.y - previous.worldOrigin.y) > kRadarChangeEpsilon ||
                    std::fabs(current.worldOrigin.z - previous.worldOrigin.z) > kRadarChangeEpsilon;
            }
        }
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
            Menu::skeleton ||
            (AimConfig::showFovCircle && AimConfig::fovCircleAnimation == 1) ||
            (Menu::offscreenIndicators && Menu::offscreenAnimation == 1);

        bool damagePopupActive = false;
        const ULONGLONG popupNow = GetTickCount64();
        const ULONGLONG popupDurationNow = kDamagePopupLifetimeMs;
        for (const auto& popup : g_damagePopups)
            if (popup.active && popupNow - popup.born < popupDurationNow) { damagePopupActive = true; break; }
        if (!g_frameDirty && !entitiesChanged && !radarChanged && !menuVisibilityChanged && !menuContentChanged &&
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
        DrawAimFovCircle();
        if (hitMarkerActive) DrawHitMarker();
        DrawDamagePopups();

        if (Menu::menuOpen && g_menuBitmap)
        {
            const D2D1_RECT_F dest{ g_menuX, g_menuY, g_menuX + static_cast<float>(kMenuSurfaceWidth), g_menuY + static_cast<float>(kMenuSurfaceHeight) };
            g_d2dTarget->DrawBitmap(g_menuBitmap, &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        }
        // Keep the independently draggable radar above the menu if the user
        // moves it over the menu canvas.
        DrawRadar();

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
        g_lastRadarEntities = g_radarEntities;
        g_lastRadarEntityCount = g_radarEntityCount;
        g_lastRadarLocalOrigin = g_radarLocalOrigin;
        g_lastRadarViewYaw = g_radarViewYaw;
        g_lastRadarLocalValid = g_radarLocalValid;
        g_lastRadarEnabled = Menu::radarEnabled;
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
        if (HandleAetherMenuInput(hwnd, message, wParam, lParam)) return 0;
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
        case WM_KEYDOWN:
            if (g_aetherSearchOpen && g_aetherSearchFocused)
            {
                if (wParam == VK_ESCAPE)
                {
                    g_aetherSearchOpen = false;
                    g_aetherSearchFocused = false;
                    g_aetherSearchQuery.clear();
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                    return 0;
                }
                if (wParam == VK_BACK)
                {
                    if (!g_aetherSearchQuery.empty()) g_aetherSearchQuery.pop_back();
                    g_aetherSearchSelection = 0;
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                    return 0;
                }
                if (wParam == VK_UP || wParam == VK_DOWN)
                {
                    AetherBuildSearchMatches();
                    if (g_aetherSearchMatchCount)
                    {
                        const int count = static_cast<int>((std::min)(g_aetherSearchMatchCount, size_t(6)));
                        g_aetherSearchSelection = (g_aetherSearchSelection +
                            (wParam == VK_DOWN ? 1 : count - 1)) % count;
                        g_menuCacheDirty = true;
                        g_frameDirty = true;
                    }
                    return 0;
                }
                if (wParam == VK_RETURN)
                {
                    AetherBuildSearchMatches();
                    if (g_aetherSearchMatchCount)
                        AetherActivateSearchResult(static_cast<size_t>(g_aetherSearchSelection));
                    return 0;
                }
            }
            if (wParam == VK_ESCAPE && g_aetherSearchOpen)
            {
                g_aetherSearchOpen = false;
                g_aetherSearchFocused = false;
                g_aetherSearchQuery.clear();
                g_menuCacheDirty = true;
                g_frameDirty = true;
                return 0;
            }
            if (wParam == VK_ESCAPE && g_aetherPopup != AetherPopup::None)
            {
                g_aetherPopup = AetherPopup::None;
                g_menuCacheDirty = true;
                g_frameDirty = true;
                return 0;
            }
            break;
        case WM_CHAR:
            if (g_aetherSearchOpen && g_aetherSearchFocused)
            {
                const wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch >= 32 && ch != 127 && g_aetherSearchQuery.size() < 48)
                {
                    g_aetherSearchQuery.push_back(ch);
                    g_aetherSearchSelection = 0;
                    g_menuCacheDirty = true;
                    g_frameDirty = true;
                }
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (Menu::menuOpen && !g_menuPresentationPending)
                return DefWindowProcW(hwnd, message, wParam, lParam);
            // Passive mode is click-through; never replace the game's cursor.
            return TRUE;
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            // Menu mouse input is handled by HandleAetherMenuInput above.
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
            // This is a backup to WM_HOTKEY, not only a registration-failure path.
            PollKeyboardState();

            if (g_keyCaptureTarget != KeyCaptureTarget::None)
            {
                CaptureHotkeyIfPressed();
                SyncMenuHotkeyRegistration();
            }
            else if (IsHotkeyPressed(g_menuToggleBind))
            {
                // Always keep a sampled-key backup. Some game/input stacks can
                // intermittently miss WM_HOTKEY delivery to a hidden overlay HWND.
                // ToggleMenuFromHotkey debounces the WM_HOTKEY + polling pair.
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