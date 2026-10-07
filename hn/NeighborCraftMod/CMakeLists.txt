set(TARGET NeighborCraftMod)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/bridge.cpp
    src/compositor.cpp
    src/ws.cpp
)

# ReShade's add-on API headers (fetched by fetch_deps.sh, not redistributed here)
set(RESHADE_INCLUDE "$ENV{USERPROFILE}/NeighborCraft-lab/deps/reshade" CACHE PATH "ReShade add-on headers")
target_include_directories(${TARGET} PRIVATE src "${RESHADE_INCLUDE}")
target_compile_definitions(${TARGET} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(${TARGET} PUBLIC UE4SS ws2_32)
