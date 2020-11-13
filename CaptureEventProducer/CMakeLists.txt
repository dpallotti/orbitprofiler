# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

project(CaptureEventProducer)

add_library(CaptureEventProducer STATIC)
target_compile_options(CaptureEventProducer PRIVATE ${STRICT_COMPILE_FLAGS})

target_sources(CaptureEventProducer PUBLIC
        include/CaptureEventProducer/CaptureEventProducer.h
        include/CaptureEventProducer/LockFreeBufferCaptureEventProducer.h)

target_sources(CaptureEventProducer PRIVATE
        CaptureEventProducer.cpp)

target_include_directories(CaptureEventProducer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(CaptureEventProducer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(CaptureEventProducer PUBLIC
        OrbitBase
        OrbitProtos
        OrbitServiceLib
        abseil::abseil
        concurrentqueue::concurrentqueue)
