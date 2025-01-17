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

static unsigned char *fnd_addr;
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

unsigned char empty_fnd[4] = {0,};

// 각 버튼에 따른 인터럽트 핸들러
irqreturn_t home_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t back_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t volup_interrupt(int irq, void* dev_id, struct pt_regs* reg);
irqreturn_t voldown_interrupt(int irq, void* dev_id, struct pt_regs* reg);

wait_queue_head_t wq_write;
DECLARE_WAIT_QUEUE_HEAD(wq_write);


// 스탑워치 정보 저장을 위한 구조체
static struct mytimer{
	struct timer_list timer;
	int count;
	unsigned long prev_jiffies;
	unsigned long paused_jiffies;
	int is_running;
};

struct mytimer mytimer;

// 종료를 위한 타이머 구조체
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

// 볼륨다운을 누른 후 3초가 지났을 때 실행될 함수
void simple_wake_up(void)
{	
	del_timer(&mytimer.timer);
	del_timer(&exit_timer);
	set_fnd(empty_fnd);
	__wake_up(&wq_write,1,1,NULL);
}

irqreturn_t voldown_interrupt(int irq, void* dev_id, struct pt_regs* reg) {
	printk("voldown pressed : %d",voldown_pressed);

	// 볼륨 다운 버튼은 누를때와 땔 때 인터럽트가 발생한다.
	// 누를 때와 땔 때를 구분하여 처리한다. 
	if (voldown_pressed == False)
	{
		voldown_pressed = True;
		init_timer(&exit_timer);
		exit_timer.expires = get_jiffies_64() + 3 *HZ;
		exit_timer.function = simple_wake_up;
		add_timer(&exit_timer);

	}
	else //누른 후 3초가 지나기 전에 때게 되면 타이머를 해제한다.
	{
		del_timer(&exit_timer);
		voldown_pressed = False;
	}

    return IRQ_HANDLED;
}


int mod_open(struct inode *minode, struct file *mfile){
	int irq;

	// 인터럽트 할당
	printk("mod_open\n");
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
	request_irq(irq, voldown_interrupt, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "voldown", 0);

	printk("finish request_irq\n");

	//초기화
	set_fnd(empty_fnd);
	mytimer.prev_jiffies = 0;
	mytimer.count = 1;
	voldown_pressed = False;
	mytimer.is_running = False;

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
	// write 호출시 인터럽터블 슬립
	interruptible_sleep_on(&wq_write);
	return 0;
}

void set_fnd(unsigned char value[4])
{
	unsigned short int value_short = 0; 
	value_short = value[0] <<12 | value[1] << 8 | value[2] << 4 | value[3];
	outw(value_short, (unsigned int) fnd_addr);
}

void start_timer(void)
{
	// 타이머가 동작중이라면 아무일도 일어나지 않는다.
	if (mytimer.is_running == True)
		return;

	mytimer.is_running = True;
	init_timer(&mytimer.timer);
	mytimer.prev_jiffies = get_jiffies_64();

	// 만약에 초기상태가 아닌 정지상태였다면, 정지상태 때의 소수점만큼 적은 시간 뒤에 콜백 실행
	mytimer.timer.expires = mytimer.prev_jiffies + (HZ-mytimer.paused_jiffies);
	mytimer.timer.function = timer_callback;
	mytimer.paused_jiffies = 0;
	add_timer(&mytimer.timer);
}

void pause_timer(void)
{
	if (mytimer.is_running == False)
		return;
	mytimer.is_running = False;

	del_timer(&mytimer.timer);

	// 소수점 단위 초 보존을 위해 정지 - 이전 타이머 작동시 만큼의 jiffies를 보존한다.
	mytimer.paused_jiffies = get_jiffies_64() - mytimer.prev_jiffies;
	
}

void reset_timer(void)
{
	// 타이머 초기화
	del_timer(&mytimer.timer);
	mytimer.paused_jiffies = 0;
	mytimer.prev_jiffies = 0;
	mytimer.count = 1;
	mytimer.is_running = False;

	set_fnd(empty_fnd);
}

void timer_callback(void)
{
	// 매 초 1증가하는 카운트를 초,분으로 환산
	int min = mytimer.count / 60 % 60;
	int sec = mytimer.count % 60;

	char minsec[4];

	minsec[0] = min/10;
	minsec[1] = min%10;
	minsec[2] = sec/10;
	minsec[3] = sec%10;
	// fnd 표시
	set_fnd(minsec);

	// 1초후 다시호출
	mytimer.count++;
	mytimer.timer.expires = get_jiffies_64() + (HZ);
	mytimer.timer.function = timer_callback;
	

	add_timer(&mytimer.timer);
}

int __init mod_init(void)
{
    int result;
	printk("init\n");
    result = register_chrdev(DEV_MAJOR, DEV_NAME, &fops);
	printk("register_chdev\n");

    if (result<0)
        return result;

	fnd_addr = ioremap(FND_ADDRESS, 0x4);

	set_fnd(empty_fnd);
    return 0;

}

void __exit mod_exit(void) {
	set_fnd(empty_fnd);
	iounmap(fnd_addr);
	unregister_chrdev(DEV_MAJOR, DEV_NAME);
}


module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
