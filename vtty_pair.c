/* Virtual Straight-Through/AT TTY Pair driver
 * Copyright (c) 2026 Sage I. Hendricks
 * forked from Tiny TTY driver
 *
 * ######################################################################
 * Tiny TTY driver
 *
 * Copyright (C) 2002-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, version 2 of the License.
 *
 * This driver shows how to create a minimal tty driver.  It does not rely on
 * any backing hardware, but creates a timer that emulates data being received
 * from some kind of hardware.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>

#if defined SCULL_DEBUG
#   define PRINTK_DEBUG(...) printk(__VA_ARGS__)
#else
#   define PRINTK_DEBUG(...) (void)0
#endif

#define DRIVER_VERSION "v0.1"
#define DRIVER_AUTHOR "Sage I. Hendricks <sage.signup@email.com>"
#define DRIVER_DESC "vtty_pair virtual straight-through cable driver"

/* Module information */
MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

#define DELAY_TIME		HZ * 2	/* 2 seconds per character */
#define VTTYP_DATA_CHARACTER	't'

#define VTTYP_TTY_MAJOR		240	/* experimental range */
#define VTTYP_TTY_MINORS	4	/* only have 4 devices */

struct vttyp_serial {
	struct tty_struct	*tty;		/* pointer to the tty for this device */
	int			open_count;	/* number of times this port has been opened */
	struct semaphore	sem;		/* locks this structure */
	struct timer_list	*timer;

	/* for tiocmget and tiocmset functions */
	int			msr;		/* MSR shadow */
	int			mcr;		/* MCR shadow */

	/* for ioctl fun */
	struct serial_struct	serial;
	wait_queue_head_t	wait;
	struct async_icount	icount;
};

static struct tty_port *vttyp_ports = NULL;
static struct vttyp_serial *vttyp_table[VTTYP_TTY_MINORS];	/* initially all NULL */
static struct vttyp_serial *vttyp_timer_data = NULL;

static void vttyp_timer(struct timer_list *timer)
{
	struct vttyp_serial *port_context = vttyp_timer_data;
	struct tty_struct *tty;
	char data[1] = {VTTYP_DATA_CHARACTER};
	int data_size = 1;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p\n", __FUNCTION__, timer);

	if (!port_context)
		return;

	tty = port_context->tty;

	/* send the data to the tty layer for users to read.  This doesn't
	 * actually push the data through unless tty->low_latency is set */
	tty_buffer_request_room(tty->port, data_size);
	tty_insert_flip_string(tty->port, data, data_size);
	tty_flip_buffer_push(tty->port);

	/* resubmit the timer again */
	port_context->timer->expires = jiffies + DELAY_TIME;
	add_timer(port_context->timer);
}

static int vttyp_open(struct tty_struct *tty, struct file *file)
{
	struct vttyp_serial *port_context;
	struct timer_list *timer;
	int index;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %p\n", __FUNCTION__, tty,
		file);

	/* initialize the pointer in case something fails */
	tty->driver_data = NULL;

	/* get the serial object associated with this tty pointer */
	index = tty->index;
	port_context = vttyp_table[index];
	if (port_context == NULL) {
		/* first time accessing this device, let's create it */
		port_context = kmalloc(sizeof(*port_context), GFP_KERNEL);
		if (!port_context)
			return -ENOMEM;

		sema_init(&port_context->sem, 1);
		port_context->open_count = 0;
		port_context->timer = NULL;

		vttyp_table[index] = port_context;
	}

	down(&port_context->sem);

	/* save our structure within the tty structure */
	tty->driver_data = port_context;
	port_context->tty = tty;

	++port_context->open_count;
	if (port_context->open_count == 1) {
		/* this is the first time this port is opened */
		/* do any hardware initialization needed here */

		/* create our timer and submit it */
		if (!port_context->timer) {
			timer = kmalloc(sizeof(*timer), GFP_KERNEL);
			if (!timer) {
				up(&port_context->sem);
				return -ENOMEM;
			}
			port_context->timer = timer;
		}
		vttyp_timer_data = port_context;
		port_context->timer->expires = jiffies + DELAY_TIME;
		port_context->timer->function = vttyp_timer;
		add_timer(port_context->timer);
	}

	up(&port_context->sem);
	return 0;
}

static void do_close(struct vttyp_serial *port_context)
{
	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p\n", __FUNCTION__,
		port_context);

	down(&port_context->sem);

	if (!port_context->open_count) {
		/* port was never opened */
		goto exit;
	}

	--port_context->open_count;
	if (port_context->open_count <= 0) {
		/* The port is being closed by the last user. */
		/* Do any hardware specific stuff here */

		/* shut down our timer */
		del_timer(port_context->timer);
	}
