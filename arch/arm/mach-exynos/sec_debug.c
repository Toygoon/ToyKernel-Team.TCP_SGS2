/*
 *  sec_debug.c
 *
 */

#include <linux/errno.h>
#include <linux/ctype.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <mach/regs-pmu.h>
#include <asm/cacheflush.h>
#include <asm/io.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bootmem.h>
#include <linux/kmsg_dump.h>

#include <plat/system-reset.h>
#include <mach/sec_debug.h>
#include <plat/map-base.h>
#include <plat/map-s5p.h>
#include <asm/mach/map.h>
#include <plat/regs-watchdog.h>
#include <linux/mfd/max8997.h> 



#define SEC_DEBUG_MAGIC_PA S5P_PA_SDRAM
#define SEC_DEBUG_MAGIC_VA phys_to_virt(SEC_DEBUG_MAGIC_PA)

extern cable_type_t max8997_muic_get_attached_device(void);

enum sec_debug_upload_cause_t {
	UPLOAD_CAUSE_INIT = 0xCAFEBABE,
	UPLOAD_CAUSE_KERNEL_PANIC = 0x000000C8,
	UPLOAD_CAUSE_FORCED_UPLOAD = 0x00000022,
	UPLOAD_CAUSE_CP_ERROR_FATAL = 0x000000CC,
	UPLOAD_CAUSE_USER_FAULT = 0x0000002F,
	UPLOAD_CAUSE_HSIC_DISCONNECTED = 0x000000DD,
};

enum sec_debug_reset_reason_t {
	RR_S = 1,
	RR_W = 2,
	RR_D = 3,
	RR_N = 4,
        RR_P = 5
};

struct sec_debug_mmu_reg_t {
	int SCTLR;
	int TTBR0;
	int TTBR1;
	int TTBCR;
	int DACR;
	int DFSR;
	int DFAR;
	int IFSR;
	int IFAR;
	int DAFSR;
	int IAFSR;
	int PMRRR;
	int NMRRR;
	int FCSEPID;
	int CONTEXT;
	int URWTPID;
	int UROTPID;
	int POTPIDR;
};

/* ARM CORE regs mapping structure */
struct sec_debug_core_t {
	/* COMMON */
	unsigned int r0;
	unsigned int r1;
	unsigned int r2;
	unsigned int r3;
	unsigned int r4;
	unsigned int r5;
	unsigned int r6;
	unsigned int r7;
	unsigned int r8;
	unsigned int r9;
	unsigned int r10;
	unsigned int r11;
	unsigned int r12;

	/* SVC */
	unsigned int r13_svc;
	unsigned int r14_svc;
	unsigned int spsr_svc;

	/* PC & CPSR */
	unsigned int pc;
	unsigned int cpsr;

	/* USR/SYS */
	unsigned int r13_usr;
	unsigned int r14_usr;

	/* FIQ */
	unsigned int r8_fiq;
	unsigned int r9_fiq;
	unsigned int r10_fiq;
	unsigned int r11_fiq;
	unsigned int r12_fiq;
	unsigned int r13_fiq;
	unsigned int r14_fiq;
	unsigned int spsr_fiq;

	/* IRQ */
	unsigned int r13_irq;
	unsigned int r14_irq;
	unsigned int spsr_irq;

	/* MON */
	unsigned int r13_mon;
	unsigned int r14_mon;
	unsigned int spsr_mon;

	/* ABT */
	unsigned int r13_abt;
	unsigned int r14_abt;
	unsigned int spsr_abt;

	/* UNDEF */
	unsigned int r13_und;
	unsigned int r14_und;
	unsigned int spsr_und;

};

/* enable/disable sec_debug feature
 * level = 0 when enable = 0 && enable_user = 0
 * level = 1 when enable = 1 && enable_user = 0
 * level = 0x10001 when enable = 1 && enable_user = 1
 * The other cases are not considered
 */
static union {
	struct {
		u16 kernel_fault;
		u16 user_fault;
	} en;
	u32 uint_val;
} debug_level = { .en.kernel_fault = 1, };

static unsigned reset_reason = RR_N;

module_param_named(enable, debug_level.en.kernel_fault, ushort, 0644);
module_param_named(enable_user, debug_level.en.user_fault, ushort, 0644);
module_param_named(level, debug_level.uint_val, uint, 0644);
module_param_named(reset_reason, reset_reason, uint, 0644);

/* klaatu - schedule log */
#ifdef CONFIG_SEC_DEBUG_SCHED_LOG
static struct sched_log gExcpTaskLog[NR_CPUS][SCHED_LOG_MAX]
	__cacheline_aligned;
