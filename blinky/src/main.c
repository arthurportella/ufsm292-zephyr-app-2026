#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Obtém a definição do LED 'led0' configurado no DTS da SAMD21 XPro (PB30) */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
    int ret;

    LOG_INF("Iniciando a aplicacao Blink na SAMD21 XPro...");

    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("Dispositivo GPIO do LED nao esta pronto!");
        return 0;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Falha ao configurar o pino do LED (%d)", ret);
        return 0;
    }

    LOG_INF("LED configurado com sucesso. Iniciando o loop de pisca...");

    while (1) {
        ret = gpio_pin_toggle_dt(&led);
        if (ret < 0) {
            LOG_ERR("Falha ao alternar o estado do LED");
        }
        k_msleep(500); /* Aguarda 500ms (pisca a 1 Hz) */
    }

    return 0;
}
