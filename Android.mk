LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := BoostFPS
LOCAL_SRC_FILES  := main.cpp
# Salin folder include/ dari repo AML (berisi amlmod.h dan mod/) ke jni/include/
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CPPFLAGS   := -O2 -std=c++17 -DNDEBUG
LOCAL_LDLIBS     := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
