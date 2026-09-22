/*
 * Supervisor (grupo D): consulta gateway e servidor via HTTP, mostra o estado
 * no OLED1, sinaliza falhas nos LEDs e tenta recuperar com POST /reset após
 * N timeouts consecutivos.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>

#include "http.h"
#include "supervisor.h"

LOG_MODULE_REGISTER(supervisor, LOG_LEVEL_INF);

K_MUTEX_DEFINE(sup_lock);

struct sup_state sup = {
	.gw = {.threshold = CONFIG_SUPERVISOR_FAIL_THRESHOLD,
	       .grace_ms = CONFIG_SUPERVISOR_RESET_GRACE_MS},
	.srv = {.threshold = CONFIG_SUPERVISOR_FAIL_THRESHOLD,
		.grace_ms = CONFIG_SUPERVISOR_RESET_GRACE_MS},
};

/* Uma resposta por vez: só a thread de consulta usa */
static char buf[1536];

/* --- JSON (contrato do PDF; campos extras são ignorados) ------------------ */

struct health_json {
	const char *status;
	int32_t uptime_ms;
	int32_t nodes_online;
};

static const struct json_obj_descr health_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct health_json, status, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct health_json, uptime_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct health_json, nodes_online, JSON_TOK_NUMBER),
};

struct node_json {
	int32_t node_id;
	int32_t light;
	struct json_obj_token temp_c;
	struct json_obj_token accel[3];
	size_t accel_len;
	int32_t last_seen_ms;
	int32_t flags;
};

static const struct json_obj_descr node_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct node_json, node_id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct node_json, light, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct node_json, temp_c, JSON_TOK_FLOAT),
	JSON_OBJ_DESCR_ARRAY(struct node_json, accel, 3, accel_len, JSON_TOK_FLOAT),
	JSON_OBJ_DESCR_PRIM(struct node_json, last_seen_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct node_json, flags, JSON_TOK_NUMBER),
};

struct data_json {
	struct node_json nodes[MAX_NODES];
	size_t n;
};

static const struct json_obj_descr data_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY(struct data_json, nodes, MAX_NODES, n, node_descr,
				 ARRAY_SIZE(node_descr)),
};

/* --- log de eventos ------------------------------------------------------- */

static void add_event(const char *fmt, ...)
{
	struct event *e;
	va_list ap;

	k_mutex_lock(&sup_lock, K_FOREVER);
	e = &sup.events[sup.n_events++ % MAX_EVENTS];
	e->t_s = k_uptime_get() / 1000;
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	LOG_INF("evento: %s", e->text);
	k_mutex_unlock(&sup_lock);
	ui_poke();
}

/* --- consultas ------------------------------------------------------------ */

static void copy_token(char *dst, size_t size, const struct json_obj_token *t)
{
	snprintf(dst, size, "%.*s", (int)t->length, t->start ? t->start : "");
}

static void poll_health(const char *name, struct monitor *m, const char *addr,
			uint16_t port, bool is_gateway)
{
	struct health_json h = {0};
	char *body = NULL;
	int status = http_request(addr, port, "GET", "/health", buf, sizeof(buf), &body);
	bool ok = (status == 200);
	uint32_t uptime = m->uptime_ms;
	unsigned int ev;

	if (ok) {
		int64_t fields = json_obj_parse(body, strlen(body), health_descr,
						ARRAY_SIZE(health_descr), &h);

		if (fields >= 0 && (fields & BIT(1))) {
			uptime = h.uptime_ms;
		}
	} else {
		LOG_WRN("%s /health falhou (%d)", name, status);
	}

	/* Vivo = respondeu 200. O status do corpo ("degraded"...) só é exibido:
	 * servidor sem gateway não é motivo para resetar o servidor.
	 */
	k_mutex_lock(&sup_lock, K_FOREVER);
	ev = monitor_update(m, ok, uptime, k_uptime_get());
	if (ok && is_gateway) {
		sup.nodes_online = h.nodes_online;
	}
	k_mutex_unlock(&sup_lock);

	if (ev & MON_EV_UP) {
		add_event("%s online", name);
	}
	if (ev & MON_EV_REBOOT) {
		add_event("%s reiniciou", name);
	}
	if (ev & MON_EV_DOWN) {
		add_event("%s FALHA", name);
	}
	if (ev & MON_EV_RESET) {
		status = http_request(addr, port, "POST", "/reset", buf, sizeof(buf), &body);
		add_event("%s reset %s", name, status == 200 ? "ok" : "s/resp");
	}
}

static void poll_data(void)
{
	static struct data_json d; /* ~420 B: fora da pilha */
	char *body = NULL;
	int status = http_request(CONFIG_SUPERVISOR_SERVER_ADDR, CONFIG_SUPERVISOR_SERVER_PORT,
				  "GET", "/data", buf, sizeof(buf), &body);
	int ret;

	if (status != 200) {
		return; /* /health já contabiliza a falha */
	}

	memset(&d, 0, sizeof(d));
	ret = json_arr_parse(body, strlen(body), data_descr, &d);
	if (ret < 0) {
		LOG_WRN("/data: JSON inválido (%d)", ret);
		return;
	}

	k_mutex_lock(&sup_lock, K_FOREVER);
	sup.n_nodes = d.n;
	for (size_t i = 0; i < d.n; i++) {
		struct node_view *v = &sup.nodes[i];
		const struct node_json *j = &d.nodes[i];

		v->id = j->node_id;
		v->light = j->light;
		v->flags = j->flags;
		v->age_ms = j->last_seen_ms;
		copy_token(v->temp, sizeof(v->temp), &j->temp_c);
		for (size_t k = 0; k < 3; k++) {
			copy_token(v->accel[k], sizeof(v->accel[k]),
				   k < j->accel_len ? &j->accel[k] : &(struct json_obj_token){0});
		}
	}
	k_mutex_unlock(&sup_lock);
}

static void poll_thread(void *p1, void *p2, void *p3)
{
	LOG_INF("gateway %s:%d, servidor %s:%d, N=%d", CONFIG_SUPERVISOR_GATEWAY_ADDR,
		CONFIG_SUPERVISOR_GATEWAY_PORT, CONFIG_SUPERVISOR_SERVER_ADDR,
		CONFIG_SUPERVISOR_SERVER_PORT, CONFIG_SUPERVISOR_FAIL_THRESHOLD);

	for (;;) {
		int64_t start = k_uptime_get();

		poll_health("GW", &sup.gw, CONFIG_SUPERVISOR_GATEWAY_ADDR,
			    CONFIG_SUPERVISOR_GATEWAY_PORT, true);
		poll_health("SRV", &sup.srv, CONFIG_SUPERVISOR_SERVER_ADDR,
			    CONFIG_SUPERVISOR_SERVER_PORT, false);
		if (!sup.srv.failed && sup.srv.fails == 0) {
			poll_data();
		}

		sup.cycles++;
		ui_poke();

		int64_t left = CONFIG_SUPERVISOR_POLL_MS - (k_uptime_get() - start);

		if (left > 0) {
			k_msleep(left);
		}
	}
}

K_THREAD_DEFINE(poll_tid, 2048, poll_thread, NULL, NULL, NULL, 7, 0, 1000);

int main(void)
{
	ui_run();
	return 0;
}
