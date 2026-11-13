// SPDX-License-Identifier: GPL-2.0
/*
 * DEBUG (run 03 bring-up): after kexec from 4.14 a woken SCHED_FIFO task
 * (msm-watchdog) never gets a CPU. Probe whether IPIs / remote wakeups
 * reach each CPU. Never blocks on a CPU that does not answer.
 */
#include <linux/nmi.h>
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
#include <linux/console.h>
#include <linux/perf_event.h>

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
	if (get_irq_regs())
		show_regs(get_irq_regs());
}

/* bark: drop fbcon, flush pending records to ramoops even if console_lock is stuck */
void k61_bark_flush(void)
{
	struct console *con;

	for_each_console(con)
		if (strncmp(con->name, "ramoops", 7))
			con->flags &= ~CON_ENABLED;
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);
}

extern void k61_scm_report(void);
void k61_gic_report(void);

void k61_dump_all_cpus(void)
{
	int self = raw_smp_processor_id();
	int c;

	pr_emerg("k61wdt: bark on cpu%d console_locked=%d\n", self, is_console_locked());
	k61_scm_report();
	k61_gic_report();
	for_each_online_cpu(c) {
		struct task_struct *ks = per_cpu(ksoftirqd, c);

		k61_dump_curr(c);
		pr_emerg("k61wdt: cpu%d softirq_pending=%#x ksoftirqd=%s state=%x on_cpu=%d\n",
			 c, per_cpu(irq_stat, c).__softirq_pending,
			 ks ? ks->comm : "-", ks ? READ_ONCE(ks->__state) : 0,
			 ks ? READ_ONCE(ks->on_cpu) : -1);
	}
	pr_emerg("k61ipi: cpu%d interrupted context (bark cpu):\n", self);
	if (get_irq_regs())
		show_regs(get_irq_regs());
	for_each_online_cpu(c) {
		if (c == self)
			continue;
		INIT_CSD(&k61_regs_csd[c], k61_regs_fn, NULL);
		smp_call_function_single_async(c, &k61_regs_csd[c]);
	}
	/* pseudo-NMI (irqchip.gicv3_pseudo_nmi=1): reaches cpus with irqs off */
	pr_emerg("k61wdt: NMI backtrace of all other cpus\n");
	trigger_allbutself_cpu_backtrace();
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

/*
 * DEBUG (run 03): per-cpu PMU cycle counter overflow = pseudo-NMI every
 * ~50 ms (needs irqchip.gicv3_pseudo_nmi=1). If a non-idle cpu has taken no
 * arch timer interrupt for ~300 ms it is stuck with irqs off or in a loop:
 * print where, from NMI context. Printk from NMI only fills the log buffer;
 * the console is flushed by whichever live cpu prints next (heartbeats).
 */
static struct perf_event *k61_pe[NR_CPUS];
static unsigned long k61_nmi_last[NR_CPUS];
static unsigned int k61_nmi_stall[NR_CPUS];

static void k61_nmi_ovf(struct perf_event *ev, struct perf_sample_data *data,
			struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	unsigned long n = kstat_irqs_cpu(11, cpu);

	if (is_idle_task(current) || n != k61_nmi_last[cpu]) {
		k61_nmi_last[cpu] = n;
		k61_nmi_stall[cpu] = 0;
		return;
	}
	k61_nmi_stall[cpu]++;
	if (k61_nmi_stall[cpu] == 6 || k61_nmi_stall[cpu] == 40 ||
	    k61_nmi_stall[cpu] == 120) {
		pr_emerg("k61nmi: cpu%d no timer irq for %u samples curr=%s pid=%d pc=%pS lr=%pS %s\n",
			 cpu, k61_nmi_stall[cpu], current->comm, current->pid,
			 (void *)instruction_pointer(regs),
			 (void *)regs->regs[30],
			 user_mode(regs) ? "(user)" : "(kernel)");
		show_regs(regs);
	}
}

static int __init k61_nmi_init(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CPU_CYCLES,
		.size = sizeof(struct perf_event_attr),
		.pinned = 1,
		.sample_period = 100000000ULL,
	};
	int cpu;

	for_each_online_cpu(cpu) {
		struct perf_event *e;

		e = perf_event_create_kernel_counter(&attr, cpu, NULL,
						     k61_nmi_ovf, NULL);
		if (IS_ERR(e)) {
			pr_err("k61nmi: cpu%d counter: %ld\n", cpu, PTR_ERR(e));
			continue;
		}
		k61_pe[cpu] = e;
	}
	pr_info("k61nmi: sampler armed\n");
	return 0;
}
late_initcall(k61_nmi_init);

