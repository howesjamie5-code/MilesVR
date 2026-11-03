# Controlled flips

CONTROLLED FLIPS is a separate setting in the launcher and Settings >
SPIDY VR > EXPERIMENTAL. It defaults off and saves in spidy_vr.ini and
shared settings profiles.

While airborne, press A to enter the mode. The left stick pitches/rolls
the view around the tracked head. Release the stick to hold the exact
angle; press A again to exit and ease upright by the shortest path.
Left-stick travel is suppressed only while the mode owns it. The right
stick keeps normal turning and zip/aim/release/heal shortcuts. FLIP SPEED
sets the rotation rate.

A must be a fresh airborne press. Holding it through a ground jump,
menu, or settings change does not enable the mode. If the stick is
already tilted on entry, let it rest once first. Tracking loss freezes
the angle and requires neutral after recovery. Landing, wall/vent
attachment, menus, recentering and disabling the setting exit the mode.
Ground and wall A jumps remain available.

CONTROLLED FLIPS takes precedence over basic FLIPS when both are on.
REALISTIC FLIPS takes precedence over either and keeps left-stick manual
steering. Basic FLIPS instead uses the right stick in the air, holds its
angle on release and resets with A; a deliberate upward stick command
retains native zip/aim priority, including release from aim mode.

Regressions cover toggling, neutral/recovery, held input across menus,
mode priority, input ownership, jumps, landing and shortcut interaction.
