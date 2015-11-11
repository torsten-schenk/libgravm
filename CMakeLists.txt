cmake_minimum_required(VERSION 2.8.8)

project(gravm)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")

find_package(BTree REQUIRED)
include_directories(${BTREE_INCLUDE_DIRS})

set(gravm_SOURCES
	runstack.c
)

set(gravm_SOURCE_FILES)
set(gravm_HEADER_FILES)
set(testlib_SOURCE_FILES)

foreach(x ${gravm_SOURCES})
	list(APPEND gravm_SOURCE_FILES "src/${x}")
endforeach()

foreach(x ${gravm_HEADERS})
	list(APPEND gravm_HEADER_FILES "src/${x}")
endforeach()

include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_executable(alltest test/main.c ${gravm_SOURCE_FILES} ${gravm_HEADER_FILES} test/runstack.h)
target_link_libraries(alltest ${BTREE_LIBRARIES} -lcunit)
set_target_properties(alltest PROPERTIES COMPILE_FLAGS -DTESTING)

add_library(gravm SHARED ${gravm_SOURCE_FILES} ${gravm_HEADER_FILES})
target_link_libraries(gravm ${BTREE_LIBRARIES})
install(TARGETS gravm DESTINATION lib)
install(FILES include/gravm/runstack.h DESTINATION include/gravm)

