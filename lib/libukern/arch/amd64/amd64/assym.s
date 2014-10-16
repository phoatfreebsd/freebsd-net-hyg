#define	COMMON_TSS_RSP0	0x4
#define	DMAP_MAX_ADDRESS	0xfffffc0000000000
#define	DMAP_MIN_ADDRESS	0xfffff80000000000
#define	EFAULT	0xe
#define	ENAMETOOLONG	0x3f
#define	ENOENT	0x2
#define	KCSEL	0x20
#define	KDSEL	0x28
#define	KERNBASE	0xffffffff80000000
#define	KSTACK_PAGES	0x4
#define	KUC32SEL	0x33
#define	KUCSEL	0x43
#define	KUDSEL	0x3b
#define	KUF32SEL	0x13
#define	KUG32SEL	0x1b
#define	LA_EOI	0xb0
#define	LA_ICR_HI	0x310
#define	LA_ICR_LO	0x300
#define	LA_ISR	0x100
#define	LA_SVR	0xf0
#define	LA_TPR	0x80
#define	LA_VER	0x30
#define	LDTSEL	0x58
#define	MAXCOMLEN	0x13
#define	MAXPATHLEN	0x400
#define	MCLBYTES	0x800
#define	MD_LDT_SD	0x8
#define	MD_LDT	0x0
#define	NPDEPG	0x200
#define	NPTEPG	0x200
#define	PAGE_MASK	0xfff
#define	PAGE_SHIFT	0xc
#define	PAGE_SIZE	0x1000
#define	PCB_32BIT	0x40
#define	PCB_CR0	0x58
#define	PCB_CR2	0x60
#define	PCB_CR3	0x68
#define	PCB_CR4	0x70
#define	PCB_CSTAR	0x100
#define	PCB_DBREGS	0x2
#define	PCB_DR0	0x78
#define	PCB_DR1	0x80
#define	PCB_DR2	0x88
#define	PCB_DR3	0x90
#define	PCB_DR6	0x98
#define	PCB_DR7	0xa0
#define	PCB_EFER	0xe8
#define	PCB_FLAGS	0xc8
#define	PCB_FPUSUSPEND	0x118
#define	PCB_FSBASE	0x40
#define	PCB_FULL_IRET	0x1
#define	PCB_GDT	0xa8
#define	PCB_GS32SD	0xd8
#define	PCB_GSBASE	0x48
#define	PCB_IDT	0xb2
#define	PCB_KGSBASE	0x50
#define	PCB_LDT	0xbc
#define	PCB_LSTAR	0xf8
#define	PCB_ONFAULT	0xd0
#define	PCB_R12	0x18
#define	PCB_R13	0x10
#define	PCB_R14	0x8
#define	PCB_R15	0x0
#define	PCB_RBP	0x20
#define	PCB_RBX	0x30
#define	PCB_RIP	0x38
#define	PCB_RSP	0x28
#define	PCB_SAVEFPU_SIZE	0x200
#define	PCB_SAVEFPU	0x120
#define	PCB_SFMASK	0x108
#define	PCB_SIZE	0x140
#define	PCB_STAR	0xf0
#define	PCB_TR	0xc6
#define	PCB_TSSP	0xe0
#define	PCB_USERFPU	0x140
#define	PCB_XSMASK	0x110
#define	PC_COMMONTSSP	0x218
#define	PC_CPUID	0x34
#define	PC_CURPCB	0x20
#define	PC_CURPMAP	0x208
#define	PC_CURTHREAD	0x0
#define	PC_FPCURTHREAD	0x10
#define	PC_FS32P	0x238
#define	PC_GS32P	0x240
#define	PC_IDLETHREAD	0x8
#define	PC_LDT	0x248
#define	PC_PM_SAVE_CNT	0x258
#define	PC_PRVSPACE	0x200
#define	PC_RSP0	0x220
#define	PC_SCRATCH_RSP	0x228
#define	PC_SIZEOF	0x400
#define	PC_TSSP	0x210
#define	PC_TSS	0x250
#define	PDESIZE	0x8
#define	PDPSHIFT	0x1e
#define	PDRSHIFT	0x15
#define	PMC_FN_USER_CALLCHAIN	0x9
#define	PML4SHIFT	0x27
#define	PM_ACTIVE	0x40
#define	PM_PCID	0x50
#define	PM_SAVE	0x48
#define	PTESIZE	0x8
#define	P_MD	0x3c0
#define	P_VMSPACE	0x170
#define	SEL_RPL_MASK	0x3
#define	SIGF_HANDLER	0x0
#define	SIGF_UC	0x10
#define	TDF_ASTPENDING	0x800
#define	TDF_NEEDRESCHED	0x10000
#define	TDP_CALLCHAIN	0x400000
#define	TDP_KTHREAD	0x200000
#define	TD_FLAGS	0xdc
#define	TD_FRAME	0x3d0
#define	TD_LOCK	0x0
#define	TD_PCB	0x370
#define	TD_PFLAGS	0xe4
#define	TD_PROC	0x8
#define	TD_TID	0x90
#define	TF_ADDR	0x80
#define	TF_CS	0xa0
#define	TF_DS	0x8e
#define	TF_ERR	0x90
#define	TF_ES	0x8c
#define	TF_FLAGS	0x88
#define	TF_FS	0x7c
#define	TF_GS	0x7e
#define	TF_HASSEGS	0x1
#define	TF_R10	0x48
#define	TF_R11	0x50
#define	TF_R12	0x58
#define	TF_R13	0x60
#define	TF_R14	0x68
#define	TF_R15	0x70
#define	TF_R8	0x20
#define	TF_R9	0x28
#define	TF_RAX	0x30
#define	TF_RBP	0x40
#define	TF_RBX	0x38
#define	TF_RCX	0x18
#define	TF_RDI	0x0
#define	TF_RDX	0x10
#define	TF_RFLAGS	0xa8
#define	TF_RIP	0x98
#define	TF_RSI	0x8
#define	TF_RSP	0xb0
#define	TF_SIZE	0xc0
#define	TF_SS	0xb8
#define	TF_TRAPNO	0x78
#define	TSSSEL	0x48
#define	UC_EFLAGS	0xc0
#define	USRSTACK	0x7ffffffff000
#define	VM_MAXUSER_ADDRESS	0x800000000000
#define	VM_PMAP	0x138
#define	V_INTR	0xc
#define	V_SYSCALL	0x8
#define	V_TRAP	0x4
#define	__FreeBSD_version	0x10c8fc
#define	addr_PDPmap	0xffff804020000000
#define	addr_PDmap	0xffff804000000000
#define	addr_PML4map	0xffff804020100000
#define	addr_PML4pml4e	0xffff804020100800
#define	addr_PTmap	0xffff800000000000
#define	val_KPDPI	0x1fe
#define	val_KPML4I	0x1ff
#define	val_PML4PML4I	0x100
