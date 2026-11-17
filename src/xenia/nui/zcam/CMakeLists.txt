add_library(xenia-nui-zcam STATIC)
xe_platform_sources(xenia-nui-zcam ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui-zcam PUBLIC xenia-base xenia-nui ws2_32)
xe_target_defaults(xenia-nui-zcam)
