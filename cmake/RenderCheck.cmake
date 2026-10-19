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
elseif(MODE STREQUAL "resume")
  # Reference, then a run killed (no clean shutdown) after its first checkpoint, then --resume: bit-identical.
  render("${NAME}_ref.exr")
  file(REMOVE "${WORKDIR}/${NAME}_cut.exr" "${WORKDIR}/${NAME}_cut.penfilm" "${WORKDIR}/${NAME}_cut.session.json")
  execute_process(COMMAND "${PENUMBRA}" ${common} -o "${NAME}_cut.exr" --threads 2 --checkpoint 10s
                  WORKING_DIRECTORY "${WORKDIR}" TIMEOUT ${KILL_AFTER} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(rc EQUAL 0)
    message(FATAL_ERROR "the render finished before it was killed after ${KILL_AFTER}s; raise --spp for this test")
  endif()
  message(STATUS "killed after ${KILL_AFTER}s (${rc})")
  if(NOT EXISTS "${WORKDIR}/${NAME}_cut.session.json")
    message(FATAL_ERROR "no session file after the kill:\n${o}${e}")
  endif()
  execute_process(COMMAND "${PENUMBRA}" --config "${WORKDIR}/empty-config.json" --log-file "${WORKDIR}/${NAME}.log"
                          --terminal --resume "${NAME}_cut.session.json" --progress plain
                  WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "--resume failed (exit ${rc}):\n${o}${e}")
  endif()
  if(NOT "${o}${e}" MATCHES "resume: [1-9][0-9]* of [0-9]+ items are already in the film")
    message(FATAL_ERROR "the resumed render did not reuse any tile from the film:\n${o}${e}")
  endif()
  string(REGEX MATCH "resume: [0-9]+ of [0-9]+ items" reused "${o}${e}")
  message(STATUS "${reused} reused")
  diff("${NAME}_cut.exr" "${NAME}_ref.exr" --exact)
  if(EXISTS "${WORKDIR}/${NAME}_cut.session.json")
    message(FATAL_ERROR "the session file should be deleted after the render completes")
  endif()
elseif(MODE STREQUAL "interrupt")
  # Ctrl+C (SIGINT): exit code 5, session kept as "interrupted", current image written; --resume finishes it
  # and matches an uninterrupted render bit for bit.
  render("${NAME}_ref.exr")
  file(REMOVE "${WORKDIR}/${NAME}_int.exr" "${WORKDIR}/${NAME}_int.penfilm" "${WORKDIR}/${NAME}_int.session.json")
  execute_process(COMMAND timeout --preserve-status -s INT ${KILL_AFTER} "${PENUMBRA}" ${common} -o "${NAME}_int.exr"
                          --threads 2
                  WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 5)
    message(FATAL_ERROR "expected exit code 5 after Ctrl+C, got ${rc}:\n${o}${e}")
  endif()
  if(NOT EXISTS "${WORKDIR}/${NAME}_int.exr")
    message(FATAL_ERROR "the interrupted render did not write its current image")
  endif()
  file(READ "${WORKDIR}/${NAME}_int.session.json" session)
  if(NOT session MATCHES "\"state\": \"interrupted\"")
    message(FATAL_ERROR "session state is not 'interrupted':\n${session}")
  endif()
  execute_process(COMMAND "${PENUMBRA}" --config "${WORKDIR}/empty-config.json" --log-file "${WORKDIR}/${NAME}.log"
                          --terminal --resume "${NAME}_int.session.json" --progress plain
                  WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "--resume failed (exit ${rc}):\n${o}${e}")
  endif()
  diff("${NAME}_int.exr" "${NAME}_ref.exr" --exact)
elseif(MODE STREQUAL "merge_regions")
  render("${NAME}_ref.exr")
  render("${NAME}_left.exr" --region 0,0,1024,1080)
  render("${NAME}_right.exr" --region 1024,0,1920,1080)
  execute_process(COMMAND "${PENUMBRA}" --config "${WORKDIR}/empty-config.json" --log-file "${WORKDIR}/${NAME}.log"
                          --bit-depth 32 merge "${NAME}_left.penfilm" "${NAME}_right.penfilm" -o "${NAME}_merged.exr"
                  WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "merge failed (exit ${rc}):\n${o}${e}")
  endif()
  diff("${NAME}_merged.exr" "${NAME}_ref.exr" --exact)
elseif(MODE STREQUAL "merge_samples")
  render("${NAME}_ref.exr")
  render("${NAME}_a.exr" --sample-range 0-3)
  render("${NAME}_b.exr" --sample-range 4-7)
  execute_process(COMMAND "${PENUMBRA}" --config "${WORKDIR}/empty-config.json" --log-file "${WORKDIR}/${NAME}.log"
                          --bit-depth 32 merge "${NAME}_a.penfilm" "${NAME}_b.penfilm" -o "${NAME}_merged.exr"
                  WORKING_DIRECTORY "${WORKDIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "merge failed (exit ${rc}):\n${o}${e}")
  endif()
  # Float rounding of the weighted mean differs slightly from one 8-sample average (docs/GUIDE.md: < 1e-6).
  diff("${NAME}_merged.exr" "${NAME}_ref.exr" --max-relmse 1e-6)
elseif(MODE STREQUAL "region_crop")
  render("${NAME}_ref.exr")
  render("${NAME}_crop.exr" --region 300,200,700,500)
  diff("${NAME}_ref.exr" "${NAME}_crop.exr" --crop 300,200,700,500 --exact)
elseif(MODE STREQUAL "presets")
  foreach(p 120p 240p 360p 480p 720p 1080p 2k 4k 6k 8k 10k 15k 20k 40k 60k 80k 160k)
    render("${NAME}_${p}.exr" --res ${p})
  endforeach()
elseif(MODE STREQUAL "large")
  # A whole 20K frame, then one tile rendered alone with --region must match the same pixels of the big file.
  render("${NAME}.exr")
  render("${NAME}_tile.exr" --region 10240,5632,10496,5888)
  diff("${NAME}.exr" "${NAME}_tile.exr" --crop 10240,5632,10496,5888 --exact)
else()
  message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
