cmake_minimum_required(VERSION 3.20)
project(GpuRamDriveVk LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(grd_core STATIC
    src/core/util.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/imdisk_api.cpp
    src/core/proxy_server.cpp
    src/core/image.cpp
    src/core/environment.cpp
    src/core/task.cpp
    src/core/drive.cpp
    src/core/drive_manager.cpp
    src/gpu/vulkan_api.cpp
    src/gpu/vulkan_runtime.cpp
    src/gpu/vulkan_drive.cpp
)

target_include_directories(grd_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)

target_compile_definitions(grd_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_compile_options(grd_core PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
    -Wno-cast-function-type
)

target_link_libraries(grd_core PUBLIC
    taskschd
    ole32
    oleaut32
    uuid
    advapi32
    user32
    shell32
    secur32
)

add_executable(gpuvramdrive-cli src/cli/main_cli.cpp src/cli/app.rc)
target_compile_options(gpuvramdrive-cli PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gpuvramdrive-cli PRIVATE grd_core)
target_link_options(gpuvramdrive-cli PRIVATE
    -static -static-libgcc -static-libstdc++ -municode
)

add_executable(gpuvramdrive WIN32
    src/gui/main_gui.cpp
    src/gui/main_window.cpp
    src/gui/app.rc
)
target_compile_options(gpuvramdrive PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gpuvramdrive PRIVATE grd_core comctl32 comdlg32)
target_link_options(gpuvramdrive PRIVATE
    -static -static-libgcc -static-libstdc++ -municode
)

add_executable(gpuvramdisk-bench tools/bench.cpp)
target_compile_options(gpuvramdisk-bench PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gpuvramdisk-bench PRIVATE grd_core)
target_link_options(gpuvramdisk-bench PRIVATE
    -static -static-libgcc -static-libstdc++
)
