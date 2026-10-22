# abss-core: mesh, parameters, results, persistence (no Qt/VTK dependency)
add_library(abss_core STATIC
  core/mesh.cpp
  core/parameter.cpp
  core/results.cpp
  core/project_io.cpp
  core/profile.cpp
)
target_link_libraries(abss_core PUBLIC nlohmann_json::nlohmann_json)

# abss-inp: structure-preserving INP parser/editor + parameter discovery
add_library(abss_inp STATIC
  inp/deck.cpp
  inp/discovery.cpp
  inp/patch.cpp
)
target_link_libraries(abss_inp PUBLIC abss_core)

# abss-linalg: CSR, factorization reuse, Kokkos kernels
add_library(abss_linalg STATIC
  linalg/sparse.cpp
  linalg/parallel.cpp
)
target_link_libraries(abss_linalg PUBLIC Eigen3::Eigen)
if(ABSS_HAVE_KOKKOS)
  target_link_libraries(abss_linalg PUBLIC Kokkos::kokkos)
  target_compile_definitions(abss_linalg PUBLIC ABSS_HAVE_KOKKOS=1)
endif()
if(ABSS_HAVE_SUITESPARSE)
  target_link_libraries(abss_linalg PRIVATE SuiteSparse::UMFPACK)
  target_compile_definitions(abss_linalg PRIVATE ABSS_HAVE_SUITESPARSE=1)
endif()

# abss-oracle: synthetic (demo) and Abaqus residual/tangent oracles
add_library(abss_oracle STATIC
  oracle/dofmap.cpp
  oracle/wave.cpp
  oracle/abaqus_replica.cpp
  oracle/synthetic_fe.cpp
  oracle/synthetic_oracle.cpp
  oracle/abaqus_env.cpp
  oracle/abaqus_process.cpp
  oracle/abaqus_files.cpp
  oracle/abaqus_loads.cpp
  oracle/abaqus_usersub.cpp
  oracle/abaqus_matbridge.cpp
  oracle/abaqus_oracle.cpp
)
target_link_libraries(abss_oracle PUBLIC abss_core abss_inp abss_linalg)

# abss-sensitivity: history-consistent multi-order sensitivity engine + validation
add_library(abss_sensitivity STATIC
  sensitivity/stencil.cpp
  sensitivity/engine.cpp
  sensitivity/validation.cpp
)
target_link_libraries(abss_sensitivity PUBLIC abss_oracle)

if(ABSS_BUILD_GUI)
  add_subdirectory(viz)
  add_subdirectory(gui)
endif()


