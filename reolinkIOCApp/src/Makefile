TOP=../..
include $(TOP)/configure/CONFIG

PROD_NAME = reolinkIOC
PROD_IOC = $(PROD_NAME)
DBD_NAME = $(PROD_NAME)

DBD += $(DBD_NAME).dbd
$(DBD_NAME)_DBD += ADReolinkSupport.dbd

PROD_SRCS_DEFAULT += $(DBD_NAME)_registerRecordDeviceDriver.cpp reolinkIOCMain.cpp
PROD_SRCS_vxWorks += $(DBD_NAME)_registerRecordDeviceDriver.cpp

PROD_LIBS += ADReolink
PROD_SYS_LIBS_Linux += avformat avcodec avutil swscale curl

# NDPlugin was built against ADSupport's bundled libjpeg.  Put that exact
# implementation ahead of the system library to avoid a libjpeg ABI collision.
jpeg_DIR = $(ADSUPPORT)/lib/$(T_A)
xml2_DIR = $(ADSUPPORT)/lib/$(T_A)
USR_LDFLAGS_Linux += -L$(ADSUPPORT)/lib/$(T_A)
USR_LDFLAGS_Linux += -Wl,-rpath,$(ADSUPPORT)/lib/$(T_A)

# This IOC needs only the PVA and JPEG portions of the standard plugin set.
WITH_PVA = YES
WITH_QSRV = NO
WITH_JPEG = YES
WITH_NETCDF = NO
WITH_GRAPHICSMAGICK = NO
WITH_TIFF = NO
WITH_NEXUS = NO
WITH_HDF5 = NO
WITH_BITSHUFFLE = NO
WITH_BLOSC = NO
WITH_SZIP = NO

include $(ADCORE)/ADApp/commonDriverMakefile
include $(TOP)/configure/RULES
