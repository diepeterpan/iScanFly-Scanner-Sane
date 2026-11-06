/* This file is licensed under Creative Commons License CC-CC0 1.0
 * (http://creativecommons.org/publicdomain/zero/1.0/).
 *
 * Created 2014-11-16 by bastel.
 *
 * See s400w.h for further information.
 *
 * @author bastel
 */

#ifdef WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define vsnprintf vsprintf_s
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

#include <errno.h>
#include <fcntl.h>

#include "s400w.h"

const char *S400W_LIB_VERSION = "1.0-20141122";

const int S400W_MIN_SET_RESOLUTION_FW = 26;

const char S400W_DEVICE_BUSY[] = {'d', 'e', 'v', 'b', 'u', 's', 'y', 0};
const char S400W_BATTERY_LOW[] = {'b', 'a', 't', 't', 'l', 'o', 'w', 0};
const char S400W_NO_PAPER[] = {'n', 'o', 'p', 'a', 'p', 'e', 'r', 0};
const char S400W_SCAN_READY[] = {'s', 'c', 'a', 'n', 'r',
                                 'e', 'a', 'd', 'y', 0};
const char S400W_CALIBRATE_GO[] = {'c', 'a', 'l', 'g', 'o', 0};
const char S400W_CALIBRATE_END[] = {'c', 'a', 'l', 'i', 'b',
                                    'r', 'a', 't', 'e', 0};
const char S400W_CLEAN_GO[] = {'c', 'l', 'e', 'a', 'n', 'g', 'o', 0};
const char S400W_CLEAN_END[] = {'c', 'l', 'e', 'a', 'n', 'e', 'n', 'd', 0};
const char S400W_DPI_STANDARD[] = {'d', 'p', 'i', 's', 't', 'd', 0};
const char S400W_DPI_HIGH[] = {'d', 'p', 'i', 'f', 'i', 'n', 'e', 0};
const char S400W_SCAN_GO[] = {'s', 'c', 'a', 'n', 'g', 'o', 0};
const char S400W_PREVIEW_END[] = {'p', 'r', 'e', 'v', 'i', 'e',
                                  'w', 'e', 'n', 'd', 0};
const char S400W_JPEG_SIZE[] = {'j', 'p', 'e', 'g', 's', 'i', 'z', 'e', 0};
const char S400W_EOF[] = {0};

/*////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Private /
//////////*/

static const char *RESPONSES[] = {S400W_DEVICE_BUSY,  S400W_BATTERY_LOW,
                                  S400W_NO_PAPER,     S400W_SCAN_READY,
                                  S400W_CALIBRATE_GO, S400W_CALIBRATE_END,
                                  S400W_CLEAN_GO,     S400W_CLEAN_END,
                                  S400W_DPI_STANDARD, S400W_DPI_HIGH,
                                  S400W_SCAN_GO,      S400W_PREVIEW_END,
                                  S400W_JPEG_SIZE,    NULL};

static const char GET_VERSION[] = {0x30, 0x30, 0x20, 0x20};
static const char GET_STATUS[] = {0x00, 0x60, 0x00, 0x50};
static const char START_CLEANING[] = {-0x80, -0x80, 0x70, 0x70};
static const char START_CALIBRATION[] = {0x00, -0x50, 0x00, -0x60};
static const char SET_DPI_STANDARD[] = {0x40, 0x30, 0x20, 0x10};
static const char SET_DPI_HIGH[] = {-0x80, 0x70, 0x60, 0x50};
static const char START_SCAN[] = {0x00, 0x20, 0x00, 0x10};
static const char SEND_PREVIEW_DATA[] = {0x40, 0x40, 0x30, 0x30};
static const char GET_JPEG_SIZE[] = {0x00, -0x30, 0x00, -0x40};
static const char SEND_JPEG_DATA[] = {0x00, -0x10, 0x00, -0x20};

static void mssleep(long ms) {
#ifdef WIN32
  _sleep(ms);
#else
  /*
  struct timespec tv;
  tv.tv_sec  = ms / 1000;
  tv.tv_nsec = ms % 1000 * 1000000L;
  nanosleep(tv);
  */
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  select(0, NULL, NULL, NULL, &tv);
#endif
}

static void _log(struct S400W *s400w, int level, const char *format, ...) {
  if (s400w->message) {
    if (strchr(format, '%') != NULL) {
      char message[1024];
      va_list args;
      va_start(args, format);
      vsnprintf(message, sizeof(message), format, args);
      va_end(args);
      s400w->message(level, message);
    } else {
      s400w->message(level, format);
    }
  }
}

