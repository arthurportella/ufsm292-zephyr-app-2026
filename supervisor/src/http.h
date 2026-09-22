#ifndef HTTP_H_
#define HTTP_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Requisição HTTP/1.1 bloqueante, com timeout (CONFIG_SUPERVISOR_TIMEOUT_MS).
 * A resposta inteira vai para buf; *body aponta para o corpo (NUL-terminado,
 * já sem chunked encoding).
 *
 * Retorna o status HTTP (200, 500, ...) ou -errno (timeout, conexão recusada...).
 */
int http_request(const char *addr, uint16_t port, const char *method, const char *path,
		 char *buf, size_t size, char **body);

#endif
