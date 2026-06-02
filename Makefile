RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
CFLAGS = -mcmodel=medany -ffreestanding -nostdlib -g -Wall -Iinclude
QEMU ?= qemu-system-riscv64
TARGET = bootloader
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BOOT_OBJS = $(OBJ_DIR)/start.o $(OBJ_DIR)/main.o $(OBJ_DIR)/uart.o \
	$(OBJ_DIR)/boot.o $(OBJ_DIR)/mm.o $(OBJ_DIR)/util.o \
	$(OBJ_DIR)/sched.o $(OBJ_DIR)/trap.o $(OBJ_DIR)/shell.o \
	$(OBJ_DIR)/thread.o $(OBJ_DIR)/syscall.o $(OBJ_DIR)/video.o \
	$(OBJ_DIR)/vm.o

build: $(TARGET)

$(TARGET): $(TARGET).elf
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET)

$(TARGET).elf: $(BOOT_OBJS)
	$(LD) -T link.ld -o $(TARGET).elf $(BOOT_OBJS)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/start.o: start.S | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/main.o: main.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/uart.o: uart.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/boot.o: kernel/boot.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mm.o: kernel/mm.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/util.o: kernel/util.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/sched.o: kernel/sched.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/trap.o: kernel/trap.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/shell.o: kernel/shell.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/thread.o: kernel/thread.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/syscall.o: kernel/syscall.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/video.o: kernel/video.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/vm.o: kernel/vm.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

run-stdio: build
	$(QEMU) -M virt -m 256M -kernel $(TARGET) -display none -serial stdio -initrd initramfs.cpio

run-gui: build
	$(QEMU) -M virt -m 256M -kernel $(TARGET) -serial stdio -device ramfb -initrd initramfs.cpio

run-vnc: build
	$(QEMU) -M virt -m 256M -kernel $(TARGET) -serial stdio -device ramfb -display vnc=:1,password=off -initrd initramfs.cpio

run: build
	$(QEMU) -M virt -m 256M -kernel $(TARGET) -display none -serial pty -initrd initramfs.cpio

clean:
	rm -f $(TARGET) $(TARGET).elf *.o
	rm -rf $(BUILD_DIR)

.PHONY: build run run-stdio run-gui run-vnc clean