/*
 * DEBUG (run 03): cross-cpu coherency probe. For every ordered pair (a, b):
 * cpu b caches a flag and polls it (irqs off, 20 ms max), then cpu a writes
 * it. If b never sees the write, the caches of a and b are not coherent.
 */
static int k61_coh_flag, k61_coh_seen, k61_coh_done;
static call_single_data_t k61_coh_csd;

static void k61_coh_poll(void *info)
{
	u64 t0 = sched_clock();
	int seen = 0;

	while (sched_clock() - t0 < 20000000ULL) {
		if (READ_ONCE(k61_coh_flag)) {
			seen = 1;
			break;
		}
		cpu_relax();
	}
	WRITE_ONCE(k61_coh_seen, seen);
	smp_wmb();
	WRITE_ONCE(k61_coh_done, 1);
}

static void k61_coh_set(void *info)
{
	WRITE_ONCE(k61_coh_flag, 1);
}

static int __maybe_unused k61_coh_thread(void *arg)
{
	unsigned int round = 0;

	msleep(1500);
	for (;;) {
		int a, b, bad = 0, skipped = 0, n = 0;

		for_each_online_cpu(a) {
			for_each_online_cpu(b) {
				int waited;

				if (a == b)
					continue;
				WRITE_ONCE(k61_coh_flag, 0);
				WRITE_ONCE(k61_coh_seen, 0);
				WRITE_ONCE(k61_coh_done, 0);
				smp_wmb();
				INIT_CSD(&k61_coh_csd, k61_coh_poll, NULL);
				if (smp_call_function_single_async(b, &k61_coh_csd)) {
					skipped++;
					continue;
				}
				usleep_range(1000, 1500);
				smp_call_function_single(a, k61_coh_set, NULL, 0);
				for (waited = 0; waited < 40 && !READ_ONCE(k61_coh_done); waited++)
					usleep_range(1000, 1500);
				n++;
				if (!READ_ONCE(k61_coh_done)) {
					pr_emerg("k61coh: %d->%d poll never finished (cpu%d not answering)\n", a, b, b);
					bad++;
					msleep(100);
				} else if (!READ_ONCE(k61_coh_seen)) {
					pr_emerg("k61coh: %d->%d write NOT seen in 20 ms\n", a, b);
					bad++;
				}
			}
		}
		if (bad || !(round % 10))
			pr_emerg("k61coh: round %u pairs=%d bad=%d skipped=%d\n", round, n, bad, skipped);
		round++;
		msleep(300);
	}
	return 0;
}

/*
 * v2: a SCHED_FIFO writer kthread pinned to cpu a increments a counter in a
 * tight loop (no dependence on IPIs to the writer). Every other cpu b reads
 * it for up to 5 ms from an IPI handler; a counter that never changes is a
 * stale cache line. On a stale read the reader does DC CIVAC and re-reads.
 */
#include <linux/sched/rt.h>
static u64 k61_cnt[NR_CPUS * 16];

struct k61_rd {
	u64 *p;
	u64 v0, v1, v2;
	int done;
};
static struct k61_rd k61_rdv;
static call_single_data_t k61_rd_csd;

