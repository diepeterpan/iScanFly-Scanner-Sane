/* This file is licensed under Creative Commons License CC-CC0 1.0 (http://creativecommons.org/publicdomain/zero/1.0/).
 *
 * Created 2014-11-16 by bastel.
 *
 * See s400w.h for further information.
 *
 * @author bastel
 */

#ifdef WIN32
#	include <winsock2.h>
#	pragma comment(lib, "Ws2_32.lib")
#	include <stdlib.h>
#	include <stdio.h>
#	include <stdarg.h>
#	include <string.h>
#	define vsnprintf vsprintf_s
#else
#	include <stdlib.h>
#	include <stdio.h>
#	include <stdarg.h>
#	include <string.h>
#	include <unistd.h>
#	include <sys/socket.h>
#	include <sys/select.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#	include <netdb.h>
#	define closesocket close
#endif

#include <fcntl.h>
#include <errno.h>

#include "s400w.h"

const char* S400W_LIB_VERSION = "1.0-20141122";

const int S400W_MIN_SET_RESOLUTION_FW = 26;

const char S400W_DEVICE_BUSY[]   = { 'd','e','v','b','u','s','y',0 };
const char S400W_BATTERY_LOW[]   = { 'b','a','t','t','l','o','w',0 };
const char S400W_NO_PAPER[]      = { 'n','o','p','a','p','e','r',0 };
const char S400W_SCAN_READY[]    = { 's','c','a','n','r','e','a','d','y',0 };
const char S400W_CALIBRATE_GO[]  = { 'c','a','l','g','o',0 };
const char S400W_CALIBRATE_END[] = { 'c','a','l','i','b','r','a','t','e',0 };
const char S400W_CLEAN_GO[]      = { 'c','l','e','a','n','g','o',0 };
const char S400W_CLEAN_END[]     = { 'c','l','e','a','n','e','n','d',0 };
const char S400W_DPI_STANDARD[]  = { 'd','p','i','s','t','d',0 };
const char S400W_DPI_HIGH[]      = { 'd','p','i','f','i','n','e',0 };
const char S400W_SCAN_GO[]       = { 's','c','a','n','g','o',0 };
const char S400W_PREVIEW_END[]   = { 'p','r','e','v','i','e','w','e','n','d',0 };
const char S400W_JPEG_SIZE[]     = { 'j','p','e','g','s','i','z','e',0 };
const char S400W_EOF[]           = { 0 };


/*////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Private /
//////////*/

static const char* RESPONSES[] = {
	S400W_DEVICE_BUSY,
	S400W_BATTERY_LOW,
	S400W_NO_PAPER,
	S400W_SCAN_READY,
	S400W_CALIBRATE_GO,
	S400W_CALIBRATE_END,
	S400W_CLEAN_GO,
	S400W_CLEAN_END,
	S400W_DPI_STANDARD,
	S400W_DPI_HIGH,
	S400W_SCAN_GO,
	S400W_PREVIEW_END,
	S400W_JPEG_SIZE,
	NULL
};

static const char GET_VERSION[]       = {  0x30,  0x30, 0x20,  0x20 };
static const char GET_STATUS[]        = {  0x00,  0x60, 0x00,  0x50 };
static const char START_CLEANING[]    = { -0x80, -0x80, 0x70,  0x70 };
static const char START_CALIBRATION[] = {  0x00, -0x50, 0x00, -0x60 };
static const char SET_DPI_STANDARD[]  = {  0x40,  0x30, 0x20,  0x10 };
static const char SET_DPI_HIGH[]      = { -0x80,  0x70, 0x60,  0x50 };
static const char START_SCAN[]        = {  0x00,  0x20, 0x00,  0x10 };
static const char SEND_PREVIEW_DATA[] = {  0x40,  0x40, 0x30,  0x30 };
static const char GET_JPEG_SIZE[]     = {  0x00, -0x30, 0x00, -0x40 };
static const char SEND_JPEG_DATA[]    = {  0x00, -0x10, 0x00, -0x20 };


static void mssleep(long ms)
{
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
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	select(0, NULL, NULL, NULL, &tv);
#endif
}


