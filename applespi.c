/**
 * The keyboard and touchpad controller on the MacBook8,1, MacBook9,1 and
 * MacBookPro12,1 can be driven either by USB or SPI. However the USB pins
 * are only connected on the MacBookPro12,1, all others need this driver.
 * The interface is selected using ACPI methods:
 *
 * * UIEN ("USB Interface Enable"): If invoked with argument 1, disables SPI
 *   and enables USB. If invoked with argument 0, disables USB.
 * * UIST ("USB Interface Status"): Returns 1 if USB is enabled, 0 otherwise.
 * * SIEN ("SPI Interface Enable"): If invoked with argument 1, disables USB
 *   and enables SPI. If invoked with argument 0, disables SPI.
 * * SIST ("SPI Interface Status"): Returns 1 if SPI is enabled, 0 otherwise.
 * * ISOL: Resets the four GPIO pins used for SPI. Intended to be invoked with
 *   argument 1, then once more with argument 0.
 *
 * UIEN and UIST are only provided on the MacBookPro12,1.
 */

#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/spinlock.h>
#include <linux/crc16.h>

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-polldev.h>

#define APPLESPI_PACKET_SIZE    256
#define APPLESPI_STATUS_SIZE    4

#define PACKET_KEYBOARD         288
#define PACKET_TOUCHPAD         544
#define PACKET_NOTHING          53312

#define MAX_ROLLOVER 		6

#define MAX_FINGERS		6
#define MAX_FINGER_ORIENTATION	16384

#define APPLE_FLAG_FKEY		0x01

static unsigned int fnmode = 1;
module_param(fnmode, uint, 0644);
MODULE_PARM_DESC(fnmode, "Mode of fn key on Apple keyboards (0 = disabled, "
		"[1] = fkeyslast, 2 = fkeysfirst)");

static unsigned int iso_layout = 0;
module_param(iso_layout, uint, 0644);
MODULE_PARM_DESC(iso_layout, "Enable/Disable hardcoded ISO-layout of the keyboard. "
		"(0 = disabled, [1] = enabled)");

struct keyboard_protocol {
	u16		packet_type;
	u8		unknown1[9];
	u8		counter;
	u8		unknown2[5];
	u8		modifiers;
	u8		unknown3;
	u8		keys_pressed[6];
	u8		fn_pressed;
	u16		crc_16;
	u8		unused[228];
};

/* trackpad finger structure, le16-aligned */
struct tp_finger {
	__le16 origin;          /* zero when switching track finger */
	__le16 abs_x;           /* absolute x coodinate */
	__le16 abs_y;           /* absolute y coodinate */
	__le16 rel_x;           /* relative x coodinate */
	__le16 rel_y;           /* relative y coodinate */
	__le16 tool_major;      /* tool area, major axis */
	__le16 tool_minor;      /* tool area, minor axis */
	__le16 orientation;     /* 16384 when point, else 15 bit angle */
	__le16 touch_major;     /* touch area, major axis */
	__le16 touch_minor;     /* touch area, minor axis */
	__le16 unused[2];       /* zeros */
	__le16 pressure;        /* pressure on forcetouch touchpad */
	__le16 multi;           /* one finger: varies, more fingers: constant */
	__le16 padding;
} __attribute__((packed,aligned(2)));

struct touchpad_protocol {
	u16			packet_type;
	u8			unknown1[4];
	u8			number_of_fingers;
	u8			unknown2[4];
	u8			counter;
	u8			unknown3[2];
	u8			number_of_fingers2;
	u8			unknown[2];
	u8			clicked;
	u8			rel_x;
	u8			rel_y;
	u8			unknown4[44];
	struct tp_finger	fingers[MAX_FINGERS];
	u8			unknown5[208];
};

struct spi_settings {
	u64	spi_sclk_period;	/* period in ns */
	u64	spi_word_size;   	/* in number of bits */
	u64	spi_bit_order;   	/* 1 = MSB_FIRST, 0 = LSB_FIRST */
	u64	spi_spo;        	/* clock polarity: 0 = low, 1 = high */
	u64	spi_sph;		/* clock phase: 0 = first, 1 = second */
	u64	spi_cs_delay;    	/* in 10us (?) */
	u64	reset_a2r_usec;  	/* active-to-receive delay? */
	u64	reset_rec_usec;  	/* ? (cur val: 10) */
};

