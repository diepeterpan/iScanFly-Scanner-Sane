#ifndef ISCANFLY_SANE_H_INCLUDED
#define ISCANFLY_SANE_H_INCLUDED

#include "s400w.h"
#include <sane/sane.h>
#include <sane/saneopts.h>

#define ISCANFLY_DEFAULT_PORT 23
#define ISCANFLY_DEFAULT_PAPER_TIMEOUT_SEC 15
#define ISCANFLY_DEFAULT_THRESHOLD 128

enum {
  OPT_NUM_OPTS = 0,
  OPT_GROUP_STANDARD,
  OPT_SCAN_SOURCE,
  OPT_SCAN_MODE,
  OPT_RESOLUTION,
  OPT_GROUP_ENHANCEMENT,
  OPT_THRESHOLD,
  OPT_AUTOCROP,
  OPT_DESKEW,
  OPT_PAPER_TIMEOUT,
  OPT_GROUP_SENSORS,
  OPT_PAGE_LOADED,
  OPT_BATTERY_LOW,
  OPT_GROUP_MAINTENANCE,
  OPT_CALIBRATE,
  OPT_CLEAN,
  NUM_OPTIONS
};

typedef struct IScanFly_Device {
  struct IScanFly_Device *next;
  SANE_Device sane;
  char hostname[256];
  int port;
} IScanFly_Device;

typedef struct IScanFly_Handle {
  IScanFly_Device *dev;
  struct S400W s400w;
  int fw_version;

  /* Options */
  SANE_Option_Descriptor opt[NUM_OPTIONS];
  SANE_Word opt_val[NUM_OPTIONS];

  /* Dynamic string option buffers */
  char scan_source[32];
  char scan_mode[32];

  /* Current scan state */
  SANE_Bool scanning;
  SANE_Bool cancelled;

  /* Image buffer & decoded raster state */
  unsigned char *jpeg_buf;
  size_t jpeg_size;
  size_t jpeg_alloc;

  /* Decoded raster buffer */
  unsigned char *raster_data;
  size_t raster_size;
  size_t raster_offset;

  SANE_Parameters params;
} IScanFly_Handle;

/* Function prototypes for sane_iscanfly_* backend */
SANE_Status sane_iscanfly_init(SANE_Int *version_code,
                               SANE_Auth_Callback authorize);
void sane_iscanfly_exit(void);
SANE_Status sane_iscanfly_get_devices(const SANE_Device ***dev_list,
                                      SANE_Bool local_only);
SANE_Status sane_iscanfly_open(SANE_String_Const devicename,
                               SANE_Handle *handle);
void sane_iscanfly_close(SANE_Handle handle);
const SANE_Option_Descriptor *
sane_iscanfly_get_option_descriptor(SANE_Handle handle, SANE_Int option);
SANE_Status sane_iscanfly_control_option(SANE_Handle handle, SANE_Int option,
                                         SANE_Action action, void *value,
                                         SANE_Int *info);
SANE_Status sane_iscanfly_get_parameters(SANE_Handle handle,
                                         SANE_Parameters *params);
SANE_Status sane_iscanfly_start(SANE_Handle handle);
SANE_Status sane_iscanfly_read(SANE_Handle handle, SANE_Byte *data,
                               SANE_Int max_length, SANE_Int *length);
void sane_iscanfly_cancel(SANE_Handle handle);
SANE_Status sane_iscanfly_set_io_mode(SANE_Handle handle,
                                      SANE_Bool non_blocking);
SANE_Status sane_iscanfly_get_select_fd(SANE_Handle handle, SANE_Int *fd);
SANE_String_Const sane_iscanfly_strstatus(SANE_Status status);

#endif
