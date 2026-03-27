"""
arc_mockup.py — visualise strobe arc layout options for the 240x320 CYD display.
Generates a side-by-side PNG of candidate configurations.
"""

import math
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import Arc, Wedge
from matplotlib.collections import PatchCollection

LCD_W, LCD_H = 240, 320
CX = LCD_W // 2          # always 120
R_INNER, R_OUTER = 60, 100
N_SEG = 18
FILL_RATIO = 0.45
K_DEMO_PHASE = 0.3       # arbitrary phase offset so segments aren't centred

configs = [
    dict(label="current\n120° arc  CY=120",  cy=120, arc_deg=120),
    dict(label="option A\n150° arc  CY=120",  cy=120, arc_deg=150),
    dict(label="option B\n160° arc  CY=120",  cy=120, arc_deg=160),
    dict(label="option C\n170° arc  CY=120",  cy=120, arc_deg=170),
    dict(label="option D\n150° arc  CY=100",  cy=100, arc_deg=150),
    dict(label="option E\n160° arc  CY=100",  cy=100, arc_deg=160),
]

fig, axes = plt.subplots(2, 3, figsize=(12, 9))
fig.patch.set_facecolor('#111')

def draw_config(ax, cfg):
    cy   = cfg['cy']
    span = cfg['arc_deg']
    half = span / 2.0
    # arc goes from (-90 - half/2) to (-90 + half/2) degrees in screen coords
    # centre of arc is at -90° (straight up in atan2 with y-down)
    arc_min_deg = -90 - half/2   # e.g. -150 for 120°
    arc_max_deg = -90 + half/2   # e.g.  -30 for 120°

    ax.set_facecolor('black')
    ax.set_xlim(0, LCD_W)
    ax.set_ylim(LCD_H, 0)        # y-down to match screen coords
    ax.set_aspect('equal')
    ax.set_xticks([]); ax.set_yticks([])

    # draw display border
    for spine in ax.spines.values():
        spine.set_edgecolor('#444')

    seg_span_rad = 2 * math.pi / N_SEG
    lit_span_rad = seg_span_rad * FILL_RATIO

    # pixel-level render of the annulus arc
    # build a sample image at half resolution for speed
    scale = 2
    img = np.zeros((LCD_H // scale, LCD_W // scale, 3), dtype=np.float32)
    arc_min_rad = math.radians(arc_min_deg)
    arc_max_rad = math.radians(arc_max_deg)

    for py in range(0, LCD_H, scale):
        dy = py - cy
        for px in range(0, LCD_W, scale):
            dx = px - CX
            d2 = dx*dx + dy*dy
            if d2 < R_INNER*R_INNER or d2 > R_OUTER*R_OUTER:
                continue
            angle = math.atan2(dy, dx)
            if angle < arc_min_rad or angle > arc_max_rad:
                continue
            rel = math.fmod(angle - K_DEMO_PHASE, seg_span_rad)
            if rel < 0:
                rel += seg_span_rad
            if rel < lit_span_rad:
                iy, ix = py // scale, px // scale
                img[iy, ix] = (1.0, 1.0, 1.0)   # white segment

    ax.imshow(img, extent=[0, LCD_W, LCD_H, 0], origin='upper',
              interpolation='nearest', zorder=2)

    # note label placeholder
    note_y = cy - R_INNER * math.sin(math.radians(-arc_max_deg)) + 18
    note_y = max(note_y, cy - R_INNER + 20)
    ax.text(CX, note_y, "A4", color='white', fontsize=10,
            ha='center', va='center', zorder=3)

    # bar placeholder
    bar_y = 274
    ax.fill_between([20, 220], bar_y-4, bar_y+4, color='#333', zorder=3)
    ax.fill_between([20, 120], bar_y-4, bar_y+4, color='#00cc44', zorder=4)
    ax.axvline(120, ymin=(LCD_H-bar_y-8)/LCD_H, ymax=(LCD_H-bar_y+8)/LCD_H,
               color='white', lw=1, zorder=5)

    ax.set_title(cfg['label'], color='#ccc', fontsize=9, pad=4)

for ax, cfg in zip(axes.flat, configs):
    draw_config(ax, cfg)

plt.tight_layout(pad=0.5)
out = 'tools/arc_mockup.png'
plt.savefig(out, dpi=120, facecolor='#111')
print(f"saved {out}")
