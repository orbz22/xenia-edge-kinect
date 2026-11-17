add_library(xenia-nui-vmc STATIC)
xe_platform_sources(xenia-nui-vmc ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui-vmc PUBLIC xenia-base xenia-nui ws2_32)
xe_target_defaults(xenia-nui-vmc)
