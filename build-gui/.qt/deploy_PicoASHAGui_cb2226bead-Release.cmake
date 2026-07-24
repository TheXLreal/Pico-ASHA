include("C:/Cochlear/Pico-ASHA/build-gui/.qt/QtDeploySupport-Release.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PicoASHAGui-plugins-Release.cmake" OPTIONAL)
set(__QT_DEPLOY_I18N_CATALOGS "qtbase;qtserialport")

qt6_deploy_runtime_dependencies(
    EXECUTABLE "C:/Cochlear/Pico-ASHA/build-gui/Release/PicoASHAGui.exe"
    GENERATE_QT_CONF
    NO_COMPILER_RUNTIME
;DEPLOY_TOOL_OPTIONS;--no-system-d3d-compiler;--no-opengl-sw;--no-opengl;--no-svg;--no-network)