struct applespi_acpi_map_entry {
	char *name;
	size_t field_offset;
};

static const struct applespi_acpi_map_entry applespi_spi_settings_map[] = {
	{ "spiSclkPeriod", offsetof(struct spi_settings, spi_sclk_period) },
	{ "spiWordSize",   offsetof(struct spi_settings, spi_word_size) },
	{ "spiBitOrder",   offsetof(struct spi_settings, spi_bit_order) },
	{ "spiSPO",        offsetof(struct spi_settings, spi_spo) },
	{ "spiSPH",        offsetof(struct spi_settings, spi_sph) },
	{ "spiCSDelay",    offsetof(struct spi_settings, spi_cs_delay) },
	{ "resetA2RUsec",  offsetof(struct spi_settings, reset_a2r_usec) },
	{ "resetRecUsec",  offsetof(struct spi_settings, reset_rec_usec) },
};

struct applespi_tp_info {
	int	x_min;
	int	x_max;
	int	y_min;
	int	y_max;
};

struct applespi_data {
	struct spi_device		*spi;
	struct spi_settings		spi_settings;
	struct input_dev		*keyboard_input_dev;
	struct input_dev		*touchpad_input_dev;

	u8				*tx_buffer;
	u8				*tx_status;
	u8				*rx_buffer;

	const struct applespi_tp_info	*tp_info;

	u8				last_keys_pressed[MAX_ROLLOVER];
	u8				last_keys_fn_pressed[MAX_ROLLOVER];
	u8				last_fn_pressed;
	struct input_mt_pos		pos[MAX_FINGERS];
	int				slots[MAX_FINGERS];
	acpi_handle			handle;
	int				gpe;
	acpi_handle			sien;
	acpi_handle			sist;

	struct spi_transfer		dl_t;
	struct spi_transfer		rd_t;
	struct spi_message		rd_m;

	struct spi_transfer		wr_t;
	struct spi_transfer		st_t;
	struct spi_message		wr_m;

	bool				want_cl_led_on;
	bool				have_cl_led_on;
	unsigned			led_msg_cntr;
	spinlock_t			led_msg_lock;
	bool				led_msg_queued;
};

static const unsigned char applespi_scancodes[] = {
	0,  0,  0,  0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_102ND,
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

struct applespi_key_translation {
	u16 from;
	u16 to;
	u8 flags;
};

static const struct applespi_key_translation applespi_fn_codes[] = {
	{ KEY_BACKSPACE, KEY_DELETE },
	{ KEY_ENTER,	KEY_INSERT },
	{ KEY_F1,	KEY_BRIGHTNESSDOWN, APPLE_FLAG_FKEY },
	{ KEY_F2,	KEY_BRIGHTNESSUP,   APPLE_FLAG_FKEY },
	{ KEY_F3,	KEY_SCALE,          APPLE_FLAG_FKEY },
	{ KEY_F4,	KEY_DASHBOARD,      APPLE_FLAG_FKEY },
	{ KEY_F5,	KEY_KBDILLUMDOWN,   APPLE_FLAG_FKEY },
	{ KEY_F6,	KEY_KBDILLUMUP,     APPLE_FLAG_FKEY },
	{ KEY_F7,	KEY_PREVIOUSSONG,   APPLE_FLAG_FKEY },
	{ KEY_F8,	KEY_PLAYPAUSE,      APPLE_FLAG_FKEY },
	{ KEY_F9,	KEY_NEXTSONG,       APPLE_FLAG_FKEY },
	{ KEY_F10,	KEY_MUTE,           APPLE_FLAG_FKEY },
	{ KEY_F11,	KEY_VOLUMEDOWN,     APPLE_FLAG_FKEY },
	{ KEY_F12,	KEY_VOLUMEUP,       APPLE_FLAG_FKEY },
	{ KEY_RIGHT,	KEY_END },
	{ KEY_LEFT,	KEY_HOME },
	{ KEY_DOWN,	KEY_PAGEDOWN },
	{ KEY_UP,	KEY_PAGEUP },
	{ },
};

static const struct applespi_key_translation apple_iso_keyboard[] = {
	{ KEY_GRAVE,	KEY_102ND },
	{ KEY_102ND,	KEY_GRAVE },
	{ },
};

static u8 *acpi_dsm_uuid = "a0b5b7c6-1318-441c-b0c9-fe695eaf949b";

static struct applespi_tp_info applespi_macbookpro131_info = { -6243, 6749, -170, 7685 };
static struct applespi_tp_info applespi_macbookpro133_info = { -7456, 7976, -163, 9283 };
// MacBook11, MacBook12
static struct applespi_tp_info applespi_default_info = { -4828, 5345, -203, 6803 };

static const struct dmi_system_id applespi_touchpad_infos[] = {
	{
		.ident = "Apple MacBookPro13,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro13,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,2")
		},
		.driver_data = &applespi_macbookpro131_info,	// same touchpad
	},
	{
		.ident = "Apple MacBookPro13,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple Generic MacBook(Pro)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
		},
		.driver_data = &applespi_default_info,
	},
};

