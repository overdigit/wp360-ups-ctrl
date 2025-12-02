prefix = /usr

all: wp360-pmuc-cm4.dtbo wp360-pmuc-cm5.dtbo

wp360-pmuc-cm4.dtbo: module/wp360-pmuc-cm4.dts
	dtc -@ -I dts -O dtb -o wp360-pmuc-cm4.dtbo module/wp360-pmuc-cm4.dts

wp360-pmuc-cm5.dtbo: module/wp360-pmuc-cm5.dts
	dtc -@ -I dts -O dtb -o wp360-pmuc-cm5.dtbo module/wp360-pmuc-cm5.dts

install:
	install -d $(DESTDIR)$(prefix)/lib/systemd/system/codesyscontrol.service.d
	install -d $(DESTDIR)$(prefix)/lib/systemd/system-shutdown
	install -d $(DESTDIR)$(prefix)/share/initramfs-tools/hooks
	install -d $(DESTDIR)$(prefix)/share/initramfs-tools/scripts/init-bottom
	install -d $(DESTDIR)$(prefix)/src/wp360_pmuc-3/
	install -d $(DESTDIR)$(prefix)/share/wp360-pmuc/
	install wp360-pmuc-poweroff $(DESTDIR)$(prefix)/lib/systemd/system-shutdown
	install wp360-codesys.d-gpio24.conf $(DESTDIR)$(prefix)/lib/systemd/system/codesyscontrol.service.d/20-wp360-pmuc.conf
	install wp360-pmuc-hook $(DESTDIR)$(prefix)/share/initramfs-tools/hooks
	install wp360-pmuc-script $(DESTDIR)$(prefix)/share/initramfs-tools/scripts/init-bottom
	install module/wp360_pmuc.c $(DESTDIR)$(prefix)/src/wp360_pmuc-3/
	install module/wp360_pmuc.h $(DESTDIR)$(prefix)/src/wp360_pmuc-3/
	install module/Makefile $(DESTDIR)$(prefix)/src/wp360_pmuc-3/
	install wp360-pmuc-cm4.dtbo $(DESTDIR)$(prefix)/share/wp360-pmuc/
	install wp360-pmuc-cm5.dtbo $(DESTDIR)$(prefix)/share/wp360-pmuc/

.PHONY: all install
