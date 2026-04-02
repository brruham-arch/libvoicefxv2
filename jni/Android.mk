LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := voicefx
LOCAL_SRC_FILES := voicefx.cpp
LOCAL_CPPFLAGS  := -O2 -ffast-math -fPIC -std=c++11
LOCAL_LDLIBS    := -lm -llog -ldl
LOCAL_ARM_MODE  := arm

include $(BUILD_SHARED_LIBRARY)
