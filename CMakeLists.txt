cmake_minimum_required(VERSION 3.10)
project(xgc2_state_machine VERSION 0.1.3 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(XGC2_STATE_MACHINE_BUILD_TESTING "Build xgc2_state_machine tests" ON)

add_library(xgc2_state_machine SHARED
  src/state_machine.cpp
)
add_library(xgc2_state_machine::state_machine ALIAS xgc2_state_machine)

set_target_properties(xgc2_state_machine PROPERTIES
  OUTPUT_NAME xgc2_state_machine
  EXPORT_NAME state_machine
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)

target_compile_features(xgc2_state_machine PUBLIC cxx_std_17)
target_include_directories(xgc2_state_machine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(TARGETS xgc2_state_machine
  EXPORT xgc2_state_machineTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY docs/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/libxgc2-state-machine-dev/docs
)

configure_package_config_file(
  cmake/xgc2_state_machineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine
)

install(EXPORT xgc2_state_machineTargets
  NAMESPACE xgc2_state_machine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine
)

if(XGC2_STATE_MACHINE_BUILD_TESTING)
  include(CTest)
  if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    if(TARGET GTest::gtest)
      set(_xgc2_state_machine_gtest GTest::gtest)
    else()
      set(_xgc2_state_machine_gtest GTest::GTest)
    endif()
    if(TARGET GTest::gtest_main)
      set(_xgc2_state_machine_gtest_main GTest::gtest_main)
    else()
      set(_xgc2_state_machine_gtest_main GTest::Main)
    endif()
    add_executable(state_machine_runtime_test
      test/state_machine_runtime_test.cpp
    )
    target_link_libraries(state_machine_runtime_test
      PRIVATE
        xgc2_state_machine::state_machine
        ${_xgc2_state_machine_gtest}
        ${_xgc2_state_machine_gtest_main}
        pthread
    )
    add_test(NAME state_machine_runtime_test COMMAND state_machine_runtime_test)

    find_package(Threads REQUIRED)
    add_executable(async_task_executor_test test/async_task_executor_test.cpp)
    target_link_libraries(async_task_executor_test
      PRIVATE xgc2_state_machine::state_machine Threads::Threads
    )
    foreach(_case self_stop external_worker_stop discard_cleanup destructor_cleanup
                  restart fifo_exceptions concurrent_lifecycle)
      add_test(NAME async_task_executor_${_case}
        COMMAND async_task_executor_test ${_case})
      set_tests_properties(async_task_executor_${_case} PROPERTIES TIMEOUT 15)
    endforeach()
  endif()
endif()
