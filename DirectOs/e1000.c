#include "e1000.h"

#define PCI_ADDRESS_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC
#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E
#define RING_SIZE 8
#define RX_BUFFER_SIZE 2048
#define TX_BUFFER_SIZE 1518
#define ETH_MIN_FRAME_SIZE 60
#define MMIO_SIZE 0x20000

enum {
	REG_CTRL = 0x0000,
	REG_STATUS = 0x0008,
	REG_IMC = 0x00D8,
	REG_RCTL = 0x0100,
	REG_TCTL = 0x0400,
	REG_TIPG = 0x0410,
	REG_RAL0 = 0x5400,
	REG_RAH0 = 0x5404,
	REG_RDBAL = 0x2800,
	REG_RDBAH = 0x2804,
	REG_RDLEN = 0x2808,
	REG_RDH = 0x2810,
	REG_RDT = 0x2818,
	REG_TDBAL = 0x3800,
	REG_TDBAH = 0x3804,
	REG_TDLEN = 0x3808,
	REG_TDH = 0x3810,
	REG_TDT = 0x3818
};

typedef struct {
	unsigned int address_low;
	unsigned int address_high;
	unsigned short length;
	unsigned short checksum;
	unsigned char status;
	unsigned char errors;
	unsigned short special;
} RxDescriptor;

typedef struct {
	unsigned int address_low;
	unsigned int address_high;
	unsigned short length;
	unsigned char checksum_offset;
	unsigned char command;
	unsigned char status;
	unsigned char checksum_start;
	unsigned short special;
} TxDescriptor;

static volatile unsigned int *registers;
static volatile RxDescriptor rx_descriptors[RING_SIZE] __attribute__((aligned(16)));
static volatile TxDescriptor tx_descriptors[RING_SIZE] __attribute__((aligned(16)));
static unsigned char rx_buffers[RING_SIZE][RX_BUFFER_SIZE] __attribute__((aligned(16)));
static unsigned char tx_buffers[RING_SIZE][TX_BUFFER_SIZE] __attribute__((aligned(16)));
static unsigned char mac_address[6];
static unsigned int rx_index;
static unsigned int tx_index;
static unsigned char initialized;
static unsigned char ready;

static void port_out32(unsigned short port, unsigned int value)
{
	__asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned int port_in32(unsigned short port)
{
	unsigned int value;
	__asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static unsigned int pci_read(unsigned int bus, unsigned int device, unsigned int function, unsigned int offset)
{
	unsigned int address = 0x80000000U | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC);
	port_out32(PCI_ADDRESS_PORT, address);
	return port_in32(PCI_DATA_PORT);
}

static void pci_write(unsigned int bus, unsigned int device, unsigned int function, unsigned int offset, unsigned int value)
{
	unsigned int address = 0x80000000U | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC);
	port_out32(PCI_ADDRESS_PORT, address);
	port_out32(PCI_DATA_PORT, value);
}

static int find_device(unsigned int *found_bus, unsigned int *found_device, unsigned int *found_function)
{
	unsigned int bus;
	unsigned int device;
	unsigned int function;
	for (bus = 0; bus < 256; bus++) {
		for (device = 0; device < 32; device++) {
			unsigned int header = pci_read(bus, device, 0, 0x0C);
			unsigned int functions;
			if ((header & 0xFFFFU) == 0xFFFFU) continue;
			functions = (header & 0x00800000U) ? 8 : 1;
			for (function = 0; function < functions; function++) {
				unsigned int identity = pci_read(bus, device, function, 0x00);
				if ((identity & 0xFFFFU) != E1000_VENDOR || (identity >> 16) != E1000_DEVICE) continue;
				*found_bus = bus;
				*found_device = device;
				*found_function = function;
				return 1;
			}
		}
	}
	return 0;
}

static unsigned int mmio_read(unsigned int offset)
{
	return registers[offset >> 2];
}

static void mmio_write(unsigned int offset, unsigned int value)
{
	registers[offset >> 2] = value;
}

