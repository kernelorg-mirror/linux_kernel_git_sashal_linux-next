// SPDX-License-Identifier: GPL-2.0
/*
 * Environmental sensors of Marvel (EV7) AlphaServers: the GS1280, ES80 and
 * ES47 families.
 *
 * The SRM console describes every sensor in the config tree (GCT). Each power
 * and environment (P&E) node may carry a list of environmental packets:
 * temperature and voltage sensors, fans and power supplies. The console fills
 * in the readings when asked to refresh the node. The packet layout was
 * taken from Tru64 UNIX.
 *
 * The management firmware gives each temperature, voltage and fan sensor a
 * warning limit and a failure limit, and puts the sensor in the warning or
 * failure state when its reading reaches the limit. Neither state means that
 * the sensor itself is broken. A power supply has no warning state; it is
 * either working or failed.
 */

#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/gct.h>
#include <asm/marvel_env.h>

#define DRVNAME "marvel_env"

#define GCT_FLAGS_REQUIRED	BIT(9)
#define GCT_FLAGS_EXCLUDED	(BIT(2) | BIT(7) | BIT(12) | BIT(14))

/* Header of a P&E node's packet list, at hd_extension. */
#define ENV_HDR_COUNT		0x10
#define ENV_HDR_FIRST		0x14
#define ENV_HDR_SIZE		0x18

/* Packet types. */
#define ENV_PKT_POWER_SUPPLY	0x08
#define ENV_PKT_FAN		0x09
#define ENV_PKT_SENSOR		0x2a

/*
 * Sensor packet: kind, status, reading, and two limits. Temperatures are in
 * tenths of a degree Celsius, voltages in millivolts. For a temperature the
 * two values are the warning and failure limits. For a voltage they are the
 * nominal voltage and an upper limit; the firmware checks a voltage against
 * a window on both sides of the nominal voltage, so neither is exported.
 */
#define SENSOR_KIND		0x28
#define SENSOR_STATUS		0x2c
#define SENSOR_VALUE		0x30
#define SENSOR_WARN		0x34
#define SENSOR_FAIL		0x38
#define SENSOR_NOMINAL		SENSOR_WARN	/* of a voltage */
#define SENSOR_SIZE		0x3c

#define SENSOR_KIND_THERMAL	2
#define SENSOR_KIND_VOLT_FIRST	3
#define SENSOR_KIND_VOLT_LAST	5

/* Fan packet: status, rpm, low warning limit, low failure limit. */
#define FAN_STATUS		0x24
#define FAN_VALUE		0x28
#define FAN_WARN		0x2c
#define FAN_SIZE		0x34

/* Power supply packet: status. */
#define PS_STATUS		0x24
#define PS_SIZE			0x28

/* Status values, in the low byte of the status word. */
#define ENV_OK			0
#define ENV_WARNING		1	/* reading reached the warning limit */
#define ENV_FAILURE		2	/* reading reached the failure limit */
#define ENV_UNKNOWN		0xfe
#define ENV_NOT_PRESENT		0xff

/* Console status of a refresh that Tru64 also treats as success. */
#define CFGTREE_STATUS_OK2	(-0x4a)

/* A refresh blocks the boot CPU for up to 300 ms, so limit how often. */
#define REFRESH_INTERVAL	(10 * HZ)

struct marvel_env_node {
	gct6_node *node;
	unsigned long offset;		/* from the start of the GCT */
	unsigned long last_refresh;
	bool valid;
	unsigned int cabinet, drawer, pe;
};

struct marvel_env_chan {
	struct marvel_env_node *node;
	unsigned int pkt;		/* packet offset in the node */
	bool ps;			/* power supply, as an in channel */
	bool present;			/* known when the driver loaded */
	char label[32];
};

struct marvel_env_data {
	struct device *dev;
	struct marvel_env_node *nodes;
	unsigned int nr_nodes, max_nodes;
	struct marvel_env_chan *chans[hwmon_max];
	unsigned int nr_chans[hwmon_max], max_chans[hwmon_max];
	bool multi_drawer;		/* label with cabinet and drawer */
};

