// clang-format off
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <jpeglib.h>
// clang-format on

#include "iscanfly.h"

#define BACKEND_NAME iscanfly

/* Static string constants for SANE options */
static const SANE_String_Const source_list[] = {"ADF", NULL};

static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR, SANE_VALUE_SCAN_MODE_GRAY,
    SANE_VALUE_SCAN_MODE_LINEART, NULL};

/* Supported resolutions in DPI */
static const SANE_Word res_list_all[] = {4, /* 4 entries */
                                         75, 150, 300, 600};

static const SANE_Word res_list_lowfw[] = {3, /* 3 entries */
                                           75, 150, 300};

static const SANE_Range threshold_range = {
    0,   /* min */
    255, /* max */
    1    /* quant */
};

static const SANE_Range timeout_range = {
    0,   /* min (0 = don't wait) */
    120, /* max (120 seconds) */
    1    /* quant */
};

/* Global state */
static SANE_Int num_devices = 0;
static IScanFly_Device *device_list = NULL;
static const SANE_Device **sane_dev_list = NULL;
static SANE_Auth_Callback auth_callback = NULL;

/* Logging helper */
static void iscanfly_log(int level, const char *msg) {
  (void)level;
  fprintf(stderr, "[iscanfly] %s\n", msg);
}

/* Custom error manager for libjpeg to prevent exit() on corrupt image */
struct iscanfly_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

static void iscanfly_jpeg_error_exit(j_common_ptr cinfo) {
  struct iscanfly_error_mgr *myerr = (struct iscanfly_error_mgr *)cinfo->err;
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  fprintf(stderr, "[iscanfly] JPEG error: %s\n", buffer);
  longjmp(myerr->setjmp_buffer, 1);
}

/* JPEG receiver context struct */
typedef struct {
  IScanFly_Handle *h;
  int err;
} JpegContext;

static JpegContext *active_jpeg_ctx = NULL;

static int active_jpeg_receiver(const char *data, int length) {
  if (!active_jpeg_ctx || !active_jpeg_ctx->h)
    return -1;
  IScanFly_Handle *h = active_jpeg_ctx->h;

  if (data == S400W_EOF) {
    return 0;
  } else if (data == S400W_JPEG_SIZE) {
    h->jpeg_size = 0;
    if (length > 0) {
      h->jpeg_alloc = length + 4096;
      h->jpeg_buf = (unsigned char *)realloc(h->jpeg_buf, h->jpeg_alloc);
      if (!h->jpeg_buf) {
        active_jpeg_ctx->err = 1;
        return -1;
      }
    }
    return 0;
  }

  if (h->cancelled) {
    return -1;
  }

  if (h->jpeg_size + length > h->jpeg_alloc) {
    h->jpeg_alloc = h->jpeg_size + length + 65536;
    h->jpeg_buf = (unsigned char *)realloc(h->jpeg_buf, h->jpeg_alloc);
    if (!h->jpeg_buf) {
      active_jpeg_ctx->err = 1;
      return -1;
    }
  }

  memcpy(h->jpeg_buf + h->jpeg_size, data, length);
  h->jpeg_size += length;
  return 0;
}

/* Configuration file loader */
static void load_config(void) {
  const char *config_paths[] = {"/etc/sane.d/iscanfly.conf",
                                "/usr/local/etc/sane.d/iscanfly.conf",
                                "conf/iscanfly.conf", NULL};

  FILE *fp = NULL;
  for (int i = 0; config_paths[i]; i++) {
    fp = fopen(config_paths[i], "r");
    if (fp)
      break;
  }

  if (!fp) {
    /* Default fallback device if no conf file present */
    IScanFly_Device *dev =
        (IScanFly_Device *)calloc(1, sizeof(IScanFly_Device));
    if (!dev)
      return;
    dev->sane.name = strdup("net:192.168.18.33:23");
    dev->sane.vendor = "Mustek";
    dev->sane.model = "iScan Air / S400W";
    dev->sane.type = "document feeder";
    strncpy(dev->hostname, "192.168.18.33", sizeof(dev->hostname) - 1);
    dev->port = ISCANFLY_DEFAULT_PORT;
    dev->next = device_list;
    device_list = dev;
    num_devices++;
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), fp)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
      continue;

    char *end = p + strlen(p) - 1;
    while (end > p &&
           (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
      *end = '\0';
      end--;
    }

    char host[200] = {0};
    int port = ISCANFLY_DEFAULT_PORT;
    char *colon = strchr(p, ':');
    if (colon) {
      *colon = '\0';
      strncpy(host, p, sizeof(host) - 1);
      port = atoi(colon + 1);
      if (port <= 0)
        port = ISCANFLY_DEFAULT_PORT;
    } else {
      strncpy(host, p, sizeof(host) - 1);
    }

    IScanFly_Device *dev =
        (IScanFly_Device *)calloc(1, sizeof(IScanFly_Device));
    if (!dev)
      continue;
    char namebuf[256];
    snprintf(namebuf, sizeof(namebuf), "net:%s:%d", host, port);
    dev->sane.name = strdup(namebuf);
    dev->sane.vendor = "Mustek";
    dev->sane.model = "iScan Air / S400W";
    dev->sane.type = "document feeder";
    strncpy(dev->hostname, host, sizeof(dev->hostname) - 1);
    dev->port = port;

    dev->next = device_list;
    device_list = dev;
    num_devices++;
  }
  fclose(fp);
}

