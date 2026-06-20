# Twiki — natural-language USD agent, packaged as a usdtweak addon.
#
# Pulled in by src/addons/CMakeLists.txt via its glob over */CMakeLists.txt.
# Adds cpp-httplib (header-only) + a private, statically-linked OpenSSL (built
# from source via FetchContent) for HTTPS. Building our own static OpenSSL means
# usdtweak depends on no system OpenSSL and ships no extra shared libraries.
#
# Off by default — enable with -DBUILD_TWIKI=ON (our CI builds set this).

option(BUILD_TWIKI "Build the Twiki LLM agent addon" OFF)
if (NOT BUILD_TWIKI)
    message(STATUS "Twiki addon: BUILD_TWIKI is OFF — skipping. Enable with -DBUILD_TWIKI=ON")
    return()
endif()

include(FetchContent)

# --- cpp-httplib (header-only) ------------------------------------------------
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL          OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

# --- OpenSSL (built from source, linked statically) ---------------------------
# Notes for CI:
#   * Linux: OpenSSL's Perl Configure needs FindBin/IPC::Cmd/Data::Dumper —
#     install perl-FindBin perl-IPC-Cmd perl-Data-Dumper (see build-linux.yml).
#   * Windows: OPENSSL_USE_CCACHE OFF avoids ccache wrapping `cl` under NMAKE,
#     which breaks the OpenSSL build (NMAKE U1077). `no-asm` (below) drops the
#     NASM requirement so nothing extra needs installing on the runner.
set(OPENSSL_TARGET_VERSION 3.3.2 CACHE STRING "OpenSSL version to fetch")
set(OPENSSL_INSTALL    OFF CACHE BOOL "" FORCE)
set(OPENSSL_USE_CCACHE OFF CACHE BOOL "" FORCE)
if (WIN32)
    # Build without assembly so we don't depend on NASM being installed on the
    # Windows runner. The crypto perf cost is irrelevant for our HTTPS usage.
    set(OPENSSL_CONFIGURE_OPTIONS no-asm CACHE STRING "" FORCE)
endif()
FetchContent_Declare(
    openssl-cmake
    GIT_REPOSITORY https://github.com/jimmy-park/openssl-cmake.git
    GIT_TAG        3.3.2
)
# Force a static OpenSSL regardless of the project-wide BUILD_SHARED_LIBS:
# openssl-cmake adds `no-shared` to Configure when BUILD_SHARED_LIBS is OFF.
set(_twiki_saved_build_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(openssl-cmake)
set(BUILD_SHARED_LIBS "${_twiki_saved_build_shared}")

set(_twiki_tls_define CPPHTTPLIB_OPENSSL_SUPPORT)
set(_twiki_tls_libs   OpenSSL::SSL OpenSSL::Crypto)

# --- addon sources ------------------------------------------------------------
usdtweak_add_addon(Twiki
    SOURCES
        JsHelpers.cpp
        LLMBackend.cpp
        AnthropicBackend.cpp
        OpenAIBackend.cpp
        HttpClient.cpp
        UsdToolDispatcher.cpp
        UsdTools.cpp
        AgentOrchestrator.cpp
        AgentChatPanel.cpp
        TwikiAddon.cpp
        JsHelpers.h
        LLMBackend.h
        AnthropicBackend.h
        OpenAIBackend.h
        HttpClient.h
        UsdToolDispatcher.h
        UsdTools.h
        AgentOrchestrator.h
        AgentChatPanel.h
)

# httplib headers + src/commands/ (UsdToolDispatcher includes "Commands.h").
target_include_directories(usdtweak PRIVATE
    ${cpp_httplib_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/commands
)
target_compile_definitions(usdtweak PRIVATE ${_twiki_tls_define})
target_link_libraries(usdtweak ${_twiki_tls_libs})

# ---- tests -------------------------------------------------------------------
# Test executables don't link the addon registration TU (no ImGui context, no
# Editor). They compile the agent sources directly with fixture-provided
# stage/layer providers via UsdToolDispatcher's constructor.

set(_twiki_agent_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/JsHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LLMBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnthropicBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenAIBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdToolDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentOrchestrator.cpp
    # UTQL core (synchronous compile+execute path used by the run_query tool).
    # NOT UtqlEngine.cpp — that pulls in addons/Api.h / the Editor and is the
    # UI-thread async wrapper the dispatcher deliberately bypasses.
    ${CMAKE_SOURCE_DIR}/src/utql/Lexer.cpp
    ${CMAKE_SOURCE_DIR}/src/utql/Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/utql/Binder.cpp
    ${CMAKE_SOURCE_DIR}/src/utql/Executor.cpp
)

set(_twiki_command_sources_for_tests
    ${CMAKE_SOURCE_DIR}/src/commands/CommandStack.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/CommandsImpl.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfCommandGroup.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfCommandGroupRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfLayerInstructions.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfUndoRedoRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/UndoLayerStateDelegate.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/AttributeCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/Selection.cpp
)

# Off by default — opt in with `-DUSDTWEAK_BUILD_TWIKI_TESTS=ON` at configure
# time. Keeps incremental builds and CI for everyone else fast.
option(USDTWEAK_BUILD_TWIKI_TESTS "Build Twiki addon unit tests" OFF)
if (USDTWEAK_BUILD_TWIKI_TESTS)
    if (NOT TARGET check)
        add_custom_target(check)
    endif()

    function(_twiki_add_test name add_to_check)
        set(_extra_sources "")
        foreach(_src ${ARGN})
            list(APPEND _extra_sources ${_src})
        endforeach()
        list(APPEND _extra_sources ${_twiki_command_sources_for_tests})

        add_executable(${name} ${_extra_sources})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PXR_INCLUDE_DIRS}
            ${cpp_httplib_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/commands
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(${name} PRIVATE ${PXR_LIBRARIES} ${_twiki_tls_libs})
        target_compile_definitions(${name} PRIVATE ${_twiki_tls_define})
        set_target_properties(${name} PROPERTIES FOLDER "addons/Twiki/tests")
        add_custom_target(run_${name}
            COMMAND ${name}
            DEPENDS ${name}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
        if (${add_to_check})
            add_dependencies(check run_${name})
        endif()
    endfunction()

    _twiki_add_test(test_js_helpers TRUE
        ${CMAKE_CURRENT_SOURCE_DIR}/JsHelpers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestJsHelpers.cpp
    )

    _twiki_add_test(test_backend_format TRUE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestBackendFormat.cpp
    )

    _twiki_add_test(test_live_anthropic FALSE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveAnthropic.cpp
    )

    _twiki_add_test(test_live_tool_call FALSE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveToolCall.cpp
    )

    _twiki_add_test(test_usd_dispatcher TRUE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdDispatcher.cpp
    )

    _twiki_add_test(test_live_agent FALSE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveAgent.cpp
    )

    _twiki_add_test(test_usd_edit TRUE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdEdit.cpp
    )

    _twiki_add_test(test_usd_select TRUE
        ${_twiki_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdSelect.cpp
    )
endif()
