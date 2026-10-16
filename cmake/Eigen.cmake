include_guard(GLOBAL)
include(FetchContent)

# Eigen 3.4.0 (header-only linear algebra: vectors, matrices, SVD, Jacobi, quaternions)
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3147391d946bb4b6c68edd901f2add6ac1f31f8c # 3.4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _phonecast_no_automatic_add_subdirectory
)
FetchContent_MakeAvailable(eigen)

if(NOT TARGET phonecast_eigen)
    add_library(phonecast_eigen INTERFACE)
    target_include_directories(phonecast_eigen SYSTEM INTERFACE
        "${eigen_SOURCE_DIR}"
    )
endif()
