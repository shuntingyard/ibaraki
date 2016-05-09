#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <asm/uaccess.h>

/*
 * For more complete cmdl examples see:
 * http://www.tldp.org/LDP/lkmpg/2.6/html/lkmpg.html#AEN345
 */

static short int pin_rs		= 7;
static short int pin_rw		= 0;
static short int pin_e		= 8;
static short int pin_d4		= 25;
static short int pin_d5		= 24;
static short int pin_d6		= 23;
static short int pin_d7		= 18;

static short int autodev	= 0;

module_param	(autodev,	short, 0444);
MODULE_PARM_DESC(autodev,	"Kernel assigns dev maj:min if set to 1, default is 60:0");

module_param	(pin_d7,	short, 0444);
MODULE_PARM_DESC(pin_d7,	"D7 connected to gpio pin N°, default=18");
module_param	(pin_d6,	short, 0444);
MODULE_PARM_DESC(pin_d6,	"D6 connected to gpio pin N°, default=23");
module_param	(pin_d5,	short, 0444);
MODULE_PARM_DESC(pin_d5,	"D5 connected to gpio pin N°, default=24");
module_param	(pin_d4,	short, 0444);
MODULE_PARM_DESC(pin_d4,	"D4 connected to gpio pin N°, default=25");
module_param	(pin_e,		short, 0444);
MODULE_PARM_DESC(pin_e,		" E  connected to gpio pin N°, default=8");
module_param	(pin_rw,	short, 0444);
MODULE_PARM_DESC(pin_rw,	"RW requires connected gpio pin for reading, default=0 (GND/ write only)");
module_param	(pin_rs,	short, 0444);
MODULE_PARM_DESC(pin_rs,	"RS connected to gpio pin N°, default=7");

static char buffer_in[1024];
static char buffer_out[80];

/* local experimental, as proposed in the Kernel's devices.txt doc */
static dev_t 		hd44780_dev_number = MKDEV(60,0);

static struct cdev	*driver_object;
static struct class	*lcd_display;
static struct device	*hd44780_dev;

/* states for input handler */
#define WRT	0
#define ESC	1
#define ARG_a	2

static int state = WRT;
static int npr = 0; /* chars not printed by input handler */


/*
 * Set reg to 0 in cases busy flag and address should
 * be read, set to 1 for reading data.
 */
static int nibble_read(int reg) {

	int value=0;

	gpio_set_value(pin_rs, reg);
	gpio_set_value(pin_rw, 1);
	udelay(40);	/* Wait at least 40ns (tAStAS) */
	gpio_set_value(pin_e,  1);
	udelay(160);	/* Wait at least 160ns for the data to settle (tDDRtDDR) */

	gpio_direction_input(pin_d4);
	gpio_direction_input(pin_d5);
	gpio_direction_input(pin_d6);
	gpio_direction_input(pin_d7);

	value	= 1*gpio_get_value(pin_d4) + 2*gpio_get_value(pin_d5)
		+ 4*gpio_get_value(pin_d6) + 8*gpio_get_value(pin_d7);
/*
	printk("nibble_read %d: %d%d%d%d (0x%x)\n", reg,
		gpio_get_value(pin_d7), gpio_get_value(pin_d6),
		gpio_get_value(pin_d5), gpio_get_value(pin_d4),
		value
	);
*/
	gpio_set_value(pin_e,  0);
	udelay(5);	/* Wait at least 5ns (tDHRtDHR) */
	gpio_set_value(pin_rw, 0);
	udelay(40);

	return value;
}

static char get_address(void) {

	int value=nibble_read(0) << 4;	/* high order */
	value=value+nibble_read(0);

	return value & 0x7f;
}

static char lcd_read(void) {

	int value=nibble_read(1) << 4;	/* high order */
	value=value+nibble_read(1);

	return (char) value;
}

static void nibble_write(int reg, int value) {

	gpio_set_value(pin_rs, reg);
	gpio_direction_output(pin_d4, value & 0x1); /* lowest to lowest */
	gpio_direction_output(pin_d5, value & 0x2);
	gpio_direction_output(pin_d6, value & 0x4);
	gpio_direction_output(pin_d7, value & 0x8);

	printk("nibble_write %d: %d%d%d%d (0x%x)\n", reg,
		gpio_get_value(pin_d7), gpio_get_value(pin_d6),
		gpio_get_value(pin_d5), gpio_get_value(pin_d4),
		value
	);

	gpio_set_value(pin_e,  1);
	udelay(40);
	gpio_set_value(pin_e,  0);
}

