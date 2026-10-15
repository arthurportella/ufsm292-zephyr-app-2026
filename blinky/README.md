# Blinky (SAM D21 Xplained Pro)

Pisca o LED0 da placa (PB30). Validada em hardware.

```bash
west build -b samd21_xpro blinky
west flash
```

Antes ficava na raiz do repositório. Foi movida para cá porque o
`CMakeLists.txt` da raiz é o ponto de entrada do módulo Zephyr (ver
[zephyr/module.yml](../zephyr/module.yml)): enquanto ele era o CMakeLists desta
aplicação, `drivers/` e `lib/` não entravam em build nenhum.
