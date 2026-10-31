#!/usr/bin/env python3
"""Same data and series as the workbook's main chart, for checking without Excel."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from build_xlsx import CASES, ROOT, agg, load
data = agg(load())
MK = {'circle': 'o', 'square': 's', 'triangle': '^', 'diamond': 'D', 'x': 'X'}
fig, ax = plt.subplots(figsize=(12, 7), dpi=130, facecolor='#fcfcfb')
ax.set_facecolor('#fcfcfb')
ax.plot([28, 66], [28, 66], color='#b5b4ad', lw=1.2, ls='--', label='forwarded = offered (nothing lost)')
for case, label, _, _, _, mk, col in CASES:
    pts = data[case]
    ax.plot([p['offered'] for p in pts], [p['fwd'] for p in pts], color='#' + col, lw=2, marker=MK[mk], ms=8, label=label)
ax.set_xlim(28, 66); ax.set_ylim(28, 66); ax.set_xticks(range(28, 67, 4)); ax.set_yticks(range(28, 67, 4))
ax.grid(color='#e1e0d9', lw=1); ax.set_axisbelow(True)
for s in ('top', 'right'):
    ax.spines[s].set_visible(False)
ax.set_xlabel('Offered at the port (Mpps), 128 B frames'); ax.set_ylabel('Forwarded (Mpps)')
ax.set_title('Forwarded against offered rate: what each queue layout holds', loc='left')
ax.legend(frameon=False, fontsize=9, loc='upper left')
fig.tight_layout()
fig.savefig(os.path.join(ROOT, 'forwarded-vs-offered.png'), facecolor='#fcfcfb')
