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
    )
    require(
        "android/app/src/main/java/com/hllartyparty/roguesquadron64recomp/LauncherActivity.java",
        "ACTION_OPEN_DOCUMENT",
        "normalize",
        "rogue_squadron.z64",
        "USA v1.0",
        "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d",
        "MessageDigest",
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