static atomic_t gExcpTaskLogIdx[NR_CPUS] = { ATOMIC_INIT(-1), ATOMIC_INIT(-1) };
static struct sched_log (*gExcpTaskLogPtr)[NR_CPUS][SCHED_LOG_MAX]
	= (&gExcpTaskLog);
#ifdef CONFIG_SEC_DEBUG_IRQ_EXIT_LOG
static unsigned long long gExcpIrqExitTime[NR_CPUS];
#endif

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
static struct auxiliary_log gExcpAuxLog	__cacheline_aligned;
static struct auxiliary_log *gExcpAuxLogPtr;
static atomic_t gExcpAuxCpuClockLogIdx = ATOMIC_INIT(-1);
#endif

static int checksum_sched_log(void)
{
	int sum = 0, i;
	for (i = 0; i < sizeof(gExcpTaskLog); i++)
		sum += *((char *)gExcpTaskLog + i);

	return sum;
}

#ifdef CONFIG_SEC_DEBUG_SCHED_LOG_NONCACHED
static void map_noncached_sched_log_buf(void)
{
	struct map_desc slog_buf_iodesc[] = {
		{
			.virtual = (unsigned long)S3C_VA_SLOG_BUF,
			.length = 0x200000,
			.type = MT_DEVICE
		}
	};

	slog_buf_iodesc[0].pfn = __phys_to_pfn
		((unsigned long)((virt_to_phys(&gExcpTaskLog)&0xfff00000)));
	iotable_init(slog_buf_iodesc, ARRAY_SIZE(slog_buf_iodesc));
	gExcpTaskLogPtr = (void *)(S3C_VA_SLOG_BUF +
		(((unsigned long)(&gExcpTaskLog))&0x000fffff));
}
#endif

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
static void map_noncached_aux_log_buf(void)
{
	struct map_desc auxlog_buf_iodesc[] = {
		{
			.virtual = (unsigned long)S3C_VA_AUXLOG_BUF,
			.length = 0x100000,
			.type = MT_DEVICE
		}
	};

	auxlog_buf_iodesc[0].pfn = __phys_to_pfn
		((unsigned long)((virt_to_phys(&gExcpAuxLog)&0xfff00000)));
	iotable_init(auxlog_buf_iodesc, ARRAY_SIZE(auxlog_buf_iodesc));
	gExcpAuxLogPtr = (void *)(S3C_VA_AUXLOG_BUF +
		(((unsigned long)(&gExcpAuxLog))&0x000fffff));
}
#endif

#else
static int checksum_sched_log(void)
{
	return 0;
}
#endif

/* klaatu - semaphore log */
#ifdef CONFIG_SEC_DEBUG_SEMAPHORE_LOG
struct sem_debug sem_debug_free_head;
struct sem_debug sem_debug_done_head;
int sem_debug_free_head_cnt;
int sem_debug_done_head_cnt;
int sem_debug_init = 0;
spinlock_t sem_debug_lock;

/* rwsemaphore logging */
struct rwsem_debug rwsem_debug_free_head;
struct rwsem_debug rwsem_debug_done_head;
int rwsem_debug_free_head_cnt;
int rwsem_debug_done_head_cnt;
int rwsem_debug_init = 0;
spinlock_t rwsem_debug_lock;

#endif /* CONFIG_SEC_DEBUG_SEMAPHORE_LOG */

DEFINE_PER_CPU(struct sec_debug_core_t, sec_debug_core_reg);
DEFINE_PER_CPU(struct sec_debug_mmu_reg_t, sec_debug_mmu_reg);
DEFINE_PER_CPU(enum sec_debug_upload_cause_t, sec_debug_upload_cause);

