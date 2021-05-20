# Evergreen Output Sequencer

This program runs another program and performs postprocessing on the second
program's output:

- Output and error streams are timestamped
- The streams are merged into one (output), with the source labeled

egos turns off buffering on all streams via setvbuf(3), peforming
the equivalent of

    stdbuf -i0 -o0 -e0
