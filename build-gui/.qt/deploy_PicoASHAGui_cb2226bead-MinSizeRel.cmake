include("C:/Cochlear/Pico-ASHA/build-gui/.qt/QtDeploySupport-MinSizeRel.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PicoASHAGui-plugins-MinSizeRel.cmake" OPTIONAL)
set(__QT_DEPLOY_I18N_CATALOGS "qtbase;qtserialport")

qt6_deploy_runtime_dependencies(
    EXECUTABLE "C:/Cochlear/Pico-ASHA/build-gui/MinSizeRel/PicoASHAGui.exe"
    GENERATE_QT_CONF
    NO_COMPILER_RUNTIME
;DEPLOY_TOOL_OPTIONS;--no-system-d3d-compiler;--no-opengl-sw;--no-opengl;--no-svg;--no-network)
