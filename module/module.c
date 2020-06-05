#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <asm/irq.h>
#include <mach/gpio.h>
#include <linux/platform_device.h>
#include <asm/gpio.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/cdev.h>
#include <linux/timer.h>
#include <linux/wait.h>

#define DEV_MAJOR 242
#define DEV_NAME "stopwatch"
#define FND_ADDRESS 0x08000004

#define True 1
#define False 0

static int voldown_pressed;
static int result;
static int mod_open(struct inode *, struct file *);
static int mod_release(struct inode *, struct file *);
static int mod_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos);

void start_timer(void);
void pause_timer(void);
void reset_timer(void);
void set_fnd(unsigned char value[4]);
void timer_callback(void);

irqreturn_t home_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t back_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t volup_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t voldown_interrupt(int irq, void* dev_id, struct pt_regs* reg);

wait_queue_head_t wq_write;
DECLARE_WAIT_QUEUE_HEAD(wq_write);

static struct mytimer{
	struct timer_list timer;
	int count;
	unsigned long prev_jiffies;
	unsigned long paused_jiffies;
};

struct mytimer mytimer;
struct timer_list exit_timer;


static struct file_operations fops =
{
	.open = mod_open,
	.write = mod_write,
	.release = mod_release,
};

irqreturn_t home_interrupt(int irq, void* dev_id, struct pt_regs* reg) {
	start_timer();
	return IRQ_HANDLED;
}

irqreturn_t back_interrupt(int irq, void* dev_id, struct pt_regs* reg) {
	pause_timer();
    return IRQ_HANDLED;
}

irqreturn_t volup_interrupt(int irq, void* dev_id,struct pt_regs* reg) {
	reset_timer();
    return IRQ_HANDLED;
}


void simple_wake_up(void)
{
	__wake_up(&wq_write,1,1,NULL);
}

irqreturn_t voldown_interrupt(int irq, void* dev_id, struct pt_regs* reg) {
	if (voldown_pressed == False)
	{
		voldown_pressed = True;
		init_timer(&exit_timer);
		exit_timer.expires = get_jiffies_64() + 3 *HZ;
		exit_timer.function = simple_wake_up;
		add_timer(&exit_timer);

	}
	else
	{
		voldown_pressed = False;
		del_timer(&exit_timer);
	}

    return IRQ_HANDLED;
}


int mod_open(struct inode *minode, struct file *mfile){
	int irq;

	// home
	gpio_direction_input(IMX_GPIO_NR(1,11));
	irq = gpio_to_irq(IMX_GPIO_NR(1,11));
	request_irq(irq, home_interrupt, IRQF_TRIGGER_RISING, "home", 0);

	// back
	gpio_direction_input(IMX_GPIO_NR(1,12));
	irq = gpio_to_irq(IMX_GPIO_NR(1,12));
	request_irq(irq, back_interrupt, IRQF_TRIGGER_RISING, "back", 0);

	// volup
	gpio_direction_input(IMX_GPIO_NR(2,15));
	irq = gpio_to_irq(IMX_GPIO_NR(2,15));
	request_irq(irq, volup_interrupt, IRQF_TRIGGER_RISING, "volup", 0);

	// voldown
	gpio_direction_input(IMX_GPIO_NR(5,14));
	irq = gpio_to_irq(IMX_GPIO_NR(5,14));
	request_irq(irq, voldown_interrupt, IRQF_TRIGGER_RISING | IRQF_TRIGGER_RISING, "voldown", 0);

	set_fnd("0000");
	mytimer.prev_jiffies = 0;
	mytimer.count = 0;
	voldown_pressed = False;

	return 0;
}

int mod_release(struct inode *minode, struct file *mfile){
	free_irq(gpio_to_irq(IMX_GPIO_NR(1, 11)), NULL);
	free_irq(gpio_to_irq(IMX_GPIO_NR(1, 12)), NULL);
	free_irq(gpio_to_irq(IMX_GPIO_NR(2, 15)), NULL);
	free_irq(gpio_to_irq(IMX_GPIO_NR(5, 14)), NULL);
	
	return 0;
}

static int mod_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos )
{
	interruptible_sleep_on(&wq_write);
	return 0;
}

void set_fnd(unsigned char value[4])
{
	unsigned short int value_short = 0; 
	value_short = value[0] <<12 | value[1] << 8 | value[2] << 4 | value[3];
	outw(value_short, (unsigned int) FND_ADDRESS);
}

void start_timer(void)
{
	init_timer(&mytimer.timer);
	mytimer.prev_jiffies = get_jiffies_64();
	mytimer.timer.expires = mytimer.prev_jiffies + (HZ-mytimer.paused_jiffies);
	mytimer.timer.function = timer_callback;
	mytimer.paused_jiffies = 0;
	add_timer(&mytimer.timer);
}

void pause_timer(void)
{
	del_timer(&mytimer.timer);
	mytimer.paused_jiffies = get_jiffies_64() - mytimer.prev_jiffies;
}

void reset_timer(void)
{
	del_timer(&mytimer.timer);
	mytimer.paused_jiffies = 0;
	mytimer.prev_jiffies = 0;
	mytimer.count = 0;
	set_fnd("0000");
}

void timer_callback(void)
{
	// count to minute, second
	int min = mytimer.count / 60 % 60;
	int sec = mytimer.count % 60;

	char minsec[4];

    // int to char
	minsec[0] = '0' + min/10;
	minsec[1] = '0' + min%10;
	minsec[2] = '0' + sec/10;
	minsec[3] = '0' + sec%10;
	
	set_fnd(minsec);

	// call after 1 sec
	mytimer.timer.expires = get_jiffies_64() + (HZ);
	mytimer.timer.function = timer_callback;
	mytimer.count++;

	add_timer(&mytimer.timer);
}

int __init mod_init(void)
{
    int result;
    result = register_chrdev(DEV_MAJOR, DEV_NAME, &fops);

    if (result<0)
        return result;

	set_fnd("0000");
    return 0;

}

void __exit mod_exit(void) {
	del_timer_sync(&mytimer.timer);
	del_timer_sync(&exit_timer);
	unregister_chrdev(DEV_MAJOR, DEV_NAME);
}


module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