static void _log(struct S400W* s400w, int level, const char* format, ...)
{
	if ( s400w->message ) {
		if ( strchr(format, '%')!=NULL ) {
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


static int openSocket(struct S400W* s400w)
{
#ifdef WIN32
	unsigned long u1 = 1;
#endif
	int fd;
	struct sockaddr_in sa;
	struct hostent* hp;

	hp = gethostbyname(s400w->hostname);
	if ( !hp ) {
		_log(s400w, -1, "openSocket(): Host not found: %s", s400w->hostname);
		return -1;
	}

	/* fill in the "sockaddr_in" structure with information from "hostent" structure */
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = hp->h_addrtype;
	sa.sin_port   = htons(s400w->port);
	memcpy((void*)&sa.sin_addr, (void*)hp->h_addr_list[0], hp->h_length);

	/* open a TCP/STREAM socket */
	fd = socket(sa.sin_family, SOCK_STREAM, 0);
	if ( fd<0 ) {
		_log(s400w, -1, "openSocket(): socket() failed");
		return -1;
	}

	/* setup connection to the remote server */
	if ( connect(fd, (struct sockaddr*)&sa, sizeof(sa))<0 ) {
		_log(s400w, -1, "openSocket(): connect(%s:%d) failed", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
		return -1;
	}

	/* set socket nonblocking */
#ifdef WIN32
	if ( ioctlsocket(fd, FIONBIO, &u1) ) {
#else
	if ( fcntl(fd, F_SETFL, O_NONBLOCK)==-1 ) {
#endif
		_log(s400w, -1, "openSocket(): couldn't set socket to nonblocking!");
		return -1;
	}
	_log(s400w, 0, "openSocket(): connected to %s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
	return fd;
}


/* timeout in milliseconds. Returns <0 for error, otherwise bit 0 set for read data, bit 1 set for error data. */
static int checkStream(int fd, long timeout)
{
	fd_set rfds, efds;
	struct timeval tv;
	int ret;

	FD_ZERO(&rfds);
	FD_ZERO(&efds);
	FD_SET(fd, &rfds);
	FD_SET(fd, &efds);

	tv.tv_sec  = timeout / 1000;
	tv.tv_usec = timeout % 1000 * 1000;

	ret = select(fd + 1, &rfds, NULL, &efds, timeout == -1 ? 0 : &tv);
	return ret<=0 ? ret : (FD_ISSET(fd, &rfds) ? 1 : 0) | (FD_ISSET(fd, &efds) ? 2 : 0);
}


/* Sends a 4 byte command to the scanner.
 * Returns number of bytes sent, -1 for error. */
static int sendCommand(struct S400W* s400w, int fd, const char* command)
{
	int sent;
	if ( fd ) {
		mssleep(200);
		sent = send(fd, command, 4, 0);
		_log(s400w, 1, "sendCommand(%x): %d", *(unsigned int*)command, sent);
		if ( sent>0 ) mssleep(200);
	} else {
		sent = -1;
		_log(s400w, -1, "sendCommand(%x): not connected", *(unsigned int*)command);
	}
	return sent;
}


/* Receives a response from the scanner.
 * Returns number of bytes received, 0 for timeout, -1 for error. */
static int recvResponse(struct S400W* s400w, int fd, char* buffer, int limit, long timeout)
{
	if ( fd>=0 ) {
		if ( checkStream(fd, timeout) & 1 ) {
			int read = recv(fd, buffer, limit, 0);
			_log(s400w, 2, "recvResponse([%d], %d): ", limit, timeout, read);
			/* closed socket */
			if ( read==0 ) read = -1;
			/* check for errors */
			else if ( read==-1 ) {
				int err = errno;
				if ( err==EAGAIN || err==EWOULDBLOCK ) read = 0; /* shouldn't happen */
			}
			return read;
		 }
		_log(s400w, -1, "recvResponse([%d], %d): timeout", limit, timeout);
		 return 0;
	}
	_log(s400w, -1, "recvResponse([%d], %d): not connected", limit, timeout);
	return -1;
}


/* Tries to map a raw response to a defined response.
 * Returns original response if there is no match. */
static const char* detectResponse(const char* buffer, int length)
{
	const char** res;
	if ( length==0 ) return NULL;
	if ( length <0 ) return S400W_EOF;
	for ( res = RESPONSES; *res; res++ ) {
		int len = strlen(*res);
		if ( len<=length && memcmp(buffer, *res, len)==0 ) return *res;
	}
	return buffer;
}


/* Convenience operation for normal responses. Clears the internal buffer before receiving. */
static const char* readResponse(struct S400W* s400w, int fd, long timeout)
{
	memset(s400w->buffer, 0, sizeof(s400w->buffer));
	_log(s400w, 1, "readResponse(%d)", timeout);
	return detectResponse(s400w->buffer, recvResponse(s400w, fd, s400w->buffer, sizeof(s400w->buffer), timeout));
}



/*///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public /
/////////*/

void s400w_init(struct S400W* s400w, const char* hostname, int port, void (*message)(int, const char*))
{
	memset(s400w, 0, sizeof(struct S400W));
	s400w->hostname          = hostname;
	s400w->port              = port;
	s400w->message           = message;
	s400w->timeout.normal    = 10000;
	s400w->timeout.data      = 30000;
	s400w->timeout.jpeg_size = 20000;
	s400w->timeout.jpeg_only = 60000;
}


int s400w_is_known_response(const char* response)
{
	const char** res;
	for ( res = RESPONSES; *res; res++ ) if ( *res==response ) return 1;
	return 0;
}


const char* s400w_get_version(struct S400W* s400w)
{
	const char* response = S400W_EOF;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		if ( sendCommand(s400w, fd, GET_VERSION)>0 ) {
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "get_version(): %s", response);
		}
		closesocket(fd);
	}
	return response;
}


const char* s400w_get_status(struct S400W* s400w)
{
	const char* response = S400W_EOF;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		if ( sendCommand(s400w, fd, GET_STATUS)>0 ) {
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "get_status(): %s", response);
		}
		closesocket(fd);
	}
	return response;
}


int s400w_set_resolution(struct S400W* s400w, int dpi)
{
	int result = -1;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		if ( sendCommand(s400w, fd, dpi==600 ? SET_DPI_HIGH : SET_DPI_STANDARD)>0 ) {
			const char* response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "set_resolution(%d): %s", dpi, response);
			result = dpi==600 && response==S400W_DPI_HIGH || dpi!=600 && response==S400W_DPI_STANDARD ? 0 : -1;
		}
		closesocket(fd);
	}
	return result;
}