static int openSocket(struct S400W *s400w) {
#ifdef WIN32
  unsigned long u1 = 1;
#endif
  int fd;
  struct sockaddr_in sa;
  struct hostent *hp;

  hp = gethostbyname(s400w->hostname);
  if (!hp) {
    _log(s400w, S400W_LOG_ERROR, "openSocket(): Host not found: %s",
         s400w->hostname);
    return -1;
  }

  /* fill in the "sockaddr_in" structure with information from "hostent"
   * structure */
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = hp->h_addrtype;
  sa.sin_port = htons(s400w->port);
  memcpy((void *)&sa.sin_addr, (void *)hp->h_addr_list[0], hp->h_length);

  /* setup connection to the remote server with retry to handle scanner waking
   * from standby */
  int retries = 3;
  fd = -1;
  while (retries > 0) {
    fd = socket(sa.sin_family, SOCK_STREAM, 0);
    if (fd < 0) {
      _log(s400w, S400W_LOG_ERROR, "openSocket(): socket() failed");
      return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&opt, sizeof(opt));
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));
#endif
    /* Clamp TCP receive buffer before connect() so Linux advertises a small
     * TCP Receive Window (RWIN <= 16 KB, Window Scaling disabled) on every
     * scan. This prevents the S400W's minimal embedded lwIP stack from
     * blasting huge bursts over 2.4 GHz Wi-Fi and deadlocking when a frame in
     * a large window is lost. */
    int rcvbuf = 16384;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf,
               sizeof(rcvbuf));

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
      break;
    }
    closesocket(fd);
    fd = -1;
    retries--;
    if (retries > 0) {
      _log(s400w, S400W_LOG_WARN,
           "openSocket(): connect to %s:%d failed, retrying in 1s...",
           inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
      mssleep(1000);
    }
  }

  if (retries == 0 || fd < 0) {
    _log(s400w, S400W_LOG_ERROR,
         "openSocket(): connect(%s:%d) failed: scanner unreachable or sleeping",
         inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    return -1;
  }

  /* set socket nonblocking */
#ifdef WIN32
  if (ioctlsocket(fd, FIONBIO, &u1)) {
#else
  if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
#endif
    _log(s400w, S400W_LOG_ERROR,
         "openSocket(): couldn't set socket to nonblocking!");
    closesocket(fd);
    return -1;
  }
  _log(s400w, S400W_LOG_INFO, "openSocket(): connected to %s:%d",
       inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  return fd;
}

/* timeout in milliseconds. Returns <0 for error, otherwise bit 0 set for read
 * data, bit 1 set for error data. */
static int checkStream(int fd, long timeout) {
  fd_set rfds, efds;
  struct timeval tv;
  int ret;

  do {
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    FD_SET(fd, &rfds);
    FD_SET(fd, &efds);

    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    ret = select(fd + 1, &rfds, NULL, &efds, timeout == -1 ? 0 : &tv);
  } while (ret < 0 && errno == EINTR);

  return ret <= 0
             ? ret
             : (FD_ISSET(fd, &rfds) ? 1 : 0) | (FD_ISSET(fd, &efds) ? 2 : 0);
}

/* Sends a 4 byte command to the scanner.
 * Returns number of bytes sent, -1 for error. */
static int sendCommand(struct S400W *s400w, int fd, const char *command) {
  int sent = 0;
  if (fd >= 0) {
    mssleep(200);
    while (sent < 4) {
      int n = send(fd, command + sent, 4 - sent, 0);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        sent = -1;
        break;
      }
      if (n == 0) {
        sent = -1;
        break;
      }
      sent += n;
    }
    _log(s400w, S400W_LOG_DEBUG, "sendCommand(%x): %d",
         *(unsigned int *)command, sent);
    if (sent > 0)
      mssleep(200);
  } else {
    sent = -1;
    _log(s400w, S400W_LOG_ERROR, "sendCommand(%x): not connected",
         *(unsigned int *)command);
  }
  return sent;
}

/* Receives a response from the scanner.
 * Returns number of bytes received, 0 for timeout, -1 for error. */
