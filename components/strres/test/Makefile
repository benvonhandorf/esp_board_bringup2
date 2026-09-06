# Host test for the .sr reader. Plain gcc: strres.c touches no ESP-IDF API
# beyond esp_err_t, and takes paths rather than mounting anything, so it
# compiles off target against small stubs and the test exercises the real
# source, not a copy.
#
#   make -C components/strres/test
#
# The fixtures are compiled by the real strres_compile.py first, so a change to
# the on-disk format or to the id assignment fails here rather than on hardware.
#
COMP   := $(abspath $(CURDIR)/..)
STUBS  := $(CURDIR)/stubs
GEN    := $(CURDIR)/gen
IMAGE  := $(CURDIR)/fixtures-build

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror \
          -DSTRRES_HOST_TEST \
          -I$(COMP)/include -I$(COMP)/src -I$(STUBS) -I$(GEN)

# Sourcing ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin on PATH,
# and their `ld` cannot link a host binary. -B names where to find it.
LDFLAGS := -B/usr/bin

GENERATED := $(GEN)/strres_ids.h $(GEN)/strres_catalog.c
SRCS := test_strres.c $(STUBS)/diag_capture.c $(COMP)/src/strres.c $(GEN)/strres_catalog.c

.PHONY: all run clean
all: run

$(GENERATED): $(wildcard fixtures/*/*.json) $(COMP)/tool/strres_compile.py
	@mkdir -p $(GEN) $(IMAGE)
	python3 $(COMP)/tool/strres_compile.py fixtures \
	    --out-fs      $(IMAGE) \
	    --out-header  $(GEN)/strres_ids.h \
	    --out-catalog $(GEN)/strres_catalog.c \
	    --out-map     $(GEN)/strres_ids.txt

test_strres: $(GENERATED) $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_strres
	./test_strres

clean:
	rm -rf test_strres $(GEN) $(IMAGE)
