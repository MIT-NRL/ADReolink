TOP=../..
include $(TOP)/configure/CONFIG

INC += ADReolink.h
INC += ReolinkClient.h

LIBRARY_IOC = ADReolink
LIB_SRCS += ADReolink.cpp
LIB_SRCS += ReolinkClient.cpp

DBD += ADReolinkSupport.dbd

LIB_SYS_LIBS_Linux += avformat
LIB_SYS_LIBS_Linux += avcodec
LIB_SYS_LIBS_Linux += avutil
LIB_SYS_LIBS_Linux += swscale
LIB_SYS_LIBS_Linux += curl

# Fedora installs FFmpeg's public headers below /usr/include/ffmpeg.
# Keep this in the module so IOC applications do not need local CXX flags.
USR_INCLUDES += -I/usr/include/ffmpeg

include $(ADCORE)/ADApp/commonLibraryMakefile

include $(TOP)/configure/RULES
