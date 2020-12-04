# Builds Avro C library from source.
ExternalProject_Add(avro-c
  URL "https://github.com/apache/avro/archive/release-1.10.0.tar.gz"
  SOURCE_DIR ${CMAKE_BINARY_DIR}/avro-c/
  PATCH_COMMAND sed -i "s/find_package(Snappy)//" lang/c/CMakeLists.txt
  SOURCE_SUBDIR lang/c/
  CMAKE_ARGS
  -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/avro-c/install
  -DCMAKE_C_FLAGS=-fPIC
  -DCMAKE_CXX_FLAGS=-fPIC
  -DJANSSON_FOUND=Y -DJANSSON_INCLUDE_DIRS=${JANSSON_INCLUDE_DIR}
  -DJANSSON_LIBRARY_DIRS=${JANSSON_LIBRARY_DIR}
  -DJANSSON_LIBRARIES=${JANSSON_LIBRARIES}

  BINARY_DIR ${CMAKE_BINARY_DIR}/avro-c/
  INSTALL_DIR ${CMAKE_BINARY_DIR}/avro-c/install
  UPDATE_COMMAND ""
  LOG_DOWNLOAD 1
  LOG_UPDATE 1
  LOG_CONFIGURE 1
  LOG_BUILD 1
  LOG_INSTALL 1)

add_dependencies(avro-c jansson)
set(AVRO_FOUND TRUE CACHE INTERNAL "")
set(AVRO_STATIC_FOUND TRUE CACHE INTERNAL "")
set(AVRO_INCLUDE_DIR ${CMAKE_BINARY_DIR}/avro-c/install/include CACHE INTERNAL "")
set(AVRO_STATIC_LIBRARIES ${CMAKE_BINARY_DIR}/avro-c/src/libavro.a CACHE INTERNAL "")
set(AVRO_LIBRARIES ${AVRO_STATIC_LIBRARIES} CACHE INTERNAL "")
