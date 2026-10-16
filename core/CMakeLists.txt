include("${PROJECT_SOURCE_DIR}/cmake/Eigen.cmake")

add_library(phonecast-core
    src/AppConfig.cpp
    src/AudioProtocol.cpp
    src/AudioPlayback.cpp
    src/ConsoleLogger.cpp
    src/DecoderRecoveryController.cpp
    src/GeneratedVideoSource.cpp
    src/StreamProtocol.cpp
    src/tracking/LineFitterRansac.cpp
    src/tracking/CornerIntersection.cpp
    src/tracking/PlanarPnpSolver.cpp
    src/tracking/InertialPoseFusion.cpp
)
target_include_directories(phonecast-core PUBLIC include)
target_compile_features(phonecast-core PUBLIC cxx_std_17)
phonecast_enable_warnings(phonecast-core)
target_link_libraries(phonecast-core PUBLIC phonecast_eigen)