exit:
	up(&port_context->sem);
}

static void vttyp_close(struct tty_struct *tty, struct file *file)
{
	struct vttyp_serial *port_context = tty->driver_data;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %p\n", __FUNCTION__, tty,
		file);

	if (port_context)
		do_close(port_context);
}	

static ssize_t vttyp_write(struct tty_struct *tty, 
                           const u8 *buffer, size_t count)
{
	struct vttyp_serial *port_context = tty->driver_data;
	int i;
	int retval = -EINVAL;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %p %zu\n", __FUNCTION__, tty,
		buffer, count);

	if (!port_context)
		return -ENODEV;

	down(&port_context->sem);

	if (!port_context->open_count)
		/* port was not opened */
		goto exit;

	/* fake sending the data out a hardware port by
	 * writing it to the kernel debug log.
	 */
	PRINTK_DEBUG(KERN_DEBUG "%s - ", __FUNCTION__);
	for (i = 0; i < count; ++i)
		printk("%02x ", buffer[i]);
	printk("\n");
		
exit:
	up(&port_context->sem);
	return retval;
}

static unsigned int vttyp_write_room(struct tty_struct *tty) 
{
	struct vttyp_serial *port_context = tty->driver_data;
	int room = -EINVAL;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p\n", __FUNCTION__, tty);

	if (!port_context)
		return -ENODEV;

	down(&port_context->sem);
	
	if (!port_context->open_count) {
		/* port was not opened */
		goto exit;
	}

	/* calculate how much room is left in the device */
	room = 255;

exit:
	up(&port_context->sem);
	return room;
}

#define RELEVANT_IFLAG(iflag) ((iflag) & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

static void vttyp_set_termios(struct tty_struct *tty,
                              const struct ktermios *old_termios)
{
	unsigned int cflag;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %p\n", __FUNCTION__, tty,
		old_termios);

	cflag = tty->termios.c_cflag;

	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
		    (RELEVANT_IFLAG(tty->termios.c_iflag) == 
		     RELEVANT_IFLAG(old_termios->c_iflag))) {
			PRINTK_DEBUG(KERN_DEBUG " - nothing to change...\n");
			return;
		}
	}

	/* get the byte size */
	switch (cflag & CSIZE) {
		case CS5:
			PRINTK_DEBUG(KERN_DEBUG " - data bits = 5\n");
			break;
		case CS6:
			PRINTK_DEBUG(KERN_DEBUG " - data bits = 6\n");
			break;
		case CS7:
			PRINTK_DEBUG(KERN_DEBUG " - data bits = 7\n");
			break;
		default:
		case CS8:
			PRINTK_DEBUG(KERN_DEBUG " - data bits = 8\n");
			break;
	}
	
	/* determine the parity */
	if (cflag & PARENB)
		if (cflag & PARODD)
			PRINTK_DEBUG(KERN_DEBUG " - parity = odd\n");
		else
			PRINTK_DEBUG(KERN_DEBUG " - parity = even\n");
	else
		PRINTK_DEBUG(KERN_DEBUG " - parity = none\n");

	/* figure out the stop bits requested */
	if (cflag & CSTOPB)
		PRINTK_DEBUG(KERN_DEBUG " - stop bits = 2\n");
	else
		PRINTK_DEBUG(KERN_DEBUG " - stop bits = 1\n");

	/* figure out the hardware flow control settings */
	if (cflag & CRTSCTS)
		PRINTK_DEBUG(KERN_DEBUG " - RTS/CTS is enabled\n");
	else
		PRINTK_DEBUG(KERN_DEBUG " - RTS/CTS is disabled\n");
	
	/* determine software flow control */
	/* if we are implementing XON/XOFF, set the start and 
	 * stop character in the device */
	if (I_IXOFF(tty) || I_IXON(tty)) {
		unsigned char stop_char  = STOP_CHAR(tty);
		unsigned char start_char = START_CHAR(tty);

		/* if we are implementing INBOUND XON/XOFF */
		if (I_IXOFF(tty))
			PRINTK_DEBUG(KERN_DEBUG " - INBOUND XON/XOFF is "
				"enabled, XON = %2x, XOFF = %2x", start_char,
				stop_char);
		else
			printk(KERN_DEBUG" - INBOUND XON/XOFF is disabled");

		/* if we are implementing OUTBOUND XON/XOFF */
		if (I_IXON(tty))
			printk(KERN_DEBUG" - OUTBOUND XON/XOFF is enabled, "
				"XON = %2x, XOFF = %2x", start_char, 
				stop_char);
		else
			printk(KERN_DEBUG" - OUTBOUND XON/XOFF is disabled");
	}

	/* get the baud rate wanted */
	PRINTK_DEBUG(KERN_DEBUG " - baud rate = %d", tty_get_baud_rate(tty));
}

