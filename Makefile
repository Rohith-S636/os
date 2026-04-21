# Kernel module
obj-m += monitor.o

# Current directory
PWD := $(shell pwd)

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build

# Default target
all:
	make -C $(KDIR) M=$(PWD) modules
	gcc engine.c -o engine -lpthread

# Clean build files
clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f engine

# Optional: rebuild everything
rebuild: clean all
