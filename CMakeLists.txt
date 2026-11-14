cmake_minimum_required(VERSION 3.16)
project(intezam LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBTORRENT REQUIRED IMPORTED_TARGET libtorrent-rasterbar>=2.1.0)
pkg_check_modules(LIBVLC REQUIRED IMPORTED_TARGET libvlc>=3.0)
if(LIBVLC_VERSION VERSION_GREATER_EQUAL 4.0)
    message(FATAL_ERROR "This player targets the LibVLC 3.x callback/lifecycle API")
endif()
find_package(Threads REQUIRED)

add_library(intezam_core
    src/cache_geometry.cpp
    src/cache_file.cpp
    src/piece_store.cpp
    src/dynamic_segment_cache.cpp
    src/cache_disk_io.cpp
    src/torrent_cache_session.cpp
    src/cache_simulation.cpp
    src/container_probe.cpp
    src/torrent_video_source.cpp
    src/vlc_player.cpp
    src/vlc_playback.cpp
    src/torrent_metadata.cpp
    src/video_files.cpp
    src/video_selection.cpp
)
target_compile_features(intezam_core PUBLIC cxx_std_17)
set_target_properties(intezam_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(intezam_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(intezam_core PUBLIC PkgConfig::LIBTORRENT PkgConfig::LIBVLC Threads::Threads)
target_compile_options(intezam_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(intezam main.cpp)
target_link_libraries(intezam PRIVATE intezam_core)
target_compile_options(intezam PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(cache_benchmark test/cache_benchmark.cpp)
    target_link_libraries(cache_benchmark PRIVATE intezam_core)
    add_executable(reserved_slot_tests test/reserved_slot_tests.cpp)
    target_link_libraries(reserved_slot_tests PRIVATE intezam_core)
    add_test(NAME reserved_slot_admission COMMAND reserved_slot_tests)
    add_executable(batch_eviction_tests test/batch_eviction_tests.cpp)
    target_link_libraries(batch_eviction_tests PRIVATE intezam_core)
    add_test(NAME batched_torrent_reconstruction COMMAND batch_eviction_tests)
    set_tests_properties(batched_torrent_reconstruction PROPERTIES TIMEOUT 60)
    add_executable(rolling_window_tests test/rolling_window_tests.cpp)
    target_link_libraries(rolling_window_tests PRIVATE intezam_core)
    add_test(NAME rolling_window_stability COMMAND rolling_window_tests)
    add_executable(playback_tests test/playback_tests.cpp)
    target_link_libraries(playback_tests PRIVATE intezam_core)
    add_dependencies(playback_tests intezam)
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    if(FFMPEG_EXECUTABLE)
        set(media_directory ${CMAKE_CURRENT_BINARY_DIR}/playback-fixtures)
        add_test(NAME generate_vlc_media COMMAND ${CMAKE_COMMAND}
            -DFFMPEG=${FFMPEG_EXECUTABLE} -DDESTINATION=${media_directory}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/generate_media.cmake)
        set_tests_properties(generate_vlc_media PROPERTIES FIXTURES_SETUP vlc_media TIMEOUT 120)
        add_test(NAME vlc_cache_playback COMMAND playback_tests
            ${media_directory}/front.mp4 ${media_directory}/tail.mp4
            ${media_directory}/video.mkv ${media_directory}/video.webm
            ${media_directory}/video.mov ${media_directory}/video.m4v
            ${media_directory}/video.avi ${media_directory}/video.ts)
        set_tests_properties(vlc_cache_playback PROPERTIES FIXTURES_REQUIRED vlc_media TIMEOUT 240)
        add_test(NAME cli_magnet_to_playback COMMAND playback_tests --cli
            $<TARGET_FILE:intezam> ${media_directory}/tail.mp4)
        set_tests_properties(cli_magnet_to_playback PROPERTIES FIXTURES_REQUIRED vlc_media TIMEOUT 45)
        add_test(NAME cleanup_vlc_media COMMAND ${CMAKE_COMMAND}
            -DDESTINATION=${media_directory} -DCLEANUP=ON
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/generate_media.cmake)
        set_tests_properties(cleanup_vlc_media PROPERTIES FIXTURES_CLEANUP vlc_media)
    else()
        message(STATUS "FFmpeg not found: real media playback integration tests will not run")
    endif()
    add_executable(container_tests test/container_tests.cpp)
    target_link_libraries(container_tests PRIVATE intezam_core)
    add_test(NAME bounded_container_probing COMMAND container_tests)
    add_executable(streaming_tests test/streaming_tests.cpp)
    target_link_libraries(streaming_tests PRIVATE intezam_core)
    add_test(NAME sliding_cache_local_peer COMMAND streaming_tests)
    set_tests_properties(sliding_cache_local_peer PROPERTIES TIMEOUT 60)
    add_executable(cache_tests test/cache_tests.cpp)
    target_link_libraries(cache_tests PRIVATE intezam_core)
    add_test(NAME bounded_disk_cache COMMAND cache_tests)
    add_executable(video_tests test/video_tests.cpp)
    target_link_libraries(video_tests PRIVATE intezam_core)
    add_test(NAME video_detection_and_selection COMMAND video_tests)

    add_executable(metadata_tests test/metadata_tests.cpp)
    target_link_libraries(metadata_tests PRIVATE intezam_core)
    add_test(NAME metadata_only_local_peer COMMAND metadata_tests)
    set_tests_properties(metadata_only_local_peer PROPERTIES TIMEOUT 30)
endif()
