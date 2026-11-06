// clang-format off
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
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
static int default_autocrop = 1;
static int default_deskew = 1;
static int current_log_level = S400W_LOG_WARN;
static int default_max_bad_lines = ISCANFLY_DEFAULT_MAX_BAD_LINES;

/* Mutex and state to serialize access to the single-threaded S400W TCP port 23
 * stack and prevent concurrent operations from colliding with active scans
 */
static pthread_mutex_t iscanfly_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int scan_in_progress = 0;

/* Parse max_bad_lines integer value (>= 0) */
static int parse_max_bad_lines(const char *str) {
  if (!str || *str == '\0')
    return ISCANFLY_DEFAULT_MAX_BAD_LINES;

  while (*str == ' ' || *str == '\t' || *str == '=')
    str++;

  char *endptr = NULL;
  long val = strtol(str, &endptr, 10);
  if (endptr == str || val < 0)
    return ISCANFLY_DEFAULT_MAX_BAD_LINES;
  if (val > 10000)
    val = 10000;
  return (int)val;
}

/* Parse log level string (named or numeric) */
static int parse_log_level(const char *str) {
  if (!str || *str == '\0')
    return S400W_LOG_WARN;

  while (*str == ' ' || *str == '\t' || *str == '=')
    str++;

  if (strcasecmp(str, "off") == 0 || strcasecmp(str, "silent") == 0 ||
      strcasecmp(str, "none") == 0 || strcmp(str, "0") == 0) {
    return S400W_LOG_OFF;
  }
  if (strcasecmp(str, "error") == 0 || strcasecmp(str, "err") == 0 ||
      strcmp(str, "1") == 0) {
    return S400W_LOG_ERROR;
  }
  if (strcasecmp(str, "warn") == 0 || strcasecmp(str, "warning") == 0 ||
      strcmp(str, "2") == 0) {
    return S400W_LOG_WARN;
  }
  if (strcasecmp(str, "info") == 0 || strcmp(str, "3") == 0) {
    return S400W_LOG_INFO;
  }
  if (strcasecmp(str, "debug") == 0 || strcmp(str, "4") == 0) {
    return S400W_LOG_DEBUG;
  }
  if (strcasecmp(str, "trace") == 0 || strcasecmp(str, "verbose") == 0 ||
      strcmp(str, "5") == 0) {
    return S400W_LOG_TRACE;
  }
  return S400W_LOG_WARN;
}

/* Logging helper */
static void iscanfly_log(int level, const char *msg) {
  if (current_log_level == S400W_LOG_OFF || level > current_log_level)
    return;
  fprintf(stderr, "[iscanfly] %s\n", msg);
}

/* Custom error manager for libjpeg to prevent exit() on corrupt image
 * and track scanline-level corruption for configurable fault tolerance */
struct iscanfly_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  int in_scanline_loop;
  int corrupt_warnings;
};

static void iscanfly_jpeg_error_exit(j_common_ptr cinfo) {
  struct iscanfly_error_mgr *myerr = (struct iscanfly_error_mgr *)cinfo->err;
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  char msg[JMSG_LENGTH_MAX + 32];
  snprintf(msg, sizeof(msg), "JPEG error: %s", buffer);
  iscanfly_log(myerr->in_scanline_loop ? S400W_LOG_WARN : S400W_LOG_ERROR, msg);
  longjmp(myerr->setjmp_buffer, 1);
}