/* core reg dump function*/
static inline void sec_debug_save_core_reg(struct sec_debug_core_t *core_reg)
{
	/* we will be in SVC mode when we enter this function. Collect
	   SVC registers along with cmn registers. */
	asm("str r0, [%0,#0]\n\t"	/* R0 is pushed first to core_reg */
	    "mov r0, %0\n\t"		/* R0 will be alias for core_reg */
	    "str r1, [r0,#4]\n\t"	/* R1 */
	    "str r2, [r0,#8]\n\t"	/* R2 */
	    "str r3, [r0,#12]\n\t"	/* R3 */
	    "str r4, [r0,#16]\n\t"	/* R4 */
	    "str r5, [r0,#20]\n\t"	/* R5 */
	    "str r6, [r0,#24]\n\t"	/* R6 */
	    "str r7, [r0,#28]\n\t"	/* R7 */
	    "str r8, [r0,#32]\n\t"	/* R8 */
	    "str r9, [r0,#36]\n\t"	/* R9 */
	    "str r10, [r0,#40]\n\t"	/* R10 */
	    "str r11, [r0,#44]\n\t"	/* R11 */
	    "str r12, [r0,#48]\n\t"	/* R12 */
	    /* SVC */
	    "str r13, [r0,#52]\n\t"	/* R13_SVC */
	    "str r14, [r0,#56]\n\t"	/* R14_SVC */
	    "mrs r1, spsr\n\t"		/* SPSR_SVC */
	    "str r1, [r0,#60]\n\t"
	    /* PC and CPSR */
	    "sub r1, r15, #0x4\n\t"	/* PC */
	    "str r1, [r0,#64]\n\t"
	    "mrs r1, cpsr\n\t"		/* CPSR */
	    "str r1, [r0,#68]\n\t"
	    /* SYS/USR */
	    "mrs r1, cpsr\n\t"		/* switch to SYS mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x1f\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#72]\n\t"	/* R13_USR */
	    "str r14, [r0,#76]\n\t"	/* R14_USR */
	    /* FIQ */
	    "mrs r1, cpsr\n\t"		/* switch to FIQ mode */
	    "and r1,r1,#0xFFFFFFE0\n\t"
	    "orr r1,r1,#0x11\n\t"
	    "msr cpsr,r1\n\t"
	    "str r8, [r0,#80]\n\t"	/* R8_FIQ */
	    "str r9, [r0,#84]\n\t"	/* R9_FIQ */
	    "str r10, [r0,#88]\n\t"	/* R10_FIQ */
	    "str r11, [r0,#92]\n\t"	/* R11_FIQ */
	    "str r12, [r0,#96]\n\t"	/* R12_FIQ */
	    "str r13, [r0,#100]\n\t"	/* R13_FIQ */
	    "str r14, [r0,#104]\n\t"	/* R14_FIQ */
	    "mrs r1, spsr\n\t"		/* SPSR_FIQ */
	    "str r1, [r0,#108]\n\t"
	    /* IRQ */
	    "mrs r1, cpsr\n\t"		/* switch to IRQ mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x12\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#112]\n\t"	/* R13_IRQ */
	    "str r14, [r0,#116]\n\t"	/* R14_IRQ */
	    "mrs r1, spsr\n\t"		/* SPSR_IRQ */
	    "str r1, [r0,#120]\n\t"
	    /* MON */
	    "mrs r1, cpsr\n\t"		/* switch to monitor mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x16\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#124]\n\t"	/* R13_MON */
	    "str r14, [r0,#128]\n\t"	/* R14_MON */
	    "mrs r1, spsr\n\t"		/* SPSR_MON */
	    "str r1, [r0,#132]\n\t"
	    /* ABT */
	    "mrs r1, cpsr\n\t"		/* switch to Abort mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x17\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#136]\n\t"	/* R13_ABT */
	    "str r14, [r0,#140]\n\t"	/* R14_ABT */
	    "mrs r1, spsr\n\t"		/* SPSR_ABT */
	    "str r1, [r0,#144]\n\t"
	    /* UND */
	    "mrs r1, cpsr\n\t"		/* switch to undef mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x1B\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#148]\n\t"	/* R13_UND */
	    "str r14, [r0,#152]\n\t"	/* R14_UND */
	    "mrs r1, spsr\n\t"		/* SPSR_UND */
	    "str r1, [r0,#156]\n\t"
	    /* restore to SVC mode */
	    "mrs r1, cpsr\n\t"		/* switch to SVC mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x13\n\t"
	    "msr cpsr,r1\n\t" :		/* output */
	    : "r"(core_reg)		/* input */
	    : "%r0", "%r1"		/* clobbered registers */
	);

	return;
}

