# abss-gui: Qt 6 widgets (main window, workflow pages, explore view, history chart) and the GUI controller.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent OpenGLWidgets Charts)

file(GLOB _gui_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB _gui_hdrs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/include/abss/gui/*.hpp)
add_library(abss_gui STATIC ${_gui_srcs} ${_gui_hdrs})
set_target_properties(abss_gui PROPERTIES AUTOMOC ON)
target_link_libraries(abss_gui PUBLIC abss_viz abss_sensitivity abss_inp
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::OpenGLWidgets Qt6::Charts)
find_package(VTK 9 REQUIRED COMPONENTS RenderingOpenGL2 RenderingFreeType InteractionStyle InteractionWidgets
  RenderingAnnotation GUISupportQt)
vtk_module_autoinit(TARGETS abss_gui MODULES ${VTK_LIBRARIES})