static void k61_rd_fn(void *info)
{
	struct k61_rd *r = info;
	u64 t0 = sched_clock();
	u64 v0 = READ_ONCE(*r->p), v1 = v0;

	while (sched_clock() - t0 < 5000000ULL) {
		v1 = READ_ONCE(*r->p);
		if (v1 != v0)
			break;
		cpu_relax();
	}
	r->v0 = v0;
	r->v1 = v1;
	r->v2 = v1;
	if (v1 == v0) {
		asm volatile("dc civac, %0\n\tdsb ish" :: "r"(r->p) : "memory");
		r->v2 = READ_ONCE(*r->p);
	}
	smp_wmb();
	WRITE_ONCE(r->done, 1);
}

static int k61_wr_fn(void *arg)
{
	u64 *p = &k61_cnt[(long)arg * 16];
	int i;

	while (!kthread_should_stop()) {
		for (i = 0; i < 4096; i++)
			WRITE_ONCE(*p, READ_ONCE(*p) + 1);
		cond_resched();
	}
	return 0;
}

static void k61_coh2_round(unsigned int round)
{
	int a, b, bad = 0, n = 0;

	for_each_online_cpu(a) {
		struct task_struct *w;

		w = kthread_create(k61_wr_fn, (void *)(long)a, "k61wr/%d", a);
		if (IS_ERR(w))
			continue;
		kthread_bind(w, a);
		sched_set_fifo(w);
		wake_up_process(w);
		usleep_range(2000, 3000);
		for_each_online_cpu(b) {
			int waited;

			if (a == b)
				continue;
			k61_rdv.p = &k61_cnt[a * 16];
			WRITE_ONCE(k61_rdv.done, 0);
			smp_wmb();
			INIT_CSD(&k61_rd_csd, k61_rd_fn, &k61_rdv);
			if (smp_call_function_single_async(b, &k61_rd_csd))
				continue;
			for (waited = 0; waited < 40 && !READ_ONCE(k61_rdv.done); waited++)
				usleep_range(1000, 1500);
			n++;
			if (!READ_ONCE(k61_rdv.done)) {
				pr_emerg("k61coh2: wr%d rd%d reader never finished\n", a, b);
				k61_scm_report();
				bad++;
			} else if (k61_rdv.v0 == k61_rdv.v1) {
				pr_emerg("k61coh2: wr%d rd%d STALE v=%llu, after dc civac %llu (%s)\n",
					 a, b, k61_rdv.v0, k61_rdv.v2,
					 k61_rdv.v2 != k61_rdv.v0 ? "local stale copy" : "memory itself stale/writer not running");
				bad++;
			}
		}
		kthread_stop(w);
	}
	if (bad || !(round % 10))
		pr_emerg("k61coh2: round %u pairs=%d bad=%d\n", round, n, bad);
}

static int k61_coh2_thread(void *arg)
{
	unsigned int round = 0;

	msleep(1600);
	for (;;) {
		k61_coh2_round(round++);
		msleep(300);
	}
	return 0;
}

static int __init k61_coh_init(void)
{
	/* v1 disabled: its irq-off polling starves the v2 writers */
	kthread_run(k61_coh2_thread, NULL, "k61coh2");
	return 0;
}
late_initcall(k61_coh_init);

/* DEBUG: retry PSCI PC-mode switch with only CPU0 online */
#include <linux/psci.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
static int k61_psci_thread(void *u)
{
	int cpu, ret;

	msleep(3000);
	for_each_present_cpu(cpu)
		if (cpu) {
			ret = remove_cpu(cpu);
			pr_info("k61psci: offline cpu%d = %d\n", cpu, ret);
		}
	pr_info("k61psci: online=%*pbl\n", cpumask_pr_args(cpu_online_mask));
	ret = psci_set_osi_mode(false);
	pr_info("k61psci: set PC mode with 1 cpu = %d\n", ret);
	ret = psci_set_osi_mode(true);
	pr_info("k61psci: set OSI mode with 1 cpu = %d\n", ret);
	ret = psci_set_osi_mode(false);
	pr_info("k61psci: set PC mode again = %d\n", ret);
	for_each_present_cpu(cpu)
		if (cpu)
			add_cpu(cpu);
	pr_info("k61psci: online=%*pbl\n", cpumask_pr_args(cpu_online_mask));
	return 0;
}
static int __init __maybe_unused k61_psci_init(void)
{
	kthread_run(k61_psci_thread, NULL, "k61psci");
	return 0;
}
/* late_initcall(k61_psci_init); */