static inline void sec_debug_save_mmu_reg(struct sec_debug_mmu_reg_t *mmu_reg)
{
	asm("mrc    p15, 0, r1, c1, c0, 0\n\t"	/* SCTLR */
	    "str r1, [%0]\n\t"
	    "mrc    p15, 0, r1, c2, c0, 0\n\t"	/* TTBR0 */
	    "str r1, [%0,#4]\n\t"
	    "mrc    p15, 0, r1, c2, c0,1\n\t"	/* TTBR1 */
	    "str r1, [%0,#8]\n\t"
	    "mrc    p15, 0, r1, c2, c0,2\n\t"	/* TTBCR */
	    "str r1, [%0,#12]\n\t"
	    "mrc    p15, 0, r1, c3, c0,0\n\t"	/* DACR */
	    "str r1, [%0,#16]\n\t"
	    "mrc    p15, 0, r1, c5, c0,0\n\t"	/* DFSR */
	    "str r1, [%0,#20]\n\t"
	    "mrc    p15, 0, r1, c6, c0,0\n\t"	/* DFAR */
	    "str r1, [%0,#24]\n\t"
	    "mrc    p15, 0, r1, c5, c0,1\n\t"	/* IFSR */
	    "str r1, [%0,#28]\n\t"
	    "mrc    p15, 0, r1, c6, c0,2\n\t"	/* IFAR */
	    "str r1, [%0,#32]\n\t"
	    /* Don't populate DAFSR and RAFSR */
	    "mrc    p15, 0, r1, c10, c2,0\n\t"	/* PMRRR */
	    "str r1, [%0,#44]\n\t"
	    "mrc    p15, 0, r1, c10, c2,1\n\t"	/* NMRRR */
	    "str r1, [%0,#48]\n\t"
	    "mrc    p15, 0, r1, c13, c0,0\n\t"	/* FCSEPID */
	    "str r1, [%0,#52]\n\t"
	    "mrc    p15, 0, r1, c13, c0,1\n\t"	/* CONTEXT */
	    "str r1, [%0,#56]\n\t"
	    "mrc    p15, 0, r1, c13, c0,2\n\t"	/* URWTPID */
	    "str r1, [%0,#60]\n\t"
	    "mrc    p15, 0, r1, c13, c0,3\n\t"	/* UROTPID */
	    "str r1, [%0,#64]\n\t"
	    "mrc    p15, 0, r1, c13, c0,4\n\t"	/* POTPIDR */
	    "str r1, [%0,#68]\n\t" :		/* output */
	    : "r"(mmu_reg)			/* input */
	    : "%r1", "memory"			/* clobbered register */
	);
}

static inline void sec_debug_save_context(void)
{
	unsigned long flags;
	local_irq_save(flags);
	sec_debug_save_mmu_reg(&per_cpu(sec_debug_mmu_reg, smp_processor_id()));
	sec_debug_save_core_reg(&per_cpu
				(sec_debug_core_reg, smp_processor_id()));

	pr_emerg("(%s) context saved(CPU:%d)\n", __func__, smp_processor_id());
	local_irq_restore(flags);
}

static void sec_debug_set_upload_magic(unsigned magic)
{
	pr_emerg("(%s) %x\n", __func__, magic);

	*(unsigned int *)SEC_DEBUG_MAGIC_VA = magic;
	*(unsigned int *)(SEC_DEBUG_MAGIC_VA + 0x4000) = magic;

	flush_cache_all();

	outer_flush_all();
}

static int sec_debug_normal_reboot_handler(struct notifier_block *nb,
					   unsigned long l, void *p)
{
	sec_debug_set_upload_magic(0x0);

	return 0;
}

static void sec_debug_set_upload_cause(enum sec_debug_upload_cause_t type)
{
	per_cpu(sec_debug_upload_cause, smp_processor_id()) = type;

	/* to check VDD_ALIVE / XnRESET issue */
	__raw_writel(type, S5P_INFORM3);
	__raw_writel(type, S5P_INFORM4);
	__raw_writel(type, S5P_INFORM6);

	pr_emerg("(%s) %x\n", __func__, type);
}

/*
 * Called from dump_stack()
 * This function call does not necessarily mean that a fatal error
 * had occurred. It may be just a warning.
 */
static inline int sec_debug_dump_stack(void)
{
	if (!debug_level.en.kernel_fault)
		return -1;

	sec_debug_save_context();

	/* flush L1 from each core.
	   L2 will be flushed later before reset. */
	flush_cache_all();

	return 0;
}

static inline void sec_debug_hw_reset(void)
{
	pr_emerg("(%s) %s\n", __func__, linux_banner);
	pr_emerg("(%s) rebooting...\n", __func__);

	flush_cache_all();

	outer_flush_all();

	arch_reset(0, 0);

	while (1) ;
}

#ifdef CONFIG_SEC_WATCHDOG_RESET
static inline void sec_debug_disable_watchdog(void)
{
	writel(0, S3C2410_WTCON);
	pr_err("(%s) disable watchdog reset while printing log\n", __func__);
}
#endif

