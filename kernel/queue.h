#if defined(MLFQ)
#include "param.h"

#define NQUEUE 5

struct queue {
  int size[NQUEUE];
};
#endif
