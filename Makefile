obj-m+=kfetch_mod.o

PWD:=$(CURDIR)

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	gcc -O3 kfetch.c -o kfetch

clean: unload
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf kfetch

run: kfetch
	sudo ./kfetch -a

load:
	@if lsmod | grep -q kfetch_mod; then \
		echo "Module already loaded, unloading first..."; \
		sudo rmmod -f kfetch_mod; \
	fi
	sudo insmod kfetch_mod.ko

unload: 
	@if lsmod | grep -q kfetch_mod; then \
		sudo rmmod -f kfetch_mod; \
	else \
		echo "Module is not loaded."; \
	fi

info:
	@if lsmod | grep -q kfetch_mod; then \
		modinfo kfetch_mod.ko; \
	else \
		echo "Module is not loaded."; \
	fi

list:
	@if lsmod | grep -q kfetch_mod; then \
		sudo lsmod | grep kfetch_mod; \
	else \
		echo "Module is not loaded."; \
	fi

log:
	@if lsmod | grep -q kfetch_mod; then \
		sudo journalctl --since "1 min ago" | grep kernel; \
	else \
		echo "Module is not loaded."; \
	fi