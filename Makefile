obj-m += adios.o


curpwd      := $(shell pwd)
kver        := $(shell uname -r)

all:
	make -C /lib/modules/${kver}/build M=${curpwd} modules
clean:
	make -C /lib/modules/${kver}/build M=${curpwd} clean
