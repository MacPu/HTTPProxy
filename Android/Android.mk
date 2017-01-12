LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := HttpProxy
LOCAL_SRC_FILES := xx_HttpProxy.c \
    src/LAHTTPProxy.c \
    src/LASemaphore.c \
    src/LAThreadPool.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/HTTPProxy/

LOCAL_LDLIBS += -llog

include $(BUILD_SHARED_LIBRARY)
