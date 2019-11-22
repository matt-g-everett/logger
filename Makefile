#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#
PROJECT_NAME := logger

include $(IDF_PATH)/make/project.mk

EXTRA_COMPONENT_DIRS := $(PROJECT_PATH)/components

.PHONY: ota increment

increment:
	# Increment the prerelease number automatically
	python ota/inc-version.py

# The ota recipe increments the version, builds the bin and copies it to the iotd registry
ota: increment all
	ota/publishbin.sh $(PROJECT_NAME) $(PROJECT_PATH)

# Run the monitor recipe after the ota recipe
monitor: | ota
