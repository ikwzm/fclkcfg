KERNEL_SRC_DIR=$(HOME)/work/linux

obj-m := fclkcfg.o

all:
	make -C $(KERNEL_SRC_DIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC_DIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=$(PWD) clean