SANE_Status sane_iscanfly_init(SANE_Int *version_code,
                               SANE_Auth_Callback authorize) {
  if (version_code) {
    *version_code =
        SANE_VERSION_CODE(SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, 0);
  }
  auth_callback = authorize;
  load_config();
  return SANE_STATUS_GOOD;
}

void sane_iscanfly_exit(void) {
  IScanFly_Device *cur = device_list;
  while (cur) {
    IScanFly_Device *next = cur->next;
    if (cur->sane.name)
      free((void *)cur->sane.name);
    free(cur);
    cur = next;
  }
  device_list = NULL;
  num_devices = 0;

  if (sane_dev_list) {
    free(sane_dev_list);
    sane_dev_list = NULL;
  }
}

SANE_Status sane_iscanfly_get_devices(const SANE_Device ***dev_list,
                                      SANE_Bool local_only) {
  (void)local_only;
  if (sane_dev_list)
    free(sane_dev_list);

  sane_dev_list =
      (const SANE_Device **)calloc(num_devices + 1, sizeof(SANE_Device *));
  if (!sane_dev_list)
    return SANE_STATUS_NO_MEM;

  IScanFly_Device *cur = device_list;
  int i = 0;
  while (cur) {
    sane_dev_list[i++] = &cur->sane;
    cur = cur->next;
  }
  sane_dev_list[i] = NULL;
  *dev_list = sane_dev_list;
  return SANE_STATUS_GOOD;
}

