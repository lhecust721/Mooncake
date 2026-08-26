set(WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}")

if(USE_HIP)
  file(GLOB EXAMPLE_SOURCES "*.cpp")
  hipify_files(EXAMPLE_SOURCES)

  file(RELATIVE_PATH EXAMPLE_REL_PATH "${CMAKE_SOURCE_DIR}"
       "${CMAKE_CURRENT_SOURCE_DIR}")
  set(WORKSPACE "${CMAKE_BINARY_DIR}/${EXAMPLE_REL_PATH}")
endif()

add_executable(transfer_engine_bench ${WORKSPACE}/transfer_engine_bench.cpp)
target_link_libraries(transfer_engine_bench PUBLIC transfer_engine)
if(USE_TENT)
  target_link_libraries(transfer_engine_bench PUBLIC tent_link_group)
  target_compile_definitions(transfer_engine_bench PRIVATE USE_TENT)
endif()

add_executable(transfer_engine_validator
               ${WORKSPACE}/transfer_engine_validator.cpp)
target_link_libraries(transfer_engine_validator PUBLIC transfer_engine)

add_executable(transfer_engine_bench_with_notify
               ${WORKSPACE}/transfer_engine_bench_with_notify.cpp)
target_link_libraries(transfer_engine_bench_with_notify PUBLIC transfer_engine)

if(USE_EFA)
  add_executable(efa_first_submit_probe
                 ${WORKSPACE}/efa_first_submit_probe.cpp)
  target_link_libraries(efa_first_submit_probe PUBLIC transfer_engine)
endif()

add_executable(memory_pool ${WORKSPACE}/memory_pool.cpp)
target_link_libraries(memory_pool PUBLIC transfer_engine)

if(USE_ASCEND)
  add_executable(transfer_engine_ascend_one_sided
                 ${WORKSPACE}/transfer_engine_ascend_one_sided.cpp)
  target_link_libraries(transfer_engine_ascend_one_sided PUBLIC transfer_engine)

  add_executable(transfer_engine_ascend_perf
                 ${WORKSPACE}/transfer_engine_ascend_perf.cpp)
  target_link_libraries(transfer_engine_ascend_perf PUBLIC transfer_engine)
endif()

if(USE_ASCEND_DIRECT)
  add_executable(transfer_engine_ascend_direct_perf
                 ${WORKSPACE}/transfer_engine_ascend_direct_perf.cpp)
  target_link_libraries(transfer_engine_ascend_direct_perf
                        PUBLIC ascendcl transfer_engine)

  add_executable(ssd_direct_hbm_probe
                 ${WORKSPACE}/ssd_direct_hbm_probe.cpp)
  target_link_libraries(ssd_direct_hbm_probe PUBLIC ascendcl)

  add_executable(hbm_p2p_mapping_probe
                 ${WORKSPACE}/hbm_p2p_mapping_probe.cpp)
  target_include_directories(hbm_p2p_mapping_probe
                             PRIVATE ${WORKSPACE}/hbm_p2p_probe)
  target_link_libraries(hbm_p2p_mapping_probe PUBLIC ascendcl)

  add_executable(nvme_hbm_l2_read_probe
                 ${WORKSPACE}/nvme_hbm_l2_read_probe.cpp)
  target_include_directories(nvme_hbm_l2_read_probe
                             PRIVATE ${WORKSPACE}/hbm_p2p_probe)
  target_link_libraries(nvme_hbm_l2_read_probe PUBLIC ascendcl)

  find_library(LIBURING_LIBRARY uring)
  find_path(LIBURING_INCLUDE_DIR liburing.h)
  if(LIBURING_LIBRARY AND LIBURING_INCLUDE_DIR)
    target_compile_definitions(ssd_direct_hbm_probe
                               PRIVATE MOONCAKE_DIRECT_PROBE_HAS_URING)
    target_include_directories(ssd_direct_hbm_probe
                               PRIVATE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(ssd_direct_hbm_probe PUBLIC ${LIBURING_LIBRARY})
  endif()
endif()

if(USE_ASCEND_HETEROGENEOUS)
  add_executable(
    transfer_engine_heterogeneous_ascend_perf_initiator
    ${WORKSPACE}/transfer_engine_heterogeneous_ascend_perf_initiator.cpp)
  target_link_libraries(transfer_engine_heterogeneous_ascend_perf_initiator
                        PUBLIC transfer_engine)
endif()

if(USE_UBSHMEM)
  target_link_libraries(transfer_engine_bench PUBLIC transfer_engine)
endif()