static void iscanfly_jpeg_emit_message(j_common_ptr cinfo, int msg_level) {
  struct iscanfly_error_mgr *myerr = (struct iscanfly_error_mgr *)cinfo->err;
  if (msg_level < 0) {
    /* Corrupt data warning (e.g., premature end of data segment / bad Huffman
     * code) */
    if (myerr->corrupt_warnings == 0) {
      char buffer[JMSG_LENGTH_MAX];
      (*cinfo->err->format_message)(cinfo, buffer);
      char msg[JMSG_LENGTH_MAX + 32];
      snprintf(msg, sizeof(msg), "JPEG data notice: %s", buffer);
      iscanfly_log(S400W_LOG_DEBUG, msg);
    }
    myerr->corrupt_warnings++;
  }
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
    const char *env_log = getenv("ISCANFLY_LOG_LEVEL");
    if (env_log && *env_log != '\0') {
      current_log_level = parse_log_level(env_log);
    }
    const char *env_bad = getenv("ISCANFLY_MAX_BAD_LINES");
    if (env_bad && *env_bad != '\0') {
      default_max_bad_lines = parse_max_bad_lines(env_bad);
    }
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

    if (strncasecmp(p, "autocrop", 8) == 0) {
      char *val = p + 8;
      while (*val == ' ' || *val == '\t' || *val == '=')
        val++;
      if (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0 ||
          strcmp(val, "0") == 0) {
        default_autocrop = 0;
      } else {
        default_autocrop = 1;
      }
      continue;
    }

    if (strncasecmp(p, "deskew", 6) == 0) {
      char *val = p + 6;
      while (*val == ' ' || *val == '\t' || *val == '=')
        val++;
      if (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0 ||
          strcmp(val, "0") == 0) {
        default_deskew = 0;
      } else {
        default_deskew = 1;
      }
      continue;
    }

    if (strncasecmp(p, "log_level", 9) == 0) {
      current_log_level = parse_log_level(p + 9);
      continue;
    }

    if (strncasecmp(p, "max_bad_lines", 13) == 0) {
      default_max_bad_lines = parse_max_bad_lines(p + 13);
      continue;
    }

    char host[200] = {0};
    int port = ISCANFLY_DEFAULT_PORT;
    char *colon = strchr(p, ':');
    if (colon) {
      *colon = '\0';
      snprintf(host, sizeof(host), "%.*s", (int)(sizeof(host) - 1), p);
      port = atoi(colon + 1);
      if (port <= 0)
        port = ISCANFLY_DEFAULT_PORT;
    } else {
      snprintf(host, sizeof(host), "%.*s", (int)(sizeof(host) - 1), p);
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

  const char *env_log = getenv("ISCANFLY_LOG_LEVEL");
  if (env_log && *env_log != '\0') {
    current_log_level = parse_log_level(env_log);
  }
  const char *env_bad = getenv("ISCANFLY_MAX_BAD_LINES");
  if (env_bad && *env_bad != '\0') {
    default_max_bad_lines = parse_max_bad_lines(env_bad);
  }
}

static void free_devices(void) {
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

SANE_Status sane_iscanfly_init(SANE_Int *version_code,
                               SANE_Auth_Callback authorize) {
  pthread_mutex_lock(&iscanfly_lock);
  if (version_code) {
    *version_code =
        SANE_VERSION_CODE(SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, 0);
  }
  auth_callback = authorize;
  free_devices();
  load_config();
  pthread_mutex_unlock(&iscanfly_lock);
  return SANE_STATUS_GOOD;
}

void sane_iscanfly_exit(void) {
  pthread_mutex_lock(&iscanfly_lock);
  free_devices();
  pthread_mutex_unlock(&iscanfly_lock);
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

  /* OPT_AUTOCROP */
  h->opt_val[OPT_AUTOCROP] = default_autocrop ? SANE_TRUE : SANE_FALSE;
  h->opt[OPT_AUTOCROP].name = "autocrop";
  h->opt[OPT_AUTOCROP].title = SANE_I18N("Auto Crop");
  h->opt[OPT_AUTOCROP].desc = SANE_I18N(
      "Automatically crop the scanned image to detected page boundary");
  h->opt[OPT_AUTOCROP].type = SANE_TYPE_BOOL;
  h->opt[OPT_AUTOCROP].unit = SANE_UNIT_NONE;
  h->opt[OPT_AUTOCROP].size = sizeof(SANE_Word);
  h->opt[OPT_AUTOCROP].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  /* OPT_DESKEW */
  h->opt_val[OPT_DESKEW] = default_deskew ? SANE_TRUE : SANE_FALSE;
  h->opt[OPT_DESKEW].name = "deskew";
  h->opt[OPT_DESKEW].title = SANE_I18N("Deskew");
  h->opt[OPT_DESKEW].desc =
      SANE_I18N("Automatically rotate and straighten skewed documents");
  h->opt[OPT_DESKEW].type = SANE_TYPE_BOOL;
  h->opt[OPT_DESKEW].unit = SANE_UNIT_NONE;
  h->opt[OPT_DESKEW].size = sizeof(SANE_Word);
  h->opt[OPT_DESKEW].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

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

  /* Wait for any active scan on this handle to finish before freeing memory */
  pthread_mutex_lock(&iscanfly_lock);
  if (h->jpeg_buf) {
    free(h->jpeg_buf);
    h->jpeg_buf = NULL;
  }
  if (h->raster_data) {
    free(h->raster_data);
    h->raster_data = NULL;
  }

  free(h);
  pthread_mutex_unlock(&iscanfly_lock);
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
    case OPT_AUTOCROP:
    case OPT_DESKEW:
    case OPT_PAPER_TIMEOUT:
      *(SANE_Word *)value = h->opt_val[option];
      break;
    case OPT_PAGE_LOADED:
      /* Do not open a live TCP connection to port 23 during option enumeration
       * or idle polling; report SANE_TRUE so frontends/AirSane proceed to
       * sane_start(), where paper-wait-timeout handles sheet insertion. */
      h->opt_val[OPT_PAGE_LOADED] = SANE_TRUE;
      *(SANE_Word *)value = SANE_TRUE;
      break;
    case OPT_BATTERY_LOW:
      *(SANE_Word *)value = h->opt_val[OPT_BATTERY_LOW];
      break;
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
    case OPT_AUTOCROP: {
      SANE_Word val = *(SANE_Word *)value;
      h->opt_val[OPT_AUTOCROP] = val ? SANE_TRUE : SANE_FALSE;
      break;
    }
    case OPT_DESKEW: {
      SANE_Word val = *(SANE_Word *)value;
      h->opt_val[OPT_DESKEW] = val ? SANE_TRUE : SANE_FALSE;
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
      if (scan_in_progress || h->scanning)
        return SANE_STATUS_DEVICE_BUSY;
      pthread_mutex_lock(&iscanfly_lock);
      const char *res = s400w_calibrate(&h->s400w);
      pthread_mutex_unlock(&iscanfly_lock);
      if (res == S400W_NO_PAPER)
        return SANE_STATUS_NO_DOCS;
      if (res != S400W_CALIBRATE_END)
        return SANE_STATUS_IO_ERROR;
      break;
    }
    case OPT_CLEAN: {
      if (scan_in_progress || h->scanning)
        return SANE_STATUS_DEVICE_BUSY;
      pthread_mutex_lock(&iscanfly_lock);
      const char *res = s400w_clean(&h->s400w);
      pthread_mutex_unlock(&iscanfly_lock);
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

/* Fast bilinear interpolation on RGB image:
 * src: width * height * 3
 * Evaluates coordinate (fx, fy) and returns RGB into out_rgb.
 */
static inline void sample_bilinear_rgb(const unsigned char *src, int width,
                                       int height, double fx, double fy,
                                       unsigned char *out_rgb) {
  int x0 = (int)floor(fx);
  int y0 = (int)floor(fy);
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  if (x0 < 0 || x0 >= width || y0 < 0 || y0 >= height) {
    out_rgb[0] = 255;
    out_rgb[1] = 255;
    out_rgb[2] = 255;
    return;
  }

  double dx = fx - x0;
  double dy = fy - y0;

  if (x1 >= width)
    x1 = width - 1;
  if (y1 >= height)
    y1 = height - 1;

  const unsigned char *p00 = src + (y0 * width + x0) * 3;
  const unsigned char *p10 = src + (y0 * width + x1) * 3;
  const unsigned char *p01 = src + (y1 * width + x0) * 3;
  const unsigned char *p11 = src + (y1 * width + x1) * 3;

  for (int c = 0; c < 3; c++) {
    double val = (1.0 - dx) * (1.0 - dy) * p00[c] + dx * (1.0 - dy) * p10[c] +
                 (1.0 - dx) * dy * p01[c] + dx * dy * p11[c];
    if (val < 0.0)
      val = 0.0;
    if (val > 255.0)
      val = 255.0;
    out_rgb[c] = (unsigned char)(val + 0.5);
  }
}

/* Fast deskew & auto-crop:
 * Analyzes RGB image buffer (*p_rgb, *p_w, *p_h)
 * Detects paper boundaries against dark feeder backing (~85 vs paper > 150)
 * Straightens page if deskew is enabled, crops flush to detected edges.
 */
static void process_autocrop_and_deskew(unsigned char **p_rgb, int *p_w,
                                        int *p_h, int enable_crop,
                                        int enable_deskew) {
  if (!enable_crop && !enable_deskew)
    return;

  int w = *p_w;
  int h = *p_h;
  const unsigned char *src = *p_rgb;

  /* Step 1: Detect paper edge coordinates across rows.
   * Feeder backing is dark gray (~70-95), paper is bright (>160).
   * We sample every 4 rows for speed and robustness.
   */
  const int sample_step = 4;
  int max_samples = (h + sample_step - 1) / sample_step;
  int *edge_y = (int *)malloc(max_samples * sizeof(int));
  int *edge_left = (int *)malloc(max_samples * sizeof(int));
  int *edge_right = (int *)malloc(max_samples * sizeof(int));
  if (!edge_y || !edge_left || !edge_right) {
    if (edge_y)
      free(edge_y);
    if (edge_left)
      free(edge_left);
    if (edge_right)
      free(edge_right);
    return;
  }

  int num_edges = 0;
  for (int y = 0; y < h; y += sample_step) {
    int left = -1;
    int right = -1;
    const unsigned char *row = src + y * w * 3;
    for (int x = 0; x < w; x++) {
      int gray =
          (77 * row[x * 3] + 150 * row[x * 3 + 1] + 29 * row[x * 3 + 2]) >> 8;
      if (gray > 140) {
        if (left == -1)
          left = x;
        right = x;
      }
    }
    /* Only count as paper line if detected paper width is at least 60 pixels */
    if (left != -1 && (right - left) >= 60) {
      edge_y[num_edges] = y;
      edge_left[num_edges] = left;
      edge_right[num_edges] = right;
      num_edges++;
    }
  }

  /* If no paper or virtually whole image matches, nothing to crop */
  if (num_edges < 10) {
    free(edge_y);
    free(edge_left);
    free(edge_right);
    return;
  }

  /* Step 2: Linear regression on left edge to find skew angle */
  double angle = 0.0;
  if (enable_deskew && num_edges >= 20) {
    double sum_y = 0.0, sum_x = 0.0;
    for (int i = 0; i < num_edges; i++) {
      sum_y += edge_y[i];
      sum_x += edge_left[i];
    }
    double mean_y = sum_y / num_edges;
    double mean_x = sum_x / num_edges;

    double num = 0.0, den = 0.0;
    for (int i = 0; i < num_edges; i++) {
      double dy = edge_y[i] - mean_y;
      double dx = edge_left[i] - mean_x;
      num += dy * dx;
      den += dy * dy;
    }

    if (den > 1e-4) {
      double slope = num / den; /* dx / dy */
      double est_angle = atan(slope);
      /* Restrict deskew correction to +/- 15 degrees (~0.26 radians) */
      if (fabs(est_angle) <= 0.2618 && fabs(est_angle) >= 0.003) {
        angle = est_angle;
      }
    }
  }

  /* Step 3: Determine bounding box */
  int min_y = edge_y[0];
  int max_y = edge_y[num_edges - 1];

  int min_x = w - 1;
  int max_x = 0;
  for (int i = 0; i < num_edges; i++) {
    if (edge_left[i] < min_x)
      min_x = edge_left[i];
    if (edge_right[i] > max_x)
      max_x = edge_right[i];
  }

  free(edge_y);
  free(edge_left);
  free(edge_right);

  /* Check minimum document dimensions (e.g., at least 50x50 pixels) */
  if ((max_x - min_x) < 50 || (max_y - min_y) < 50)
    return;

  if (!enable_crop && fabs(angle) < 0.003)
    return;

  /* If only deskewing without crop */
  int crop_x0 = enable_crop ? min_x : 0;
  int crop_y0 = enable_crop ? min_y : 0;
  int crop_w = enable_crop ? (max_x - min_x + 1) : w;
  int crop_h = enable_crop ? (max_y - min_y + 1) : h;

  unsigned char *out_buf = (unsigned char *)malloc((size_t)crop_w * crop_h * 3);
  if (!out_buf)
    return;

  double cos_a = cos(angle);
  double sin_a = sin(angle);
  double cx = crop_x0 + crop_w / 2.0;
  double cy = crop_y0 + crop_h / 2.0;

  for (int y = 0; y < crop_h; y++) {
    double dy = (crop_y0 + y) - cy;
    unsigned char *dst_row = out_buf + y * crop_w * 3;
    for (int x = 0; x < crop_w; x++) {
      double dx = (crop_x0 + x) - cx;
      /* Rotate by -angle around center to un-skew */
      double src_x = cx + (dx * cos_a - dy * sin_a);
      double src_y = cy + (dx * sin_a + dy * cos_a);
      sample_bilinear_rgb(src, w, h, src_x, src_y, dst_row + x * 3);
    }
  }

  free(*p_rgb);
  *p_rgb = out_buf;
  *p_w = crop_w;
  *p_h = crop_h;
}

/* Decode JPEG buffer and process resolution scaling, autocrop, deskew, and bit
 * depth conversion */
static SANE_Status decode_and_process_jpeg(IScanFly_Handle *h) {
  if (!h->jpeg_buf || h->jpeg_size == 0)
    return SANE_STATUS_IO_ERROR;

  /* Allow runtime environment override on each scan */
  const char *env_bad = getenv("ISCANFLY_MAX_BAD_LINES");
  int max_bad_lines = (env_bad && *env_bad != '\0')
                          ? parse_max_bad_lines(env_bad)
                          : default_max_bad_lines;

  struct jpeg_decompress_struct cinfo;
  struct iscanfly_error_mgr jerr;
  memset(&jerr, 0, sizeof(jerr));

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = iscanfly_jpeg_error_exit;
  jerr.pub.emit_message = iscanfly_jpeg_emit_message;

  unsigned char *volatile rgb_buf = NULL;
  JSAMPROW volatile row_buf = NULL;
  volatile size_t line = 0;
  volatile int bad_lines = 0;
  volatile int width = 0;
  volatile int height = 0;

  if (setjmp(jerr.setjmp_buffer)) {
    if (!jerr.in_scanline_loop || !rgb_buf || width <= 0 || height <= 0) {
      if (row_buf)
        free(row_buf);
      if (rgb_buf)
        free(rgb_buf);
      jpeg_destroy_decompress(&cinfo);
      return SANE_STATUS_IO_ERROR;
    }
    /* Error occurred inside scanline decoding loop:
     * all remaining scanlines from 'line' to 'height - 1' are halted/bad */
    size_t remaining_lines = (size_t)height > line ? (size_t)height - line : 1;
    bad_lines += (int)remaining_lines;
    while (line < (size_t)height) {
      if (line > 0) {
        memcpy(rgb_buf + (line * (size_t)width * 3),
               rgb_buf + ((line - 1) * (size_t)width * 3), (size_t)width * 3);
      } else {
        memset(rgb_buf, 255, (size_t)width * 3);
      }
      line++;
    }
    if (row_buf) {
      free(row_buf);
      row_buf = NULL;
    }
    jpeg_destroy_decompress(&cinfo);
    goto scanlines_done;
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

  width = (int)cinfo.output_width;
  height = (int)cinfo.output_height;
  if (width <= 0 || height <= 0) {
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_IO_ERROR;
  }

  /* Decode full image to RGB buffer */
  rgb_buf = (unsigned char *)malloc((size_t)width * (size_t)height * 3);
  if (!rgb_buf) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_NO_MEM;
  }

  row_buf = (JSAMPROW)malloc((size_t)width * 3);
  if (!row_buf) {
    free(rgb_buf);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return SANE_STATUS_NO_MEM;
  }

  jerr.in_scanline_loop = 1;
  while (cinfo.output_scanline < cinfo.output_height && line < (size_t)height) {
    int prev_warnings = jerr.corrupt_warnings;
    JSAMPROW row_pointer[1];
    row_pointer[0] = row_buf;
    JDIMENSION nread = jpeg_read_scanlines(&cinfo, row_pointer, 1);
    if (nread == 0 || jerr.corrupt_warnings > prev_warnings) {
      /* Scanline halted or had corrupt Huffman data:
       * If corrupt_warnings triggered during an 8-line MCU block refill,
       * all remaining lines in the stream from this point on are affected. */
      size_t remaining_lines =
          (size_t)height > line ? (size_t)height - line : 1;
      bad_lines += (int)remaining_lines;
      while (line < (size_t)height) {
        if (line > 0) {
          memcpy(rgb_buf + (line * (size_t)width * 3),
                 rgb_buf + ((line - 1) * (size_t)width * 3), (size_t)width * 3);
        } else {
          memset(rgb_buf + (line * (size_t)width * 3), 255, (size_t)width * 3);
        }
        line++;
      }
      break;
    } else {
      memcpy(rgb_buf + (line * (size_t)width * 3), row_buf, (size_t)width * 3);
      line++;
    }
  }
  jerr.in_scanline_loop = 0;

  if (row_buf) {
    free(row_buf);
    row_buf = NULL;
  }
  if (bad_lines == 0 && cinfo.output_scanline == cinfo.output_height) {
    jpeg_finish_decompress(&cinfo);
  }
  jpeg_destroy_decompress(&cinfo);

scanlines_done:;
  int final_width = width;
  int final_height = height;
  unsigned char *final_rgb_buf = rgb_buf;

  if (bad_lines > max_bad_lines) {
    char err_msg[160];
    snprintf(err_msg, sizeof(err_msg),
             "Scan failed: %d scanline(s) corrupted/halted (exceeds fault "
             "tolerance of %d)",
             bad_lines, max_bad_lines);
    iscanfly_log(S400W_LOG_ERROR, err_msg);
    free(final_rgb_buf);
    return SANE_STATUS_IO_ERROR;
  } else if (bad_lines > 0) {
    char warn_msg[160];
    snprintf(warn_msg, sizeof(warn_msg),
             "Warning: recovered %d corrupted/halted scanline(s) by "
             "duplicating previous line (fault tolerance: %d)",
             bad_lines, max_bad_lines);
    iscanfly_log(S400W_LOG_WARN, warn_msg);
  }

  int is_color = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_COLOR) == 0);
  int is_lineart = (strcmp(h->scan_mode, SANE_VALUE_SCAN_MODE_LINEART) == 0);
  int threshold = h->opt_val[OPT_THRESHOLD];

  /* Process Auto-Crop and Deskew on decoded RGB raster prior to
   * thresholding/conversion */
  int enable_crop = (h->opt_val[OPT_AUTOCROP] == SANE_TRUE);
  int enable_deskew = (h->opt_val[OPT_DESKEW] == SANE_TRUE);
  process_autocrop_and_deskew(&final_rgb_buf, &final_width, &final_height,
                              enable_crop, enable_deskew);
  rgb_buf = final_rgb_buf;
  width = final_width;
  height = final_height;

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
    free(rgb_buf);
    return SANE_STATUS_NO_MEM;
  }

  if (is_lineart) {
    /* For Lineart: convert RGB to grayscale first, then apply
     * adaptive Bradley thresholding (or user-specified manual threshold) */
    unsigned char *gray_buf = (unsigned char *)malloc((size_t)width * height);
    if (!gray_buf) {
      free(rgb_buf);
      free(h->raster_data);
      h->raster_data = NULL;
      return SANE_STATUS_NO_MEM;
    }

    for (int y = 0; y < height; y++) {
      const unsigned char *src_row = rgb_buf + (y * width * 3);
      unsigned char *row_gray = gray_buf + (y * width);
      for (int x = 0; x < width; x++) {
        unsigned int r = src_row[x * 3 + 0];
        unsigned int g = src_row[x * 3 + 1];
        unsigned int b = src_row[x * 3 + 2];
        row_gray[x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
      }
    }
    free(rgb_buf);

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
  } else if (is_color) {
    memcpy(h->raster_data, rgb_buf, total_raster_bytes);
    free(rgb_buf);
  } else {
    /* 8-bit Grayscale */
    for (int y = 0; y < height; y++) {
      const unsigned char *src_row = rgb_buf + (y * width * 3);
      unsigned char *dst_row = h->raster_data + (y * width);
      for (int x = 0; x < width; x++) {
        unsigned int r = src_row[x * 3 + 0];
        unsigned int g = src_row[x * 3 + 1];
        unsigned int b = src_row[x * 3 + 2];
        dst_row[x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
      }
    }
    free(rgb_buf);
  }

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

  if (h->scanning || scan_in_progress)
    return SANE_STATUS_DEVICE_BUSY;

  pthread_mutex_lock(&iscanfly_lock);
  if (h->scanning || scan_in_progress) {
    pthread_mutex_unlock(&iscanfly_lock);
    return SANE_STATUS_DEVICE_BUSY;
  }
  scan_in_progress = 1;
  h->cancelled = SANE_FALSE;

  int hw_dpi = 0;
  if (h->fw_version >= S400W_MIN_SET_RESOLUTION_FW) {
    hw_dpi = (h->opt_val[OPT_RESOLUTION] == 600) ? 600 : 300;
  }

  /* Perform status polling and scan over a single unified TCP connection inside
   * s400w_scan() so we never open and close a separate status socket right
   * before starting a scan */
  h->s400w.paper_wait_sec = h->opt_val[OPT_PAPER_TIMEOUT];
  h->s400w.cancelled = (const volatile int *)&h->cancelled;

  JpegContext ctx = {h, 0};
  active_jpeg_ctx = &ctx;
  h->jpeg_size = 0;

  const char *scan_res =
      s400w_scan(&h->s400w, hw_dpi, NULL, active_jpeg_receiver);
  active_jpeg_ctx = NULL;

  h->opt_val[OPT_BATTERY_LOW] =
      (scan_res == S400W_BATTERY_LOW) ? SANE_TRUE : SANE_FALSE;

  if (h->cancelled) {
    scan_in_progress = 0;
    pthread_mutex_unlock(&iscanfly_lock);
    return SANE_STATUS_CANCELLED;
  }

  if (scan_res == S400W_NO_PAPER) {
    scan_in_progress = 0;
    pthread_mutex_unlock(&iscanfly_lock);
    return SANE_STATUS_NO_DOCS;
  }

  if (ctx.err != 0 || h->jpeg_size == 0) {
    scan_in_progress = 0;
    pthread_mutex_unlock(&iscanfly_lock);
    return SANE_STATUS_IO_ERROR;
  }

  SANE_Status dec_st = decode_and_process_jpeg(h);
  if (dec_st != SANE_STATUS_GOOD) {
    scan_in_progress = 0;
    pthread_mutex_unlock(&iscanfly_lock);
    return dec_st;
  }

  h->scanning = SANE_TRUE;
  scan_in_progress = 0;
  pthread_mutex_unlock(&iscanfly_lock);
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
