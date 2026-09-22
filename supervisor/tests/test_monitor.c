/*
 * Teste da lógica de falha, roda no PC (sem Zephyr):
 *   gcc -I../src test_monitor.c ../src/monitor.c -o t && ./t
 */

#include <assert.h>
#include <stdio.h>

#include "monitor.h"

int main(void)
{
	struct monitor m = {.threshold = 3, .grace_ms = 10000};
	int64_t t = 0;

	/* primeira resposta: UP */
	assert(monitor_update(&m, true, 1000, t) == MON_EV_UP);
	assert(monitor_update(&m, true, 3000, t += 2000) == 0);

	/* 2 falhas não bastam; 3a marca falha e pede reset */
	assert(monitor_update(&m, false, 0, t += 2000) == 0);
	assert(monitor_update(&m, false, 0, t += 2000) == 0);
	assert(monitor_update(&m, false, 0, t += 2000) == (MON_EV_DOWN | MON_EV_RESET));
	assert(m.failed && m.resets == 1);

	/* durante o grace as falhas não contam */
	for (int i = 0; i < 4; i++) {
		assert(monitor_update(&m, false, 0, t += 2000) == 0);
	}

	/* passou o grace e segue morto: novo reset, sem novo DOWN */
	t += 2000;
	assert(monitor_update(&m, false, 0, t += 2000) == 0);
	assert(monitor_update(&m, false, 0, t += 2000) == 0);
	assert(monitor_update(&m, false, 0, t += 2000) == MON_EV_RESET);
	assert(m.resets == 2);

	/* voltou com uptime zerado: UP + REBOOT */
	assert(monitor_update(&m, true, 500, t += 2000) == (MON_EV_UP | MON_EV_REBOOT));
	assert(!m.failed && m.fails == 0);

	/* falha isolada zera ao responder de novo */
	assert(monitor_update(&m, false, 0, t += 2000) == 0);
	assert(monitor_update(&m, true, 5000, t += 2000) == 0);
	assert(m.fails == 0);

	/* reboot espontâneo (sem falha no meio) */
	assert(monitor_update(&m, true, 100, t += 2000) == MON_EV_REBOOT);

	puts("test_monitor: ok");
	return 0;
}
