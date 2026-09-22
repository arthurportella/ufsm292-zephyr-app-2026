/*
 * Interface local: OLED1 (128x32, 4 linhas de 21 caracteres), 3 LEDs, 3 botões.
 *
 *   BOTÃO 1: tela anterior    BOTÃO 2: próxima tela    BOTÃO 3: rolar (nó / evento)
 *   LED 1: gateway em falha   LED 2: servidor em falha LED 3: pisca a cada ciclo
 *
 * Sem display (native_sim) a tela é impressa no console quando muda.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "supervisor.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define ROWS 4
#define COLS 21

enum { SCR_STATUS, SCR_NODES, SCR_EVENTS, SCR_COUNT };

static K_SEM_DEFINE(redraw, 0, 1);
static atomic_t screen;
static atomic_t scroll;

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(led_gw), gpios, {0}),
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(led_srv), gpios, {0}),
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(led_hb), gpios, {0}),
};

#if defined(CONFIG_CHARACTER_FRAMEBUFFER)
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#else
static const struct device *const disp;
#endif

void ui_poke(void)
{
	k_sem_give(&redraw);
}

#ifdef CONFIG_INPUT
static void on_key(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_KEY || evt->value != 1) {
		return; /* só o aperto */
	}

	switch (evt->code) {
	case INPUT_KEY_1:
		atomic_set(&screen, (atomic_get(&screen) + SCR_COUNT - 1) % SCR_COUNT);
		atomic_clear(&scroll);
		break;
	case INPUT_KEY_2:
		atomic_set(&screen, (atomic_get(&screen) + 1) % SCR_COUNT);
		atomic_clear(&scroll);
		break;
	case INPUT_KEY_3:
		atomic_inc(&scroll);
		break;
	default:
		return;
	}
	ui_poke();
}
INPUT_CALLBACK_DEFINE(NULL, on_key, NULL);
#endif

static const char *state_str(const struct monitor *m, char tmp[6])
{
	if (m->failed) {
		return "FALHA";
	}
	if (m->fails > 0) {
		snprintf(tmp, 6, "?%d", m->fails); /* suspeito: falhou mas < N */
		return tmp;
	}
	return m->known ? "OK" : "--";
}

/* Monta as 4 linhas da tela atual. Chamar com sup_lock. */
static void build(char l[ROWS][COLS + 1], int scr, unsigned int off)
{
	char a[6];
	char b[6];

	memset(l, 0, ROWS * (COLS + 1));

	switch (scr) {
	case SCR_STATUS:
		snprintf(l[0], COLS + 1, "GW  %-5s up%5us n%d", state_str(&sup.gw, a),
			 sup.gw.uptime_ms / 1000, sup.nodes_online);
		snprintf(l[1], COLS + 1, "SRV %-5s up%5us", state_str(&sup.srv, b),
			 sup.srv.uptime_ms / 1000);
		snprintf(l[2], COLS + 1, "falhas gw%d srv%d N=%d", sup.gw.fails, sup.srv.fails,
			 CONFIG_SUPERVISOR_FAIL_THRESHOLD);
		snprintf(l[3], COLS + 1, "resets gw%d srv%d", sup.gw.resets, sup.srv.resets);
		break;

	case SCR_NODES: {
		if (sup.n_nodes == 0) {
			snprintf(l[0], COLS + 1, "Nos: sem dados");
			break;
		}
		int i = off % sup.n_nodes;
		const struct node_view *v = &sup.nodes[i];

		snprintf(l[0], COLS + 1, "No%d L:%d fl:%02x %d/%d", v->id, v->light, v->flags,
			 i + 1, sup.n_nodes);
		snprintf(l[1], COLS + 1, "T:%sC visto%5dms", v->temp, v->age_ms);
		snprintf(l[2], COLS + 1, "ax%s ay%s", v->accel[0], v->accel[1]);
		snprintf(l[3], COLS + 1, "az%s", v->accel[2]);
		break;
	}

	case SCR_EVENTS: {
		int avail = MIN(sup.n_events, MAX_EVENTS);

		if (avail == 0) {
			snprintf(l[0], COLS + 1, "Eventos: nenhum");
			break;
		}
		/* mais recente primeiro; BOTÃO 3 rola para os mais antigos */
		for (int r = 0, k = off % avail; r < ROWS && k < avail; r++, k++) {
			const struct event *e = &sup.events[(sup.n_events - 1 - k) % MAX_EVENTS];

			snprintf(l[r], COLS + 1, "%5u %s", e->t_s, e->text);
		}
		break;
	}
	}
}

static void draw(char l[ROWS][COLS + 1])
{
	static char last[ROWS][COLS + 1];

	if (disp != NULL) {
		cfb_framebuffer_clear(disp, false);
		for (int r = 0; r < ROWS; r++) {
			cfb_print(disp, l[r], 0, r * 8);
		}
		cfb_framebuffer_finalize(disp);
		return;
	}

	if (memcmp(l, last, sizeof(last)) != 0) {
		memcpy(last, l, sizeof(last));
		printk("+---------------------+\n");
		for (int r = 0; r < ROWS; r++) {
			printk("|%-21s|\n", l[r]);
		}
		printk("+---------------------+\n");
	}
}

static const struct device *display_init(void)
{
	if (disp == NULL) {
		return NULL;
	}
	if (!device_is_ready(disp)) {
		LOG_ERR("display não está pronto");
		return NULL;
	}
	if (display_set_pixel_format(disp, PIXEL_FORMAT_MONO10) != 0 &&
	    display_set_pixel_format(disp, PIXEL_FORMAT_MONO01) != 0) {
		LOG_ERR("formato de pixel não suportado");
		return NULL;
	}
	if (cfb_framebuffer_init(disp) != 0) {
		LOG_ERR("cfb_framebuffer_init falhou");
		return NULL;
	}
	cfb_framebuffer_clear(disp, true);
	cfb_framebuffer_set_font(disp, 0);
	display_blanking_off(disp);
	return disp;
}

void ui_run(void)
{
	char l[ROWS][COLS + 1];
	const struct device *d = display_init();

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (leds[i].port != NULL) {
			gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		}
	}

	for (;;) {
		bool on[ARRAY_SIZE(leds)];

		k_mutex_lock(&sup_lock, K_FOREVER);
		build(l, atomic_get(&screen), atomic_get(&scroll));
		on[0] = sup.gw.failed;
		on[1] = sup.srv.failed;
		on[2] = sup.cycles & 1;
		k_mutex_unlock(&sup_lock);

		for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
			if (leds[i].port != NULL) {
				gpio_pin_set_dt(&leds[i], on[i]);
			}
		}
		if (d != NULL || disp == NULL) {
			draw(l);
		}

		k_sem_take(&redraw, K_MSEC(1000));
	}
}