static void init_options(IScanFly_Handle *h) {
  memset(h->opt, 0, sizeof(h->opt));

  /* OPT_NUM_OPTS */
  h->opt[OPT_NUM_OPTS].name = SANE_NAME_NUM_OPTIONS;
  h->opt[OPT_NUM_OPTS].title = SANE_TITLE_NUM_OPTIONS;
  h->opt[OPT_NUM_OPTS].desc = SANE_DESC_NUM_OPTIONS;
  h->opt[OPT_NUM_OPTS].type = SANE_TYPE_INT;
  h->opt[OPT_NUM_OPTS].unit = SANE_UNIT_NONE;
  h->opt[OPT_NUM_OPTS].size = sizeof(SANE_Word);
  h->opt[OPT_NUM_OPTS].cap = SANE_CAP_SOFT_DETECT;
  h->opt_val[OPT_NUM_OPTS] = NUM_OPTIONS;

  /* OPT_GROUP_STANDARD */
  h->opt[OPT_GROUP_STANDARD].title = SANE_I18N("Standard");
  h->opt[OPT_GROUP_STANDARD].desc = SANE_I18N("Standard scanner options");
  h->opt[OPT_GROUP_STANDARD].type = SANE_TYPE_GROUP;
  h->opt[OPT_GROUP_STANDARD].cap = 0;

  /* OPT_SCAN_SOURCE */
  strncpy(h->scan_source, "ADF", sizeof(h->scan_source) - 1);
  h->opt[OPT_SCAN_SOURCE].name = SANE_NAME_SCAN_SOURCE;
  h->opt[OPT_SCAN_SOURCE].title = SANE_TITLE_SCAN_SOURCE;
  h->opt[OPT_SCAN_SOURCE].desc = SANE_DESC_SCAN_SOURCE;
  h->opt[OPT_SCAN_SOURCE].type = SANE_TYPE_STRING;
  h->opt[OPT_SCAN_SOURCE].unit = SANE_UNIT_NONE;
  h->opt[OPT_SCAN_SOURCE].size = sizeof(h->scan_source);
  h->opt[OPT_SCAN_SOURCE].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
  h->opt[OPT_SCAN_SOURCE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
  h->opt[OPT_SCAN_SOURCE].constraint.string_list = source_list;

  /* OPT_SCAN_MODE */
  strncpy(h->scan_mode, SANE_VALUE_SCAN_MODE_COLOR, sizeof(h->scan_mode) - 1);
  h->opt[OPT_SCAN_MODE].name = SANE_NAME_SCAN_MODE;
  h->opt[OPT_SCAN_MODE].title = SANE_TITLE_SCAN_MODE;
  h->opt[OPT_SCAN_MODE].desc = SANE_DESC_SCAN_MODE;
  h->opt[OPT_SCAN_MODE].type = SANE_TYPE_STRING;
  h->opt[OPT_SCAN_MODE].unit = SANE_UNIT_NONE;
  h->opt[OPT_SCAN_MODE].size = sizeof(h->scan_mode);
  h->opt[OPT_SCAN_MODE].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
  h->opt[OPT_SCAN_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
  h->opt[OPT_SCAN_MODE].constraint.string_list = mode_list;

  /* OPT_RESOLUTION */
  h->opt_val[OPT_RESOLUTION] = 300;
  h->opt[OPT_RESOLUTION].name = SANE_NAME_SCAN_RESOLUTION;
  h->opt[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
  h->opt[OPT_RESOLUTION].desc = SANE_DESC_SCAN_RESOLUTION;
  h->opt[OPT_RESOLUTION].type = SANE_TYPE_INT;
  h->opt[OPT_RESOLUTION].unit = SANE_UNIT_DPI;
  h->opt[OPT_RESOLUTION].size = sizeof(SANE_Word);
  h->opt[OPT_RESOLUTION].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
  h->opt[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
  if (h->fw_version >= S400W_MIN_SET_RESOLUTION_FW) {
    h->opt[OPT_RESOLUTION].constraint.word_list = res_list_all;
  } else {
    h->opt[OPT_RESOLUTION].constraint.word_list = res_list_lowfw;
  }

  /* OPT_GROUP_ENHANCEMENT */
  h->opt[OPT_GROUP_ENHANCEMENT].title = SANE_I18N("Enhancement");
  h->opt[OPT_GROUP_ENHANCEMENT].desc = SANE_I18N("Image enhancement options");
  h->opt[OPT_GROUP_ENHANCEMENT].type = SANE_TYPE_GROUP;
  h->opt[OPT_GROUP_ENHANCEMENT].cap = 0;

  /* OPT_THRESHOLD */
  h->opt_val[OPT_THRESHOLD] = ISCANFLY_DEFAULT_THRESHOLD;
  h->opt[OPT_THRESHOLD].name = SANE_NAME_THRESHOLD;
  h->opt[OPT_THRESHOLD].title = SANE_TITLE_THRESHOLD;
  h->opt[OPT_THRESHOLD].desc = SANE_DESC_THRESHOLD;
  h->opt[OPT_THRESHOLD].type = SANE_TYPE_INT;
  h->opt[OPT_THRESHOLD].unit = SANE_UNIT_NONE;
  h->opt[OPT_THRESHOLD].size = sizeof(SANE_Word);
  h->opt[OPT_THRESHOLD].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
  h->opt[OPT_THRESHOLD].constraint_type = SANE_CONSTRAINT_RANGE;
  h->opt[OPT_THRESHOLD].constraint.range = &threshold_range;

  /* OPT_PAPER_TIMEOUT */
  h->opt_val[OPT_PAPER_TIMEOUT] = ISCANFLY_DEFAULT_PAPER_TIMEOUT_SEC;
  h->opt[OPT_PAPER_TIMEOUT].name = "paper-wait-timeout";
  h->opt[OPT_PAPER_TIMEOUT].title = SANE_I18N("Paper Wait Timeout");
  h->opt[OPT_PAPER_TIMEOUT].desc =
      SANE_I18N("Seconds to wait for sheet insertion before failing (0 = fail "
                "immediately)");
  h->opt[OPT_PAPER_TIMEOUT].type = SANE_TYPE_INT;
  h->opt[OPT_PAPER_TIMEOUT].unit = SANE_UNIT_NONE;
  h->opt[OPT_PAPER_TIMEOUT].size = sizeof(SANE_Word);
  h->opt[OPT_PAPER_TIMEOUT].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
  h->opt[OPT_PAPER_TIMEOUT].constraint_type = SANE_CONSTRAINT_RANGE;
  h->opt[OPT_PAPER_TIMEOUT].constraint.range = &timeout_range;

  /* OPT_GROUP_SENSORS */
  h->opt[OPT_GROUP_SENSORS].title = SANE_I18N("Sensors");
  h->opt[OPT_GROUP_SENSORS].desc = SANE_I18N("Scanner hardware sensors");
  h->opt[OPT_GROUP_SENSORS].type = SANE_TYPE_GROUP;
  h->opt[OPT_GROUP_SENSORS].cap = 0;

  /* OPT_PAGE_LOADED */
  h->opt_val[OPT_PAGE_LOADED] = SANE_FALSE;
  h->opt[OPT_PAGE_LOADED].name = SANE_NAME_PAGE_LOADED;
  h->opt[OPT_PAGE_LOADED].title = SANE_TITLE_PAGE_LOADED;
  h->opt[OPT_PAGE_LOADED].desc = SANE_DESC_PAGE_LOADED;
  h->opt[OPT_PAGE_LOADED].type = SANE_TYPE_BOOL;
  h->opt[OPT_PAGE_LOADED].unit = SANE_UNIT_NONE;
  h->opt[OPT_PAGE_LOADED].size = sizeof(SANE_Word);
  h->opt[OPT_PAGE_LOADED].cap = SANE_CAP_SOFT_DETECT;

  /* OPT_BATTERY_LOW */
  h->opt_val[OPT_BATTERY_LOW] = SANE_FALSE;
  h->opt[OPT_BATTERY_LOW].name = "battery-low";
  h->opt[OPT_BATTERY_LOW].title = SANE_I18N("Battery Low");
  h->opt[OPT_BATTERY_LOW].desc =
      SANE_I18N("Indicates whether scanner battery is low");
  h->opt[OPT_BATTERY_LOW].type = SANE_TYPE_BOOL;
  h->opt[OPT_BATTERY_LOW].unit = SANE_UNIT_NONE;
  h->opt[OPT_BATTERY_LOW].size = sizeof(SANE_Word);
  h->opt[OPT_BATTERY_LOW].cap = SANE_CAP_SOFT_DETECT;

  /* OPT_GROUP_MAINTENANCE */
  h->opt[OPT_GROUP_MAINTENANCE].title = SANE_I18N("Maintenance");
  h->opt[OPT_GROUP_MAINTENANCE].desc = SANE_I18N("Scanner maintenance actions");
  h->opt[OPT_GROUP_MAINTENANCE].type = SANE_TYPE_GROUP;
  h->opt[OPT_GROUP_MAINTENANCE].cap = 0;

  /* OPT_CALIBRATE */
  h->opt[OPT_CALIBRATE].name = "calibrate";
  h->opt[OPT_CALIBRATE].title = SANE_I18N("Calibrate Scanner");
  h->opt[OPT_CALIBRATE].desc =
      SANE_I18N("Trigger scanner calibration with calibration sheet inserted");
  h->opt[OPT_CALIBRATE].type = SANE_TYPE_BUTTON;
  h->opt[OPT_CALIBRATE].unit = SANE_UNIT_NONE;
  h->opt[OPT_CALIBRATE].size = 0;
  h->opt[OPT_CALIBRATE].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  /* OPT_CLEAN */
  h->opt[OPT_CLEAN].name = "clean";
  h->opt[OPT_CLEAN].title = SANE_I18N("Clean Scanner");
  h->opt[OPT_CLEAN].desc =
      SANE_I18N("Trigger scanner cleaning with cleaning sheet inserted");
  h->opt[OPT_CLEAN].type = SANE_TYPE_BUTTON;
  h->opt[OPT_CLEAN].unit = SANE_UNIT_NONE;
  h->opt[OPT_CLEAN].size = 0;
  h->opt[OPT_CLEAN].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
}

SANE_Status sane_iscanfly_open(SANE_String_Const devicename,
                               SANE_Handle *handle) {
  if (!handle)
    return SANE_STATUS_INVAL;

  IScanFly_Device *chosen = NULL;
  if (!devicename || devicename[0] == '\0') {
    chosen = device_list;
  } else {
    for (IScanFly_Device *d = device_list; d; d = d->next) {
      if (strcmp(d->sane.name, devicename) == 0 ||
          strcmp(d->hostname, devicename) == 0) {
        chosen = d;
        break;
      }
    }
  }

  if (!chosen)
    return SANE_STATUS_INVAL;

  IScanFly_Handle *h = (IScanFly_Handle *)calloc(1, sizeof(IScanFly_Handle));
  if (!h)
    return SANE_STATUS_NO_MEM;

  h->dev = chosen;
  s400w_init(&h->s400w, chosen->hostname, chosen->port, iscanfly_log);

  /* Default assumed capable; avoids hanging if scanner is offline/unreachable
   * during open */
  h->fw_version = 26;

  init_options(h);
  *handle = (SANE_Handle)h;
  return SANE_STATUS_GOOD;
}

void sane_iscanfly_close(SANE_Handle handle) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h)
    return;

  if (h->scanning) {
    sane_iscanfly_cancel(handle);
  }

  if (h->jpeg_buf) {
    free(h->jpeg_buf);
    h->jpeg_buf = NULL;
  }
  if (h->raster_data) {
    free(h->raster_data);
    h->raster_data = NULL;
  }

  free(h);
}

const SANE_Option_Descriptor *
sane_iscanfly_get_option_descriptor(SANE_Handle handle, SANE_Int option) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h || option < 0 || option >= NUM_OPTIONS)
    return NULL;
  return &h->opt[option];
}

