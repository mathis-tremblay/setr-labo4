export ARCH:=arm
export CROSS_COMPILE:=$(HOME)/arm-cross-comp-env/arm-raspbian-linux-gnueabihf/bin/arm-linux-gnueabihf-
# chemin du kernel compilé lors du lab1
export KERNEL_SRC:=$(HOME)/linux-build

obj-m += setr_driver_polling.o setr_driver_irq.o

all:
	printenv
	make -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC) M=$(PWD) clean
