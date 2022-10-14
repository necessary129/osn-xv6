import matplotlib.pyplot as plt

import numpy as np

from collections import defaultdict

d = defaultdict(lambda: {'ticks':[], 'queue':[]})

with open('clockdump.csv') as f:
    for line in f.readlines():
        tick, pid, queue = line.split(',')
        print(tick, pid, queue)
        d[pid]['ticks'].append(tick)
        d[pid]['queue'].append(queue)

for pid, obj in d.items():
    col = (np.random.random(), np.random.random(), np.random.random())
    print(obj)
    plt.plot(obj['ticks'],obj['queue'], color=col)

plt.show()
