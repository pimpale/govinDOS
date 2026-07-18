#ifndef sched_h_INCLUDED
#define sched_h_INCLUDED

struct sched_param {
  int sched_priority;
};

int sched_yield(void);

#endif // sched_h_INCLUDED
