cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{DOLCESDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DOLCESDK}/share/dolce.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define DOLCESDK to point to your SDK path!")
  endif()
endif()

project(EmergencyMount)
include("$ENV{DOLCESDK}/share/dolce.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

link_directories(
	${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(${PROJECT_NAME}
	main.c
	main.h
	rikka.c
	rikka.h
	blit/blit.c
	blit/font.c
)

target_link_libraries(${PROJECT_NAME}
k
		SceCpuForDriver_stub 
		SceSblAIMgrForDriver_stub 
		SceUdcdForDriver_stub
		SceAppMgrForDriver_stub
		SceKernelSuspendForDriver_stub
        SceSysmemForDriver_stub
		SceProcessmgrForKernel_stub
        SceThreadmgrForDriver_stub

        SceIofilemgrForDriver_stub
        SceUsbstorVStorDriver_stub
        SceCpuForDriver_stub
        taihenForKernel_stub
		SceDebugForDriver_stub
		SceDisplayForDriver_stub
		SceModulemgrForKernel_stub
		SceCtrlForDriver_stub
		SceDebugForDriver_stub
		ScePowerForDriver_stub
		SceSysrootForKernel_stub
		SceProcessmgrForDriver_stub
		taihenModuleUtils_stub
		SceSysconForDriver_stub
        -nostdlib
        gcc
)
dolce_create_self(${PROJECT_NAME}.skprx ${PROJECT_NAME}
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/EmergencyMount.yml
)