SANE_Status sane_iscanfly_control_option(SANE_Handle handle, SANE_Int option,
                                         SANE_Action action, void *value,
                                         SANE_Int *info) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h || option < 0 || option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;

  if (info)
    *info = 0;

  switch (action) {
  case SANE_ACTION_GET_VALUE:
    switch (option) {
    case OPT_NUM_OPTS:
      *(SANE_Word *)value = NUM_OPTIONS;
      break;
    case OPT_SCAN_SOURCE:
      strcpy((char *)value, h->scan_source);
      break;
    case OPT_SCAN_MODE:
      strcpy((char *)value, h->scan_mode);
      break;
    case OPT_RESOLUTION:
    case OPT_THRESHOLD:
    case OPT_PAPER_TIMEOUT:
      *(SANE_Word *)value = h->opt_val[option];
      break;
    case OPT_PAGE_LOADED: {
      const char *st = s400w_get_status(&h->s400w);
      h->opt_val[OPT_PAGE_LOADED] =
          (st == S400W_SCAN_READY) ? SANE_TRUE : SANE_FALSE;
      *(SANE_Word *)value = h->opt_val[OPT_PAGE_LOADED];
      break;
    }
    case OPT_BATTERY_LOW: {
      const char *st = s400w_get_status(&h->s400w);
      h->opt_val[OPT_BATTERY_LOW] =
          (st == S400W_BATTERY_LOW) ? SANE_TRUE : SANE_FALSE;
      *(SANE_Word *)value = h->opt_val[OPT_BATTERY_LOW];
      break;
    }
    default:
      return SANE_STATUS_INVAL;
    }
    break;

  case SANE_ACTION_SET_VALUE:
    if (h->scanning)
      return SANE_STATUS_DEVICE_BUSY;

    switch (option) {
    case OPT_SCAN_SOURCE: {
      const char *req = (const char *)value;
      if (strcmp(req, "ADF") == 0 ||
          strcmp(req, "Automatic Document Feeder") == 0) {
        strncpy(h->scan_source, "ADF", sizeof(h->scan_source) - 1);
      } else {
        return SANE_STATUS_INVAL;
      }
      break;
    }
    case OPT_SCAN_MODE: {
      const char *req = (const char *)value;
      if (strcmp(req, SANE_VALUE_SCAN_MODE_COLOR) == 0 ||
          strcmp(req, SANE_VALUE_SCAN_MODE_GRAY) == 0 ||
          strcmp(req, SANE_VALUE_SCAN_MODE_LINEART) == 0) {
        strncpy(h->scan_mode, req, sizeof(h->scan_mode) - 1);
        if (info)
          *info |= SANE_INFO_RELOAD_PARAMS;
      } else {
        return SANE_STATUS_INVAL;
      }
      break;
    }
    case OPT_RESOLUTION: {
      SANE_Word dpi = *(SANE_Word *)value;
      if (dpi == 75 || dpi == 150 || dpi == 300 ||
          (dpi == 600 && h->fw_version >= S400W_MIN_SET_RESOLUTION_FW)) {
        h->opt_val[OPT_RESOLUTION] = dpi;
        if (info)
          *info |= SANE_INFO_RELOAD_PARAMS;
      } else {
        return SANE_STATUS_INVAL;
      }
      break;
    }
    case OPT_THRESHOLD: {
      SANE_Word val = *(SANE_Word *)value;
      if (val < 0)
        val = 0;
      if (val > 255)
        val = 255;
      h->opt_val[OPT_THRESHOLD] = val;
      break;
    }
    case OPT_PAPER_TIMEOUT: {
      SANE_Word val = *(SANE_Word *)value;
      if (val < 0)
        val = 0;
      if (val > 120)
        val = 120;
      h->opt_val[OPT_PAPER_TIMEOUT] = val;
      break;
    }
    case OPT_CALIBRATE: {
      const char *res = s400w_calibrate(&h->s400w);
      if (res == S400W_NO_PAPER)
        return SANE_STATUS_NO_DOCS;
      if (res != S400W_CALIBRATE_END)
        return SANE_STATUS_IO_ERROR;
      break;
    }
    case OPT_CLEAN: {
      const char *res = s400w_clean(&h->s400w);
      if (res == S400W_NO_PAPER)
        return SANE_STATUS_NO_DOCS;
      if (res != S400W_CLEAN_END)
        return SANE_STATUS_IO_ERROR;
      break;
    }
    default:
      return SANE_STATUS_INVAL;
    }
    break;

  case SANE_ACTION_SET_AUTO:
    return SANE_STATUS_UNSUPPORTED;

  default:
    return SANE_STATUS_INVAL;
  }

  return SANE_STATUS_GOOD;
}

