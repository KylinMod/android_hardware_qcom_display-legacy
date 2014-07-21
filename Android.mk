
#Enables the listed display HAL modules
#libs to be built for QCOM targets only

ifeq ($(TARGET_QCOM_DISPLAY_VARIANT),legacy-old)
ifeq ($(call is-vendor-board-platform,QCOM),true)
display-hals := libgralloc libgenlock libcopybit liblight
display-hals += libhwcomposer liboverlay libqdutils
endif

display-hals += libtilerenderer

include $(call all-named-subdir-makefiles,$(display-hals))
endif
ifeq ($(TARGET_QCOM_DISPLAY_VARIANT),legacy)

display-hals := libgralloc libgenlock libcopybit
display-hals += libhwcomposer liboverlay libqdutils libexternal libqservice
display-hals += libmemtrack

ifneq ($(TARGET_PROVIDES_LIBLIGHT),true)
display-hals += liblight
endif

ifeq ($(call is-vendor-board-platform,QCOM),true)
include $(call all-named-subdir-makefiles,$(display-hals))
endif
endif

