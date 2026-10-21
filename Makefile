TOP = .
include $(TOP)/configure/CONFIG

DIRS := $(DIRS) configure
DIRS := $(DIRS) reolinkApp
DIRS := $(DIRS) reolinkIOCApp
reolinkIOCApp_DEPEND_DIRS += reolinkApp
DIRS := $(DIRS) iocBoot
iocBoot_DEPEND_DIRS += reolinkIOCApp
DIRS := $(DIRS) test
test_DEPEND_DIRS += reolinkApp

include $(TOP)/configure/RULES_TOP