SANE_Status sane_iscanfly_get_parameters(SANE_Handle handle,
                                         SANE_Parameters *params) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h || !params)
    return SANE_STATUS_INVAL;

  if (h->scanning && h->raster_data) {
    *params = h->params;
    return SANE_STATUS_GOOD;
  }

  /* Estimated parameters prior to scan based on standard A4 width */
  int dpi = h->opt_val[OPT_RESOLUTION];
  int estimated_pixels = (int)(8.27 * dpi);
  int is_color = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0);
  int is_lineart = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_LINEART) == 0);

  params->last_frame = SANE_TRUE;
  params->lines = -1; /* Variable length feed until page exits roller */
  params->pixels_per_line = estimated_pixels;

  if (is_color) {
    params->format = SANE_FRAME_RGB;
    params->depth = 8;
    params->bytes_per_line = estimated_pixels * 3;
  } else if (is_lineart) {
    params->format = SANE_FRAME_GRAY;
    params->depth = 1;
    params->bytes_per_line = (estimated_pixels + 7) / 8;
  } else {
    /* Gray */
    params->format = SANE_FRAME_GRAY;
    params->depth = 8;
    params->bytes_per_line = estimated_pixels;
  }

  return SANE_STATUS_GOOD;
}

/* Decode JPEG buffer and process resolution scaling and bit depth conversion */
static SANE_Status decode_and_process_jpeg(IScanFly_Handle *h) {
  if (!h->jpeg_buf || h->jpeg_size == 0)
    return SANE_STATUS_IO_ERROR;

  struct jpeg_decompress_struct cinfo;
  struct iscanfly_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = iscanfly_jpeg_error_exit;

  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_IO_ERROR;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, h->jpeg_buf, h->jpeg_size);

  int rc = jpeg_read_header(&cinfo, TRUE);
  if (rc != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_IO_ERROR;
  }

  int dpi = h->opt_val[OPT_RESOLUTION];
  if (dpi == 150) {
    cinfo.scale_num = 1;
    cinfo.scale_denom = 2;
  } else if (dpi == 75) {
    cinfo.scale_num = 1;
    cinfo.scale_denom = 4;
  }

  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  int width = cinfo.output_width;
  int height = cinfo.output_height;
  int is_color = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0);
  int is_lineart = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_LINEART) == 0);
  int threshold = h->opt_val[OPT_THRESHOLD];

  size_t out_bytes_per_line;
  if (is_color) {
    out_bytes_per_line = width * 3;
  } else if (is_lineart) {
    out_bytes_per_line = (width + 7) / 8;
  } else {
    out_bytes_per_line = width;
  }

  size_t total_raster_bytes = out_bytes_per_line * height;
  if (h->raster_data)
    free(h->raster_data);
  h->raster_data = (unsigned char *)malloc(total_raster_bytes);
  if (!h->raster_data) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_NO_MEM;
  }

  if (is_lineart) {
    /* For Lineart: read the entire image as grayscale first, then apply
     * adaptive Bradley thresholding (or user-specified manual threshold) */
    unsigned char *gray_buf = (unsigned char *)malloc(width * height);
    if (!gray_buf) {
      free(h->raster_data);
      h->raster_data = NULL;
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return SANE_STATUS_NO_MEM;
    }

    JSAMPROW row_pointer[1];
    row_pointer[0] = (JSAMPROW)malloc(width * 3);
    if (!row_pointer[0]) {
      free(gray_buf);
      free(h->raster_data);
      h->raster_data = NULL;
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return SANE_STATUS_NO_MEM;
    }

    size_t line = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
      jpeg_read_scanlines(&cinfo, row_pointer, 1);
      unsigned char *row_gray = gray_buf + (line * width);
      for (int x = 0; x < width; x++) {
        unsigned int r = row_pointer[0][x * 3 + 0];
        unsigned int g = row_pointer[0][x * 3 + 1];
        unsigned int b = row_pointer[0][x * 3 + 2];
        row_gray[x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
      }
      line++;
    }
    free(row_pointer[0]);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    /* Binarization:
     * If threshold != ISCANFLY_DEFAULT_THRESHOLD (128), user explicitly set it.
     * Otherwise, use adaptive Bradley-Roth local thresholding.
     */
    if (threshold != ISCANFLY_DEFAULT_THRESHOLD) {
      /* Fixed manual threshold (SANE 1-bit: 1=Black/ink, 0=White/paper) */
      for (int y = 0; y < height; y++) {
        unsigned char *dst = h->raster_data + (y * out_bytes_per_line);
        const unsigned char *src = gray_buf + (y * width);
        memset(dst, 0, out_bytes_per_line);
        for (int x = 0; x < width; x++) {
          if (src[x] < (unsigned char)threshold) {
            dst[x / 8] |= (1 << (7 - (x % 8)));
          }
        }
      }
    } else {
      /* Bradley-Roth adaptive thresholding using an integral image.
       * Window size S = width / 16, sensitivity T = 12% darker than local mean.
       */
      int S = width / 16;
      if (S < 8)
        S = 8;
      const int T = 12;

      /* Allocate integral image: (width + 1) * (height + 1) of uint64_t */
      uint64_t *integral =
          (uint64_t *)calloc((width + 1) * (height + 1), sizeof(uint64_t));
      if (integral) {
        for (int y = 0; y < height; y++) {
          uint64_t row_sum = 0;
          for (int x = 0; x < width; x++) {
            row_sum += gray_buf[y * width + x];
            integral[(y + 1) * (width + 1) + (x + 1)] =
                integral[y * (width + 1) + (x + 1)] + row_sum;
          }
        }

        for (int y = 0; y < height; y++) {
          unsigned char *dst = h->raster_data + (y * out_bytes_per_line);
          const unsigned char *src = gray_buf + (y * width);
          memset(dst, 0, out_bytes_per_line);

          int y1 = y - S;
          if (y1 < 0)
            y1 = 0;
          int y2 = y + S;
          if (y2 >= height)
            y2 = height - 1;

          for (int x = 0; x < width; x++) {
            int x1 = x - S;
            if (x1 < 0)
              x1 = 0;
            int x2 = x + S;
            if (x2 >= width)
              x2 = width - 1;

            uint64_t count = (uint64_t)(x2 - x1 + 1) * (y2 - y1 + 1);
            uint64_t sum = integral[(y2 + 1) * (width + 1) + (x2 + 1)] -
                           integral[y1 * (width + 1) + (x2 + 1)] -
                           integral[(y2 + 1) * (width + 1) + x1] +
                           integral[y1 * (width + 1) + x1];

            uint64_t val = src[x];
            /* If pixel is T% darker than surrounding local neighborhood, it is
             * ink/black */
            if (val * 100 * count < sum * (100 - T)) {
              dst[x / 8] |= (1 << (7 - (x % 8)));
            }
          }
        }
        free(integral);
      } else {
        /* Fallback to simple mean if memory allocation fails */
        for (int y = 0; y < height; y++) {
          unsigned char *dst = h->raster_data + (y * out_bytes_per_line);
          const unsigned char *src = gray_buf + (y * width);
          memset(dst, 0, out_bytes_per_line);
          for (int x = 0; x < width; x++) {
            if (src[x] < 128) {
              dst[x / 8] |= (1 << (7 - (x % 8)));
            }
          }
        }
      }
    }
    free(gray_buf);
  } else {
    /* Color or 8-bit Grayscale */
    JSAMPROW row_pointer[1];
    row_pointer[0] = (JSAMPROW)malloc(width * 3);
    if (!row_pointer[0]) {
      free(h->raster_data);
      h->raster_data = NULL;
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return SANE_STATUS_NO_MEM;
    }

    size_t line = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
      jpeg_read_scanlines(&cinfo, row_pointer, 1);
      unsigned char *dst = h->raster_data + (line * out_bytes_per_line);

      if (is_color) {
        memcpy(dst, row_pointer[0], width * 3);
      } else {
        for (int x = 0; x < width; x++) {
          unsigned int r = row_pointer[0][x * 3 + 0];
          unsigned int g = row_pointer[0][x * 3 + 1];
          unsigned int b = row_pointer[0][x * 3 + 2];
          dst[x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
        }
      }
      line++;
    }

    free(row_pointer[0]);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
  }
  jpeg_destroy_decompress(&cinfo);

  h->raster_size = total_raster_bytes;
  h->raster_offset = 0;

  h->params.last_frame = SANE_TRUE;
  h->params.pixels_per_line = width;
  h->params.lines = height;
  h->params.bytes_per_line = out_bytes_per_line;

  if (is_color) {
    h->params.format = SANE_FRAME_RGB;
    h->params.depth = 8;
  } else if (is_lineart) {
    h->params.format = SANE_FRAME_GRAY;
    h->params.depth = 1;
  } else {
    h->params.format = SANE_FRAME_GRAY;
    h->params.depth = 8;
  }

  return SANE_STATUS_GOOD;
}

