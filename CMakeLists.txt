cmake_minimum_required(VERSION 3.16)
project(intezam LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBTORRENT REQUIRED IMPORTED_TARGET libtorrent-rasterbar>=2.1.0)
find_package(Threads REQUIRED)

add_library(intezam_core
    src/cache_geometry.cpp
    src/cache_file.cpp
    src/piece_store.cpp
    src/dynamic_segment_cache.cpp
    src/cache_disk_io.cpp
    src/torrent_cache_session.cpp
    src/cache_simulation.cpp
    src/torrent_metadata.cpp
    src/video_files.cpp
    src/video_selection.cpp
)
target_compile_features(intezam_core PUBLIC cxx_std_17)
set_target_properties(intezam_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(intezam_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(intezam_core PUBLIC PkgConfig::LIBTORRENT Threads::Threads)
target_compile_options(intezam_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(intezam main.cpp)
target_link_libraries(intezam PRIVATE intezam_core)
target_compile_options(intezam PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
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
