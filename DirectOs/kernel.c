#include "fs.h"
#include "keyboard.h"
#include "network.h"
#include "screen.h"
#include "timer.h"

#define INPUT_SIZE 160
#define ARG_MAX 12
#define USER_MAX 8
#define ALIAS_MAX 8
#define CMOS_VERBOSE_MAGIC_REG 0x38
#define CMOS_VERBOSE_VALUE_REG 0x39
#define CMOS_VERBOSE_CHECK_REG 0x3A
#define CMOS_VERBOSE_MAGIC 0xD6
#define CMOS_VERBOSE_SALT 0xA5

typedef struct {
	char name[16];
	char password[24];
} User;

typedef struct {
	char name[24];
	char command[64];
} Alias;

typedef struct {
	unsigned int second;
	unsigned int minute;
	unsigned int hour;
	unsigned int day;
	unsigned int month;
	unsigned int year;
} DateTime;

static User users[USER_MAX] = { { "root", "root" } };
static unsigned int user_count = 1;
static unsigned int current_user;
static Alias aliases[ALIAS_MAX];
static unsigned int alias_count;
static unsigned char verbose_enabled;

static void boot_status(const char *state, const char *message)
{
	if (!verbose_enabled) return;
	screen_write("[");
	screen_write(state);
	screen_write("] ");
	screen_write(message);
	screen_putchar('\n');
}

