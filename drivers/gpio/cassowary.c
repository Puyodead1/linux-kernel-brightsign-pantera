/*
 * BrightSign Cassowary Silego FPGA driver
 *
 * The SLG46533 is the Silego FPGA that is used to expand 10 GPOs on
 * BrightSign Outback.  This driver enables controls of those outputs pins.
 *
 * There are two banks of five latches enabling ten GPOs on the SLG chip
 * Bits 0-4 map to the first bank and bits 8-15 map to the second bank
 * Bit 5 is the bank 0 latch and is active low.  Setting this bit low will
 * write the i2c bus data to the first bank of latches.
 * Bit 6 is the bank 1 latch and is active low.  Setting this bit low will
 * write the i2c bus data to the second bank of latches.
 * Writing to the first or second bank of latches is mutually exclusive
 */

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>

#define MASK_GPIO_BYTE_CTRL		0x60
#define MASK_GPIO_BYTE_DATA		0x1F

#define MASK_GPIO_BANK_0		0x1F
#define MASK_GPIO_BANK_1		0x1F00
#define MASK_GPIO_BANK_0_LATCH	BIT(5)
#define MASK_GPIO_BANK_1_LATCH	BIT(6)

#define GPIO_GPIO_EN_0			BIT(0)
#define GPIO_GPIO_EN_1			BIT(1)
#define GPIO_GPIO_EN_2			BIT(2)
#define GPIO_GPIO_EN_3			BIT(3)
#define GPIO_LED_WIFI			BIT(4)

#define GPIO_SYS_FAN_			BIT(8)
#define GPIO_RESET_HUB_			BIT(9)
#define GPIO_UART_0_INVERT		BIT(10)
#define GPIO_UART_1_INVERT		BIT(11)
#define GPIO_LED_PWR			BIT(12)

#define VIRTUAL_INPUT_REG		0xF4

#define DEBUG 0

#if DEBUG
#define cassowary_dbg(...) pr_err("cassowary: " __VA_ARGS__)
#else // !DEBUG
#define cassowary_dbg(dev, ...) do { } while(0)
#endif // !DEBUG


struct cassowary_priv {
	struct gpio_chip	chip;
	struct i2c_client	*client;
	struct mutex		lock;				/* protect output_status */
	u16					output_status;		/* current status of outputs */
};

static int i2c_write(struct i2c_client *client, unsigned data)
{
	u8 buf[2] = { (data >> 8) & 0xff, data & 0xFF, };
	int status;

	status = i2c_master_send(client, buf, 2);
	cassowary_dbg("i2c send 0x%x status=%d\n", data, status);
	return (status < 0) ? status : 0;
}

static void update_priv_data(struct cassowary_priv *priv, unsigned gpio_bit, int value)
{
	if (value)
		priv->output_status |= gpio_bit;
	else
		priv->output_status &= ~gpio_bit;
}

static void prepare_bus_data_with_latches_disabled(struct cassowary_priv *priv, unsigned char data)
{
	unsigned i2c_data = (VIRTUAL_INPUT_REG << 8);
	i2c_data |= MASK_GPIO_BYTE_CTRL;
	i2c_data |= data;
	i2c_write(priv->client, i2c_data);
}

static void latch_bus_data(struct cassowary_priv *priv, unsigned char next_bank_data)
{
	unsigned i2c_data = (VIRTUAL_INPUT_REG << 8);
	//Retrieve latch status and write new bank data
	i2c_data |= priv->output_status & MASK_GPIO_BYTE_CTRL;
	i2c_data |= next_bank_data;
	i2c_write(priv->client, i2c_data);
}