u8 *applespi_init_commands[] = {
	"\x40\x02\x00\x00\x00\x00\x0C\x00\x52\x02\x00\x00\x02\x00\x02\x00\x02\x01\x7B\x11\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x23\xAB",
};

u8 *applespi_caps_lock_led_cmd = "\x40\x01\x00\x00\x00\x00\x0C\x00\x51\x01\x00\x00\x02\x00\x02\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x66\x6a";

static void
applespi_setup_read_txfr(struct applespi_data *applespi,
			 struct spi_transfer *rd_t)
{
	memset(rd_t, 0, sizeof *rd_t);

	rd_t->rx_buf = applespi->rx_buffer;
	rd_t->len = APPLESPI_PACKET_SIZE;
	rd_t->delay_usecs = applespi->spi_settings.spi_cs_delay;
}

static void
applespi_setup_write_txfr(struct applespi_data *applespi,
			  struct spi_transfer *wr_t, struct spi_transfer *st_t)
{
	memset(wr_t, 0, sizeof *wr_t);
	memset(st_t, 0, sizeof *st_t);

	wr_t->tx_buf = applespi->tx_buffer;
	wr_t->len = APPLESPI_PACKET_SIZE;
	wr_t->delay_usecs = applespi->spi_settings.spi_cs_delay;

	st_t->rx_buf = applespi->tx_status;
	st_t->len = APPLESPI_STATUS_SIZE;
	st_t->delay_usecs = applespi->spi_settings.spi_cs_delay;
}

static void
applespi_setup_spi_message(struct spi_message *message, int num_txfrs, ...)
{
	va_list txfrs;

	spi_message_init(message);

	va_start(txfrs, num_txfrs);
	while (num_txfrs-- > 0)
		spi_message_add_tail(va_arg(txfrs, struct spi_transfer *),
				     message);
	va_end(txfrs);
}

static int
applespi_sync(struct applespi_data *applespi, struct spi_message *message)
{
	struct spi_device *spi;
	int status;

	spi = applespi->spi;

	status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}

static int
applespi_async(struct applespi_data *applespi, struct spi_message *message,
	       void (*complete)(void *))
{
	message->complete = complete;
	message->context = applespi;

	return spi_async(applespi->spi, message);
}

static inline void
applespi_check_write_status(struct applespi_data *applespi, int sts)
{
	u8 sts_ok[] = { 0xac, 0x27, 0x68, 0xd5 };

	if (sts < 0)
		pr_warn("Error writing to device: %d\n", sts);
	else if (memcmp(applespi->tx_status, sts_ok, APPLESPI_STATUS_SIZE) != 0)
		pr_warn("Error writing to device: %x %x %x %x\n",
			applespi->tx_status[0], applespi->tx_status[1],
			applespi->tx_status[2], applespi->tx_status[3]);
}

