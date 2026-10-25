# compas-sdtool: the player's library code as a command-line tool for a
# computer (see docs/SDTOOL.md). Included by the top-level Makefile, which
# supplies the vendored library variables and flags.
#
#   make sdtool                  native build  -> build_sdtool/compas-sdtool
#   make sdtool SDTOOL_TARGET=windows
#                                MinGW-w64 build -> build_sdtool_windows/compas-sdtool.exe
#
# SDTOOL_TARGET=windows expects x86_64-w64-mingw32-gcc and a mingw cmake
# toolchain (for example the Fedora mingw64-* packages' `mingw64-cmake`
# wrapper, or CMAKE_TOOLCHAIN_FILE in SDTOOL_CMAKE_FLAGS).

SDTOOL_TARGET ?= native
SDTOOL_SRC_DIR = src/tools/sdtool

ifeq ($(SDTOOL_TARGET),windows)
SDTOOL_DIR = build_sdtool_windows
SDTOOL_CC ?= x86_64-w64-mingw32-gcc
SDTOOL_EXE = .exe
SDTOOL_PLATFORM_CFLAGS = -include $(SDTOOL_SRC_DIR)/compat.h -I$(SDTOOL_SRC_DIR)/compat \
                         -D_FILE_OFFSET_BITS=64 -D__USE_MINGW_ANSI_STDIO=1
SDTOOL_PLATFORM_SRCS = $(SDTOOL_SRC_DIR)/compat.c $(SDTOOL_SRC_DIR)/compat_dirs.c
SDTOOL_PLATFORM_LIBS = -static -lbcrypt -lpthread
SDTOOL_CMAKE_FLAGS = -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(SDTOOL_CC) $(SDTOOL_CMAKE_EXTRA)
else
SDTOOL_DIR = build_sdtool
SDTOOL_CC ?= $(CC)
SDTOOL_EXE =
# macOS lacks two names the library uses (compat.h); Linux needs no shim.
SDTOOL_PLATFORM_CFLAGS = $(if $(filter Darwin,$(shell uname -s)),-include $(SDTOOL_SRC_DIR)/compat.h)
SDTOOL_PLATFORM_SRCS =
SDTOOL_PLATFORM_LIBS = -lpthread
SDTOOL_CMAKE_FLAGS = -DCMAKE_C_COMPILER=$(SDTOOL_CC)
endif

SDTOOL_BIN = $(SDTOOL_DIR)/compas-sdtool$(SDTOOL_EXE)

SDTOOL_OWN_SRCS = $(addprefix $(SDTOOL_SRC_DIR)/,main.c json_out.c root.c database.c scan.c art.c stubs.c lvgl_shim.c dr_libs_impl.c) \
                  $(SDTOOL_PLATFORM_SRCS)
# The player's own library code, unchanged: tagcache and metadata_db for the
# database, albumart and cover_decode for the covers, metadata for the tags.
SDTOOL_PLAYER_SRCS = $(addprefix src/library/,tagcache.c metadata_db.c metadata.c albumart.c path_cache.c \
                       remote_state.c subsonic_saved_servers.c cue_parser.c sacd_iso.c lyrics.c cover_decode.c \
                       artwork_coordinator.c library_walk.c library_scan_tags.c) \
                     src/core/db_log.c src/core/utf8_util.c \
                     $(addprefix src/audio/,ogg_probe.c opusfile_alloc.c wavpack_decoder.c caf_decoder.c)
SDTOOL_LVGL_SRCS = $(LVGL_DIR)/src/libs/lodepng/lodepng.c $(LVGL_DIR)/src/libs/tjpgd/tjpgd.c
SDTOOL_MBEDTLS_SRCS = $(MBEDTLS_DIR)/library/base64.c $(MBEDTLS_DIR)/library/platform_util.c

# HOST_BUILD drops the player's SD-mount check and its device-only paths; the
# card root is chosen at run time (storage_paths.h).
SDTOOL_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 -DCOMPAS_RUNTIME_SD_ROOT=1 -DARTWORK_FALLBACK_MEM_BYTES=1073741824U \
                -I$(TINFL_DIR) -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS -I$(SDTOOL_SRC_DIR) \
                $(SDTOOL_PLATFORM_CFLAGS)

