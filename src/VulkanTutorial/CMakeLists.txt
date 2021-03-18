project(VulkanTutorial)

add_executable(VulkanTutorial)
target_compile_options(VulkanTutorial PRIVATE ${STRICT_COMPILE_FLAGS})

target_sources(VulkanTutorial PRIVATE
        frag_spv.h
        main.cpp
        vert_spv.h)

target_link_libraries(VulkanTutorial PRIVATE
        OrbitBase
        Vulkan::Vulkan
        CONAN_PKG::vulkan-headers)

