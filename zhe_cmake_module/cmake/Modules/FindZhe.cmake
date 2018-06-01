# Copyright 2018 ADLINK Technology Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

###############################################################################
#
# CMake module for finding ADLINK Zhe.
#
# Output variables:
#
# - Zhe_FOUND: flag indicating if the package was found
# - Zhe_INCLUDE_DIR: Paths to the header files
#
# Example usage:
#
#   find_package(zhe_cmake_module REQUIRED)
#   find_package(Zhe MODULE)
#   # use Zhe_* variables
#
###############################################################################

# lint_cmake: -convention/filename, -package/stdargs

set(Zhe_FOUND FALSE)

#find_path(Zhe_INCLUDE_DIR
#  NAMES zhe/)

find_package(fastcdr REQUIRED CONFIG)
find_package(zhe REQUIRED CONFIG)

string(REGEX MATCH "^[0-9]+\\.[0-9]+" fastcdr_MAJOR_MINOR_VERSION "${fastcdr_VERSION}")
#string(REGEX MATCH "^[0-9]+\\.[0-9]+" zhe_MAJOR_MINOR_VERSION "${zhe_VERSION}")

find_library(FastCDR_LIBRARY_RELEASE
  NAMES fastcdr-${fastcdr_MAJOR_MINOR_VERSION} fastcdr)

find_library(FastCDR_LIBRARY_DEBUG
  NAMES fastcdrd-${fastcdr_MAJOR_MINOR_VERSION})

if(FastCDR_LIBRARY_RELEASE AND FastCDR_LIBRARY_DEBUG)
  set(FastCDR_LIBRARIES
    optimized ${FastCDR_LIBRARY_RELEASE}
    debug ${FastCDR_LIBRARY_DEBUG}
  )
elseif(FastCDR_LIBRARY_RELEASE)
  set(FastCDR_LIBRARIES
    ${FastCDR_LIBRARY_RELEASE}
  )
elseif(FastCDR_LIBRARY_DEBUG)
  set(FastCDR_LIBRARIES
    ${FastCDR_LIBRARY_DEBUG}
  )
else()
  set(FastCDR_LIBRARIES "")
endif()

#find_library(Zhe_LIBRARY_RELEASE
#  NAMES zhe-${zhe_MAJOR_MINOR_VERSION} zhe)
find_library(Zhe_LIBRARY_RELEASE
  NAMES zhe zhe)
#find_library(Zhe_LIBRARY_DEBUG
#  NAMES zhed-${zhe_MAJOR_MINOR_VERSION})

if(Zhe_LIBRARY_RELEASE AND Zhe_LIBRARY_DEBUG)
  set(Zhe_LIBRARIES
    optimized ${Zhe_LIBRARY_RELEASE}
    debug ${Zhe_LIBRARY_DEBUG}
    ${FastCDR_LIBRARIES}
  )
elseif(Zhe_LIBRARY_RELEASE)
  set(Zhe_LIBRARIES
    ${Zhe_LIBRARY_RELEASE}
    ${FastCDR_LIBRARIES}
  )
elseif(Zhe_LIBRARY_DEBUG)
  set(Zhe_LIBRARIES
    ${Zhe_LIBRARY_DEBUG}
    ${FastCDR_LIBRARIES}
  )
else()
  set(Zhe_LIBRARIES "")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zhe
  FOUND_VAR Zhe_FOUND
  REQUIRED_VARS
    Zhe_INCLUDE_DIR
    Zhe_LIBRARIES
)