static const char * const pe_names[] = {
	[0x09] = "MBM",
	[0x0a] = "MBM/PBM",
	[0x0c] = "PS",
	[0x0d] = "FAN",
	[0x0f] = "CMM0", [0x10] = "CMM1", [0x11] = "CMM2", [0x12] = "CMM3",
	[0x14] = "VRM0", [0x15] = "VRM1", [0x16] = "VRM2", [0x17] = "VRM3",
	[0x19] = "PBM",
	[0x1a] = "XSHELF",
	[0x1b] = "PCIPS",
	[0x1c] = "PCIFANS",
};

/* The sensor types the driver exports. */
static const enum hwmon_sensor_types marvel_env_types[] = {
	hwmon_temp, hwmon_fan, hwmon_in,
};

/* The attributes that report the state of a channel. */
static const u32 marvel_env_alarm_attrs[hwmon_max] = {
	[hwmon_temp] = HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM,
	[hwmon_fan] = HWMON_F_MIN_ALARM | HWMON_F_FAULT,
	[hwmon_in] = HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM |
		     HWMON_I_LCRIT_ALARM | HWMON_I_CRIT_ALARM | HWMON_I_FAULT,
};

static inline u32 env_rd(struct marvel_env_node *n, unsigned int off)
{
	return *(u32 *)((char *)n->node + off);
}

static bool env_status_valid(u8 status)
{
	return status != ENV_UNKNOWN && status != ENV_NOT_PRESENT;
}

static u8 marvel_env_status(enum hwmon_sensor_types type,
			    const struct marvel_env_chan *c)
{
	unsigned int off;

	if (type == hwmon_fan)
		off = FAN_STATUS;
	else if (c->ps)
		off = PS_STATUS;
	else
		off = SENSOR_STATUS;
	return env_rd(c->node, c->pkt + off) & 0xff;
}

/*
 * The alarm attributes that are set in @status. A reading past the failure
 * limit is also past the warning limit, so it sets both alarms. A fan below
 * its failure limit is one the firmware counts as failed. A voltage outside
 * its limits is too low or too high depending on which side of the nominal
 * voltage it reads.
 */
static u32 marvel_env_alarms(enum hwmon_sensor_types type,
			     const struct marvel_env_chan *c, u8 status)
{
	switch (type) {
	case hwmon_temp:
		if (status == ENV_FAILURE)
			return HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM;
		if (status == ENV_WARNING)
			return HWMON_T_MAX_ALARM;
		break;
	case hwmon_fan:
		if (status == ENV_FAILURE)
			return HWMON_F_MIN_ALARM | HWMON_F_FAULT;
		if (status == ENV_WARNING)
			return HWMON_F_MIN_ALARM;
		break;
	case hwmon_in:
		if (c->ps)
			return status == ENV_FAILURE ? HWMON_I_FAULT : 0;
		if (status != ENV_WARNING && status != ENV_FAILURE)
			break;
		if ((s32)env_rd(c->node, c->pkt + SENSOR_VALUE) <
		    (s32)env_rd(c->node, c->pkt + SENSOR_NOMINAL)) {
			if (status == ENV_FAILURE)
				return HWMON_I_MIN_ALARM | HWMON_I_LCRIT_ALARM;
			return HWMON_I_MIN_ALARM;
		}
		if (status == ENV_FAILURE)
			return HWMON_I_MAX_ALARM | HWMON_I_CRIT_ALARM;
		return HWMON_I_MAX_ALARM;
	default:
		break;
	}
	return 0;
}

static inline gct6_node *gct_node(gct6_handle h)
{
	return GCT_NODE_PTR(h);
}

