/*
 * Cliente HTTP mínimo sobre sockets. Funciona com qualquer interface de rede
 * do Zephyr (driver Ethernet SPI, ou sockets do host no native_sim).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "http.h"

/* Remove o "Transfer-Encoding: chunked" no próprio buffer */
static int dechunk(char *p)
{
	char *out = p;
	char *in = p;

	for (;;) {
		char *end;
		unsigned long n = strtoul(in, &end, 16);

		if (end == in || (end = strstr(end, "\r\n")) == NULL) {
			return -EBADMSG;
		}
		if (n == 0) {
			break;
		}
		in = end + 2;
		if (strlen(in) < n) {
			return -EBADMSG;
		}
		memmove(out, in, n);
		out += n;
		in += n + 2;
	}
	*out = '\0';
	return 0;
}

/* Valor do cabeçalho `name` (sem o ':'), ou NULL */
static const char *header(const char *hdrs, const char *name)
{
	size_t len = strlen(name);

	for (const char *p = hdrs; p != NULL; p = strstr(p, "\r\n")) {
		p += (p == hdrs) ? 0 : 2;
		if (strncasecmp(p, name, len) == 0 && p[len] == ':') {
			return p + len + 1 + strspn(p + len + 1, " ");
		}
	}
	return NULL;
}

/* Resposta completa? Evita esperar o timeout se o servidor não fechar a conexão */
static bool complete(const char *buf, size_t used)
{
	const char *sep = strstr(buf, "\r\n\r\n");
	const char *v;

	if (sep == NULL) {
		return false;
	}
	v = header(buf, "Content-Length");
	if (v != NULL) {
		return used >= (size_t)(sep + 4 - buf) + strtoul(v, NULL, 10);
	}
	v = header(buf, "Transfer-Encoding");
	return v != NULL && strncasecmp(v, "chunked", 7) == 0 &&
	       used >= 5 && strcmp(buf + used - 5, "0\r\n\r\n") == 0;
}

int http_request(const char *addr, uint16_t port, const char *method, const char *path,
		 char *buf, size_t size, char **body)
{
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
	struct timeval tv = {
		.tv_sec = CONFIG_SUPERVISOR_TIMEOUT_MS / 1000,
		.tv_usec = (CONFIG_SUPERVISOR_TIMEOUT_MS % 1000) * 1000,
	};
	size_t used = 0;
	ssize_t n = 0;
	char *sep;
	int sock;
	int len;
	int ret;

	if (zsock_inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		return -EINVAL;
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}
	zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));

	len = snprintk(buf, size,
		       "%s %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Length: 0\r\n"
		       "Connection: close\r\n\r\n",
		       method, path, addr, port);

	if (zsock_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    zsock_send(sock, buf, len, 0) != len) {
		ret = -errno;
		goto out;
	}

	buf[0] = '\0';
	while (used < size - 1) {
		n = zsock_recv(sock, buf + used, size - 1 - used, 0);
		if (n <= 0) {
			break;
		}
		used += n;
		buf[used] = '\0';
		if (complete(buf, used)) {
			break;
		}
	}

	if (n < 0) {
		ret = -errno; /* EAGAIN = timeout */
	} else if (used == size - 1) {
		ret = -ENOBUFS;
	} else if (used < 12 || strncmp(buf, "HTTP/1.", 7) != 0 ||
		   (sep = strstr(buf, "\r\n\r\n")) == NULL) {
		ret = -EBADMSG; /* conexão fechada sem resposta (ex.: placa rebootando) */
	} else {
		const char *te = header(buf, "Transfer-Encoding");

		*body = sep + 4;
		ret = atoi(buf + 9);
		if (te != NULL && strncasecmp(te, "chunked", 7) == 0 && dechunk(*body) < 0) {
			ret = -EBADMSG;
		}
	}

out:
	zsock_close(sock);
	return ret;
}