static int recvResponse(struct S400W *s400w, int fd, char *buffer, int limit,
                        long timeout) {
  if (fd >= 0) {
    if (checkStream(fd, timeout) & 1) {
      int read;
      do {
        read = recv(fd, buffer, limit, 0);
      } while (read < 0 && errno == EINTR);
      _log(s400w, S400W_LOG_TRACE, "recvResponse([%d], %ld): %d bytes", limit,
           timeout, read);
      /* closed socket */
      if (read == 0)
        read = -1;
      /* check for errors */
      else if (read == -1) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK)
          read = 0; /* shouldn't happen */
      }
      return read;
    }
    _log(s400w, S400W_LOG_ERROR, "recvResponse([%d], %ld): timeout", limit,
         timeout);
    return 0;
  }
  _log(s400w, S400W_LOG_ERROR, "recvResponse([%d], %ld): not connected", limit,
       timeout);
  return -1;
}

/* Tries to map a raw response to a defined response.
 * Returns original response if there is no match. */
static const char *detectResponse(const char *buffer, int length) {
  const char **res;
  if (length == 0)
    return NULL;
  if (length < 0)
    return S400W_EOF;
  for (res = RESPONSES; *res; res++) {
    int len = strlen(*res);
    if (len <= length && memcmp(buffer, *res, len) == 0)
      return *res;
  }
  return buffer;
}

/* Convenience operation for normal responses. Clears the internal buffer before
 * receiving. */
static const char *readResponse(struct S400W *s400w, int fd, long timeout) {
  memset(s400w->buffer, 0, sizeof(s400w->buffer));
  _log(s400w, S400W_LOG_DEBUG, "readResponse(%ld)", timeout);
  return detectResponse(
      s400w->buffer,
      recvResponse(s400w, fd, s400w->buffer, sizeof(s400w->buffer), timeout));
}

/* Opens a fresh TCP socket, sends a 4-byte command, and reads the response.
 * If the first socket is stale/half-open and times out without responding,
 * closes that socket, waits 500ms, and reconnects on a fresh socket instead of
 * writing duplicate bytes onto the same TCP stream. */
static int openAndCommand(struct S400W *s400w, const char *command,
                          long timeout, const char **out_response) {
  long attempt_timeout = (timeout > 4000) ? 3500 : timeout;
  *out_response = S400W_EOF;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      _log(s400w, S400W_LOG_WARN,
           "openAndCommand(%x): no response on initial socket, reconnecting "
           "(attempt %d)...",
           *(const unsigned int *)command, attempt + 1);
      mssleep(500);
    }
    int fd = openSocket(s400w);
    if (fd < 0) {
      *out_response = S400W_EOF;
      return -1;
    }
    if (sendCommand(s400w, fd, command) > 0) {
      const char *res = readResponse(s400w, fd, attempt_timeout);
      *out_response = res;
      if (res != NULL && res != S400W_EOF) {
        return fd;
      }
    }
    closesocket(fd);
  }
  return -1;
}

/*///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public /
/////////*/

void s400w_init(struct S400W *s400w, const char *hostname, int port,
                void (*message)(int, const char *)) {
  memset(s400w, 0, sizeof(struct S400W));
  s400w->hostname = hostname;
  s400w->port = port;
  s400w->message = message;
  s400w->timeout.normal = 10000;
  s400w->timeout.data = 30000;
  s400w->timeout.jpeg_size = 20000;
  s400w->timeout.jpeg_only = 60000;
}

int s400w_is_known_response(const char *response) {
  const char **res;
  for (res = RESPONSES; *res; res++)
    if (*res == response)
      return 1;
  return 0;
}

const char *s400w_get_version(struct S400W *s400w) {
  const char *response = S400W_EOF;
  int fd = openAndCommand(s400w, GET_VERSION, s400w->timeout.normal, &response);
  if (fd >= 0) {
    _log(s400w, S400W_LOG_DEBUG, "get_version(): %s",
         response ? response : "NULL");
    closesocket(fd);
    mssleep(300);
  }
  return response;
}

const char *s400w_get_status(struct S400W *s400w) {
  const char *response = S400W_EOF;
  int fd = openAndCommand(s400w, GET_STATUS, s400w->timeout.normal, &response);
  if (fd >= 0) {
    _log(s400w, S400W_LOG_DEBUG, "get_status(): %s",
         response ? response : "NULL");
    closesocket(fd);
    mssleep(300);
  }
  return response;
}

