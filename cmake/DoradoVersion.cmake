set(DORADO_VERSION_MAJOR 0)
set(DORADO_VERSION_MINOR 0)
set(DORADO_VERSION_REV 1)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  message(************* ${PROJECT_SOURCE_DIR} ************)
  execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --pretty=format:%h OUTPUT_VARIABLE DORADO_SHORT_HASH WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet RESULT_VARIABLE IS_DIRTY WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  if(IS_DIRTY)
    string(APPEND DORADO_SHORT_HASH "+dirty")
  endif()
else()
  set(DORADO_SHORT_HASH 0)
endif()

set(DORADO_VERSION "${DORADO_VERSION_MAJOR}.${DORADO_VERSION_MINOR}.${DORADO_VERSION_REV}+${DORADO_SHORT_HASH}")
