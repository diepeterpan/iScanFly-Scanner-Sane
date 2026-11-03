CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -fPIC -I./include
LDFLAGS ?= -shared -Wl,-soname,libsane-iscanfly.so.1 -ljpeg

DIST_DIR ?= dist
SANE_BACKEND_NAME = iscanfly
LIB_MAJOR = 1
LIB_NAME = libsane-$(SANE_BACKEND_NAME).so.$(LIB_MAJOR).0.0
SONAME = libsane-$(SANE_BACKEND_NAME).so.$(LIB_MAJOR)
SOFILE = libsane-$(SANE_BACKEND_NAME).so

SRCS = src/s400w.c src/iscanfly-sane.c
OBJS = $(SRCS:.c=.o)

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	ln -sf $(LIB_NAME) $(SONAME)
	ln -sf $(SONAME) $(SOFILE)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

dist: $(LIB_NAME)
	mkdir -p $(DIST_DIR)
	cp -a $(LIB_NAME) $(DIST_DIR)/
	cp -a $(SONAME) $(DIST_DIR)/
	cp -a $(SOFILE) $(DIST_DIR)/
	cp conf/iscanfly.conf $(DIST_DIR)/
	cp scripts/install.sh $(DIST_DIR)/
	chmod +x $(DIST_DIR)/install.sh
	@echo "=============================================="
	@echo "Distribution package ready in $(DIST_DIR)/"
	@echo "=============================================="

clean:
	rm -f $(OBJS) $(LIB_NAME) $(SONAME) $(SOFILE)
	rm -rf $(DIST_DIR)
	$(MAKE) -C tools clean || true

.PHONY: all dist clean

