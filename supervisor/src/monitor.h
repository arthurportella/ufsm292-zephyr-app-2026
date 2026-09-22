/*
 * Detecção de falha de um subsistema (gateway ou servidor).
 *
 * Lógica pura, sem Zephyr: dá para testar no PC (ver tests/test_monitor.c).
 */

#ifndef MONITOR_H_
#define MONITOR_H_

#include <stdbool.h>
#include <stdint.h>

/* Eventos devolvidos por monitor_update() (podem vir combinados) */
#define MON_EV_UP     (1u << 0) /* respondeu pela 1a vez ou voltou de uma falha */
#define MON_EV_DOWN   (1u << 1) /* N timeouts consecutivos: marcado em falha */
#define MON_EV_RESET  (1u << 2) /* enviar POST /reset agora */
#define MON_EV_REBOOT (1u << 3) /* uptime_ms voltou para trás: a placa reiniciou */

struct monitor {
	/* configuração */
	int threshold;     /* N falhas consecutivas até marcar falha */
	uint32_t grace_ms; /* tempo sem contar falhas depois de um reset */

	/* estado */
	bool known;        /* já respondeu alguma vez */
	bool failed;       /* marcado em falha */
	int fails;         /* falhas consecutivas */
	int resets;        /* resets enviados */
	uint32_t uptime_ms;
	int64_t hold_until;
};

/*
 * Registra o resultado de uma consulta a /health.
 * ok: respondeu HTTP 200. uptime_ms: valor lido (ignorado se !ok).
 */
unsigned int monitor_update(struct monitor *m, bool ok, uint32_t uptime_ms, int64_t now_ms);

#endif
