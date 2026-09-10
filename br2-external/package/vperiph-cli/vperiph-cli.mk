VPERIPH_CLI_VERSION = 1.0
VPERIPH_CLI_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../user
VPERIPH_CLI_SITE_METHOD = local

define VPERIPH_CLI_BUILD_CMDS
	$(TARGET_CXX) $(TARGET_CXXFLAGS) \
		$(@D)/vperiph_cli.cpp \
		-o $(@D)/vperiph_cli \
		$(TARGET_LDFLAGS)
endef

define VPERIPH_CLI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/vperiph_cli \
		$(TARGET_DIR)/usr/bin/vperiph_cli
endef

$(eval $(generic-package))
