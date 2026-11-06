/* This file is licensed under CC-CC0 1.0
 * (http://creativecommons.org/publicdomain/zero/1.0/).
 *
 * Created 2014-11-20 by bastel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#define sleep(A) _sleep(1000 * (A))
#define snprintf sprintf_s
#else
#include <unistd.h>
#endif

#include "s400w.h"

static const char *name = "";
static FILE *file = NULL;

static int preview(const char *data, int length) {
  if (data == S400W_EOF) {
    if (file)
      fclose(file);
    file = NULL;
    return 0;
  }
  if (!file) {
    file = fopen(name, "wb");
    return file ? 0 : -1;
  }
  return fwrite(data, length, 1, file) == 1 ? 0 : -1;
}

static int jpeg(const char *data, int length) {
  if (data == S400W_EOF) {
    if (file)
      fclose(file);
    file = NULL;
    return 0;
  } else if (data == S400W_JPEG_SIZE) {
    file = fopen(name, "wb");
    return file ? 0 : -1;
  }
  return fwrite(data, length, 1, file) == 1 ? 0 : -1;
}

static int init() {
#ifdef WIN32
  WORD wVersionRequested = MAKEWORD(1, 1);
  WSADATA wsaData;
  int err = WSAStartup(wVersionRequested, &wsaData);
  if (err != 0) {
    fprintf(stderr, "main(): Error: Couldn't init winsock\n");
    return -1;
  }
#endif
  return 0;
}

static int cleanup() {
#ifdef WIN32
  WSACleanup();
#endif
  return 0;
}

static void debug(int level, const char *message) {
  switch (level) {
  case S400W_LOG_ERROR:
    fprintf(stderr, "Error> s400w: %s\n", message);
    break;
  case S400W_LOG_WARN:
    fprintf(stderr, "Warn > s400w: %s\n", message);
    break;
  case S400W_LOG_INFO:
    fprintf(stdout, "Info > s400w: %s\n", message);
    break;
  case S400W_LOG_DEBUG:
    fprintf(stdout, "Debug> s400w: %s\n", message);
    break;
    /*case S400W_LOG_TRACE: fprintf(stdout, "Trace> s400w: %s\n", message);*/
  }
}

static int canDoDPI(struct S400W *s400w) {
  const char *buf = s400w_get_version(s400w);
  const char *dot;
  sleep(1);
  if (buf == NULL || buf == S400W_EOF || s400w_is_known_response(buf))
    return -1;
  dot = strchr(buf, '.');
  return dot && atoi(dot + 1) >= S400W_MIN_SET_RESOLUTION_FW ? 0 : -1;
}

static int process(int argc, char *argv[]) {
  const char *cmd = argv[3];
  char **ev = argv + 4;
  const int ec = argc - 4;
  const char *buf;
  struct S400W s400w;
  int ret = -1;

  s400w_init(&s400w, argv[1], atoi(argv[2]), &debug);

  if (!strcmp("version", cmd)) {
    buf = s400w_get_version(&s400w);
    if (buf)
      printf("result: %s\n", buf);
    if (buf != NULL && buf != S400W_EOF && !s400w_is_known_response(buf))
      ret = 0;
  }

  else if (!strcmp("status", cmd)) {
    buf = s400w_get_status(&s400w);
    if (buf)
      printf("result: %s\n", buf);
    if (buf != NULL && buf != S400W_EOF)
      ret = 0;
  }

  else if (!strcmp("dpi", cmd)) {
    if (ec > 0 && (!strcmp("300", ev[0]) || !strcmp("600", ev[0])) &&
        !s400w_set_resolution(&s400w, atoi(ev[0])))
      ret = 0;
  }

  else if (!strcmp("clean", cmd)) {
    buf = s400w_clean(&s400w);
    printf("result: %s\n", buf);
    if (buf == S400W_CLEAN_END)
      ret = 0;
  }

  else if (!strcmp("calibrate", cmd)) {
    buf = s400w_calibrate(&s400w);
    printf("result: %s\n", buf);
    if (buf == S400W_CALIBRATE_END)
      ret = 0;
  }

  else if (!strcmp("preview", cmd)) {
    char temp[30];
    snprintf(temp, 30, "./%d.raw", time(NULL));
    name = ec > 0 ? ev[0] : temp;
    buf = s400w_scan(&s400w, 0, preview, NULL);
    printf("result: %s\n", buf);
    if (buf == S400W_SCAN_READY)
      ret = 0;
  }

  else if (!strcmp("scan", cmd)) {
    const int dpi = ec == 0                 ? 0
                    : !strcmp("300", ev[0]) ? 300
                    : !strcmp("600", ev[0]) ? 600
                                            : 0;
    char temp[30];
    snprintf(temp, 30, "./%d.jpg", time(NULL));
    name = ec > 1 ? ev[1] : ec > 0 && dpi == 0 ? ev[0] : temp;
    buf = s400w_scan(&s400w, dpi && canDoDPI(&s400w) ? 0 : dpi, NULL, jpeg);
    printf("result: %s\n", buf);
    if (buf == S400W_SCAN_READY)
      ret = 00;
  }
  return ret;
}

int main(int argc, char *argv[]) {
  int ret = 0;
  printf("s400w command line scanner v%s by bastel\n", S400W_LIB_VERSION);
  if (argc < 4) {
    printf("usage : %s <host> <port> <command> <options>\n"
           "command:\n"
           "  version\n"
           "  status\n"
           "  clean - use the cleaning sheet\n"
           "  calibrate - use the calibration sheet\n"
           "  dpi <300|600>\n"
           "  preview [filename]\n"
           "  scan [300|600] [filename]\n",
           argv[0]);
    return -1;
  }
  init();
  ret = process(argc, argv);
  cleanup();
  return ret;
}
