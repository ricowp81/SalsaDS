/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — logger para PS3 (TTY + UDP). Ver log.h.
 */
#include "platform/ps3/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <lv2/mutex.h>
#include <net/net.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/systime.h>

static int log_sock = -1;
static struct sockaddr_in log_dest;
static char log_tag[24] = "salsads";
static u64 log_t0;
static u32 log_seq;
static FILE *log_file;
static int log_send_fails, log_send_ret, log_send_errno;
/* Una línea a la vez: el frontend escribe desde dos hilos. */
static sys_lwmutex_t log_mutex;
static int log_mutex_ok;

static void log_load_dest(void)
{
	char cfg[64] = "255.255.255.255";
	unsigned port = SALSADS_LOG_PORT;

	FILE *f = fopen(SALSADS_LOG_CFG, "r");
	if (f) {
		if (fgets(cfg, sizeof cfg, f)) {
			cfg[strcspn(cfg, " \r\n")] = '\0';
			char *colon = strchr(cfg, ':');
			if (colon) {
				*colon = '\0';
				port = (unsigned)strtoul(colon + 1, NULL, 10);
			}
		}
		fclose(f);
	}

	memset(&log_dest, 0, sizeof log_dest);
	log_dest.sin_family = AF_INET;
	log_dest.sin_port = htons(port);
	if (inet_pton(AF_INET, cfg, &log_dest.sin_addr) != 1)
		inet_pton(AF_INET, "255.255.255.255", &log_dest.sin_addr);
}

void salsads_log_init(const char *program)
{
	salsads_log_init_banner(program, NULL);
}

void salsads_log_init_banner(const char *program, const char *banner)
{
	snprintf(log_tag, sizeof log_tag, "%s", program);
	log_t0 = sysGetSystemTime();
	sys_lwmutex_attr_t attr = { SYS_LWMUTEX_PROTOCOL_FIFO, SYS_LWMUTEX_ATTR_NOT_RECURSIVE, "log" };
	log_mutex_ok = sysLwMutexCreate(&log_mutex, &attr) == 0;

	char path[64];
	/* La carpeta se crea si falta y se comprueba que quedó (stat), porque un
	 * mkdir que falla en silencio deja sin log por FTP. */
	const int mk = mkdir(SALSADS_DATA_DIR, 0777), mk_errno = errno;
	struct stat dir_st;
	const int dir_ok = stat(SALSADS_DATA_DIR, &dir_st) == 0 && S_ISDIR(dir_st.st_mode);
	snprintf(path, sizeof path, SALSADS_DATA_DIR "/%s.log", log_tag);
	log_file = fopen(path, "w");
	const int file_errno = errno;

	int ret = netInitialize(), sock = -1;
	if (ret >= 0) {
		sock = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock >= 0) {
			int on = 1;
			netSetSockOpt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
			log_load_dest();
			log_sock = sock;
		}
	}

	if (banner)
		salsads_log("%s", banner);
	if (!dir_ok)
		salsads_log("FALLO la carpeta " SALSADS_DATA_DIR " no existe y mkdir falló (errno %d): sin log por archivo",
			    mk_errno);
	else if (!log_file)
		salsads_log("FALLO no pude abrir %s (errno %d)", path, file_errno);
	else
		salsads_log("carpeta " SALSADS_DATA_DIR ": %s; log en %s", mk == 0 ? "creada ahora" : "ya existía", path);
	if (ret < 0)
		salsads_log("red no disponible (netInitialize = %d): solo TTY y archivo", ret);
	else if (sock < 0)
		salsads_log("netSocket falló (%d): solo TTY", sock);
	else
		salsads_log("log UDP hacia %s:%u", inet_ntoa(log_dest.sin_addr), ntohs(log_dest.sin_port));
}

void salsads_log(const char *fmt, ...)
{
	char line[SALSADS_LOG_LINE_MAX];
	if (log_mutex_ok)
		sysLwMutexLock(&log_mutex, 0);
	u64 us = sysGetSystemTime() - log_t0;
	int n = snprintf(line, sizeof line, "[%s %4u.%03u #%u] ", log_tag,
			 (unsigned)(us / 1000000), (unsigned)(us / 1000 % 1000), (unsigned)++log_seq);

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
	va_end(ap);

	size_t len = strlen(line);
	if (len == 0 || line[len - 1] != '\n') {
		line[len++] = '\n';
		line[len] = '\0';
	}

	fputs(line, stdout);
	fflush(stdout);
	if (log_file) {
		fputs(line, log_file);
		fflush(log_file);
	}
	if (log_sock >= 0) {
		int ret = netSendTo(log_sock, line, len, 0, (const struct sockaddr *)&log_dest, sizeof log_dest);
		if (ret < 0 && log_send_fails++ == 0) {
			log_send_ret = ret;
			log_send_errno = errno;
		}
	}
	if (log_mutex_ok)
		sysLwMutexUnlock(&log_mutex);
}

void salsads_log_shutdown(void)
{
	if (log_send_fails)
		salsads_log("netSendTo falló %d veces (primero: %d, errno %d)",
			    log_send_fails, log_send_ret, log_send_errno);
	if (log_sock >= 0) {
		netClose(log_sock);
		log_sock = -1;
		netDeinitialize();
	}
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
	if (log_mutex_ok) {
		sysLwMutexDestroy(&log_mutex);
		log_mutex_ok = 0;
	}
}