static void lcd_write(int reg, int value) {

	nibble_write(reg, value >>   4); /* rshift high nibble into position */
	nibble_write(reg, value &  0xf); /* set all but low four bits to 0   */
}

static int gpio_request_wrapper(int nr) {

	char gpio_name[12]; int err;

	snprintf(gpio_name, sizeof(gpio_name), "gpio-%d", nr);

	err=gpio_request(nr, gpio_name);
	if ( err ) {
		printk("gpio_request for %s failed with %d\n", gpio_name, err);
		return -1;
	}

	if ( nr==pin_rs || nr==pin_rw || nr==pin_e ) {
		err=gpio_direction_output(nr, 0);
		if ( err ) {
			printk("gpio_direction_output failed %d\n", err);
			gpio_free(nr);
			return -1;
		}
	}

	return 0;
}

static int display_init(void) {

	if ( gpio_request_wrapper(pin_rs)==-1 )	return -EIO;
	if ( gpio_request_wrapper(pin_e )==-1 )	goto free_RS;
	if ( gpio_request_wrapper(pin_d7)==-1 )	goto free_E ;
	if ( gpio_request_wrapper(pin_d6)==-1 )	goto free_D7;
	if ( gpio_request_wrapper(pin_d5)==-1 )	goto free_D6;
	if ( gpio_request_wrapper(pin_d4)==-1 )	goto free_D5;
	/* Did we read as well? */
	if ( pin_rw!=0 )
		if ( gpio_request_wrapper(pin_rw)==-1 )
			goto free_D4;
	msleep(15);
	
	nibble_write(0, 0x3);	msleep(5);
	nibble_write(0, 0x3);	udelay(100);
	nibble_write(0, 0x3);	msleep(5);
	nibble_write(0, 0x2);	msleep(5);	/* 4-bit mode set! */
	
	lcd_write(0, 0x28);	msleep(2);	/* 2 lines with 5x8 font set! */

	lcd_write(0, 0x01);	msleep(2);	/* display cleared */

	lcd_write(0, 0x0c);	/* display on, cursor off, don't blink */

	/* mini easter-egg */
	lcd_write(0, 0x80);
	lcd_write(1, '<');
	lcd_write(1, '3');
	lcd_write(1, ' ');
	lcd_write(1, '4');
	lcd_write(1, 'o');
	
	return 0;

free_D4:gpio_free(pin_d4);
free_D5:gpio_free(pin_d5);
free_D6:gpio_free(pin_d6);
free_D7:gpio_free(pin_d7);
free_E :gpio_free(pin_e );
free_RS:gpio_free(pin_rs);

	return -EIO;
}

static void display_free(void) {

	lcd_write(0, 0x01);	msleep(2);	/* display cleared */

	lcd_write(0, 0x08);	msleep(2);	/* display off, cursor off, don't blink */

	gpio_free(pin_d4);
	gpio_free(pin_d5);
	gpio_free(pin_d6);
	gpio_free(pin_d7);
	gpio_free(pin_e );
	gpio_free(pin_rs);

	/* Did we read as well? */
	if ( pin_rw!=0 ) gpio_free(pin_rw);

	return;
}

static ssize_t driver_read(
	struct file *instance,
	char __user *user,
	size_t count,
	loff_t *offset)
{
	unsigned long not_copied, to_copy;

	int address, i;
	address=get_address();		/* save current address */
	lcd_write(0, 0x80);		/* set start address */

	for ( i=0; i<sizeof(buffer_out); i++ ) {

		buffer_out[i]=lcd_read();
	}
	lcd_write(0, 0x80+address);	/* restore address */

	to_copy=min(count, sizeof(buffer_out));
	not_copied=copy_to_user(user, buffer_out, to_copy);

	return to_copy-not_copied;
}

/*
 * A basic handler for some custom escape commands
 *
 * return 1 if char not printed
 */
