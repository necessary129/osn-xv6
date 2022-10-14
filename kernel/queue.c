#if defined(MLFQ)

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "proc.h"
#include "queue.h"

struct proc *procmlfq[NQUEUE][NPROC];
struct queue queue;

void init_queue()
{
  for (int i = 0; i < NQUEUE; i++)
  {
    for (int j = 0; j < NPROC; j++)
    {
      procmlfq[i][j] = 0;
    }
    queue.size[i] = 0;
  }
}

void push_back(struct proc *p, int qnum)
{
  if (queue.size[qnum] >= NPROC)
    panic("Proc in queue > NPROC");
  procmlfq[qnum][queue.size[qnum]] = p;
  queue.size[qnum]++;

  p->inqueue = 1;
  p->qlevel = qnum;
  p->qticks = (1 << qnum);
  p->qwaittime = 0;
  p->qentered = ticks;
}

void remove_queue(struct proc *p, int qnum)
{
  int found = -1;

  for (int i = 0; i < NPROC; i++)
  {
    if (procmlfq[qnum][i] == p)
    {
      found = i;
      break;
    }
  }
  if (found == -1)
    return;
  p->inqueue = 0;
  p->qentered = 0;
  p->qwaittime = -1;
  queue.size[qnum]--;
  for (int i = found + 1; i < NPROC; i++)
  {
    procmlfq[qnum][i - 1] = procmlfq[qnum][i];
    if (!procmlfq[qnum][i])
      break;
  }
}

#endif
