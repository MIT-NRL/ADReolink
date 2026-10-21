#!../../bin/linux-x86_64/reolinkIOC

< envPaths

errlogInit(20000)

# Separate server/search ports let this IOC coexist reliably with 4dh4 on one host.
# Automatic beaconing is deliberately disabled; clients must name the IOC host.
epicsEnvSet("EPICS_CA_SERVER_PORT", "$(REOLINK_CA_SERVER_PORT)")
epicsEnvSet("EPICS_CAS_SERVER_PORT", "$(REOLINK_CA_SERVER_PORT)")
epicsEnvSet("EPICS_CAS_INTF_ADDR_LIST", "$(REOLINK_SERVER_INTERFACE)")
epicsEnvSet("EPICS_CAS_AUTO_BEACON_ADDR_LIST", "NO")
epicsEnvSet("EPICS_PVAS_SERVER_PORT", "$(REOLINK_PVA_SERVER_PORT)")
epicsEnvSet("EPICS_PVAS_BROADCAST_PORT", "$(REOLINK_PVA_BROADCAST_PORT)")
epicsEnvSet("EPICS_PVAS_INTF_ADDR_LIST", "$(REOLINK_SERVER_INTERFACE)")
epicsEnvSet("EPICS_PVAS_AUTO_BEACON_ADDR_LIST", "NO")

epicsEnvSet("PREFIX", "$(REOLINK_PREFIX=4dh4:)")
epicsEnvSet("ADREOLINK", "$(TOP)")
epicsEnvSet("EPICS_CA_MAX_ARRAY_BYTES", "100000000")
epicsEnvSet("EPICS_DB_INCLUDE_PATH", "$(ADCORE)/db")
epicsEnvSet("IOCSH_PS1", "reolinkIOC> ")
epicsEnvSet("TZ", "EST5EDT")
epicsEnvSet("ENGINEER", "Sean Fayfar")
epicsEnvSet("LOCATION", "MITR 4DH4")

dbLoadDatabase("$(TOP)/dbd/reolinkIOC.dbd")
reolinkIOC_registerRecordDeviceDriver(pdbbase)

< ../ADReolink.iocsh

dbLoadRecords("$(DEVIOCSTATS)/db/iocAdminSoft.db", "IOC=$(PREFIX)IOC:Reolink")

dbLoadRecords("$(TOP)/iocBoot/iocReolink/stats-compat.db", "")

iocshLoad("$(AUTOSAVE)/iocsh/autosave_settings.iocsh", "PREFIX=$(PREFIX)ReolinkIOC:,SAVE_PATH=$(TOP)/iocBoot/iocReolink")
set_requestfile_path("$(ADREOLINK)/db")
set_pass0_restoreFile("ADReolink_all_settings.sav")
set_pass1_restoreFile("ADReolink_all_settings.sav")

iocInit

create_monitor_set("ADReolink_all_settings.req", 30, "P=$(PREFIX)")

dbpf "$(PREFIX)Reolink1:Codec1:Mode" 0
dbpf "$(PREFIX)Reolink1:Codec1:Compressor" 1
dbpf "$(PREFIX)Reolink1:Codec1:JPEGQuality" 90
dbpf "$(PREFIX)Reolink1:Codec1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink1:Pva1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink1:JPEG1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink1:Acquire" "$(REOLINK_AUTOSTART=0)"

dbpf "$(PREFIX)Reolink2:Codec1:Mode" 0
dbpf "$(PREFIX)Reolink2:Codec1:Compressor" 1
dbpf "$(PREFIX)Reolink2:Codec1:JPEGQuality" 90
dbpf "$(PREFIX)Reolink2:Codec1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink2:Pva1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink2:JPEG1:EnableCallbacks" 1
dbpf "$(PREFIX)Reolink2:Acquire" "$(REOLINK_AUTOSTART=0)"

dbl > dbl-all.txt
date
