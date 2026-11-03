#ifndef s400w_h_included
#define s400w_h_included

/* This file is licensed under Creative Commons License CC-CC0 1.0 (http://creativecommons.org/publicdomain/zero/1.0/).
 *
 * Created 2014-11-22 by bastel.
 *
 * Implements a low level interface to the Mustek(tm) S400W iScanAir(R) scanner.
 *
 * This is a clean room implementation based on written down specs (see commands.txt)
 * and does not contain any code by mustek. Using simple c and non blocking I/O,
 * it's easy to understand, clean and easily portable to other programming languages.
 * It is not thread safe, but only needs a little ram (around 64k kiB for scanning).
 *
 * Scanner related methods usually return a byte array containing the scanner's response. Please be aware
 * that the response array is not a copy and not constant for unknown responses but will be altered on the next
 * call to any scanner related method. Do not store it 'for later'.
 * Returned known responses are mapped to their static equivalent and can be stored and compared with <code>==</code>.
 *
 *
 * This library is licensed under the Creative Commons License CC-CC0 1.0 (http://creativecommons.org/publicdomain/zero/1.0/).
 */

/** Version */
extern const char* S400W_LIB_VERSION;

/** Firmware version necessary to set (higher) resolution. */
extern const int S400W_MIN_SET_RESOLUTION_FW;


/** S400W control object */
struct S400W {
	/* Scanner's address */
	const char* hostname;
	int port;
	/* Function to receive debug information, level is -1 = error, 0 = info, 1 = debug, 2 = verbose */
	void (*message)(int level, const char* message);
	/* Timeouts in milliseconds. */
	struct {
		/** Timeout for normal responses. */
		long normal;
		/** Timeout for receiving preview / jpeg data. */
		long data;
		/** Timeout for receiving jpegsize response after preview. */
		long jpeg_size;
		/** Timeout for receiving jpegsize response w/o preview. */
		long jpeg_only;
	} timeout;
	/* Internal buffer for responses to return to caller. */
	char buffer[16];
	/* Safety net for string routines. */
	char end;
};

/** Response: Device busy */
extern const char S400W_DEVICE_BUSY[];

/** Response: Battery low */
extern const char S400W_BATTERY_LOW[];

/** Response: No paper inserted */
extern const char S400W_NO_PAPER[];

/** Response: Paper inserted, ready to scan, calibrate, clean */
extern const char S400W_SCAN_READY[];

/** Response: Calibration has started */
extern const char S400W_CALIBRATE_GO[];

/** Response: Calibration has finished */
extern const char S400W_CALIBRATE_END[];

/** Response: Cleaning has started */
extern const char S400W_CLEAN_GO[];

/** Response: Cleaning has finished */
extern const char S400W_CLEAN_END[];

/** Response: Standard DPI selected */
extern const char S400W_DPI_STANDARD[];

/** Response: High DPI selected */
extern const char S400W_DPI_HIGH[];

/** Response: Scanning has started */
extern const char S400W_SCAN_GO[];

/** Response: Preview data in stream end marker */
extern const char S400W_PREVIEW_END[];

/** Response: JPEG size */
extern const char S400W_JPEG_SIZE[];

/** Artifical response: EOF */
extern const char S400W_EOF[];


/* Notifies clients of preview data, end of preview data, jpeg size, jpeg data, and end of jpeg data.
 * Note: S400W_EOF for jpeg is guaranteed to be called if jpeg size has been given (as to close file handles etc).
 *
 * data: either an array of bytes with preview or jpeg data, S400W_EOF if end of preview/jpeg data,
 *       or S400W_JPEG_SIZE for jpeg size.
 * length: number of bytes to read from data or jpeg size of data is S400W_JPEG_SIZE.
 *
 * Returns a value !=0 to abort receiving (negative values indicate error condition).
 */
typedef int (*s400w_receiveFunc)(const char* data, int length);


/* Initializes a S400W object */
extern void s400w_init(struct S400W* s400w, const char* hostname, int port, void (*message)(int, const char*));


/* Returns 1 if the given response is one of the defined response constants, except S400W_EOF.
 * Returns 0 otherwise. */
extern int s400w_is_known_response(const char* response);


/* Reads the scanner's version. Returns a version string, S400W_EOF, or NULL if timeout. */
extern const char* s400w_get_version(struct S400W* s400w);


/* Reads scanner's current status. Returns response, S400W_EOF, or NULL if timeout. */
extern const char* s400w_get_status(struct S400W* s400w);

/* Sets the scanner's resolution if supported (see S400W_MIN_SET_RESOLUTION_FW).
 * Supported DPI settings: 300 or 600.
 * Returns 0 if the resolution change was successful, -1 otherwise.
 */
extern int s400w_set_resolution(struct S400W* s400w, int dpi);


/* Executes the scanner's cleaning routine.
 * Returns S400W_CLEAN_END if sucessfully finished, any other otherwise,
 * including S400W_EOF or NULL for timeouts. S400W_NO_PAPER if cleaning sheet is not inserted.
 */
extern const char* s400w_clean(struct S400W* s400w);


/* Executes the scanner's calibration routine.
 * Returns S400W_CALIBRATE_END if sucessfully finished, any other otherwise
 * including S400W_EOF or NULL for timeouts. S400W_NO_PAPER if calibration sheet is not inserted.
 */
extern const char* s400w_calibrate(struct S400W* s400w);


/* Executes the scanner's scanning procedure.
 * resolution: resolution setting, or <code>0</code> if no setting is supported / desired
 * preview: callback handler for preview data, or <code>null</code> if no preview should be read
 * jpeg: callback handler for jpeg data
 * Returns S400W_SCAN_READY if sucessfully finished, any other response otherwise, including S400W_EOF or NULL for timeouts.
 */
extern const char* s400w_scan(struct S400W* s400w, int resolution, s400w_receiveFunc previewFunc, s400w_receiveFunc jpegFunc);


#endif
