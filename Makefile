#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#
PROJECT_NAME := logger

include $(IDF_PATH)/make/project.mk

EXTRA_COMPONENT_DIRS := $(PROJECT_PATH)/components

.PHONY: ota increment coapapi

increment:
	# Increment the prerelease number automatically
	python ota/inc-version.py

ota: increment all
	# Copy the version into the publish location
	cp $(PROJECT_PATH)/version.txt $(PROJECT_PATH)/publish/version
