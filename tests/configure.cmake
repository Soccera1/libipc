set(generator_arguments -G "${GENERATOR}")
if(PLATFORM)
  list(APPEND generator_arguments -A "${PLATFORM}")
endif()
if(TOOLSET)
  list(APPEND generator_arguments -T "${TOOLSET}")
endif()
foreach(test backend reactor)
  if(test STREQUAL "backend")
    set(argument -DIPC_MESSAGE_BACKEND=nonexistent)
  else()
    set(argument -DIPC_EVENT_BACKEND=nonexistent)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/${test}"
    ${generator_arguments} "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
    "-DCMAKE_C_COMPILER=${COMPILER}" "-DCMAKE_C_FLAGS=${C_FLAGS}"
    "${argument}" -DBUILD_TESTING=OFF
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "requires|Unsupported Unix reactor")
    message(FATAL_ERROR "Invalid configuration did not fail as expected: ${output}${error}")
  endif()
endforeach()
