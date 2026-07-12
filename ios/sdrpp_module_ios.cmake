# Get needed values depending on if this is in-tree or out-of-tree
if (NOT SDRPP_CORE_ROOT)
    set(SDRPP_CORE_ROOT "@SDRPP_CORE_ROOT@")
endif ()
if (NOT SDRPP_MODULE_COMPILER_FLAGS)
    set(SDRPP_MODULE_COMPILER_FLAGS @SDRPP_MODULE_COMPILER_FLAGS@)
endif ()

# Every module is built as an OBJECT library and linked into the main binary.
# Each module's `_INIT_` / `_INFO_` / etc. extern "C" symbols are renamed via
# SDRPP_MODULE_TOKEN (see core/src/module.h) so they don't collide when many
# modules end up in the same image. File-scope globals in module sources
# (e.g. `ConfigManager config;`) need the `static` keyword for the same
# reason — that's a one-line touch per module.
add_library(${PROJECT_NAME} OBJECT ${SRC})
target_include_directories(${PROJECT_NAME} PRIVATE "${SDRPP_CORE_ROOT}/src/")
target_include_directories(${PROJECT_NAME} PRIVATE "${SDRPP_CORE_ROOT}/imgui")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    SDRPP_MODULE_TOKEN=${PROJECT_NAME}_
)
target_compile_options(${PROJECT_NAME} PRIVATE ${SDRPP_MODULE_COMPILER_FLAGS})

# Pull headers / transitive includes (imgui, fftw, volk, ...) via core.
if (TARGET sdrpp_core)
    target_link_libraries(${PROJECT_NAME} PRIVATE sdrpp_core)
endif ()

set_property(GLOBAL APPEND PROPERTY SDRPP_STATIC_MODULE_TARGETS ${PROJECT_NAME})
