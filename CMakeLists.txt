# Ponto de entrada CMake do módulo (ver zephyr/module.yml), NÃO uma aplicação:
# é o que põe drivers/ e lib/ no build. As aplicações ficam em app/, blinky/,
# gateway/ e supervisor/.

zephyr_syscall_include_directories(include)

zephyr_include_directories(include)

add_subdirectory(drivers)
add_subdirectory(lib)