SANE_Status sane_iscanfly_start(SANE_Handle handle) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h)
    return SANE_STATUS_INVAL;

  if (h->scanning)
    return SANE_STATUS_DEVICE_BUSY;
  h->cancelled = SANE_FALSE;

  int timeout_sec = h->opt_val[OPT_PAPER_TIMEOUT];
  int elapsed = 0;
  const char *st = NULL;

  while (1) {
    st = s400w_get_status(&h->s400w);
    if (st == S400W_SCAN_READY) {
      break;
    } else if (st == S400W_BATTERY_LOW) {
      fprintf(stderr, "[iscanfly] Warning: battery low\n");
    }

    if (timeout_sec <= 0 || elapsed >= timeout_sec) {
      return (st == S400W_NO_PAPER) ? SANE_STATUS_NO_DOCS
                                    : SANE_STATUS_IO_ERROR;
    }

    if (h->cancelled)
      return SANE_STATUS_CANCELLED;

    sleep(1);
    elapsed++;
  }

  int hw_dpi = (h->opt_val[OPT_RESOLUTION] == 600) ? 600 : 300;

  if (h->fw_version >= S400W_MIN_SET_RESOLUTION_FW) {
    s400w_set_resolution(&h->s400w, hw_dpi);
  }

  JpegContext ctx = {h, 0};
  active_jpeg_ctx = &ctx;
  h->jpeg_size = 0;

  s400w_scan(&h->s400w, hw_dpi, NULL, active_jpeg_receiver);
  active_jpeg_ctx = NULL;

  if (h->cancelled) {
    return SANE_STATUS_CANCELLED;
  }

  if (ctx.err != 0 || h->jpeg_size == 0) {
    return SANE_STATUS_IO_ERROR;
  }

  SANE_Status dec_st = decode_and_process_jpeg(h);
  if (dec_st != SANE_STATUS_GOOD) {
    return dec_st;
  }

  h->scanning = SANE_TRUE;
  return SANE_STATUS_GOOD;
}