int s400w_set_resolution(struct S400W *s400w, int dpi) {
  int result = -1;
  const char *response = S400W_EOF;
  int fd = openAndCommand(s400w, dpi == 600 ? SET_DPI_HIGH : SET_DPI_STANDARD,
                          s400w->timeout.normal, &response);
  if (fd >= 0) {
    _log(s400w, S400W_LOG_DEBUG, "set_resolution(%d): %s", dpi,
         response ? response : "NULL");
    result = (dpi == 600 && response == S400W_DPI_HIGH) ||
                     (dpi != 600 && response == S400W_DPI_STANDARD)
                 ? 0
                 : -1;
    closesocket(fd);
    mssleep(300);
  }
  return result;
}

const char *s400w_clean(struct S400W *s400w) {
  const char *response = S400W_EOF;
  int fd = openAndCommand(s400w, GET_STATUS, s400w->timeout.normal, &response);
  if (fd >= 0) {
    _log(s400w, S400W_LOG_DEBUG, "clean.check: %s",
         response ? response : "NULL");
    if (response == S400W_SCAN_READY) {
      sendCommand(s400w, fd, START_CLEANING);
      mssleep(500);
      response = readResponse(s400w, fd, s400w->timeout.normal);
      _log(s400w, S400W_LOG_DEBUG, "clean().go: %s",
           response ? response : "NULL");
      if (response == S400W_CLEAN_GO) {
        response = readResponse(s400w, fd, 40000);
        _log(s400w, S400W_LOG_DEBUG, "clean().end: %s",
             response ? response : "NULL");
      }
    }
    closesocket(fd);
    mssleep(300);
  }
  return response;
}

const char *s400w_calibrate(struct S400W *s400w) {
  const char *response = S400W_EOF;
  int fd = openAndCommand(s400w, GET_STATUS, s400w->timeout.normal, &response);
  if (fd >= 0) {
    _log(s400w, S400W_LOG_DEBUG, "calibrate().check: %s",
         response ? response : "NULL");
    if (response == S400W_SCAN_READY) {
      sendCommand(s400w, fd, START_CALIBRATION);
      mssleep(500);
      response = readResponse(s400w, fd, s400w->timeout.normal);
      _log(s400w, S400W_LOG_DEBUG, "calibrate().go: %s",
           response ? response : "NULL");
      if (response == S400W_CALIBRATE_GO) {
        response = readResponse(s400w, fd, 60000);
        _log(s400w, S400W_LOG_DEBUG, "calibrate().end: %s",
             response ? response : "NULL");
      }
    }
    closesocket(fd);
    mssleep(300);
  }
  return response;
}

