# NB: the desktop files are installed for the benefit of KImageIO in KDELibs4Support.

##################################

function(kimageformats_add_plugin plugin)
    set(options)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(KIF_ADD_PLUGIN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT KIF_ADD_PLUGIN_SOURCES)
        message(FATAL_ERROR "kimageformats_add_plugin called without SOURCES parameter")
    endif()

    add_library(${plugin} MODULE ${KIF_ADD_PLUGIN_SOURCES})
    set_target_properties(${plugin} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/imageformats")
    target_link_libraries(${plugin} Qt${QT_MAJOR_VERSION}::Gui)
    install(TARGETS ${plugin} DESTINATION ${KDE_INSTALL_QTPLUGINDIR}/imageformats)
endfunction()

##################################

if (LibJXL_FOUND AND LibJXLThreads_FOUND)
    kimageformats_add_plugin("libqjpegxl${QT_MAJOR_VERSION}" SOURCES "main.cpp" "qjpegxlhandler.cpp")
    target_link_libraries("libqjpegxl${QT_MAJOR_VERSION}" PkgConfig::LibJXL PkgConfig::LibJXLThreads)
    #install(FILES jxl.desktop DESTINATION ${KDE_INSTALL_KSERVICESDIR}/qimageioplugins/)
endif()

##################################
