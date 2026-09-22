# Simulador do Gateway e do Servidor — Projeto Final

Mock em Python (só stdlib) do **Gateway (grupo B)** e do **Servidor (grupo C)**,
para o **grupo D (Supervisor)** desenvolver sem depender da entrega dos colegas.

| Subsistema | Porta | Rotas (contrato do PDF) |
|---|---|---|
| Gateway  | **5500** | `GET /sensors`, `GET /health`, `POST /reset` |
| Servidor | **5501** | `GET /health`, `GET /data`, `GET /log?node=X&n=N`, `POST /reset` |

São dois sockets porque no sistema real são duas placas distintas, e as duas
expõem `/health` e `/reset`. Assim as rotas ficam idênticas ao contrato.

## Subir

```bash
docker compose up --build -d
docker compose logs -f        # mostra as requisições chegando
```

Teste rápido:

```bash
curl http://localhost:5500/sensors
curl http://localhost:5500/health
curl http://localhost:5501/health
curl "http://localhost:5501/log?node=1&n=5"
curl -X POST http://localhost:5500/reset
```

Para a placa SAMD21 alcançar o simulador, use o **IP do PC na rede do
laboratório** (`ip addr` / `ipconfig`), não `localhost`:
`http://192.168.x.y:5500/health`.

## O que ele simula

- 3 nós sensores transmitindo a cada 500 ms: luz (0–1023), temperatura ~24 °C,
  aceleração com gravidade em Z, `seq`, `uptime_ms` e `flags` (bit 0 = bateria
  baixa, bit 1 = log local ativo).
- O servidor consulta `GET /sensors` do gateway a cada 1 s, guarda o histórico e
  grava CSV em `./sd/log.csv` (o "cartão SD").
- `POST /reset` responde `{"status":"resetting"}` e **derruba o subsistema por
  3 s**, voltando com `uptime_ms` zerado e `reset_count` incrementado — é o ciclo
  que o supervisor precisa detectar.

## Injeção de falhas (fora do contrato, prefixo `/_sim/`)

Para exercitar a contagem de N timeouts consecutivos e a recuperação:

```bash
# gateway trava: as requisições ficam penduradas -> timeout no supervisor
curl -X POST "http://localhost:5500/_sim/fault?mode=hang&seconds=30"

# servidor responde HTTP 500
curl -X POST "http://localhost:5501/_sim/fault?mode=error&seconds=15"

# conexão recusada/derrubada (cabo solto, placa travada)
curl -X POST "http://localhost:5500/_sim/fault?mode=close&seconds=15"

# cancelar antes da hora
curl -X POST "http://localhost:5500/_sim/fault?mode=none"

# tirar um nó sensor do ar (last_seen_ms cresce, nodes_online cai)
curl -X POST "http://localhost:5500/_sim/node?id=2&state=offline"
curl -X POST "http://localhost:5500/_sim/node?id=2&state=online"

# estado atual do simulador
curl http://localhost:5500/_sim/state
```

## Ajustes

Variáveis de ambiente no `docker-compose.yml`: `NODE_IDS`, `RADIO_PERIOD_S`,
`SERVER_POLL_S`, `RESET_DOWNTIME_S`, `NODE_ONLINE_MS`, `CSV_PATH`,
`GATEWAY_PORT`, `SERVER_PORT`.

## Nota sobre o JSON

O formato segue o exemplo do PDF e acrescenta `seq`, `uptime_ms` e `flags`, que
existem no pacote de rádio mas ficaram de fora do exemplo. Vale confirmar com os
grupos B e C se eles vão repassar esses campos — e, em geral, **comparar este
mock com a implementação real assim que ela existir**, porque o contrato válido
é o que eles implementarem.
