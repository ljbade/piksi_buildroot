################################################################################
#
# geoid_daemon
#
################################################################################

GEOID_DAEMON_VERSION = 0.1
GEOID_DAEMON_SITE = \
  "${BR2_EXTERNAL_piksi_buildroot_PATH}/package/geoid_daemon/src"
GEOID_DAEMON_SITE_METHOD = local
GEOID_DAEMON_DEPENDENCIES = czmq libsbp libpiksi libcurl libnetwork

define GEOID_DAEMON_BUILD_CMDS
    $(MAKE) CC=$(TARGET_CC) LD=$(TARGET_LD) -C $(@D) all
endef

define GEOID_DAEMON_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/geoid_daemon $(TARGET_DIR)/usr/bin
endef

$(eval $(generic-package))