static int sec_debug_panic_handler(struct notifier_block *nb,
				   unsigned long l, void *buf)
{
	cable_type_t  type = max8997_muic_get_attached_device();

	if ((type != CABLE_TYPE_JIG_UART_OFF ||
		type != CABLE_TYPE_JIG_UART_OFF_VB) && (strcmp(buf, "Commercial Dump"))) 
    		if (!debug_level.en.kernel_fault)
    			return -1;

	sec_debug_set_upload_magic(0x66262564);

	if ((type == CABLE_TYPE_JIG_UART_OFF ||
			type == CABLE_TYPE_JIG_UART_OFF_VB) && (!strcmp(buf, "Commercial Dump"))) 
		sec_debug_set_upload_cause(UPLOAD_CAUSE_FORCED_UPLOAD);
	else if (!strcmp(buf, "User Fault"))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_USER_FAULT);
	else if (!strcmp(buf, "Crash Key"))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_FORCED_UPLOAD);
	else if (!strcmp(buf, "CP Crash"))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_CP_ERROR_FATAL);
	else if (!strcmp(buf, "HSIC Disconnected"))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_HSIC_DISCONNECTED);
	else
		sec_debug_set_upload_cause(UPLOAD_CAUSE_KERNEL_PANIC);

	pr_err("(%s) checksum_sched_log: %x\n", __func__, checksum_sched_log());

#ifdef CONFIG_SEC_WATCHDOG_RESET
	sec_debug_disable_watchdog();
#endif
	show_state();

	sec_debug_dump_stack();
	sec_debug_hw_reset();

	return 0;
}

#define LOCKUP_FIRST_KEY KEY_VOLUMEUP
#define LOCKUP_SECOND_KEY KEY_POWER
#define LOCKUP_THIRD_KEY KEY_POWER
#define LOCKUP_EXTRA_KEY KEY_VOLUMEDOWN

void sec_debug_check_crash_key(unsigned int code, int value)
{
    	static enum { NONE, STEP1, STEP2, STEP3, STEP4, STEP5, STEP6, STEP7, STEP8, STEP9, STEP10} state = NONE;
	static bool volup_p;
	static bool voldown_p;
	static int loopcount;

	if (!debug_level.en.kernel_fault) {
        switch (state) 
		{
		case NONE:
			state = (code == LOCKUP_FIRST_KEY && value) ? STEP1 : NONE;
			break;
		case STEP1:
			state = (code == LOCKUP_EXTRA_KEY && value) ? STEP2 : NONE;
			break;
		case STEP2:
			state = (code == LOCKUP_FIRST_KEY && !value) ? STEP3 : NONE;
			break;
		case STEP3:
			state = (code == LOCKUP_FIRST_KEY && value) ? STEP4 : NONE;
			break;
		case STEP4:
			state = (code == LOCKUP_FIRST_KEY && !value) ? STEP5 : NONE;
			break;
		case STEP5:
			state = (code == LOCKUP_FIRST_KEY && value) ? STEP6 : NONE;
			break;
		case STEP6:
			state = (code == LOCKUP_EXTRA_KEY && !value) ? STEP7 : NONE;
			break;
		case STEP7:
			state = (code == LOCKUP_EXTRA_KEY && value) ? STEP8 : NONE;
			break;
		case STEP8:
			state = (code == LOCKUP_EXTRA_KEY && !value) ? STEP9 : NONE;
			break;
		case STEP9:
			state = (code == LOCKUP_EXTRA_KEY && value) ? STEP10 : NONE;
			break;
		case STEP10:
			if (code == LOCKUP_THIRD_KEY && value) 
			{
				
				cable_type_t  type = max8997_muic_get_attached_device();
				
				if (type == CABLE_TYPE_JIG_UART_OFF ||
					type == CABLE_TYPE_JIG_UART_OFF_VB) {
					panic("Commercial Dump");
				}
			}					
			else
				state = NONE;

			break;
		default:
			break;
		}
		return;
    }

	/* Must be deleted later */
#if defined(CONFIG_MACH_MIDAS)
	pr_info("%s:key code(%d) value(%d)\n",
		__func__, code, value);
#endif

	/* Enter Force Upload
	 *  Hold volume down key first
	 *  and then press power key twice
	 *  and volume up key should not be pressed
	 */
	if (value) {
		if (code == KEY_VOLUMEUP)
		{
			pr_info("Debug Checkup high1\n");
			volup_p = true;
		}
		if (code == KEY_VOLUMEDOWN)
		{
			pr_info("Debug Checkup low1\n");
			voldown_p = true;
		}
		if (!volup_p && voldown_p) {
			if (code == KEY_POWER) {
				pr_info
				    ("%s: count for enter forced upload : %d\n",
				     __func__, ++loopcount);
				if (loopcount == 2)
					panic("Crash Key");
			}
		}
	} 
	else 
	{
		if (code == KEY_VOLUMEUP)
		{
			pr_info("Debug Checkup high0\n");
			volup_p = false;
		}
		if (code == KEY_VOLUMEDOWN) 
		{	
			pr_info("Debug Checkup low0\n");
			loopcount = 0;
			voldown_p = false;
		}
	}
}

