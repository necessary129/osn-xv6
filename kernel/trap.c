#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "queue.h"

#if defined(CSVCLOCKDUMP)
extern struct proc proc[NPROC];
#endif

struct spinlock tickslock;
uint ticks;

#if defined(MLFQ)
extern struct proc *procmlfq[NQUEUE][NPROC];
extern struct queue queue;
#endif

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//

static inline void checkalarm(struct proc * p){
  if (!p->inalarm && p->alarm_interval && ++(p->alarm_ticks) == p->alarm_interval){
    p->inalarm = 1;
	p->alarm_ticks = 0;
	p->savedtrapframe = *(p->trapframe);
	p->trapframe->epc = p->alarm_handler;
  }
}

int cowfault(pagetable_t pagetable, uint64 va){
	if (va >= MAXVA)
		return -1;
	pte_t * pte = walk(pagetable, va, 0);
	if (pte == 0)
		return -1;
	if (!(*pte & PTE_C))
		return -1;
	uint64 oldpa = PTE2PA(*pte);

  if (safe_getref((void *)oldpa) == 1){
    *pte = (*pte | PTE_W);
    *pte = (*pte & ~PTE_C);
    return 0;
  }

	uint64 newpa = (uint64) kalloc();

	if (newpa == 0){
		printf("cowfault: Cannot allocate new page\n");
		return -1;
	}

	memmove((void *) newpa, (void *) oldpa, PGSIZE);
	kfree((void *) oldpa);

	uint64 flags = PTE_FLAGS(*pte);
	flags &= ~PTE_C;
	flags |= PTE_W;

	*pte = PA2PTE(newpa) | flags;
	return 0;
}

#if defined(MLFQ)
void mlfq_intr(){
      struct proc *p = myproc();
    if (p && p->state == RUNNING)
    {
      p->qticks--;
      if (p->qticks <= 0)
      {
        remove_queue(p, p->qlevel);
        int qnum = (p->qlevel >= 4) ? 4 : p->qlevel + 1;
        push_back(p, qnum);
        p->qticks = (1 << p->qlevel);
        yield();
      }
      else
      {
        for (int i = 0; i < p->qlevel; i++)
        {
          if (queue.size[i])
          {
            p->qticks = (1 << p->qlevel);
            remove_queue(p, p->qlevel);
            push_back(p, p->qlevel);
            yield();
          }
        }
      }
    }
}
#endif
#if defined(CSVCLOCKDUMP)
void csvclockdump()
{
  struct proc *p = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    printf("%d,%d,%d\n", ticks, p->pid, p->qlevel);
  }
}
#endif

void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if (r_scause() == 15 || r_scause() == 13) {
	if (cowfault(p->pagetable, r_stval()) != 0)
		setkilled(p);
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2){
    checkalarm(p);
#if defined(CSVCLOCKDUMP) && defined(MLFQ)
    csvclockdump();
#endif
#if defined(MLFQ)
        mlfq_intr();
#endif
#if (PREEMPTIVE && ! defined(MLFQ))
	yield();
#endif
  }
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && myproc() != 0)
  {
    checkalarm(myproc()); // Do we count kernel mode also?
#if defined(MLFQ)
    mlfq_intr();
#endif
#if (PREEMPTIVE && ! defined(MLFQ))
    if (myproc()->state == RUNNING)
      yield();
#endif
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  update_time();
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

