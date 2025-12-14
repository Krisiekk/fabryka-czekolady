cmake_minimum_required(VERSION 3.10)
project(FabrykaCzekolady LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nagłówki
include_directories(${CMAKE_SOURCE_DIR}/include)

# binarki obok siebie w build/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(magazyn    src/magazyn.cpp)
add_executable(dyrektor   src/dyrektor.cpp)
add_executable(dostawca   src/dostawca.cpp)
add_executable(stanowisko src/stanowisko.cpp)

# --- ipc.key obok binarek (ważne dla ftok("./ipc.key", ...)) ---

# jeśli masz ipc.key w repo (root), kopiuj; jeśli nie ma, utwórz pusty w build/
set(IPC_KEY_SRC "${CMAKE_SOURCE_DIR}/ipc.key")

function(ensure_ipc_key TARGET_NAME)
  if (EXISTS "${IPC_KEY_SRC}")
    add_custom_command(
      TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${IPC_KEY_SRC}"
              "$<TARGET_FILE_DIR:${TARGET_NAME}>/ipc.key"
      COMMENT "Copy ipc.key next to ${TARGET_NAME}"
    )
  else()
    add_custom_command(
      TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E touch
              "$<TARGET_FILE_DIR:${TARGET_NAME}>/ipc.key"
      COMMENT "Create empty ipc.key next to ${TARGET_NAME}"
    )
  endif()
endfunction()

ensure_ipc_key(magazyn)
ensure_ipc_key(dyrektor)
ensure_ipc_key(dostawca)
ensure_ipc_key(stanowisko)