set(GAMMATHINK_LLAMA_ROOT "" CACHE PATH "External llama.cpp source tree")
set(GAMMATHINK_LLAMA_RUNTIME_DIR "" CACHE PATH "Directory containing built llama.cpp libraries")
if(NOT IS_ABSOLUTE "${GAMMATHINK_LLAMA_ROOT}" OR
   NOT IS_ABSOLUTE "${GAMMATHINK_LLAMA_RUNTIME_DIR}")
  message(FATAL_ERROR "Model runtime requires absolute GAMMATHINK_LLAMA_ROOT and GAMMATHINK_LLAMA_RUNTIME_DIR")
endif()
foreach(header include/llama.h ggml/include/gguf.h common/jinja/parser.h)
  if(NOT EXISTS "${GAMMATHINK_LLAMA_ROOT}/${header}")
    message(FATAL_ERROR "Missing external llama.cpp header: ${header}")
  endif()
endforeach()
find_path(GAMMATHINK_LLAMA_JSON_INCLUDE_DIR json.h
  PATHS "${GAMMATHINK_LLAMA_ROOT}/common" "${GAMMATHINK_LLAMA_ROOT}/vendor"
  NO_DEFAULT_PATH REQUIRED)
add_library(gammathink_model_runtime INTERFACE)
target_include_directories(gammathink_model_runtime SYSTEM INTERFACE
  "${GAMMATHINK_LLAMA_ROOT}"
  "${GAMMATHINK_LLAMA_ROOT}/include"
  "${GAMMATHINK_LLAMA_ROOT}/ggml/include"
  "${GAMMATHINK_LLAMA_ROOT}/common"
  "${GAMMATHINK_LLAMA_ROOT}/vendor"
  "${GAMMATHINK_LLAMA_ROOT}/src"
  "${GAMMATHINK_LLAMA_JSON_INCLUDE_DIR}")
foreach(name llama-common llama ggml-base)
  string(REPLACE "-" "_" variable "GAMMATHINK_${name}_LIBRARY")
  find_library(${variable} NAMES "${name}"
    PATHS "${GAMMATHINK_LLAMA_RUNTIME_DIR}" NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(gammathink_model_runtime INTERFACE "${${variable}}")
endforeach()
set(CMAKE_BUILD_RPATH "${GAMMATHINK_LLAMA_RUNTIME_DIR}")
