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

ota: increment all
	ota/publishbin.sh $(PROJECT_NAME) $(PROJECT_PATH)
