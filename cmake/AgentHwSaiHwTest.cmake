# CMake to build libraries and binaries in fboss/agent/hw/sai/hw_test

# In general, libraries and binaries in fboss/foo/bar are built by
# cmake/FooBar.cmake

add_library(thrift_test_handler
  fboss/agent/hw/sai/hw_test/SaiTestHandler.cpp
)

target_link_libraries(thrift_test_handler
  diag_shell
  sai_test_ctrl_cpp2
)

set_target_properties(thrift_test_handler PROPERTIES COMPILE_FLAGS
  "-DSAI_VER_MAJOR=${SAI_VER_MAJOR} \
  -DSAI_VER_MINOR=${SAI_VER_MINOR}  \
  -DSAI_VER_RELEASE=${SAI_VER_RELEASE}"
)

add_library(sai_switch_ensemble
  fboss/agent/hw/sai/hw_test/HwSwitchEnsembleFactory.cpp
  fboss/agent/hw/sai/hw_test/SaiSwitchEnsemble.cpp
  fboss/agent/hw/sai/hw_test/SaiLinkStateToggler.cpp
)

target_link_libraries(sai_switch_ensemble
  core
  setup_thrift
  sai_switch
  thrift_test_handler
  hw_switch_ensemble
  sai_platform
  sai_test_ctrl_cpp2
)

set_target_properties(sai_switch_ensemble PROPERTIES COMPILE_FLAGS
  "-DSAI_VER_MAJOR=${SAI_VER_MAJOR} \
  -DSAI_VER_MINOR=${SAI_VER_MINOR}  \
  -DSAI_VER_RELEASE=${SAI_VER_RELEASE}"
)

function(BUILD_SAI_TEST SAI_IMPL_NAME SAI_IMPL_ARG)

  message(STATUS "Building SAI_IMPL_NAME: ${SAI_IMPL_NAME} SAI_IMPL_ARG: ${SAI_IMPL_ARG}")

  add_executable(sai_test-${SAI_IMPL_NAME}-${SAI_VER_MAJOR}.${SAI_VER_MINOR}.${SAI_VER_RELEASE}
    fboss/agent/hw/sai/hw_test/HwTestAclUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestCoppUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestEcmpUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestMacUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestMplsUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestPacketTrapEntry.cpp
    fboss/agent/hw/sai/hw_test/HwTestQosUtils.cpp
    fboss/agent/hw/sai/hw_test/HwTestRouteUtils.cpp
    fboss/agent/hw/sai/hw_test/HwVlanUtils.cpp
    fboss/agent/hw/sai/hw_test/SaiNextHopGroupTest.cpp
    fboss/agent/hw/sai/hw_test/SaiPortUtils.cpp
  )

  target_link_libraries(sai_test-${SAI_IMPL_NAME}-${SAI_VER_MAJOR}.${SAI_VER_MINOR}.${SAI_VER_RELEASE}
    # --whole-archive is needed for gtest to find these tests
    -Wl,--whole-archive
    ${SAI_IMPL_ARG}
    sai_switch_ensemble
    hw_switch_test
    hw_test_main
    -Wl,--no-whole-archive
    ref_map
    ${GTEST}
    ${LIBGMOCK_LIBRARIES}
  )

  set_target_properties(sai_test-${SAI_IMPL_NAME}-${SAI_VER_MAJOR}.${SAI_VER_MINOR}.${SAI_VER_RELEASE}
      PROPERTIES COMPILE_FLAGS
      "-DSAI_VER_MAJOR=${SAI_VER_MAJOR} \
      -DSAI_VER_MINOR=${SAI_VER_MINOR}  \
      -DSAI_VER_RELEASE=${SAI_VER_RELEASE}"
    )

endfunction()

BUILD_SAI_TEST("fake" fake_sai)

# If libsai_impl is provided, build sai tests linking with it
find_library(SAI_IMPL sai_impl)
message(STATUS "SAI_IMPL: ${SAI_IMPL}")

if(SAI_IMPL)
  BUILD_SAI_TEST("sai_impl" ${SAI_IMPL})
  install(
    TARGETS
    sai_test-sai_impl-${SAI_VER_MAJOR}.${SAI_VER_MINOR}.${SAI_VER_RELEASE})
endif()