const char *s400w_scan(struct S400W *s400w, int resolution,
                       s400w_receiveFunc previewFunc,
                       s400w_receiveFunc jpegFunc) {
  const char *response = S400W_EOF;
  int fd = -1;

  /* Poll GET_STATUS with a fresh socket if "nopaper" is returned, and keep the
   * open socket fd as soon as "scanready" is returned so the entire scan runs
   * on that single ready connection without closing/reopening. */
  for (int wait_sec = 0;; wait_sec++) {
    fd = openAndCommand(s400w, GET_STATUS, s400w->timeout.normal, &response);
    _log(s400w, S400W_LOG_DEBUG, "scan().check: %s",
         response ? response : "NULL");
    if (fd >= 0 && response == S400W_SCAN_READY) {
      break;
    }
    if (fd >= 0) {
      closesocket(fd);
      fd = -1;
    }
    if (response == S400W_BATTERY_LOW) {
      _log(s400w, S400W_LOG_WARN, "Warning: battery low");
    }
    if (s400w->paper_wait_sec <= 0 || wait_sec >= s400w->paper_wait_sec ||
        (s400w->cancelled && *s400w->cancelled) ||
        (response != S400W_NO_PAPER && response != S400W_BATTERY_LOW)) {
      return response;
    }
    mssleep(1000);
  }

  if (fd > 0) {
    const int tagLength =
        sizeof(S400W_PREVIEW_END); /* the padding 0 byte is necessary */
    const int bufferSize = 61440 + tagLength;
    int abort = 0;
    char *buffer = (char *)malloc(bufferSize);

    if (!abort && resolution > 0) {
      if (sendCommand(s400w, fd,
                      resolution == 600 ? SET_DPI_HIGH : SET_DPI_STANDARD) >
          0) {
        response = readResponse(s400w, fd, s400w->timeout.normal);
        _log(s400w, S400W_LOG_DEBUG, "scan().dpi(%d): %s", resolution,
             response ? response : "NULL");
        abort = (resolution == 600 && response == S400W_DPI_HIGH) ||
                        (resolution != 600 && response == S400W_DPI_STANDARD)
                    ? 0
                    : 1;
      } else {
        abort = 1;
      }
    }

    if (!abort) {
      if (sendCommand(s400w, fd, START_SCAN) > 0) {
        response = readResponse(s400w, fd, s400w->timeout.normal);
        _log(s400w, S400W_LOG_DEBUG, "scan().go: %s",
             response ? response : "NULL");
        if (response != S400W_SCAN_GO)
          abort = 1;
      } else {
        abort = 1;
      }
    }

    if (!abort && previewFunc) {
      if (sendCommand(s400w, fd, SEND_PREVIEW_DATA) > 0) {
        char *partBuf = buffer + tagLength;
        int read, total = 0;

        mssleep(1000);
        read = recvResponse(s400w, fd, partBuf, bufferSize - tagLength,
                            s400w->timeout.data);

        /* if known response: error */
        if (read > 0 && read <= (int)sizeof(s400w->buffer) &&
            detectResponse(partBuf, read) != partBuf) {
          abort = 1;
          memset(s400w->buffer, 0, sizeof(s400w->buffer));
          memcpy(s400w->buffer, partBuf, read);
          response = s400w->buffer;
          _log(s400w, S400W_LOG_ERROR, "scan().preview: %s", response);
        } else {
          int previewStatus = 0; /* = previewFunc(SEND_PREVIEW_DATA, 0, 0);*/
          /* this is a bit tricky. we need to carry over bytes in between
           * fetching so we can detect the end marker even if it is torn
           * apart.*/
          while (read > 0) {
            total += read;
            _log(s400w, S400W_LOG_TRACE, "scan().preview: %d (%d lines)", total,
                 total / 1920);
            if (previewStatus == 0)
              previewStatus = previewFunc(partBuf, read);
            memmove(buffer, partBuf + read - tagLength, tagLength);
            if (memcmp(S400W_PREVIEW_END, buffer, tagLength - 1) == 0)
              break;
            read = recvResponse(s400w, fd, partBuf, bufferSize - tagLength,
                                s400w->timeout.data);
          }
          if (read < 0) {
            response = S400W_EOF;
            abort = 1;
          }
          if (read == 0) {
            response = NULL;
            abort = 1;
          }
          if (read > 0) {
            response = S400W_SCAN_READY;
          }
        }
        previewFunc(S400W_EOF, 0);
      }
    }

    if (!abort && jpegFunc) {
      if (previewFunc)
        mssleep(1000);

      int jslen = (int)strlen(S400W_JPEG_SIZE);
      int expected_hdr_len = jslen + 4;
      int size = 0;

      for (int js_attempt = 0; js_attempt < 2 && size <= 0; js_attempt++) {
        if (js_attempt > 0) {
          _log(s400w, S400W_LOG_WARN,
               "scan().jpegsize: retrying GET_JPEG_SIZE (attempt %d)...",
               js_attempt + 1);
          mssleep(500);
        }
        if (sendCommand(s400w, fd, GET_JPEG_SIZE) > 0) {
          memset(s400w->buffer, 0, sizeof(s400w->buffer));
          long js_timeout =
              previewFunc ? s400w->timeout.jpeg_size : s400w->timeout.jpeg_only;
          int rlen = recvResponse(s400w, fd, s400w->buffer,
                                  sizeof(s400w->buffer), js_timeout);
          /* Ensure all 12 bytes ("jpegsize" + 4-byte size) are read if split
           * across TCP segments */
          while (rlen >= jslen && rlen < expected_hdr_len &&
                 memcmp(s400w->buffer, S400W_JPEG_SIZE, jslen) == 0) {
            int more =
                recvResponse(s400w, fd, s400w->buffer + rlen,
                             expected_hdr_len - rlen, s400w->timeout.normal);
            if (more <= 0)
              break;
            rlen += more;
          }
          response = detectResponse(s400w->buffer, rlen);
          _log(s400w, S400W_LOG_DEBUG, "scan().jpegsize: %s (read %d bytes)",
               response ? response : "NULL", rlen);
          if (response == S400W_JPEG_SIZE && rlen >= expected_hdr_len) {
            size =
                (0x000000FF & (unsigned char)s400w->buffer[jslen]) |
                (0x0000FF00 &
                 ((unsigned int)(unsigned char)s400w->buffer[jslen + 1] << 8)) |
                (0x00FF0000 &
                 ((unsigned int)(unsigned char)s400w->buffer[jslen + 2]
                  << 16)) |
                (0xFF000000 &
                 ((unsigned int)(unsigned char)s400w->buffer[jslen + 3] << 24));
          }
        }
      }

      if (response == S400W_JPEG_SIZE && size > 0) {
        _log(s400w, S400W_LOG_INFO, "scan().jpeg: %d bytes", size);

        response = S400W_EOF;
        int transfer_stalled = 0;
        for (int data_attempt = 0; data_attempt < 2; data_attempt++) {
          if (data_attempt > 0) {
            _log(s400w, S400W_LOG_WARN,
                 "scan().jpeg: incomplete transfer, retrying SEND_JPEG_DATA "
                 "(attempt %d)...",
                 data_attempt + 1);
            mssleep(500);
          }
          if (jpegFunc(S400W_JPEG_SIZE, size) == 0 &&
              sendCommand(s400w, fd, SEND_JPEG_DATA) > 0) {
            int read = 0, total = 0, chunks = 0;
            unsigned char prev_byte = 0, last_byte = 0;
            int eoi_detected = 0;
            transfer_stalled = 0;
            mssleep(500);
            do {
              /* Wait up to timeout.data for the first chunk; once streaming
               * starts (total > 0), use an 8s inter-chunk timeout so 2.4 GHz
               * Wi-Fi retransmissions or scanner buffer pauses don't abort
               * prematurely while still avoiding a 30s hang */
              long chunk_timeout = (total > 0) ? 8000 : s400w->timeout.data;
              if (total > 0 && !(checkStream(fd, chunk_timeout) & 1)) {
                _log(s400w, S400W_LOG_WARN,
                     "scan().jpeg: stream idle after %d / %d bytes (%d chunks)",
                     total, size, chunks);
                transfer_stalled = 1;
                read = 0;
                break;
              }
              read = recvResponse(s400w, fd, buffer, bufferSize, chunk_timeout);
              if (read > 0) {
                chunks++;
                if (read >= 2) {
                  prev_byte = (unsigned char)buffer[read - 2];
                  last_byte = (unsigned char)buffer[read - 1];
                } else if (read == 1) {
                  prev_byte = last_byte;
                  last_byte = (unsigned char)buffer[0];
                }
                total += read;
                _log(s400w, S400W_LOG_TRACE, "scan().jpeg: %d / %d bytes",
                     total, size);
                if (jpegFunc(buffer, read) != 0)
                  break;

                /* Check for JPEG End-Of-Image (EOI: 0xFF 0xD9) marker when the
                 * scanner firmware's reported jpegsize is larger than actual
                 * compressed image bytes and no more data is queued */
                if (total > 1024 && prev_byte == 0xFF && last_byte == 0xD9 &&
                    (total >= size || !(checkStream(fd, 150) & 1))) {
                  eoi_detected = 1;
                  _log(s400w, S400W_LOG_DEBUG,
                       "scan().jpeg: EOI (FF D9) reached at %d / %d bytes "
                       "(%d chunks)",
                       total, size, chunks);
                  break;
                }
              }
            } while (total < size && read > 0);
            if (total >= size || eoi_detected) {
              _log(s400w, S400W_LOG_DEBUG,
                   "scan().jpeg: transfer complete (%d / %d bytes, %d chunks)",
                   total, size, chunks);
              response = S400W_SCAN_READY;
              abort = 0;
              transfer_stalled = 0;
              break;
            }
            /* If >= 95% of reported jpegsize arrived, only the last 1-2
             * scanlines may be truncated; preserve buffer so
             * decode_and_process_jpeg() can apply scanline fault tolerance.
             * If < 95% arrived on attempt 1, allow retry of SEND_JPEG_DATA. */
            if (total > 0 && ((long long)total * 100 >= (long long)size * 95 ||
                              data_attempt == 1)) {
              response = S400W_SCAN_READY;
              abort = 0;
              break;
            }
            if (read < 0) {
              response = S400W_EOF;
              abort = 1;
            } else if (read == 0) {
              response = NULL;
              abort = 1;
            }
          } else {
            break;
          }
        }
        if (transfer_stalled) {
          /* Send immediate TCP RST when closing a stalled stream so the
           * scanner's single-threaded lwIP stack aborts its stuck send() loop
           * immediately instead of blocking the next scan */
          struct linger sl = {1, 0};
          setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&sl, sizeof(sl));
        }
        jpegFunc(S400W_EOF, 0);
      }
    }

    free(buffer);
    closesocket(fd);
    mssleep(300);
  }
  return response;
}