/* DEBUG k61gic: per-cpu tick snapshot of GIC CPU interface + interrupted context */
#include <linux/hrtimer.h>
#include <asm/sysreg.h>
extern u32 k61_gic_waker(int cpu);
extern u64 k61_gic_typer(int cpu);
extern void k61_gic_rd_read(int cpu, u32 *ctlr, u32 *en, u32 *pend, u32 *act, u32 *prio0);
void k61_pcsr_report(const char *why);
static atomic_t k61gic_stale_dumps;
static u64 k61gic_last_stale;
struct k61gic_snap {
	u64 t, pc, pstate, pmr_save, pmr_now, rpr, hppir, igrpen, daif;
	u64 sgi_t, sgi_pc, sgi_pstate, sgi_pmr_save, sgi_hppir;
	u32 pend_local;
};
static DEFINE_PER_CPU(struct k61gic_snap, k61gic);
static DEFINE_PER_CPU(struct hrtimer, k61gic_timer);

static enum hrtimer_restart k61gic_fn(struct hrtimer *h)
{
	struct k61gic_snap *s = this_cpu_ptr(&k61gic);
	struct pt_regs *r = get_irq_regs();
	u32 c, en, pend, act, pr;

	s->t = sched_clock();
	s->pc = r ? r->pc : 0;
	s->pstate = r ? r->pstate : 0;
	s->pmr_save = r ? r->pmr_save : 0;
	s->pmr_now = read_sysreg_s(SYS_ICC_PMR_EL1);
	s->rpr = read_sysreg_s(SYS_ICC_RPR_EL1);
	s->hppir = read_sysreg_s(SYS_ICC_HPPIR1_EL1);
	s->igrpen = read_sysreg_s(SYS_ICC_IGRPEN1_EL1);
	asm volatile("mrs %0, daif" : "=r"(s->daif));
	k61_gic_rd_read(smp_processor_id(), &c, &en, &pend, &act, &pr);
	s->pend_local = pend;
	if (pend & 0xff) {
		s->sgi_t = s->t; s->sgi_pc = s->pc; s->sgi_pstate = s->pstate;
		s->sgi_pmr_save = s->pmr_save; s->sgi_hppir = s->hppir;
	}
	if (s->t > 10ULL * NSEC_PER_SEC && s->t - k61gic_last_stale > 500000000ULL &&
	    atomic_read(&k61gic_stale_dumps) < 12) {
		int o;

		for_each_online_cpu(o) {
			struct k61gic_snap *os = per_cpu_ptr(&k61gic, o);

			if (os->t && (s64)(s->t - os->t) > 1000000000LL) {
				k61gic_last_stale = s->t;
				atomic_inc(&k61gic_stale_dumps);
				pr_emerg("k61pcsr: cpu%d tick stale %llu ms, seen from cpu%d\n", o,
					 (s->t - os->t) / 1000000, smp_processor_id());
				k61_pcsr_report("stale");
				k61_gic_report();
				k61_bark_flush();
				break;
			}
		}
	}
	hrtimer_forward_now(h, ms_to_ktime(50));
	return HRTIMER_RESTART;
}

