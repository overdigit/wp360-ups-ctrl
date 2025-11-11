prefix = /usr

all:

install:
	install -d $(DESTDIR)$(prefix)/lib/systemd/system/codesyscontrol.service.d
	install -d $(DESTDIR)$(prefix)/lib/systemd/system-shutdown
	install -d $(DESTDIR)$(prefix)/sbin
	install -d $(DESTDIR)$(prefix)/share/initramfs-tools/hooks
	install -d $(DESTDIR)$(prefix)/share/initramfs-tools/scripts/init-bottom
	install wp360-ups-ctrl.service $(DESTDIR)$(prefix)/lib/systemd/system
	install wp360-poweroff-gpio $(DESTDIR)$(prefix)/lib/systemd/system-shutdown
	install wp360-codesys.d-gpio24.conf $(DESTDIR)$(prefix)/lib/systemd/system/codesyscontrol.service.d/20-wp360-ups-ctrl.conf
	install wp360-ups-ctrl $(DESTDIR)$(prefix)/sbin
	install wp360-ups-ctrl-hook $(DESTDIR)$(prefix)/share/initramfs-tools/hooks
	install wp360-ups-ctrl-script $(DESTDIR)$(prefix)/share/initramfs-tools/scripts/init-bottom

.PHONY: all install