SANE_Status sane_iscanfly_read(SANE_Handle handle, SANE_Byte *data,
                               SANE_Int max_length, SANE_Int *length) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h || !data || !length)
    return SANE_STATUS_INVAL;

  *length = 0;

  if (!h->scanning || !h->raster_data) {
    return SANE_STATUS_INVAL;
  }

  if (h->cancelled) {
    h->scanning = SANE_FALSE;
    return SANE_STATUS_CANCELLED;
  }

  size_t remaining = h->raster_size - h->raster_offset;
  if (remaining == 0) {
    h->scanning = SANE_FALSE;
    return SANE_STATUS_EOF;
  }

  size_t chunk =
      (remaining > (size_t)max_length) ? (size_t)max_length : remaining;
  memcpy(data, h->raster_data + h->raster_offset, chunk);
  h->raster_offset += chunk;
  *length = (SANE_Int)chunk;

  return SANE_STATUS_GOOD;
}

void sane_iscanfly_cancel(SANE_Handle handle) {
  IScanFly_Handle *h = (IScanFly_Handle *)handle;
  if (!h)
    return;
  h->cancelled = SANE_TRUE;
  h->scanning = SANE_FALSE;
}

SANE_Status sane_iscanfly_set_io_mode(SANE_Handle handle,
                                      SANE_Bool non_blocking) {
  (void)handle;
  return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status sane_iscanfly_get_select_fd(SANE_Handle handle, SANE_Int *fd) {
  (void)handle;
  (void)fd;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const sane_iscanfly_strstatus(SANE_Status status) {
  switch (status) {
  case SANE_STATUS_GOOD:
    return "Success";
  case SANE_STATUS_UNSUPPORTED:
    return "Operation not supported";
  case SANE_STATUS_CANCELLED:
    return "Operation cancelled";
  case SANE_STATUS_DEVICE_BUSY:
    return "Device busy";
  case SANE_STATUS_INVAL:
    return "Data or argument invalid";
  case SANE_STATUS_EOF:
    return "End of file reached";
  case SANE_STATUS_JAMMED:
    return "Document feeder jammed";
  case SANE_STATUS_NO_DOCS:
    return "Document feeder out of documents";
  case SANE_STATUS_COVER_OPEN:
    return "Scanner cover open";
  case SANE_STATUS_IO_ERROR:
    return "Error during device I/O";
  case SANE_STATUS_NO_MEM:
    return "Out of memory";
  case SANE_STATUS_ACCESS_DENIED:
    return "Access denied";
  default:
    return "Unknown SANE status";
  }
}
