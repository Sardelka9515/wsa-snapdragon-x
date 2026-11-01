# Magisk module installer script (sourced by Magisk's installer).
EGL=$MODPATH/system/vendor/lib64/egl

[ -f "$EGL/libEGL_emulation_real.so" ] || abort "! libEGL_emulation_real.so missing from the module zip"
for f in libEGL_emulation_real.so libGLESv2_emulation_real.so; do
  [ -f "$EGL/$f" ] || continue
  # The bundled drivers must be WSA's own, not copies of this module's wrappers.
  if grep -qE "egl_surfaceless|gles-wrapper" "$EGL/$f"; then
    abort "! $f is a wrapper, not WSA's driver"
  fi
done

set_perm_recursive $MODPATH 0 0 0755 0644
# Same label as the files they replace, so app processes may load them.
for f in "$EGL"/*.so; do
  set_perm "$f" 0 0 0644 u:object_r:same_process_hal_file:s0
done
ui_print "- Restart WSA to apply"
