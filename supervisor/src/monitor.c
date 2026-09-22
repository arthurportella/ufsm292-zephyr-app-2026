#include "monitor.h"

unsigned int monitor_update(struct monitor *m, bool ok, uint32_t uptime_ms, int64_t now_ms)
{
	unsigned int ev = 0;

	if (ok) {
		if (m->known && uptime_ms < m->uptime_ms) {
			ev |= MON_EV_REBOOT;
		}
		if (!m->known || m->failed) {
			ev |= MON_EV_UP;
		}
		m->known = true;
		m->failed = false;
		m->fails = 0;
		m->uptime_ms = uptime_ms;
		return ev;
	}

	/* Placa reiniciando por causa do nosso reset: não conta como falha */
	if (now_ms < m->hold_until) {
		return 0;
	}

	if (++m->fails < m->threshold) {
		return 0;
	}

	/* ponytail: reenvia reset a cada N falhas + grace, sem backoff; adicionar
	 * backoff se resets repetidos atrapalharem a placa na integração.
	 */
	m->fails = 0;
	m->resets++;
	m->hold_until = now_ms + m->grace_ms;
	ev = MON_EV_RESET;
	if (!m->failed) {
		m->failed = true;
		ev |= MON_EV_DOWN;
	}
	return ev;
}
