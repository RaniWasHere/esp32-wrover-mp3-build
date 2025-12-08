add_library(usermod_mp3dec INTERFACE)

target_sources(usermod_mp3dec INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mp3dec.c
)

target_include_directories(usermod_mp3dec INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_mp3dec)