/* Our fake UART values */
#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_LOOP	0x04
#define MSR_CTS		0x08
#define MSR_CD		0x10
#define MSR_RI		0x20
#define MSR_DSR		0x40

static int vttyp_tiocmget(struct tty_struct *tty)
{
	struct vttyp_serial *port_context = tty->driver_data;

	unsigned int result = 0;
	unsigned int msr = port_context->msr;
	unsigned int mcr = port_context->mcr;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p\n", __FUNCTION__, tty);

	result = ((mcr & MCR_DTR)  ? TIOCM_DTR  : 0) |	/* DTR is set */
                 ((mcr & MCR_RTS)  ? TIOCM_RTS  : 0) |	/* RTS is set */
                 ((mcr & MCR_LOOP) ? TIOCM_LOOP : 0) |	/* LOOP is set */
                 ((msr & MSR_CTS)  ? TIOCM_CTS  : 0) |	/* CTS is set */
                 ((msr & MSR_CD)   ? TIOCM_CAR  : 0) |	/* Carrier detect is set*/
                 ((msr & MSR_RI)   ? TIOCM_RI   : 0) |	/* Ring Indicator is set */
                 ((msr & MSR_DSR)  ? TIOCM_DSR  : 0);	/* DSR is set */

	return result;
}

static int vttyp_tiocmset(struct tty_struct *tty, unsigned int set,
                         unsigned int clear)
{
	struct vttyp_serial *port_context = tty->driver_data;
	unsigned int mcr = port_context->mcr;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %x %x\n", __FUNCTION__, tty,
		set, clear);

	if (set & TIOCM_RTS)
		mcr |= MCR_RTS;
	if (set & TIOCM_DTR)
		mcr |= MCR_RTS;

	if (clear & TIOCM_RTS)
		mcr &= ~MCR_RTS;
	if (clear & TIOCM_DTR)
		mcr &= ~MCR_RTS;

	/* set the new MCR value in the device */
	port_context->mcr = mcr;
	return 0;
}

static int vttyp_ioctl_tiocgserial(struct tty_struct *tty, unsigned int cmd,
                                  unsigned long arg)
{
	struct vttyp_serial *port_context = tty->driver_data;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %u %lu\n", __FUNCTION__,
		tty, cmd, arg);

	if (cmd == TIOCGSERIAL) {
		struct serial_struct tmp;

		if (!arg)
			return -EFAULT;

		memset(&tmp, 0, sizeof(tmp));

		tmp.type		= port_context->serial.type;
		tmp.line		= port_context->serial.line;
		tmp.port		= port_context->serial.port;
		tmp.irq			= port_context->serial.irq;
		tmp.flags		= ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
		tmp.xmit_fifo_size	= port_context->serial.xmit_fifo_size;
		tmp.baud_base		= port_context->serial.baud_base;
		tmp.close_delay		= 5*HZ;
		tmp.closing_wait	= 30*HZ;
		tmp.custom_divisor	= port_context->serial.custom_divisor;
		tmp.hub6		= port_context->serial.hub6;
		tmp.io_type		= port_context->serial.io_type;

		if (copy_to_user((void __user *)arg, &tmp, sizeof(struct serial_struct)))
			return -EFAULT;
		return 0;
	}
	return -ENOIOCTLCMD;
}

