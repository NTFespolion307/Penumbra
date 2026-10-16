# Renders with penumbra and checks the result with penumbra-diff.
# Inputs: PENUMBRA, DIFF, WORKDIR, NAME, SCENE, ARGS ("|"-separated extra flags), MODE and per mode:
#   uniform      TOLERANCE                 every pixel within TOLERANCE (relative) of 1.0 (furnace test)
#   threads      THREADS ("|"-separated)   renders once per thread count; all outputs must be bit-identical
#   reference    REFERENCE, MAXRELMSE      relMSE against a golden image
#   runs         -                         the render must succeed

string(REPLACE "|" ";" extra "${ARGS}")
set(common --terminal "${SCENE}" --config "${WORKDIR}/empty-config.json" --log-file "${WORKDIR}/${NAME}.log"
           --progress plain --bit-depth 32 ${extra})
file(WRITE "${WORKDIR}/empty-config.json" "{}\n")

function(render out)
  execute_process(COMMAND "${PENUMBRA}" ${common} -o "${out}" ${ARGN} WORKING_DIRECTORY "${WORKDIR}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "render failed (exit ${rc}):\n${o}${e}")
  endif()
  message(STATUS "rendered ${out}")
endfunction()

function(diff)
  execute_process(COMMAND "${DIFF}" ${ARGN} WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o
                  ERROR_VARIABLE e)
  message(STATUS "${o}${e}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "penumbra-diff ${ARGN} failed (exit ${rc})")
  endif()
endfunction()

if(MODE STREQUAL "uniform")
  render("${NAME}.exr")
  diff("${NAME}.exr" --uniform 1 --tolerance ${TOLERANCE})
elseif(MODE STREQUAL "threads")
  string(REPLACE "|" ";" thread_list "${THREADS}")
  set(first "")
  foreach(t IN LISTS thread_list)
    render("${NAME}_t${t}.exr" --threads ${t})
    if(first STREQUAL "")
      set(first "${NAME}_t${t}.exr")
    else()
      diff("${first}" "${NAME}_t${t}.exr" --exact)
    endif()
  endforeach()
elseif(MODE STREQUAL "reference")
  render("${NAME}.exr")
  diff("${NAME}.exr" "${REFERENCE}" --max-relmse ${MAXRELMSE})
elseif(MODE STREQUAL "runs")
  render("${NAME}.exr")
else()
  message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
