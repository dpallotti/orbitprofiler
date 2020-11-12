# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.15)

project(MyTarget)
add_executable(MyTarget
        MyTarget.cpp)
target_compile_options(MyTarget PRIVATE ${STRICT_COMPILE_FLAGS})
target_link_libraries(MyTarget PRIVATE
        OrbitBase
        OrbitProtos
        OrbitServiceLib
        concurrentqueue::concurrentqueue)