static struct notifier_block nb_reboot_block = {
	.notifier_call = sec_debug_normal_reboot_handler
};

static struct notifier_block nb_panic_block = {
	.notifier_call = sec_debug_panic_handler,
};

static void sec_kmsg_dump(struct kmsg_dumper *dumper,
			  enum kmsg_dump_reason reason, const char *s1,
			  unsigned long l1, const char *s2, unsigned long l2)
{
	char *ptr = (char *)SEC_DEBUG_MAGIC_VA + 64;
	int total_chars = SZ_4K - 64;
	int total_lines = 50;
	int last_chars; /* no of chars which fits in total_chars *and* in total_lines */

	for (last_chars = 0;
	     l2 && l2 > last_chars && total_lines > 0
	     && total_chars > 0; ++last_chars, --total_chars) {
		if (s2[l2 - last_chars] == '\n')
			--total_lines;
	}
	s2 += (l2 - last_chars);
	l2 = last_chars;

	for (last_chars = 0;
	     l1 && l1 > last_chars && total_lines > 0
	     && total_chars > 0; ++last_chars, --total_chars) {
		if (s1[l1 - last_chars] == '\n')
			--total_lines;
	}
	s1 += (l1 - last_chars);
	l1 = last_chars;

	while (l1-- > 0)
		*ptr++ = *s1++;
	while (l2-- > 0)
		*ptr++ = *s2++;
}

static struct kmsg_dumper sec_dumper = {
	.dump = sec_kmsg_dump,
};

__init int sec_debug_init(void)
{
	if (!debug_level.en.kernel_fault)
		return -1;

	sec_debug_set_upload_magic(0x66262564);
	sec_debug_set_upload_cause(UPLOAD_CAUSE_INIT);

#ifdef CONFIG_SEC_DEBUG_SCHED_LOG_NONCACHED
	if (debug_level.en.kernel_fault)
		map_noncached_sched_log_buf();
#endif

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
	map_noncached_aux_log_buf();
#endif

	kmsg_dump_register(&sec_dumper);

	register_reboot_notifier(&nb_reboot_block);

	atomic_notifier_chain_register(&panic_notifier_list, &nb_panic_block);

	return 0;
}

int sec_debug_level(void)
{
	return debug_level.uint_val;
}

/* klaatu - schedule log */
#ifdef CONFIG_SEC_DEBUG_SCHED_LOG
void sec_debug_task_sched_log(int cpu, struct task_struct *task)
{
	unsigned i = 0;

	if (!debug_level.en.kernel_fault)
		return;

	i = atomic_inc_return(&gExcpTaskLogIdx[cpu]) & (SCHED_LOG_MAX - 1);
	(*gExcpTaskLogPtr)[cpu][i].time = cpu_clock(cpu);
	strcpy((*gExcpTaskLogPtr)[cpu][i].log.task.comm, task->comm);
	(*gExcpTaskLogPtr)[cpu][i].log.task.pid = task->pid;
	(*gExcpTaskLogPtr)[cpu][i].log.task.cpu = cpu;
}

void sec_debug_irq_sched_log(unsigned int irq, void *fn, int en)
{
	int cpu = 0; 
	unsigned i = 0;

	if (!debug_level.en.kernel_fault)
		return;

	cpu = smp_processor_id();
	i = atomic_inc_return(&gExcpTaskLogIdx[cpu]) & (SCHED_LOG_MAX - 1);
	
	(*gExcpTaskLogPtr)[cpu][i].time = cpu_clock(cpu);
	(*gExcpTaskLogPtr)[cpu][i].log.irq.cpu = cpu;
	(*gExcpTaskLogPtr)[cpu][i].log.irq.irq = irq;
	(*gExcpTaskLogPtr)[cpu][i].log.irq.fn = (void *)fn;
	(*gExcpTaskLogPtr)[cpu][i].log.irq.en = en;
}