static inline ssize_t
applespi_sync_write_and_response(struct applespi_data *applespi)
{
	/*
	The Windows driver always seems to do a 256 byte write, followed
	by a 4 byte read with CS still the same, followed by a toggling of
	CS and a 256 byte read for the real response.
	*/
	struct spi_transfer t1;
	struct spi_transfer t2;
	struct spi_transfer t3;
	struct spi_message m;
	ssize_t ret;

	applespi_setup_write_txfr(applespi, &t1, &t2);
	t2.cs_change = 1;
	applespi_setup_read_txfr(applespi, &t3);
	applespi_setup_spi_message(&m, 3, &t1, &t2, &t3);

	ret = applespi_sync(applespi, &m);

#ifdef DEBUG_ALL_WRITE
	print_hex_dump(KERN_INFO, "applespi: write  ", DUMP_PREFIX_NONE,
		       32, 1, applespi->tx_buffer, APPLESPI_PACKET_SIZE, false);
	print_hex_dump(KERN_INFO, "applespi: status ", DUMP_PREFIX_NONE,
		       32, 1, applespi->tx_status, APPLESPI_STATUS_SIZE, false);
#endif
#ifdef DEBUG_ALL_READ
	print_hex_dump(KERN_INFO, "applespi: read  ", DUMP_PREFIX_NONE,
		       32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);
#endif

	applespi_check_write_status(applespi, ret);

	return ret;
}

static inline ssize_t
applespi_sync_read(struct applespi_data *applespi)
{
	struct spi_transfer t;
	struct spi_message m;
	ssize_t ret;

	applespi_setup_read_txfr(applespi, &t);
	applespi_setup_spi_message(&m, 1, &t);

	ret = applespi_sync(applespi, &m);

#ifdef DEBUG_ALL_READ
	print_hex_dump(KERN_INFO, "applespi: read  ", DUMP_PREFIX_NONE,
		       32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);
#endif

	if (ret < 0)
		pr_warn("Error reading from device: %ld\n", ret);

	return ret;
}

static int applespi_find_settings_field(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(applespi_spi_settings_map); i++) {
		if (strcmp(applespi_spi_settings_map[i].name, name) == 0)
			return applespi_spi_settings_map[i].field_offset;
	}

	return -1;
}

static int applespi_get_spi_settings(struct applespi_data *applespi)
{
	u8 uuid[16];
	union acpi_object *spi_info;
	union acpi_object name;
	union acpi_object value;
	int i;
	int field_off;
	u64 *field;

	acpi_str_to_uuid(acpi_dsm_uuid, uuid);

	spi_info = acpi_evaluate_dsm(applespi->handle, uuid, 1, 1, NULL);
	if (!spi_info) {
		pr_err("Failed to get SPI info from _DSM method\n");
		return -ENODEV;
	}
	if (spi_info->type != ACPI_TYPE_PACKAGE) {
		pr_err("Unexpected data returned from SPI _DSM method: "
		       "type=%d\n", spi_info->type);
		ACPI_FREE(spi_info);
		return -ENODEV;
	}

	/* The data is stored in pairs of items, first a string containing
	 * the name of the item, followed by an 8-byte buffer containing the
	 * value in little-endian.
	 */
	for (i = 0; i < spi_info->package.count - 1; i += 2) {
		name = spi_info->package.elements[i];
		value = spi_info->package.elements[i + 1];

		if (!(name.type == ACPI_TYPE_STRING &&
		      value.type == ACPI_TYPE_BUFFER &&
		      value.buffer.length == 8)) {
			pr_warn("Unexpected data returned from SPI _DSM method:"
			        " name.type=%d, value.type=%d\n", name.type,
				value.type);
			continue;
		}

		field_off = applespi_find_settings_field(name.string.pointer);
		if (field_off < 0) {
			pr_debug("Skipping unknown SPI setting '%s'\n",
				 name.string.pointer);
			continue;
		}

		field = (u64 *) ((char *) &applespi->spi_settings + field_off);
		*field = le64_to_cpu(*((__le64 *) value.buffer.pointer));
	}
	ACPI_FREE(spi_info);

	/* acpi provided value is in 10us units */
	applespi->spi_settings.spi_cs_delay *= 10;

	return 0;
}

static int applespi_setup_spi(struct applespi_data *applespi)
{
	int sts;

	sts = applespi_get_spi_settings(applespi);
	if (sts)
		return sts;

	spin_lock_init(&applespi->led_msg_lock);

	return 0;
}