/* Cabinet and drawer come from the P&E node's parent. */
static void marvel_env_keys(struct marvel_env_node *n)
{
	gct6_node *parent = n->node->parent ? gct_node(n->node->parent) : NULL;

	n->pe = n->node->id & 0xff;
	if (!parent)
		return;
	if (parent->type == GCT_TYPE_PE && parent->subtype == GCT_SUBTYPE_PE_CABINET)
		n->cabinet = (parent->id >> 48) & 0xff;
	else
		n->cabinet = (parent->id >> 48) & 0x3f;
	n->drawer = (parent->id >> 32) & 0xff;
}

static int marvel_env_refresh(struct marvel_env_data *data,
			      struct marvel_env_node *n)
{
	long status;

	if (n->valid &&
	    time_before(jiffies, n->last_refresh + REFRESH_INTERVAL))
		return 0;

	status = marvel_cfgtree_partial_update(n->offset);
	if (status != 1 && status != CFGTREE_STATUS_OK2) {
		dev_warn_ratelimited(data->dev,
				     "refresh of GCT node %#lx failed: %ld\n",
				     n->offset, status);
		return -EIO;
	}
	n->last_refresh = jiffies;
	n->valid = true;
	return 0;
}

/* The hwmon type of a packet, or hwmon_max to skip it. */
static enum hwmon_sensor_types marvel_env_pkt_type(struct marvel_env_node *n,
						   unsigned int pkt,
						   unsigned int type)
{
	unsigned int len = env_rd(n, pkt) & 0xffff;
	u32 kind;

	switch (type) {
	case ENV_PKT_SENSOR:
		if (len < SENSOR_SIZE)
			break;
		kind = env_rd(n, pkt + SENSOR_KIND);
		if (kind == SENSOR_KIND_THERMAL)
			return hwmon_temp;
		if (kind >= SENSOR_KIND_VOLT_FIRST && kind <= SENSOR_KIND_VOLT_LAST)
			return hwmon_in;
		break;
	case ENV_PKT_FAN:
		if (len >= FAN_SIZE)
			return hwmon_fan;
		break;
	case ENV_PKT_POWER_SUPPLY:
		if (len >= PS_SIZE)
			return hwmon_in;
		break;
	}
	return hwmon_max;
}

static void marvel_env_add_chan(struct marvel_env_data *data,
				struct marvel_env_node *n, unsigned int pkt,
				enum hwmon_sensor_types type, bool ps,
				unsigned int index)
{
	static const char * const names[hwmon_max] = {
		[hwmon_temp] = "Temp", [hwmon_fan] = "Fan", [hwmon_in] = "Volt",
	};
	const char *what = ps ? "PS" : names[type];
	struct marvel_env_chan *c;
	const char *pe = NULL;
	size_t len = 0;
	u8 status;

	c = &data->chans[type][data->nr_chans[type]++];
	c->node = n;
	c->pkt = pkt;
	c->ps = ps;
	status = marvel_env_status(type, c);
	c->present = env_status_valid(status);

	if (data->multi_drawer)
		len = scnprintf(c->label, sizeof(c->label), "Cab%u Dra%u ",
				n->cabinet, n->drawer);

	if (n->pe < ARRAY_SIZE(pe_names))
		pe = pe_names[n->pe];
	if (pe && !strncasecmp(pe, what, strlen(what)))
		scnprintf(c->label + len, sizeof(c->label) - len, "%s %u",
			  pe, index);
	else if (pe)
		scnprintf(c->label + len, sizeof(c->label) - len, "%s %s %u",
			  pe, what, index);
	else
		scnprintf(c->label + len, sizeof(c->label) - len, "PE%u %s %u",
			  n->pe, what, index);
}

/*
 * Walk a P&E node's packets, counting them, or adding them when @add. As
 * Tru64 does, visit count + 1 packets and number sensors from their
 * position minus one.
 */
static void marvel_env_walk(struct marvel_env_data *data,
			    struct marvel_env_node *n, bool add)
{
	unsigned int size = n->node->size;
	unsigned int hdr = n->node->hd_extension;
	unsigned int count, pkt, i;

	if (!hdr || hdr + ENV_HDR_SIZE > size)
		return;
	count = env_rd(n, hdr + ENV_HDR_COUNT);
	pkt = env_rd(n, hdr + ENV_HDR_FIRST);