SDTOOL_OBJS = $(SDTOOL_OWN_SRCS:%.c=$(SDTOOL_DIR)/%.o) $(SDTOOL_PLAYER_SRCS:%.c=$(SDTOOL_DIR)/%.o) \
              $(SDTOOL_LVGL_SRCS:%.c=$(SDTOOL_DIR)/%.o) $(SDTOOL_MBEDTLS_SRCS:%.c=$(SDTOOL_DIR)/%.o) \
              $(CJSON_SRCS:%.c=$(SDTOOL_DIR)/%.o) $(STB_VORBIS_SRCS:%.c=$(SDTOOL_DIR)/%.o) \
              $(TINFL_SRCS:%.c=$(SDTOOL_DIR)/%.o) $(OPUS_SRCS:%.c=$(SDTOOL_DIR)/%.o) \
              $(OGG_SRCS:%.c=$(SDTOOL_DIR)/%.o) $(OPUSFILE_SRCS:%.c=$(SDTOOL_DIR)/%.o) \
              $(JPEG_SRCS:%.c=$(SDTOOL_DIR)/%.o)
SDTOOL_LIBS = $(SDTOOL_DIR)/libwavpack/libwavpack.a $(SDTOOL_DIR)/libsndfile/libsndfile.a

.PHONY: sdtool
sdtool: $(SDTOOL_BIN)

$(SDTOOL_BIN): $(SDTOOL_OBJS) $(SDTOOL_LIBS)
	$(SDTOOL_CC) -o $@ $(SDTOOL_OBJS) $(SDTOOL_LIBS) $(SDTOOL_PLATFORM_LIBS) -lm

# Vendored libraries keep the flags the player builds them with.
$(SDTOOL_DIR)/$(OPUS_DIR)/%.o: $(OPUS_DIR)/%.c $(OPUS_WARNING_STAMP)
	@mkdir -p $(dir $@)
	$(SDTOOL_CC) $(OPUS_CFLAGS) -c $< -o $@
$(SDTOOL_DIR)/$(OGG_DIR)/%.o: $(OGG_DIR)/%.c libogg_vendor_config/ogg/config_types.h
	@mkdir -p $(dir $@)
	$(SDTOOL_CC) $(OGG_CFLAGS) -c $< -o $@
$(SDTOOL_DIR)/$(OPUSFILE_DIR)/%.o: $(OPUSFILE_DIR)/%.c libogg_vendor_config/ogg/config_types.h $(OPUSFILE_PATCH_STAMP) $(OPUSFILE_WARN_STAMP)
	@mkdir -p $(dir $@)
	$(SDTOOL_CC) $(OPUSFILE_CFLAGS) -c $< -o $@
$(SDTOOL_DIR)/$(JPEG_DIR)/%.o: $(JPEG_DIR)/%.c
	@mkdir -p $(dir $@)
	$(SDTOOL_CC) $(JPEG_CFLAGS) -c $< -o $@
$(SDTOOL_DIR)/%.o: %.c $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(SDTOOL_CC) $(SDTOOL_CFLAGS) -c $< -o $@

$(SDTOOL_DIR)/libwavpack/libwavpack.a: $(WAVPACK_DIR)/CMakeLists.txt $(WAVPACK_CMAKE_STAMP)
	cmake -S $(WAVPACK_DIR) -B $(dir $@) $(WAVPACK_OPTIONS) $(SDTOOL_CMAKE_FLAGS)
	+cmake --build $(dir $@) --target wavpack
$(SDTOOL_DIR)/libsndfile/libsndfile.a: $(SNDFILE_DIR)/CMakeLists.txt $(SNDFILE_CMAKE_STAMP)
	cmake -S $(SNDFILE_DIR) -B $(dir $@) $(SNDFILE_OPTIONS) $(SDTOOL_CMAKE_FLAGS)
	+cmake --build $(dir $@) --target sndfile

-include $(SDTOOL_OBJS:.o=.d)

.PHONY: test-sdtool
# End to end on a generated card: scan, art, no-write rescans, the file-time rule.
test-sdtool: $(SDTOOL_BIN)
	sh tests/sdtool/run_sdtool_test.sh $(SDTOOL_BIN)
