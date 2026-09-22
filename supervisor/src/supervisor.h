/* Estado compartilhado entre a thread de consulta (main.c) e a UI (ui.c) */

#ifndef SUPERVISOR_H_
#define SUPERVISOR_H_

#include <zephyr/kernel.h>

#include "monitor.h"

#define MAX_NODES  8
#define MAX_EVENTS 16

struct node_view {
	int id;
	int light;
	int flags;
	int age_ms;
	char temp[8];
	char accel[3][8];
};

struct event {
	uint32_t t_s;
	char text[18];
};

struct sup_state {
	struct monitor gw;
	struct monitor srv;
	int nodes_online;
	struct node_view nodes[MAX_NODES];
	int n_nodes;
	struct event events[MAX_EVENTS]; /* anel: evento i em events[i % MAX_EVENTS] */
	int n_events;                    /* total já registrado */
	uint32_t cycles;                 /* ciclos de consulta (LED de heartbeat) */
};

extern struct sup_state sup;
extern struct k_mutex sup_lock; /* protege sup */

/* Pede para a UI redesenhar agora */
void ui_poke(void);

/* Loop da UI (display, LEDs, botões). Não retorna. */
void ui_run(void);

#endif
