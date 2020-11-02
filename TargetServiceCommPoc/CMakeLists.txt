cmake_minimum_required(VERSION 3.15)

project(Comm)
add_library(Comm STATIC)
target_compile_options(Comm PRIVATE ${STRICT_COMPILE_FLAGS})
target_sources(Comm PRIVATE
        service.proto)
grpc_helper(Comm)

project(MyService)
add_executable(MyService
        MyService.cpp
        SocketPath.h)
target_compile_options(MyService PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(MyService PUBLIC
        Comm
        OrbitBase
        concurrentqueue::concurrentqueue)

project(MyTarget)
add_executable(MyTarget
        MyTarget.cpp
        SocketPath.h)
target_compile_options(MyTarget PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(MyTarget PUBLIC
        Comm
        OrbitBase
        concurrentqueue::concurrentqueue)

project(Reader)
add_executable(Reader reader.cpp)
target_compile_options(Reader PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(Reader PUBLIC
        Comm
        OrbitBase
        concurrentqueue::concurrentqueue)

project(Writer)
add_executable(Writer writer.cpp)
target_compile_options(Writer PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(Writer PUBLIC
        Comm
        OrbitBase
        concurrentqueue::concurrentqueue)
