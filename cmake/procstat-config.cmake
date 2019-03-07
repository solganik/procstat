include(FindPackageHandleStandardArgs)
include("$ENV{WORKSPACE_TOP}/common/cmake/lb.cmake")

lb_local_path(PROCSTAT_BUILD procstat procstat)

find_path(Procstat_INCLUDE_DIR
  "procstat.h"
  PATHS "$ENV{WORKSPACE_TOP}/procstat"
  PATH_SUFFIXES "src"
)

find_library(Procstat_LIBRARY NAMES libprocstat.a PATHS "${PROCSTAT_BUILD}/lib")

lb_version(Procstat_VERSION procstat procstat)

find_package_handle_standard_args(Procstat DEFAULT_MSG
  Procstat_INCLUDE_DIR Procstat_LIBRARY
)

# I'm not entirely sure if it's needed, but it sounds
# like a best practice. I'm not sure I understand the documentation:
# https://cmake.org/cmake/help/v3.12/manual/cmake-developer.7.html#modules
if(Procstat_FOUND)
  set(Procstat_LIBRARIES ${Procstat_LIBRARY})
  set(Procstat_INCLUDE_DIRS ${Procstat_INCLUDE_DIR})
  message("GOT ${Procstat_INCLUDE_DIR}")
endif()

if(Procstat_FOUND AND NOT TARGET Procstat::Procstat)
  add_library(Procstat::Procstat UNKNOWN IMPORTED)
  set_target_properties(Procstat::Procstat PROPERTIES
    IMPORTED_LOCATION "${Procstat_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Procstat_INCLUDE_DIR}"
  )
endif()
