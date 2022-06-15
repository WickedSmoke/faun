LOCAL_PATH := .

# Path to Ogg & Vorbis libraries.
#USR=/tmp/usr

include $(CLEAR_VARS)
LOCAL_MODULE := libfaun
LOCAL_CFLAGS := -DUSE_SFX_GEN -DUSE_FLAC
LOCAL_C_INCLUDES := support $(USR)/include
#LOCAL_EXPORT_LDLIBS := -lz
LOCAL_SRC_FILES := faun.c support/tmsg.c
include $(BUILD_STATIC_LIBRARY)
