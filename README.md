I recommend using the new C version. This should work on ANY Xorg desktop with zero configuration.

To build #
gcc -o cursor_scaler cursor_scaler.c -lX11 -lXcursor -lXrender -lXfixes -lm

The Python script Depends on ```xcur2png``` and is far more resource intenstive.

Be sure to change directory to your cursor in the python script.
```cursor_path = "/usr/share/icons/Simp1e-Dark/cursors/left_ptr"```
