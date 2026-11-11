// SPDX-License-Identifier: GPL-2.0
/*
 * DEBUG (run 03 bring-up): after kexec from 4.14 a woken SCHED_FIFO task
 * (msm-watchdog) never gets a CPU. Probe whether IPIs / remote wakeups
 * reach each CPU. Never blocks on a CPU that does not answer.
 */
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/sched/clock.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <asm/irq_regs.h>
#include <linux/ptrace.h>
#include <linux/sched/debug.h>

struct k61_cpu {
	struct task_struct *t;
	wait_queue_head_t wq;
	bool go;
	u64 wake_ack;
	call_single_data_t csd;
	u64 ipi_ack;
};
static struct k61_cpu k61c[NR_CPUS];

static void k61_ipi_fn(void *info)
{
	struct k61_cpu *c = info;

	c->ipi_ack = sched_clock();
}

void k61_dump_curr(int cpu)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (task_cpu(t) == cpu && task_curr(t))
			pr_emerg("k61ipi: cpu%d curr=%s pid=%d state=%x prio=%d policy=%u\n",
				 cpu, t->comm, t->pid, READ_ONCE(t->__state),
				 t->prio, t->policy);
	}
	rcu_read_unlock();
}

static call_single_data_t k61_regs_csd[NR_CPUS];

static void k61_regs_fn(void *info)
{
	pr_emerg("k61ipi: cpu%d interrupted context:\n", raw_smp_processor_id());
	show_regs(get_irq_regs());
}

static int k61_worker(void *arg)
{
	struct k61_cpu *c = arg;

	while (!kthread_should_stop()) {
		wait_event_interruptible(c->wq, c->go);
		c->wake_ack = sched_clock();
		c->go = false;
	}
	return 0;
}

static struct timer_list k61_irq_timer;
static unsigned int k61_irq_last[1024];

static void k61_irq_fn(struct timer_list *t)
{
	int irq;

	rcu_read_lock();
	for_each_irq_nr(irq) {
		struct irq_desc *d;
		unsigned int c, delta;

		if (irq >= 1024)
			break;
		d = irq_to_desc(irq);
		if (!d)
			continue;
		c = kstat_irqs_usr(irq);
		delta = c - k61_irq_last[irq];
		k61_irq_last[irq] = c;
		if (delta > 40)
			pr_info("k61irq: irq%d %s +%u/s\n", irq,
				d->action ? d->action->name : "-", delta);
	}
	rcu_read_unlock();
	mod_timer(&k61_irq_timer, jiffies + HZ);
}

static struct timer_list k61_hb_timer;
static unsigned int k61_hb_n;

static void k61_hb_fn(struct timer_list *t)
{
	pr_info("k61hbT: %u jiffies=%lu cpu%d\n", k61_hb_n++, jiffies, raw_smp_processor_id());
	mod_timer(&k61_hb_timer, jiffies + HZ / 2);
}

static int k61_probe_thread(void *arg)
{
	int round, cpu;

	for_each_online_cpu(cpu) {
		struct k61_cpu *c = &k61c[cpu];

		init_waitqueue_head(&c->wq);
		c->t = kthread_create(k61_worker, c, "k61w/%d", cpu);
		if (IS_ERR(c->t)) {
			c->t = NULL;
			continue;
		}
		kthread_bind(c->t, cpu);
		wake_up_process(c->t);
	}
	msleep(200);

	for (round = 0; round < 9; round++) {
		if (round == 1) {
			for (cpu = 5; cpu < 8; cpu++) {
				pr_emerg("k61ipi: cpu%d idle_cpu=%d\n", cpu, idle_cpu(cpu));
				k61_dump_curr(cpu);
			}
			INIT_CSD(&k61_regs_csd[7], k61_regs_fn, NULL);
			smp_call_function_single_async(7, &k61_regs_csd[7]);
			msleep(100);
		}
		for_each_online_cpu(cpu) {
			struct k61_cpu *c = &k61c[cpu];
			u64 t0;
			bool ipi_ok, wake_ok;

			c->ipi_ack = 0;
			c->wake_ack = 0;
			t0 = sched_clock();
			INIT_CSD(&c->csd, k61_ipi_fn, c);
			smp_call_function_single_async(cpu, &c->csd);
			if (c->t) {
				c->go = true;
				wake_up(&c->wq);
			}
			msleep(40);
			ipi_ok = c->ipi_ack != 0;
			wake_ok = c->wake_ack != 0;
			pr_info("k61ipi: r%d from cpu%d -> cpu%d ipi=%s(%lluus) wake=%s(%lluus)\n",
				round, raw_smp_processor_id(), cpu,
				ipi_ok ? "ok" : "LOST",
				ipi_ok ? (c->ipi_ack - t0) / 1000 : 0,
				wake_ok ? "ok" : "LOST",
				wake_ok ? (c->wake_ack - t0) / 1000 : 0);
			/* csd still busy if IPI lost; later rounds just skip re-queue */
			if (!ipi_ok)
				msleep(1);
		}
		msleep(500);
	}
	for (;;) {
		pr_info("k61hbK: %llu ms cpu%d\n", sched_clock() / 1000000, raw_smp_processor_id());
		msleep(250);
	}
	return 0;
}

static int __init k61_ipitest_init(void)
{
	timer_setup(&k61_irq_timer, k61_irq_fn, 0);
	mod_timer(&k61_irq_timer, jiffies + HZ);
	timer_setup(&k61_hb_timer, k61_hb_fn, 0);
	mod_timer(&k61_hb_timer, jiffies + HZ / 2);
	kthread_run(k61_probe_thread, NULL, "k61ipitest");
	return 0;
}
device_initcall(k61_ipitest_init);