static int applespi_enable_spi(struct applespi_data *applespi)
{
	int result;
	long long unsigned int spi_status;

	/* Check if SPI is already enabled, so we can skip the delay below */
	result = acpi_evaluate_integer(applespi->sist, NULL, NULL, &spi_status);
	if (ACPI_SUCCESS(result) && spi_status)
		return 0;

	/* SIEN(1) will enable SPI communication */
	result = acpi_execute_simple_method(applespi->sien, NULL, 1);
	if (ACPI_FAILURE(result)) {
		pr_err("SIEN failed: %s\n", acpi_format_exception(result));
		return -ENODEV;
	}

	/*
	 * Allow the SPI interface to come up before returning. Without this
	 * delay, the SPI commands to enable multitouch mode may not reach
	 * the trackpad controller, causing pointer movement to break upon
	 * resume from sleep.
	 */
	msleep(50);

	return 0;
}

static void applespi_init(struct applespi_data *applespi)
{
	int i;
	ssize_t items = ARRAY_SIZE(applespi_init_commands);

	// Do a read to flush the trackpad
	applespi_sync_read(applespi);

	for (i=0; i < items; i++) {
		memcpy(applespi->tx_buffer, applespi_init_commands[i], 256);
		applespi_sync_write_and_response(applespi);
	}

	pr_info("modeswitch done.\n");
}

static int applespi_send_leds_cmd(struct applespi_data *applespi);

static void applespi_async_write_complete(void *context)
{
	struct applespi_data *applespi = context;
	unsigned long flags;

	applespi_check_write_status(applespi, applespi->wr_m.status);

	spin_lock_irqsave(&applespi->led_msg_lock, flags);

	applespi->led_msg_queued = false;
	applespi_send_leds_cmd(applespi);

	spin_unlock_irqrestore(&applespi->led_msg_lock, flags);
}

static int
applespi_send_leds_cmd(struct applespi_data *applespi)
{
	u16 crc;
	int sts;

	/* check whether send is needed and whether one is in progress */
	if (applespi->want_cl_led_on == applespi->have_cl_led_on ||
	    applespi->led_msg_queued) {
		return 0;
	}

	applespi->have_cl_led_on = applespi->want_cl_led_on;

	/* build led command buffer */
	memcpy(applespi->tx_buffer, applespi_caps_lock_led_cmd,
	       APPLESPI_PACKET_SIZE);

	applespi->tx_buffer[11] = applespi->led_msg_cntr++ & 0xff;
	applespi->tx_buffer[17] = applespi->have_cl_led_on ? 2 : 0;

	crc = crc16(0, applespi->tx_buffer + 8, 10);
	applespi->tx_buffer[18] = crc & 0xff;
	applespi->tx_buffer[19] = crc >> 8;

	/* send command */
	applespi_setup_write_txfr(applespi, &applespi->wr_t, &applespi->st_t);
	applespi_setup_spi_message(&applespi->wr_m, 2, &applespi->wr_t,
				   &applespi->st_t);

	sts = applespi_async(applespi, &applespi->wr_m,
			     applespi_async_write_complete);

	if (sts != 0)
		pr_warn("Error queueing async write to device: %d\n", sts);
	else
		applespi->led_msg_queued = true;

	return sts;
}

static int
applespi_set_leds(struct applespi_data *applespi, bool capslock_on)
{
	unsigned long flags;
	int sts;

	spin_lock_irqsave(&applespi->led_msg_lock, flags);

	applespi->want_cl_led_on = capslock_on;
	sts = applespi_send_leds_cmd(applespi);

	spin_unlock_irqrestore(&applespi->led_msg_lock, flags);

	return sts;
}

static int
applespi_event(struct input_dev *dev, unsigned int type, unsigned int code,
	       int value)
{
	struct applespi_data *applespi = input_get_drvdata(dev);

	switch (type) {

	case EV_LED:
		applespi_set_leds(applespi, !!test_bit(LED_CAPSL, dev->led));
		return 0;
	}

	return -1;
}

/* Lifted from the BCM5974 driver */
/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x)
{
	return (signed short)le16_to_cpu(x);
}

