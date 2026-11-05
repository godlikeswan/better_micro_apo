cmake_minimum_required(VERSION 3.20)
project(better_micro_apo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the DLL is loaded into audiodg.exe, which must not need the
# Visual C++ redistributable just because of this APO.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(better_micro_apo SHARED
    src/apo_settings.cpp
    src/better_micro_apo.cpp
    src/better_micro_apo.def
)
target_include_directories(better_micro_apo PRIVATE src)
target_compile_definitions(better_micro_apo PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00
)
target_compile_options(better_micro_apo PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(better_micro_apo PRIVATE
    ole32 oleaut32 advapi32
    # CBaseAudioProcessingObject + the audio media type helpers it uses.
    AudioBaseProcessingObjectV140.lib audiomediatypecrt.lib AudioEng.lib
    rtworkq.lib Propsys.lib)

# ---------------------------------------------------------------------------
# Embed the APO DLL into the GUI executable
# ---------------------------------------------------------------------------

# The resource compiler needs a config-independent path, so stage a copy of the
# built DLL and embed that. The GUI target depends on the DLL target, which
# guarantees the copy exists before the resource is compiled.
set(APO_EMBED_DIR "${CMAKE_BINARY_DIR}/apo_embed")
set(APO_EMBED_DLL "${APO_EMBED_DIR}/better_micro_apo.dll")

add_custom_command(TARGET better_micro_apo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APO_EMBED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:better_micro_apo>" "${APO_EMBED_DLL}"
    # Touch the resource script so the resource compiler re-runs and picks up
    # the new DLL. MSBuild does not know the script embeds a binary file.
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/apo_dll.rc"
    COMMENT "Staging the APO DLL for embedding"
)

file(TO_CMAKE_PATH "${APO_EMBED_DLL}" APO_EMBED_DLL_RC)
configure_file(gui/apo_dll.rc.in "${CMAKE_BINARY_DIR}/apo_dll.rc" @ONLY)

# ---------------------------------------------------------------------------
# Installer / status GUI
# ---------------------------------------------------------------------------

add_executable(better_micro_apo_gui WIN32
    src/apo_settings.cpp
    gui/apo_monitor.cpp
    gui/apo_ops.cpp
    gui/apo_status.cpp
    gui/apo_util.cpp
    gui/main.cpp
    "${CMAKE_BINARY_DIR}/apo_dll.rc"
)

target_include_directories(better_micro_apo_gui PRIVATE gui src)
target_compile_definitions(better_micro_apo_gui PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00
)
target_compile_options(better_micro_apo_gui PRIVATE
    /W4 /permissive- /EHsc /utf-8
)
target_link_libraries(better_micro_apo_gui PRIVATE
    comctl32 advapi32 shell32 user32 gdi32 ole32 mmdevapi
)

# The GUI writes HKLM and controls Audiosrv, so it always asks for elevation.
target_link_options(better_micro_apo_gui PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)

add_dependencies(better_micro_apo_gui better_micro_apo)

