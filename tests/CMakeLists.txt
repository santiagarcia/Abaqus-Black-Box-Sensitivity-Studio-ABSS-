find_package(doctest REQUIRED)

# One executable per module; every TEST_CASE is registered with CTest individually via labels per module.
function(abss_add_test_module name libs)
  file(GLOB _srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${name}/*.cpp)
  if(NOT _srcs)
    return()
  endif()
  add_executable(test_${name} ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${_srcs})
  target_link_libraries(test_${name} PRIVATE ${libs} doctest::doctest)
  target_compile_definitions(test_${name} PRIVATE
    ABSS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ABSS_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    ABSS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND test_${name})
  set_tests_properties(${name} PROPERTIES LABELS "${name}")
endfunction()

abss_add_test_module(core "abss_core")
abss_add_test_module(inp "abss_inp")
abss_add_test_module(linalg "abss_linalg")
abss_add_test_module(sensitivity "abss_sensitivity")
abss_add_test_module(oracle "abss_oracle")

# Tests that need a licensed Abaqus installation (label "abaqus"; excluded from default runs).
file(GLOB _abq CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/abaqus/*.cpp)
if(_abq)
  add_executable(test_abaqus ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${_abq})
  target_link_libraries(test_abaqus PRIVATE abss_sensitivity doctest::doctest)
  target_compile_definitions(test_abaqus PRIVATE ABSS_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    ABSS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME abaqus COMMAND test_abaqus)
  set_tests_properties(abaqus PROPERTIES LABELS "abaqus" TIMEOUT 7200)
endif()

if(ABSS_BUILD_GUI)
  file(GLOB _gui CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gui/*.cpp)
  if(_gui)
    add_executable(test_gui ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${_gui})
    target_link_libraries(test_gui PRIVATE abss_gui doctest::doctest)
    add_test(NAME gui COMMAND test_gui)
    set_tests_properties(gui PROPERTIES LABELS "gui" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
endif()