const char* s400w_clean(struct S400W* s400w)
{
	const char* response = S400W_EOF;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		if ( sendCommand(s400w, fd, GET_STATUS)>0 ) {
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "clean.check: %s", response);
			if ( response==S400W_SCAN_READY ) {
				sendCommand(s400w, fd, START_CLEANING);
				mssleep(500);
				response = readResponse(s400w, fd, s400w->timeout.normal);
				_log(s400w, 1, "clean().go: %s", response);
				if ( response==S400W_CLEAN_GO ) {
					response = readResponse(s400w, fd, 40000);
					_log(s400w, 1, "clean().end: %s", response);
				}
			}
		}
		closesocket(fd);
	}
	return response;
}


const char* s400w_calibrate(struct S400W* s400w)
{
	const char* response = S400W_EOF;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		if ( sendCommand(s400w, fd, GET_STATUS)>0 ) {
			mssleep(200);
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "calibrate().check: %s", response);
			if ( response==S400W_SCAN_READY ) {
				sendCommand(s400w, fd, START_CALIBRATION);
				mssleep(500);
				response = readResponse(s400w, fd, s400w->timeout.normal);
				_log(s400w, 1, "calibrate().go: %s", response);
				if ( response==S400W_CALIBRATE_GO ) {
					response = readResponse(s400w, fd, 60000);
					_log(s400w, 1, "calibrate().end: %s", response);
				}
			}
		}
		closesocket(fd);
	}
	return response;
}


