#!/usr/bin/env python3
"""Structural smoke test for the first Android bootstrap slice."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, *needles: str) -> None:
    target = ROOT / path
    assert target.is_file(), f"missing {path}"
    text = target.read_text(encoding="utf-8")
    for needle in needles:
        assert needle in text, f"{path} missing {needle!r}"


def test_android_project_structure() -> None:
    require("android/settings.gradle", "RogueSquadron64RecompAndroid", "include ':app'")
    require("android/build.gradle", "com.android.application", "8.7.3")
    require("android/gradle.properties", "org.gradle.jvmargs", "android.useAndroidX=false")
    require(
        "android/app/build.gradle",
        "com.hllartyparty.roguesquadron64recomp",
        "arm64-v8a",
        "27.1.12297006",
        "SDL2-2.32.10-android-arm64-vulkanfix1",
        "RogueSquadron64Recomp",
    )
    require(
        "android/app/src/main/AndroidManifest.xml",
        ".LauncherActivity",
        ".GameActivity",
        "sensorLandscape",
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/GameActivity.java",
        "extends SDLActivity",
        '"SDL2","RogueSquadron64Recomp"',
        "--android-data-dir=",
        'getStringExtra("display_mode")',
        '"--display-mode=" + displayMode',
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/LauncherActivity.java",
        "ACTION_OPEN_DOCUMENT",
        "normalize",
        "rogue_squadron.z64",
        "USA v1.0",
        "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d",
        "MessageDigest",
        'normal.setText("Normal Boot")',
        'widescreen.setText("16:9 Boot (Experimental)")',
        'logs.setText("Show Log")',
        'pick.setText("Choose Different ROM")',
        'launchGame("normal")',
        'launchGame("horplus")',
        'putExtra("display_mode", displayMode)',
        'copy.setText("Copy All")',
        'share.setText("Share Log")',
        "new ScrollView(this)",
        "ClipboardManager",
        "Intent.ACTION_SEND",
        "Intent.EXTRA_STREAM",
        "FLAG_GRANT_READ_URI_PERMISSION",
        'new File(getFilesDir(), "roguesq-runtime.log")',
        'new File(getFilesDir(), "roguesq-runtime.previous.log")',
        "Showing previous boot log",
        "MAX_LOG_CHARS",
    )
    launcher_source = (
        ROOT
        / "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/LauncherActivity.java"
    ).read_text(encoding="utf-8")
    assert "launchGame();\n        }" not in launcher_source, (
        "an installed ROM must show the dashboard instead of auto-launching"
    )
    assert "Intent.EXTRA_TEXT" not in launcher_source, (
        "complete logs must be attached as files rather than truncation-prone text payloads"
    )
    require(
        "android/app/src/main/AndroidManifest.xml",
        '.RuntimeLogProvider',
        'android:grantUriPermissions="true"',
        'android:exported="false"',
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/RuntimeLogProvider.java",
        "extends ContentProvider",
        "ParcelFileDescriptor.open",
        "roguesq-runtime.previous.log",
        "OpenableColumns.DISPLAY_NAME",
        "OpenableColumns.SIZE",
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/GameActivity.java",
        'new File(getFilesDir(), "roguesq-runtime.log")',
        'new File(getFilesDir(), "roguesq-runtime.previous.log")',
        "rotateRuntimeLog()",
        "new FileOutputStream(latest, false)",
    )


def test_native_target_has_android_shared_library_path() -> None:
    require(
        "CMakeLists.txt",
        "if(ANDROID)",
        "add_library(RogueSquadron64Recomp SHARED",
        "SDL2::SDL2 android log vulkan",
        'target_link_options(RogueSquadron64Recomp PRIVATE "-Wl,-z,max-page-size=16384")',
    )
    require(
        "src/main/main.cpp",
        "#if !defined(__ANDROID__)",
        "#define SDL_MAIN_HANDLED",
        "ROGUESQ_ANDROID_DATA_DIR",
        "ROGUESQ_ANDROID_PROGRAM_DIR",
    )
    require("src/main/register_overlays.cpp", '#include "recomp_overlays.inl"')


def test_android_arm64_and_storage_guards() -> None:
    require(
        "CMakeLists.txt",
        "RSP_UCODE_COMPILE_OPTIONS",
        "CMAKE_SYSTEM_PROCESSOR MATCHES",
        "src/android/nfd_android_stub.cpp",
        "if(NOT ANDROID)",
    )
    require(
        "src/main/main.cpp",
        'freopen(log_path.string().c_str(), "a", stdout)',
        'freopen(log_path.string().c_str(), "a", stderr)',
        "roguesq-runtime.log",
        "register_config_path(config_path)",
        '"/dev/null"',
    )
    require(
        "src/android/nfd_android_stub.cpp",
        "NFD_OpenDialogN",
        "NFD_CANCEL",
        "Android file dialogs are handled by Java",
    )
    require(
        "lib/rt64/CMakeLists.txt",
        "CMAKE_HOST_APPLE",
        "CMAKE_HOST_SYSTEM_PROCESSOR",
        "ZELDA_HOST_FILE_TO_C",
        "if (NOT TARGET nfd)",
        "FILE_TO_C_DEP",
    )
    require(
        "lib/rt64/src/hle/rt64_application.cpp",
        "#if defined(__ANDROID__)",
        "swapChainDesc.format = RenderFormat::R8G8B8A8_UNORM;",
        "swapChainDesc.format = RenderFormat::B8G8R8A8_UNORM;",
    )
    require(
        "lib/rt64/src/hle/rt64_application_window.cpp",
        "wmInfo.info.android.window",
        "defined(__linux__) && !defined(__ANDROID__)",
        "SDL_WINDOW_VULKAN",
        "defined(__ANDROID__)",
    )
    require(
        "src/main/main.cpp",
        "SDL_NumJoysticks()",
        "SDL_IsGameController",
        "SDL_GameControllerOpen",
    )
    require(
        "src/main/rt64_render_context.cpp",
        "#if defined(__ANDROID__)",
        "app->userConfig.aspectRatio = UC::AspectRatio::Expand;",
        "app->userConfig.extAspectRatio = UC::AspectRatio::Manual;",
        "app->userConfig.extAspectTarget = 16.0 / 9.0;",
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/GameActivity.java",
        "onWindowFocusChanged",
        "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION",
        "LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES",
    )
    require(
        "src/main/input_bindings.cpp",
        "SDL_CONTROLLER_AXIS_RIGHTX, -1",
        "SDL_CONTROLLER_AXIS_RIGHTX, +1",
        "SDL_CONTROLLER_AXIS_RIGHTY, -1",
        "SDL_CONTROLLER_AXIS_RIGHTY, +1",
        'j["schema"] = 2;',
        'j.value("schema", 0) < 2',
    )
    input_source = (ROOT / "src/main/input_bindings.cpp").read_text(encoding="utf-8")
    assert "Target::CLeft, pb(SDL_CONTROLLER_BUTTON_BACK)" not in input_source
    assert "Target::CRight,pb(SDL_CONTROLLER_BUTTON_GUIDE)" not in input_source
    require(
        "src/main/main.cpp",
        '{"display-mode",     "ROGUESQ_DISPLAY_MODE"',
    )
    require(
        "src/main/rt64_render_context.cpp",
        'std::string_view(display_mode) == "horplus"',
        "app->userConfig.aspectRatio = UC::AspectRatio::Expand;",
        "app->userConfig.extAspectTarget = 16.0 / 9.0;",
    )
    render_source = (ROOT / "src/main/rt64_render_context.cpp").read_text(encoding="utf-8")
    assert "app->userConfig.extAspectTarget = 4.0 / 3.0;" not in render_source
    require(
        "CMakeLists.txt",
        "setupCameraMatrices=setupCameraMatrices_original",
        "guPerspective=guPerspective_original",
        "buildVisibleTerrainGridAroundCamera=buildVisibleTerrainGridAroundCamera_original",
    )
    require(
        "src/main/upstream_compat.cpp",
        "setupCameraMatrices_original",
        "guPerspective_original",
        "buildVisibleTerrainGridAroundCamera_original",
        "void setupCameraMatrices(uint8_t* rdram, recomp_context* ctx)",
        "void guPerspective(uint8_t* rdram, recomp_context* ctx)",
        "void buildVisibleTerrainGridAroundCamera(uint8_t* rdram, recomp_context* ctx)",
        "uint32_t(ctx->r4) == 0x80138D20u",
        "s_rs64_interactive_camera",
        "16.0f / 9.0f",
        "0xFFFFFFFF8003A51Cull",
        "0xC0AAAAABu",
        "g_rs64_interactive_projection_address.store",
        "uint32_t(ctx->r4) & 0x00FFFFFFu",
    )
    require(
        "lib/rt64/src/render/rt64_framebuffer_renderer.cpp",
        "g_rs64_interactive_projection_address",
        "interactiveProjection",
        "coversWholeWidth",
        "horizontalRatio",
        "[widescreen-probe]",
        "fbScissor=",
        "projScissor=",
        "viewport=",
    )
    framebuffer_renderer_source = (ROOT / "lib/rt64/src/render/rt64_framebuffer_renderer.cpp").read_text(encoding="utf-8")
    assert "useWideViewport = interactiveProjection" not in framebuffer_renderer_source
    workload_source = (ROOT / "lib/rt64/src/hle/rt64_workload_queue.cpp").read_text(encoding="utf-8")
    assert "g_active_overlay" not in workload_source
    assert "ROGUESQ_DISPLAY_MODE" not in workload_source
    assert "workloadConfig.aspectRatioSource = 4.0f / 3.0f;" not in workload_source
    require(
        "lib/rt64/src/hle/rt64_present_queue.cpp",
        "extern \"C\" volatile int g_active_overlay;",
        'std::string_view(displayMode) != "horplus"',
        "(g_active_overlay == 0)",
        "missionPresentation.resolutionScale.x = missionPresentation.resolutionScale.y;",
        "VIRenderer::getViewportAndScissor",
        "commandList->clearColor(0, RenderColor(), missionSideBars",
    )
    require(
        "src/main/main.cpp",
        "SDL_AddEventWatch(android_lifecycle_event_watch",
        "SDL_APP_WILLENTERBACKGROUND",
        "g_android_resume_pending.store(true",
        "try_resume_android_surface();",
        "ultramodern::set_app_paused(false)",
    )
    main_source = (ROOT / "src/main/main.cpp").read_text(encoding="utf-8")
    foreground_case = main_source.index("case SDL_APP_DIDENTERFOREGROUND:")
    foreground_end = main_source.index("default:", foreground_case)
    assert "SDL_GetWindowWMInfo" not in main_source[foreground_case:foreground_end], (
        "foreground callback must request resume, not perform the donor's one-shot window lookup"
    )
    pump = main_source.index("SDL_PumpEvents();")
    assert main_source.index("try_resume_android_surface();", pump) > pump, (
        "surface resume must retry from the always-running graphics loop after pumping SDL"
    )
    require(
        "lib/N64ModernRuntime/ultramodern/include/ultramodern/ultramodern.hpp",
        "void set_app_paused(bool paused);",
        "void set_exited_and_wake();",
    )
    require(
        "lib/N64ModernRuntime/ultramodern/src/events.cpp",
        "std::condition_variable app_pause_cv",
        "wait_while_app_paused()",
    )
    require(
        "src/main/rt64_render_context.cpp",
        "pending_resume_window",
        "publish_resume_window",
        "setRenderWindow",
    )
    require(
        "lib/rt64/src/contrib/plume/plume_render_interface.h",
        "virtual void setRenderWindow(RenderWindow window)",
    )
    require(
        "lib/rt64/src/contrib/plume/plume_vulkan.cpp",
        "ANativeWindow_acquire",
        "bool VulkanSwapChain::recreateSurface()",
        "pendingRenderWindow.exchange",
    )
    require(
        "lib/rt64/src/hle/rt64_present_queue.cpp",
        "ext.swapChain->isEmpty() || !swapChainValid",
    )
    require(
        "src/main/rt64_render_context.cpp",
        "SDL_GetWindowWMInfo",
        "wm_info.info.android.window",
        "ROGUESQ_ANDROID_DATA_DIR",
    )


if __name__ == "__main__":
    test_android_project_structure()
    test_native_target_has_android_shared_library_path()
    test_android_arm64_and_storage_guards()
    print("Android bootstrap structure: OK")