int e1000_init(void)
{
	unsigned int bus, device, function;
	unsigned int bar0, command;
	unsigned int timeout;
	unsigned int i;
	unsigned int low;
	unsigned int high;
	if (initialized) return ready;
	initialized = 1;
	if (!find_device(&bus, &device, &function)) return 0;
	bar0 = pci_read(bus, device, function, 0x10);
	if (bar0 & 1U) return 0;
	bar0 &= 0xFFFFFFF0U;
	if (!bar0 || bar0 > 0xFFFFFFFFU - MMIO_SIZE) return 0;
	registers = (volatile unsigned int *)bar0;
	command = pci_read(bus, device, function, 0x04);
	pci_write(bus, device, function, 0x04, (command & 0xFFFFU) | 0x00000006U);
	mmio_write(REG_CTRL, mmio_read(REG_CTRL) | 0x04000000U);
	for (timeout = 0; timeout < 1000000U; timeout++)
		if (!(mmio_read(REG_CTRL) & 0x04000000U)) break;
	if (timeout == 1000000U) return 0;
	mmio_write(REG_IMC, 0xFFFFFFFFU);
	low = mmio_read(REG_RAL0);
	high = mmio_read(REG_RAH0);
	for (i = 0; i < 4; i++) mac_address[i] = (unsigned char)(low >> (i * 8));
	mac_address[4] = (unsigned char)high;
	mac_address[5] = (unsigned char)(high >> 8);
	if (!(high & 0x80000000U)) return 0;
	for (i = 0; i < RING_SIZE; i++) {
		rx_descriptors[i].address_low = (unsigned int)rx_buffers[i];
		rx_descriptors[i].address_high = 0;
		rx_descriptors[i].length = 0;
		rx_descriptors[i].checksum = 0;
		rx_descriptors[i].status = 0;
		rx_descriptors[i].errors = 0;
		rx_descriptors[i].special = 0;
		tx_descriptors[i].address_low = (unsigned int)tx_buffers[i];
		tx_descriptors[i].address_high = 0;
		tx_descriptors[i].length = 0;
		tx_descriptors[i].checksum_offset = 0;
		tx_descriptors[i].command = 0;
		tx_descriptors[i].status = 1;
		tx_descriptors[i].checksum_start = 0;
		tx_descriptors[i].special = 0;
	}
	rx_index = 0;
	tx_index = 0;
	mmio_write(REG_RDBAL, (unsigned int)rx_descriptors);
	mmio_write(REG_RDBAH, 0);
	mmio_write(REG_RDLEN, sizeof(rx_descriptors));
	mmio_write(REG_RDH, 0);
	mmio_write(REG_RDT, RING_SIZE - 1);
	mmio_write(REG_TDBAL, (unsigned int)tx_descriptors);
	mmio_write(REG_TDBAH, 0);
	mmio_write(REG_TDLEN, sizeof(tx_descriptors));
	mmio_write(REG_TDH, 0);
	mmio_write(REG_TDT, 0);
	mmio_write(REG_RCTL, 0x04008002U);
	mmio_write(REG_TCTL, 0x0004010AU);
	mmio_write(REG_TIPG, 0x0060200AU);
	ready = 1;
	return 1;
}

int e1000_send(const unsigned char *frame, unsigned short length)
{
	volatile TxDescriptor *descriptor;
	unsigned int timeout;
	unsigned int transmit_length;
	unsigned int i;
	if (!ready || !frame || length < 14 || length > TX_BUFFER_SIZE) return 0;
	descriptor = &tx_descriptors[tx_index];
	for (timeout = 0; timeout < 1000000U && !(descriptor->status & 1); timeout++) { }
	if (timeout == 1000000U) return 0;
	transmit_length = length < ETH_MIN_FRAME_SIZE ? ETH_MIN_FRAME_SIZE : length;
	for (i = 0; i < length; i++) tx_buffers[tx_index][i] = frame[i];
	for (i = length; i < transmit_length; i++) tx_buffers[tx_index][i] = 0;
	descriptor->length = (unsigned short)transmit_length;
	descriptor->checksum_offset = 0;
	descriptor->command = 0x0B;
	descriptor->status = 0;
	descriptor->checksum_start = 0;
	descriptor->special = 0;
	__asm__ volatile ("" : : : "memory");
	tx_index = (tx_index + 1) % RING_SIZE;
	mmio_write(REG_TDT, tx_index);
	for (timeout = 0; timeout < 1000000U && !(descriptor->status & 1); timeout++) { }
	return (descriptor->status & 1) != 0 && (descriptor->status & 0x0E) == 0;
}

int e1000_receive(unsigned char *frame, unsigned short capacity, unsigned short *length)
{
	volatile RxDescriptor *descriptor;
	unsigned short frame_length;
	unsigned int i;
	if (!ready || !frame || !length) return 0;
	descriptor = &rx_descriptors[rx_index];
	if (!(descriptor->status & 1)) return 0;
	frame_length = descriptor->length;
	if (frame_length > capacity) frame_length = capacity;
	for (i = 0; i < frame_length; i++) frame[i] = rx_buffers[rx_index][i];
	i = descriptor->errors;
	*length = frame_length;
	descriptor->status = 0;
	descriptor->errors = 0;
	__asm__ volatile ("" : : : "memory");
	mmio_write(REG_RDT, rx_index);
	rx_index = (rx_index + 1) % RING_SIZE;
	if (i || frame_length < 14) return -1;
	return 1;
}

void e1000_get_mac(unsigned char mac[6])
{
	unsigned int i;
	if (!mac) return;
	for (i = 0; i < 6; i++) mac[i] = mac_address[i];
}