static void report_finger_data(struct input_dev *input, int slot,
			       const struct input_mt_pos *pos,
			       const struct tp_finger *f)
{
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

	input_report_abs(input, ABS_MT_TOUCH_MAJOR,
			 raw2int(f->touch_major) << 1);
	input_report_abs(input, ABS_MT_TOUCH_MINOR,
			 raw2int(f->touch_minor) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR,
			 raw2int(f->tool_major) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MINOR,
			 raw2int(f->tool_minor) << 1);
	input_report_abs(input, ABS_MT_ORIENTATION,
			 MAX_FINGER_ORIENTATION - raw2int(f->orientation));
	input_report_abs(input, ABS_MT_POSITION_X, pos->x);
	input_report_abs(input, ABS_MT_POSITION_Y, pos->y);
}

static int report_tp_state(struct applespi_data *applespi, struct touchpad_protocol* t)
{
	const struct tp_finger *f;
	struct input_dev *input = applespi->touchpad_input_dev;
	const struct applespi_tp_info *tp_info = applespi->tp_info;
	int i, n;

	n = 0;

	for (i = 0; i < MAX_FINGERS; i++) {
		f = &t->fingers[i];
		if (raw2int(f->touch_major) == 0)
			continue;
		applespi->pos[n].x = raw2int(f->abs_x);
		applespi->pos[n].y = tp_info->y_min + tp_info->y_max - raw2int(f->abs_y);
		n++;
	}

	input_mt_assign_slots(input, applespi->slots, applespi->pos, n, 0);

	for (i = 0; i < n; i++)
		report_finger_data(input, applespi->slots[i],
				   &applespi->pos[i], &t->fingers[i]);

	input_mt_sync_frame(input);
	input_report_key(input, BTN_LEFT, t->clicked);

	input_sync(input);
	return 0;
}

static const struct applespi_key_translation*
applespi_find_translation(const struct applespi_key_translation *table, u16 key)
{
	const struct applespi_key_translation *trans;

	for (trans = table; trans->from; trans++)
		if (trans->from == key)
			return trans;

	return NULL;
}

static unsigned int
applespi_code_to_key(u8 code, int fn_pressed)
{
	unsigned int key = applespi_scancodes[code];

	const struct applespi_key_translation *trans;

	if (fnmode) {
		int do_translate;

		trans = applespi_find_translation(applespi_fn_codes, key);
		if (trans) {
			if (trans->flags & APPLE_FLAG_FKEY)
				do_translate = (fnmode == 2 && fn_pressed) ||
					(fnmode == 1 && !fn_pressed);
			else
				do_translate = fn_pressed;

			if (do_translate)
				key = trans->to;
		}
	}

	if (iso_layout) {
		trans = applespi_find_translation(apple_iso_keyboard, key);
		if (trans)
			key = trans->to;
	}

	return key;
}

static void
applespi_got_data(struct applespi_data *applespi)
{
	struct keyboard_protocol keyboard_protocol;
	int i, j;
	unsigned int key;
	bool still_pressed;

	memcpy(&keyboard_protocol, applespi->rx_buffer, APPLESPI_PACKET_SIZE);
	if (keyboard_protocol.packet_type == PACKET_NOTHING) {
		return;
	} else if (keyboard_protocol.packet_type == PACKET_KEYBOARD) {
		for (i=0; i<6; i++) {
			still_pressed = false;
			for (j=0; j<6; j++) {
				if (applespi->last_keys_pressed[i] == keyboard_protocol.keys_pressed[j]) {
					still_pressed = true;
					break;
				}
			}

			if (! still_pressed) {
				key = applespi_code_to_key(applespi->last_keys_pressed[i], applespi->last_keys_fn_pressed[i]);
				input_report_key(applespi->keyboard_input_dev, key, 0);
				applespi->last_keys_fn_pressed[i] = 0;
			}
		}

		for (i=0; i<6; i++) {
			if (keyboard_protocol.keys_pressed[i] < ARRAY_SIZE(applespi_scancodes) && keyboard_protocol.keys_pressed[i] > 0) {
				key = applespi_code_to_key(keyboard_protocol.keys_pressed[i], keyboard_protocol.fn_pressed);
				input_report_key(applespi->keyboard_input_dev, key, 1);
				applespi->last_keys_fn_pressed[i] = keyboard_protocol.fn_pressed;
			}
		}

		// Check control keys
		for (i=0; i<8; i++) {
			if (test_bit(i, (long unsigned int *)&keyboard_protocol.modifiers)) {
				input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 1);
			} else {
				input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 0);
			}
		}

		// Check function key
		if (keyboard_protocol.fn_pressed && !applespi->last_fn_pressed) {
			input_report_key(applespi->keyboard_input_dev, KEY_FN, 1);
		} else if (!keyboard_protocol.fn_pressed && applespi->last_fn_pressed) {
			input_report_key(applespi->keyboard_input_dev, KEY_FN, 0);
		}
		applespi->last_fn_pressed = keyboard_protocol.fn_pressed;

		input_sync(applespi->keyboard_input_dev);
		memcpy(&applespi->last_keys_pressed, keyboard_protocol.keys_pressed, sizeof(applespi->last_keys_pressed));
	} else if (keyboard_protocol.packet_type == PACKET_TOUCHPAD) {
		report_tp_state(applespi, (struct touchpad_protocol*)&keyboard_protocol);
	}
