# Magisk module installer script (sourced by Magisk's installer).
EGL=system/vendor/lib64/egl
REAL=$MODPATH/$EGL/libEGL_emulation_real.so

if [ ! -f "$REAL" ]; then
  abort "! libEGL_emulation_real.so missing from the module zip"
fi
# The bundled driver must be WSA's own, not a copy of this module's wrapper.
if grep -q egl_surfaceless "$REAL"; then
  abort "! libEGL_emulation_real.so is the wrapper, not WSA's driver"
fi

set_perm_recursive $MODPATH 0 0 0755 0644
# Same label as the files they replace, so app processes may load them.
set_perm $MODPATH/$EGL/libEGL_emulation.so 0 0 0644 u:object_r:same_process_hal_file:s0
set_perm $REAL 0 0 0644 u:object_r:same_process_hal_file:s0
ui_print "- Restart WSA to apply"