	for (i = 0; i <= count; i++) {
		enum hwmon_sensor_types type;
		unsigned int len, pkt_type;

		if (pkt < 8 || pkt + 8 > size)
			break;
		len = env_rd(n, pkt) & 0xffff;
		pkt_type = env_rd(n, pkt + 4) & 0xffff;
		if (len < 8 || pkt + len > size)
			break;

		type = marvel_env_pkt_type(n, pkt, pkt_type);
		if (type != hwmon_max && add &&
		    data->nr_chans[type] < data->max_chans[type])
			marvel_env_add_chan(data, n, pkt, type,
					    pkt_type == ENV_PKT_POWER_SUPPLY,
					    pkt_type == ENV_PKT_SENSOR && i ?
					    i - 1 : i);
		else if (type != hwmon_max && !add)
			data->nr_chans[type]++;
		pkt += len;
	}
}

/* Collect the P&E nodes with an environmental packet list. */
static void marvel_env_find_nodes(struct marvel_env_data *data,
				  gct6_node *node, bool record)
{
	for (; node; node = node->next ? gct_node(node->next) : NULL) {
		if (node->magic != GCT_NODE_MAGIC)
			return;
		if (node->type == GCT_TYPE_PE && node->hd_extension &&
		    (node->flags & GCT_FLAGS_REQUIRED) &&
		    !(node->flags & GCT_FLAGS_EXCLUDED)) {
			if (record) {
				struct marvel_env_node *n;

				if (data->nr_nodes == data->max_nodes)
					return;
				n = &data->nodes[data->nr_nodes];

				n->node = node;
				n->offset = (char *)node - (char *)gct_node(0);
				marvel_env_keys(n);
			}
			data->nr_nodes++;
		}
		if (node->child)
			marvel_env_find_nodes(data, gct_node(node->child),
					      record);
	}
}

static umode_t marvel_env_is_visible(const void *drvdata,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	const struct marvel_env_data *data = drvdata;

	/* Hide sensors that are absent when the driver loads. */
	return data->chans[type][channel].present ? 0444 : 0;
}

/* Called with the hwmon device lock held, which serializes refreshes. */
static int marvel_env_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct marvel_env_data *data = dev_get_drvdata(dev);
	struct marvel_env_chan *c;
	struct marvel_env_node *n;
	unsigned int pkt;
	u8 status;
	int ret;

	c = &data->chans[type][channel];
	n = c->node;
	pkt = c->pkt;

	ret = marvel_env_refresh(data, n);
	if (ret)
		return ret;
	status = marvel_env_status(type, c);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_max:
			*val = (s32)env_rd(n, pkt + SENSOR_WARN) * 100L;
			return 0;
		case hwmon_temp_crit:
			*val = (s32)env_rd(n, pkt + SENSOR_FAIL) * 100L;
			return 0;
		}
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_min) {
			*val = env_rd(n, pkt + FAN_WARN);
			return 0;
		}
		break;
	default:
		break;
	}

	if (!env_status_valid(status))
		return -ENODATA;

	if (marvel_env_alarm_attrs[type] & BIT(attr)) {
		*val = !!(marvel_env_alarms(type, c, status) & BIT(attr));
		return 0;
	}

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			break;
		*val = (s32)env_rd(n, pkt + SENSOR_VALUE) * 100L;
		return 0;
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			break;
		*val = env_rd(n, pkt + FAN_VALUE);
		return 0;
	case hwmon_in:
		if (attr != hwmon_in_input)
			break;
		*val = (s32)env_rd(n, pkt + SENSOR_VALUE);
		return 0;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int marvel_env_read_string(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel, const char **str)
{
	struct marvel_env_data *data = dev_get_drvdata(dev);

	*str = data->chans[type][channel].label;
	return 0;
}

static const struct hwmon_ops marvel_env_ops = {
	.is_visible = marvel_env_is_visible,
	.read = marvel_env_read,
	.read_string = marvel_env_read_string,
};