#ifdef DEBUG_UNKNOWN_PACKET
	else {
		pr_info("--- %d\n", keyboard_protocol.packet_type);
		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
	}
#endif
}

static void applespi_async_read_complete(void *context)
{
	struct applespi_data *applespi = context;

	if (applespi->rd_m.status < 0)
		pr_warn("Error reading from device: %d\n", applespi->rd_m.status);
	else
		applespi_got_data(applespi);

#ifdef DEBUG_ALL_READ
	print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, 256, false);
#endif
	acpi_finish_gpe(NULL, applespi->gpe);
}

static u32 applespi_notify(acpi_handle gpe_device, u32 gpe, void *context)
{
	struct applespi_data *applespi = context;

	memset(&applespi->dl_t, 0, sizeof(applespi->dl_t));
	applespi->dl_t.delay_usecs = applespi->spi_settings.reset_a2r_usec;
	applespi_setup_read_txfr(applespi, &applespi->rd_t);
	applespi_setup_spi_message(&applespi->rd_m, 2, &applespi->dl_t, &applespi->rd_t);

	applespi_async(applespi, &applespi->rd_m, applespi_async_read_complete);
	return ACPI_INTERRUPT_HANDLED;
}

static int applespi_probe(struct spi_device *spi)
{
	struct applespi_data *applespi;
	int result, i;
	long long unsigned int gpe, usb_status;

	/* Allocate driver data */
	applespi = devm_kzalloc(&spi->dev, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;

	applespi->spi = spi;

	/* Create our buffers */
	applespi->tx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);
	applespi->tx_status = devm_kmalloc(&spi->dev, APPLESPI_STATUS_SIZE, GFP_KERNEL);
	applespi->rx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);

	if (!applespi->tx_buffer || !applespi->tx_status || !applespi->rx_buffer)
		return -ENOMEM;

	/* Store the driver data */
	spi_set_drvdata(spi, applespi);

	/* Set up touchpad dimensions */
	applespi->tp_info = dmi_first_match(applespi_touchpad_infos)->driver_data;

	/* Setup the keyboard input dev */
	applespi->keyboard_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->keyboard_input_dev)
		return -ENOMEM;

	applespi->keyboard_input_dev->name = "Apple SPI Keyboard";
	applespi->keyboard_input_dev->phys = "applespi/input0";
	applespi->keyboard_input_dev->dev.parent = &spi->dev;
	applespi->keyboard_input_dev->id.bustype = BUS_SPI;

	applespi->keyboard_input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
	applespi->keyboard_input_dev->ledbit[0] = BIT_MASK(LED_CAPSL);

	input_set_drvdata(applespi->keyboard_input_dev, applespi);
	applespi->keyboard_input_dev->event = applespi_event;

	for (i = 0; i<ARRAY_SIZE(applespi_scancodes); i++)
		if (applespi_scancodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_scancodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_controlcodes); i++)
		if (applespi_controlcodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_controlcodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_fn_codes); i++)
		if (applespi_fn_codes[i].to)
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_fn_codes[i].to);

	input_set_capability(applespi->keyboard_input_dev, EV_KEY, KEY_FN);

	result = input_register_device(applespi->keyboard_input_dev);
	if (result) {
		pr_err("Unabled to register keyboard input device (%d)\n",
		       result);
		return -ENODEV;
	}

	/* Now, set up the touchpad as a seperate input device */
	applespi->touchpad_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->touchpad_input_dev)
		return -ENOMEM;

	applespi->touchpad_input_dev->name = "Apple SPI Touchpad";
	applespi->touchpad_input_dev->phys = "applespi/input1";
	applespi->touchpad_input_dev->dev.parent = &spi->dev;
	applespi->touchpad_input_dev->id.bustype = BUS_SPI;

	applespi->touchpad_input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

	__set_bit(EV_KEY, applespi->touchpad_input_dev->evbit);
	__set_bit(EV_ABS, applespi->touchpad_input_dev->evbit);

	__set_bit(BTN_LEFT, applespi->touchpad_input_dev->keybit);

	__set_bit(INPUT_PROP_POINTER, applespi->touchpad_input_dev->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, applespi->touchpad_input_dev->propbit);

	/* finger touch area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MINOR, 0, 2048, 0, 0);

	/* finger approach area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MINOR, 0, 2048, 0, 0);

	/* finger orientation */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_ORIENTATION, -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION, 0, 0);

	/* finger position */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_X, applespi->tp_info->x_min, applespi->tp_info->x_max, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_Y, applespi->tp_info->y_min, applespi->tp_info->y_max, 0, 0);

	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_LEFT);

	input_mt_init_slots(applespi->touchpad_input_dev, MAX_FINGERS,
		INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED | INPUT_MT_TRACK);

	result = input_register_device(applespi->touchpad_input_dev);
	if (result) {
		pr_err("Unabled to register touchpad input device (%d)\n",
		       result);
		return -ENODEV;
	}

	applespi->handle = ACPI_HANDLE(&spi->dev);

	/* Check if the USB interface is present and enabled already */
	result = acpi_evaluate_integer(applespi->handle, "UIST", NULL, &usb_status);
	if (ACPI_SUCCESS(result) && usb_status) {
		/* Let the USB driver take over instead */
		pr_info("USB interface already enabled\n");
		return -ENODEV;
	}

	/* Cache ACPI method handles */
	if (ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIEN", &applespi->sien)) ||
	    ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIST", &applespi->sist))) {
		pr_err("Failed to get required ACPI method handle\n");
		return -ENODEV;
	}

	/* Switch on the SPI interface */
	result = applespi_setup_spi(applespi);
	if (result)
		return result;

	result = applespi_enable_spi(applespi);
	if (result)
		return result;

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	/*
	 * The applespi device doesn't send interrupts normally (as is described
	 * in its DSDT), but rather seems to use ACPI GPEs.
	 */
	result = acpi_evaluate_integer(applespi->handle, "_GPE", NULL, &gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to obtain GPE for SPI slave device: %s\n",
		       acpi_format_exception(result));
		return -ENODEV;
	}
	applespi->gpe = (int)gpe;

	result = acpi_install_gpe_handler(NULL, applespi->gpe, ACPI_GPE_LEVEL_TRIGGERED, applespi_notify, applespi);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to install GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		return -ENODEV;
	}

	result = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to enable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);
		return -ENODEV;
	}

	pr_info("module probe done.\n");

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);

	acpi_disable_gpe(NULL, applespi->gpe);
	acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);

	pr_info("module remove done.\n");
	return 0;
}

#ifdef CONFIG_PM
static int applespi_suspend(struct device *dev)
{
	pr_info("device suspend done.\n");
	return 0;
}

static int applespi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);

	/* Switch on the SPI interface */
	applespi_enable_spi(applespi);

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	pr_info("device resume done.\n");

	return 0;
}
#endif

static const struct acpi_device_id applespi_acpi_match[] = {
	{ "APP000D", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static UNIVERSAL_DEV_PM_OPS(applespi_pm_ops, applespi_suspend,
                            applespi_resume, NULL);

static struct spi_driver applespi_driver = {
	.driver		= {
		.name			= "applespi",
		.owner			= THIS_MODULE,

		.acpi_match_table	= ACPI_PTR(applespi_acpi_match),
		.pm			= &applespi_pm_ops,
	},
	.probe		= applespi_probe,
	.remove		= applespi_remove,
};
module_spi_driver(applespi_driver)

MODULE_LICENSE("GPL");
