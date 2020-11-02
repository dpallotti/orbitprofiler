cmake_minimum_required(VERSION 3.15)

project(Reader)
add_executable(Reader reader.cpp)
target_compile_options(Reader PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(Reader PUBLIC
        OrbitBase
        OrbitVersion
        concurrentqueue::concurrentqueue)

project(Writer)
add_executable(Writer writer.cpp)
target_compile_options(Writer PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(Writer PUBLIC
        OrbitBase
        OrbitVersion
        concurrentqueue::concurrentqueue)
