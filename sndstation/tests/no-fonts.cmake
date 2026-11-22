# ctest: with no Open Sans where it looks, the app does not start, says why, and draws
# nothing; it never falls back to the platform's face. $LXR_FONT_DIR, when set, is the
# only place it looks, so an empty one stands in for a unit without the files.
file(REMOVE_RECURSE ${DIR})
file(MAKE_DIRECTORY ${DIR}/fonts ${DIR}/shots)
execute_process(COMMAND ${CMAKE_COMMAND} -E env LXR_FONT_DIR=${DIR}/fonts ${APP} --shots ${DIR}/shots
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if (rc EQUAL 0)
    message(FATAL_ERROR "lxr-sndstation started without Open Sans:\n${out}\n${err}")
endif()
if (NOT err MATCHES "no Open Sans")
    message(FATAL_ERROR "lxr-sndstation failed (${rc}) without saying it was the fonts:\n${out}\n${err}")
endif()
file(GLOB drawn ${DIR}/shots/*)
if (drawn)
    message(FATAL_ERROR "lxr-sndstation drew without Open Sans: ${drawn}")
endif()
