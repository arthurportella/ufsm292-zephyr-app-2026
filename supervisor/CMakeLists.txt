cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(supervisor LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/monitor.c
  src/http.c
  src/ui.c
)
target_sources_ifdef(CONFIG_CHARACTER_FRAMEBUFFER app PRIVATE src/font6x8.c)

# Linhas do display são cortadas em 21 colunas de propósito
target_compile_options(app PRIVATE -Wno-format-truncation)