static void k61gic_start(void *u)
{
	struct hrtimer *h = this_cpu_ptr(&k61gic_timer);

	hrtimer_init(h, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	h->function = k61gic_fn;
	hrtimer_start(h, ms_to_ktime(50), HRTIMER_MODE_REL_PINNED_HARD);
}

void k61_gic_report(void)
{
	u64 now = sched_clock();
	int c;

	for_each_online_cpu(c) {
		struct k61gic_snap *s = per_cpu_ptr(&k61gic, c);
		u32 ctl, en, pend, act, pr;

		k61_gic_rd_read(c, &ctl, &en, &pend, &act, &pr);
		pr_emerg("k61gic: cpu%d WAKER=%08x TYPER=%016llx\n", c, k61_gic_waker(c), k61_gic_typer(c));
		pr_emerg("k61gic: cpu%d RD ctlr=%08x isenable0=%08x ispend0=%08x isactive0=%08x prio0=%08x\n",
			 c, ctl, en, pend, act, pr);
		pr_emerg("k61gic: cpu%d tick age=%lluus pc=%016llx pstate=%016llx pmr_save=%llx | now pmr=%llx rpr=%llx hppir=%llx igrpen1=%llx daif=%llx\n",
			 c, (now - s->t) / 1000, s->pc, s->pstate, s->pmr_save,
			 s->pmr_now, s->rpr, s->hppir, s->igrpen, s->daif);
		pr_emerg("k61gic: cpu%d last tick with SGI pending: age=%lluus pc=%016llx pstate=%016llx pmr_save=%llx hppir=%llx\n",
			 c, s->sgi_t ? (now - s->sgi_t) / 1000 : 0, s->sgi_pc, s->sgi_pstate,
			 s->sgi_pmr_save, s->sgi_hppir);
	}
}

static int __init k61gic_init(void)
{
	on_each_cpu(k61gic_start, NULL, 1);
	return 0;
}
late_initcall(k61gic_init);

/* DEBUG k61gic: periodic dump from cpu0 (bark may never fire) */
static struct hrtimer k61gic_dump_timer;
static int k61gic_dumps;
static enum hrtimer_restart k61gic_dump_fn(struct hrtimer *h)
{
	u64 t = sched_clock();

	if (t > 14ULL * NSEC_PER_SEC && k61gic_dumps < 14) {
		k61gic_dumps++;
		pr_emerg("k61gic: periodic dump #%d t=%llu ms\n", k61gic_dumps, t / 1000000);
		k61_gic_report();
		k61_bark_flush();
	}
	hrtimer_forward_now(h, ms_to_ktime(2000));
	return k61gic_dumps < 14 ? HRTIMER_RESTART : HRTIMER_NORESTART;
}
static void k61gic_dump_start(void *u)
{
	hrtimer_init(&k61gic_dump_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	k61gic_dump_timer.function = k61gic_dump_fn;
	hrtimer_start(&k61gic_dump_timer, ms_to_ktime(2000), HRTIMER_MODE_REL_PINNED_HARD);
}
static int __init k61gic_dump_init(void)
{
	smp_call_function_single(0, k61gic_dump_start, NULL, 1);
	return 0;
}
late_initcall_sync(k61gic_dump_init);

/* DEBUG k61dbg: probe cpu debug frame candidates (EDPCSR). A hang here = culprit is the last "probing" line. */
#include <linux/io.h>
extern int k61_aoss_send(const char *msg);
static unsigned long k61dbg_addr;
static int __init k61dbg_setup(char *str)
{
	return kstrtoul(str, 0, &k61dbg_addr) == 0;
}
__setup("k61dbg=", k61dbg_setup);

static u32 k61dbg_id(void __iomem *b)
{
	return (readl_relaxed(b + 0xfe0) & 0xff) | ((readl_relaxed(b + 0xfe4) & 0xff) << 8);
}

static int __init k61dbg_probe(void)
{
	void __iomem *b;

	if (!k61dbg_addr)
		return 0;
	pr_emerg("k61dbg: qdss vote = %d\n", k61_aoss_send("{class: clock, res: qdss, val: 1}"));
	k61_bark_flush();
	b = ioremap(k61dbg_addr, 0x1000);
	pr_emerg("k61dbg: probing %#lx\n", k61dbg_addr);
	k61_bark_flush();
	if (!b)
		return 0;
	pr_emerg("k61dbg: %#lx PIDR01=%04x\n", k61dbg_addr, k61dbg_id(b));
	k61_bark_flush();
	pr_emerg("k61dbg: %#lx PIDR2=%08x DEVTYPE=%08x\n", k61dbg_addr,
		 readl_relaxed(b + 0xfe8), readl_relaxed(b + 0xfcc));
	k61_bark_flush();
	pr_emerg("k61dbg: %#lx OSLSR=%08x EDPRSR=%08x\n", k61dbg_addr,
		 readl_relaxed(b + 0x34), readl_relaxed(b + 0x314));
	k61_bark_flush();
	iounmap(b);
	return 0;
}
late_initcall_sync(k61dbg_probe);

/* DEBUG k61pcsr: external-debug PC sampling (EDPCSR) of all cpus; works for cpus stuck with irqs off / bus hang */
static void __iomem *k61pcsr_base[8];

static void k61pcsr_read(int c, u32 *prsr, u64 *pc, u32 *cid, u32 *vid)
{
	void __iomem *b = k61pcsr_base[c];
	u32 lo, hi;

	*prsr = readl_relaxed(b + 0x314);
	lo = readl_relaxed(b + 0xa0);
	hi = readl_relaxed(b + 0xac);
	*cid = readl_relaxed(b + 0xa4);
	*vid = readl_relaxed(b + 0xa8);
	*pc = ((u64)hi << 32) | lo;
}

void k61_pcsr_report(const char *why)
{
	int c, n;

	for (c = 0; c < 8; c++) {
		/* an offline CPU is powered down: its debug registers abort */
		if (!k61pcsr_base[c] || !cpu_online(c))
			continue;
		for (n = 0; n < 3; n++) {
			u32 prsr, cid, vid;
			u64 pc;

			k61pcsr_read(c, &prsr, &pc, &cid, &vid);
			pr_emerg("k61pcsr: %s cpu%d[%d] EDPRSR=%08x PC=%016llx CID=%08x VID=%08x\n",
				 why, c, n, prsr, pc, cid, vid);
		}
	}
}

static int __init k61pcsr_init(void)
{
	int c;

	pr_emerg("k61pcsr: qdss vote = %d\n", k61_aoss_send("{class: clock, res: qdss, val: 1}"));
	for (c = 0; c < 8; c++) {
		unsigned long pa = 0x7010000UL + 0x100000UL * c;

		pr_emerg("k61pcsr: map cpu%d %#lx\n", c, pa);
		k61_bark_flush();
		k61pcsr_base[c] = ioremap(pa, 0x1000);
		if (k61pcsr_base[c]) {
			pr_emerg("k61pcsr: cpu%d DEVTYPE=%08x\n", c,
				 readl_relaxed(k61pcsr_base[c] + 0xfcc));
			k61_bark_flush();
		}
	}
	for (c = 0; c < 8; c++) {
		void __iomem *b = k61pcsr_base[c];

		pr_emerg("k61pcsr: cpu%d EDAUTHSTATUS=%08x EDDEVID=%08x EDDEVID1=%08x EDDEVID2=%08x EDSCR=%08x EDECR=%08x EDPFR=%08x/%08x\n",
			 c, readl_relaxed(b + 0xfb8), readl_relaxed(b + 0xfc8), readl_relaxed(b + 0xfc4),
			 readl_relaxed(b + 0xfc0), readl_relaxed(b + 0x88), readl_relaxed(b + 0x24),
			 readl_relaxed(b + 0xd20), readl_relaxed(b + 0xd24));
		k61_bark_flush();
	}
	k61_pcsr_report("boot");
	k61_bark_flush();
	return 0;
}
late_initcall_sync(k61pcsr_init);