static void cassowary_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct cassowary_priv *priv = gpiochip_get_data(chip);
	unsigned gpio_bit;
	bool switch_bank = false;
	unsigned char current_bank_data = 0;
	unsigned char next_bank_data = 0;
	int state = value;

	//GPIO index 0-4 map directly to the lower bank byte. 5-9 map to the higher bank byte
	if (offset >= 5)
		gpio_bit = BIT(offset + 3);
	else
		gpio_bit = BIT(offset);

	//GPIO index 9 corresponds to Latch12 in the SLG HW, the only latch which inverts its output
	if (offset == 9)
		state = !state;

	//Only set supported GPIO on the Silego
	if (gpio_bit & MASK_GPIO_BANK_0 || gpio_bit & MASK_GPIO_BANK_1)
	{
		mutex_lock(&priv->lock);

		if (gpio_bit & MASK_GPIO_BANK_0)
		{
			update_priv_data(priv, gpio_bit, state);
			next_bank_data |= priv->output_status & MASK_GPIO_BANK_0;
			if (priv->output_status & MASK_GPIO_BANK_0_LATCH)
			{
				current_bank_data |= (priv->output_status & MASK_GPIO_BANK_1) >> 8;
				switch_bank = true;
			}
		}
		else
		{
			update_priv_data(priv, gpio_bit, state);
			next_bank_data |= (priv->output_status & MASK_GPIO_BANK_1) >> 8;
			if (priv->output_status & MASK_GPIO_BANK_1_LATCH)
			{
				current_bank_data |= priv->output_status & MASK_GPIO_BANK_0;
				switch_bank = true;
			}


		}

		if (switch_bank)
		{
			// Hold the "old" data bits and deassert both strobes on the i2c bus
			prepare_bus_data_with_latches_disabled(priv, current_bank_data);

			// While both strobes are off, drive out the new data bits
			prepare_bus_data_with_latches_disabled(priv, next_bank_data);

			// While holding the new data bits, assert the new strobe
			if (gpio_bit & MASK_GPIO_BANK_0)
			{
				priv->output_status &= ~MASK_GPIO_BANK_0_LATCH;
				priv->output_status |= MASK_GPIO_BANK_1_LATCH;
			}
			else
			{
				priv->output_status &= ~MASK_GPIO_BANK_1_LATCH;
				priv->output_status |= MASK_GPIO_BANK_0_LATCH;
			}
		}
		latch_bus_data(priv, next_bank_data);

		mutex_unlock(&priv->lock);
	}
}

static int cassowary_output(struct gpio_chip *chip, unsigned offset, int value)
{
	return 0;
}

static int cassowary_input(struct gpio_chip *chip, unsigned offset)
{
	return 0;
}

static int cassowary_get(struct gpio_chip *chip, unsigned offset)
{
	return -ENOSYS;
}

static int cassowary_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct	cassowary_priv *gpio;
	int		status = 0;

	/* Allocate, initialize, and register this gpio_chip. */
	gpio = devm_kzalloc(&client->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	mutex_init(&gpio->lock);

	gpio->chip.base					= -1;
	gpio->chip.can_sleep			= true;
	gpio->chip.parent				= &client->dev;
	gpio->chip.owner				= THIS_MODULE;
	gpio->chip.set					= cassowary_set;
	gpio->chip.get					= cassowary_get;
	gpio->chip.direction_output		= cassowary_output;
	gpio->chip.direction_input		= cassowary_input;
	gpio->chip.ngpio				= id->driver_data;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		status = -EIO;

	if (status < 0)
		goto fail;

	gpio->chip.label = client->name;
	gpio->client = client;

	status = devm_gpiochip_add_data(&client->dev, &gpio->chip, gpio);
	if (status < 0)
		goto fail;

	//Disable writing to both bank 0 and bank 1 of latches on SLG HW
	gpio->output_status = MASK_GPIO_BYTE_CTRL;
	//Apply GPO default values:
	//Enable serial port inversion
	gpio->chip.set(&gpio->chip, 7, 1);
	//Take USB hub out of reset
	gpio->chip.set(&gpio->chip, 6, 1);

	dev_info(&client->dev, "probed\n");

	return 0;

fail:
	dev_dbg(&client->dev, "probe error %d for '%s'\n", status,
		client->name);

	return status;
}

static const struct i2c_device_id cassowary_ids[] = {
	{ "cassowary", 10 }, // 10 GPOs
	{ }
};
MODULE_DEVICE_TABLE(i2c, cassowary_ids);

static const struct of_device_id cassowary_of_match[] = {
	{ .compatible = "brightsign,cassowary" },
	{},
};
MODULE_DEVICE_TABLE(of, cassowary_of_match);

static struct i2c_driver cassowary_driver = {
	.driver = {
		.name	= "brightsign-cassowary",
		.of_match_table = of_match_ptr(cassowary_of_match),
	},
	.probe		 = cassowary_probe,
	.id_table	 = cassowary_ids,
};

static int __init cassowary_init(void)
{
	return i2c_add_driver(&cassowary_driver);
}
/* register after i2c postcore initcall and before
 * subsys initcalls that may rely on these GPIOs
 */
subsys_initcall(cassowary_init);

static void __exit cassowary_exit(void)
{
	i2c_del_driver(&cassowary_driver);
}
module_exit(cassowary_exit);

MODULE_DESCRIPTION("BrightSign Cassowary GPIO expander driver");
MODULE_AUTHOR("BrightSign Digital Ltd.");
MODULE_LICENSE("GPL v2");
