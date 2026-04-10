PROJECT_NAME := L2CAP_server

.PHONY: mrproper

mrproper:
	rm -rf build dependencies.lock managed_components .cache

IDF_PROJECT_MK := $(IDF_PATH)/make/project.mk

ifneq ($(wildcard $(IDF_PROJECT_MK)),)
include $(IDF_PROJECT_MK)
endif