static unsigned char port_in(unsigned short port)
{
	unsigned char value;
	__asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_out(unsigned short port, unsigned char value)
{
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void port_out_word(unsigned short port, unsigned short value)
{
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned int port_in_dword(unsigned short port)
{
	unsigned int value;
	__asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_out_dword(unsigned short port, unsigned int value)
{
	__asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static int str_equal(const char *left, const char *right)
{
	while (*left && *left == *right) { left++; right++; }
	return *left == *right;
}

static unsigned int str_length(const char *text)
{
	unsigned int length = 0;
	while (text && text[length]) length++;
	return length;
}

static void str_copy(char *target, const char *source, unsigned int capacity)
{
	unsigned int i = 0;
	if (!capacity) return;
	while (i + 1 < capacity && source[i]) { target[i] = source[i]; i++; }
	target[i] = 0;
}

static void str_append(char *target, const char *source, unsigned int capacity)
{
	unsigned int length = str_length(target);
	unsigned int i = 0;
	while (length + 1 < capacity && source[i]) target[length++] = source[i++];
	if (capacity) target[length] = 0;
}

static int contains(const char *text, const char *needle)
{
	unsigned int i;
	unsigned int j;
	if (!*needle) return 1;
	for (i = 0; text[i]; i++) {
		for (j = 0; needle[j] && text[i + j] == needle[j]; j++) { }
		if (!needle[j]) return 1;
	}
	return 0;
}

static void print_unsigned(unsigned int value)
{
	char digits[11];
	unsigned int count = 0;
	if (!value) { screen_putchar('0'); return; }
	while (value && count < sizeof(digits)) {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	}
	while (count) screen_putchar(digits[--count]);
}

static void print_two(unsigned int value)
{
	if (value < 10) screen_putchar('0');
	print_unsigned(value);
}

static void print_hex_byte(unsigned char value)
{
	static const char digits[] = "0123456789ABCDEF";
	screen_putchar(digits[value >> 4]);
	screen_putchar(digits[value & 15]);
}

static void print_hex_word(unsigned int value)
{
	print_hex_byte((unsigned char)(value >> 8));
	print_hex_byte((unsigned char)value);
}

static void cpuid_read(unsigned int leaf, unsigned int subleaf, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d)
{
	__asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}

static unsigned int pci_read(unsigned int bus, unsigned int device, unsigned int function, unsigned int offset)
{
	unsigned int address = 0x80000000U | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC);
	port_out_dword(0xCF8, address);
	return port_in_dword(0xCFC);
}

static void find_pci_devices(unsigned int *display_id, unsigned int *ethernet_id)
{
	unsigned int bus, device, function;
	*display_id = 0;
	*ethernet_id = 0;
	for (bus = 0; bus < 256 && (!*display_id || !*ethernet_id); bus++) {
		for (device = 0; device < 32 && (!*display_id || !*ethernet_id); device++) {
			unsigned int header = pci_read(bus, device, 0, 0x0C);
			unsigned int functions;
			if ((header & 0xFFFF) == 0xFFFF) continue;
			functions = (header & 0x00800000) ? 8 : 1;
			for (function = 0; function < functions; function++) {
				unsigned int identity = pci_read(bus, device, function, 0x00);
				unsigned int classes;
				unsigned int class_code;
				unsigned int vendor = identity & 0xFFFF;
				if (vendor == 0xFFFF) continue;
				classes = pci_read(bus, device, function, 0x08);
				class_code = classes >> 16;
				if (!*display_id && (class_code >> 8) == 0x03)
					*display_id = ((identity >> 16) << 16) | vendor;
				if (!*ethernet_id && class_code == 0x0200)
					*ethernet_id = ((identity >> 16) << 16) | vendor;
			}
		}
	}
}

static void read_cpu_brand(char brand[49])
{
	unsigned int maximum, b, c, d, leaf, reg;
	unsigned int registers[4];
	unsigned int character;
	cpuid_read(0x80000000, 0, &maximum, &b, &c, &d);
	if (maximum < 0x80000004) {
		str_copy(brand, "x86-compatible CPU", 49);
		return;
	}
	for (leaf = 0; leaf < 3; leaf++) {
		cpuid_read(0x80000002 + leaf, 0, &registers[0], &registers[1], &registers[2], &registers[3]);
		for (reg = 0; reg < 4; reg++) {
			for (character = 0; character < 4; character++)
				brand[leaf * 16 + reg * 4 + character] = (char)(registers[reg] >> (character * 8));
		}
	}
	brand[48] = 0;
	for (character = 47; character > 0 && (brand[character] == ' ' || brand[character] == 0); character--) brand[character] = 0;
	for (character = 0; brand[character] == ' ' && character < 47; character++) { }
	if (character) {
		for (reg = 0; brand[character + reg]; reg++) brand[reg] = brand[character + reg];
		brand[reg] = 0;
	}
}

static unsigned int usable_ram_mib(void)
{
	unsigned int low;
	unsigned int high;
	__asm__ volatile ("movl 0x500, %0\n\tmovl 0x504, %1" : "=r"(low), "=r"(high));
	return (high << 12) | (low >> 20);
}

static unsigned int parse_unsigned(const char *text, unsigned int base)
{
	unsigned int value = 0;
	while (*text) {
		unsigned int digit;
		if (*text >= '0' && *text <= '9') digit = (unsigned int)(*text - '0');
		else if (*text >= 'a' && *text <= 'f') digit = (unsigned int)(*text - 'a' + 10);
		else if (*text >= 'A' && *text <= 'F') digit = (unsigned int)(*text - 'A' + 10);
		else return 0;
		if (digit >= base) return 0;
		value = value * base + digit;
		text++;
	}
	return value;
}

static int parse_ipv4_address(const char *text, unsigned char address[4])
{
	unsigned int part;
	for (part = 0; part < 4; part++) {
		unsigned int value = 0;
		if (*text < '0' || *text > '9') return 0;
		while (*text >= '0' && *text <= '9') {
			unsigned int digit = (unsigned int)(*text - '0');
			if (value > (255U - digit) / 10U) return 0;
			value = value * 10 + digit;
			text++;
		}
		address[part] = (unsigned char)value;
		if (part < 3) {
			if (*text != '.') return 0;
			text++;
		}
	}
	return *text == 0;
}

static int parse_ping_size(const char *text, unsigned short *size)
{
	unsigned int value = 0;
	if (!*text) return 0;
	while (*text) {
		unsigned int digit;
		if (*text < '0' || *text > '9') return 0;
		digit = (unsigned int)(*text - '0');
		if (value > (1472U - digit) / 10U) return 0;
		value = value * 10 + digit;
		text++;
	}
	*size = (unsigned short)value;
	return 1;
}

static unsigned int rtc_value(unsigned char value, int binary)
{
	if (binary) return value;
	return (unsigned int)((value & 15) + ((value >> 4) * 10));
}

static unsigned char rtc_read(unsigned char reg)
{
	port_out(0x70, reg);
	return port_in(0x71);
}

static void rtc_write(unsigned char reg, unsigned char value)
{
	port_out(0x70, reg);
	port_out(0x71, value);
}

static unsigned char verbose_load(void)
{
	unsigned char enabled;
	if (rtc_read(CMOS_VERBOSE_MAGIC_REG) != CMOS_VERBOSE_MAGIC) return 0;
	enabled = rtc_read(CMOS_VERBOSE_VALUE_REG);
	if (enabled > 1 || rtc_read(CMOS_VERBOSE_CHECK_REG) != (unsigned char)(CMOS_VERBOSE_MAGIC ^ enabled ^ CMOS_VERBOSE_SALT)) return 0;
	return enabled;
}

static int verbose_save(unsigned char enabled)
{
	rtc_write(CMOS_VERBOSE_MAGIC_REG, 0);
	rtc_write(CMOS_VERBOSE_VALUE_REG, enabled);
	rtc_write(CMOS_VERBOSE_CHECK_REG, (unsigned char)(CMOS_VERBOSE_MAGIC ^ enabled ^ CMOS_VERBOSE_SALT));
	rtc_write(CMOS_VERBOSE_MAGIC_REG, CMOS_VERBOSE_MAGIC);
	return verbose_load() == enabled;
}

static unsigned char rtc_encode(unsigned int value, int binary)
{
	if (binary) return (unsigned char)value;
	return (unsigned char)(((value / 10) << 4) | (value % 10));
}

static int parse_fixed_number(const char *text, unsigned int start, unsigned int length, unsigned int *value)
{
	unsigned int i;
	unsigned int result = 0;
	for (i = 0; i < length; i++) {
		char digit = text[start + i];
		if (digit < '0' || digit > '9') return 0;
		result = result * 10 + (unsigned int)(digit - '0');
	}
	*value = result;
	return 1;
}

static int leap_year(unsigned int year)
{
	return !(year % 4) && ((year % 100) || !(year % 400));
}

static void rtc_begin_write(unsigned char *status)
{
	while (rtc_read(0x0A) & 0x80) timer_poll();
	*status = rtc_read(0x0B);
	rtc_write(0x0B, (unsigned char)(*status | 0x80));
}

static void rtc_end_write(unsigned char status)
{
	rtc_write(0x0B, status);
}

static int set_rtc_date(const char *text)
{
	static const unsigned int month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	unsigned int year, month, day, limit;
	unsigned char status;
	if (str_length(text) != 10 || text[4] != '-' || text[7] != '-' ||
		!parse_fixed_number(text, 0, 4, &year) || !parse_fixed_number(text, 5, 2, &month) ||
		!parse_fixed_number(text, 8, 2, &day) || year < 2000 || year > 2099 || month < 1 || month > 12) return 0;
	limit = month_days[month - 1] + (month == 2 && leap_year(year));
	if (day < 1 || day > limit) return 0;
	rtc_begin_write(&status);
	rtc_write(0x07, rtc_encode(day, status & 4));
	rtc_write(0x08, rtc_encode(month, status & 4));
	rtc_write(0x09, rtc_encode(year % 100, status & 4));
	rtc_end_write(status);
	return 1;
}

static int set_rtc_time(const char *text)
{
	unsigned int hour, minute, second;
	unsigned char status;
	unsigned int encoded_hour;
	if (str_length(text) != 8 || text[2] != ':' || text[5] != ':' ||
		!parse_fixed_number(text, 0, 2, &hour) || !parse_fixed_number(text, 3, 2, &minute) ||
		!parse_fixed_number(text, 6, 2, &second) || hour > 23 || minute > 59 || second > 59) return 0;
	rtc_begin_write(&status);
	encoded_hour = hour;
	if (!(status & 2)) {
		unsigned int pm = hour >= 12;
		encoded_hour = hour % 12;
		if (!encoded_hour) encoded_hour = 12;
		encoded_hour = rtc_encode(encoded_hour, status & 4) | (pm ? 0x80 : 0);
	} else encoded_hour = rtc_encode(encoded_hour, status & 4);
	rtc_write(0x04, (unsigned char)encoded_hour);
	rtc_write(0x02, rtc_encode(minute, status & 4));
	rtc_write(0x00, rtc_encode(second, status & 4));
	rtc_end_write(status);
	return 1;
}

static DateTime read_datetime(void)
{
	DateTime result;
	unsigned char status;
	unsigned char hour;
	while (rtc_read(0x0A) & 0x80) timer_poll();
	result.second = rtc_read(0x00);
	result.minute = rtc_read(0x02);
	hour = rtc_read(0x04);
	result.day = rtc_read(0x07);
	result.month = rtc_read(0x08);
	result.year = rtc_read(0x09);
	status = rtc_read(0x0B);
	result.second = rtc_value((unsigned char)result.second, status & 4);
	result.minute = rtc_value((unsigned char)result.minute, status & 4);
	result.hour = rtc_value((unsigned char)(hour & 0x7F), status & 4);
	if (!(status & 2) && (hour & 0x80)) result.hour = (result.hour % 12) + 12;
	result.day = rtc_value((unsigned char)result.day, status & 4);
	result.month = rtc_value((unsigned char)result.month, status & 4);
	result.year = 2000 + rtc_value((unsigned char)result.year, status & 4);
	return result;
}

static int is_root(void)
{
	return current_user == 0;
}

static void command_error(const char *message)
{
	screen_write_color("error: ", 0x0C);
	screen_write(message);
	screen_putchar('\n');
}

static int tokenize(char *line, char **arguments)
{
	char *read = line;
	char *write = line;
	int count = 0;
	while (*read && count < ARG_MAX) {
		int quoted = 0;
		while (*read == ' ' || *read == '\t') read++;
		if (!*read) break;
		arguments[count++] = write;
		while (*read) {
			if (*read == '"') { quoted = !quoted; read++; continue; }
			if (!quoted && (*read == ' ' || *read == '\t')) break;
			*write++ = *read++;
		}
		if (*read) read++;
		*write++ = 0;
	}
	return count;
}

static int find_user(const char *name)
{
	unsigned int i;
	for (i = 0; i < user_count; i++) if (str_equal(users[i].name, name)) return (int)i;
	return -1;
}

static int authenticate_user(int user)
{
	char password[sizeof(users[0].password)];
	screen_write("Password: ");
	keyboard_read_line_hidden(password, sizeof(password));
	if (!str_equal(password, users[user].password)) {
		command_error("authentication failed");
		return 0;
	}
	current_user = (unsigned int)user;
	screen_write("Logged in as ");
	screen_write(users[current_user].name);
	screen_putchar('\n');
	return 1;
}

static int read_new_password(char *password, int capacity)
{
	char confirmation[sizeof(users[0].password)];
	screen_write("New password: ");
	keyboard_read_line_hidden(password, capacity);
	if (!password[0]) { command_error("password cannot be empty"); return 0; }
	screen_write("Confirm password: ");
	keyboard_read_line_hidden(confirmation, sizeof(confirmation));
	if (!str_equal(password, confirmation)) { command_error("passwords do not match"); return 0; }
	return 1;
}

static int find_alias(const char *name)
{
	unsigned int i;
	for (i = 0; i < alias_count; i++) if (str_equal(aliases[i].name, name)) return (int)i;
	return -1;
}

static void print_file(const char *path, int paged)
{
	unsigned int size;
	unsigned int lines = 0;
	unsigned int shown = 0;
	const char *text = fs_read(path, &size);
	unsigned int i;
	if (!text) { command_error("cannot read file"); return; }
	for (i = 0; i < size; i++) {
		screen_putchar(text[i]);
		if (text[i] == '\n') {
			lines++;
			shown++;
			if (paged && shown == 22 && i + 1 < size) {
				int key;
				screen_write("-- more: space/q --");
				key = keyboard_wait_key();
				screen_write("                    \r");
				if (key == 'q' || key == 'Q') break;
				shown = 0;
			}
		}
	}
	if (size && text[size - 1] != '\n') screen_putchar('\n');
	(void)lines;
}

static void command_ls(const char *path)
{
	int directory = fs_resolve(path);
	int count;
	int i;
	if (directory < 0) { command_error("directory not found"); return; }
	{
		FsInfo info;
		fs_get_info(directory, &info);
		if (!info.is_dir) { screen_write(info.name); screen_putchar('\n'); return; }
	}
	count = fs_child_count(directory);
	if (!count) { screen_write("(empty)\n"); return; }
	for (i = 0; i < count; i++) {
		FsInfo info;
		int child = fs_child_at(directory, i);
		fs_get_info(child, &info);
		if (info.is_dir) screen_write_color(info.name, 0x0B);
		else screen_write(info.name);
		if (info.is_dir) screen_putchar('/');
		else {
			screen_write("  ");
			print_unsigned(info.size);
			screen_write(" B");
		}
		screen_putchar('\n');
	}
}

static void command_wc(const char *path)
{
	unsigned int size;
	unsigned int lines = 0;
	unsigned int words = 0;
	unsigned int i;
	int in_word = 0;
	const char *text = fs_read(path, &size);
	if (!text) { command_error("cannot read file"); return; }
	for (i = 0; i < size; i++) {
		if (text[i] == '\n') lines++;
		if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t' || text[i] == '\r') in_word = 0;
		else if (!in_word) { words++; in_word = 1; }
	}
	print_unsigned(lines); screen_putchar(' ');
	print_unsigned(words); screen_putchar(' ');
	print_unsigned(size); screen_putchar(' ');
	screen_write(path); screen_putchar('\n');
}

static void command_lines(const char *command, const char *path, unsigned int requested)
{
	unsigned int size;
	unsigned int total_lines = 1;
	unsigned int line = 0;
	unsigned int i;
	unsigned int start_line = 0;
	const char *text = fs_read(path, &size);
	if (!text) { command_error("cannot read file"); return; }
	for (i = 0; i < size; i++) if (text[i] == '\n') total_lines++;
	if (str_equal(command, "tail")) start_line = total_lines > requested ? total_lines - requested : 0;
	for (i = 0; i < size; i++) {
		if (line >= start_line && (str_equal(command, "tail") || line < requested)) screen_putchar(text[i]);
		if (text[i] == '\n') line++;
	}
	if (size && text[size - 1] != '\n') screen_putchar('\n');
}

static void command_grep(const char *needle, const char *path)
{
	unsigned int size;
	unsigned int start = 0;
	unsigned int i;
	const char *text = fs_read(path, &size);
	if (!text) { command_error("cannot read file"); return; }
	for (i = 0; i <= size; i++) {
		if (i == size || text[i] == '\n') {
			char line[FS_DATA_MAX];
			unsigned int length = i - start;
			unsigned int j;
			if (length >= sizeof(line)) length = sizeof(line) - 1;
			for (j = 0; j < length; j++) line[j] = text[start + j];
			line[length] = 0;
			if (contains(line, needle)) { screen_write(line); screen_putchar('\n'); }
			start = i + 1;
		}
	}
}

static unsigned int directory_bytes(int directory)
{
	unsigned int total = 0;
	int count = fs_child_count(directory);
	int i;
	for (i = 0; i < count; i++) {
		int child = fs_child_at(directory, i);
		FsInfo info;
		fs_get_info(child, &info);
		total += info.is_dir ? directory_bytes(child) : info.size;
	}
	return total;
}

static void command_hex(const char *path)
{
	unsigned int size;
	unsigned int i;
	const char *data = fs_read(path, &size);
	if (!data) { command_error("cannot read file"); return; }
	for (i = 0; i < size; i += 16) {
		unsigned int j;
		print_hex_byte((unsigned char)(i >> 8));
		print_hex_byte((unsigned char)i);
		screen_write("  ");
		for (j = 0; j < 16 && i + j < size; j++) {
			print_hex_byte((unsigned char)data[i + j]);
			screen_putchar(' ');
		}
		screen_write(" | ");
		for (j = 0; j < 16 && i + j < size; j++) {
			char value = data[i + j];
			screen_putchar(value >= 32 && value < 127 ? value : '.');
		}
		screen_putchar('\n');
	}
}

static unsigned int day_of_week(unsigned int day, unsigned int month, unsigned int year)
{
	static const unsigned int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	if (month < 3) year--;
	return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

static void command_calendar(void)
{
	static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
	static const unsigned int month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	DateTime date = read_datetime();
	unsigned int days;
	unsigned int column;
	unsigned int day;
	if (date.month < 1 || date.month > 12) { command_error("RTC date unavailable"); return; }
	days = month_days[date.month - 1];
	if (date.month == 2 && !(date.year % 4) && ((date.year % 100) || !(date.year % 400))) days++;
	screen_write("     "); screen_write(months[date.month - 1]); screen_putchar(' '); print_unsigned(date.year); screen_putchar('\n');
	screen_write("Su Mo Tu We Th Fr Sa\n");
	column = day_of_week(1, date.month, date.year);
	for (day = 0; day < column; day++) screen_write("   ");
	for (day = 1; day <= days; day++) {
		if (day < 10) screen_putchar(' ');
		print_unsigned(day);
		if (++column == 7) { column = 0; screen_putchar('\n'); }
		else screen_putchar(' ');
	}
	screen_putchar('\n');
}

static void command_help(void)
{
	screen_write_color(" DIRECTOS 0.2  |  EVEN BETTER POTATO  |  COMMAND INDEX\n", 0x0B);
	screen_write_color(" FILES  ", 0x0E); screen_write(" - ls, dir, cd, pwd, mkdir, rmdir, touch, rm, del, cp, mv, cat, type, stat, chmod\n");
	screen_write_color(" TEXT   ", 0x0E); screen_write(" - echo, less, more, edit, nano, grep, head, tail, wc, hexedit, clear, cls\n");
	screen_write_color(" SYSTEM ", 0x0E); screen_write(" - version, uname, fastfetch, date, time, cal, uptime, top, free, mem, ps, verbose\n");
	screen_write_color(" STORAGE", 0x0E); screen_write(" - df, du, format, mount, chkdsk, fsck\n");
	screen_write_color(" USERS  ", 0x0E); screen_write(" - login, whoami, useradd, passwd, su, sudo\n");
	screen_write_color(" NETWORK", 0x0E); screen_write(" - ping, ifconfig, ip, ssh\n");
	screen_write_color(" TOOLS  ", 0x0E); screen_write(" - sleep, history, alias, man, reboot, shutdown\n");
	screen_write("Use man COMMAND for syntax. Accounts and passwords reset on reboot.\n");
}

static void fastfetch_prefix(const char *logo, const char *label)
{
	screen_write_color(logo, 0x0B);
	screen_write_color(" | ", 0x08);
	screen_write_color(label, 0x0E);
	screen_write(": ");
}

static void command_fastfetch(void)
{
	char cpu_brand[49];
	unsigned int display_id, ethernet_id, ram_mib;
	DateTime date = read_datetime();
	read_cpu_brand(cpu_brand);
	find_pci_devices(&display_id, &ethernet_id);
	ram_mib = usable_ram_mib();

	fastfetch_prefix("       .--.       ", "User");
	screen_write(users[current_user].name); screen_putchar('\n');
	fastfetch_prefix("      |o_o |      ", "GPU");
	if (display_id) {
		print_hex_word(display_id); screen_putchar(':'); print_hex_word(display_id >> 16);
		screen_write(" (PCI display; no acceleration driver)\n");
	} else screen_write("VGA text framebuffer; no graphics driver\n");
	fastfetch_prefix("      |:_/ |      ", "CPU");
	screen_write(cpu_brand); screen_putchar('\n');
	fastfetch_prefix("     //   \\\\     ", "Version");
	screen_write("DirectOs 0.2\n");
	fastfetch_prefix("    (|     | )    ", "Codename");
	screen_write("Even Better Potato\n");
	fastfetch_prefix("     `-.__.-'     ", "RAM");
	if (ram_mib) { print_unsigned(ram_mib); screen_write(" MiB usable (BIOS E820)\n"); }
	else screen_write("unavailable (BIOS memory map not provided)\n");
	fastfetch_prefix("                  ", "IP");
	if (ethernet_id) {
		screen_write("unavailable (no NIC driver); Ethernet PCI ");
		print_hex_word(ethernet_id); screen_putchar(':'); print_hex_word(ethernet_id >> 16); screen_putchar('\n');
		screen_write("                  | Come Back on the future on a new Version\n");
	} else screen_write("unavailable (no PCI Ethernet device detected)\n");
	fastfetch_prefix("                  ", "Date/Time");
	print_unsigned(date.year); screen_putchar('-'); print_two(date.month); screen_putchar('-'); print_two(date.day);
	screen_putchar(' '); print_two(date.hour); screen_putchar(':'); print_two(date.minute); screen_putchar(':'); print_two(date.second); screen_putchar('\n');
}

static void command_man(const char *name)
{
	static const struct { const char *name; const char *text; } entries[] = {
		{ "ls", "ls [PATH] - list files and directories" }, { "cd", "cd [PATH] - change directory; no argument selects /" },
		{ "pwd", "pwd - print the current directory" }, { "mkdir", "mkdir PATH - create a directory" },
		{ "rmdir", "rmdir PATH - remove an empty directory" }, { "touch", "touch FILE - create an empty file" },
		{ "rm", "rm FILE - remove a file" }, { "cp", "cp SOURCE DEST - copy a file" },
		{ "mv", "mv SOURCE DEST - rename or move a file or directory" }, { "cat", "cat FILE - print a text file" },
		{ "echo", "echo TEXT... - print text" }, { "more", "more FILE - page through a text file" },
		{ "edit", "edit FILE - append lines; enter . on a line to save" }, { "grep", "grep TEXT FILE - print matching lines" },
		{ "head", "head FILE [LINES] - print the first lines" }, { "tail", "tail FILE [LINES] - print the last lines" },
		{ "wc", "wc FILE - count lines, words, and bytes" }, { "hexedit", "hexedit FILE - display file bytes in hexadecimal" },
		{ "history", "history - show entered commands" }, { "alias", "alias [NAME [COMMAND]] - list or set a command alias" },
		{ "stat", "stat PATH - show RAMFS metadata" }, { "df", "df - show RAMFS node and data capacity" },
		{ "du", "du [PATH] - count file bytes below a path" }, { "format", "format yes - reset the volatile RAM filesystem" },
		{ "mount", "mount - report available block devices" }, { "fsck", "fsck - check RAMFS consistency" },
		{ "chmod", "chmod OCTAL FILE - set RAMFS permission metadata" }, { "whoami", "whoami - print the current shell account" },
		{ "login", "login [USER] - authenticate and switch to an account" },
		{ "useradd", "useradd NAME - create a volatile account (root only)" }, { "passwd", "passwd [USER] - securely change a password" },
		{ "su", "su [USER] - authenticate and switch shell account" }, { "ps", "ps - show the kernel shell task" },
		{ "kill", "kill PID - unavailable without a process scheduler" }, { "free", "free - show managed RAMFS usage; no heap allocator exists" },
		{ "top", "top - show a one-shot kernel status snapshot" }, { "uptime", "uptime - show PIT-based elapsed time" },
		{ "date", "date [YYYY-MM-DD] - read or set the RTC date" }, { "time", "time [HH:MM:SS] - read or set the RTC time" },
		{ "cal", "cal - print the RTC month's calendar" }, { "sleep", "sleep SECONDS - wait using PIT polling" },
		{ "verbose", "verbose [on|off] - toggle boot/network status messages; saves to virtual CMOS" },
		{ "ping", "ping IPv4 [BYTES] - send one ICMP echo; payload defaults to 56 bytes (max 1472)" }, { "ifconfig", "ifconfig - report network interface and static IPv4 configuration" },
		{ "ssh", "ssh HOST - unavailable; TCP and the SSH protocol are not implemented" },
		{ "man", "man COMMAND - show a command summary" }, { "fastfetch", "fastfetch - show kernel-visible system information" }
	};
	unsigned int i;
	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (str_equal(name, entries[i].name)) { screen_write(entries[i].text); screen_putchar('\n'); return; }
	}
	screen_write("No manual entry; type help for the command list.\n");
}

static void dispatch(char **args, int count);

static void dispatch(char **args, int count)
{
	const char *command;
	int alias_index;
	if (!count) return;
	command = args[0];
	alias_index = find_alias(command);
	if (alias_index >= 0) command = aliases[alias_index].command;

	if (str_equal(command, "help")) command_help();
	else if (str_equal(command, "clear") || str_equal(command, "cls")) screen_clear();
	else if (str_equal(command, "version") || str_equal(command, "ver")) screen_write("DirectOs 0.2 (codename: Even Better Potato)\n");
	else if (str_equal(command, "uname")) screen_write("DirectOS i386 32-bit Protected Mode\n");
	else if (str_equal(command, "pwd")) { char path[FS_PATH_MAX]; fs_get_cwd(path, sizeof(path)); screen_write(path); screen_putchar('\n'); }
	else if (str_equal(command, "ls") || str_equal(command, "dir")) command_ls(count > 1 ? args[1] : ".");
	else if (str_equal(command, "cd")) {
		if (count < 2) fs_chdir("/");
		else if (!fs_chdir(args[1])) command_error("directory not found");
	}
	else if (str_equal(command, "mkdir")) {
		if (count < 2 || !fs_create(args[1], 1)) command_error("mkdir failed (name exists, invalid path, or RAMFS full)");
	}
	else if (str_equal(command, "rmdir")) {
		if (count < 2 || !fs_remove(args[1], 1)) command_error("directory is missing, not empty, or current root");
	}
	else if (str_equal(command, "touch")) {
		if (count < 2) command_error("usage: touch FILE");
		else if (fs_resolve(args[1]) >= 0) {
			FsInfo info; fs_get_info(fs_resolve(args[1]), &info);
			if (info.is_dir) command_error("is a directory");
		} else if (!fs_create(args[1], 0)) command_error("touch failed (invalid path or RAMFS full)");
	}
	else if (str_equal(command, "rm") || str_equal(command, "del")) {
		if (count < 2 || !fs_remove(args[1], 0)) command_error("file not found or is a directory");
	}
	else if (str_equal(command, "cp")) {
		unsigned int size;
		const char *data;
		char destination[FS_PATH_MAX];
		if (count < 3) command_error("usage: cp SOURCE DEST");
		else if (!(data = fs_read(args[1], &size))) command_error("source file not found");
		else {
			int target = fs_resolve(args[2]);
			if (target >= 0) {
				FsInfo target_info;
				FsInfo source_info;
				fs_get_info(target, &target_info);
				if (!target_info.is_dir) str_copy(destination, args[2], sizeof(destination));
				else {
					fs_get_info(fs_resolve(args[1]), &source_info);
					str_copy(destination, args[2], sizeof(destination));
					if (str_length(destination) + str_length(source_info.name) + 2 < sizeof(destination)) {
						if (destination[str_length(destination) - 1] != '/') str_append(destination, "/", sizeof(destination));
						str_append(destination, source_info.name, sizeof(destination));
					}
				}
			} else str_copy(destination, args[2], sizeof(destination));
			if (!fs_write(destination, data, size)) command_error("copy failed (destination invalid or RAMFS full)");
		}
	}
	else if (str_equal(command, "mv")) {
		if (count < 3 || !fs_move(args[1], args[2])) command_error("move failed");
	}
	else if (str_equal(command, "cat") || str_equal(command, "type")) {
		if (count < 2) command_error("usage: cat FILE"); else print_file(args[1], 0);
	}
	else if (str_equal(command, "less") || str_equal(command, "more")) {
		if (count < 2) command_error("usage: more FILE"); else print_file(args[1], 1);
	}
	else if (str_equal(command, "echo")) {
		int i;
		for (i = 1; i < count; i++) { if (i > 1) screen_putchar(' '); screen_write(args[i]); }
		screen_putchar('\n');
	}
	else if (str_equal(command, "head") || str_equal(command, "tail")) {
		unsigned int lines = count > 2 ? parse_unsigned(args[2], 10) : 10;
		if (count < 2) command_error("usage: head|tail FILE [LINES]");
		else command_lines(command, args[1], lines ? lines : 10);
	}
	else if (str_equal(command, "grep")) {
		if (count < 3) command_error("usage: grep TEXT FILE"); else command_grep(args[1], args[2]);
	}
	else if (str_equal(command, "wc")) {
		if (count < 2) command_error("usage: wc FILE"); else command_wc(args[1]);
	}
	else if (str_equal(command, "edit") || str_equal(command, "nano")) {
		char buffer[FS_DATA_MAX];
		char line[128];
		unsigned int used = 0;
		unsigned int existing_size;
		const char *existing;
		if (count < 2) command_error("usage: edit FILE");
		else {
			buffer[0] = 0;
			existing = fs_read(args[1], &existing_size);
			if (existing) {
				unsigned int i;
				for (i = 0; i < (unsigned int)existing_size && used + 1 < sizeof(buffer); i++) buffer[used++] = existing[i];
			}
			screen_write("Enter text; a line containing only . saves. New text is appended.\n");
			for (;;) {
				screen_write("> ");
				keyboard_read_line(line, sizeof(line));
				if (str_equal(line, ".")) break;
				if (used + str_length(line) + 1 >= sizeof(buffer)) { command_error("file size limit reached"); break; }
				str_append(buffer, line, sizeof(buffer));
				used = str_length(buffer);
				buffer[used++] = '\n';
				buffer[used] = 0;
			}
			if (!fs_write(args[1], buffer, used)) command_error("save failed (RAMFS full)");
		}
	}
	else if (str_equal(command, "hexedit")) {
		if (count < 2) command_error("usage: hexedit FILE"); else command_hex(args[1]);
	}
	else if (str_equal(command, "history")) {
		int i;
		for (i = 0; i < keyboard_history_count(); i++) { print_unsigned((unsigned int)(i + 1)); screen_write("  "); screen_write(keyboard_history_get(i)); screen_putchar('\n'); }
	}
	else if (str_equal(command, "alias")) {
		if (count == 1) {
			unsigned int i;
			for (i = 0; i < alias_count; i++) { screen_write(aliases[i].name); screen_write(" = "); screen_write(aliases[i].command); screen_putchar('\n'); }
		} else if (count == 2) {
			alias_index = find_alias(args[1]);
			if (alias_index >= 0) { screen_write(aliases[alias_index].name); screen_write(" = "); screen_write(aliases[alias_index].command); screen_putchar('\n'); }
			else command_error("alias not found");
		} else {
			alias_index = find_alias(args[1]);
			if (alias_index < 0 && alias_count < ALIAS_MAX) alias_index = (int)alias_count++;
			if (alias_index < 0) command_error("alias table full");
			else { str_copy(aliases[alias_index].name, args[1], sizeof(aliases[alias_index].name)); str_copy(aliases[alias_index].command, args[2], sizeof(aliases[alias_index].command)); }
		}
	}
	else if (str_equal(command, "stat")) {
		FsInfo info;
		int node = count > 1 ? fs_resolve(args[1]) : -1;
		if (node < 0) command_error("path not found");
		else {
			fs_get_info(node, &info);
			screen_write("Name: "); screen_write(info.name); screen_putchar('\n');
			screen_write("Type: "); screen_write(info.is_dir ? "directory\n" : "file\n");
			screen_write("Size: "); print_unsigned(info.size); screen_write(" bytes\nNode: "); print_unsigned(info.node_id); screen_write(" (RAMFS slot, not disk sector)\nPermissions: "); print_unsigned(info.permissions); screen_putchar('\n');
		}
	}
	else if (str_equal(command, "df")) {
		unsigned int used, free_nodes, bytes;
		fs_stats(&used, &free_nodes, &bytes);
		screen_write("RAMFS nodes: "); print_unsigned(used); screen_putchar('/'); print_unsigned(FS_NODE_MAX);
		screen_write(" used, "); print_unsigned(free_nodes); screen_write(" free; "); print_unsigned(bytes); screen_write("/"); print_unsigned(FS_NODE_MAX * FS_DATA_MAX); screen_write(" data bytes\nNo disk is mounted.\n");
	}
	else if (str_equal(command, "du")) {
		int node = count > 1 ? fs_resolve(args[1]) : fs_resolve(".");
		if (node < 0) command_error("path not found");
		else { print_unsigned(directory_bytes(node)); screen_write(" bytes\n"); }
	}
	else if (str_equal(command, "format")) {
		if (!is_root()) command_error("root required");
		else if (count < 2 || !str_equal(args[1], "yes")) screen_write("This clears the volatile RAMFS. Run: format yes\n");
		else { fs_format(); screen_write("RAMFS reset; no disk was formatted.\n"); }
	}
	else if (str_equal(command, "mount")) screen_write("No ATA, floppy, or block-device driver is installed.\n");
	else if (str_equal(command, "chkdsk") || str_equal(command, "fsck")) screen_write(fs_check() ? "RAMFS check: clean\n" : "RAMFS check: consistency error\n");
	else if (str_equal(command, "chmod")) {
		unsigned int mode;
		if (count < 3) command_error("usage: chmod OCTAL FILE");
		else { mode = parse_unsigned(args[1], 8); if (!fs_chmod(args[2], (unsigned char)mode)) command_error("chmod failed (permissions are metadata only)"); }
	}
	else if (str_equal(command, "whoami")) { screen_write(users[current_user].name); screen_putchar('\n'); }
	else if (str_equal(command, "useradd")) {
		int user;
		char password[sizeof(users[0].password)];
		if (!is_root()) command_error("root required");
		else if (count < 2) command_error("usage: useradd NAME");
		else if (user_count >= USER_MAX || find_user(args[1]) >= 0 || str_length(args[1]) >= sizeof(users[0].name)) command_error("invalid name, user exists, or account table full");
		else if (read_new_password(password, sizeof(password))) {
			user = (int)user_count++;
			str_copy(users[user].name, args[1], sizeof(users[user].name));
			str_copy(users[user].password, password, sizeof(users[user].password));
			screen_write("Account created.\n");
		}
	}
	else if (str_equal(command, "passwd")) {
		int user = (int)current_user;
		char password[sizeof(users[0].password)];
		if (count > 2) command_error("usage: passwd [USER]");
		else if (count == 2 && !is_root()) command_error("root required to change another user's password");
		else {
			if (count == 2) user = find_user(args[1]);
			if (user < 0) command_error("user not found");
			else if (read_new_password(password, sizeof(password))) {
				str_copy(users[user].password, password, sizeof(users[user].password));
				screen_write("Password changed in volatile memory.\n");
			}
		}
	}
	else if (str_equal(command, "login")) {
		char username[sizeof(users[0].name)];
		int user;
		if (count > 2) command_error("usage: login [USER]");
		else {
			if (count == 2) str_copy(username, args[1], sizeof(username));
			else { screen_write("Username: "); keyboard_read_line(username, sizeof(username)); }
			user = find_user(username);
			if (user < 0) command_error("user not found");
			else authenticate_user(user);
		}
	}
	else if (str_equal(command, "su")) {
		int user = count > 1 ? find_user(args[1]) : 0;
		if (count > 2) command_error("usage: su [USER]");
		else if (user < 0) command_error("user not found");
		else authenticate_user(user);
	}
	else if (str_equal(command, "sudo")) {
		if (!is_root()) command_error("sudo is unavailable without privilege separation (switch with su)");
		else if (count < 2) command_error("usage: sudo COMMAND");
		else dispatch(args + 1, count - 1);
	}
	else if (str_equal(command, "ps")) screen_write("PID  STATE  COMMAND\n1    running DirectOS shell\nProcess isolation and scheduler are not implemented.\n");
	else if (str_equal(command, "kill")) screen_write("No user processes exist; kill is not supported.\n");
	else if (str_equal(command, "free") || str_equal(command, "mem")) {
		unsigned int used, free_nodes, bytes;
		fs_stats(&used, &free_nodes, &bytes);
		screen_write("Managed RAM: heap allocator unavailable.\nRAMFS content: "); print_unsigned(bytes); screen_write(" bytes; "); print_unsigned(free_nodes); screen_write(" of "); print_unsigned(FS_NODE_MAX); screen_write(" file nodes free.\n");
	}
	else if (str_equal(command, "top")) { screen_write("DirectOS task snapshot\nCPU: timer polling active; load accounting unavailable\n"); dispatch((char *[]){ "uptime" }, 1); dispatch((char *[]){ "ps" }, 1); }
	else if (str_equal(command, "uptime")) { unsigned int seconds = timer_seconds(); screen_write("up "); print_unsigned(seconds / 3600); screen_write("h "); print_unsigned((seconds / 60) % 60); screen_write("m "); print_unsigned(seconds % 60); screen_write("s\n"); }
	else if (str_equal(command, "date") || str_equal(command, "time")) {
		DateTime date;
		if (count > 1) {
			int updated = str_equal(command, "date") ? set_rtc_date(args[1]) : set_rtc_time(args[1]);
			if (!updated) command_error(str_equal(command, "date") ? "use date YYYY-MM-DD (2000-2099)" : "use time HH:MM:SS");
		} else {
			date = read_datetime();
			if (str_equal(command, "date")) { print_two(date.year); screen_putchar('-'); print_two(date.month); screen_putchar('-'); print_two(date.day); screen_putchar('\n'); }
			else { print_two(date.hour); screen_putchar(':'); print_two(date.minute); screen_putchar(':'); print_two(date.second); screen_putchar('\n'); }
		}
	}
	else if (str_equal(command, "cal")) command_calendar();
	else if (str_equal(command, "sleep")) {
		unsigned int seconds = count > 1 ? parse_unsigned(args[1], 10) : 1;
		unsigned int start = timer_seconds();
		if (seconds > 3600) command_error("maximum sleep is 3600 seconds");
		else while ((unsigned int)(timer_seconds() - start) < seconds) timer_poll();
	}
	else if (str_equal(command, "verbose")) {
		unsigned char desired;
		if (count == 1) {
			boot_status("OK", verbose_enabled ? "verbose mode is enabled and persistent" : "verbose mode is disabled");
			if (!verbose_enabled) screen_write("Verbose mode is disabled. Use verbose on to enable it.\n");
		} else if (count != 2 || (!str_equal(args[1], "on") && !str_equal(args[1], "off")))
			screen_write("Usage: verbose [on|off]\n");
		else {
			desired = str_equal(args[1], "on") ? 1 : 0;
			if (!verbose_save(desired)) screen_write("[Failed] unable to save verbose setting to virtual CMOS\n");
			else {
				verbose_enabled = desired;
				network_set_verbose(verbose_enabled);
				if (verbose_enabled) boot_status("OK", "verbose mode enabled and saved persistently");
				else screen_write("[OK] verbose mode disabled and saved persistently\n");
			}
		}
	}
	else if (str_equal(command, "shutdown")) {
		boot_status("Waiting", "sending virtual ACPI poweroff request");
		boot_status("STOPPED", "kernel halted for poweroff");
		screen_write("Powering off (emulator ACPI request)...\n");
		port_out_word(0x604, 0x2000);
		port_out_word(0xB004, 0x2000);
		port_out_word(0x4004, 0x3400);
		for (;;) __asm__ volatile ("cli; hlt");
	}
	else if (str_equal(command, "reboot")) {
		unsigned int wait;
		boot_status("Waiting", "requesting keyboard-controller reset");
		screen_write("Rebooting...\n");
		__asm__ volatile ("cli");
		for (wait = 0; wait < 100000; wait++) if (!(port_in(0x64) & 2)) break;
		port_out(0x64, 0xFE);
		boot_status("STOPPED", "CPU halted while waiting for reset");
		for (;;) __asm__ volatile ("hlt");
	}
	else if (str_equal(command, "ssh")) screen_write("SSH client not implemented; TCP and cryptography are not available yet.\n");
	else if (str_equal(command, "ping")) {
		unsigned char address[4];
		unsigned short payload_size = 56;
		unsigned int elapsed_ms = 0;
		int result;
		if ((count != 2 && count != 3) || !parse_ipv4_address(args[1], address) ||
			(count == 3 && !parse_ping_size(args[2], &payload_size)))
			screen_write("Usage: ping IPv4 [PAYLOAD_BYTES] (0-1472)\n");
		else {
			screen_write("PING ");
			print_unsigned(address[0]); screen_putchar('.'); print_unsigned(address[1]); screen_putchar('.');
			print_unsigned(address[2]); screen_putchar('.'); print_unsigned(address[3]); screen_write(" ... ");
			result = network_ping(address, payload_size, &elapsed_ms);
			if (result < 0) screen_write("no supported Ethernet adapter detected\n");
			else if (!result) screen_write("request timed out\n");
			else {
				screen_write("reply, bytes=");
				print_unsigned(payload_size);
				screen_write(" time=");
				print_unsigned(elapsed_ms);
				screen_write(" ms\n");
			}
		}
	}
	else if (str_equal(command, "ifconfig") || str_equal(command, "ip")) {
		unsigned char mac[6];
		unsigned int i;
		if (!network_init()) screen_write("No supported RTL8139 or Intel PRO/1000 adapter detected.\n");
		else {
			network_get_mac(mac);
			screen_write(network_driver_name());
			screen_write(": driver ready\ninet 10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2\nMAC: ");
			for (i = 0; i < 6; i++) {
				print_hex_byte(mac[i]);
				if (i != 5) screen_putchar(':');
			}
			screen_putchar('\n');
		}
	}
	else if (str_equal(command, "fastfetch")) command_fastfetch();
	else if (str_equal(command, "man")) {
		if (count < 2) screen_write("Usage: man COMMAND. See help for the command list.\n");
		else command_man(args[1]);
	}
	else command_error("unknown command; type help");
}

void kernel_main(void)
{
	char line[INPUT_SIZE];
	char path[FS_PATH_MAX];
	char *arguments[ARG_MAX];
	verbose_enabled = verbose_load();
	network_set_verbose(verbose_enabled);
	screen_init();
	boot_status("OK", "VGA text console initialized");
	if (verbose_enabled) boot_status("OK", "persistent verbose setting restored from CMOS");
	timer_init();
	boot_status("OK", "PIT timer initialized at approximately 1 kHz");
	fs_init();
	boot_status("OK", "RAM filesystem initialized");
	if (!network_init()) boot_status("Failed", "network unavailable; shell will continue");
	else boot_status("OK", "network interface initialized");
	screen_write_color("DirectOs 0.2\n", 0x0B);
	screen_write("Codename: Even Better Potato | Type help for commands.\n");
	for (;;) {
		fs_get_cwd(path, sizeof(path));
		screen_write_color(users[current_user].name, 0x0A);
		screen_write(":");
		screen_write(path);
		screen_write("> ");
		keyboard_read_line(line, sizeof(line));
		dispatch(arguments, tokenize(line, arguments));
		screen_write_color("------------------------------------------------------------------------\n", 0x08);
	}
}
