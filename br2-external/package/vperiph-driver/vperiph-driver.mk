VPERIPH_DRIVER_VERSION = 1.0
VPERIPH_DRIVER_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../driver
VPERIPH_DRIVER_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