const char* s400w_scan(struct S400W* s400w, int resolution, s400w_receiveFunc previewFunc, s400w_receiveFunc jpegFunc)
{
	const char* response = S400W_EOF;
	int fd = openSocket(s400w);
	if ( fd>0 ) {
		const int tagLength = sizeof(S400W_PREVIEW_END); /* the padding 0 byte is necessary */
		const int bufferSize = 61440 + tagLength;
		int abort = 0;
		char* buffer = (char*)malloc(bufferSize);

		if ( sendCommand(s400w, fd, GET_STATUS)>0 ) {
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "scan().check: %s", response);
			if ( response!=S400W_SCAN_READY ) abort = 1;
		} else abort = 1;

		if ( !abort && resolution>0 ) {
			if ( sendCommand(s400w, fd, resolution==600 ? SET_DPI_HIGH : SET_DPI_STANDARD)>0 ) {
				response = readResponse(s400w, fd, s400w->timeout.normal);
				_log(s400w, 1, "scan().dpi(%d): %s", resolution, response);
				abort = resolution==600 && response==S400W_DPI_HIGH || resolution!=600 && response==S400W_DPI_STANDARD ? 0 : 1;
			} else abort = 1;
		}

		if ( !abort && sendCommand(s400w, fd, START_SCAN)>0 ) {
			response = readResponse(s400w, fd, s400w->timeout.normal);
			_log(s400w, 1, "scan().go: %s", response);
			if ( response!=S400W_SCAN_GO ) abort = 1;
		} else abort = 1;

		if ( !abort && previewFunc ) {
			if ( sendCommand(s400w, fd, SEND_PREVIEW_DATA)>0 ) {
				char* partBuf = buffer + tagLength;
				int read, total = 0;

				mssleep(1000);
				read = recvResponse(s400w, fd, partBuf, bufferSize - tagLength, s400w->timeout.data);

				/* if known response: error */
				if ( read>0 && read<=sizeof(s400w->buffer) && detectResponse(partBuf, read)!=partBuf ) {
					abort = 1;
					memset(s400w->buffer, 0, sizeof(s400w->buffer));
					memcpy(s400w->buffer, partBuf, read);
					response = s400w->buffer;
					_log(s400w, -1, "scan().preview: %s", response);
				} else {
					int previewStatus = 0; /* = previewFunc(SEND_PREVIEW_DATA, 0, 0);*/
					/* this is a bit tricky. we need to carry over bytes in between fetching
					 * so we can detect the end marker even if it is torn apart.*/
					while ( read>0 ) {
						total += read;
						_log(s400w, 2, "scan().preview: %d (%d lines)", total, total / 1920);
						if ( previewStatus==0 ) previewStatus = previewFunc(partBuf, read);
						memmove(buffer, partBuf + read - tagLength, tagLength);
						if ( memcmp(S400W_PREVIEW_END, buffer, tagLength - 1)==0 ) break;
						read = recvResponse(s400w, fd, partBuf, bufferSize - tagLength, s400w->timeout.data);
					}
					if ( read <0 ) { response = S400W_EOF; abort = 1; }
					if ( read==0 ) { response = NULL;      abort = 1; }
					if ( read >0 ) { response = S400W_SCAN_READY; }
				}
				previewFunc(S400W_EOF, 0);
			}
		}

		if ( !abort && jpegFunc ) {
			if ( previewFunc ) mssleep(1000);
			if ( sendCommand(s400w, fd, GET_JPEG_SIZE)>0 ) {
				response = readResponse(s400w, fd, previewFunc ? s400w->timeout.jpeg_size : s400w->timeout.jpeg_only);
				_log(s400w, 1, "scan().jpegsize: %s", response);
				if ( response==S400W_JPEG_SIZE ) {
					int jslen = strlen(S400W_JPEG_SIZE);
					/* TODO: a bit lazy here, not checking if size read == JPEG_SIZE.length + 4... */
					int size = 0x000000FF & s400w->buffer[jslen]
						| 0x0000FF00 & (s400w->buffer[jslen + 1] << 8)
						| 0x00FF0000 & (s400w->buffer[jslen + 2] << 16)
						| 0xFF000000 & (s400w->buffer[jslen + 3] << 24);
					_log(s400w, 1, "scan().jpeg: %d bytes", size);

					response = S400W_EOF;
					if ( jpegFunc(S400W_JPEG_SIZE, size)==0 && sendCommand(s400w, fd, SEND_JPEG_DATA)>0 ) {
						int read, total = 0;
						mssleep(500);
						do {
							read = recvResponse(s400w, fd, buffer, bufferSize, s400w->timeout.data);
							if ( read>0 ) {
								total += read;
								_log(s400w, 2, "scan().jpeg: %d / %d bytes", total, size);
								if ( jpegFunc(buffer, read)!=0 ) break;
							}
						} while ( total<size && read>0 );
						if ( read <0 ) { response = S400W_EOF; abort = 1; }
						if ( read==0 ) { response = NULL;      abort = 1; }
						if ( read >0 ) { response = S400W_SCAN_READY; }
					}
					jpegFunc(S400W_EOF,  0);
				}
			}
		}

		free(buffer);
		closesocket(fd);
	}
	return response;
}
