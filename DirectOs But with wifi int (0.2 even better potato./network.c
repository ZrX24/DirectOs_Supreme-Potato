#include "network.h"
#include "rtl8139.h"
#include "timer.h"

#define ETHERNET_FRAME_SIZE 1518
#define NETWORK_TIMEOUT_TICKS 300

static const unsigned char local_ip[4] = { 10, 0, 2, 15 };
static const unsigned char netmask[4] = { 255, 255, 255, 0 };
static const unsigned char gateway_ip[4] = { 10, 0, 2, 2 };
static unsigned char local_mac[6];
static unsigned char arp_target_ip[4];
static unsigned char arp_target_mac[6];
static unsigned char arp_resolved;
static unsigned char ping_target_ip[4];
static unsigned short ping_identifier = 0xD105;
static unsigned short ping_sequence;
static unsigned char ping_reply_received;
static unsigned int ip_identifier;
static unsigned char receive_frame[ETHERNET_FRAME_SIZE];

static unsigned short read_be16(const unsigned char *data)
{
	return (unsigned short)(((unsigned short)data[0] << 8) | data[1]);
}

static void write_be16(unsigned char *data, unsigned short value)
{
	data[0] = (unsigned char)(value >> 8);
	data[1] = (unsigned char)value;
}

static void copy_bytes(unsigned char *destination, const unsigned char *source, unsigned int length)
{
	unsigned int i;
	for (i = 0; i < length; i++) destination[i] = source[i];
}

static int bytes_equal(const unsigned char *left, const unsigned char *right, unsigned int length)
{
	unsigned int i;
	for (i = 0; i < length; i++) if (left[i] != right[i]) return 0;
	return 1;
}

static unsigned short checksum(const unsigned char *data, unsigned int length)
{
	unsigned int sum = 0;
	while (length > 1) {
		sum += read_be16(data);
		data += 2;
		length -= 2;
	}
	if (length) sum += (unsigned int)data[0] << 8;
	while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
	return (unsigned short)~sum;
}

static int ip_is_local(const unsigned char *address)
{
	return bytes_equal(address, local_ip, 4);
}

static int network_send_arp(unsigned short operation, const unsigned char *target_mac, const unsigned char *target_ip)
{
	unsigned char frame[60];
	unsigned int i;
	for (i = 0; i < sizeof(frame); i++) frame[i] = 0;
	if (operation == 1) for (i = 0; i < 6; i++) frame[i] = 0xFF;
	else copy_bytes(frame, target_mac, 6);
	copy_bytes(frame + 6, local_mac, 6);
	write_be16(frame + 12, 0x0806);
	write_be16(frame + 14, 1);
	write_be16(frame + 16, 0x0800);
	frame[18] = 6;
	frame[19] = 4;
	write_be16(frame + 20, operation);
	copy_bytes(frame + 22, local_mac, 6);
	copy_bytes(frame + 28, local_ip, 4);
	if (operation == 1) {
		for (i = 0; i < 6; i++) frame[32 + i] = 0;
	} else copy_bytes(frame + 32, target_mac, 6);
	copy_bytes(frame + 38, target_ip, 4);
	return rtl8139_send(frame, sizeof(frame));
}

static int network_send_icmp_reply(unsigned char *frame, unsigned int ip_header_length, unsigned int total_length)
{
	unsigned char address[4];
	unsigned int icmp_length = total_length - ip_header_length;
	copy_bytes(frame, frame + 6, 6);
	copy_bytes(frame + 6, local_mac, 6);
	copy_bytes(address, frame + 26, 4);
	copy_bytes(frame + 26, local_ip, 4);
	copy_bytes(frame + 30, address, 4);
	frame[22] = 64;
	write_be16(frame + 24, 0);
	write_be16(frame + 24, checksum(frame + 14, ip_header_length));
	frame[14 + ip_header_length] = 0;
	write_be16(frame + 14 + ip_header_length + 2, 0);
	write_be16(frame + 14 + ip_header_length + 2, checksum(frame + 14 + ip_header_length, icmp_length));
	return rtl8139_send(frame, (unsigned short)(14 + total_length));
}

static void process_arp(const unsigned char *frame, unsigned short length)
{
	unsigned short operation;
	if (length < 42 || read_be16(frame + 14) != 1 || read_be16(frame + 16) != 0x0800 || frame[18] != 6 || frame[19] != 4) return;
	operation = read_be16(frame + 20);
	if (operation == 1 && ip_is_local(frame + 38)) {
		network_send_arp(2, frame + 22, frame + 28);
		return;
	}
	if (operation == 2 && bytes_equal(frame + 28, arp_target_ip, 4) && ip_is_local(frame + 38)) {
		copy_bytes(arp_target_mac, frame + 22, 6);
		arp_resolved = 1;
	}
}

static void process_ipv4(unsigned char *frame, unsigned short length)
{
	unsigned int ip_header_length;
	unsigned int total_length;
	unsigned int icmp_length;
	unsigned char *ip;
	unsigned char *icmp;
	if (length < 34) return;
	ip = frame + 14;
	if ((ip[0] >> 4) != 4) return;
	ip_header_length = (ip[0] & 15U) * 4U;
	if (ip_header_length < 20 || length < 14 + ip_header_length) return;
	total_length = read_be16(ip + 2);
	if (total_length < ip_header_length + 8 || total_length > length - 14) return;
	if (checksum(ip, ip_header_length) != 0) return;
	if (read_be16(ip + 6) & 0x3FFF) return;
	if (ip[9] != 1) return;
	icmp = ip + ip_header_length;
	icmp_length = total_length - ip_header_length;
	if (checksum(icmp, icmp_length) != 0) return;
	if (icmp[0] == 8 && icmp[1] == 0 && ip_is_local(ip + 16)) {
		network_send_icmp_reply(frame, ip_header_length, total_length);
		return;
	}
	if (icmp[0] == 0 && icmp[1] == 0 && bytes_equal(ip + 12, ping_target_ip, 4) &&
		read_be16(icmp + 4) == ping_identifier && read_be16(icmp + 6) == ping_sequence)
		ping_reply_received = 1;
}

static void network_poll(void)
{
	unsigned short length;
	int result = rtl8139_receive(receive_frame, sizeof(receive_frame), &length);
	if (result != 1 || length < 14) return;
	switch (read_be16(receive_frame + 12)) {
	case 0x0806:
		process_arp(receive_frame, length);
		break;
	case 0x0800:
		process_ipv4(receive_frame, length);
		break;
	}
}

static int network_resolve(const unsigned char *address)
{
	unsigned int start;
	copy_bytes(arp_target_ip, address, 4);
	arp_resolved = 0;
	if (!network_send_arp(1, 0, arp_target_ip)) return 0;
	start = timer_ticks();
	while ((unsigned int)(timer_ticks() - start) < NETWORK_TIMEOUT_TICKS && !arp_resolved) network_poll();
	return arp_resolved;
}

int network_init(void)
{
	if (!rtl8139_init()) return 0;
	rtl8139_get_mac(local_mac);
	return 1;
}

int network_ping(const unsigned char address[4], unsigned int *elapsed_ms)
{
	unsigned char frame[60];
	unsigned char next_hop[4];
	unsigned char *ip = frame + 14;
	unsigned char *icmp = ip + 20;
	unsigned int start;
	unsigned int elapsed;
	unsigned int i;
	if (!address || !network_init()) return -1;
	copy_bytes(ping_target_ip, address, 4);
	for (i = 0; i < 4; i++) {
		if ((address[i] & netmask[i]) != (local_ip[i] & netmask[i])) break;
	}
	copy_bytes(next_hop, i == 4 ? address : gateway_ip, 4);
	if (!network_resolve(next_hop)) return 0;
	for (i = 0; i < sizeof(frame); i++) frame[i] = 0;
	copy_bytes(frame, arp_target_mac, 6);
	copy_bytes(frame + 6, local_mac, 6);
	write_be16(frame + 12, 0x0800);
	ip[0] = 0x45;
	write_be16(ip + 2, 36);
	write_be16(ip + 4, (unsigned short)++ip_identifier);
	write_be16(ip + 6, 0x4000);
	ip[8] = 64;
	ip[9] = 1;
	copy_bytes(ip + 12, local_ip, 4);
	copy_bytes(ip + 16, address, 4);
	write_be16(ip + 10, checksum(ip, 20));
	icmp[0] = 8;
	icmp[1] = 0;
	write_be16(icmp + 4, ping_identifier);
	write_be16(icmp + 6, ++ping_sequence);
	copy_bytes(icmp + 8, (const unsigned char *)"DirectOS", 8);
	write_be16(icmp + 2, checksum(icmp, 16));
	ping_reply_received = 0;
	start = timer_ticks();
	if (!rtl8139_send(frame, sizeof(frame))) return 0;
	while ((unsigned int)(timer_ticks() - start) < NETWORK_TIMEOUT_TICKS && !ping_reply_received) network_poll();
	elapsed = timer_ticks() - start;
	if (!ping_reply_received) return 0;
	if (elapsed_ms) *elapsed_ms = (elapsed * 1000U) / 100U;
	return 1;
}