# Comment/uncomment the following line to disable/enable debugging
#DEBUG = y


# Add your debugging flag (or not) to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS)

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m	:= vtty_pair.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

default: modules

modules modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) $@

endif



clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.order *.mod
	rm -rf *.symvers

depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend

.PHONY: default modules modules_install clean depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
# vim: noet
