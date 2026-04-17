# Find the popt library
find_package(Popt REQUIRED)
set(LIBS ${LIBS} ${POPT_LIBRARIES})
include_directories(${POPT_INCLUDE_DIRS})
add_definitions(${POPT_DEFINITIONS})

# Find the GCrypt library
#find_package(GCrypt REQUIRED)
#set(LIBS ${LIBS} ${GCRYPT_LIBRARIES})
#include_directories(${GCRYPT_INCLUDE_DIRS})
#add_definitions(${GCRYPT_DEFINITIONS})

# Find jsoncpp library
#find_package(jsoncpp REQUIRED)
#get_target_property(JSONCPP_INC_PATH jsoncpp_lib INTERFACE_INCLUDE_DIRECTORIES)
#include_directories(${JSONCPP_INC_PATH})
#set(LIBS ${LIBS} jsoncpp_lib)
pkg_check_modules (JSONCPP REQUIRED jsoncpp)
include_directories(${JSONCPP_INCLUDE_DIRS})
set(LIBS ${LIBS} ${JSONCPP_LIBRARIES})

# Optional MQTT publishing — pass -DWITH_MQTT=OFF to build without
# libmosquitto. Call sites keep working through a no-op stub class
# defined in MqttPublisher.h.
option(WITH_MQTT "Enable MQTT publishing (requires libmosquitto)" ON)
if(WITH_MQTT)
  pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
  include_directories(${MOSQUITTO_INCLUDE_DIRS})
  set(LIBS ${LIBS} ${MOSQUITTO_LIBRARIES})
  message(STATUS "WITH_MQTT=ON: building with libmosquitto support")
else()
  message(STATUS "WITH_MQTT=OFF: building WITHOUT MQTT support; "
                 "[MQTT] config sections will fail to initialize at startup")
endif()

# Optional Redis-backed config store — pass -DWITH_REDIS=OFF to build
# without libhiredis. Call sites keep working through a no-op stub class
# defined in RedisStore.h.
option(WITH_REDIS "Enable Redis-backed config store (requires libhiredis)" ON)
if(WITH_REDIS)
  pkg_check_modules(HIREDIS REQUIRED hiredis)
  include_directories(${HIREDIS_INCLUDE_DIRS})
  set(LIBS ${LIBS} ${HIREDIS_LIBRARIES})
  message(STATUS "WITH_REDIS=ON: building with libhiredis support")
else()
  message(STATUS "WITH_REDIS=OFF: building WITHOUT Redis support; "
                 "[REDIS] config sections will cause startup to abort")
endif()

# Add project libraries
set(LIBS asynccpp asyncaudio asynccore svxmisc ${LIBS})

# Build the executable
set(REFLECTOR_SOURCES
  svxreflector.cpp Reflector.cpp ReflectorClient.cpp TGHandler.cpp TrunkLink.cpp
  TwinLink.cpp
  SatelliteLink.cpp SatelliteClient.cpp
)
if(WITH_MQTT)
  list(APPEND REFLECTOR_SOURCES MqttPublisher.cpp)
endif()
if(WITH_REDIS)
  list(APPEND REFLECTOR_SOURCES RedisStore.cpp RedisAsyncAdapter.cpp RedisLiveQueue.cpp)
endif()

add_executable(svxreflector ${REFLECTOR_SOURCES})
target_link_libraries(svxreflector ${LIBS})
if(WITH_MQTT)
  target_compile_definitions(svxreflector PRIVATE WITH_MQTT)
endif()
if(WITH_REDIS)
  target_compile_definitions(svxreflector PRIVATE WITH_REDIS)
endif()
set_target_properties(svxreflector PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${RUNTIME_OUTPUT_DIRECTORY}
)

# Generate config file with correct paths
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/svxreflector.conf.in
  ${CMAKE_CURRENT_BINARY_DIR}/svxreflector.conf
  @ONLY
  )
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ca-hook.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/ca-hook.py
  @ONLY
  )

# Install targets
install(TARGETS svxreflector DESTINATION ${BIN_INSTALL_DIR})
install_if_not_exists(${CMAKE_CURRENT_BINARY_DIR}/svxreflector.conf
  ${SVX_SYSCONF_INSTALL_DIR}
  )
install_if_not_exists(ca-hook.yaml ${SVX_SYSCONF_INSTALL_DIR})
install(PROGRAMS svxreflector-status
  DESTINATION ${BIN_INSTALL_DIR}
  )
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/ca-hook.py
  DESTINATION ${SVX_SHARE_INSTALL_DIR}
  )
