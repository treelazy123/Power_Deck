set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings
set(TOOLCHAIN_PREFIX                arm-none-eabi-)
set(STM32_GNU_TOOLCHAIN_ROOT
    "D:/STM32Cube/bundles/gnu-tools-for-stm32/14.3.1+st.2"
    CACHE PATH "Path to the GNU Arm Embedded toolchain")
set(TOOLCHAIN_BIN_DIR              "${STM32_GNU_TOOLCHAIN_ROOT}/bin")

if(NOT EXISTS "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc.exe")
    message(FATAL_ERROR
        "GNU Arm toolchain was not found at ${STM32_GNU_TOOLCHAIN_ROOT}")
endif()

set(CMAKE_C_COMPILER                "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc.exe")
set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})
set(CMAKE_CXX_COMPILER              "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}g++.exe")
set(CMAKE_LINKER                    "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}g++.exe")
set(CMAKE_OBJCOPY                   "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objcopy.exe")
set(CMAKE_SIZE                      "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}size.exe")

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU specific flags
set(TARGET_FLAGS "-mcpu=cortex-m3 ")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections -fstack-usage")

# The cyclomatic-complexity parameter must be defined for the Cyclomatic complexity feature in STM32CubeIDE to work.
# However, most GCC toolchains do not support this option, which causes a compilation error; for this reason, the feature is disabled by default.
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcyclomatic-complexity")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32F103xx_FLASH.ld\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(TOOLCHAIN_LINK_LIBRARIES "m")