static int input_handler(char c) {

	if ( c==033 ) {
		state=ESC;
		return 1;
	}
	else if ( state==ESC ) {
		
		switch ( c ) {
			case 'a': /* GOTO address */
				state=ARG_a; return 1;
			case 'c': /* clear */
				lcd_write(0, 0x01);
				msleep(2); state=WRT; return 1;
			case 'H': /* Home */
				lcd_write(0, 0x02);
				msleep(2); state=WRT; return 1;

			/* Don't decrement cursor for L to R writing systems */
			case 's': /* don't shift text in display */
				lcd_write(0, 0x06); break;
			case 'S': /* shift text in display */
				lcd_write(0, 0x07); break;

			case 'v': /* cursor not visible */
				lcd_write(0, 0x0c); break;
			case 'b': /* blink */
				lcd_write(0, 0x0d); break;
			case 'V': /* cursor Visible */
				lcd_write(0, 0x0e); break;
			case 'B': /* Blink and visible */
				lcd_write(0, 0x0f); break;

			case 'l': /* display right */
				lcd_write(0, 0x10); break;
			case 'r': /* display left */
				lcd_write(0, 0x14); break;
			case 'L': /* cursor right */
				lcd_write(0, 0x18); break;
			case 'R': /* cursor left */
				lcd_write(0, 0x1c); break;

			case 'f': /* singe line, small font */
				lcd_write(0, 0x20); break;
			case 'F': /* single line, large font */
				lcd_write(0, 0x24); break;
			case 'T': /* double line, small font */
				lcd_write(0, 0x28); break;

			case 'o': /* test cmd for reading (output), SECRET */
				printk("char read at 0x%02x '%c'\n",
					get_address(), (char) lcd_read());
				state=WRT; return 1;

			default : /* for syntax errors in 
				     escape commands just
				     discard char and return
				     to write mode.
				     */
				state=WRT; return 1;
		}
		/* standard return upon success */
		udelay(40);
		state=WRT;
		return 1;
	}
	else if ( state==ARG_a ) {
		/* Set address after range checks only! */
		if ( 	(0x80 <= (int) c && (int) c <= 0xa7) ||
			(0xc0 <= (int) c && (int) c <= 0xe7)
		   ) lcd_write(0, (int) c);
		//printk("ARG_a: 0x%02x\n", (int) c);
		state=WRT;
		return 1;
	}
	else if ( isprint(c) ) {
		lcd_write(1, c);
		return 0;
	}
	return 1;
}

static ssize_t driver_write(
	struct file *instance,
	const char __user *user,
	size_t count,
	loff_t *offset)
{
	unsigned long not_copied, to_copy;
	int i;

	to_copy=min(count, sizeof(buffer_in));
	not_copied=copy_from_user(buffer_in, user, to_copy);

//	lcd_write(0, 0x80);
	npr=0;
	for ( i=0; i<to_copy && buffer_in[i]; i++ ) {

		npr=npr+input_handler(buffer_in[i]);

//		if ( i-npr==15 )
//			lcd_write(0, 0xc0);
	}
	return to_copy-not_copied;
}

static struct file_operations fops = {
	.owner	= THIS_MODULE,
	.read	= driver_read,
	.write	= driver_write,
};

static int __init mod_init(void) {

	if ( autodev==1 ) {
		if ( alloc_chrdev_region(
					&hd44780_dev_number,
					0,
					1,
					"lcd_display")<0
		) return -EIO;
	}
       	else {
		if ( register_chrdev_region(
					hd44780_dev_number,
					1,
					"lcd_display")<0
		) {
			printk("Device number %u:%u in use!\n",
					MAJOR(hd44780_dev_number),
					MINOR(hd44780_dev_number));
			return  -EIO;
		}
	}

	driver_object = cdev_alloc();
	if ( driver_object==NULL ) goto free_device_number;

	driver_object->owner	= THIS_MODULE;
	driver_object->ops	= &fops;
	if ( cdev_add(driver_object, hd44780_dev_number, 1) ) goto free_cdev;

	lcd_display = class_create(THIS_MODULE, "lcd_display");
	if ( IS_ERR(lcd_display) ) {
		pr_err("lcd_display: class_create failed\n");
		goto free_cdev;
	}

	hd44780_dev = device_create(lcd_display,
			NULL,
			hd44780_dev_number,
			NULL,
			"%s",
			"hd44780");
	dev_info(hd44780_dev,
			"module_init RS=%hd RW=%hd E=%hd D4=%hd D5=%hd D6=%hd D7=%hd autodev=%hd\n",
			pin_rs, pin_rw, pin_e,
			pin_d4,pin_d5, pin_d6, pin_d7,
			autodev);

	if ( display_init()==0 ) return 0;

free_cdev:
	kobject_put(&driver_object->kobj);
free_device_number:
	unregister_chrdev_region(hd44780_dev_number, 1);

	return -EIO;
}

static void __exit mod_exit(void)
{
	dev_info(hd44780_dev, "module_exit called\n");
	display_free();
	device_destroy(lcd_display, hd44780_dev_number);
	class_destroy(lcd_display);
	cdev_del(driver_object);
	unregister_chrdev_region(hd44780_dev_number, 1);

	return;
}

module_init	(mod_init);
module_exit	(mod_exit);
MODULE_LICENSE	("GPL");
MODULE_AUTHOR	("shuntingyard@gmail.com");