static int vttyp_ioctl_tiocmiwait(struct tty_struct *tty, unsigned int cmd,
                                  unsigned long arg)
{
	struct vttyp_serial *port_context = tty->driver_data;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %u %lu\n", __FUNCTION__, tty,
		cmd, arg);

	if (cmd == TIOCMIWAIT) {
		DECLARE_WAITQUEUE(wait, current);
		struct async_icount cnow;
		struct async_icount cprev;

		cprev = port_context->icount;
		while (1) {
			add_wait_queue(&port_context->wait, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			remove_wait_queue(&port_context->wait, &wait);

			/* see if a signal woke us up */
			if (signal_pending(current))
				return -ERESTARTSYS;

			cnow = port_context->icount;
			if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
			    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
				return -EIO; /* no change => error */
			if (((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
			    ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
			    ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
			    ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
				return 0;
			}
			cprev = cnow;
		}

	}
	return -ENOIOCTLCMD;
}

static int vttyp_ioctl_tiocgicount(struct tty_struct *tty, unsigned int cmd, 
                                  unsigned long arg)
{
	struct vttyp_serial *port_context = tty->driver_data;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %u %lu\n", __FUNCTION__, tty, cmd, arg);

	if (cmd == TIOCGICOUNT) {
		struct async_icount cnow = port_context->icount;
		struct serial_icounter_struct icount;

		icount.cts	= cnow.cts;
		icount.dsr	= cnow.dsr;
		icount.rng	= cnow.rng;
		icount.dcd	= cnow.dcd;
		icount.rx	= cnow.rx;
		icount.tx	= cnow.tx;
		icount.frame	= cnow.frame;
		icount.overrun	= cnow.overrun;
		icount.parity	= cnow.parity;
		icount.brk	= cnow.brk;
		icount.buf_overrun = cnow.buf_overrun;

		if (copy_to_user((void __user *)arg, &icount, sizeof(icount)))
			return -EFAULT;
		return 0;
	}
	return -ENOIOCTLCMD;
}

/* the real vttyp_ioctl function.  The above is done to get the small functions in the book */
static int vttyp_ioctl(struct tty_struct *tty, unsigned int cmd,
                       unsigned long arg)
{
	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s %p %u %lu\n", __FUNCTION__, tty,
		cmd, arg);

	switch (cmd) {
	case TIOCGSERIAL:
		return vttyp_ioctl_tiocgserial(tty, cmd, arg);
	case TIOCMIWAIT:
		return vttyp_ioctl_tiocmiwait(tty, cmd, arg);
	case TIOCGICOUNT:
		return vttyp_ioctl_tiocgicount(tty, cmd, arg);
	}

	return -ENOIOCTLCMD;
}

static struct tty_operations serial_ops = {
	.open = vttyp_open,
	.close = vttyp_close,
	.write = vttyp_write,
	.write_room = vttyp_write_room,
	.set_termios = vttyp_set_termios,
	.tiocmget = vttyp_tiocmget,
	.tiocmset = vttyp_tiocmset,
	.ioctl = vttyp_ioctl,
};

static struct tty_driver *vttyp_tty_driver;

static int __init vttyp_init(void)
{
	int retval;
	int i;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s\n", __FUNCTION__);

	/* allocate ports */
	vttyp_ports = kmalloc(VTTYP_TTY_MINORS * sizeof (struct tty_port), GFP_KERNEL);

	/* allocate the tty driver */
	vttyp_tty_driver = tty_alloc_driver(VTTYP_TTY_MINORS, 0);
	if (!vttyp_tty_driver) {
		printk(KERN_ERR "vtty_pair: failed to allocate vtty_pair driver");
		return -ENOMEM;
	}

	/* initialize the tty driver */
	vttyp_tty_driver->owner = THIS_MODULE;
	vttyp_tty_driver->driver_name = "vttyp_tty";
	vttyp_tty_driver->name = "vttyp";
	vttyp_tty_driver->major = VTTYP_TTY_MAJOR,
	vttyp_tty_driver->type = TTY_DRIVER_TYPE_SERIAL,
	vttyp_tty_driver->subtype = SERIAL_TYPE_NORMAL,
	vttyp_tty_driver->flags = TTY_DRIVER_REAL_RAW,
	vttyp_tty_driver->init_termios = tty_std_termios;
	vttyp_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(vttyp_tty_driver, &serial_ops);

	/* register the tty driver */
	for (i = 0; i < VTTYP_TTY_MINORS; ++i) {
		tty_port_init(&vttyp_ports[i]);
		tty_port_link_device(&vttyp_ports[i], vttyp_tty_driver, i);
	}

	retval = tty_register_driver(vttyp_tty_driver);
	if (retval) {
		printk(KERN_ERR "vtty_pair: failed to register vtty_pair driver");
		tty_driver_kref_put(vttyp_tty_driver);
		return retval;
	}

	printk(KERN_INFO "vtty_pair: " DRIVER_DESC " " DRIVER_VERSION);
	return retval;
}

static void __exit vttyp_exit(void)
{
	struct vttyp_serial *port_context;
	int i;

	PRINTK_DEBUG(KERN_DEBUG "vtty_pair: %s\n", __FUNCTION__);

	for (i = 0; i < VTTYP_TTY_MINORS; ++i) {
		tty_port_destroy(&vttyp_ports[i]);
		tty_unregister_device(vttyp_tty_driver, i);
	}

	tty_unregister_driver(vttyp_tty_driver);

	/* shut down all of the timers and free the memory */
	for (i = 0; i < VTTYP_TTY_MINORS; ++i) {
		port_context = vttyp_table[i];
		if (port_context) {
			/* close the port */
			while (port_context->open_count)
				do_close(port_context);

			/* shut down our timer and free the memory */
			del_timer(port_context->timer);
			kfree(port_context->timer);
			kfree(port_context);
			vttyp_table[i] = NULL;
		}
	}

    kfree(vttyp_ports);
}

module_init(vttyp_init);
module_exit(vttyp_exit);
/* vim: ts=8 sts=8 sw=8 noet
 * end of file */