#ifdef CONFIG_SEC_DEBUG_IRQ_EXIT_LOG
void sec_debug_irq_last_exit_log(void)
{
	int cpu = 0; 

	if (!debug_level.en.kernel_fault)
		return;	
	
	cpu = smp_processor_id();
	gExcpIrqExitTime[cpu] = cpu_clock(cpu);
}
#endif
#endif /* CONFIG_SEC_DEBUG_SCHED_LOG */

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
void sec_debug_aux_log(int idx, char *fmt, ...)
{
	va_list args;
	char buf[128];
	unsigned i;
	int cpu = raw_smp_processor_id();

	if (!gExcpAuxLogPtr)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch (idx) {
	case SEC_DEBUG_AUXLOG_CPU_BUS_CLOCK_CHANGE:
		i = atomic_inc_return(&gExcpAuxCpuClockLogIdx)
			& (AUX_LOG_CPU_CLOCK_MAX - 1);
		(*gExcpAuxLogPtr).CpuClockLog[i].time = cpu_clock(cpu);
		(*gExcpAuxLogPtr).CpuClockLog[i].cpu = cpu;
		strncpy((*gExcpAuxLogPtr).CpuClockLog[i].log,
			buf, AUX_LOG_LENGTH);
		break;

	default:
		break;
	}
}
#endif

/* klaatu - semaphore log */
#ifdef CONFIG_SEC_DEBUG_SEMAPHORE_LOG
void debug_semaphore_init(void)
{
	int i = 0;
	struct sem_debug *sem_debug = NULL;

	spin_lock_init(&sem_debug_lock);
	sem_debug_free_head_cnt = 0;
	sem_debug_done_head_cnt = 0;

	/* initialize list head of sem_debug */
	INIT_LIST_HEAD(&sem_debug_free_head.list);
	INIT_LIST_HEAD(&sem_debug_done_head.list);

	for (i = 0; i < SEMAPHORE_LOG_MAX; i++) {
		/* malloc semaphore */
		sem_debug = kmalloc(sizeof(struct sem_debug), GFP_KERNEL);
		/* add list */
		list_add(&sem_debug->list, &sem_debug_free_head.list);
		sem_debug_free_head_cnt++;
	}

	sem_debug_init = 1;
}

void debug_semaphore_down_log(struct semaphore *sem)
{
	struct list_head *tmp;
	struct sem_debug *sem_dbg;
	unsigned long flags;

	if (!sem_debug_init)
		return;

	spin_lock_irqsave(&sem_debug_lock, flags);
	list_for_each(tmp, &sem_debug_free_head.list) {
		sem_dbg = list_entry(tmp, struct sem_debug, list);
		sem_dbg->task = current;
		sem_dbg->sem = sem;
		/* strcpy(sem_dbg->comm,current->group_leader->comm); */
		sem_dbg->pid = current->pid;
		sem_dbg->cpu = smp_processor_id();
		list_del(&sem_dbg->list);
		list_add(&sem_dbg->list, &sem_debug_done_head.list);
		sem_debug_free_head_cnt--;
		sem_debug_done_head_cnt++;
		break;
	}
	spin_unlock_irqrestore(&sem_debug_lock, flags);
}

void debug_semaphore_up_log(struct semaphore *sem)
{
	struct list_head *tmp;
	struct sem_debug *sem_dbg;
	unsigned long flags;

	if (!sem_debug_init)
		return;

	spin_lock_irqsave(&sem_debug_lock, flags);
	list_for_each(tmp, &sem_debug_done_head.list) {
		sem_dbg = list_entry(tmp, struct sem_debug, list);
		if (sem_dbg->sem == sem && sem_dbg->pid == current->pid) {
			list_del(&sem_dbg->list);
			list_add(&sem_dbg->list, &sem_debug_free_head.list);
			sem_debug_free_head_cnt++;
			sem_debug_done_head_cnt--;
			break;
		}
	}
	spin_unlock_irqrestore(&sem_debug_lock, flags);
}

/* rwsemaphore logging */
void debug_rwsemaphore_init(void)
{
	int i = 0;
	struct rwsem_debug *rwsem_debug = NULL;

	spin_lock_init(&rwsem_debug_lock);
	rwsem_debug_free_head_cnt = 0;
	rwsem_debug_done_head_cnt = 0;

	/* initialize list head of sem_debug */
	INIT_LIST_HEAD(&rwsem_debug_free_head.list);
	INIT_LIST_HEAD(&rwsem_debug_done_head.list);

	for (i = 0; i < RWSEMAPHORE_LOG_MAX; i++) {
		/* malloc semaphore */
		rwsem_debug = kmalloc(sizeof(struct rwsem_debug), GFP_KERNEL);
		/* add list */
		list_add(&rwsem_debug->list, &rwsem_debug_free_head.list);
		rwsem_debug_free_head_cnt++;
	}

	rwsem_debug_init = 1;
}

