# Stage config.txt next to VRMPet.exe, and only if there is not one there.
#
# Run from CMakeLists.txt as a POST_BUILD step with -DSRC= and -DDEST=.
#
#   if(EXISTS ${DEST})   leave it alone. The live copy is the one holding the
#                         API key and the user's tuning; overwriting it on every
#                         build would silently throw both away, and the build
#                         would then look like it had deleted the configuration.
#   else                  copy the shipped file, which is a template with every
#                         key in it and an empty FISH_API_KEY.
#
# A plain `cmake -E copy_if_different` cannot express "only if absent", and
# copy_if_different would happily overwrite the live file every time the
# template changes - which is exactly the case this script is written against.
if(NOT DEFINED SRC OR NOT DEFINED DEST)
    message(FATAL_ERROR "stage_config.cmake needs -DSRC= and -DDEST=")
endif()

get_filename_component(DIR "${DEST}" DIRECTORY)

if(EXISTS "${DEST}")
    message(STATUS "config.txt already beside the exe: keeping ${DEST}")
elseif(EXISTS "${SRC}")
    file(COPY "${SRC}" DESTINATION "${DIR}")
    message(STATUS "staged config.txt: ${SRC} -> ${DEST}")
else()
    message(WARNING "no config.txt to stage (expected at ${SRC}); VRMPet will write its own on the next run")
endif()
