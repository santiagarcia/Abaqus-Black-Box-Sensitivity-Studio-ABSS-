# abss-viz: VTK scene for ABSS results (mixed-element grids, colormaps, ranges, picking, VTU export).
# The colormap / range / label / display-projection code is VTK-free so it can be unit tested headless.
find_package(VTK 9 REQUIRED COMPONENTS
  CommonCore CommonDataModel CommonColor
  RenderingCore RenderingOpenGL2 RenderingAnnotation RenderingFreeType
  InteractionStyle InteractionWidgets
  FiltersCore FiltersGeometry FiltersSources FiltersExtraction
  IOXML IOImage
  GUISupportQt)

file(GLOB _viz_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(abss_viz STATIC ${_viz_srcs})
target_link_libraries(abss_viz PUBLIC abss_core ${VTK_LIBRARIES})
vtk_module_autoinit(TARGETS abss_viz MODULES ${VTK_LIBRARIES})
