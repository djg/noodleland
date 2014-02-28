LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES :=   \
	Noodleland.cpp

LOCAL_MODULE := noodleland

LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := \
	libEGL      \
	libGLESv2   \
	libcutils   \
	libgui      \
	libhardware	\
	libui       \
	libutils    \

include $(BUILD_EXECUTABLE)