static u32 marvel_env_config(enum hwmon_sensor_types type,
			     const struct marvel_env_chan *c)
{
	switch (type) {
	case hwmon_temp:
		return HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_CRIT |
		       HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM | HWMON_T_LABEL;
	case hwmon_fan:
		return HWMON_F_INPUT | HWMON_F_MIN | HWMON_F_MIN_ALARM |
		       HWMON_F_FAULT | HWMON_F_LABEL;
	case hwmon_in:
		if (c->ps)
			return HWMON_I_FAULT | HWMON_I_LABEL;
		return HWMON_I_INPUT | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM |
		       HWMON_I_LCRIT_ALARM | HWMON_I_CRIT_ALARM | HWMON_I_LABEL;
	default:
		return 0;
	}
}

static int marvel_env_probe(struct platform_device *pdev)
{
	u32 *config[hwmon_max] = { };
	struct device *dev = &pdev->dev;
	const struct hwmon_channel_info **info;
	struct marvel_env_data *data;
	struct hwmon_chip_info *chip;
	struct device *hwmon;
	unsigned int i, k, nr_info = 0;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;

	marvel_env_find_nodes(data, gct_node(0), false);
	if (!data->nr_nodes)
		return -ENODEV;
	data->nodes = devm_kcalloc(dev, data->nr_nodes, sizeof(*data->nodes),
				   GFP_KERNEL);
	if (!data->nodes)
		return -ENOMEM;
	data->max_nodes = data->nr_nodes;
	data->nr_nodes = 0;
	marvel_env_find_nodes(data, gct_node(0), true);

	/* Label with cabinet and drawer if any unit is outside 0/0. */
	for (i = 0; i < data->nr_nodes; i++)
		if (data->nodes[i].cabinet || data->nodes[i].drawer)
			data->multi_drawer = true;

	for (i = 0; i < data->nr_nodes; i++) {
		marvel_env_refresh(data, &data->nodes[i]);
		marvel_env_walk(data, &data->nodes[i], false);
	}

	info = devm_kcalloc(dev, ARRAY_SIZE(marvel_env_types) + 1,
			    sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	for (k = 0; k < ARRAY_SIZE(marvel_env_types); k++) {
		enum hwmon_sensor_types type = marvel_env_types[k];
		struct hwmon_channel_info *ci;

		if (!data->nr_chans[type])
			continue;
		data->chans[type] = devm_kcalloc(dev, data->nr_chans[type],
						 sizeof(*data->chans[type]),
						 GFP_KERNEL);
		ci = devm_kzalloc(dev, sizeof(*ci), GFP_KERNEL);
		config[type] = devm_kcalloc(dev, data->nr_chans[type] + 1,
					    sizeof(*config[type]), GFP_KERNEL);
		if (!data->chans[type] || !ci || !config[type])
			return -ENOMEM;
		ci->type = type;
		ci->config = config[type];
		info[nr_info++] = ci;
		data->max_chans[type] = data->nr_chans[type];
		data->nr_chans[type] = 0;
	}
	if (!nr_info)
		return -ENODEV;

	for (i = 0; i < data->nr_nodes; i++)
		marvel_env_walk(data, &data->nodes[i], true);

	for (k = 0; k < ARRAY_SIZE(marvel_env_types); k++) {
		enum hwmon_sensor_types type = marvel_env_types[k];

		for (i = 0; i < data->nr_chans[type]; i++)
			config[type][i] = marvel_env_config(type,
							    &data->chans[type][i]);
	}

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->ops = &marvel_env_ops;
	chip->info = info;

	hwmon = devm_hwmon_device_register_with_info(dev, DRVNAME, data, chip,
						     NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}

static struct platform_driver marvel_env_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.probe = marvel_env_probe,
};
module_platform_driver(marvel_env_driver);

MODULE_ALIAS("platform:" DRVNAME);
MODULE_DESCRIPTION("Marvel (EV7) AlphaServer environmental sensors");
MODULE_LICENSE("GPL");
