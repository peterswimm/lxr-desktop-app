# ctest: the app quits cleanly, as when MENU is held, and saves its session on the way
# out. It starts for real, not headless: a window on SDL's dummy driver, the sound on
# ALSA's null device, MIDI as the platform starts it. Then it is told to quit with
# SIGTERM, which SDL turns into the same SDL_QUIT the main loop ends on. A thread left
# running at exit ends it in std::terminate instead (exit 134).
file(REMOVE_RECURSE ${DIR})
file(MAKE_DIRECTORY ${DIR})
execute_process(COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy AUDIODEV=null
                        ${TIMEOUT} --preserve-status -s TERM 3 ${APP} --session ${DIR}/session.lxr
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "lxr-sndstation did not quit cleanly (${rc}):\n${out}\n${err}")
endif()
if (NOT EXISTS ${DIR}/session.lxr OR NOT out MATCHES "lxr: saved ")
    message(FATAL_ERROR "lxr-sndstation quit without saving its session:\n${out}\n${err}")
endif()
