# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
target_compile_options(${BUILD_UNIT_0_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>: ${CUBE_CMAKE_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>: ${CUBE_CMAKE_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:ASM>: ${CUBE_CMAKE_ASM_FLAGS}>
)

# Linker options
target_link_options(${BUILD_UNIT_0_NAME} PRIVATE ${CUBE_CMAKE_EXE_LINKER_FLAGS})

# Add sources to executable/library
target_sources(${BUILD_UNIT_0_NAME} PRIVATE
    "Common/System/system_stm32wlxx.c"
    "Middlewares/FatFs/ff.c"
    "Core/WL55JC1/Src/adc.c"
    "Core/WL55JC1/Src/battery_adc.c"
    "Core/WL55JC1/Src/battery_flow.c"
    "Core/WL55JC1/Src/envnode_config.c"
    "Core/WL55JC1/Src/envnode_identity.c"
    "Core/WL55JC1/Src/envnode_keystore.c"
    "Core/WL55JC1/Src/envnode_diskio.c"
    "Core/WL55JC1/Src/envnode_log.c"
    "Core/WL55JC1/Src/envnode_power.c"
    "Core/WL55JC1/Src/envnode_sdlog.c"
    "Core/WL55JC1/Src/envnode_sensorset.c"
    "Core/WL55JC1/Src/gpio.c"
    "Core/WL55JC1/Src/i2c.c"
    "Core/WL55JC1/Src/ina219.c"
    "Core/WL55JC1/Src/main.c"
    "Core/WL55JC1/Src/rtc.c"
    "Core/WL55JC1/Src/sd_spi.c"
    "Core/WL55JC1/Src/spi.c"
    "Core/WL55JC1/Src/sensors/analog_sensors.c"
    "Core/WL55JC1/Src/sensors/bme280.c"
    "Core/WL55JC1/Src/sensors/envnode_payload.c"
    "Core/WL55JC1/Src/sensors/envnode_sensors.c"
    "Core/WL55JC1/Src/sensors/max31865.c"
    "Core/WL55JC1/Src/sensors/pulse_counter.c"
    "Core/WL55JC1/Src/stm32wlxx_hal_msp.c"
    "Core/WL55JC1/Src/stm32wlxx_it.c"
    "Core/WL55JC1/Src/usart.c"
    "Core/WL55JC1/Startup/startup_stm32wl55jcix.s"
    "Drivers/BSP/STM32WLxx_Nucleo/stm32wlxx_nucleo.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_adc.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_adc_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_cortex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_dma.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_dma_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_exti.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_flash.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_flash_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_gpio.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_i2c.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_i2c_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_pwr.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_pwr_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rcc.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rcc_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rtc.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_rtc_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_spi.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_spi_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_uart.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_uart_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_usart.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_hal_usart_ex.c"
    "Drivers/STM32WLxx_HAL_Driver/Src/stm32wlxx_ll_adc.c"
)

target_include_directories(${BUILD_UNIT_0_NAME} PRIVATE
    "Core/WL55JC1/Inc"
    "Middlewares/FatFs"
    "Drivers/STM32WLxx_HAL_Driver/Inc"
    "Drivers/STM32WLxx_HAL_Driver/Inc/Legacy"
    "Drivers/BSP/STM32WLxx_Nucleo"
    "Drivers/CMSIS/Device/ST/STM32WLxx/Include"
    "Drivers/CMSIS/Include"
)

configure_file("${CMAKE_SOURCE_DIR}/STM32WL55JCIX_FLASH.ld" "${CMAKE_BINARY_DIR}" COPYONLY)

set_target_properties(${BUILD_UNIT_0_NAME} PROPERTIES LINK_DEPENDS "STM32WL55JCIX_FLASH.ld")

