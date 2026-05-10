# src/agent — natural-language USD agent integration.
# Step 1: JsHelpers.
# Step 2: LLMBackend + Anthropic/OpenAI request and response formatting.
# Step 3: HTTP via cpp-httplib + TLS (system OpenSSL or fetched mbedTLS).

include(FetchContent)

# --- cpp-httplib (header-only, single header used directly) -------------
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)
# We don't want cpp-httplib's own CMake target (it auto-detects OpenSSL and
# adds compile defines that we control ourselves). Just populate the source.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL          OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

# --- OpenSSL via FetchContent --------------------------------------------
# Uses jimmy-park/openssl-cmake: a CMake wrapper that downloads and builds
# OpenSSL from source. The first configure takes a few minutes; subsequent
# configures are cached. Exposes OpenSSL::SSL and OpenSSL::Crypto targets.
set(OPENSSL_TARGET_VERSION 3.3.2 CACHE STRING "OpenSSL version to fetch")
set(OPENSSL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    openssl-cmake
    GIT_REPOSITORY https://github.com/jimmy-park/openssl-cmake.git
    GIT_TAG        3.3.2
)
FetchContent_MakeAvailable(openssl-cmake)

set(_agent_tls_define CPPHTTPLIB_OPENSSL_SUPPORT)
set(_agent_tls_libs   OpenSSL::SSL OpenSSL::Crypto)

set(_agent_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/JsHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LLMBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnthropicBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenAIBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdToolDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentOrchestrator.cpp
)
set(_agent_headers
    ${CMAKE_CURRENT_SOURCE_DIR}/JsHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LLMBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/AnthropicBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenAIBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdToolDispatcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UsdTools.h
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentOrchestrator.h
)

target_sources(usdtweak PRIVATE ${_agent_sources} ${_agent_headers})

# UI panel — only added to the usdtweak target (depends on Dear ImGui).
# Excluded from the test executables, which have no ImGui context.
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentChatPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AgentChatPanel.h
)
target_include_directories(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${cpp_httplib_SOURCE_DIR}
    # UsdToolDispatcher's edit tools include "Commands.h" from src/commands/.
    ${CMAKE_SOURCE_DIR}/src/commands
)
target_compile_definitions(usdtweak PRIVATE ${_agent_tls_define})
# Plain (no-keyword) signature to match the call in the root CMakeLists.txt.
target_link_libraries(usdtweak ${_agent_tls_libs})

# Subset of src/commands/ sources required for the agent's edit tools.
# usdtweak's main target gets these via src/commands/CMakeLists.txt; test
# executables that link _agent_sources need them too because UsdToolDispatcher
# now references ExecuteAfterDraw<AttributeSet>, <UndoCommand>, … and the
# template instantiations live in those .cpps.
set(_command_sources_for_agent_tests
    ${CMAKE_SOURCE_DIR}/src/commands/CommandStack.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/CommandsImpl.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfCommandGroup.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfCommandGroupRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfLayerInstructions.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/SdfUndoRedoRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/UndoLayerStateDelegate.cpp
    ${CMAKE_SOURCE_DIR}/src/commands/AttributeCommands.cpp
    # Selection.cpp implements the Selection class used by the dispatcher's
    # get_selection / select_prims tools.
    ${CMAKE_SOURCE_DIR}/src/Selection.cpp
    # Intentionally NOT included: PrimCommands.cpp / UsdAPICommands.cpp /
    # LayerCommands.cpp / EditorCommands.cpp pull in editor-side helpers
    # (UsdHelpers, FindNextAvailableTokenString, …) that the standalone
    # tests don't need. The agent's edit tools only require AttributeSet
    # and the undo/redo plumbing in CommandStack.cpp.
)

# Test executables. No HTTP, no USD scene, no agent loop.
option(USDTWEAK_BUILD_AGENT_TESTS "Build agent unit tests" ON)
if (USDTWEAK_BUILD_AGENT_TESTS)
    if (NOT TARGET check)
        add_custom_target(check)
    endif()

    # `add_to_check` controls whether `run_<name>` is added to the aggregate
    # `check` target. Live API tests opt out (require network + key).
    function(_agent_add_test name add_to_check)
        # Pull in the command sources whenever the test pulls in agent
        # sources (UsdToolDispatcher.cpp references ExecuteAfterDraw template
        # instantiations from those .cpps).
        set(_extra_sources "")
        foreach(_src ${ARGN})
            list(APPEND _extra_sources ${_src})
        endforeach()
        list(APPEND _extra_sources ${_command_sources_for_agent_tests})

        add_executable(${name} ${_extra_sources})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PXR_INCLUDE_DIRS}
            ${cpp_httplib_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/commands
            # PrimCommands.cpp etc. include "UsdHelpers.h" from src/.
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(${name} PRIVATE ${PXR_LIBRARIES} ${_agent_tls_libs})
        target_compile_definitions(${name} PRIVATE ${_agent_tls_define})
        set_target_properties(${name} PROPERTIES FOLDER "agent/tests")
        add_custom_target(run_${name}
            COMMAND ${name}
            DEPENDS ${name}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
        if (${add_to_check})
            add_dependencies(check run_${name})
        endif()
    endfunction()

    _agent_add_test(test_js_helpers TRUE
        ${CMAKE_CURRENT_SOURCE_DIR}/JsHelpers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestJsHelpers.cpp
    )

    _agent_add_test(test_backend_format TRUE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestBackendFormat.cpp
    )

    # Step 3 — live API. Not part of `check`; run manually with API key in env.
    _agent_add_test(test_live_anthropic FALSE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveAnthropic.cpp
    )

    # Step 4 — tool-call round-trip with hardcoded fake results. Live, not in `check`.
    _agent_add_test(test_live_tool_call FALSE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveToolCall.cpp
    )

    # Step 5 — UsdToolDispatcher exercising every read-only tool against an
    # in-memory stage. No network, in `check`.
    _agent_add_test(test_usd_dispatcher TRUE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdDispatcher.cpp
    )

    # Step 6 — full agent integration: orchestrator + dispatcher + real LLM
    # against a fixture stage. Live, not in `check`.
    _agent_add_test(test_live_agent FALSE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestLiveAgent.cpp
    )

    # Step 7 — edit tool dispatcher round-trip with undo (no LLM, no network).
    _agent_add_test(test_usd_edit TRUE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdEdit.cpp
    )

    # Selection tools — get_selection / select_prims round-trip.
    _agent_add_test(test_usd_select TRUE
        ${_agent_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestUsdSelect.cpp
    )
endif()
