# Supervisor (grupo D)

SAM D21 Xplained Pro + OLED1 Xplained Pro + módulo Ethernet SPI.

A cada `CONFIG_SUPERVISOR_POLL_MS` (2 s): `GET /health` no gateway e no servidor,
`GET /data` no servidor. Depois de N timeouts/erros consecutivos (N = 3), marca
falha, acende o LED e manda `POST /reset`. Depois do reset espera
`CONFIG_SUPERVISOR_RESET_GRACE_MS` (10 s) antes de voltar a contar falhas.
Se o `uptime_ms` voltar para trás, registra "reiniciou".

"Vivo" = respondeu HTTP 200. O `status` do corpo (ex.: `"degraded"` do servidor
quando o gateway caiu) não dispara reset.

## Telas (OLED 128x32, 4 linhas)

| Botão | Ação |
|---|---|
| 1 | tela anterior |
| 2 | próxima tela |
| 3 | rola (próximo nó / eventos mais antigos) |

1. **Status**: estado de GW e SRV (`OK`, `?1` = falhou mas < N, `FALHA`), uptime, nós online, falhas e resets
2. **Nós**: luz, temperatura, aceleração, `last_seen_ms`, flags (de `GET /data`)
3. **Eventos**: `online`, `FALHA`, `reset ok/s/resp`, `reiniciou`, com o tempo em s

LED 1 = gateway em falha, LED 2 = servidor em falha, LED 3 = pisca a cada ciclo.

## Compilar e gravar

```bash
# só OLED1 (sem Ethernet): testa display, botões e LEDs; tudo fica em FALHA
west build -b samd21_xpro supervisor

# com Ethernet (exemplo W5500 no EXT1; trocar quando soubermos o módulo)
west build -b samd21_xpro supervisor -- -DEXTRA_DTC_OVERLAY_FILE=eth_w5500.overlay

west flash
```

IPs e portas em [prj.conf](prj.conf) e IP da placa em
[boards/samd21_xpro.conf](boards/samd21_xpro.conf). Com o simulador, gateway e
servidor são o IP do PC, portas 5500 e 5501.

Pinos do OLED1 (EXT3) em [boards/samd21_xpro.overlay](boards/samd21_xpro.overlay).

## Testar sem placa

Lógica de falha (só gcc, sem Zephyr):

```bash
cd supervisor/tests
gcc -I../src test_monitor.c ../src/monitor.c -o t && ./t
```

O firmware inteiro no PC contra o simulador (native_sim usa os sockets do host):

```bash
apt-get install -y gcc make        # a imagem do devcontainer não traz gcc do host
cd sim && docker compose up -d && cd ..
west build -b native_sim/native/64 supervisor -d build-nsim
./build-nsim/zephyr/zephyr.exe
```

Em outro terminal, provocar falhas (ver [sim/README.md](../sim/README.md)):

```bash
curl -X POST "http://localhost:5500/_sim/fault?mode=hang&seconds=20"  # GW: FALHA, reset, volta online
curl -X POST http://localhost:5500/reset                               # GW: "reiniciou", sem FALHA
curl -X POST "http://localhost:5501/_sim/fault?mode=close&seconds=10" # SRV: FALHA, reset, volta
```

## Pendências

- **Módulo Ethernet**: descobrir qual é. Zephyr tem driver para W5500, ENC28J60 e
  ENC424J600. O ETHERNET1 Xplained Pro (KSZ8851SNL) **não tem driver** no Zephyr.
- **Conector do OLED1**: o overlay assume EXT3 (pinos conferidos pelo esquema da
  SAMD21 Xpro; conferir a pinagem do OLED1 no user guide dele).
- **RAM**: 85% usada com W5500 (32 KB no total). Na placa, habilitar
  `CONFIG_THREAD_ANALYZER=y` uma vez para conferir as pilhas.
- **Contrato**: confirmar com os grupos B e C porta e formato do JSON reais.
