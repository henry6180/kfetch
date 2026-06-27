obj-m+=kfetch_mod.o

PWD:=$(CURDIR)

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	gcc -O3 kfetch.c -o kfetch

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm kfetch

run:
	sudo ./kfetch -a

load:
	sudo insmod kfetch_mod.ko

unload:
	sudo rmmod -f kfetch_mod

info:
	modinfo kfetch_mod.ko

list:
	sudo lsmod | grep kfetch_mod

log:
	sudo journalctl --since "1 hour ago" | grep kernel