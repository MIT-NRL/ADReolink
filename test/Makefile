TOP=..
include $(TOP)/configure/CONFIG

TESTPROD_HOST += ReolinkClientTest
ReolinkClientTest_SRCS += ReolinkClientTest.cpp
ReolinkClientTest_LIBS += ADReolink
ReolinkClientTest_LIBS += $(EPICS_BASE_IOC_LIBS)

TESTS += ReolinkClientTest

include $(TOP)/configure/RULES
