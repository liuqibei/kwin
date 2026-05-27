# Debian qt6-base-private-dev ships headers without Qt6CorePrivate CMake configs.
find_package(Qt6CorePrivate CONFIG QUIET)
find_package(Qt6GuiPrivate CONFIG QUIET)

if(NOT TARGET Qt6::CorePrivate)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)
    get_target_property(_qt6_core_include_dirs Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
    file(GLOB _qt6_core_private_dir "${_qt6_core_include_dirs}/../../../*/qt6/QtCore/*/QtCore/private")
    add_library(Qt6::CorePrivate INTERFACE IMPORTED GLOBAL)
    target_include_directories(Qt6::CorePrivate SYSTEM INTERFACE
        "${_qt6_core_include_dirs}"
        "${_qt6_core_include_dirs}/QtCore"
        ${_qt6_core_private_dir}
    )
    target_link_libraries(Qt6::CorePrivate INTERFACE Qt6::Core)
endif()

if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Gui)
    get_target_property(_qt6_gui_include_dirs Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
    file(GLOB _qt6_gui_private_dir "${_qt6_gui_include_dirs}/../../../*/qt6/QtGui/*/QtGui/private")
    add_library(Qt6::GuiPrivate INTERFACE IMPORTED GLOBAL)
    target_include_directories(Qt6::GuiPrivate SYSTEM INTERFACE
        "${_qt6_gui_include_dirs}"
        "${_qt6_gui_include_dirs}/QtGui"
        ${_qt6_gui_private_dir}
    )
    target_link_libraries(Qt6::GuiPrivate INTERFACE Qt6::Gui)
endif()

unset(_qt6_core_include_dirs)
unset(_qt6_gui_include_dirs)
unset(_qt6_core_private_dir)
unset(_qt6_gui_private_dir)
