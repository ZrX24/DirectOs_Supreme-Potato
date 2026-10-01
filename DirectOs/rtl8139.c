#include "rtl8139.h"

#define PCI_ADDRESS_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC
#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139
#define RX_BUFFER_SIZE 8192
#define RX_BUFFER_ALLOCATION (RX_BUFFER_SIZE + 16)
#define TX_BUFFER_SIZE 1518
#define ETH_MIN_FRAME_SIZE 60
#define TSD_HOST_OWNS 0x00002000U
#define TSD_TRANSMIT_OK 0x00008000U

enum {
	REG_IDR0 = 0x00,
	REG_TSD0 = 0x10,
	REG_TSAD0 = 0x20,
	REG_RBSTART = 0x30,
	REG_CR = 0x37,
	REG_CAPR = 0x38,
	REG_CBR = 0x3A,
	REG_IMR = 0x3C,
	REG_RCR = 0x44
};

static unsigned short io_base;
static unsigned short rx_offset;
static unsigned char mac_address[6];
static unsigned char initialized;
static unsigned char ready;
static unsigned char rx_buffer[RX_BUFFER_ALLOCATION] __attribute__((aligned(16)));
static unsigned char tx_buffer[TX_BUFFER_SIZE] __attribute__((aligned(16)));

static void port_out8(unsigned short port, unsigned char value)
{
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char port_in8(unsigned short port)
{
	unsigned char value;
	__asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_out16(unsigned short port, unsigned short value)
{
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned short port_in16(unsigned short port)
{
	unsigned short value;
	__asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

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
			if ((header & 0xFFFF) == 0xFFFF) continue;
			functions = (header & 0x00800000U) ? 8 : 1;
			for (function = 0; function < functions; function++) {
				unsigned int identity = pci_read(bus, device, function, 0x00);
				if ((identity & 0xFFFF) != RTL8139_VENDOR || (identity >> 16) != RTL8139_DEVICE) continue;
				*found_bus = bus;
				*found_device = device;
				*found_function = function;
				return 1;
			}
		}
	}
	return 0;
}

static unsigned char rx_read8(unsigned int offset)
{
	return rx_buffer[offset % RX_BUFFER_SIZE];
}

static unsigned short rx_read16(unsigned int offset)
{
	return (unsigned short)(rx_read8(offset) | ((unsigned short)rx_read8(offset + 1) << 8));
}

int rtl8139_init(void)
{
	unsigned int bus, device, function;
	unsigned int bar0, command;
	unsigned int timeout;
	unsigned int i;
	if (initialized) return ready;
	initialized = 1;
	if (!find_device(&bus, &device, &function)) return 0;
	bar0 = pci_read(bus, device, function, 0x10);
	if (!(bar0 & 1)) return 0;
	io_base = (unsigned short)(bar0 & 0xFFFC);
	if (!io_base) return 0;
	command = pci_read(bus, device, function, 0x04);
	pci_write(bus, device, function, 0x04, (command & 0xFFFFU) | 0x00000005U);
	port_out8(io_base + REG_CR, 0x10);
	for (timeout = 0; timeout < 1000000U; timeout++)
		if (!(port_in8(io_base + REG_CR) & 0x10)) break;
	if (timeout == 1000000U) return 0;
	for (i = 0; i < sizeof(mac_address); i++) mac_address[i] = port_in8(io_base + REG_IDR0 + i);
	for (i = 0; i < RX_BUFFER_ALLOCATION; i++) rx_buffer[i] = 0;
	for (i = 0; i < TX_BUFFER_SIZE; i++) tx_buffer[i] = 0;
	rx_offset = 0;
	port_out32(io_base + REG_RBSTART, (unsigned int)rx_buffer);
	port_out16(io_base + REG_CAPR, 0xFFF0);
	port_out16(io_base + REG_IMR, 0);
	port_out32(io_base + REG_RCR, 0x0000008FU);
	port_out8(io_base + REG_CR, 0x0C);
	ready = 1;
	return 1;
}

int rtl8139_is_ready(void)
{
	return ready;
}

int rtl8139_send(const unsigned char *frame, unsigned short length)
{
	unsigned int timeout;
	unsigned int transmit_length;
	unsigned int i;
	unsigned int status;
	if (!ready || !frame || length < 14 || length > TX_BUFFER_SIZE) return 0;
	for (timeout = 0; timeout < 1000000U; timeout++) {
		status = port_in32(io_base + REG_TSD0);
		if (status & TSD_HOST_OWNS) break;
	}
	if (timeout == 1000000U) return 0;
	for (i = 0; i < length; i++) tx_buffer[i] = frame[i];
	transmit_length = length < ETH_MIN_FRAME_SIZE ? ETH_MIN_FRAME_SIZE : length;
	for (i = length; i < transmit_length; i++) tx_buffer[i] = 0;
	port_out32(io_base + REG_TSAD0, (unsigned int)tx_buffer);
	port_out32(io_base + REG_TSD0, transmit_length);
	for (timeout = 0; timeout < 1000000U; timeout++) {
		status = port_in32(io_base + REG_TSD0);
		if (status & TSD_HOST_OWNS) return (status & TSD_TRANSMIT_OK) != 0;
	}
	return 0;
}

int rtl8139_receive(unsigned char *frame, unsigned short capacity, unsigned short *length)
{
	unsigned short packet_length;
	unsigned short frame_length;
	unsigned short status;
	unsigned int i;
	unsigned int next_offset;
	if (!ready || !frame || !length || (port_in8(io_base + REG_CR) & 1)) return 0;
	status = rx_read16(rx_offset);
	packet_length = rx_read16(rx_offset + 2);
	if (packet_length < 4 || packet_length > 1518) {
		rx_offset = (unsigned short)(port_in16(io_base + REG_CBR) & (RX_BUFFER_SIZE - 1));
		port_out16(io_base + REG_CAPR, (unsigned short)(rx_offset - 16));
		return -1;
	}
	frame_length = (unsigned short)(packet_length - 4);
	if (frame_length > capacity) {
		frame_length = capacity;
		status = 0;
	}
	for (i = 0; i < frame_length; i++) frame[i] = rx_read8(rx_offset + 4 + i);
	next_offset = (rx_offset + 4 + packet_length + 3) & ~3U;
	rx_offset = (unsigned short)(next_offset & (RX_BUFFER_SIZE - 1));
	port_out16(io_base + REG_CAPR, (unsigned short)(rx_offset - 16));
	if (!(status & 1)) return -1;
	*length = frame_length;
	return 1;
}

void rtl8139_get_mac(unsigned char mac[6])
{
	unsigned int i;
	if (!mac) return;
	for (i = 0; i < 6; i++) mac[i] = mac_address[i];
}