void debug_rwsemaphore_down_log(struct rw_semaphore *sem, int dir)
{
	struct list_head *tmp;
	struct rwsem_debug *sem_dbg;
	unsigned long flags;

	if (!rwsem_debug_init)
		return;

	spin_lock_irqsave(&rwsem_debug_lock, flags);
	list_for_each(tmp, &rwsem_debug_free_head.list) {
		sem_dbg = list_entry(tmp, struct rwsem_debug, list);
		sem_dbg->task = current;
		sem_dbg->sem = sem;
		/* strcpy(sem_dbg->comm,current->group_leader->comm); */
		sem_dbg->pid = current->pid;
		sem_dbg->cpu = smp_processor_id();
		sem_dbg->direction = dir;
		list_del(&sem_dbg->list);
		list_add(&sem_dbg->list, &rwsem_debug_done_head.list);
		rwsem_debug_free_head_cnt--;
		rwsem_debug_done_head_cnt++;
		break;
	}
	spin_unlock_irqrestore(&rwsem_debug_lock, flags);
}

void debug_rwsemaphore_up_log(struct rw_semaphore *sem)
{
	struct list_head *tmp;
	struct rwsem_debug *sem_dbg;
	unsigned long flags;

	if (!rwsem_debug_init)
		return;

	spin_lock_irqsave(&rwsem_debug_lock, flags);
	list_for_each(tmp, &rwsem_debug_done_head.list) {
		sem_dbg = list_entry(tmp, struct rwsem_debug, list);
		if (sem_dbg->sem == sem && sem_dbg->pid == current->pid) {
			list_del(&sem_dbg->list);
			list_add(&sem_dbg->list, &rwsem_debug_free_head.list);
			rwsem_debug_free_head_cnt++;
			rwsem_debug_done_head_cnt--;
			break;
		}
	}
	spin_unlock_irqrestore(&rwsem_debug_lock, flags);
}
#endif /* CONFIG_SEC_DEBUG_SEMAPHORE_LOG */

#ifdef CONFIG_SEC_DEBUG_USER
void sec_user_fault_dump(void)
{
	if (debug_level.en.kernel_fault == 1 && debug_level.en.user_fault == 1)
		panic("User Fault");
}

static int sec_user_fault_write(struct file *file, const char __user * buffer,
				size_t count, loff_t * offs)
{
	char buf[100];

	if (count > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (strncmp(buf, "dump_user_fault", 15) == 0)
		sec_user_fault_dump();

	return count;
}

static const struct file_operations sec_user_fault_proc_fops = {
	.write = sec_user_fault_write,
};

static int __init sec_debug_user_fault_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("user_fault", S_IWUGO, NULL,
			    &sec_user_fault_proc_fops);
	if (!entry)
		return -ENOMEM;
	return 0;
}

device_initcall(sec_debug_user_fault_init);
#endif

static int set_reset_reason_proc_show(struct seq_file *m, void *v)
{
	printk("%s : %d", __func__, reset_reason);
	if (reset_reason == RR_S)
		seq_printf(m, "SPON\n");
	else if(reset_reason == RR_W)
		seq_printf(m, "WPON\n");
	else if(reset_reason == RR_D)
		seq_printf(m, "DPON\n");
        else if(reset_reason == RR_P)
		seq_printf(m, "PPON\n");
	else
		seq_printf(m, "NPON\n");

	return 0;
}

static int sec_reset_reason_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, set_reset_reason_proc_show, NULL);
}

static const struct file_operations sec_reset_reason_proc_fops = {
	.open		= sec_reset_reason_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init sec_debug_reset_reason_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("reset_reason", S_IWUGO, NULL,
			    &sec_reset_reason_proc_fops);
	if (!entry)
		return -ENOMEM;

	return 0;
}

device_initcall(sec_debug_reset_reason_init);

int sec_debug_magic_init(void)
{
	if (reserve_bootmem(SEC_DEBUG_MAGIC_PA, SZ_4K, BOOTMEM_EXCLUSIVE)) {
		pr_err("%s: failed reserving magic code area\n", __func__);
		return -ENOMEM;
	}

	pr_info("%s: success reserving magic code area\n", __func__);
	return 0;
}
