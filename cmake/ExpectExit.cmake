# Runs EXE with ARGS ("|"-separated) and checks the exit code and, optionally, that the output matches a regex.
# Inputs: EXE, ARGS, EXPECT (exit code), MATCH (optional regex over stdout + stderr), WORKDIR (optional).

string(REPLACE "|" ";" arg_list "${ARGS}")
if(NOT WORKDIR)
  set(WORKDIR ".")
endif()
execute_process(COMMAND "${EXE}" ${arg_list} WORKING_DIRECTORY "${WORKDIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT rc STREQUAL "${EXPECT}")
  message(FATAL_ERROR "penumbra ${arg_list}\nexpected exit code ${EXPECT}, got ${rc}\n--- output ---\n${all}")
endif()
if(DEFINED MATCH AND NOT all MATCHES "${MATCH}")
  message(FATAL_ERROR "penumbra ${arg_list}\noutput does not match '${MATCH}'\n--- output ---\n${all}")
endif()
message(STATUS "exit ${rc} as expected")
