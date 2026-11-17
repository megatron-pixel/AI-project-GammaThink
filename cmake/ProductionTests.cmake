# Explicit production inventory: never glob training, policy, probe or benchmark tests.
set(core_tests core relational binary_dispatch problem_input problem problem_wire)
set(production_tests
  summary_maintenance task_plan context_documents task_frontend
  source_reasoner source_quantity source_bindings bound_sources publisher_feed
  publisher_catalogue block_retention retention contribution_publish contribution
  inspection session_tasks knowledge_reasoner source_ingestion read_policy drive_network
  task_ir context conversation knowledge remote_mount remote_query block_pin gateway
  block_cache block_cid virtual_drive archive_registry gks_snapshot gks retrieval
  knowledge_stream source_archive source_compiler source_knowledge journal reasoning
  forward_reasoning hypotheses goal_planner native_cok library translator gpu semantic
  semantic_meter problem_batch problem_signed_plan problem_semantic problem_registry
  problem_sources problem_transaction problem_quantity_frame
  quantified_theory quantified_equations source_quantified parallel_reasoning)
set(runtime_tests
  execution maintenance_scheduler task_session summary_session task_code task_source
  task_refinement task_reviews experiments model_manager model_prompt task_operators
  task_source_quantified task_grammar)
if(GAMMATHINK_WITH_MODEL_RUNTIME)
  list(APPEND production_tests ${runtime_tests})
endif()

find_program(BUN_EXECUTABLE bun)
if(BUN_EXECUTABLE)
  set(bun_tests
    bound_sources_cli catalogue_cli contribution_cli direct_graph_cli maintenance_cli
    parallel_runtime_cli runtime_cli source_cli source_quantity_cli source_reasoner_cli
    task_plan_cli)
  if(EXISTS "${PROJECT_SOURCE_DIR}/viewer/graph.mjs")
    list(APPEND bun_tests viewer_graph)
  endif()
  if(EXISTS "${PROJECT_SOURCE_DIR}/viewer/server.mjs")
    list(APPEND bun_tests viewer_server)
  endif()
  foreach(test IN LISTS bun_tests)
    if(EXISTS "${PROJECT_SOURCE_DIR}/tests/${test}.test.mjs")
      add_test(NAME "bun-${test}"
        COMMAND "${BUN_EXECUTABLE}" test "./tests/${test}.test.mjs")
      set_tests_properties("bun-${test}" PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 120)
    endif()
  endforeach()
endif()

function(gammathink_test name source library)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/${source}")
    return()
  endif()
  add_executable(${name} "tests/${source}")
  target_link_libraries(${name} PRIVATE ${library})
  if(name STREQUAL "problem-registry-test")
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>
      "${PROJECT_SOURCE_DIR}/tests/fixtures/linear-unit-context.json")
  else()
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
  endif()
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 120)
endfunction()
foreach(test IN LISTS core_tests)
  string(REPLACE "_" "-" name "${test}-test")
  gammathink_test("${name}" "${test}_test.cpp" gammathink_core)
endforeach()
foreach(test IN LISTS production_tests)
  string(REPLACE "_" "-" name "${test}-test")
  gammathink_test("${name}" "${test}_test.cpp" gammathink_components)
endforeach()
# The historical executable name differs from the source basename.
gammathink_test(document-retrieval-test context_document_retrieval_test.cpp gammathink_components)

foreach(fixture contribution viewer remote_query_response parallel_runtime)
  if(EXISTS "${PROJECT_SOURCE_DIR}/tests/${fixture}_fixture.cpp")
    string(REPLACE "_" "-" name "${fixture}-fixture")
    add_executable(${name} "tests/${fixture}_fixture.cpp")
    if(fixture STREQUAL "parallel_runtime")
      target_link_libraries(${name} PRIVATE gammathink_core)
    else()
      target_link_libraries(${name} PRIVATE gammathink_components)
    endif()
  endif()
endforeach()
if(GAMMATHINK_WITH_MODEL_RUNTIME)
  foreach(test execution task_code)
    string(REPLACE "_" "-" name "${test}-test")
    if(TARGET ${name})
      add_dependencies(${name} execution-helper)
    endif()
  endforeach()
endif()
